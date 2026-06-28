/* <base/thread_local_sigma_calc.h>

   A drop-in replacement for TSigmaCalc whose Push() path never contends across
   producer threads.

   TSigmaCalc itself is not thread-safe, so concurrent producers historically
   had to serialize their Push() calls behind a single global mutex. That mutex
   then became per-event contention on hot paths (e.g. every server `Try`
   pushing latency stats). This wrapper keeps a private TSigmaCalc per producing
   thread, each behind its own lock: a thread's Push() takes only that thread's
   lock, so producers never contend with one another (only, briefly, with the
   reporter). Report() merges all the per-thread instances into a single
   aggregate using TSigmaCalc::Add (parallel Welford), so the reported
   count/min/max/mean/sigma are exact across all threads.

   Lifetime safety (this is why the state is a heap TBlock): the shared state
   lives in a heap block co-owned via std::shared_ptr by the
   TThreadLocalSigmaCalc and by every thread that has pushed into it. Either may
   be destroyed first:
     - thread exits first: its registry folds its calculator into the block's
       retired accumulator under the block's mutex, so the samples survive.
     - owner destroyed first: it just drops its shared_ptr; surviving thread
       registries keep the block (and its mutexes) alive until they exit and
       fold/remove their own entries.
   Because every mutex lives in the shared block, no destructor ever locks a
   mutex it does not co-own, and no destructor throws.

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
#include <memory>
#include <mutex>
#include <unordered_map>

#include <base/class_traits.h>
#include <base/sigma_calc.h>

namespace Base {

  /* See file comment. Exposes the subset of the TSigmaCalc interface used by
     contended producers: Push() (per-thread lock, uncontended among producers),
     and Report()/Reset() (which fold across all threads). Not copyable or
     movable -- identity is the heap control block it owns. */
  class TThreadLocalSigmaCalc {
    NO_COPY(TThreadLocalSigmaCalc);
    public:

    TThreadLocalSigmaCalc()
        : Block(std::make_shared<TBlock>()) {}

    /* Push a value into the calling thread's private calculator, under that
       thread's own lock -- no contention with other producers. */
    void Push(double val) {
      TEntry &entry = GetThreadEntry();
      std::lock_guard<std::mutex> lock(entry.Mutex);
      entry.Calc.Push(val);
    }

    /* Merge every thread's calculator into one aggregate and report it, exactly
       as TSigmaCalc::Report would for the union of all pushes. */
    size_t Report(double &min, double &max, double &mean, double &sigma) const {
      std::lock_guard<std::mutex> structure_lock(Block->Mutex);
      TSigmaCalc aggregate(Block->Retired);
      for (const auto &item : Block->Entries) {
        TEntry &entry = *item.second;
        std::lock_guard<std::mutex> entry_lock(entry.Mutex);
        aggregate.Add(entry.Calc);
      }
      return aggregate.Report(min, max, mean, sigma);
    }

    /* Reset every thread's calculator (and the retired accumulator). */
    void Reset() {
      std::lock_guard<std::mutex> structure_lock(Block->Mutex);
      Block->Retired.Reset();
      for (auto &item : Block->Entries) {
        TEntry &entry = *item.second;
        std::lock_guard<std::mutex> entry_lock(entry.Mutex);
        entry.Calc.Reset();
      }
    }

    /* Report the aggregate across all threads AND reset, atomically -- every
       value pushed so far is counted exactly once and then cleared, with no gap
       (unlike a separate Report() then Reset(), where a value landing between the
       two would be dropped). This is what periodic reporters want. */
    size_t Drain(double &min, double &max, double &mean, double &sigma) {
      std::lock_guard<std::mutex> structure_lock(Block->Mutex);
      TSigmaCalc aggregate(Block->Retired);
      Block->Retired.Reset();
      for (auto &item : Block->Entries) {
        TEntry &entry = *item.second;
        std::lock_guard<std::mutex> entry_lock(entry.Mutex);
        aggregate.Add(entry.Calc);
        entry.Calc.Reset();
      }
      return aggregate.Report(min, max, mean, sigma);
    }

    private:

    /* One producing thread's calculator plus its own lock. Pointer-stable in the
       owning unordered_map (node-based), so a thread can cache the address for
       its lock-light Push path. */
    struct TEntry {
      std::mutex Mutex;
      TSigmaCalc Calc;
    };  // TEntry

    /* The shared state for one TThreadLocalSigmaCalc, kept on the heap and
       co-owned (via shared_ptr) by the owner and by every thread registry that
       references it, so it outlives whichever is destroyed last. */
    struct TBlock {
      /* Guards the structure of Entries (insert/erase/iterate) and Retired. A
       producer's Push does NOT take this lock once registered; it takes only its
       own TEntry::Mutex. */
      std::mutex Mutex;
      /* Per-thread calculators, keyed by the thread's registry-entry token (a
         stable per-thread address). Guarded by Mutex for structure; each value's
         data is guarded by its own TEntry::Mutex. */
      std::unordered_map<const void *, std::unique_ptr<TEntry>> Entries;
      /* Stats of threads (or owners) retired since the last Reset(), folded in
         so their contribution is not lost. Guarded by Mutex. */
      TSigmaCalc Retired;
    };  // TBlock

    /* Per-thread bookkeeping: caches, for every TThreadLocalSigmaCalc this
       thread has pushed into, the shared block and this thread's entry within
       it. On thread exit it folds each calculator into the corresponding
       (still-alive) block's retired accumulator under that block's mutex --
       never touching the owner, which may already be gone. */
    class TRegistry {
      NO_COPY(TRegistry);
      public:

      TRegistry() = default;

      ~TRegistry() {
        for (auto &item : Blocks) {
          const std::shared_ptr<TBlock> &block = item.second.Block;
          std::lock_guard<std::mutex> lock(block->Mutex);
          /* Holding the block mutex excludes Report()/Reset(); the only other
             accessor of this entry would be this same thread's Push(), which
             cannot run while the thread is exiting. So folding the calc here is
             race-free without taking the per-entry lock. */
          auto iter = block->Entries.find(Token());
          if (iter != block->Entries.end()) {
            block->Retired.Add(iter->second->Calc);
            block->Entries.erase(iter);
          }
        }
      }

      /* This thread's entry within `block`, creating and registering it on first
         use. The returned reference is pointer-stable for the thread's life. */
      TEntry &GetEntryFor(const std::shared_ptr<TBlock> &block) {
        auto iter = Blocks.find(block.get());
        if (iter != Blocks.end()) {
          return *iter->second.Entry;
        }
        std::lock_guard<std::mutex> lock(block->Mutex);
        auto &slot = block->Entries[Token()];
        slot = std::make_unique<TEntry>();
        TEntry *entry = slot.get();
        Blocks.emplace(block.get(), TRef{block, entry});
        return *entry;
      }

      private:

      /* A stable per-thread token: the address of this registry. Unique while
         the thread lives; never dereferenced. */
      const void *Token() const {
        return this;
      }

      struct TRef {
        std::shared_ptr<TBlock> Block;  // keeps the block alive
        TEntry *Entry;                  // pointer-stable cache for the hot path
      };

      std::unordered_map<const TBlock *, TRef> Blocks;
    };  // TRegistry

    TEntry &GetThreadEntry() const {
      thread_local TRegistry registry;
      return registry.GetEntryFor(Block);
    }

    /* The shared control block. Co-owned with every thread registry that has
       pushed into this instance. */
    std::shared_ptr<TBlock> Block;

  };  // TThreadLocalSigmaCalc

}  // Base
