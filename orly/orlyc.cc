/* <orly/orly.cc>

   The controller for the orly compiler. Runs all the phases, exits on first phase failure.

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

#include <chrono>
#include <memory>
#include <ostream>

#include <unistd.h>

#include <base/cmd.h>
#include <base/scheduler.h>
#include <orly/compiler.h>
#include <orly/error.h>
#include <orly/server/server.h>
#include <orly/server/session.h>
#include <orly/spa/honcho.h>
#include <orly/spa/service.h>
#include <orly/type.h>
#include <base/util/path.h>

using namespace std;
using namespace Orly;
using namespace Orly::Spa; // Sort of ug. Would be nice to remove. Not strictly necessary for running tests...

class TCompilerConfig : public Base::TCmd {
  public:
  TCompilerConfig(int argc, char **argv)
      : DebugOutput(false),
        InfoReport(false),
        MachineForm(false),
        OutputDir(Util::GetCwd()),
        SemanticOnly(false),
        SkipTests(false),
        TestEngine("indy"),
        VerboseTests(false) {

    Parse(argc, argv, TMeta());
  }

  private:

  class TMeta : public TCmd::TMeta {
    public:
    TMeta() : TCmd::TMeta("Orly Compiler v0.5") {
      Param(&TCompilerConfig::DebugOutput, "debug_output", Optional, "debug\0d\0", "Compile the orly package in debug mode.");
      Param(&TCompilerConfig::MachineForm, "machine_form", Optional, "machine-form\0m\0", "Print out machine readable progress.");
      Param(&TCompilerConfig::OutputDir, "output_directory", Optional, "output\0o\0", "The directory to write output to.");
      Param(&TCompilerConfig::SemanticOnly, "semantic_only", Optional, "semantic-only\0", "Don't actually produce output, just syntactically and semantically validate the program.");
      Param(&TCompilerConfig::SkipTests, "skip_tests", Optional, "skip-tests\0", "Don't run tests after compiling.");
      Param(&TCompilerConfig::TestEngine, "test_engine", Optional, "test-engine\0",
          "Which storage engine runs the package's test{} blocks: 'indy' (default, the real server engine) or 'spa' (the legacy flux_capacitor engine, kept as an escape hatch ahead of its removal; see #262).");
      Param(&TCompilerConfig::VerboseTests, "verbose_tests", Optional, "v\0",
          "Run tests in 'verbose' mode, printing out the result of every test, rather than just failing tests.");
      Param(&TCompilerConfig::Source, "source", Required, "The Orly source file to compile.");
    }

    private:
    virtual void WriteAfterDesc(std::ostream &strm) const {
      strm << "Build: Unknown" << endl // TODO: Version from SCM.
           << endl
           << "Copyright Atomic Kismet Company" << endl
           << "Licensed under the Apache License, Version 2.0" << endl;
    }

  };  // TMeta

  virtual TCmd::TMeta *NewMeta() const {
    return new TMeta();
  }

  public:
  //NOTE: These should really be private w/ accessors but I'm going for quick right now, not pretty.
  bool DebugOutput;
  bool InfoReport;
  bool MachineForm;
  std::string OutputDir;
  bool SemanticOnly;
  std::string Source;
  bool SkipTests;
  std::string TestEngine;
  bool VerboseTests;
};

/* Run the freshly-compiled package's test{} blocks on an in-process indy
   server (#262), instead of the legacy SPA flux_capacitor engine. Stands up a
   short-lived, memory-simulated TServer (no disk, ephemeral/no listener ports,
   tiny fixed pools so 158 lang_test invocations don't blow up RAM), installs
   the package, and runs RunTestSuite. Returns true iff every test passed. */
/* TServer::TCmd's default ctor is protected (it's meant to be subclassed by a
   binary's arg parser, as orlyi does); subclass it so we can default-construct
   and tweak fields directly. */
class TIndyTestServerCmd final : public Orly::Server::TServer::TCmd {
  public:
  TIndyTestServerCmd() : Orly::Server::TServer::TCmd() {}
};

static bool RunTestsOnIndy(const Package::TVersionedName &output, const TCompilerConfig &cmd) {
  /* The server's fiber machinery (transactions, the engine) must run inside a
     scheduler/fiber context -- the same context RunUntilCtrlC gives its main
     job (orlyi constructs its TServer there too). We do the work in that job
     and then _exit with the result.

     We deliberately do NOT destroy the server. ~TServer would have to run on
     this (non-fiber) thread to free the fiber frame it allocated here, yet it
     also takes a fiber lock (StopAllPlayers) that asserts a fiber context --
     a contradiction. orlyi sidesteps this the same way: it never destroys its
     server; RunUntilCtrlC blocks until a signal and the process just exits.
     This is a short-lived compiler invocation, so leaking and _exit()ing is
     the clean, deterministic path (it also skips the crash-prone teardown). */
  Base::TScheduler::TPolicy(2, 8, std::chrono::milliseconds(1000)).RunUntilCtrlC(
      [&](Base::TScheduler *scheduler) {
        int exit_code = EXIT_FAILURE;
        try {
          TIndyTestServerCmd server_cmd;
          server_cmd.MemorySim = true;
          server_cmd.Create = true;
          server_cmd.StartingState = "SOLO";
          server_cmd.InstanceName = "orlyc";
          server_cmd.PackageDirectory = cmd.OutputDir.size() > 0 ? cmd.OutputDir : Util::GetCwd();
          /* No client should ever connect; bind listeners to ephemeral ports so
             concurrent lang_test compilers never collide on a port. */
          server_cmd.PortNumber = 0;
          server_cmd.WsPortNumber = 0;
          server_cmd.MemcachePortNumber = 0;
          server_cmd.SlavePortNumber = 0;
          server_cmd.ReportingPortNumber = 0;
          /* Keep this compiler-embedded server tiny and host-RAM-independent:
             every pool below is otherwise scaled up by available memory. */
          server_cmd.MemorySimMB = 32;
          server_cmd.MemorySimSlowMB = 8;
          server_cmd.PageCacheSizeMB = 8;
          server_cmd.BlockCacheSizeMB = 8;
          server_cmd.DurableCacheSize = 256;
          server_cmd.MaxRepoCacheSize = 512;
          server_cmd.NumFiberFrames = 512;
          server_cmd.NumDiskEvents = 512;
          server_cmd.NumMemMergeThreads = 1;
          server_cmd.NumDiskMergeThreads = 1;
          server_cmd.NumWsThreads = 1;
          server_cmd.DurableMappingPoolSize = 256;
          server_cmd.DurableMappingEntryPoolSize = 2000;
          server_cmd.DurableLayerPoolSize = 512;
          server_cmd.DurableMemEntryPoolSize = 5000;
          server_cmd.RepoMappingPoolSize = 512;
          server_cmd.RepoMappingEntryPoolSize = 5000;
          server_cmd.RepoDataLayerPoolSize = 512;
          server_cmd.TransactionMutationPoolSize = 500;
          server_cmd.TransactionPoolSize = 200;
          server_cmd.UpdatePoolSize = 4000;
          server_cmd.UpdateEntryPoolSize = 8000;
          server_cmd.DiskBufferBlockPoolSize = 256;
          /* Populate the hardware-derived fast/slow/merge core assignment that
             Parse() would normally fill in; the server requires non-empty
             fast/slow core vectors. */
          server_cmd.ResolveCoreVecDefaults();

          /* Intentionally leaked (see above). */
          auto *server = new Orly::Server::TServer(scheduler, server_cmd);
          /* Installing the package, opening a session, and running the tests all
             create durable objects / transactions, which must execute inside a
             fiber (TCompletionTrigger::Wait asserts on LocalFrame). RunPackageTests
             does all of that on a fast runner and hands back the result. */
          bool ok = server->RunPackageTests(output.Name.Name, output.Version, cmd.VerboseTests);
          exit_code = ok ? EXIT_SUCCESS : EXIT_FAILURE;
        } catch (const std::exception &ex) {
          std::cerr << "error: running tests on indy: " << ex.what() << std::endl;
          exit_code = EXIT_FAILURE;
        } catch (...) {
          std::cerr << "error: running tests on indy: unknown error" << std::endl;
          exit_code = EXIT_FAILURE;
        }
        if (cmd.MachineForm) {
          std::cout << "MM_NOTICE: Tests done" << std::endl;
        }
        std::cout.flush();
        std::cerr.flush();
        _exit(exit_code);
      });

  /* Unreachable: the job _exit()s. Present so the function has a return path. */
  return false;
}

int CompileCode(const TCompilerConfig &cmd) {

  //TODO: This 'make absolute' is something we do fairly commonly. (At least here and <jhm/jhm>). Should canonicalize.
  string src = cmd.Source;
  if(src.front() != '/') {
    src = Util::GetCwd() + '/' + src;
  }

  //TODO: Practically speaking for each src file given, esp. if we have more than one
  // We need to find the root of it's repository (__orly__) file. And use that
  int result = EXIT_FAILURE;
  const bool indy_tests = (cmd.TestEngine == "indy");
  try {
    Package::TVersionedName output;
    /* A TypeCzar (carried by THoncho, which also owns the SPA flux stores) must
       be live while we compile, and -- for the SPA engine -- while it runs the
       tests. The indy engine stands up its own TServer, which brings its own
       TypeCzar; a process may hold only one, so for indy we let this honcho die
       before the server is built. */ {
      THoncho honcho;
      output = Compiler::Compile(
                   Base::TPath(src),
                   cmd.OutputDir,
                   cmd.DebugOutput,
                   cmd.MachineForm,
                   cmd.SemanticOnly);
      if (cmd.SkipTests || cmd.SemanticOnly) {
        if (cmd.MachineForm) {
          cout << "MM_NOTICE: Skipped tests" << endl;
        }
        return EXIT_SUCCESS;
      }
      if (!indy_tests) {
        if (cmd.MachineForm) {
          cout << "MM_NOTICE: Running tests" << endl;
        }
        TService service;

        /* TODO: This is a little ugly */
        service.SetPackageDir(cmd.OutputDir.size() > 0 ? cmd.OutputDir : "");
        service.GetPackageManager().Install({output});
        result = service.RunTestSuite(output.Name, cmd.VerboseTests) ? EXIT_SUCCESS : EXIT_FAILURE;
        service.GetPackageManager().Uninstall({output});

        if (cmd.MachineForm) {
          cout << "MM_NOTICE: Tests done" << endl;
        }
      }
    }
    if (indy_tests) {
      /* #262: run the test{} blocks on the real server engine. */
      if (cmd.MachineForm) {
        cout << "MM_NOTICE: Running tests" << endl;
      }
      result = RunTestsOnIndy(output, cmd) ? EXIT_SUCCESS : EXIT_FAILURE;
      if (cmd.MachineForm) {
        cout << "MM_NOTICE: Tests done" << endl;
      }
    }
  } catch (const Compiler::TCompileFailure &ex) {
    /* Do nothing, we already printed out the error message. */
    if (cmd.DebugOutput) {
      cerr << "compile failure: " << ex.what() << endl;
    }
    //TODO: The do nothing is not the prettiest.
  } catch (const TSourceError &src_error) {
    //TODO: It would be nice to put the orly code location at the end, and only in debug mode.
    cerr << src_error.GetPosRange() << ' ' << src_error.what() << endl;
  } catch (const exception &ex) {
    cerr << "error: " << ex.what() << endl;
  }

  return result;
}

/* TODO: Main should be injected */
int main(int argc, char **argv) {
  TCompilerConfig config(argc, argv);

  return CompileCode(config);
}
