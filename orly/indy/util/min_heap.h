/* <orly/indy/util/min_heap.h>

   `TMinHeap<TVal, TRef, TComparator>` -- a min-heap that yields
   the smallest of a set of (value, ref) pairs. Used by every k-way
   merge in the disk layer (`TIndexManager::TCursor`,
   the merge sorter, present/update walker fan-in).

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
#include <type_traits>

#include <orly/atom/kit2.h>
#include <orly/indy/key.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    namespace Util {

      template <typename TVal, typename TRef, class TComparator = std::less<TVal>>
      class TMinHeap {
        NO_COPY(TMinHeap);
        public:

        class TMinHeapElem {
          public:

          TMinHeapElem(const TVal &val, TRef ref) : Val(&val), Ref(ref) {}

          TMinHeapElem(const TMinHeapElem &&that) : Val(that.Val), Ref(that.Ref) {}

          TMinHeapElem &operator=(TMinHeapElem &&that) {
            if (this != &that) {
              std::swap(Val, that.Val);
              std::swap(Ref, that.Ref);
            }
            return *this;
          }

          private:

          const TVal *Val;

          TRef Ref;

          friend class TMinHeap;

        };  // TMinHeapElem

        /* The comparator takes (lhs, rhs) or, to break ties by the elements' refs,
           (lhs, lhs_ref, rhs, rhs_ref). */
        TMinHeap(size_t max_elem, const TComparator &comp = TComparator())
            : MaxElem(max_elem), NumElem(0UL), Data(nullptr), Comp(comp) {
          Data = reinterpret_cast<TMinHeapElem *>(malloc(MaxElem *sizeof(TMinHeapElem)));
          if (!Data) {
            throw std::bad_alloc();
          }
        }

        ~TMinHeap() {
          for (size_t i = 0; i < NumElem; ++i) {
            Data[i].~TMinHeapElem();
          }
          free(Data);
        }

        inline operator bool() const {
          return NumElem > 0;
        }

        inline void Insert(const TVal &val, TRef ref) {
          assert(NumElem < MaxElem);
          size_t i = NumElem;
          ++NumElem;
          new (Data + i) TMinHeapElem(val, ref);
          SiftUp(i);
        }

        inline const TVal &Peek(TRef &ref) const {
          assert(NumElem > 0);
          const TMinHeapElem &min_val = Data[0];
          ref = min_val.Ref;
          return *(min_val.Val);
        }

        inline const TVal &Pop(TRef &ref) {
          assert(NumElem > 0);
          const TMinHeapElem &min_val = Data[0];
          ref = min_val.Ref;
          const TVal &min_ref = *(min_val.Val);
          --NumElem;
          if (NumElem) {
            SiftDown(0, Data[NumElem].Val, Data[NumElem].Ref);
          }
          Data[NumElem].~TMinHeapElem();
          return min_ref;
        }

        /* Replace the value of the minimum element, keeping its ref, then restore heap order with a
           single sift-down. This is Pop() followed by Insert(val, ref) for the same ref, minus one pass:
           if the new value still orders before the root's children, nothing moves. The heap must be
           non-empty. */
        inline void ReplaceTop(const TVal &val) {
          assert(NumElem > 0);
          SiftDown(0, &val, Data[0].Ref);
        }

        private:

        inline bool Less(const TMinHeapElem &lhs, const TMinHeapElem &rhs) const {
          return Less(lhs.Val, lhs.Ref, rhs.Val, rhs.Ref);
        }

        inline bool Less(const TVal *lhs, const TRef &lhs_ref, const TVal *rhs, const TRef &rhs_ref) const {
          if constexpr (std::is_invocable_r_v<bool, const TComparator &, const TVal &, const TRef &, const TVal &, const TRef &>) {
            return Comp(*lhs, lhs_ref, *rhs, rhs_ref);
          } else {
            return Comp(*lhs, *rhs);
          }
        }

        /* Return the offset of the parent of offset i. */
        inline size_t Parent(size_t i) const {
          assert(i > 0);
          return ((i + 1) / 2) - 1;
        }

        /* Return the offset of the left child of offset i. */
        inline size_t Left(size_t i) const {
          return (2 * (i + 1)) - 1;
        }

        /* Return the offset of the right child of offset i. */
        inline size_t Right(size_t i) const {
          return 2 * (i + 1);
        }

        /* Move the element at offset i up to its place. Parents that order after it shift down one level
           into the hole it leaves, and it is written once, where the hole stops, rather than swapped at
           every level. */
        void SiftUp(size_t i) {
          const TVal *val = Data[i].Val;
          const TRef ref = Data[i].Ref;
          while (i > 0) {
            const size_t parent = Parent(i);
            if (!Less(val, ref, Data[parent].Val, Data[parent].Ref)) {
              break;
            }
            Data[i].Val = Data[parent].Val;
            Data[i].Ref = Data[parent].Ref;
            i = parent;
          }
          Data[i].Val = val;
          Data[i].Ref = ref;
        }

        /* Place (val, ref) in the subtree rooted at offset i, whose current element is discarded. The
           smaller child moves up into the hole until neither child orders before (val, ref), which is then
           written once. Ties keep (val, ref) above its children and prefer the left child, as the
           swap-based version did. */
        void SiftDown(size_t i, const TVal *val, TRef ref) {
          for (;;) {
            size_t child = Left(i);
            if (child >= NumElem) {
              break;
            }
            if (child + 1 < NumElem && Less(Data[child + 1], Data[child])) {
              ++child;
            }
            if (!Less(Data[child].Val, Data[child].Ref, val, ref)) {
              break;
            }
            Data[i].Val = Data[child].Val;
            Data[i].Ref = Data[child].Ref;
            i = child;
          }
          Data[i].Val = val;
          Data[i].Ref = ref;
        }

        const size_t MaxElem;

        size_t NumElem;

        TMinHeapElem *Data;

        /* Comparator */
        TComparator Comp;

      };  // TMinHeap

      template <typename TVal, typename TRef, class TComparator = std::less<TVal>>
      class TCopyMinHeap {
        NO_COPY(TCopyMinHeap);
        public:

        class TMinHeapElem {
          public:

          template <typename... TArgs>
          TMinHeapElem(TRef ref, TArgs &&...args) : Val(args...), Ref(ref) {}

          TMinHeapElem(const TMinHeapElem &&that) : Val(that.Val), Ref(that.Ref) {}

          TMinHeapElem &operator=(TMinHeapElem &&that) {
            if (this != &that) {
              std::swap(Val, that.Val);
              std::swap(Ref, that.Ref);
            }
            return *this;
          }

          private:

          TVal Val;

          TRef Ref;

          friend class TCopyMinHeap;

        };  // TMinHeapElem

        TCopyMinHeap(size_t max_elem, const TComparator &comp = std::less<TVal>())
            : MaxElem(max_elem), NumElem(0UL), Data(nullptr), Comp(comp) {
          Data = reinterpret_cast<TMinHeapElem *>(malloc(MaxElem *sizeof(TMinHeapElem)));
          if (!Data) {
            throw std::bad_alloc();
          }
        }

        ~TCopyMinHeap() {
          for (size_t i = 0; i < NumElem; ++i) {
            Data[i].~TMinHeapElem();
          }
          free(Data);
        }

        inline operator bool() const {
          return NumElem > 0;
        }

        template <typename... TArgs>
        inline void Emplace(TRef ref, TArgs &&...args) {
          assert(NumElem < MaxElem);
          size_t i = NumElem;
          ++NumElem;
          new (Data + i) TMinHeapElem(ref, args...);
          while (i > 0 && Comp(Data[i].Val, Data[Parent(i)].Val)) {
            std::swap(Data[i], Data[Parent(i)]);
            i = Parent(i);
          }
        }

        inline const TVal &Peek(TRef &ref) const {
          assert(NumElem > 0);
          const TMinHeapElem &min_val = Data[0];
          ref = min_val.Ref;
          return min_val.Val;
        }

        inline TRef Pop(TVal &val) {
          assert(NumElem > 0);
          TMinHeapElem &min_val = Data[0];
          TRef ref = min_val.Ref;
          val = min_val.Val;
          Data[0] = std::move(Data[NumElem - 1]);
          Data[NumElem - 1].~TMinHeapElem();
          --NumElem;
          MinHeapify(0);
          return ref;
        }

        private:

        /* Return the offset of the parent of offset i. */
        inline size_t Parent(size_t i) const {
          assert(i > 0);
          return ((i + 1) / 2) - 1;
        }

        /* Return the offset of the left child of offset i. */
        inline size_t Left(size_t i) const {
          return (2 * (i + 1)) - 1;
        }

        /* Return the offset of the right child of offset i. */
        inline size_t Right(size_t i) const {
          return 2 * (i + 1);
        }

        void MinHeapify(size_t i) {
          const size_t left = Left(i);
          const size_t right = Right(i);
          size_t smallest;
          if (left < NumElem && Comp(Data[left].Val, Data[i].Val)) {
            smallest = left;
          } else {
            smallest = i;
          }
          if (right < NumElem && Comp(Data[right].Val, Data[smallest].Val)) {
            smallest = right;
          }
          if (smallest != i) {
            std::swap(Data[i], Data[smallest]);
            MinHeapify(smallest);
          }
        }

        const size_t MaxElem;

        size_t NumElem;

        TMinHeapElem *Data;

        /* Comparator */
        TComparator Comp;

      };  // TCopyMinHeap

    }  // Util

  }  // Indy

}  // Orly