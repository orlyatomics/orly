/* <base/parker.h>

   Lets one consumer thread block until a producer hands it work, instead of
   polling its queues with short sleeps (#764).

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
#include <cstdint>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#include <base/class_traits.h>

namespace Base {

  /* One consumer parks here; any number of producers wake it.

     The consumer owns some lock-free queues (Treiber stacks, say) that
     producers push onto. When it finds them all empty it parks:

       parker.PrepareToPark();
       if (queues are non-empty || told to stop) {
         parker.CancelPark();
       } else {
         parker.Park(timeout);
       }

     A producer pushes, then calls Wake().

     No wakeup is lost, provided the producer's push and the consumer's
     re-check of the queue are both seq_cst (a seq_cst CAS or exchange on the
     queue head, and a seq_cst load in the re-check). PrepareToPark() and
     Wake()'s load are seq_cst too, so the four operations sit in one total
     order: either the re-check sees the push, or Wake() sees the parked
     state and wakes the consumer.

     Wake() costs one load when the consumer is not parked, which is the
     common case on a busy server; only a wake of a parked consumer makes a
     system call.

     On Linux the wait is a futex on the state word. Elsewhere it falls back
     to std::atomic::wait, which has no timeout. */
  class TParker {
    NO_COPY(TParker);
    public:

    TParker() : State(Running) {}

    /* Consumer: announce that we are about to park. Re-check the queues
       (with seq_cst loads) after this, and before Park(). */
    void PrepareToPark() {
      State.exchange(Parked, std::memory_order_seq_cst);
    }

    /* Consumer: the re-check found work; don't park after all. */
    void CancelPark() {
      State.store(Running, std::memory_order_relaxed);
    }

    /* Consumer: block until Wake(), the timeout, or a spurious wakeup. The
       caller must re-check its queues afterwards either way. Returns at once
       if a Wake() came between PrepareToPark() and here. */
    void Park(std::chrono::nanoseconds timeout) {
      #if defined(__linux__)
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
      struct timespec ts;
      ts.tv_sec = secs.count();
      ts.tv_nsec = (timeout - secs).count();
      /* EINTR, EAGAIN (the state already changed) and ETIMEDOUT all just
         mean "go and look again". */
      syscall(SYS_futex, reinterpret_cast<uint32_t *>(&State), FUTEX_WAIT_PRIVATE, Parked, &ts, nullptr, 0);
      #else
      (void)timeout;
      State.wait(Parked, std::memory_order_seq_cst);
      #endif
      State.store(Running, std::memory_order_relaxed);
    }

    /* Producer: call after pushing work (with a seq_cst RMW) where the
       consumer will look for it. */
    void Wake() {
      if (State.load(std::memory_order_seq_cst) == Parked &&
          State.exchange(Running, std::memory_order_seq_cst) == Parked) {
        #if defined(__linux__)
        syscall(SYS_futex, reinterpret_cast<uint32_t *>(&State), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
        #else
        State.notify_one();
        #endif
      }
    }

    private:

    static constexpr uint32_t Running = 0U;
    static constexpr uint32_t Parked = 1U;

    static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "futex needs a plain 32-bit word");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "futex needs a lock-free word");

    std::atomic<uint32_t> State;

  };  // TParker

}  // Base
