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

        TMinHeap(size_t max_elem, const TComparator &comp = std::less<TVal>())
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
          while (i > 0 && Comp(*(Data[i].Val), *(Data[Parent(i)].Val))) {
            std::swap(Data[i], Data[Parent(i)]);
            i = Parent(i);
          }
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
          Data[0] = std::move(Data[NumElem - 1]);
          Data[NumElem - 1].~TMinHeapElem();
          --NumElem;
          MinHeapify(0);
          return min_ref;
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
          if (left < NumElem && Comp(*(Data[left].Val), *(Data[i].Val))) {
            smallest = left;
          } else {
            smallest = i;
          }
          if (right < NumElem && Comp(*(Data[right].Val), *(Data[smallest].Val))) {
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