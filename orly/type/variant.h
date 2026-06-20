/* <orly/type/variant.h>

   The variant / tagged-union type: `{ Tag(TType) | ... }` -- an
   ordered (by tag name, for stable iteration) map from tag string to
   payload type. Interned by the full `TVariantElems` map, exactly as
   `TObj` (`orly/type/obj.h`) is interned by its field map.

   On disk a variant value serializes to a fixed-shape record (issue #96):
   `Integer(-384)` of `{ Integer(int) | Deleted }` stores as
   `<{.$which:1, .Deleted:<empty-opt>, .Integer:-384?}>` -- a `$which`
   discriminant plus one optional payload field per arm. The payload-type
   map here therefore mirrors a record's field-type map, and the variant
   type maps to that record type in orly/type/new_sabot.cc (reversed in
   orly/type/sabot_to_type.cc via the `$which` sentinel field).

   Representation choice for a tag-only variant (e.g. `Deleted`): its
   payload type is the empty object `TObj::Get({})` (the unit type).
   This reuses the existing empty-record representation -- the same
   `EnsureEmptyObject` notion used elsewhere -- rather than inventing a
   new void type. Constructors of `TVariant` take whatever payload type the
   caller supplies; the empty-object convention is purely a documented
   contract for the language layer and is not enforced here.

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

#include <map>

#include <base/class_traits.h>
#include <orly/type/managed_type.h>
#include <orly/type/has_optional.h>

namespace Orly {

  namespace Type {

    /* NOTE: This is ordered (like TObjElems) so that comparing tag maps is easy and tags come out in a
             stable, asciibetical order. Maps tag name -> payload type. A tag-only variant uses the empty
             object as its payload type (the unit type); see the file comment. */
    typedef std::map<std::string, TType> TVariantElems;

    /* The variant / tagged-union type. Near-mirror of TObj. */
    class TVariant : public TInternedType<TVariant, TVariantElems> {
      NO_COPY(TVariant);
      public:

      typedef TVariantElems TElems;

      /* The tag -> payload-type map. */
      const TVariantElems &GetElems() const {
        return std::get<0>(GetKey());
      }

      /* TODO */
      static TType Get(const TVariantElems &elems) {
        return TInternedType::Get(elems);
      }

      /* The widening (subtype) relation `this ⊑ wide` (issue #104): true
         iff every `(tag → payload)` arm of `this` appears identically (same
         tag, same payload type) in `wide`. The tag set widens; payloads stay
         invariant. A direct mirror of `TObj::IsSubsetOf` (orly/type/obj.h) --
         a narrow variant's arms must be a subset of the wide one's. Reflexive
         (a type is widenable to itself). Used by the explicit `as` cast
         (orly/expr/as.cc) to permit a narrow value to flow into a wider
         variant context. */
      bool IsWidenableTo(const TVariant *wide) const {
        assert(this);
        assert(wide);
        for (const auto &arm : GetElems()) {
          auto pos = wide->GetElems().find(arm.first);
          if (pos == wide->GetElems().end() || arm.second != pos->second) {
            return false;
          }
        }
        return true;
      }

      private:
      TVariant(const TVariantElems &elems) : TInternedType(elems) {}
      virtual ~TVariant();

      virtual void Write(std::ostream &) const;

      friend class TInternedType;
    };  // TVariant

  }  // Type

}  // Orly
