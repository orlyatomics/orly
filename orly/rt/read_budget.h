/* <orly/rt/read_budget.h>

   The part of the per-read budget (#694) that is charged while a method runs (#729).

   #694 counts the rows a call walks and the bytes its result takes in the call's arena. A call
   that computes a big value from few rows, such as `[0..n] as [int]`, passed neither until its
   result reached the arena, so it could take gigabytes, or run forever, first. The runtime the
   generated code is built from charges this budget as it goes instead:

     - steps: one for each element a sequence source yields (a range, or a list, set or dict
       walked as a sequence), so a loop with no rows and no memory still ends;
     - bytes: the memory a call builds for its values, charged when a list grows its storage, a
       set or dict gains an element, or two lists, sets, dicts or strings are joined.

   A charge with no budget installed does nothing, so compile-time `test {}` blocks, Tetris and
   replication run as before. TContext::SetReadBudget() (orly/indy/context.h) installs the budget
   on the fiber that runs the call; a fiber can change threads mid-call, so the budget lives in
   the fiber's frame, not in a thread-local.

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

#include <cstddef>
#include <cstdint>

#include <base/class_traits.h>

namespace Orly {

  namespace Rt {

    /* Told as a call works, so that a long one can make room for others on its fast runner
       (#1015). A fiber doesn't yield while it computes, so without this a point read that lands
       behind a 1,000-row scan waits for the whole scan. */
    class TReadPacer {
      public:

      /* Called once every so much work (TReadBudget::SetPacer()), with the work done so far. May
         yield the fiber: nothing that charges a budget holds a lock across the charge. */
      virtual void Pace(size_t work) = 0;

      protected:

      ~TReadPacer() = default;

    };  // TReadPacer

    /* The in-flight budget of one method call (or one batch of them). */
    class TReadBudget final {
      NO_COPY(TReadBudget);
      public:

      /* 0 means no limit. */
      TReadBudget(size_t max_steps = 0UL, size_t max_bytes = 0UL) {
        SetLimits(max_steps, max_bytes);
      }

      void SetLimits(size_t max_steps, size_t max_bytes) {
        MaxSteps = max_steps ? max_steps : Unlimited;
        MaxBytes = max_bytes ? max_bytes : Unlimited;
      }

      /* Count steps and bytes; throws Server::TReadTooLarge once either passes its limit. */
      void Charge(size_t steps, size_t bytes) {
        Steps += steps;
        Bytes += bytes;
        if (Steps > MaxSteps || Bytes > MaxBytes) [[unlikely]] {
          OnOverBudget(Steps > MaxSteps);
        }
        Tick(steps);
      }

      /* Tell `pacer` every `every` units of work: a sequence step is one unit, and a row a
         context walks is RowWork of them (TContext::ChargeRow()). A null pacer or an `every` of
         0 stops pacing. */
      void SetPacer(TReadPacer *pacer, size_t every) noexcept {
        Pacer = every ? pacer : nullptr;
        PaceEvery = every;
        NextPace = Pacer ? Work + every : Unlimited;
      }

      /* Count work toward the pacer, without charging the budget. */
      void Tick(size_t work) {
        Work += work;
        if (Work >= NextPace) [[unlikely]] {
          NextPace = Work + PaceEvery;
          Pacer->Pace(Work);
        }
      }

      /* The work one row stands for, in sequence steps: a row costs microseconds, a step tens of
         nanoseconds. */
      static constexpr size_t RowWork = 64UL;

      /* Throws as Charge() would if this much more were charged, but charges nothing. For work
         whose size is known before it starts. */
      void CheckAhead(size_t steps, size_t bytes) const;

      size_t GetSteps() const {
        return Steps;
      }

      size_t GetBytes() const {
        return Bytes;
      }

      private:

      static constexpr size_t Unlimited = static_cast<size_t>(-1);

      /* Throws Server::TReadTooLarge, naming the limit that was passed. */
      [[noreturn]] void OnOverBudget(bool steps) const;

      size_t Steps = 0UL, Bytes = 0UL;

      size_t MaxSteps, MaxBytes;

      /* See SetPacer(). */
      TReadPacer *Pacer = nullptr;
      size_t Work = 0UL, PaceEvery = 0UL, NextPace = Unlimited;

    };  // TReadBudget

    /* Makes `budget` the one charged by code running on this fiber (or, off any fiber, this
       thread) until it goes, then puts back whatever was there before. */
    class TReadBudgetScope final {
      NO_COPY(TReadBudgetScope);
      public:

      explicit TReadBudgetScope(TReadBudget *budget);

      ~TReadBudgetScope();

      private:

      /* Where the budget was installed, and what it replaced. */
      TReadBudget **Slot;
      TReadBudget *Prev;

    };  // TReadBudgetScope

    /* The budget installed by the innermost TReadBudgetScope on this fiber, or null. */
    TReadBudget *GetCurrentReadBudget();

    /* Charge the current budget, if there is one. */
    inline void ChargeReadBudget(size_t steps, size_t bytes) {
      if (TReadBudget *budget = GetCurrentReadBudget()) {
        budget->Charge(steps, bytes);
      }
    }

    /* See TReadBudget::CheckAhead(). */
    inline void CheckReadBudgetAhead(size_t steps, size_t bytes) {
      if (TReadBudget *budget = GetCurrentReadBudget()) {
        budget->CheckAhead(steps, bytes);
      }
    }

    /* The bytes a node-based container (TSet, TDict) spends on one element: the element and the
       tree node's three pointers and colour. */
    template <typename TVal>
    constexpr size_t GetNodeSize() {
      return sizeof(TVal) + 4UL * sizeof(void *);
    }

    /* Charge what a vector is about to allocate to hold `extra` more elements: a vector's
       storage grows geometrically, so this charges O(log n) times for n push_backs, and charges
       the memory before it is taken. */
    template <typename TVec>
    void ChargeVectorGrowth(const TVec &vec, size_t extra) {
      const size_t need = vec.size() + extra;
      if (need > vec.capacity()) [[unlikely]] {
        /* libstdc++ grows to size + max(size, n); match it so the charge is the allocation. */
        const size_t grown = vec.size() + (vec.size() > extra ? vec.size() : extra);
        ChargeReadBudget(0UL, (grown - vec.capacity()) * sizeof(typename TVec::value_type));
      }
    }

  }  // Rt

}  // Orly
