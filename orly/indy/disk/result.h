/* <orly/indy/disk/result.h>

   The result of a disk controller action.

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

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <base/spin_lock.h>
#include <base/event_counter.h>

#include <orly/indy/fiber/fiber.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      /* An error thrown when an I/O request fails due to a runtime / logic error. */
      class TDiskError
          : public std::runtime_error {
        public:

        /* Do-little. */
        TDiskError(const char *err_str)
            : runtime_error(err_str) {}

      };  // TDiskError

      /* An error thrown when an I/O request fails due to disk failure. */
      class TDiskFailure
          : public TDiskError {
        public:

        /* Do-little. */
        TDiskFailure()
            : TDiskError("Disk Failure") {}

      };  // TDiskFailure

      /* An error thrown when an I/O request fails due to server shutdown. */
      class TDiskServiceShutdown
          : public std::runtime_error {
        public:

        /* Do-little. */
        TDiskServiceShutdown()
            : runtime_error("Disk Error: Service Shutdown") {}

      };  // TDiskServiceShutdown

      /* The status of a disk controller action. Should be used to continue or halt
         progress of threads blocked on I/O. */
      enum TDiskResult {

        /* The I/O was successful. */
        Success,

        /* An error Occured */
        Error,

        /* This I/O failed. This could mean it was mal-formed or the disk device is
           dead / gone. We should unwind as our action was not successful. */
        DiskFailure,

        /* The server is shutting down. The I/O was interrupted by the thread manager.
           We should unwind as our action may not have been successful. */
        ServerShutdown

      };  // TResult

      template <typename TVal>
      static void WaitForDiskSpin(TVal &trigger, TVal wait_for_this, std::condition_variable &cond, std::unique_lock<std::mutex> &lock, TDiskResult &result, const char *err_str) {
        while (trigger != wait_for_this) {
          switch (result) {
            case Success : {
              cond.wait(lock);
              break;
            }
            case Error : {
              throw TDiskError(err_str);
              break;
            }
            case DiskFailure : {
              throw TDiskFailure();
              break;
            }
            case ServerShutdown : {
              throw TDiskServiceShutdown();
              break;
            }
          }
        }
      }

      class TCompletionTrigger {
        NO_COPY(TCompletionTrigger);
        public:

        inline TCompletionTrigger();

        inline ~TCompletionTrigger();

        inline void WaitForMore(size_t num);

        inline void WaitForOneMore();

        inline void Callback(TDiskResult disk_result, const char *err_str);

        inline void Wait(bool come_back_right_away = false);

        private:

        std::atomic<size_t> WaitFor;

        std::atomic<size_t> NumFinished;

        Fiber::TFrame *FrameWaiting;
        Fiber::TRunner *RunnerToReactivateOn;

        Base::TSpinLock SpinLock;

        TDiskResult Result;

        const char *ErrStr;

      };  // TCompletionTrigger

      /***************
        *** inline ***
        *************/

      inline TCompletionTrigger::TCompletionTrigger()
          : WaitFor(0UL),
          NumFinished(0UL),
          FrameWaiting(nullptr),
          RunnerToReactivateOn(nullptr),
          Result(Success),
          ErrStr(nullptr) {}

      inline TCompletionTrigger::~TCompletionTrigger() {}

      inline void TCompletionTrigger::WaitForMore(size_t num) {
        WaitFor += num;
      }

      inline void TCompletionTrigger::WaitForOneMore() {
        ++WaitFor;
      }

      inline void TCompletionTrigger::Callback(TDiskResult disk_result, const char *err_str) {
        /* Contract (#452): the completion COUNT alone decides the wake -- the waiter runs only
           after every outstanding completion has arrived, even when one of them reported an
           error (Result is the sticky first non-Success, thrown by Wait() after the barrier).
           Everything -- the count, the sticky error, and the wake decision -- happens under the
           spinlock, and the wake itself is issued from stack copies after the unlock, so this
           object is never touched after a waiter could observe completion and destroy it (the
           trigger is typically stack-owned and freed the moment Wait() returns; same
           free-on-wake contract the fiber sync primitives pin in sem.test.cc).
           The old shape had three holes here: the count was bumped and Result read outside the
           lock (a fast-path waiter could see the full count, return, and free the trigger while
           the last completer was still walking into the spinlock); an error scheduled the waiter
           immediately without the count check (the woken waiter threw and freed the trigger
           under the feet of the still-inflight stragglers); and once an error was recorded, the
           remaining completions were ignored entirely -- so an error that landed before the
           waiter parked left the final completions unable to wake it: a hang. */
        Fiber::TFrame *frame = nullptr;
        Fiber::TRunner *runner = nullptr;
        /* lock scope -- the unlock at the end is this function's last touch of the object */ {
          Base::TSpinLock::TLock lock(SpinLock);
          const size_t num_finished = std::atomic_fetch_add(&NumFinished, 1UL) + 1UL;
          if (Result == Success && disk_result != Success) {
            /* Keep the first failure; later results (of either kind) don't overwrite it. */
            Result = disk_result;
            ErrStr = err_str;
          }
          if (num_finished == WaitFor.load() && FrameWaiting) {
            /* Final completion and a frame is parked: claim it for the post-unlock wake. */
            assert(RunnerToReactivateOn);
            frame = FrameWaiting;
            runner = RunnerToReactivateOn;
            FrameWaiting = nullptr;
            RunnerToReactivateOn = nullptr;
          }
        }
        if (frame) {
          runner->ScheduleFrame(frame);
        }
      }

      inline void TCompletionTrigger::Wait(bool come_back_right_away) {
        assert(Fiber::TFrame::LocalFrame);
        assert(Fiber::TRunner::LocalRunner);
        bool should_wait = false;
        /* set frame waiting */ {
          Base::TSpinLock::TLock lock(SpinLock);
          if (NumFinished.load() != WaitFor.load()) {
            /* we are going to wait... */
            should_wait = true;
            FrameWaiting = Fiber::TFrame::LocalFrame;
            RunnerToReactivateOn = Fiber::TRunner::LocalRunner;
          }
        }
        if (should_wait) {
          Fiber::Wait(come_back_right_away);
        }
        /* fall through means we are ready... take the lock so the read of Result below
           synchronizes with the completer's locked write of it (#452). */
        assert(NumFinished.load() <= WaitFor.load());
        Base::TSpinLock::TLock lock(SpinLock);
        switch (Result) {
          case Success : {
            break;
          }
          case Error : {
            throw TDiskError(ErrStr);
            break;
          }
          case DiskFailure : {
            throw TDiskFailure();
            break;
          }
          case ServerShutdown : {
            throw TDiskServiceShutdown();
            break;
          }
        }
      }

    }  // Disk

  }  // Indy

}  // Orly
