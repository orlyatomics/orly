/* <orly/server/runner_balancer.h>

   Which of a set of runners a statement should go to (#908).

   A fiber doesn't yield while it computes, so a statement latched onto a runner that is busy with
   a long one (a 4,096-call batch, say) waits for that one to finish first. Blind round-robin sent
   every Nth statement there, so under a steady stream of large batches each reading session
   stalled once per batch, for most of the batch.

   This sends each statement to the runner with the least work on it (join the shortest queue).
   A statement holds a lease on its runner from the moment it is assigned until it is done, so a
   runner that is stuck on a long statement keeps a load of at least one, and the statements that
   queue behind it raise its load further; new work goes elsewhere for as long as another runner
   has less. Ties go in rotation, so with nothing in flight the assignment is the plain
   round-robin it used to be, and the load still spreads.

   Counting statements alone still lets a few reads in behind a long one: with many sessions,
   every runner has a statement or two in flight, so the one holding a single long batch often
   looks no busier than the rest. A lease therefore has a weight, the work it stands for in units
   of one small statement; a batch weighs as many as its calls (Charge(), once it knows), and the
   balancer steers around it until the other runners have that much queued too.

   The loads are a hint, not an invariant: two statements assigned at the same moment may both
   pick the same runner. That costs no more than round-robin did, so the counters need no
   ordering beyond their own atomicity.

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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

#include <base/class_traits.h>

namespace Orly {

  namespace Server {

    /* Picks the least-loaded of 'size' runners, known by index; see above. Thread-safe. */
    class TRunnerBalancer final {
      NO_COPY(TRunnerBalancer);
      public:

      /* One statement's claim on a runner. Releasing it (or destroying it) takes the statement
         off that runner's load. Movable, so it can travel with the statement. */
      class TLease final {
        NO_COPY(TLease);
        public:

        /* An empty lease: names no runner. */
        TLease() noexcept = default;

        TLease(TLease &&that) noexcept
            : Balancer(std::exchange(that.Balancer, nullptr)), Index(that.Index), Weight(that.Weight) {}

        TLease &operator=(TLease &&that) noexcept {
          if (this != &that) {
            Release();
            Balancer = std::exchange(that.Balancer, nullptr);
            Index = that.Index;
            Weight = that.Weight;
          }
          return *this;
        }

        ~TLease() {
          Release();
        }

        explicit operator bool() const noexcept {
          return Balancer != nullptr;
        }

        /* The runner this lease is on. */
        [[nodiscard]] size_t GetIndex() const noexcept {
          assert(Balancer);
          return Index;
        }

        /* Take the statement off its runner's load, once; leaves the lease empty. */
        void Release() noexcept {
          if (Balancer) {
            [[maybe_unused]] const size_t prev = Balancer->Slots[Index].Load.fetch_sub(Weight, std::memory_order_relaxed);
            assert(prev >= Weight);
            Balancer = nullptr;
          }
        }

        private:

        TLease(TRunnerBalancer *balancer, size_t index, size_t weight) noexcept
            : Balancer(balancer), Index(index), Weight(weight) {}

        TRunnerBalancer *Balancer = nullptr;

        size_t Index = 0UL;

        size_t Weight = 0UL;

        friend class TRunnerBalancer;

      };  // TLease

      explicit TRunnerBalancer(size_t size)
          : Size(size), Slots(std::make_unique<TSlot[]>(size)) {
        if (!size) {
          throw std::invalid_argument("TRunnerBalancer needs at least one runner");
        }
      }

      [[nodiscard]] size_t GetSize() const noexcept {
        return Size;
      }

      /* The total weight of the leases out on the given runner. */
      [[nodiscard]] size_t GetLoad(size_t index) const noexcept {
        assert(index < Size);
        return Slots[index].Load.load(std::memory_order_relaxed);
      }

      /* A lease of the given weight (at least 1) on the runner with the least load. Among equals,
         the first one at or after the next position in rotation. Stops looking at the first idle
         runner. */
      [[nodiscard]] TLease Acquire(size_t weight = 1UL) noexcept {
        weight = std::max(weight, 1UL);
        size_t best = Next.fetch_add(1UL, std::memory_order_relaxed) % Size;
        size_t best_load = GetLoad(best);
        for (size_t step = 1UL, index = best; best_load && step < Size; ++step) {
          if (++index == Size) {
            index = 0UL;
          }
          const size_t load = GetLoad(index);
          if (load < best_load) {
            best = index;
            best_load = load;
          }
        }
        return Charge(best, weight);
      }

      /* A lease of the given weight on the given runner: for work that is already there, as a
         batch is once it knows its size. Weight 0 gives an empty lease. */
      [[nodiscard]] TLease Charge(size_t index, size_t weight) noexcept {
        assert(index < Size);
        if (!weight) {
          return TLease();
        }
        Slots[index].Load.fetch_add(weight, std::memory_order_relaxed);
        return TLease(this, index, weight);
      }

      private:

      /* A runner's load, on a cache line of its own: every assignment reads them all, and each
         runner's statements write their own. */
      struct alignas(64) TSlot {
        std::atomic<size_t> Load{0UL};
      };

      const size_t Size;

      const std::unique_ptr<TSlot[]> Slots;

      /* Where the next search starts. */
      std::atomic<size_t> Next{0UL};

    };  // TRunnerBalancer

  }  // Server

}  // Orly
