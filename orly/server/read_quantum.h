/* <orly/server/read_quantum.h>

   When a long read gives way on its fast runner (#1015).

   Statements run as fibers on a few fast runners, and a fiber runs until it gives way. A read
   that walks many rows would hold its runner for all of them, and a point read queued behind it
   would wait for the whole read. So a method call's reads are paced (Rt::TReadPacer): every few
   rows the call looks at the clock, and once it has run for a quantum without a break, and
   another fiber is ready to run on its runner, it yields.

   Yielding on time rather than on rows keeps what a yield costs (a trip through the runner's
   queue, and the cache the read had warmed) a small, fixed share of a long read's run time,
   however cheap its rows are, while bounding how long a short read can wait behind it to about
   one quantum. A read shorter than the quantum never yields at all.

   The quantum runs from the last time the call was switched in, for whatever reason: its own
   yield, or a wait for a page from disk. Time spent switched out isn't time the call held the
   runner. The call knows it was switched out because its runner's switch count moved since it
   last looked (Indy::Fiber::TRunner::GetSwitchCount()); while it runs, nothing else switches on
   that runner. So the slice starts at the first look after the call is switched in, up to one
   look's worth of rows late.

   It yields only to a fiber that can run now (Indy::Fiber::TRunner::HasOtherReadyFrame()), not
   to statements that are on the runner but parked waiting for disk.

   As it walks, the call also adds to its runner's load in the balancer (TRunnerBalancer), one
   small statement's worth per ScanUnit, so that new statements steer around a runner that is
   busy with a long read; that keeps short the queue a yield has to wait through. It does so at
   most once every WeighEvery, since each time is an atomic add on a line the other runners'
   statements write too.

   The policy is here, apart from the fiber runner, so it can be tested with a fake clock and
   runner: TDerived provides

     TClock::time_point Now();
     TPlace Where();         // The runner the call is on, and that runner's switch count.
     bool OthersReady();     // Is another fiber ready to run on that runner?
     void GiveWay();         // Yield the fiber.
     void Weigh(size_t n);   // Add n small statements' worth to the runner's load.

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

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <orly/rt/read_budget.h>

namespace Orly {

  namespace Server {

    /* See above. */
    template <typename TDerived>
    class TReadQuantum : public Rt::TReadPacer {
      public:

      using TClock = std::chrono::steady_clock;

      /* Where a call is running: its runner, and how many times that runner had switched frames
         when it looked. */
      struct TPlace {
        const void *Runner = nullptr;
        uint64_t Switches = 0UL;
        bool operator==(const TPlace &) const = default;
      };

      /* See Rt::TReadPacer. Weighs first, so the balancer sees the weight while the call is
         yielded. */
      void Pace(size_t work) final {
        TDerived &self = static_cast<TDerived &>(*this);
        const TClock::time_point now = self.Now();
        const size_t units = work / ScanUnit;
        if (units > Charged && (!Charged || now - LastWeighed >= WeighEvery)) {
          self.Weigh(units - Charged);
          Charged = units;
          LastWeighed = now;
        }
        const TPlace place = self.Where();
        if (place != Place) {
          /* Switched out since the last look, or the first look: the slice starts now. */
          Place = place;
          SliceStart = now;
          return;
        }
        if (Quantum.count() && now - SliceStart >= Quantum && self.OthersReady()) {
          ++Yields;
          /* Comes back with the switch count moved, so the next look starts a new slice. */
          self.GiveWay();
        }
      }

      /* How many times the call has yielded. */
      [[nodiscard]] size_t GetYields() const noexcept {
        return Yields;
      }

      /* The work, in Rt::TReadBudget units, that weighs as much as one small statement: 32 rows,
         about what a point read costs end to end. */
      static constexpr size_t ScanUnit = 32UL * Rt::TReadBudget::RowWork;

      /* The least time between two additions to the runner's load (see above). */
      static constexpr std::chrono::microseconds WeighEvery{100};

      protected:

      /* 0 never yields, but still weighs. */
      explicit TReadQuantum(std::chrono::microseconds quantum) noexcept
          : Quantum(quantum) {}

      ~TReadQuantum() = default;

      private:

      /* How long a slice runs before the call yields to a ready fiber. */
      const std::chrono::microseconds Quantum;

      /* Where the call was at the last look, and when its slice began. */
      TPlace Place;
      TClock::time_point SliceStart;

      /* What Weigh() has added, in small statements, and when it last did. */
      size_t Charged = 0UL;
      TClock::time_point LastWeighed;

      /* See GetYields(). */
      size_t Yields = 0UL;

    };  // TReadQuantum<TDerived>

  }  // Server

}  // Orly
