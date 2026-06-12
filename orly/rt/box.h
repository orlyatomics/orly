/* <orly/rt/box.h>

   The heap indirection for a recursive variant arm (issue #103): a value
   cannot contain itself by value, so the generated variant struct stores a
   self-referential payload (or self-referential payload-record field) as
   `TBox<TVal>` -- a shared_ptr with value semantics.

   A default-constructed box is null. That state exists only in the
   payload storage of a variant's *inactive* arms (which the generated
   struct never reads or compares -- every operation switches on the
   active arm first), but the comparison and hash operations handle null
   defensively anyway: null boxes compare equal to each other, order
   before non-null, and hash to zero.

   `TVal` may be incomplete where a TBox member is *declared* (that's the
   point: the boxed-payload struct is defined before the variant struct
   that contains it); the member function bodies require it complete,
   which holds at every instantiation site because they're instantiated
   lazily from package code where all generated types are defined.

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
#include <functional>
#include <memory>

#include <orly/rt/operator.h>

namespace Orly {

  namespace Rt {

    /* Heap indirection with value semantics for recursive variant arms. */
    template <typename TVal>
    class TBox {
      public:

      /* Null; only a variant's inactive arm storage stays in this state. */
      TBox() {}

      /* Box a value. */
      TBox(const TVal &val) : Ptr(std::make_shared<const TVal>(val)) {}

      /* The boxed value; the box must not be null. */
      const TVal &Get() const {
        assert(Ptr);
        return *Ptr;
      }

      bool EqEq(const TBox &that) const {
        if (!Ptr || !that.Ptr) {
          return !Ptr && !that.Ptr;
        }
        return Rt::EqEq(*Ptr, *that.Ptr);
      }

      bool Match(const TBox &that) const {
        if (!Ptr || !that.Ptr) {
          return !Ptr && !that.Ptr;
        }
        return Rt::Match(*Ptr, *that.Ptr);
      }

      bool MatchLess(const TBox &that) const {
        if (!Ptr || !that.Ptr) {
          return !Ptr && that.Ptr;
        }
        return Rt::MatchLess(*Ptr, *that.Ptr);
      }

      size_t GetHash() const {
        return Ptr ? std::hash<TVal>()(*Ptr) : 0;
      }

      private:

      std::shared_ptr<const TVal> Ptr;

    };  // TBox<TVal>

    template <typename TVal>
    struct EqEqStruct<TBox<TVal>, TBox<TVal>> {
      NO_CONSTRUCTION(EqEqStruct);
      static bool Do(const TBox<TVal> &lhs, const TBox<TVal> &rhs) {
        return lhs.EqEq(rhs);
      }
    };  // EqEqStruct

    template <typename TVal>
    struct NeqStruct<TBox<TVal>, TBox<TVal>> {
      NO_CONSTRUCTION(NeqStruct);
      static bool Do(const TBox<TVal> &lhs, const TBox<TVal> &rhs) {
        return !lhs.EqEq(rhs);
      }
    };  // NeqStruct

    template <typename TVal>
    bool Match(const TBox<TVal> &lhs, const TBox<TVal> &rhs) {
      return lhs.Match(rhs);
    }

    template <typename TVal>
    bool MatchLess(const TBox<TVal> &lhs, const TBox<TVal> &rhs) {
      return lhs.MatchLess(rhs);
    }

  }  // Rt

}  // Orly

namespace std {

  /* A standard hasher for Orly::Rt::TBox<TVal>. */
  template <typename TVal>
  struct hash<Orly::Rt::TBox<TVal>> {

    typedef size_t result_type;
    typedef Orly::Rt::TBox<TVal> argument_type;

    size_t operator()(const argument_type &that) const {
      return that.GetHash();
    }

  };  // hash<TBox<TVal>>

}  // std
