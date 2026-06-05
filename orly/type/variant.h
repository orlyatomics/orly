/* <orly/type/variant.h>

   The variant / tagged-union type: `{ Tag(TType) | ... }` -- an
   ordered (by tag name, for stable iteration) map from tag string to
   payload type. Interned by the full `TVariantElems` map, exactly as
   `TObj` (`orly/type/obj.h`) is interned by its field map.

   At the storage layer a variant value is "a record that has exactly
   one of N possible single-key shapes" (see issue #95): `Integer(-384)`
   round-trips as the single-field record `<{.Integer: -384}>`. So the
   payload-type map mirrors a record's field-type map.

   Representation choice for a tag-only variant (e.g. `Deleted`): its
   payload type is the empty object `TObj::Get({})` (the unit type).
   This reuses the existing empty-record representation -- the same
   `EnsureEmptyObject` notion used elsewhere -- rather than inventing a
   new void type, and keeps the single-key-record storage reuse exact
   (`Deleted` stores as `<{.Deleted: <{}>}>`). Constructors of `TVariant`
   take whatever payload type the caller supplies; the empty-object
   convention is purely a documented contract for the language layer
   (Phase 3) and is not enforced here.

   This type is currently purely additive: no orlyscript surface
   produces a `TVariant` yet (that is Phase 3 of #95). It exists so the
   type-layer visitor matrices can be threaded through ahead of the
   language work.

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

      private:
      TVariant(const TVariantElems &elems) : TInternedType(elems) {}
      virtual ~TVariant();

      virtual void Write(std::ostream &) const;

      friend class TInternedType;
    };  // TVariant

  }  // Type

}  // Orly
