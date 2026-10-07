/* <orly/server/admission_wait.h>

   A bounded, first-come-first-served wait at memory admission (#765).

   Memory admission (#607) refuses a write the moment its blocks would take the update pools
   past the line that keeps the merges' reserve free. Under sustained overload that is right,
   but a burst that the root's memory merge clears a few tens of milliseconds later was refused
   too, and the client had to retry. With this, a write that finds no room waits, up to a bound,
   for the merges and Tetris promotions to free some, and is refused only if the bound passes.

   How it waits:
     - In arrival order. While anyone waits, a new write queues behind them instead of trying
       first, so a large batch can't be starved by a stream of small ones.
     - Holding nothing. A waiter has taken no pool blocks and no admission (it was refused), and
       it holds no lock: the callers release their read views and take their backlog room before
       they get here (#721, #722). Its fiber is parked, so its runner keeps running other work.
     - Woken by the release that makes room, not by polling. The writer at the head of the queue
       arms a watch on the pool that refused it (TPool::ArmRoomWake) at the level that would
       admit it. The first block freed, admission released or copy claim released that brings the
       pool to that level calls the room callback, which wakes this object's thread; that thread
       admits writers from the head of the queue for as long as they fit, and wakes each one it
       admits. The same thread refuses a writer whose bound has passed.
     - Never into the reserve. The thread admits a writer with the same TryAcquire a writer that
       didn't wait uses, so the merges' reserve and the admission hysteresis are unchanged.

   Close() refuses every waiter at once and makes later writes try once without waiting, so a
   graceful stop (#744) never waits on a writer parked here.

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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

#include <base/class_traits.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/update.h>

namespace Orly {

  namespace Server {

    class TAdmissionWait {
      NO_COPY(TAdmissionWait);
      public:

      /* What Admit did. */
      struct TResult {

        /* True if the write now holds its admission. */
        bool Admitted = false;

        /* True if it queued, whether or not it was then admitted. */
        bool Waited = false;

        /* How long it waited. */
        std::chrono::microseconds WaitTime {0};

      };  // TResult

      /* Writers wait at most bound. Starts the thread that admits them. Only one object at a time
         receives the pools' room callback: the newest. */
      explicit TAdmissionWait(std::chrono::milliseconds bound);

      /* Closes. */
      ~TAdmissionWait();

      /* Admits a write of num_entries entries into admission (TUpdate::TWriteAdmission::
         TryAcquire), waiting up to the bound for room if it doesn't fit at once, or if others are
         already waiting. Waits only on a fiber, and not once closed. On a refusal, admission's
         GetRefusal() describes the last attempt. */
      TResult Admit(Indy::TUpdate::TWriteAdmission &admission, size_t num_entries);

      /* Refuses every waiter now, stops the thread, and makes Admit try once without waiting
         from then on. Idempotent. */
      void Close();

      std::chrono::milliseconds GetBound() const {
        return Bound;
      }

      /* Writers waiting now. */
      size_t GetWaiting() const {
        return Waiting.load();
      }

      /* Writers that have waited, ever, whatever the outcome. */
      size_t GetWaited() const {
        return Waited.load();
      }

      /* Writers refused because the bound passed. */
      size_t GetTimedOut() const {
        return TimedOut.load();
      }

      /* Writers refused because Close() was called while they waited. */
      size_t GetClosedOut() const {
        return ClosedOut.load();
      }

      /* The longest wait so far, in microseconds. */
      size_t GetLongestWaitUs() const {
        return LongestWaitUs.load();
      }

      private:

      enum TOutcome : int {
        Pending,
        Granted,
        Expired,
        Shut
      };

      /* A writer in the queue. It lives on the writer's fiber stack; once its outcome is set and
         its Sem pushed, the thread doesn't touch it again. */
      struct TWaiter {

        Indy::TUpdate::TWriteAdmission *Admission;

        size_t NumEntries;

        std::chrono::steady_clock::time_point Deadline;

        Indy::Fiber::TSingleSem Sem;

        /* Set before Sem is pushed (release); read once the writer wakes (acquire), which also
           publishes what the thread's TryAcquire wrote into Admission. */
        std::atomic<int> Outcome {Pending};

      };  // TWaiter

      /* The thread's loop. */
      void Run();

      /* Admits waiters from the head of the queue while they fit, refuses those whose bound has
         passed, and arms the pools' watch for the head. Returns false if the queue is empty, else
         sets next_deadline to the head's deadline. Called with Mutex held. */
      bool ServeLocked(std::chrono::steady_clock::time_point &next_deadline);

      /* Tries to admit waiter; if it doesn't fit, arms the watch on the pool that refused it, at
         the level that would admit it, and tries once more. Called with Mutex held. */
      static bool TryAdmitLocked(TWaiter &waiter);

      /* Sets waiter's outcome and wakes it. Called with Mutex held; waiter is gone after this. */
      static void Finish(TWaiter &waiter, TOutcome outcome);

      /* The pools' room callback (TPool::SetRoomCallback): wakes the registered object's thread. */
      static void OnRoom();

      /* Tells the thread to look at the queue. */
      static void KickLocked(TAdmissionWait *self);

      const std::chrono::milliseconds Bound;

      /* Guards Queue and Closed. Taken before a pool's lock (TryAcquire), and before
         RegistryMutex (a TryAcquire that releases half its admission can call OnRoom). */
      std::mutex Mutex;

      std::deque<TWaiter *> Queue;

      bool Closed = false;

      /* Guarded by RegistryMutex, the static leaf lock that OnRoom takes. */
      bool Kicked = false;

      bool Stopping = false;

      std::condition_variable Cv;

      std::thread Thread;

      std::atomic<size_t> Waiting {0UL};

      std::atomic<size_t> Waited {0UL};

      std::atomic<size_t> TimedOut {0UL};

      std::atomic<size_t> ClosedOut {0UL};

      std::atomic<size_t> LongestWaitUs {0UL};

      static std::mutex RegistryMutex;

      static TAdmissionWait *Registered;

    };  // TAdmissionWait

  }  // Server

}  // Orly
