/* <jhm/jhm.cc>

   JHM build system.

   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <poll.h>

#include <base/backtrace.h>
#include <base/cmd.h>
#include <base/dir_walker.h>
#include <base/json.h>
#include <base/path.h>
#include <base/split.h>
#include <base/thrower.h>
#include <jhm/env.h>
#include <jhm/status_line.h>
#include <jhm/test.h>
#include <jhm/work_finder.h>
#include <base/util/io.h>
#include <base/util/path.h>

using namespace Base;
using namespace Jhm;
using namespace std;
using namespace std::placeholders;
using namespace Util;

/* Walks the build output tree collecting per-compile JSON Compilation
   Database snippets (written by TCompileCFamily::GetCmd) into one
   `<src_root>/compile_commands.json` file, the format clangd /
   clang-tidy / IDE tooling expect.

   Per-snippet rather than in-memory accumulation: an incremental build
   that only recompiles a handful of files preserves the entries for
   everything that didn't change. The snippet files live next to their
   .o output and get re-emitted whenever the file is recompiled. */
namespace {

class TCompdbCollector final : public TDirWalker {
  public:
  explicit TCompdbCollector(TJson::TArray *entries) : Entries(entries) {}
  bool OnFile(const TEntry &entry) override {
    const std::string name(entry.Name);
    static const std::string kSuffix = ".compdb.json";
    if (name.size() <= kSuffix.size() ||
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
      return true;
    }
    try {
      Entries->push_back(TJson::Read(entry.AccessPath));
    } catch (const std::exception &ex) {
      // A malformed snippet shouldn't kill the build; just skip it.
      std::cerr << "warning: skipping " << entry.AccessPath << ": " << ex.what() << '\n';
    }
    return true;
  }
  private:
  TJson::TArray *Entries;
};

void WriteCompileCommandsJson(const TEnv &env) {
  const std::string out_root = AsStr(*env.GetOut());
  const std::string src_root = AsStr(*env.GetSrc());
  TJson::TArray entries;
  TCompdbCollector collector(&entries);
  collector.Walk(out_root.c_str());
  if (entries.empty()) {
    return;  // nothing to write (e.g. test-only invocation, or first-ever bootstrap)
  }
  const std::string compdb_path = src_root + "/compile_commands.json";
  std::ofstream out(compdb_path);
  if (!out.is_open()) {
    std::cerr << "warning: could not open " << compdb_path << " for write\n";
    return;
  }
  out << TJson(std::move(entries));
}

/* Echo whatever output the pump has already delivered for a TIMED-OUT test,
   giving up after a quiet period instead of waiting for EOF: a SIGKILLed
   test can leave grandchildren holding the pipe's write side (its forked
   servers, say), and then EOF never comes -- the reporter must not inherit
   the wedge the timeout just cut short (#537). */
void EchoOutputBounded(TFd &&fd) {
  uint8_t buf[4096];
  for (;;) {
    pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 2000) <= 0 || !(p.revents & POLLIN)) {
      break;
    }
    ssize_t got = read(fd, buf, sizeof(buf));
    if (got <= 0) {
      break;
    }
    WriteExactly(STDOUT_FILENO, buf, static_cast<size_t>(got));
  }
}

}  // namespace

/* Converts relative file to absolute path if needed, then has the environment find/make the actual file object. */
TFile *FindFile(const string &cwd, TEnv &env, TWorkFinder &work_finder, const string &name) {
  if (name.size() < 1) {
    THROW_ERROR(runtime_error) << "Invalid target name " << quoted(name);
  }

  // If filename starts with '/' then it's an absolute path rooted at the base of the tree
  // Otherwise it's relative to the position where jhm was invoked.
  TFile *file = nullptr;
  if (name[0] == '/') {
    // Starts with a '/', so relative to src / an absolute pathname
    file = env.GetFile(TRelPath(TPath(name)));
  } else {
    // Doesn't start with a '/' so relative to execution location name.
    TPath abs_path(cwd + '/' + name);
    const TTree *tree;
    if(env.GetSrc()->Contains(abs_path)) {
      tree = env.GetSrc();
    } else if (env.GetOut()->Contains(abs_path)) {
      tree = env.GetOut();
    } else {
      THROW_ERROR(runtime_error) << "Target " << quoted(name)
                                 << " not relative to 'jhm' execution in either `src` or `out` directory";
    }
    file = env.GetFile(tree->GetRelPath(move(abs_path)));
  }

  if (!file) {
    THROW_ERROR(runtime_error) << "Error finding target" << quoted(name);
  }

  // If the file isn't buildable as is, try making it an executable (Add an empty extension to the end)
  if (!work_finder.IsBuildable(file)) {
    file = env.GetFile(TRelPath(AddExtension(TPath(file->GetRelPath().Path), {""})));
  }

  return file;
}

class TJhm : public TCmd {
  public:
  TJhm(int argc, char *argv[]) : WorkerCount(thread::hardware_concurrency()) {
    Parse(argc, argv, TMeta());
  }

  int Run() {
    auto cwd = GetCwd();

    // Build up the environment. Find the root, grab the project, user, and system configuration.
    // The project name is the folder immediately inside the .jhm root that cwd lives in.
    // Falls back to "src" when cwd is the root itself, preserving the legacy layout.
    auto root = TTree::Find(cwd, ".jhm");
    string proj_name = "src";
    auto cwd_parts = Base::SplitNamespace(cwd);
    if (cwd_parts.size() > root.Root.size()) {
      bool prefix_match = true;
      for (size_t i = 0; i < root.Root.size(); ++i) {
        if (cwd_parts[i] != root.Root[i]) {
          prefix_match = false;
          break;
        }
      }
      if (prefix_match && cwd_parts[root.Root.size()].compare(0, 4, "out_") != 0
          && cwd_parts[root.Root.size()] != "out") {
        proj_name = cwd_parts[root.Root.size()];
      }
    }
    TEnv env(root, proj_name, Config, ConfigMixin);

    // chdir to the src folder so we can always use relative paths. for commands
    /* abs_root */ {

      auto abs_root = AsStr(*env.GetSrc());
      if (!ExistsPath(abs_root.c_str())) {
        THROW_ERROR(runtime_error) << "Source directory " << quoted(abs_root) << " does not exist";
      }
      IfLt0(chdir(abs_root.c_str()));
    }

    // Get the files for the targets
    TWorkFinder work_finder(WorkerCount,
                            PrintCmd,
                            //NOTE: Env is guaranteed to always have a timestamp, so derefrencing it here is safe.
                            *env.GetConfig().GetTimestamp(),
                            env);

    // Break the cyclic dependency by registering these back.
    env.SetFuncs(bind(&TWorkFinder::IsBuildable, &work_finder, _1), bind(&TWorkFinder::IsFileDone, &work_finder, _1));

    /* Resolve every target before failing, so one bad target name doesn't
       hide the rest. */
    TSet<TFile *> target_files;
    vector<string> target_errors;
    auto add_target = [&](const string &name) {
      try {
        InsertOrFail(target_files, FindFile(cwd, env, work_finder, name));
      } catch (const exception &ex) {
        target_errors.emplace_back(ex.what());
      }
    };

    // Either build the explicitly specified targets, or the default targets
    if (!Targets.empty()) {
      for(const auto &target: Targets) {
        add_target(target);
      }
    } else {
      // Add the default targets
      for(const auto &target: env.GetConfig().Read<vector<string>>({"targets"})) {
        add_target('/' + target);
      }

      // Add the tests if we're supposed to by default
      bool build_tests = false;
      env.GetConfig().TryRead({"test","build_with_default_targets"}, build_tests);
      if (build_tests || !PrintTests.empty()) {
        auto tests = FindTests(env);

        // If the file is not buildable, skip the test. This prevents us from trying to test things that are
        // untestable.
        // NOTE: If this excludes something that should be tested, we should be able to catch that in test reports.
        /* filter */ {
          TSet<TFile *> filtered_tests;
          for (TFile *test : tests) {
            if (work_finder.IsBuildable(test)) {
              InsertOrFail(filtered_tests, test);
            }
          }
          tests = move(filtered_tests);
        }
        if (!PrintTests.empty()) {
          ofstream out(PrintTests);
          if (!out.is_open()) {
            THROW_ERROR(runtime_error) << "Unable to open file " << quoted(PrintTests) << "to write tests out to.";
          }
          TJson::TArray tmp;
          tmp.reserve(tests.size());
          std::for_each(tests.begin(), tests.end(), [&tmp] (const TFile *f) {
            tmp.push_back(AsStr(f->GetPath()));
          });
          out << TJson(move(tmp));
          out.close();
        }

        if (build_tests) {
          target_files.insert(tests.begin(), tests.end());
        }
      }
    }

    if (!target_errors.empty()) {
      THROW_ERROR(runtime_error) << "failed to resolve " << target_errors.size()
                                 << " target(s): " << Join(target_errors, "; ");
    }

    // Add all target files as needed
    for (TFile *f : target_files) {
      work_finder.AddNeededFile(f);
    }

    if (!work_finder.FinishAll()) {
      return 1;
    }

    // All compile jobs done -- collect the per-target compile-database
    // snippets they emitted into <src>/compile_commands.json.
    WriteCompileCommandsJson(env);

    // Run all the tests if requested
    if (!RunTests) {
      return 0;
    }

    // Run every test which should have been built, up to WorkerCount at a time (#349). Tests have
    // no dependencies on each other (unlike compile jobs), so this doesn't need the job runner's
    // full job-graph machinery -- just its same "keep up to WorkerCount subprocesses in flight,
    // refill as each finishes" shape, directly against TSubprocess.
    vector<TFile *> tests;
    for (TFile *f : target_files) {
      if (f->GetRelPath().Path.EndsWith({"test", ""})) {
        tests.push_back(f);
      }
    }

    /* A test that wedges must become a named failure in minutes, not a
       silently cancelled job half an hour later: without a per-test
       timeout, one hung binary eats the whole CI job budget and the only
       evidence is the orphan-process list (#537).  The flag wins if given;
       otherwise the config may override the default. */
    uint32_t test_timeout_secs = TestTimeout;
    if (test_timeout_secs == UnsetTestTimeout) {
      double configured = DefaultTestTimeoutSecs;
      env.GetConfig().TryRead({"test", "timeout_secs"}, configured);
      test_timeout_secs = static_cast<uint32_t>(configured);
    }

    TPump pump;
    unordered_map<int, TFile *> pid_to_test;
    unordered_map<TFile *, unique_ptr<TSubprocess>> running;
    unordered_map<TFile *, chrono::steady_clock::time_point> started_at;
    unordered_map<TFile *, uint32_t> timed_out;
    bool any_failed = false;
    size_t next_test = 0;

    auto start_test = [&](TFile *test) {
      vector<string> cmd{test->GetPath()};
      if (VerboseTests) {
        cmd.push_back("-v");
      }
      auto subprocess = TSubprocess::New(pump, cmd);
      pid_to_test[subprocess->GetChildId()] = test;
      started_at[test] = chrono::steady_clock::now();
      running.emplace(test, move(subprocess));
    };

    auto report_status = [&]{
      if (VerboseTests) {
        cout << "TESTS: " << running.size() << " running, " << (tests.size() - next_test) << " queued\n";
      } else {
        TStatusLine() << "TESTS: " << running.size() << " running, " << (tests.size() - next_test) << " queued";
      }
    };

    while (next_test < tests.size() && running.size() < WorkerCount) {
      start_test(tests[next_test++]);
    }
    report_status();

    while (!running.empty()) {
      auto pid = TSubprocess::TryWaitAll();
      if (!pid) {
        /* Nothing has exited; sweep for tests past their deadline.  A killed
           test still flows through the reap below on a later pass, so the
           bookkeeping (echo, EXITCODE, refill) stays in one place. */
        if (test_timeout_secs) {
          const auto now = chrono::steady_clock::now();
          for (const auto &item : running) {
            if (timed_out.count(item.first)) {
              continue;  // already killed; the reap is on its way
            }
            const auto ran = chrono::duration_cast<chrono::seconds>(now - started_at.at(item.first)).count();
            if (ran >= test_timeout_secs) {
              TStatusLine::Cleanup();
              cout << "TIMEOUT: " << item.first << " still running after " << ran
                   << "s; killing it" << endl;
              kill(item.second->GetChildId(), SIGKILL);
              timed_out[item.first] = static_cast<uint32_t>(ran);
            }
          }
        }
        this_thread::sleep_for(chrono::milliseconds(100));
        continue;
      }
      TFile *test = pid_to_test.at(pid);
      EraseOrFail(pid_to_test, pid);
      auto &subprocess = running.at(test);
      auto status = subprocess->Wait();

      if (VerboseTests || status) {
        TStatusLine::Cleanup(); // Make sure the TEST:/TESTS: line stays at the top.
        /* endl, not '\n': EchoOutput writes raw to fd 1, so cout must be
           flushed or (in a non-tty run, e.g. CI) the header and every queued
           TESTS: line land displaced from the test's own output, making the
           failure read as if the test printed nothing (#520). */
        cout << "TEST: " << test << endl;
        if (timed_out.count(test)) {
          EchoOutputBounded(subprocess->TakeStdOutFromChild());
          EchoOutputBounded(subprocess->TakeStdErrFromChild());
        } else {
          EchoOutput(subprocess->TakeStdOutFromChild());
          EchoOutput(subprocess->TakeStdErrFromChild());
        }
      }
      if (timed_out.count(test)) {
        cout << "TIMEOUT: " << test << " killed after " << timed_out.at(test)
             << "s (test.timeout_secs=" << test_timeout_secs << ")" << endl;
        any_failed = true;
      }
      if (status) {
        cout << "EXITCODE: " << status << endl;
        any_failed = true;
      }
      EraseOrFail(running, test);

      if (next_test < tests.size()) {
        start_test(tests[next_test++]);
      }
      report_status();
    }
    TStatusLine::Cleanup();
    return any_failed ? 2 : 0;
  }

  private:
  class TMeta : public TCmd::TMeta {
    public:
    TMeta() : TCmd::TMeta("JHM") {
      Param(&TJhm::Config, "config", Optional, "config\0c\0", "Run JHM in a particular configuration");
      Param(&TJhm::ConfigMixin,
            "config_mixin",
            Optional,
            "mixin\0m\0",
            "Add a configuration mixin to the base configuration");
      Param(&TJhm::PrintCmd, "print_cmd", Optional, "print-cmd\0p\0", "Print out all commands run");
      Param(
          &TJhm::PrintTests, "print_tests", Optional, "print-test\0", "Write a list of tests to the given filename.");
      Param(&TJhm::RunTests, "run_tests", Optional, "test\0", "Run the unit tests");
      Param(&TJhm::TestTimeout,
            "test_timeout",
            Optional,
            "test-timeout\0",
            "Per-test timeout in seconds when running tests (0 disables; default 600, or config test.timeout_secs)");
      Param(&TJhm::VerboseTests, "verbose_tests", Optional, "verbose-test\0", "Run tests in verbose mode");
      Param(&TJhm::WorkerCount,
            "worker_count",
            Optional,
            "worker-count\0",
            "Change the number of worker threads JHM uses to run jobs simultaneously");
      Param(&TJhm::Targets, "targets", Optional, "Targets to try to build");
    }

    virtual void WriteAfterDesc(ostream &strm) const {
      strm << "Copyright Atomic Kismet Company\n"
           << "Licensed under the Apache License, Version 2.0" << endl;
    }
  };

  /* Sentinel meaning "flag not given": fall back to config, then default. */
  static constexpr uint32_t UnsetTestTimeout = std::numeric_limits<uint32_t>::max();
  static constexpr double DefaultTestTimeoutSecs = 600;

  bool PrintCmd = false;
  string Config = "debug";
  string ConfigMixin;
  string PrintTests;
  bool RunTests = false;
  uint32_t TestTimeout = UnsetTestTimeout;
  bool VerboseTests = false;
  uint32_t WorkerCount = 0;
  vector<string> Targets;
};

int main(int argc, char *argv[]) {
  /* Ignore SIGPIPE so a subprocess closing its read end mid-write doesn't
     kill jhm. TPump's background thread writes through pipes that connect
     to child stdin / our buffered read of child stdout/stderr. When a
     child exits (normal flow at end of build, especially in the LTO link
     region with hundreds of input fds in flight), one of those writes can
     hit a closed read end. Default SIGPIPE terminates the process; with
     this handler write() just returns -1/EPIPE and the existing
     pump.cc:117-124 fallthrough flags the pipe dead and continues. */
  std::signal(SIGPIPE, SIG_IGN);

  /* Backtrace on terminate so an uncaught std::system_error in a worker
     thread (e.g. TPump's background thread) doesn't disappear into a bare
     `terminate called after throwing` line. */
  Base::SetBacktraceOnTerminate();

  try {
    return TJhm(argc, argv).Run();
  }
  catch (const std::exception &ex) {
    cout << "EXCEPTION: " << ex.what() << endl;
    return -1;
  }
}
