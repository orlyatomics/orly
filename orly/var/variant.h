/* <orly/var/variant.h>

   A Orly variant value -- one active tag plus its payload value. The
   runtime mirror of `orly/type/variant.h` and a near-clone of
   `orly/var/obj.h`.

   In memory a variant value holds one active tag plus its payload:
   `Integer(-384)` holds tag `"Integer"` and payload `TVar(-384)`. A
   tag-only variant (e.g. `Deleted`) holds the empty object as its
   payload (the unit value), matching `orly/type/variant.h`.

   On disk it serializes to a fixed-shape record (issue #96, see
   orly/var/new_sabot.h): `<{.$which:int, .Tag:payload?, ...}>` -- a
   `$which` discriminant plus one optional payload field per arm, only the
   active arm's optional set. Every value of one declared variant shares
   that record type, which is what makes a stored set of differently-tagged
   variants homogeneous.

   `GetType()` reports the *full declared* variant type (every arm),
   not just the active one -- `Integer(7)` and `Deleted` of the same
   declared `{ Integer(int) | Deleted }` therefore report the SAME type.
   That is required for a set of differently-tagged variants to be
   homogeneous at the Var layer (see orly/var/set.cc). The value carries
   the declared type because a single arm cannot know the others; it is
   supplied at construction and, on read-back, reconstructed from the
   stored record's `$which`/arm fields (orly/var/sabot_to_var.cc) -- the
   `$which` sentinel removes the need for the call-site `::(T)` crutch.

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

      virtual Var::TVar &Index(const TVar &);

      virtual TVariant &Add(const TVar &);

      virtual TVariant &And(const TVar &);

      virtual TVariant &Div(const TVar &);

      virtual TVariant &Exp(const TVar &);

      virtual TVariant &Intersection(const TVar &);

      virtual TVariant &Mod(const TVar &);

      virtual TVariant &Mult(const TVar &);

      virtual TVariant &Or(const TVar &);

      virtual TVariant &Sub(const TVar &);

      virtual TVariant &SymmetricDiff(const TVar &);

      virtual TVariant &Union(const TVar &);

      virtual TVariant &Xor(const TVar &);

      /* The active tag. */
      const std::string &GetTag() const {
        return Tag;
      }

      /* The payload value carried by the active tag. */
      const TVar &GetVal() const {
        return Payload;
      }

      virtual size_t GetHash() const;

      /* The full declared variant type (every arm), so all values of one
         declared variant report the same type. */
      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &) const;

      private:

      TVariant(const Type::TType &variant_type, const std::string &tag, const TVar &payload);

      virtual ~TVariant();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      /* The full declared variant type (all arms). */
      Type::TType VariantType;

      /* The active tag name. */
      std::string Tag;

      /* The payload value carried by the active tag. */
      TVar Payload;

      size_t Hash;

      friend class TVar;

    };  // TVariant

  }  // Var

}  // Orly
