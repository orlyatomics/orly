/* <util/error.test.cc>

   Unit test for <util/error.h>.

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

#include <base/util/error.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include <signal.h>
#include <unistd.h>

#include <base/zero.h>
#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Util;

FIXTURE(LibraryGenerated) {
  bool caught = false;
  try {
    thread().detach();
  } catch (const system_error &error) {
    caught = true;
  } catch (...) {}
  EXPECT_TRUE(caught);
}

FIXTURE(UtilsGenerated) {
  bool caught = false;
  try {
    IfLt0(read(-1, 0, 0));
  } catch (const system_error &error) {
    caught = true;
  } catch (...) {}
  EXPECT_TRUE(caught);
}

/* Re-enabled for #282. The old version used pause(), which loses the race
   where the signal lands between the worker's ready-notification and the
   pause() call -- the no-op handler consumed it and pause() then blocked
   forever ("sometimes never exits"). SIGUSR1 is now blocked before the
   worker exists, so an early signal stays pending, and sigsuspend()
   atomically unblocks + waits: the wakeup is deterministic no matter when
   the signal arrives. */
FIXTURE(Interruption) {
  struct sigaction action;
  Zero(action);
  action.sa_handler = [](int) {};
  sigaction(SIGUSR1, &action, 0);
  sigset_t block_set, old_set;
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGUSR1);
  IfNe0(pthread_sigmask(SIG_BLOCK, &block_set, &old_set));
  mutex mx;
  condition_variable cv;
  bool running = false, was_interrupted = false;
  thread t([&mx, &cv, &running, &was_interrupted] {
    /* lock */ {
      unique_lock<mutex> lock(mx);
      running = true;
      cv.notify_one();
    }
    try {
      sigset_t suspend_set;
      sigemptyset(&suspend_set);
      IfLt0(sigsuspend(&suspend_set));
    } catch (system_error &error) {
      was_interrupted = WasInterrupted(error);
    } catch (...) {}
  });
  /* lock */ {
    unique_lock<mutex> lock(mx);
    while (!running) {
      cv.wait(lock);
    }
  }
  IfNe0(pthread_kill(t.native_handle(), SIGUSR1));
  t.join();
  IfNe0(pthread_sigmask(SIG_SETMASK, &old_set, 0));
  EXPECT_TRUE(was_interrupted);
}