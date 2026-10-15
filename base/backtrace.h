/* <base/backtrace.h>

   Print a backtrace

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

#pragma once

#include <functional>
#include <string>

namespace Base {

  void PrintBacktrace(int max_frame_count);

  /* Generate a backtrace one line at a time, calling the callback once for each backtrace frame. */
  void GenBacktrace(int max_frame_count, const std::function<void (const std::string &)> &cb);

  void SetBacktraceOnTerminate();

  /* Gives the calling thread an alternate signal stack, so the crash handler can run when the fault was
     a stack overflow or the thread is on a small fiber stack. Call once at the start of every thread
     that may fault; later calls on the same thread do nothing. */
  void InstallThreadCrashStack();

  /* Installs a handler for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT that writes "*** CRASH SIGNAL
     RECEIVED ***" to stderr, then a backtrace, then dies of the same signal. The message goes out
     before anything that could fault, and the handler runs on the alternate stack. Also installs the
     calling thread's crash stack. */
  void InstallCrashHandler();

} // Base
