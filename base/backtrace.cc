/* <base/backtrace.cc>

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

#include <iostream>

#include <array>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string_view>

#include <execinfo.h>
#include <unistd.h>

#include <base/as_str.h>

using namespace Base;
using namespace std;

void Base::PrintBacktrace(int max_frame_count) {
  GenBacktrace(max_frame_count, [] (const string &msg) {
    cout << msg << endl;
  });
}

void Base::GenBacktrace(int max_frame_count, const function<void (const string &)> &cb) {
  void *frames[max_frame_count+1];
  int frame_count = backtrace(frames, max_frame_count+1);
  char **symbols = backtrace_symbols(frames, frame_count);
  for (int frame_idx = 1; frame_idx < frame_count; ++frame_idx) {
    auto frame_print = AsStr('[', frame_idx, '/', frame_count-1, "][");
    if (symbols) {
      frame_print += symbols[frame_idx];
    } else {
      frame_print += AsStr(frames[frame_idx]);
    }
    cb(frame_print);
  }
}

void Base::SetBacktraceOnTerminate() {
  std::set_terminate([]() {
    static bool tried_throw = false;
    try {
      if (!tried_throw) {
        tried_throw = true;
        throw;
      }
      cerr << "TERMINATE (no exception)" << endl;
    } catch (const std::exception &ex) {
      cerr << "TERMINATE (standard exception): " << ex.what() << endl;
    } catch (...) {
      cerr << "TERMINATE (unknown exception): " << endl;
    }
    cerr << "BACKTRACE" << endl;
    PrintBacktrace(100);
    cerr << "TERMINATED" << endl;
  });
}

namespace {

  constexpr size_t CrashStackBytes = 64UL * 1024UL;

  void CrashHandler(int sig) {
    constexpr std::string_view msg = "\n*** CRASH SIGNAL RECEIVED ***\n";
    [[maybe_unused]] const auto written = write(STDERR_FILENO, msg.data(), msg.size());
    std::array<void *, 64> frames;
    const int frame_count = backtrace(frames.data(), static_cast<int>(frames.size()));
    backtrace_symbols_fd(frames.data(), frame_count, STDERR_FILENO);
    /* SA_RESETHAND already put the default action back, and SA_NODEFER left the signal unblocked, so
       this kills the process with the original signal (and a core, if enabled). */
    raise(sig);
  }

}  // namespace

void Base::InstallThreadCrashStack() {
  /* Keep an alternate stack the thread already has: ours from an earlier call, or ASan's. ASan installs one per thread
     and unmaps whatever is installed when the thread exits, so replacing it with a malloc'd block makes every thread
     exit die in ASan's munmap. */
  stack_t cur;
  if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) {
    return;
  }
  /* Leaked on purpose: the stack must outlive every signal the thread can take. */
  void *mem = malloc(CrashStackBytes);
  if (!mem) {
    return;
  }
  stack_t ss;
  ss.ss_sp = mem;
  ss.ss_size = CrashStackBytes;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, nullptr) != 0) {
    free(mem);
  }
}

void Base::InstallCrashHandler() {
  /* The first backtrace() call loads libgcc_s, which allocates; do it now, not inside the handler. */
  std::array<void *, 4> warm;
  backtrace(warm.data(), static_cast<int>(warm.size()));
  InstallThreadCrashStack();
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = CrashHandler;
  sa.sa_flags = SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
    sigaction(sig, &sa, nullptr);
  }
}
