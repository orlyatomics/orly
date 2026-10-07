/* <orly/indy/fiber/fiber.cc>

   Implements <orly/indy/fiber/fiber.h>.

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

#include <orly/indy/fiber/fiber.h>

#include <cxxabi.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

using namespace std::literals;
using namespace Orly::Indy::Fiber;

/* TRunner::LocalRunner, TFrame::LocalFrame and TFrame::LocalFramePool are
   inline static TFiberSafeLocal members now, and define themselves (#554). */
FiberLocal::TFiberLocal *FiberLocal::TFiberLocal::Root = nullptr;

void Orly::Indy::Fiber::swap_eh_state(eh_state_t &save_to, const eh_state_t &restore_from) {
  /* The Itanium ABI's __cxa_eh_globals is opaque in <cxxabi.h>; eh_state_t mirrors its
     (non-ARM-EABI) layout, which both libstdc++ and libc++abi use. */
  eh_state_t *const globals = reinterpret_cast<eh_state_t *>(abi::__cxa_get_globals());
  save_to.caught_exceptions = globals->caught_exceptions;
  save_to.uncaught_exceptions = globals->uncaught_exceptions;
  globals->caught_exceptions = restore_from.caught_exceptions;
  globals->uncaught_exceptions = restore_from.uncaught_exceptions;
}

/********************************************************/
/******************* EXTERN FIBER ***********************/
/********************************************************/

void Orly::Indy::ExternFiber::SchedTaskLocally(const std::function<void ()> &func) {
  class TRunFunc : public TRunnable {
    NO_COPY(TRunFunc);
    public:
    TRunFunc(const std::function<void ()> &func)
        : FramePool(Indy::Fiber::TFrame::LocalFramePool),
          Func(func) {
      assert(FramePool);
      auto *frame = FramePool->Alloc();
      assert(frame);
      try {
        frame->Latch(TRunner::LocalRunner, this, static_cast<Indy::Fiber::TRunnable::TFunc>(&TRunFunc::Run));
      } catch (...) {
        Indy::Fiber::TFrame::LocalFramePool->Free(frame);
        throw;
      }
    }
    ~TRunFunc() {
      FreeMyFrame(FramePool);
    }
    void Run() {
      Func();
      delete this;
    }
    private:
    Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;
    const std::function<void ()> Func;
  };
  new TRunFunc(func);
}

Orly::Indy::ExternFiber::TSync::TSync(size_t waiting_for) {
  new (GetImpl()) Fiber::TSync(waiting_for);
}

Orly::Indy::ExternFiber::TSync::~TSync() {
  GetImpl()->~TSync();
}

void Orly::Indy::ExternFiber::TSync::Sync(bool come_back_right_away) {
  GetImpl()->Sync(come_back_right_away);
}

void Orly::Indy::ExternFiber::TSync::Complete() {
  GetImpl()->Complete();
}

void Orly::Indy::ExternFiber::TSync::WaitForMore(size_t num) {
  GetImpl()->WaitForMore(num);
}

/********************************************************/
/***************** END EXTERN FIBER *********************/
/********************************************************/

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
void TRunner::Run() {
  LocalRunner = this;
  #if defined(__SANITIZE_THREAD__)
  /* The scheduler loop runs on this OS thread's native stack. Adopt the
     current TSan fiber as MainFiber's handle so that every switch back to the
     scheduler (switch_to_fiber(..., MainFiber)) announces a real destination
     fiber. We borrow it -- it must never be created or destroyed here. */
  MainFiber.tsan_fiber = TSanFiber::Current();
  #endif
  TFrame *rt_queue = nullptr; /* we use this to push come back right away jobs to the front... */
  TFrame *next_frame = nullptr; /* we use this to loop through a queue... */
  try {
    /* Idle laps (each one polls every queue we can be handed frames on)
       before we park. The spin keeps a busy server's handoffs free of
       system calls; past it, an idle runner blocks instead of polling (#764). */
    const size_t laps_before_park = 100UL;
    size_t laps_without_work = 0UL;
    /* EXPERIMENT (#772): after the spin, poll with 10 us sleeps (the old loop) until idle this
       long, then park. */
    static const int64_t sleep_phase_ns = [] {
      const char *env = getenv("ORLY_RUNNER_SLEEP_PHASE_US");
      return env ? atol(env) * 1000L : 0L;
    }();
    Diag = TDiag772::Enabled ? TDiag772::Register(DiagLabel) : nullptr;
    ParkSlots[RunnerId].Diag.store(Diag, std::memory_order_relaxed);
    int64_t idle_start = 0;
    for (; likely(KeepRunning.load());) {
      assert(!ReadyToRunQueue);
      /* check for inbound frames */ {
        if (InboundFrameQueue.load(std::memory_order_acquire)) {
          assert(rt_queue == nullptr);
          /* Drain the whole Treiber stack in one shot. Acquire pairs with the
             pushers' release CAS so the frames' InboundQueueNextFrame links we
             walk below are visible. */
          TFrame *cur_tail = InboundFrameQueue.exchange(nullptr, std::memory_order_acquire);
          for (TFrame *frame = cur_tail; frame; frame = next_frame) {
            assert(frame->InboundQueueNextFrame != frame);
            next_frame = frame->InboundQueueNextFrame;
            if (!frame->ComeBackRightAway) {
              frame->InboundQueueNextFrame = ReadyToRunQueue;
              ReadyToRunQueue = frame;
              //frame->QueueMembership.Insert(&MyFrameQueue, InvCon::Rev);
            } else {
              frame->InboundQueueNextFrame = rt_queue;
              rt_queue = frame;
            }
            //printf("TRunner [%p] push frame [%p]\n", this, frame);
          }
          /* here we have to choose between putting the most recent or least recent "high priority" (come_back_soon) fiber first. We currently
             implement the more recent one (as opposed to fair one) because it's most likely to still have data in the cache... */
          for (TFrame *frame = rt_queue; frame; frame = next_frame) {
            //frame->QueueMembership.Insert(&MyFrameQueue, InvCon::Rev);
            next_frame = frame->InboundQueueNextFrame;
            frame->InboundQueueNextFrame = ReadyToRunQueue;
            ReadyToRunQueue = frame;
          }
          rt_queue = nullptr;
        } else {
          /* Drain the handoff slots our peers push for us. The slots live in
             the TRunnerCons-owned matrix, deliberately NOT on the peer
             runners: a peer can be destroyed while we are mid-poll (a
             manager's member runner dying during teardown), so we must never
             dereference a peer TRunner here (#463). */
          for (size_t i = 0; i < TotalNumRunners; ++i) {
            std::atomic<TFrame *> &cur_inbound_queue = HandoffSlot(RunnerId, i).Ptr;
            if (cur_inbound_queue.load(std::memory_order_acquire)) {
              assert(rt_queue == nullptr);
              /* Drain the whole Treiber stack; acquire pairs with the
                 pushers' release CAS (see InboundFrameQueue above). */
              TFrame *cur_tail = cur_inbound_queue.exchange(nullptr, std::memory_order_acquire);
              for (TFrame *frame = cur_tail; frame; frame = next_frame) {
                assert(frame->InboundQueueNextFrame != frame);
                next_frame = frame->InboundQueueNextFrame;
                if (!frame->ComeBackRightAway) {
                  frame->InboundQueueNextFrame = ReadyToRunQueue;
                  ReadyToRunQueue = frame;
                  //frame->QueueMembership.Insert(&MyFrameQueue, InvCon::Rev);
                  //__builtin_prefetch(frame->MyFiber.jmp, 0, 2);
                } else {
                  frame->InboundQueueNextFrame = rt_queue;
                  rt_queue = frame;
                }
                //printf("TRunner [%p] push frame [%p]\n", this, frame);
              }
              /* here we have to choose between putting the most recent or least recent "high priority" (come_back_soon) fiber first. We currently
                 implement the more recent one (as opposed to fair one) because it's most likely to still have data in the cache... */
              for (TFrame *frame = rt_queue; frame; frame = next_frame) {
                next_frame = frame->InboundQueueNextFrame;
                frame->InboundQueueNextFrame = ReadyToRunQueue;
                ReadyToRunQueue = frame;
                //frame->QueueMembership.Insert(&MyFrameQueue, InvCon::Rev);
                #ifdef FAST_SWITCH
                __builtin_prefetch(frame->MyFiber.jmp, 0, 2);
                #endif
              }
              rt_queue = nullptr;
            }
          }
        }
      }
      if (ReadyToRunQueue) {
        if (Diag && laps_without_work) {
          const int64_t gap = TDiag772::Now() - idle_start;
          const size_t b = TDiag772::Bucket(gap);
          TDiag772::Add(Diag->GapN[b], 1);
          TDiag772::Add(Diag->GapNs[b], gap);
        }
        laps_without_work = 0UL;
      } else {
        if (++laps_without_work == 1UL && (Diag || sleep_phase_ns)) {
          idle_start = TDiag772::Now();
        }
        if (laps_without_work >= laps_before_park) {
          if (sleep_phase_ns && TDiag772::Now() - idle_start < sleep_phase_ns) {
            std::this_thread::sleep_for(10000ns);
            if (Diag) {
              TDiag772::Add(Diag->SleepLaps, 1);
            }
          } else {
            /* Stays past the threshold, so a wakeup that finds nothing (the
               timeout, a stale wake) parks again after one more lap. */
            Park();
          }
        }
      }
      for (;;) {
        for (TFrame *frame = ReadyToRunQueue; ReadyToRunQueue; frame = ReadyToRunQueue) {
          ReadyToRunQueue = frame->InboundQueueNextFrame;
          __builtin_prefetch(reinterpret_cast<uint8_t *>(ReadyToRunQueue) + offsetof(TFrame, MyFiber), 0, 3);
          fiber_t *sched_fib = &frame->GetFiber();
          TFrame::LocalFrame = frame;
          FreeFrame = nullptr;
          FreeFramePool = nullptr;
          //printf("[%p]\tSwitch to Frame\n", this);
          switch_to_fiber(*sched_fib, MainFiber);
          if (Diag) {
            TDiag772::Add(Diag->Frames, 1);
          }
          //printf("[%p]\tDone Frame\n", this);
          if (FreeFrame) {
            assert(FreeFrame == frame);
            assert(FreeFramePool);
            FreeFramePool->Free(FreeFrame);
            FreeFrame = nullptr;
            FreeFramePool = nullptr;
          }
          if (ForeignRunnerToMoveFrameTo) {
            assert(FrameToMoveToForeignRunner);
            ScheduleFrameSlow(ForeignRunnerToMoveFrameTo, FrameToMoveToForeignRunner);
            ForeignRunnerToMoveFrameTo = nullptr;
            FrameToMoveToForeignRunner = nullptr;
          }
          TFrame::LocalFrame = nullptr;
        }
        if (NewReadyToRunQueue) {
          assert(!ReadyToRunQueue);
          std::swap(ReadyToRunQueue, NewReadyToRunQueue);
          assert(ReadyToRunQueue);
          assert(!NewReadyToRunQueue);
        } else {
          break;
        }
      }
    }
  } catch (const std::exception &ex) {
    syslog(LOG_INFO, "TRunner [%p] caught exception [%s]", this, ex.what());
  } catch (...) {
    LocalRunner = nullptr;
    throw;
  }
  LocalRunner = nullptr;
}

#pragma GCC diagnostic pop

bool TRunner::HasInboundFrames() const {
  if (InboundFrameQueue.load(std::memory_order_seq_cst)) {
    return true;
  }
  for (size_t i = 0; i < TotalNumRunners; ++i) {
    if (HandoffSlot(RunnerId, i).Ptr.load(std::memory_order_seq_cst)) {
      return true;
    }
  }
  return false;
}

void TRunner::Park() {
  /* Every push onto our queues is followed by a Wake() of this slot, and
     ShutDown() wakes it too, so the timeout only bounds the cost of a
     wakeup we failed to foresee; it is not how work gets noticed. */
  static constexpr auto safety_net = 100ms;
  Base::TParker &parker = ParkSlots[RunnerId].Parker;
  parker.PrepareToPark();
  if (!KeepRunning.load(std::memory_order_seq_cst) || HasInboundFrames()) {
    parker.CancelPark();
    return;
  }
  if (!Diag) {
    parker.Park(safety_net);
    return;
  }
  const int64_t t0 = TDiag772::Now();
  parker.Park(safety_net);
  const int64_t t1 = TDiag772::Now();
  TDiag772::Add(Diag->Parks, 1);
  TDiag772::Add(Diag->ParkNs, t1 - t0);
  const int64_t stamp = ParkSlots[RunnerId].WakeStamp.exchange(0, std::memory_order_relaxed);
  if (stamp && stamp <= t1) {
    const size_t b = TDiag772::Bucket(t1 - stamp);
    TDiag772::Add(Diag->LatN[b], 1);
    TDiag772::Add(Diag->LatNs[b], t1 - stamp);
  } else {
    TDiag772::Add(Diag->Unstamped, 1);
  }
}

/* DIAGNOSTIC (#772) */
bool TRunner::TDiag772::Enabled = getenv("ORLY_DIAG772") != nullptr;
TRunner::TDiag772 TRunner::TDiag772::NonRunner;

namespace {
  constexpr size_t Diag772Max = 512;
  TRunner::TDiag772 Diag772Slots[Diag772Max];
  std::atomic<size_t> Diag772Count{0};

  void Diag772Dump(FILE *f) {
    using T = TRunner::TDiag772;
    auto row = [f](const T &d, const char *label, long tid) {
      auto ld = [](const std::atomic<uint64_t> &c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
      fprintf(f, "%-18s tid=%-7ld frames=%llu parks=%llu park_ms=%llu wakes_in=%llu unstamped=%llu sleeps=%llu wake_out=%llu wake_out_us=%llu",
              label ? label : "?", tid, ld(d.Frames), ld(d.Parks), ld(d.ParkNs) / 1000000ULL, ld(d.WakesIn), ld(d.Unstamped), ld(d.SleepLaps),
              ld(d.WakeOut), ld(d.WakeOutNs) / 1000ULL);
      fprintf(f, " gaps=");
      for (size_t b = 0; b < T::NB; ++b) {
        fprintf(f, "%s%llu", b ? "/" : "", ld(d.GapN[b]));
      }
      fprintf(f, " gap_ms=");
      for (size_t b = 0; b < T::NB; ++b) {
        fprintf(f, "%s%llu", b ? "/" : "", ld(d.GapNs[b]) / 1000000ULL);
      }
      fprintf(f, " lat=");
      for (size_t b = 0; b < T::NB; ++b) {
        fprintf(f, "%s%llu", b ? "/" : "", ld(d.LatN[b]));
      }
      fprintf(f, " lat_us=");
      for (size_t b = 0; b < T::NB; ++b) {
        fprintf(f, "%s%llu", b ? "/" : "", ld(d.LatNs[b]) / 1000ULL);
      }
      fprintf(f, "\n");
    };
    const size_t n = std::min(Diag772Count.load(), Diag772Max);
    for (size_t i = 0; i < n; ++i) {
      row(Diag772Slots[i], Diag772Slots[i].Label.load(), Diag772Slots[i].Tid.load());
    }
    row(T::NonRunner, "(non-runner)", 0);
  }
}

TRunner::TDiag772 *TRunner::TDiag772::Register(const char *label) {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      const char *path = getenv("ORLY_DIAG772");
      const std::string tmp = std::string(path) + ".tmp";
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (FILE *f = fopen(tmp.c_str(), "w")) {
          Diag772Dump(f);
          fclose(f);
          rename(tmp.c_str(), path);
        }
      }
    }).detach();
  });
  const size_t i = Diag772Count.fetch_add(1);
  if (i >= Diag772Max) {
    return nullptr;
  }
  Diag772Slots[i].Label.store(label);
  Diag772Slots[i].Tid.store(static_cast<long>(syscall(SYS_gettid)));
  return &Diag772Slots[i];
}