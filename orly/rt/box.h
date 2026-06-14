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
#include <vector>

#include <orly/rt/containers.h>
#include <orly/rt/operator.h>
#include <orly/rt/opt.h>

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

    /* DeepBox / DeepUnbox: map a value between its unrolled shape and its
       boxed storage shape, elementwise through the containers a recursive
       arm's payload may use (issue #116): `[self]` stores as
       `std::vector<TBox<V>>`, `self?` as `TOpt<TBox<V>>`, and so on. The
       primary templates handle the leaves -- TBoxed constructible from
       TVal covers both the box itself (TBox<V> from V, or a generated
       boxed payload struct from its unrolled record) and the identity
       copy of a self-free subvalue. The generated variant header calls
       these from member templates only, so TVal may be incomplete where
       the header is parsed. */

    template <typename TBoxed, typename TVal>
    struct TDeepBox {
      static TBoxed Do(const TVal &val) {
        return TBoxed(val);
      }
    };  // TDeepBox

    template <typename TElemB, typename TElem>
    struct TDeepBox<std::vector<TElemB>, std::vector<TElem>> {
      static std::vector<TElemB> Do(const std::vector<TElem> &val) {
        std::vector<TElemB> out;
        out.reserve(val.size());
        for (const auto &elem : val) {
          out.push_back(TDeepBox<TElemB, TElem>::Do(elem));
        }
        return out;
      }
    };  // TDeepBox<vector>

    template <typename TElemB, typename TElem>
    struct TDeepBox<TOpt<TElemB>, TOpt<TElem>> {
      static TOpt<TElemB> Do(const TOpt<TElem> &val) {
        return val.IsKnown() ? TOpt<TElemB>(TDeepBox<TElemB, TElem>::Do(val.GetVal()))
                             : TOpt<TElemB>();
      }
    };  // TDeepBox<TOpt>

    template <typename TElemB, typename TElem>
    struct TDeepBox<TSet<TElemB>, TSet<TElem>> {
      static TSet<TElemB> Do(const TSet<TElem> &val) {
        TSet<TElemB> out;
        for (const auto &elem : val) {
          out.insert(TDeepBox<TElemB, TElem>::Do(elem));
        }
        return out;
      }
    };  // TDeepBox<TSet>

    template <typename TKey, typename TValB, typename TVal>
    struct TDeepBox<TDict<TKey, TValB>, TDict<TKey, TVal>> {
      static TDict<TKey, TValB> Do(const TDict<TKey, TVal> &val) {
        TDict<TKey, TValB> out;
        for (const auto &elem : val) {
          out.insert(std::make_pair(elem.first, TDeepBox<TValB, TVal>::Do(elem.second)));
        }
        return out;
      }
    };  // TDeepBox<TDict>

    template <typename TBoxed, typename TVal>
    TBoxed DeepBox(const TVal &val) {
      return TDeepBox<TBoxed, TVal>::Do(val);
    }

    /* The inverse: identity for self-free subvalues, TBox::Get() at the
       box, elementwise through the containers. */

    template <typename TVal, typename TBoxed>
    struct TDeepUnbox {
      static TVal Do(const TBoxed &boxed) {
        return boxed;
      }
    };  // TDeepUnbox

    template <typename TVal>
    struct TDeepUnbox<TVal, TBox<TVal>> {
      static TVal Do(const TBox<TVal> &boxed) {
        return boxed.Get();
      }
    };  // TDeepUnbox<TBox>

    template <typename TElem, typename TElemB>
    struct TDeepUnbox<std::vector<TElem>, std::vector<TElemB>> {
      static std::vector<TElem> Do(const std::vector<TElemB> &boxed) {
        std::vector<TElem> out;
        out.reserve(boxed.size());
        for (const auto &elem : boxed) {
          out.push_back(TDeepUnbox<TElem, TElemB>::Do(elem));
        }
        return out;
      }
    };  // TDeepUnbox<vector>

    template <typename TElem, typename TElemB>
    struct TDeepUnbox<TOpt<TElem>, TOpt<TElemB>> {
      static TOpt<TElem> Do(const TOpt<TElemB> &boxed) {
        return boxed.IsKnown() ? TOpt<TElem>(TDeepUnbox<TElem, TElemB>::Do(boxed.GetVal()))
                               : TOpt<TElem>();
      }
    };  // TDeepUnbox<TOpt>

    template <typename TElem, typename TElemB>
    struct TDeepUnbox<TSet<TElem>, TSet<TElemB>> {
      static TSet<TElem> Do(const TSet<TElemB> &boxed) {
        TSet<TElem> out;
        for (const auto &elem : boxed) {
          out.insert(TDeepUnbox<TElem, TElemB>::Do(elem));
        }
        return out;
      }
    };  // TDeepUnbox<TSet>

    template <typename TKey, typename TVal, typename TValB>
    struct TDeepUnbox<TDict<TKey, TVal>, TDict<TKey, TValB>> {
      static TDict<TKey, TVal> Do(const TDict<TKey, TValB> &boxed) {
        TDict<TKey, TVal> out;
        for (const auto &elem : boxed) {
          out.insert(std::make_pair(elem.first, TDeepUnbox<TVal, TValB>::Do(elem.second)));
        }
        return out;
      }
    };  // TDeepUnbox<TDict>

    template <typename TVal, typename TBoxed>
    TVal DeepUnbox(const TBoxed &boxed) {
      return TDeepUnbox<TVal, TBoxed>::Do(boxed);
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
