/* <orly/indy/disk/util/index_manager.h>

   External-merge-sort coordinator. `TIndexManager<TVal, MemSize, ...>`
   accumulates values in an in-memory sorter; when memory pressure or
   explicit flush triggers, the contents spill to disk as a
   `TIndexSortFile` (snappy-compressed sorted run). `TCursor` produces
   a globally-sorted stream by min-heaping across every spilled file
   plus the in-memory sorter -- classic k-way merge.

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
#include <cassert>
#include <functional>

#include <base/class_traits.h>
#include <base/inv_con/ordered_list.h>
#include <orly/indy/disk/util/index_sort_file.h>
#include <orly/indy/util/merge_sorter.h>
#include <orly/indy/util/min_heap.h>
#include <orly/indy/util/sorter.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        template <typename TVal, size_t MemSize, size_t MinParallelSortThreshold, class TComparator = std::less<TVal>>
        class TIndexManager {
          NO_COPY(TIndexManager);
          public:

          typedef TIndexManager<TVal, MemSize, MinParallelSortThreshold, TComparator> TMe;
          typedef TIndexSortFile<TIndexManager, TVal, MemSize, TComparator> TSortFile;
          typedef InvCon::OrderedList::TCollection<TIndexManager, TSortFile, size_t> TSortFileCollection;

          class TCursor {
            NO_COPY(TCursor);
            public:

            TCursor(TIndexManager *manager, size_t total_read_ahead_slots)
                : Manager(manager) {
              Manager->SortMem();
              size_t num_csr_required = 1UL;
              for (typename TSortFileCollection::TCursor csr(&Manager->SortFileCollection); csr; ++csr, ++num_csr_required) {}
              size_t read_ahead_per_csr = std::max(1UL, total_read_ahead_slots / num_csr_required);
              for (typename TSortFileCollection::TCursor csr(&Manager->SortFileCollection); csr; ++csr) {
                CsrVec.push_back(std::unique_ptr<typename Indy::Util::TSorter<TVal, MemSize>::TCursor>(new typename TSortFile::TCursor(&*csr, read_ahead_per_csr)));
              }
              CsrVec.push_back(std::unique_ptr<typename Indy::Util::TSorter<TVal, MemSize>::TCursor>(new typename Indy::Util::TSorter<TVal, MemSize>::TMemCursor(&Manager->MemSorter)));
              MinHeap = std::make_unique<Indy::Util::TMinHeap<TVal, size_t, TComparator>>(CsrVec.size(), manager->Comp);
              for (size_t i = 0; i < CsrVec.size(); ++i) {
                typename Indy::Util::TSorter<TVal, MemSize>::TCursor &csr = *CsrVec[i];
                if (csr) {
                  MinHeap->Insert(*csr, i);
                }
              }
            }

            ~TCursor() {
            }

            operator bool() const {
              return static_cast<bool>(*MinHeap);
            }

            const TVal &operator*() const {
              #ifndef NDEBUG
              if (!static_cast<bool>(*MinHeap)) {
                throw std::logic_error("empty minheap");
              }
              #endif
              assert(static_cast<bool>(*MinHeap));
              size_t dummy;
              return MinHeap->Peek(dummy);
            }

            const TVal *operator->() const {
              #ifndef NDEBUG
              if (!static_cast<bool>(*MinHeap)) {
                throw std::logic_error("empty minheap");
              }
              #endif
              assert(static_cast<bool>(*MinHeap));
              size_t dummy;
              return &(MinHeap->Peek(dummy));
            }

            TCursor &operator++() {
              size_t pos = 0U;
              MinHeap->Pop(pos);
              assert(pos < CsrVec.size());
              typename Indy::Util::TSorter<TVal, MemSize>::TCursor &csr = *CsrVec[pos];
              ++csr;
              if (csr) {
                MinHeap->Insert(*csr, pos);
              }
              return *this;
            }

            private:

            TIndexManager *Manager;

            std::vector<std::unique_ptr<typename Indy::Util::TSorter<TVal, MemSize>::TCursor>> CsrVec;

            std::unique_ptr<Indy::Util::TMinHeap<TVal, size_t, TComparator>> MinHeap;

          };  // TCursor

          TIndexManager(const Base::TCodeLocation &code_location /* DEBUG */,
                        uint8_t util_src,
                        size_t consolidation_threshold,
                        TVolume::TDesc::TStorageSpeed storage_speed,
                        TEngine *engine,
                        bool do_cache,
                        const TComparator &comp = std::less<TVal>())
              : StorageSpeed(storage_speed),
                Engine(engine),
                VolMan(Engine->GetVolMan()),
                BlockCache(Engine->GetBlockCache()),
                CacheInstr(do_cache ? CacheBlockOnly : ClearBlockOnly),
                Size(0UL),
                SortFileCollection(this),
                MemSorted(false),
                Comp(comp),
                CodeLocation(code_location),
                UtilSrc(util_src),
                ConsolidationThreshold(consolidation_threshold) {
            assert(consolidation_threshold > 0UL);
          }

          ~TIndexManager() {
            SortFileCollection.DeleteEachMember();
          }

          template <class... Args>
          void Emplace(Args &&... args) {
            if (MemSorter.IsFull()) {
              SortRange();
              new TSortFile(CodeLocation, UtilSrc, StorageSpeed, Engine, VolMan, BlockCache, 0UL, MemSorter, SortFileCollection, CacheInstr);
              MemSorter.Clear();
              /* #321 investigated and declined: this scan is bounded by ConsolidationThreshold
                 itself -- it stops (and triggers ConsolidateGeneration) as soon as any
                 generation's count reaches the threshold, so it can never walk more than a
                 threshold's worth of sort files no matter how large SortFileCollection grows
                 overall. A dedicated running-count structure would trade this simplicity for no
                 demonstrated win. */
              size_t gen = -1;
              size_t count = 0UL;
              bool done = false;
              for (typename TSortFileCollection::TCursor csr(&SortFileCollection); csr; ++csr) {
                if (csr.GetMembership()->GetKey() != gen) {
                  if (count >= ConsolidationThreshold) {
                    ConsolidateGeneration(gen, count);
                    done = true;
                    break;
                  }
                  gen = csr.GetMembership()->GetKey();
                  count = 1UL;
                } else {
                  ++count;
                }
              }
              if (!done && count >= ConsolidationThreshold) {
                ConsolidateGeneration(gen, count);
              }
            }
            MemSorted = false;
            MemSorter.Emplace(std::forward<Args>(args)...);
            ++Size;
          }

          void Clear() {
            MemSorter.Clear();
            MemSorted = false;
            SortFileCollection.DeleteEachMember();
            Size = 0UL;
          }

          inline size_t GetSize() const {
            return Size;
          }

          /* A check the in-memory sorts call while they run, and which may throw to abandon them (#1025). A sort of
             a full buffer takes seconds; without this a cancelled merge sat through it. Unset, sorting is plain
             std::sort. */
          void SetCancelPoll(std::function<void ()> poll) {
            CancelPoll = std::move(poll);
          }

          private:

          /* std::sort of the in-memory buffer; with a CancelPoll, the same quicksort with the check run on every
             partition step, so a sort can be abandoned within one pass over its current range. */
          void SortRange() {
            if (CancelPoll) {
              PollingSort(MemSorter.begin(), MemSorter.end(), 2UL * 64UL);
            } else {
              std::sort(MemSorter.begin(), MemSorter.end(), Comp);
            }
          }

          template <typename TIter>
          void PollingSort(TIter first, TIter last, size_t depth_budget) {
            static constexpr size_t SmallRange = 4096UL;
            while (static_cast<size_t>(last - first) > SmallRange) {
              CancelPoll();
              if (depth_budget == 0UL) {
                break;  // a bad run of pivots: std::sort's own heapsort fallback finishes it
              }
              --depth_budget;
              TIter mid = first + (last - first) / 2;
              TIter back = last - 1;
              /* median of three, as a copy: the partitions below move elements around it */
              const TVal &x = *first;
              const TVal &y = *mid;
              const TVal &z = *back;
              const TVal pivot = Comp(x, y) ? (Comp(y, z) ? y : (Comp(x, z) ? z : x))
                                            : (Comp(x, z) ? x : (Comp(y, z) ? z : y));
              /* [first, lo): less than pivot, [lo, hi): equal to it, [hi, last): greater. The equal run is
                 left out of both recursions, so every pass shrinks the range even on all-equal input. */
              TIter lo = std::partition(first, last, [&](const TVal &v) { return Comp(v, pivot); });
              TIter hi = std::partition(lo, last, [&](const TVal &v) { return !Comp(pivot, v); });
              if (lo - first < last - hi) {
                PollingSort(first, lo, depth_budget);
                first = hi;
              } else {
                PollingSort(hi, last, depth_budget);
                last = lo;
              }
            }
            CancelPoll();
            std::sort(first, last, Comp);
          }

          void SortMem() {
            if (!MemSorted) {
              SortRange();
              MemSorted = true;
            }
          }

          void ConsolidateGeneration(size_t gen, size_t num) {
            new TSortFile(CodeLocation, UtilSrc, StorageSpeed, Engine, VolMan, BlockCache, gen, num, SortFileCollection, 10UL, Comp, CacheInstr);
          }

          TVolume::TDesc::TStorageSpeed StorageSpeed;

          TEngine *Engine;

          TVolumeManager *VolMan;

          TBlockCache *BlockCache;

          TCacheInstr CacheInstr;

          size_t Size;

          Indy::Util::TSorter<TVal, MemSize> MemSorter;

          mutable typename TSortFileCollection::TImpl SortFileCollection;

          bool MemSorted;

          TComparator Comp;

          std::function<void ()> CancelPoll;

          /* DEBUG */
          const Base::TCodeLocation CodeLocation;

          const uint8_t UtilSrc;

          size_t ConsolidationThreshold;

        };  // TIndexManager

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly