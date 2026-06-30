/* <orly/indy/fiber/fiber.h>

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
#include <cassert>
#include <cerrno>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_set>


#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <ucontext.h>

#include <base/assert_true.h>
#include <base/class_traits.h>
#include <base/likely.h>
#include <base/spin_lock.h>
#include <base/thread_local_global_pool.h>
#include <base/zero.h>
#include <base/inv_con/unordered_list.h>
#include <orly/indy/fiber/extern_fiber.h>
#include <base/util/error.h>

/* ThreadSanitizer fiber annotations.

   This fiber runtime switches stacks by hand (setjmp/longjmp on the
   FAST_SWITCH path, swapcontext during fiber creation). TSan models a
   program as a fixed set of OS threads and cannot follow a manual stack
   switch; left unannotated it mis-attributes a fiber's memory accesses to
   whichever OS thread last touched the stack (producing spurious data-race
   reports) and treats a longjmp into a different stack as a wild jump,
   aborting the process with "longjmp causes uninitialized stack frame".

   The TSan fiber API (see <sanitizer/tsan_interface.h>) lets us tell the
   sanitizer that each fiber is its own logical thread of execution: we
   associate a fiber handle with each ucontext, and immediately before every
   stack switch we announce which fiber we are switching to. These calls are
   *only* compiled when building under -fsanitize=thread (gcc defines
   __SANITIZE_THREAD__), so non-TSan builds carry zero overhead and have no
   dependency on the sanitizer interface. */
#if defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
namespace Orly {
  namespace Indy {
    namespace Fiber {
      namespace TSanFiber {
        inline void *Current() { return __tsan_get_current_fiber(); }
        inline void *Create() { return __tsan_create_fiber(0); }
        inline void Destroy(void *fiber) { __tsan_destroy_fiber(fiber); }
        inline void SwitchTo(void *fiber) { __tsan_switch_to_fiber(fiber, 0); }
        /* Establish a happens-before edge across a custom fiber wait/wake
           handoff (e.g. TSafeSync). Release() publishes the calling fiber's
           prior writes into the sync object identified by addr; a later
           Acquire(addr) on the woken fiber observes them, so ThreadSanitizer
           tracks the ordering the primitive really provides. */
        inline void Acquire(void *addr) { __tsan_acquire(addr); }
        inline void Release(void *addr) { __tsan_release(addr); }
      }  // TSanFiber
    }  // Fiber
  }  // Indy
}  // Orly
#else
namespace Orly {
  namespace Indy {
    namespace Fiber {
      namespace TSanFiber {
        /* No-ops: compile away entirely when not building under TSan. */
        inline void *Current() { return nullptr; }
        inline void *Create() { return nullptr; }
        inline void Destroy(void *) {}
        inline void SwitchTo(void *) {}
        inline void Acquire(void *) {}
        inline void Release(void *) {}
      }  // TSanFiber
    }  // Fiber
  }  // Indy
}  // Orly
#endif

namespace Orly {

  namespace Indy {

    namespace Fiber {

      namespace FiberLocal {

        /* Base class for fiber local variables. */
        class TFiberLocal {
          NO_COPY(TFiberLocal);
          public:

          /* Return the root of the static list of TFiberLocals. */
          inline static const TFiberLocal *GetRoot() {
            return Root;
          }

          /* Return the next TFiberLocal in the static list. */
          inline const TFiberLocal *GetNext() const {
            return Next;
          }

          /* Initializes this fiber local at the given position and returns
             the number of bytes required to store it. */
          virtual size_t Init(void *const ptr) const = 0;

          protected:

          /* Store the size of the variable and calculate our offset
             within the stack. Prepend this to the static list. */
          TFiberLocal(size_t size)
              : Size(size),
                Offset(Root ? Root->Offset + Root->Size : 0),
                Next(Root) {
            Root = this;
          }

          /* The size I take up on the stack. */
          const size_t Size;

          /* The offset from the start of the stack at which I can be found. */
          const size_t Offset;

          private:

          /* See accessor. */
          static TFiberLocal *Root;

          /* See accessor. */
          const TFiberLocal *Next;

        };  // TFiberLocal

      }  // FiberLocal

      #define FAST_SWITCH

      #ifdef FAST_SWITCH

      #if !defined(__SANITIZE_THREAD__)

      /* ============================ FAST PATH ============================
         Production fibers: context switches are a bare setjmp/longjmp pair,
         which is dramatically cheaper than swapcontext (no syscall, no signal
         mask save/restore). ucontext is used only once, to bootstrap each
         fiber's jmp_buf. This is the only path compiled in normal builds. */

      struct fiber_t {
        ucontext_t fib;
        jmp_buf jmp;
        uint8_t *start_of_stack;
      };

      struct fiber_ctx_t {
        void (*fnc)(void *);
        void *ctx;
        jmp_buf *cur;
        ucontext_t *prv;
      };

      static void fiber_start_fnc(void *p) {
        fiber_ctx_t *ctx = reinterpret_cast<fiber_ctx_t *>(p);
        void (*ufnc)(void *) = ctx->fnc;
        void *uctx = ctx->ctx;
        if (_setjmp(*ctx->cur) == 0) {
          ucontext_t tmp;
          swapcontext(&tmp, ctx->prv);
        }
        ufnc(uctx);
      }

      inline void create_fiber(fiber_t &fib, void(*ufnc)(void *), void *uctx, size_t stack_size) {
        getcontext(&fib.fib);
        fib.start_of_stack = reinterpret_cast<uint8_t *>(malloc(stack_size));
        /* mlock keeps fiber stacks resident in RAM as a perf hint; on systems
           where RLIMIT_MEMLOCK is small (modern Linux default is 64KB) this
           fails with ENOMEM for MB-sized fiber stacks. The stack is still
           valid memory, so degrade gracefully rather than aborting. */
        if (mlock(fib.start_of_stack, stack_size) < 0 && errno != ENOMEM && errno != EPERM) {
          Util::ThrowSystemError(errno);
        }
        // init the fiber locals
        size_t bytes_of_loc = 0UL;
        fib.fib.uc_stack.ss_sp = fib.start_of_stack;
        for (const auto *loc = FiberLocal::TFiberLocal::GetRoot(); loc; loc = loc->GetNext()) {
          const size_t nbytes = loc->Init(fib.fib.uc_stack.ss_sp);
          reinterpret_cast<uint8_t *&>(fib.fib.uc_stack.ss_sp) += nbytes;
          bytes_of_loc += nbytes;
        }
        //printf("ss_sp=[%p], [%p]\n", fib.fib.uc_stack.ss_sp, reinterpret_cast<uint8_t *>(fib.fib.uc_stack.ss_sp) + stack_size);
        fib.fib.uc_stack.ss_size = stack_size - bytes_of_loc;
        fib.fib.uc_link = 0;
        ucontext_t tmp;
        fiber_ctx_t ctx = {ufnc, uctx, &fib.jmp, &tmp};
        makecontext(&fib.fib, reinterpret_cast<void(*)()>(fiber_start_fnc), 1, &ctx);
        swapcontext(&tmp, &fib.fib);
      }

      inline size_t get_stack_size(fiber_t &fib) {
        return fib.fib.uc_stack.ss_size;
      }

      inline void free_fiber(fiber_t &fib) {
        assert(fib.start_of_stack);
        free(fib.start_of_stack);
      }

      inline void switch_to_fiber(fiber_t &fib, fiber_t &prv) {
        if (_setjmp(prv.jmp) == 0) {
          _longjmp(fib.jmp, 1);
        }
      }

      #else  // __SANITIZE_THREAD__

      /* ========================= THREADSANITIZER =========================
         ThreadSanitizer models a fixed set of OS threads and cannot follow a
         manual stack switch. The fast setjmp/longjmp switch above is opaque to
         it: a longjmp onto another fiber's stack trips TSan's longjmp guard
         ("longjmp causes uninitialized stack frame") and aborts the process,
         and accesses on a fiber get mis-attributed to whichever OS thread last
         ran it, yielding spurious data races.

         Under TSan we therefore switch fibers with swapcontext, which TSan's
         own fiber API understands, and pair every switch with
         __tsan_switch_to_fiber so TSan tracks each fiber as its own logical
         thread of execution. Each fiber carries the same fields as the fast
         path PLUS a TSan fiber handle and the entry function/argument (the
         fast path consumes those during bootstrap; here the fiber is not
         entered until its first real switch, so we must keep them).

         This path is ONLY compiled under -fsanitize=thread; it never affects
         production builds, which use the setjmp/longjmp path above. */

      struct fiber_t {
        ucontext_t fib;
        jmp_buf jmp;  // unused under TSan; kept so layout/_mm_prefetch sites compile.
        uint8_t *start_of_stack;
        /* The TSan handle for this fiber's logical thread of execution. Minted
           in create_fiber, released in free_fiber. The scheduler's MainFiber
           instead borrows the OS thread's current fiber (see TRunner::Run) and
           must never be created or destroyed. */
        void *tsan_fiber = nullptr;
        /* Entry point, deferred until the first switch into this fiber. */
        void (*entry_fnc)(void *) = nullptr;
        void *entry_ctx = nullptr;
      };

      static void fiber_start_fnc(void *p) {
        fiber_t *fib = reinterpret_cast<fiber_t *>(p);
        fib->entry_fnc(fib->entry_ctx);
      }

      inline void create_fiber(fiber_t &fib, void(*ufnc)(void *), void *uctx, size_t stack_size) {
        getcontext(&fib.fib);
        fib.start_of_stack = reinterpret_cast<uint8_t *>(malloc(stack_size));
        if (mlock(fib.start_of_stack, stack_size) < 0 && errno != ENOMEM && errno != EPERM) {
          Util::ThrowSystemError(errno);
        }
        // init the fiber locals
        size_t bytes_of_loc = 0UL;
        fib.fib.uc_stack.ss_sp = fib.start_of_stack;
        for (const auto *loc = FiberLocal::TFiberLocal::GetRoot(); loc; loc = loc->GetNext()) {
          const size_t nbytes = loc->Init(fib.fib.uc_stack.ss_sp);
          reinterpret_cast<uint8_t *&>(fib.fib.uc_stack.ss_sp) += nbytes;
          bytes_of_loc += nbytes;
        }
        fib.fib.uc_stack.ss_size = stack_size - bytes_of_loc;
        fib.fib.uc_link = 0;
        fib.entry_fnc = ufnc;
        fib.entry_ctx = uctx;
        fib.tsan_fiber = TSanFiber::Create();
        /* The fiber is not entered now; fiber_start_fnc runs on first switch.
           We pass &fib so the entry point can be recovered from the fiber. */
        makecontext(&fib.fib, reinterpret_cast<void(*)()>(fiber_start_fnc), 1, &fib);
      }

      inline size_t get_stack_size(fiber_t &fib) {
        return fib.fib.uc_stack.ss_size;
      }

      inline void free_fiber(fiber_t &fib) {
        assert(fib.start_of_stack);
        if (fib.tsan_fiber) {
          TSanFiber::Destroy(fib.tsan_fiber);
          fib.tsan_fiber = nullptr;
        }
        free(fib.start_of_stack);
      }

      inline void switch_to_fiber(fiber_t &fib, fiber_t &prv) {
        /* Announce the switch to TSan, then swapcontext (which saves our
           resume point into prv.fib and resumes fib.fib). */
        TSanFiber::SwitchTo(fib.tsan_fiber);
        swapcontext(&prv.fib, &fib.fib);
      }

      #endif  // __SANITIZE_THREAD__

      #else

      typedef ucontext_t fiber_t;

      inline void create_fiber(fiber_t &fib, void(*ufnc)(void *), void *uctx, size_t stack_size) {
        static_assert(false, "swapcontext based fibers need fiber local support");
        getcontext(&fib);
        fib.uc_stack.ss_sp = malloc(stack_size);
        fib.uc_stack.ss_size = stack_size;
        fib.uc_link = 0;
        makecontext(&fib, reinterpret_cast<void (*)()>(ufnc), 1, uctx);
      }

      inline size_t get_stack_size(fiber_t &fib) {
        return fib.uc_stack.ss_size;
      }

      inline void free_fiber(fiber_t &fib) {
        assert(fib.uc_stack.ss_sp);
        free(fib.uc_stack.ss_sp);
      }

      inline void switch_to_fiber(fiber_t &fib, fiber_t &prev) {
        swapcontext(&prev, &fib);
      }
      #endif

      /* Forward Declaration */
      class TFrame;

      class alignas(64) TRunner {
        NO_COPY(TRunner);
        public:

        //typedef InvCon::UnorderedList::TCollection<TRunner, TFrame> TFrameQueue;

        class TRunnerCons {
          NO_COPY(TRunnerCons);
          public:

          TRunnerCons(size_t num_runners)
              : NumRunners(num_runners), NextId(0UL) {
            syslog(LOG_INFO, "TRunnerCons [%ld]", num_runners);
            RunnerArray = new std::atomic<TRunner *>[num_runners];
            syslog(LOG_INFO, "TRunnerCons [%ld] A", num_runners);
            for (size_t i = 0; i < num_runners; ++i) {
              RunnerArray[i].store(nullptr, std::memory_order_relaxed);
            }
            syslog(LOG_INFO, "TRunnerCons [%ld] B", num_runners);
          }

          ~TRunnerCons() {
            delete[] RunnerArray;
          }

          private:

          size_t GetNewId() {
            syslog(LOG_INFO, "TRunnerCons [%ld] GetNewId()", NextId);
            size_t ret = NextId;
            if (ret < NumRunners) {
              ++NextId;
              return ret;
            } else {
              throw std::logic_error("Adjust logic that computes num_runners for TRunnerCons so that we have enough communication channels.");
            }
          }

          const size_t NumRunners;

          size_t NextId;

          /* Per-runner publication slots. A TRunner publishes itself into its
             own slot in its ctor (store/release) and other runner threads read
             the slots in TRunner::Run (load/acquire), so the element type must
             be atomic. */
          std::atomic<TRunner *> *RunnerArray;

          friend class TRunner;

        };  // TRunnerCons

        TRunner(TRunnerCons &runner_cons) : TRunner(runner_cons.NumRunners, runner_cons.GetNewId(), runner_cons.RunnerArray) {
          /* Publish ourselves into the shared runner array. Release so that a
             peer runner that reads this slot (acquire) in TRunner::Run sees a
             fully constructed TRunner -- in particular our QueueArray. */
          runner_cons.RunnerArray[RunnerId].store(this, std::memory_order_release);
        }

        TRunner(size_t total_num_runners, size_t runner_id, std::atomic<TRunner *> *runner_array)
            : FreeFrame(nullptr),
              FreeFramePool(nullptr),
              //MyFrameQueue(this),
              ReadyToRunQueue(nullptr),
              NewReadyToRunQueue(nullptr),
              KeepRunning(true),
              InboundFrameQueue{nullptr},
              ForeignRunnerToMoveFrameTo(nullptr),
              FrameToMoveToForeignRunner(nullptr),
              TotalNumRunners(total_num_runners),
              RunnerId(runner_id),
              RunnerArray(runner_array) {
          assert(runner_id < total_num_runners);
          #ifdef FAST_SWITCH
          Base::Zero(MainFiber.fib);
          #else
          Base::Zero(MainFiber);
          #endif
          QueueArray = new TOutboundQueue[total_num_runners];
          for (size_t i = 0; i < total_num_runners; ++i) {
            QueueArray[i].Ptr.store(nullptr, std::memory_order_relaxed);
          }
        }

        ~TRunner() {
          RunnerArray[RunnerId].store(nullptr, std::memory_order_release);
          delete[] QueueArray;
        }

        void Run();

        void ShutDown() {
          KeepRunning.store(false);
        }

        static inline void Yield(fiber_t &fiber) {
          assert(LocalRunner);
          switch_to_fiber(LocalRunner->MainFiber, fiber);
        }

        static inline void Schedule(TFrame *frame) {
          assert(LocalRunner);
          LocalRunner->ScheduleFrame(frame);
        }

        inline void ScheduleFrame(TFrame *frame);

        fiber_t MainFiber;

        static __thread TRunner *LocalRunner;

        TFrame *FreeFrame;
        Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FreeFramePool;

        private:

        /* Schedule this frame using the CAS queue, so that we can exit the runner loop for the local queue. An example of somewhere to use this is
           when you are waiting on an event in a spin loop, and the event can only be triggered by also being scheduled on this runner using the CAS
           queue. */
        inline void ScheduleFrameSlow(TFrame *frame);

        inline void ScheduleFrameSlow(TRunner *other_runner, TFrame *frame);

        /* Push 'frame' onto a Treiber-stack queue head (see definition). */
        static inline void PushFrameOntoQueue(std::atomic<TFrame *> &head, TFrame *frame);

        //mutable TFrameQueue::TImpl MyFrameQueue;
        TFrame *ReadyToRunQueue;
        TFrame *NewReadyToRunQueue;

        std::atomic<bool> KeepRunning;

        /* Treiber-stack head for frames scheduled onto this runner from a
           thread that has no LocalRunner. Pushers CAS (release) onto it; the
           owning runner drains it with exchange (acquire) in TRunner::Run. */
        std::atomic<TFrame *> InboundFrameQueue;
        struct alignas(64) TOutboundQueue {
          /* Treiber-stack head: frames this runner wants to hand to runner i
             live in this->QueueArray[i]. Pusher CAS (release); the consuming
             runner drains with exchange (acquire). */
          std::atomic<TFrame *> Ptr;
        };
        TOutboundQueue *QueueArray;

        TRunner *ForeignRunnerToMoveFrameTo;
        TFrame *FrameToMoveToForeignRunner;

        size_t TotalNumRunners alignas(64);
        size_t RunnerId alignas(64);
        size_t blank_buf[7];

        std::atomic<TRunner *> *RunnerArray;

        /* Access to ComeBackSoon */
        friend class TFrame;
        friend class TFramePool;

      };  // TRunner

      class TRunnable {
        NO_COPY(TRunnable);
        public:

        typedef void (TRunnable::*TFunc)();

        protected:

        TRunnable() {}

      };  // TRunnable

      class alignas(64) TRunnerPool {
        NO_COPY(TRunnerPool);
        public:

        TRunnerPool(TRunner::TRunnerCons &runner_cons,
                    size_t num_worker)
            : WorkerCount(num_worker), AssignPos(0UL) {
          for (size_t i = 0; i < num_worker; ++i) {
            RunnerVec.emplace_back(new TRunner(runner_cons));
            ThreadVec.emplace_back(new std::thread(std::bind([](TRunner *runner) {
              runner->Run();
            }, RunnerVec.back().get())));
          }
        }

        ~TRunnerPool() {
          for (auto &runner : RunnerVec) {
            runner->ShutDown();
          }
          for (auto &t : ThreadVec) {
            t->join();
          }
        }

        inline size_t GetWorkerCount() const {
          return WorkerCount;
        }

        inline void Schedule(TFrame *frame, TRunnable *runnable, const TRunnable::TFunc &func);

        private:

        const size_t WorkerCount;

        /* TODO: use better data structure */
        std::vector<std::unique_ptr<TRunner>> RunnerVec;
        std::vector<std::unique_ptr<std::thread>> ThreadVec;

        /* TODO: we can do runner assignment more effectively than iterating through the vector */
        std::atomic<size_t> AssignPos;

      };  // TRunnerPool

      static void StartFrame(void *void_frame);

      class TFrame
          : public Base::TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TObjBase {
        NO_COPY(TFrame);
        public:

        TFrame(size_t stack_size, TRunner */*runner*/)
            :
              #ifndef NDEBUG
              DebugIsRunning(false),
              #endif
              RunnableFunc(nullptr),
              Runnable(nullptr),
              //QueueMembership(this),
              InboundQueueNextFrame(nullptr),
              ComeBackRightAway(false) {
          create_fiber(MyFiber, StartFrame, this, stack_size);
        }

        virtual ~TFrame() {
          /* this is where we make sure that our frame's stack unwound properly... */
          CheckFrameUnwound();
          assert(Runnable == nullptr);
          assert(RunnableFunc == nullptr);
          #ifndef NDEBUG
          /* Acquire-load pairs with the release-stores to DebugIsRunning in Run(),
             giving ThreadSanitizer a happens-before from a fiber's last write to
             this teardown so the pool free that follows is not reported as racing
             it (#200). Not a tripwire: a frame may be parked mid-runnable here
             (DebugIsRunning still true) when a test tears the pool down, so we only
             need the synchronization edge, not an assertion. */
          (void)DebugIsRunning.load(std::memory_order_acquire);
          #endif
          free_fiber(MyFiber);
        }

        inline void Latch(TRunner *runner, TRunnable *runnable, TRunnable::TFunc runnable_func) {
          //printf("TFrame [%p] Latch runnable [%p]\n", this, runnable);
          CheckFrameUnwound();
          assert(Runnable == nullptr);
          assert(RunnableFunc == nullptr);
          Runnable = runnable;
          RunnableFunc = runnable_func;
          runner->ScheduleFrame(this);
        }

        inline void Latch(TRunnable *runnable, TRunnable::TFunc runnable_func) {
          //printf("TFrame [%p] Latch runnable [%p]\n", this, runnable);
          CheckFrameUnwound();
          assert(Runnable == nullptr);
          assert(RunnableFunc == nullptr);
          Runnable = runnable;
          RunnableFunc = runnable_func;
          TRunner::Schedule(this);
        }

        inline void Yield() {
          ComeBackRightAway = false;
          TRunner::Schedule(this);
          TRunner::Yield(MyFiber);
        }

        inline void YieldSlow() {
          ComeBackRightAway = false;
          TRunner::LocalRunner->ScheduleFrameSlow(this);
          TRunner::Yield(MyFiber);
        }

        inline void Wait(bool come_back_right_away) {
          ComeBackRightAway = come_back_right_away;
          TRunner::Yield(MyFiber);
        }

        inline void SwitchTo(TRunner *runner) {
          ComeBackRightAway = false;
          runner->ScheduleFrame(this);
          TRunner::Yield(MyFiber);
        }

        void Run() {
          //printf("TFrame [%p] Run()\n", this);
          /* wait to get scheduled (after a Latch). */
          for (;;) {
            assert(Runnable);
            assert(RunnableFunc);
            //printf("TFrame [%p] calling func\n", this);

            #ifndef NDEBUG
            /* Release (paired with the acquire-load in ~TFrame, #200) so that even
               a frame parked mid-runnable at teardown has its last DebugIsRunning
               write synchronize with the pool free. */
            DebugIsRunning.store(true, std::memory_order_release);
            #endif
            TRunnable::TFunc runnable_func = RunnableFunc;
            TRunnable *const runnable = Runnable;
            Runnable = nullptr;
            RunnableFunc = nullptr;
            try {
              ((*runnable).*runnable_func)();
            } catch (const std::exception &ex) {
              syslog(LOG_EMERG, "FATAL ERROR: Fiber Runner caught exception. These must be handled within the fiber. %s", ex.what());
              //abort();
            }
            #ifndef NDEBUG
            /* Release so the acquire-load in ~TFrame (run on the teardown thread)
               synchronizes-with this write, giving ThreadSanitizer a happens-before
               from a finished fiber's last writes to the frame-pool free -- an
               ordering that physically holds (frames complete before teardown) but
               travels through the ucontext stack switch, which TSan can't model as
               a happens-before the way it does pthread_join (#200). */
            DebugIsRunning.store(false, std::memory_order_release);
            #endif
            //printf("TFrame [%p] FINISH calling func\n", this);

            /* wait to get scheduled again. */
            switch_to_fiber(TRunner::LocalRunner->MainFiber, MyFiber);
          }
        }

        #ifndef NDEBUG
        void AssertCanFree() {
          if (DebugIsRunning.load(std::memory_order_relaxed)) {
            throw std::logic_error("Cannot Free Frame that is still running");
          }
          assert(!DebugIsRunning.load(std::memory_order_relaxed));
        }
        #endif

        inline fiber_t &GetFiber() {
          return MyFiber;
        }

        static __thread TFrame *LocalFrame;

        static __thread Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool *LocalFramePool;

        private:

        inline void CheckFrameUnwound() {
          /* this is where we make sure that our frame's stack unwound properly... */
          #ifndef NDEBUG
          if (Runnable != nullptr || RunnableFunc != nullptr) {
            throw std::logic_error("Stack frame was not unwound properly!");
            /* this exception is thrown when a function scheduled against this stack switched back to the main scheduler, and was never queued again.
               This indicated a logical error that can leak resources. We should always switch back to each frame so that is can clean up. */
          }
          #endif
        }

        #ifndef NDEBUG
        std::atomic<bool> DebugIsRunning;
        #endif

        fiber_t MyFiber;

        TRunnable::TFunc RunnableFunc;

        TRunnable *Runnable;

        //TQueueMembership::TImpl QueueMembership;

        TFrame *InboundQueueNextFrame;

        bool ComeBackRightAway;

        /* MyFiber */
        friend class TFramePool;
        friend class TRunner;

        template <typename TVal, typename TArgs>
        friend class TFiberLocal;

      };  // TFrame

      static void StartFrame(void *void_frame) {
        TFrame *frame = reinterpret_cast<TFrame*>(void_frame);
        assert(frame);
        frame->Run();
        TRunner::Yield(frame->GetFiber());
      }

      template <typename TVal, typename TArgs>
      class TFiberLocal : public FiberLocal::TFiberLocal {
        NO_COPY(TFiberLocal);
        public:

        using TSuper = FiberLocal::TFiberLocal;

        /* Provided only to allow MakeFiberLocal() to exist.
           MakeFiberLocal takes advantage of RVO. But since we're non-copyable,
           we're required to provide at least a move ctor. */
        TFiberLocal(TFiberLocal &&that) : TFiberLocal(std::move(that.Args)) {}

        /* Do-little. */
        TFiberLocal(TArgs &&args)
            : TSuper(sizeof(TVal)), Args(std::move(args)) {}

        /* Allocates an instance of TVal(args...) at 'ptr'. */
        template <std::size_t... Is>
        static void Allocate(void *const ptr,
                             const TArgs &args,
                             std::index_sequence<Is...>) {
          new (ptr) TVal(std::get<Is>(args)...);
        }

        /* Initialize the stack at 'ptr' and return the size. */
        virtual size_t Init(void *const ptr) const {
          Allocate(ptr,
                   Args,
                   std::make_index_sequence<std::tuple_size<TArgs>::value>());
          return TSuper::Size;
        }

        /* Read the fiber local variable out of the fiber local stack. */
        inline TVal &operator*() const {
          assert(TFrame::LocalFrame);
          return *reinterpret_cast<TVal *>(
                      TFrame::LocalFrame->MyFiber.start_of_stack + Offset);
        }

        /* Read the fiber local variable out of the fiber local stack. */
        inline TVal *operator->() const {
          assert(TFrame::LocalFrame);
          return reinterpret_cast<TVal *>(
                     TFrame::LocalFrame->MyFiber.start_of_stack + Offset);
        }

        private:

        /* Cache the provided args and copy them to TVal's ctor on Init(). */
        TArgs Args;

      };  // TFiberLocal

      /* Factory function for FiberLocals. We leverage type deduction so that
         we only have to explicitly provide the target class.

           static int v = 42;
           static auto MyLocal = MakeFiberLocal<TObj>(42, "hello", std::ref(v));

         will initialize TObj with (int, const char *, int &) */
      template <typename TVal, typename... TFwdArgs>
      auto MakeFiberLocal(TFwdArgs &&... fwd_args) {
        auto args = std::make_tuple(std::forward<TFwdArgs>(fwd_args)...);
        return TFiberLocal<TVal, decltype(args)>(std::move(args));
      }

      /* This should only be used within the same scheduler. It is not safe to use between schedulers. */
      class TSync {
        NO_COPY(TSync);
        public:

        TSync(size_t waiting_for = 0UL) : Frame(nullptr), WaitingFor(waiting_for), Finished(0UL) {
          #ifndef NDEBUG
          RunnerSafety = TRunner::LocalRunner;
          #endif
        }

        inline void Sync(bool come_back_right_away = true);

        inline void Complete();

        inline void WaitForMore(size_t num);

        private:

        TFrame *Frame;

        size_t WaitingFor;
        size_t Finished;

        #ifndef NDEBUG
        TRunner *RunnerSafety;
        #endif

      };  // TSync
      static_assert(ExternFiber::SyncImplSize == sizeof(TSync), "ExternFiber::SyncImplSize must be adjusted to equal sizeof(TSync)");

      /* This is a multi-scheduler safe version of TSync. There is more synchronization involved, so use this when you are actually parallelizing
         tasks as opposed to scheduling multiple fibers on the same scheduler. */
      class TSafeSync {
        NO_COPY(TSafeSync);
        public:

        TSafeSync(size_t waiting_for = 0UL)
            : WaitingFor(waiting_for),
              Finished(0UL),
              FrameWaiting(nullptr),
              RunnerToReactivateOn(nullptr) {}

        inline void Sync(bool come_back_right_away = true);

        inline void Complete();

        inline void WaitForMore(size_t num);

        private:

        std::atomic<size_t> WaitingFor;
        std::atomic<size_t> Finished;

        Fiber::TFrame *FrameWaiting;
        Fiber::TRunner *RunnerToReactivateOn;

        Base::TSpinLock SpinLock;

      };  // TSafeSync

      class TSingleSem {
        NO_COPY(TSingleSem);
        public:

        TSingleSem() : FlagOn(false), FrameWaiting(nullptr), RunnerToReactivateOn(nullptr) {}

        ~TSingleSem() {}

        inline void Push();

        inline void Pop();

        private:

        Base::TSpinLock SpinLock;
        bool FlagOn;

        Fiber::TFrame *FrameWaiting;
        Fiber::TRunner *RunnerToReactivateOn;

      };  // TSingleSem

      class TSem {
        NO_COPY(TSem);
        public:

        TSem() : Count(0UL), FrameWaiting(nullptr), RunnerToReactivateOn(nullptr) {}

        ~TSem() {}

        inline void Push();

        inline void Pop();

        private:

        Base::TSpinLock SpinLock;
        size_t Count;

        Fiber::TFrame *FrameWaiting;
        Fiber::TRunner *RunnerToReactivateOn;

      };  // TSingleSem


      /* Use this locking mechanism when you want to lock and run the critical section on your current scheduler (core). This is usefull if you intend
         to use thread local or fiber local storage. It is also beneficial if you want to stay with the same cpu cache. The total throughput using
         this lock is less than a Queued lock as the lock bounces between schedulers. */
      class alignas(64) TFiberLock {
        NO_COPY(TFiberLock);
        public:

        inline TFiberLock()
            : Taken(false), RootLock(nullptr) {}

        class alignas(64) TLock {
          NO_COPY(TLock);
          public:

          inline TLock(TFiberLock &lock)
              : Lock(lock) {
            #ifndef NDEBUG
            DebugRunner = TRunner::LocalRunner;
            #endif
            assert(TRunner::LocalRunner);
            assert(TFrame::LocalFrame); /* we have to be fiber scheduled in order to use this locking mechanism. */
            /* Grab the spin lock to see if we can take control, or if we have to queue. */ {
              Base::TSpinLock::TLock my_lock(Lock.SpinLock);
              if (!lock.Taken) {
                lock.Taken = true;
                return;
              } else {
                NextLock = lock.RootLock;
                Runner = TRunner::LocalRunner;
                Frame = TFrame::LocalFrame;
                lock.RootLock = this;
                /* wait to get re-scheduled... once we're re-scheduled we have the lock. */
              }
            }  // release spin lock
            TFrame::LocalFrame->Wait(true);
            assert(lock.Taken);
          }

          inline ~TLock() {
            /* Grab the spin lock to release control, and enqueue anyone who is waiting. */ {
              Base::TSpinLock::TLock lock(Lock.SpinLock);
              TLock *next_to_release;
              if (!(next_to_release = Lock.RootLock)) {
                Lock.Taken = false;
              } else {
                Lock.RootLock = next_to_release->NextLock;
                /* schedule next guy... */
                next_to_release->Runner->ScheduleFrame(next_to_release->Frame);
                //TRunner::LocalRunner->ScheduleFrame(next_to_release->Frame);
              }
            }
            assert(DebugRunner == TRunner::LocalRunner);
          }

          private:

          TFiberLock &Lock;

          TLock *NextLock;

          TRunner *Runner;
          TFrame *Frame;

          #ifndef NDEBUG
          TRunner *DebugRunner;
          #endif

        };  // TLock

        private:

        Base::TSpinLock SpinLock;

        bool Taken;

        TLock *RootLock;

      };  // TFiberLock

      /* Use this lock if you don't care about thread or fiber local storage. Your critical section will be run on the scheduler (core) that the lock
         is tied to. The benefit of this is that lots of frames can queue to use this lock, meaning the raw bandwidth of this lock is larger. The
         downside is that your critical section is run on a different scheduler (core) so you may have a cold cpu cache. This lock performs better
         when tied to the scheduler that uses it the most. (the fewest moves to a different scheduler). The major difference with this lock versus the
         fiber lock is that the critical sections move to a single lock scheduler, as opposed to the critical sections determining which scheduler
         should run them next. */
      class alignas(64) TLockedQueue {
        NO_COPY(TLockedQueue);
        public:

        inline TLockedQueue(TRunner *runner)
            : Runner(runner) {}

        class alignas(64) TLock {
          NO_COPY(TLock);
          public:

          inline TLock(TLockedQueue &lock)
              : Lock(lock) {
            assert(TRunner::LocalRunner);
            assert(TFrame::LocalFrame); /* we have to be fiber scheduled in order to use this locking mechanism. */
            Runner = TRunner::LocalRunner;
            if (TRunner::LocalRunner != lock.Runner) {
              TFrame::LocalFrame->SwitchTo(lock.Runner);
            }
          }

          inline ~TLock() {
            if (Runner != Lock.Runner) {
              TFrame::LocalFrame->SwitchTo(Runner);
            }
          }

          private:

          TLockedQueue &Lock;

          TRunner *Runner;

        };  // TLock

        private:

        TRunner *Runner;

      };  // TLockedQueue

      /***************
        *** Inline ***
        *************/

      inline void TRunnerPool::Schedule(TFrame *frame, TRunnable *runnable, const TRunnable::TFunc &func) {
        size_t prev_assignment_count = std::atomic_fetch_add(&AssignPos, 1UL);
        TRunner *const chosen_runner = RunnerVec[prev_assignment_count % WorkerCount].get();
        frame->Latch(chosen_runner, runnable, func);
      }

      static inline void Yield() {
        assert(TFrame::LocalFrame);
        TFrame::LocalFrame->Yield();
      }

      static inline void YieldSlow() {
        assert(TFrame::LocalFrame);
        TFrame::LocalFrame->YieldSlow();
      }

      static inline void Wait(bool come_back_right_away = false) {
        assert(TFrame::LocalFrame);
        TFrame::LocalFrame->Wait(come_back_right_away);
      }

      static inline void SwitchTo(TRunner *runner) {
        assert(runner);
        assert(TFrame::LocalFrame);
        TFrame::LocalFrame->SwitchTo(runner);
      }

      static inline void FreeMyFrame(Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *pool) {
        assert(TFrame::LocalFrame);
        assert(TRunner::LocalRunner);
        TRunner::LocalRunner->FreeFrame = TFrame::LocalFrame;
        TRunner::LocalRunner->FreeFramePool = pool;
      }

      class TSwitchToRunner {
        NO_COPY(TSwitchToRunner);
        public:

        TSwitchToRunner(TRunner *runner_to_switch_to)
            : ComeFromRunner(Base::AssertTrue(TRunner::LocalRunner)) {
          assert(runner_to_switch_to);
          SwitchTo(runner_to_switch_to);
        }

        ~TSwitchToRunner() {
          assert(ComeFromRunner);
          SwitchTo(ComeFromRunner);
        }

        private:

        TRunner *ComeFromRunner;

      };  // TSwitchToRunner

      /* Push 'frame' onto a Treiber-stack queue head. The next-link field is a
         plain TFrame member written here and read by the draining runner; the
         head's release CAS / acquire exchange publishes it across threads, so
         it needs no atomic of its own. We relaxed-load the current head to seed
         the link and let compare_exchange_weak refresh 'expected' on failure. */
      inline void TRunner::PushFrameOntoQueue(std::atomic<TFrame *> &head, TFrame *frame) {
        TFrame *expected = head.load(std::memory_order_relaxed);
        do {
          frame->InboundQueueNextFrame = expected;
        } while (!head.compare_exchange_weak(
            expected, frame, std::memory_order_release, std::memory_order_relaxed));
      }

      inline void TRunner::ScheduleFrameSlow(TFrame *frame) {
        assert(frame);
        if (LocalRunner) {
          PushFrameOntoQueue(LocalRunner->QueueArray[RunnerId].Ptr, frame);
        } else {
          PushFrameOntoQueue(InboundFrameQueue, frame);
        }
      }

      inline void TRunner::ScheduleFrameSlow(TRunner *other_runner, TFrame *frame) {
        assert(LocalRunner);
        assert(other_runner);
        assert(frame);
        PushFrameOntoQueue(QueueArray[other_runner->RunnerId].Ptr, frame);
      }

      inline void TRunner::ScheduleFrame(TFrame *frame) {
        assert(frame);
        if (this == LocalRunner) {
          //printf("ScheduleFrame local\n");
          if (!frame->ComeBackRightAway) {
            frame->InboundQueueNextFrame = NewReadyToRunQueue;
            NewReadyToRunQueue = frame;
          } else {
            frame->InboundQueueNextFrame = ReadyToRunQueue;
            ReadyToRunQueue = frame;
          }
        } else if (LocalRunner && TFrame::LocalFrame == frame) {
          /* if we're rescheduling the local frame, then delay the schedule operation till after the frame returns to the scheduler so there's no race. */
          //printf("ScheduleFrame foreign from inside a scheduler [%p]\n", frame);
          assert(LocalRunner->ForeignRunnerToMoveFrameTo == nullptr);
          assert(LocalRunner->FrameToMoveToForeignRunner == nullptr);
          LocalRunner->ForeignRunnerToMoveFrameTo = this;
          LocalRunner->FrameToMoveToForeignRunner = frame;
        } else {
          /* if we're scheduling a different frame, then we can CAS right onto the scheduler of interest. */
          //printf("ScheduleFrame foreign from outside a scheduler [%p]\n", frame);
          ScheduleFrameSlow(frame);
        }
      }

      inline void TSync::Sync(bool come_back_right_away) {
        assert(RunnerSafety == TRunner::LocalRunner);
        //printf("TSync::Sync()\n");
        if (Finished < WaitingFor) {
          Frame = TFrame::LocalFrame;
          Wait(come_back_right_away);
          //printf("TSync::Sync completed()\n");
        }
        /* fall through */
        assert(RunnerSafety == TRunner::LocalRunner);
      }

      inline void TSync::Complete() {
        assert(RunnerSafety == TRunner::LocalRunner);
        ++Finished;
        //printf("TSync::Complete [%ld] of [%ld]\n", Finished, WaitingFor);
        if (Finished >= WaitingFor && Frame) {
          assert(Finished == WaitingFor);
          assert(Frame);
          TRunner::Schedule(Frame);
        }
        assert(RunnerSafety == TRunner::LocalRunner);
      }

      inline void TSync::WaitForMore(size_t num) {
        assert(RunnerSafety == TRunner::LocalRunner);
        WaitingFor += num;
      }

      /********************************************************/
      inline void TSafeSync::Sync(bool come_back_right_away) {
        #ifndef NDEBUG
        TRunner *safety_runner = TRunner::LocalRunner;
        #endif
        assert(Fiber::TFrame::LocalFrame);
        assert(Fiber::TRunner::LocalRunner);
        bool should_wait = false;
        /* set frame waiting */ {
          Base::TSpinLock::TLock lock(SpinLock);
          if (Finished.load() != WaitingFor.load()) {
            /* we are going to wait... */
            should_wait = true;
            FrameWaiting = Fiber::TFrame::LocalFrame;
            RunnerToReactivateOn = Fiber::TRunner::LocalRunner;
          }
        }
        if (should_wait) {
          Fiber::Wait(come_back_right_away);
        }
        /* fall through means we are ready... */
        assert(Finished.load() <= WaitingFor.load());
        //Base::TSpinLock::TLock lock(SpinLock);
        /* fall through */
        /* Acquire the happens-before published by every Complete() (paired
           release below) so ThreadSanitizer sees the completers' writes as
           ordered-before this fiber's continuation across the wait/wake
           handoff -- which the fiber switch alone does not convey (#269). */
        TSanFiber::Acquire(this);
        assert(safety_runner == TRunner::LocalRunner);
      }

      inline void TSafeSync::Complete() {
        /* Release this completer's prior writes into the sync object; the
           waiter's Acquire() in Sync() (above) pairs with it. Released on
           every Complete() so a multi-completer sync (WaitForMore > 1)
           accumulates each contributor's happens-before (#269). */
        TSanFiber::Release(this);
        size_t prev = std::atomic_fetch_add(&Finished, 1UL);
        if ((prev + 1UL) == WaitingFor.load()) {
          Base::TSpinLock::TLock lock(SpinLock);
          if (FrameWaiting) {
            /* there's a frame waiting for us... let's activate him. */
            assert(RunnerToReactivateOn);
            Fiber::TFrame *frame = FrameWaiting;
            Fiber::TRunner *runner = RunnerToReactivateOn;
            FrameWaiting = nullptr;
            RunnerToReactivateOn = nullptr;
            runner->ScheduleFrame(frame);
          }
        }
      }

      inline void TSafeSync::WaitForMore(size_t num) {
        WaitingFor += num;
      }

      inline void TSingleSem::Push() {
        Base::TSpinLock::TLock lock(SpinLock);
        if (FrameWaiting) {
          assert(!FlagOn);
          assert(RunnerToReactivateOn);
          Fiber::TFrame *frame = FrameWaiting;
          Fiber::TRunner *runner = RunnerToReactivateOn;
          FrameWaiting = nullptr;
          RunnerToReactivateOn = nullptr;
          runner->ScheduleFrame(frame);
        } else {
          FlagOn = true;
        }
      }

      inline void TSingleSem::Pop() {
        assert(TRunner::LocalRunner);
        bool do_wait = false;
        assert(!FrameWaiting);
        assert(!RunnerToReactivateOn);
        /* spinlock scope */ {
          Base::TSpinLock::TLock lock(SpinLock);
          if (FlagOn) {
            FlagOn = false;
          } else {
            do_wait = true;
            FrameWaiting = TFrame::LocalFrame;
            RunnerToReactivateOn = TRunner::LocalRunner;
          }
        } // end spinlock scope
        if (do_wait) {
          Fiber::Wait();
        }
      }

      inline void TSem::Push() {
        Base::TSpinLock::TLock lock(SpinLock);
        if (FrameWaiting) {
          assert(Count == 0UL);
          assert(RunnerToReactivateOn);
          Fiber::TFrame *frame = FrameWaiting;
          Fiber::TRunner *runner = RunnerToReactivateOn;
          FrameWaiting = nullptr;
          RunnerToReactivateOn = nullptr;
          runner->ScheduleFrame(frame);
        } else {
          ++Count;
        }
      }

      inline void TSem::Pop() {
        assert(TRunner::LocalRunner);
        bool do_wait = false;
        assert(!FrameWaiting);
        assert(!RunnerToReactivateOn);
        /* spinlock scope */ {
          Base::TSpinLock::TLock lock(SpinLock);
          if (Count > 0) {
            --Count;
          } else {
            do_wait = true;
            FrameWaiting = TFrame::LocalFrame;
            RunnerToReactivateOn = TRunner::LocalRunner;
          }
        } // end spinlock scope
        if (do_wait) {
          Fiber::Wait();
        }
      }

      /********************************************************/

      /* We use this as a lambda to pass to a thread that is going run a fiber scheduler with just 1 task assigned. This is temporary till we convert
         some background threads to properly fiber schedule... */
      static inline void LaunchSlowFiberSched(TRunner *runner, Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager) {
        if (!Fiber::TFrame::LocalFramePool) {
          Fiber::TFrame::LocalFramePool = new Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(frame_pool_manager);
        }
        runner->Run();
        delete Fiber::TFrame::LocalFramePool;
        Fiber::TFrame::LocalFramePool = nullptr;
      };

    }  // Fiber

  }  // Indy

}  // Orly
