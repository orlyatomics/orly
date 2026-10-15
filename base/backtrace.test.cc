/* <base/backtrace.test.cc>

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

#include <base/backtrace.h>

#include <string>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;

static void NullWrite() {
  *static_cast<volatile int *>(nullptr) = 1;
}

/* Overflows the stack with frames the compiler can't turn into a loop. */
static int Recurse(volatile int n) {
  volatile char pad[4096];
  pad[0] = static_cast<char>(n);
  return Recurse(n + 1) + pad[0];
}

/* Forks a child that runs `body` with the crash handler installed and its stderr piped to us. Returns what
   the child wrote and sets `sig` to the signal that killed it (0 if it didn't die of one). */
template <typename TBody>
static string RunChild(TBody body, int &sig) {
  int fds[2];
  EXPECT_EQ(pipe(fds), 0);
  const pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    /* If the handler ever hangs instead of dying, SIGALRM kills the child and the checks below fail. */
    alarm(10);
    InstallCrashHandler();
    body();
    _exit(0);
  }
  close(fds[1]);
  string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    out.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  return out;
}

FIXTURE(NullDereference) {
  int sig = 0;
  const string out = RunChild(NullWrite, sig);
  EXPECT_NE(out.find("*** CRASH SIGNAL RECEIVED ***"), string::npos);
  EXPECT_EQ(sig, SIGSEGV);
}

FIXTURE(FaultOnAnotherThread) {
  int sig = 0;
  const string out = RunChild([] {
    thread t([] {
      InstallThreadCrashStack();
      NullWrite();
    });
    t.join();
  }, sig);
  EXPECT_NE(out.find("*** CRASH SIGNAL RECEIVED ***"), string::npos);
  EXPECT_EQ(sig, SIGSEGV);
}

FIXTURE(StackOverflow) {
  int sig = 0;
  const string out = RunChild([] {
    thread t([] {
      InstallThreadCrashStack();
      Recurse(0);
    });
    t.join();
  }, sig);
  EXPECT_NE(out.find("*** CRASH SIGNAL RECEIVED ***"), string::npos);
  EXPECT_EQ(sig, SIGSEGV);
}

FIXTURE(KeepsAnExistingAltStack) {
  /* ASan gives each thread an alternate stack and unmaps whatever is installed at thread exit, so ours must not
     replace one that is already there. Under ASan the existing stack is ASan's own; otherwise the test installs one. */
  thread t([] {
    static char existing[64 * 1024];
    stack_t before;
    EXPECT_EQ(sigaltstack(nullptr, &before), 0);
    const bool installed_here = (before.ss_flags & SS_DISABLE) != 0;
    if (installed_here) {
      stack_t ss;
      ss.ss_sp = existing;
      ss.ss_size = sizeof(existing);
      ss.ss_flags = 0;
      EXPECT_EQ(sigaltstack(&ss, nullptr), 0);
    }
    stack_t expected;
    EXPECT_EQ(sigaltstack(nullptr, &expected), 0);
    InstallThreadCrashStack();
    stack_t cur;
    EXPECT_EQ(sigaltstack(nullptr, &cur), 0);
    EXPECT_EQ(cur.ss_sp, expected.ss_sp);
    if (installed_here) {
      stack_t off;
      off.ss_sp = nullptr;
      off.ss_size = 0;
      off.ss_flags = SS_DISABLE;
      EXPECT_EQ(sigaltstack(&off, nullptr), 0);
    }
  });
  t.join();
}
