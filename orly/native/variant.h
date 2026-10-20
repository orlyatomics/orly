/* <orly/native/variant.h>

   Sabot type and state for native (generated) variant types.

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
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

#include <base/class_traits.h>
#include <orly/native/state.h>
#include <orly/native/type.h>
#include <orly/sabot/state.h>
#include <orly/sabot/type.h>

namespace Orly {

  namespace Native {

    /* The arms of a generated variant. The compiler specializes this for each
       variant it gives a native sabot (see orly/code_gen/variant.cc), with:

         static size_t GetArmCount();
         static const char *GetArmName(size_t arm);
         static size_t GetWhich(const TVariant &val);
         static Sabot::Type::TOpt *NewArmOptType(size_t arm, void *type_alloc);
         static Sabot::State::TAny *NewPayloadState(const TVariant &val, void *state_alloc);

       Arms are numbered in tag order, which is the variant's `Which`.
       NewPayloadState returns a state for the active arm's payload, which must
       be stored in `val` (the state refers to it). The specialization holds no
       static data and is emitted with hidden visibility, like Variant below,
       so nothing in it is shared across packages. */
    template <typename TVariant>
    class VariantArms;

    /* The native sabot type and state of a generated variant.

       A variant value is stored as a fixed-shape record (#96):

         <{ .$which: int, .Tag0: payload0?, .Tag1: payload1?, ... }>

       `$which` sorts before every tag, so it is field 0 and arm i is field
       i + 1. Only the active arm's optional is set. This builds exactly the
       type and state that Var::SS::TVariantRec builds for the same value, but
       from the native value directly: no Var, no type interning and no heap
       allocation per value (#946).

       Hidden visibility for the same reason as Record<T> (#797): generated
       class names depend only on the variant's shape, so two packages can
       instantiate the same Variant<T>. Nothing here has static storage, but
       keeping it hidden keeps every instantiation private to its package. */
    template <typename TVariant>
    class [[gnu::visibility("hidden")]] Variant {
      NO_CONSTRUCTION(Variant);
      public:

      /* The generated arms of TVariant. */
      using TArms = VariantArms<TVariant>;

      /* Construct a new type sabot for TVariant. */
      static Sabot::Type::TRecord *NewType(void *type_alloc) {
        return new (type_alloc) TType();
      }

      /* Construct a new state sabot around the given value, which must outlive it. */
      static Sabot::State::TAny *NewState(const TVariant &val, void *state_alloc) {
        return new (state_alloc) TState(val);
      }

      private:

      /* The name of a field of the storage record. */
      static const char *GetFieldName(size_t elem_idx) {
        return elem_idx ? TArms::GetArmName(elem_idx - 1) : "$which";
      }

      /* The type of a field of the storage record. */
      static Sabot::Type::TAny *NewFieldType(size_t elem_idx, void *type_alloc) {
        if (elem_idx == 0) {
          return Type::For<int64_t>::GetType(type_alloc);
        }
        return TArms::NewArmOptType(elem_idx - 1, type_alloc);
      }

      /* The number of fields of the storage record. */
      static size_t GetFieldCount() {
        return TArms::GetArmCount() + 1;
      }

      /* Type sabot: the storage record. */
      class TType final
          : public Sabot::Type::TRecord {
        public:

        /* Our pin. */
        class TPin final
            : public Sabot::Type::TRecord::TPin {
          NO_COPY(TPin);
          public:

          /* Do-little. */
          TPin(const TType *type)
              : Sabot::Type::TRecord::TPin(type) {}

          /* See Sabot::Type::TRecord. */
          virtual Sabot::Type::TAny *NewElem(size_t elem_idx, std::string &field_name, void *type_alloc) const override {
            CheckIdx(elem_idx);
            field_name = GetFieldName(elem_idx);
            return NewFieldType(elem_idx, type_alloc);
          }

          /* See Sabot::Type::TRecord. */
          virtual Sabot::Type::TAny *NewElem(
              size_t elem_idx, void *&out_field_name_state, void *field_name_state_alloc, void *type_alloc) const override {
            CheckIdx(elem_idx);
            out_field_name_state = Native::State::New(GetFieldName(elem_idx), field_name_state_alloc);
            return NewFieldType(elem_idx, type_alloc);
          }

          /* See Sabot::Type::TRecord. */
          virtual Sabot::Type::TAny *NewElem(size_t elem_idx, void *type_alloc) const override {
            CheckIdx(elem_idx);
            return NewFieldType(elem_idx, type_alloc);
          }

          private:

          /* Throw if the index is out of range, like Record<T>. */
          static void CheckIdx(size_t elem_idx) {
            if (elem_idx >= GetFieldCount()) {
              throw Sabot::TIdxTooBig();
            }
          }

        };  // TType::TPin

        /* Do-little. */
        TType() {}

        /* See Sabot::Type::TNAry. */
        virtual size_t GetElemCount() const override {
          return GetFieldCount();
        }

        /* See Sabot::Type::TRecord. */
        virtual Sabot::Type::TRecord::TPin *Pin(void *pin_alloc) const override {
          return new (pin_alloc) TPin(this);
        }

      };  // TType

      /* State sabot: one arm's optional payload, set iff it is the active arm. */
      class TArm final
          : public Sabot::State::TOpt {
        public:

        /* Our pin. */
        class TPin final
            : public Sabot::State::TOpt::TPin {
          NO_COPY(TPin);
          public:

          /* Do-little. */
          TPin(const TArm *arm)
              : Sabot::State::TOpt::TPin(arm), Arm(arm) {}

          private:

          /* See Sabot::State::TOpt::TPin. */
          virtual Sabot::State::TAny *NewElemInRange(size_t, void *state_alloc) const override {
            assert(Arm->Val);
            return TArms::NewPayloadState(*Arm->Val, state_alloc);
          }

          /* The arm we pin. */
          const TArm *Arm;

        };  // TArm::TPin

        /* Val is null for an inactive arm; otherwise it must outlive us. */
        TArm(const TVariant *val, size_t arm_idx)
            : Val(val), ArmIdx(arm_idx) {}

        /* See Sabot::State::TArrayOfSingleStates. */
        virtual size_t GetElemCount() const override {
          return Val ? 1 : 0;
        }

        /* See Sabot::State::TOpt. */
        virtual Sabot::Type::TOpt *GetOptType(void *type_alloc) const override {
          return TArms::NewArmOptType(ArmIdx, type_alloc);
        }

        /* See Sabot::State::TArrayOfSingleStates. */
        virtual Sabot::State::TOpt::TPin *Pin(void *pin_alloc) const override {
          return new (pin_alloc) TPin(this);
        }

        private:

        /* The variant, if this is its active arm. */
        const TVariant *Val;

        /* Our index in tag order. */
        size_t ArmIdx;

      };  // TArm

      /* State sabot: the storage record of one value. */
      class TState final
          : public Sabot::State::TRecord {
        public:

        /* Our pin. */
        class TPin final
            : public Sabot::State::TRecord::TPin {
          NO_COPY(TPin);
          public:

          /* Do-little. */
          TPin(const TState *state)
              : Sabot::State::TRecord::TPin(state), State(state) {}

          private:

          /* See Sabot::State::TRecord::TPin. */
          virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override {
            if (elem_idx == 0) {
              return Native::State::New(State->Which, state_alloc);
            }
            const size_t arm_idx = elem_idx - 1;
            return new (state_alloc) TArm(
                (arm_idx == static_cast<size_t>(State->Which)) ? &State->Val : nullptr, arm_idx);
          }

          /* The state we pin. */
          const TState *State;

        };  // TState::TPin

        /* Val must outlive us. */
        TState(const TVariant &val)
            : Val(val), Which(static_cast<int64_t>(TArms::GetWhich(val))) {
          assert(static_cast<size_t>(Which) < TArms::GetArmCount());
        }

        /* See Sabot::State::TStaticArrayOfSingleStates. Known without building the type. */
        virtual size_t GetElemCount() const override {
          return GetFieldCount();
        }

        /* See Sabot::State::TRecord. */
        virtual Sabot::Type::TRecord *GetRecordType(void *type_alloc) const override {
          return new (type_alloc) TType();
        }

        /* See Sabot::State::TArrayOfSingleStates. */
        virtual Sabot::State::TRecord::TPin *Pin(void *pin_alloc) const override {
          return new (pin_alloc) TPin(this);
        }

        private:

        /* The value we are a sabot to. */
        const TVariant &Val;

        /* The active arm, stored here because the `$which` int state refers to it. */
        int64_t Which;

      };  // TState

      /* Sizes of our types, states and pins. */
      friend class Orly::Sabot::TSizeChecker;

    };  // Variant<TVariant>

  }  // Native

}  // Orly
