/* <orly/indy/fiber/jump_runner.h>

  Jump into and back out of fiber scheduling.

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

#include <cassert>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>

#include <base/thread_local_global_pool.h>
#include <orly/indy/fiber/fiber.h>

namespace Orly {

  namespace Indy {

    namespace Fiber {

      /* Construct one of these with a closure of the function you want to run.
         When you call the object (it defines operator()), the closure will run
         in fiber-land.  The call will not return until the closure completes. */
      class TJumpRunnable final
          : public Indy::Fiber::TRunnable {
        public:

        /* Conveniences. */
        using TFunc = std::function<void ()>;
        using TFramePoolMngr = Base::TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>;
        using TFramePool = Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>;

        /* Takes ownership of the closure. */
        TJumpRunnable(TFunc &&func)
            : Func(move(func)) {
          assert(Func);
        }

        ~TJumpRunnable() noexcept(false) {
          if (ExceptionPtr) {
            std::rethrow_exception(ExceptionPtr);
          }
        }

        /* Calls the closure in the given fiber runner.  If this thread has no local pool of fiber
           frames, we'll make one now, using the given pool manager. */
        void operator()(TFramePoolMngr *frame_pool_mngr, Indy::Fiber::TRunner *runner) {
          EnsureLocalFramePool(frame_pool_mngr);
          /* The fiber will set this flag when it's done. */
          Flag = false;
          /* Remember the pool the frame is drawn from so Main() can return it
             once the closure completes.  The frame is allocated on this (calling)
             thread but runs on the runner's thread, where TFrame::LocalFramePool
             names a different pool -- so we capture the origin pool here and the
             fiber frees back to it.  Published to Main() through the Latch below. */
          FramePool = Fiber::TFrame::LocalFramePool;
          /* Make a frame and latch it into the runner. */
          auto *frame = FramePool->Alloc();
          assert(frame);
          try {
            frame->Latch(runner, this, static_cast<Fiber::TRunnable::TFunc>(&TJumpRunnable::Main));
          } catch (...) {
            FramePool->Free(frame);
            throw;
          }
          /* Wait for the flag to be set. */
          std::unique_lock<std::mutex> lock(Mutex);
          while (!Flag) {
            FlagSet.wait(lock);
          }
        }

        /* If the calling thread doesn't already have a local pool of disk events, make one for it;
           otherwise, do nothing. */
        static void EnsureLocalDiskEventPool() {
          /* Alias make this a little easier to read. */
          using event_t = Disk::Util::TDiskController::TEvent;
          using pool_mngr_t = Base::TThreadLocalGlobalPoolManager<event_t>;
          using pool_t = pool_mngr_t::TThreadLocalPool;
          pool_t *&pool = event_t::LocalEventPool;
          if (!pool) {
            pool = new pool_t(event_t::DiskEventPoolManager.get());
          }
        }

        /* If the calling thread doesn't already have a local pool of fiber frames, make one for it;
           otherwise, do nothing. */
        static void EnsureLocalFramePool(TFramePoolMngr *frame_pool_mngr) {
          assert(frame_pool_mngr);
          /* Make sure we have a frame pool. */
          if (!Fiber::TFrame::LocalFramePool) {
            Fiber::TFrame::LocalFramePool = new TFramePool::TThreadLocalPool(frame_pool_mngr);
          }
        }

        private:

        /* Entry point of the fiber. */
        void Main() {
          /* Make this thread has a frame pool and a disk event pool. */
          EnsureLocalDiskEventPool();
          try {
            Func();
          } catch (...) {
            ExceptionPtr = std::current_exception();
          }
          /* Return our frame to the pool it came from.  FreeMyFrame() only
             records the frame with the local runner; the scheduler reclaims it
             when this fiber switches back to it after Main() returns.  We must do
             this -- and read FramePool -- BEFORE signalling the waiter, because
             once Flag is set the waiting thread may return from operator() and
             destroy *this (the closure is typically a temporary).  FreeMyFrame()
             touches only thread-locals, never *this, so it is safe to call after
             we have copied the pool pointer out. */
          auto *pool = FramePool;
          Fiber::FreeMyFrame(pool);
          /* Set the flag and notify with the mutex held.  Both halves matter.
             Setting the flag outside the mutex can lose the wakeup: the waiter
             checks the flag under the mutex, we set it and notify in the
             instant before it blocks on the condition variable, and it then
             sleeps forever (#386's hang).  And notifying after the flag is
             observable touches members of *this after the waiter may have
             returned from operator() and destroyed us (#386's abort).  With
             the mutex held for both, the waiter cannot observe the flag until
             we are completely done with *this. */
          std::lock_guard<std::mutex> lock(Mutex);
          Flag = true;
          FlagSet.notify_one();
        }

        /* The closure we run in fiber-land. */
        TFunc Func;

        /* The thread-local frame pool the frame was allocated from in operator().
           Captured on the calling thread and read by Main() on the runner thread
           (published via the Latch) so the fiber returns its frame to the right
           pool. */
        TFramePool::TThreadLocalPool *FramePool = nullptr;

        /* Covers 'Flag', below. */
        std::mutex Mutex;

        /* Rethrows exceptions on destruction if an exception terminated the fiber. */
        std::exception_ptr ExceptionPtr;

        /* Notifies when 'Flag', below, is set. */
        std::condition_variable FlagSet;

        /* Synchronizes the waiting thread with the fiber-running thread. */
        bool Flag;

      };  // TJumpRunnable

    }  // Fiber

  }  // Indy

}  // Orly
