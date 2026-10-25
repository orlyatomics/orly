/* <orly/server/orlyi.cc>

   The 'main' of the Orly server.

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

#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <thread>
#include <unistd.h>

#include <base/backtrace.h>
#include <base/debug_log.h>
#include <base/scheduler.h>
#include <base/web/daemonize.h>
#include <orly/server/server.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly::Server;

/* Command-line arguments. */
class TCmd final
    : public TServer::TCmd {
  public:

  /* Construct with defaults. */
  TCmd() = default;

  /* Construct from argc/argv. */
  TCmd(int argc, char *argv[])
      : TCmd() {
    Parse(argc, argv, TMeta());
    /* Resolve core-assignment defaults AFTER parsing so --fast_cores etc.
       override the hardware defaults instead of appending to them (issue #240). */
    ResolveCoreVecDefaults();
  }

  /* Parameters for the scheduler policy. */
  size_t MinWorkerCount = 50;
  size_t MaxWorkerCount = 50000;
  size_t IdleWorkerTimeout = 30000;

  /* If true, then run as a daemon. */
  bool Daemon = false;

  private:

  /* Our meta-type. */
  class TMeta final
      : public TServer::TCmd::TMeta {
    public:

    /* Registers our fields. */
    TMeta()
        : TServer::TCmd::TMeta("The Orly server.") {
      Param(
          &TCmd::MinWorkerCount, "min_worker_count", Optional, "min_worker_count\0",
          "The minimum number of worker threads to maintain in the thread pool."
      );
      Param(
          &TCmd::MaxWorkerCount, "max_worker_count", Optional, "max_worker_count\0",
          "The maximum number of worker threads to maintain in the thread pool."
      );
      Param(
          &TCmd::IdleWorkerTimeout, "idle_worker_timeout", Optional, "idle_worker_timeout\0",
          "The maximum number of milliseconds a worker will wait for a job before self-destructing."
      );
      Param(
          &TCmd::Daemon, "daemon", Optional, "daemon\0d\0",
          "Run as a daemon.  The pid of the spawned daemon process is returned on stdout."
      );
    }

  };  // TCmd::TMeta

};  // TCmd

[[nodiscard]] static shared_ptr<TServer> LaunchServer(TScheduler *scheduler, const ::TCmd &cmd) {
  DEBUG_LOG("main: constructing server");
  auto server = make_shared<TServer>(scheduler, cmd);
  DEBUG_LOG("main: server constructed");
  return server;
}

#if defined(__SANITIZE_THREAD__)
/* The negative control for the TSan job's orlyi smoke (#713), as base/tsan_control.test is for
   its unit tests. With ORLY_TSAN_RACY_CONTROL set, two threads increment a plain int with no
   synchronisation, a data race TSan must report in this process's log. The job runs the smoke
   once that way and fails unless the race is counted, so a change that stops orlyi being
   instrumented, or its reports being collected, can't turn the gate green by accident. Only a
   TSan build has this, and only the variable runs it. */
[[gnu::noinline]] static void TsanControlBump(int &n) {
  for (int i = 0; i < 100000; ++i) {
    n = n + 1;
  }
}

static void RunTsanRacyControl() {
  if (!getenv("ORLY_TSAN_RACY_CONTROL")) {
    return;
  }
  int n = 0;
  std::thread a(TsanControlBump, std::ref(n));
  std::thread b(TsanControlBump, std::ref(n));
  a.join();
  b.join();
}
#endif

/* Overwrites the values of --auth_token and --replication_token in argv once they are parsed, so
   the secrets don't stay in /proc/<pid>/cmdline (#710). A listing taken before this still shows
   them, which is why the docs point at the file and environment forms. */
static void ScrubSecretArgs(std::span<char *> args) {
  constexpr std::array<std::string_view, 2> Secrets{"auth_token", "replication_token"};
  for (size_t i = 1; i < args.size(); ++i) {
    if (!args[i]) {
      continue;
    }
    std::string_view arg(args[i]);
    size_t dash_count = 0;
    while (dash_count < arg.size() && arg[dash_count] == '-') {
      ++dash_count;
    }
    if (dash_count == 0) {
      continue;
    }
    const std::string_view name = arg.substr(dash_count);
    for (const auto secret : Secrets) {
      if (!name.starts_with(secret)) {
        continue;
      }
      if (name.size() > secret.size() && name[secret.size()] == '=') {
        char *value = args[i] + dash_count + secret.size() + 1;
        memset(value, '*', strlen(value));
      } else if (name.size() == secret.size() && i + 1 < args.size() && args[i + 1]) {
        memset(args[i + 1], '*', strlen(args[i + 1]));
      }
    }
  }
}

/* Names the deprecated spellings of renamed flags that were given on the command line (#1034). They
   still work; the warning says what to use instead. */
static void WarnDeprecatedFlags(int argc, char *argv[]) {
  static const std::pair<const char *, const char *> renamed[] = {
      {"slave_port_number", "replica_port_number"},
      {"spn", "replica_port_number"},
      {"slave_bind_address", "replica_bind_address"},
      {"address_of_master", "address_of_primary"},
  };
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.size() < 2 || arg[0] != '-') {
      continue;
    }
    arg.remove_prefix(arg[1] == '-' ? 2 : 1);
    const auto eq = arg.find('=');
    std::string_view value;
    if (eq != std::string_view::npos) {
      value = arg.substr(eq + 1);
    } else if (i + 1 < argc) {
      value = argv[i + 1];
    }
    arg = arg.substr(0, eq);
    for (const auto &[old_name, new_name] : renamed) {
      if (arg == old_name) {
        std::cerr << "orlyi: --" << old_name << " is deprecated; use --" << new_name << std::endl;
      }
    }
    if (arg == "starting_state" && value == "SLAVE") {
      std::cerr << "orlyi: --starting_state=SLAVE is deprecated; use --starting_state=REPLICA" << std::endl;
    }
  }
}

int main(int argc, char *argv[]) {
  // Make std::terminate calls produce more data / info for us.
  SetBacktraceOnTerminate();
  InstallCrashHandler();
#if defined(__SANITIZE_THREAD__)
  RunTsanRacyControl();
#endif
  WarnDeprecatedFlags(argc, argv);
  ::TCmd cmd(argc, argv);
  ScrubSecretArgs(std::span(argv, static_cast<size_t>(argc)));
  TLog log(cmd);
  if (cmd.Daemon) {
    auto pid = Server::Daemonize();
    if (pid) {
      cout << pid << endl;
      return EXIT_SUCCESS;
    }
  }
  std::shared_ptr<TServer> server = nullptr;
  TScheduler::TPolicy(cmd.MinWorkerCount, cmd.MaxWorkerCount, milliseconds(cmd.IdleWorkerTimeout), false).RunUntilCtrlC(
      [&cmd, &server](TScheduler *scheduler) {
        server = LaunchServer(scheduler, cmd);
      },
      /* Orderly shutdown on ctrl-c (#440): stop serving, tear down the
         fiber-entangled managers on a fiber, stop and join the server's
         threads.  Destruction stays deferred (docs/teardown-design.md). */
      [&server] {
        if (server) {
          server->Shutdown();
        }
      });
  return EXIT_SUCCESS;
}
