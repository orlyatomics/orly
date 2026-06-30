/* <orly/indy/util/merge_sorter.h>

   `TKeySorter<TRef>` -- an in-memory linked-list sorter that keeps
   elements in `(key, seq_num)` order on insert (ties broken by
   higher seq num first, so the freshest write wins on equal keys).
   Used by the index-merge paths where elements are produced
   incrementally and need to come out sorted.

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

      template <typename TRef>
      class TKeySorter {
        NO_COPY(TKeySorter);
        public:

        class TMergeElement {
          NO_COPY(TMergeElement);
          public:

          TMergeElement(TKeySorter *sorter, const TKey &key, TSequenceNumber seq_num, TRef ref)
              : Next(0), Key(key), SeqNum(seq_num), Ref(ref) {
            TMergeElement *first = sorter->First;
            TMergeElement *prev = 0;
            for (;;) {
              if (!first) break;
              Atom::TComparison comp = first->Key.Compare(Key);
              if (Atom::IsLt(comp) || (Atom::IsEq(comp) && first->SeqNum >= SeqNum)) {
                prev = first;
                first = first->Next;
              } else {
                break;
              }
            }
            if (prev) {
              prev->Next = this;
            } else {
              sorter->First = this;
            }
            Next = first;
          }

          ~TMergeElement() {}

          private:

          TMergeElement *Next;

          const TKey &Key;

          TSequenceNumber SeqNum;

          TRef Ref;

          friend class TKeySorter;

        };  // TMergeElement

        TKeySorter() : First(0) {}

        ~TKeySorter() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        TRef Pop() {
          assert(!IsEmpty());
          TRef ref = First->Ref;
          TMergeElement *first = First;
          First = First->Next;
          first->~TMergeElement();
          return ref;
        }

        bool IsEmpty() const {
          return First == 0;
        }

        private:

        TMergeElement *First;

      };  // TKeySorter

      template <typename TRef>
      class TKeyCopySorter {
        NO_COPY(TKeyCopySorter);
        public:

        class TMergeElement {
          NO_COPY(TMergeElement);
          public:

          TMergeElement(TKeyCopySorter *sorter, const TKey &key, TSequenceNumber seq_num, TRef ref)
              : Next(0), Key(key), SeqNum(seq_num), Ref(ref) {
            TMergeElement *first = sorter->First;
            TMergeElement *prev = 0;
            for (;;) {
              if (!first) break;
              Atom::TComparison comp = first->Key.Compare(Key);
              if (Atom::IsLt(comp) || (Atom::IsEq(comp) && first->SeqNum >= SeqNum)) {
                prev = first;
                first = first->Next;
              } else {
                break;
              }
            }
            if (prev) {
              prev->Next = this;
            } else {
              sorter->First = this;
            }
            Next = first;
          }

          ~TMergeElement() {}

          private:

          TMergeElement *Next;

          TKey Key;

          TSequenceNumber SeqNum;

          TRef Ref;

          friend class TKeyCopySorter;

        };  // TMergeElement

        TKeyCopySorter() : First(0) {}

        ~TKeyCopySorter() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        const TKey &Peek() const {
          assert(!IsEmpty());
          return First->Key;
        }

        TKey &Peek() {
          assert(!IsEmpty());
          return First->Key;
        }

        TRef Pop(TSequenceNumber &seq_num) {
          assert(!IsEmpty());
          TRef ref = First->Ref;
          seq_num = First->SeqNum;
          TMergeElement *first = First;
          First = First->Next;
          first->~TMergeElement();
          return ref;
        }

        bool IsEmpty() const {
          return First == 0;
        }

        private:

        TMergeElement *First;

      };  // TKeyCopySorter

      template <typename TRef>
      class TCoreSorter {
        NO_COPY(TCoreSorter);
        public:

        class TMergeElement {
          NO_COPY(TMergeElement);
          public:

          TMergeElement(TCoreSorter *sorter, const Atom::TCore *core, Atom::TCore::TArena *arena, TSequenceNumber seq_num, TRef ref)
              : Next(0), Core(core), Arena(arena), SeqNum(seq_num), Ref(ref) {
            TMergeElement *first = sorter->First;
            TMergeElement *prev = 0;
            void *lhs_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
            void *rhs_state_alloc = reinterpret_cast<uint8_t *>(lhs_state_alloc) + Sabot::State::GetMaxStateSize();
            for (;;) {
              if (!first) break;
              Atom::TComparison comp;
              if (!first->Core->TryQuickOrderComparison(first->Arena, *Core, Arena, comp)) {
                comp = Sabot::OrderStates(*Sabot::State::TAny::TWrapper(first->Core->NewState(first->Arena, lhs_state_alloc)),
                                          *Sabot::State::TAny::TWrapper(Core->NewState(Arena, rhs_state_alloc)));
              }
              if (Atom::IsLt(comp) || (Atom::IsEq(comp) && first->SeqNum >= SeqNum)) {
                prev = first;
                first = first->Next;
              } else {
                break;
              }
            }
            if (prev) {
              prev->Next = this;
            } else {
              sorter->First = this;
            }
            Next = first;
          }

          ~TMergeElement() {}

          private:

          TMergeElement *Next;

          const Atom::TCore *Core;

          Atom::TCore::TArena *Arena;

          TSequenceNumber SeqNum;

          TRef Ref;

          friend class TCoreSorter;

        };  // TMergeElement

        TCoreSorter() : First(0) {}

        ~TCoreSorter() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        void Clear() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        TRef Pop(TSequenceNumber &seq_num) {
          assert(!IsEmpty());
          TRef ref = First->Ref;
          seq_num = First->SeqNum;
          TMergeElement *first = First;
          First = First->Next;
          first->~TMergeElement();
          return ref;
        }

        bool IsEmpty() const {
          return First == 0;
        }

        private:

        TMergeElement *First;

      };  // TCoreSorter

      template <typename TRef>
      class TDurableSorter {
        NO_COPY(TDurableSorter);
        public:

        class TMergeElement {
          NO_COPY(TMergeElement);
          public:

          TMergeElement(TDurableSorter *sorter, const uuid_t &id, TSequenceNumber seq_num, TRef ref)
              : Next(0), SeqNum(seq_num), Ref(ref) {
            uuid_copy(Id, id);
            TMergeElement *first = sorter->First;
            TMergeElement *prev = 0;
            for (;;) {
              if (!first) break;
              int comp = uuid_compare(first->Id, Id);
              if (comp < 0 || (comp == 0 && first->SeqNum >= SeqNum)) {
                prev = first;
                first = first->Next;
              } else {
                break;
              }
            }
            if (prev) {
              prev->Next = this;
            } else {
              sorter->First = this;
            }
            Next = first;
          }

          ~TMergeElement() {}

          private:

          TMergeElement *Next;

          uuid_t Id;

          const TSequenceNumber SeqNum;

          const TRef Ref;

          friend class TDurableSorter;

        };  // TMergeElement

        TDurableSorter() : First(0) {}

        ~TDurableSorter() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        TRef Pop(TSequenceNumber &seq_num, uuid_t &id) {
          assert(!IsEmpty());
          TRef ref = First->Ref;
          seq_num = First->SeqNum;
          uuid_copy(id, First->Id);
          TMergeElement *first = First;
          First = First->Next;
          first->~TMergeElement();
          return ref;
        }

        bool IsEmpty() const {
          return First == 0;
        }

        private:

        TMergeElement *First;

      };  // TDurableSorter

      template <typename TVal, typename TRef, class TComparator = std::less<TVal>>
      class TMergeSorter {
        NO_COPY(TMergeSorter);
        public:

        class TMergeElement {
          NO_COPY(TMergeElement);
          public:

          TMergeElement(TMergeSorter *sorter, const TVal &val, TRef ref)
              : Next(0), Val(val), Ref(ref) {
            TMergeElement *first = sorter->First;
            TMergeElement *prev = 0;
            for (;;) {
              if (!first) break;
              if (sorter->Comp(first->Val, Val)) {
                prev = first;
                first = first->Next;
              } else {
                break;
              }
            }
            if (prev) {
              prev->Next = this;
            } else {
              sorter->First = this;
            }
            Next = first;
          }

          ~TMergeElement() {}

          private:

          TMergeElement *Next;

          const TVal &Val;

          TRef Ref;

          friend class TMergeSorter;

        };  // TMergeElement

        TMergeSorter(const TComparator &comp = std::less<TVal>())
            : Comp(comp), First(0) {}

        ~TMergeSorter() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        void Clear() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        const TVal &Peek() const {
          assert(!IsEmpty());
          return First->Val;
        }

        const TVal &Pop(TRef &ref) {
          assert(!IsEmpty());
          ref = First->Ref;
          const TVal &val = First->Val;
          TMergeElement *first = First;
          First = First->Next;
          first->~TMergeElement();
          return val;
        }

        bool IsEmpty() const {
          return First == 0;
        }

        private:

        TComparator Comp;

        TMergeElement *First;

      };  // TMergeSorter

      template <typename TVal, typename TRef, class TComparator = std::less<TVal>>
      class TCopyMergeSorter {
        NO_COPY(TCopyMergeSorter);
        public:

        class TMergeElement {
          NO_COPY(TMergeElement);
          public:

          TMergeElement(TCopyMergeSorter *sorter, const TVal &val, TRef ref)
              : Next(0), Val(val), Ref(ref) {
            TMergeElement *first = sorter->First;
            TMergeElement *prev = 0;
            for (;;) {
              if (!first) break;
              if (sorter->Comp(first->Val, Val)) {
                prev = first;
                first = first->Next;
              } else {
                break;
              }
            }
            if (prev) {
              prev->Next = this;
            } else {
              sorter->First = this;
            }
            Next = first;
          }

          ~TMergeElement() {}

          private:

          TMergeElement *Next;

          const TVal Val;

          TRef Ref;

          friend class TCopyMergeSorter;

        };  // TMergeElement

        TCopyMergeSorter(const TComparator &comp = std::less<TVal>())
            : Comp(comp), First(0) {}

        ~TCopyMergeSorter() {
          while (First) {
            TMergeElement *first = First;
            First = First->Next;
            first->~TMergeElement();
          }
        }

        const TVal &Peek() const {
          assert(!IsEmpty());
          return First->Val;
        }

        TVal Pop(TRef &ref) {
          assert(!IsEmpty());
          ref = First->Ref;
          const TVal val = First->Val;
          TMergeElement *first = First;
          First = First->Next;
          first->~TMergeElement();
          return val;
        }

        bool IsEmpty() const {
          return First == 0;
        }

        private:

        TComparator Comp;

        TMergeElement *First;

      };  // TCopyMergeSorter

    }  // Util

  }  // Indy

}  // Orly