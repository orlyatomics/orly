/* <orly/var/variant.h>

   A Orly variant value -- one active tag plus its payload value. The
   runtime mirror of `orly/type/variant.h` and a near-clone of
   `orly/var/obj.h`.

   At the storage layer a variant value is "a record that has exactly
   one of N possible single-key shapes" (see issue #95): the value
   `Integer(-384)` holds tag `"Integer"` and payload `TVar(-384)`, and
   round-trips as the single-field record `<{.Integer: -384}>`. A
   tag-only variant (e.g. `Deleted`) holds the empty object as its
   payload (the unit value), matching `orly/type/variant.h`.

   `GetType()` reports the *full declared* variant type (every arm),
   not just the active one -- `Integer(7)` and `Deleted` of the same
   declared `{ Integer(int) | Deleted }` therefore report the SAME type.
   That is required for a set of differently-tagged variants to be
   homogeneous at the Var layer (see orly/var/set.cc). The value carries
   the declared type because a single arm cannot know the others; it is
   supplied at construction (the ctor's declared type) and re-supplied on
   read-back via the call-site `::(T)` annotation (issue #95).

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

#include <string>

#include <base/class_traits.h>
#include <orly/rt/runtime_error.h>
#include <orly/type/impl.h>
#include <orly/type/variant.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TVariant
        : public TVar::TImpl {
      public:

      /* TODO */
      virtual Var::TVar &Index(const TVar &);

      /* TODO */
      virtual TVariant &Add(const TVar &);

      /* TODO */
      virtual TVariant &And(const TVar &);

      /* TODO */
      virtual TVariant &Div(const TVar &);

      /* TODO */
      virtual TVariant &Exp(const TVar &);

      /* TODO */
      virtual TVariant &Intersection(const TVar &);

      /* TODO */
      virtual TVariant &Mod(const TVar &);

      /* TODO */
      virtual TVariant &Mult(const TVar &);

      /* TODO */
      virtual TVariant &Or(const TVar &);

      /* TODO */
      virtual TVariant &Sub(const TVar &);

      /* TODO */
      virtual TVariant &SymmetricDiff(const TVar &);

      /* TODO */
      virtual TVariant &Union(const TVar &);

      /* TODO */
      virtual TVariant &Xor(const TVar &);

      /* The active tag. */
      const std::string &GetTag() const {
        return Tag;
      }

      /* The payload value carried by the active tag. */
      const TVar &GetVal() const {
        return Payload;
      }

      /* TODO */
      virtual size_t GetHash() const;

      /* The full declared variant type (every arm), so all values of one
         declared variant report the same type. */
      virtual Type::TType GetType() const;

      /* TODO */
      virtual void Touch();

      /* TODO */
      virtual void Write(std::ostream &) const;

      private:

      /* TODO */
      TVariant(const Type::TType &variant_type, const std::string &tag, const TVar &payload);

      /* TODO */
      virtual ~TVariant();

      /* TODO */
      virtual void Accept(const TVisitor &visitor) const;

      /* TODO */
      virtual TVar Copy() const;

      /* TODO */
      void SetHash();

      /* The full declared variant type (all arms). */
      Type::TType VariantType;

      /* The active tag name. */
      std::string Tag;

      /* The payload value carried by the active tag. */
      TVar Payload;

      /* TODO */
      size_t Hash;

      /* TODO */
      friend class TVar;

    };  // TVariant

  }  // Var

}  // Orly
