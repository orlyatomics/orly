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

#include <functional>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <csignal>
#include <unistd.h>
#include <execinfo.h>

#include <base/backtrace.h>
#include <base/debug_log.h>
#include <base/scheduler.h>
#include <base/web/daemonize.h>
#include <orly/server/server.h>

using namespace std;
using namespace chrono;
using namespace placeholders;
using namespace Base;
using namespace Orly::Server;

/* Command-line arguments. */
class TCmd final
    : public TServer::TCmd {
  public:

  /* Construct with defaults. */
  TCmd()
      : MinWorkerCount(50), MaxWorkerCount(50000), IdleWorkerTimeout(30000), Daemon(false) {}

  /* Construct from argc/argv. */
  TCmd(int argc, char *argv[])
      : TCmd() {
    Parse(argc, argv, TMeta());
    /* Resolve core-assignment defaults AFTER parsing so --fast_cores etc.
       override the hardware defaults instead of appending to them (issue #240). */
    ResolveCoreVecDefaults();
  }

  /* Parameters for the scheduler policy. */
  size_t MinWorkerCount, MaxWorkerCount, IdleWorkerTimeout;

  /* If true, then run as a daemon. */
  bool Daemon;

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

static void LaunchServer(TScheduler *scheduler, const ::TCmd &cmd, shared_ptr<TServer> &server) {
  DEBUG_LOG("main: constructing server");
  server = make_shared<TServer>(scheduler, cmd);
  DEBUG_LOG("main: server constructed");
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
static void ScrubSecretArgs(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    const char *name = argv[i];
    while (*name == '-') {
      ++name;
    }
    for (const char *secret: {"auth_token", "replication_token"}) {
      const size_t len = strlen(secret);
      if (name == argv[i] || strncmp(name, secret, len) != 0) {
        continue;
      }
      if (name[len] == '=') {
        char *value = argv[i] + (name - argv[i]) + len + 1;
        memset(value, '*', strlen(value));
      } else if (name[len] == '\0' && i + 1 < argc) {
        memset(argv[i + 1], '*', strlen(argv[i + 1]));
      }
    }
  }
}

static void CrashHandler(int sig) {
  void *frames[64];
  int frame_count = backtrace(frames, 64);
  const char *msg = "\n*** CRASH SIGNAL RECEIVED ***\n";
  write(STDERR_FILENO, msg, strlen(msg));
  backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
  signal(sig, SIG_DFL);
  raise(sig);
}

int main(int argc, char *argv[]) {
  // Make std::terminate calls produce more data / info for us.
  SetBacktraceOnTerminate();
  signal(SIGSEGV, CrashHandler);
  signal(SIGABRT, CrashHandler);
  signal(SIGBUS, CrashHandler);
  signal(SIGILL, CrashHandler);
  signal(SIGFPE, CrashHandler);
#if defined(__SANITIZE_THREAD__)
  RunTsanRacyControl();
#endif
  ::TCmd cmd(argc, argv);
  ScrubSecretArgs(argc, argv);
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
      bind(LaunchServer, _1, cref(cmd), ref(server)),
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
