/* <orly/rt/opt.h>

   The 'opt' runtime value.

   Copyright 2010-2014 OrlyAtomics, Inc.

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

#include <base/opt.h>  // For conversion from Base::TOpt to Rt::TOpt
#include <orly/rt/runtime_error.h>

namespace Orly {

  namespace Rt {

    DEFINE_ERROR(TDerefUnknownError, TRuntimeError, "Attempted to perform 'known' operation on an unknown optional value.");

    template <typename _TVal>
    class TOpt {
      public:

      /* Typedef the template parameter into scope. */
      typedef _TVal TVal;

      /* Default-construct as an unknown. */
      TOpt()
          : Val(nullptr) {}

      /* Move contructor.  We get the donor's value (if any) and the donor becomes unknown. */
      TOpt(TOpt &&that) {
        assert(&that);
        if (that.Val) {
          Val = new (Storage) TVal(std::move(*that.Val));
          that.Reset();
        } else {
          Val = nullptr;
        }
      }

      /* Copy constructor. */
      TOpt(const TOpt &that) {
        assert(&that);
        Val = that.Val ? new (Storage) TVal(*that.Val) : nullptr;
      }

      /* Construct by moving the given value into our internal storage.
         What remains of the donor is up to TVal. */
      TOpt(TVal &&that) {
        assert(&that);
        Val = new (Storage) TVal(std::move(that));
      }

      /* Construct with a copy of the given value. */
      TOpt(const TVal &that) {
        assert(&that);
        Val = new (Storage) TVal(that);
      }

      /* Move-construct from a Base::TOpt */
      TOpt(Base::TOpt<TVal> &&that) {
        assert(&that);
        if (that.Val) {
          Val = new (Storage) TVal(std::move(*that));
          that.Reset();
        } else {
          Val = nullptr;
        }
      }

      /* Copy-construct from a Base::TOpt */
      TOpt(const Base::TOpt<TVal> &that) {
        assert(&that);
        Val = that ? new (Storage) TVal(*that) : nullptr;
      }

      /* If we're known, destroy our value as we go. */
      ~TOpt() {
        assert(this);
        Reset();
      }

      /* Swaperator. */
      TOpt &operator=(TOpt &&that) {
        assert(this);
        assert(&that);
        if (this != &that) {
          if (Val && that.Val) {
            std::swap(*Val, *that.Val);
          } else if (that.Val) {
            Val = new (Storage) TVal(std::move(*that.Val));
            that.Reset();
          } else if (Val) {
            that.Val = new (that.Storage) TVal(std::move(*Val));
            Reset();
          }
        }
        return *this;
      }

      /* Assignment operator. */
      TOpt &operator=(const TOpt &that) {
        assert(this);
        return (this != &that) ? *this = TOpt(that) : *this;
      }

      /* If we're already known, swap our value with the given one;
         otherwise, move-construct the value into our storage. */
      TOpt &operator=(TVal &&that) {
        assert(this);
        assert(&that);
        if (Val != &that) {
          if (Val) {
            std::swap(*Val, that);
          } else {
            Val = new (Storage) TVal(std::move(that));
          }
        }
        return *this;
      }

      /* Assume a copy of the given value.  If we weren't known before, we will be now. */
      TOpt &operator=(const TVal &that) {
        assert(this);
        return (Val != &that) ? *this = TOpt(that) : *this;
      }

      /* If val is known, std::hash<TVal>() to compute the hash of Val. Otherwise return 0. */
      size_t GetHash() const {
        assert(this);
        return Val ? std::hash<TVal>()(*Val) : 0;
      }

      /* Returns the containing value. Throws if unknown. */
      const TVal &GetVal() const {
        assert(this);
        if (!Val) {
          throw TDerefUnknownError(HERE);
        }
        return *Val;
      }

      /* Returns the containing value if known. Throws if unknown. */
      TVal &GetVal() {
        assert(this);
        if (!Val) {
          throw TDerefUnknownError(HERE);
        }
        return *Val;
      }

      /* Check if known */
      bool IsKnown() const {
        assert(this);
        return Val;
      }

      /* Check if unknown */
      bool IsUnknown() const {
        assert(this);
        return !Val;
      }

      /* If we're already known, do nothing; otherwise, construct a new value using the given args.
         Return a refernce to our (possibly new) value. */
      template <typename... TArgs>
      TVal &MakeKnown(TArgs &&... args) {
        assert(this);
        if (!Val) {
          Val = new (Storage) TVal(args...);
        }
        return *Val;
      }

      /* Reset to the unknown state. */
      TOpt &Reset() {
        assert(this);
        if (Val) {
          Val->~TVal();
          Val = nullptr;
        }
        return *this;
      }

      private:

      /* The storage space used to hold our known value, if any.  We use in-place new operators
         and explicit destruction to make values come and go from this buffer. */
      alignas(TVal) char Storage[sizeof(TVal)];

      /* A pointer to our current value.  If this is null, then our value is unknown.
         If it is non-null, then it points (in a type-safe way) to Storage. */
      TVal *Val;

    };  // TOpt

  }  // Rt

}  // Orly

namespace std {

  /* A standard hasher for Rt::TOpt<TVal>. */
  template <typename TVal>
  struct hash<Orly::Rt::TOpt<TVal>> {

    typedef size_t result_type;
    typedef Orly::Rt::TOpt<TVal> argument_type;

    result_type operator()(const argument_type &that) const {
      return that.GetHash();
    }

  };  // hash<Orly::Rt::TOpt<TVal>>

}  // std
