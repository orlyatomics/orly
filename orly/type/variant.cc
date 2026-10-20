/* <orly/type/variant.cc>

   Implements <orly/type/variant.h>.

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

#include <orly/type/variant.h>

#include <cassert>

#include <orly/type/int.h>
#include <orly/type/obj.h>
#include <orly/type/opt.h>
#include <orly/type/unroll.h>

using namespace Orly::Type;

IMPL_INTERNED_TYPE(TVariant, TVariantElems);

TType TVariant::Get(const TVariantElems &elems) {
  return TInternedType::Get(elems);
}

TVariant::~TVariant() {}

TVariant::TLowered TVariant::NewLowered() const {
  static const std::string which_name("$which");
  TLowered lowered;
  TObjElems rec;
  rec[which_name] = TInt::Get();
  lowered.OptTypes.reserve(GetElems().size());
  lowered.WhichPos = 0;
  for (const auto &arm: GetElems()) {
    /* An arm of type `T?` is stored as `T??`, so a taken arm with an unknown
       payload differs from an arm not taken (#978). */
    TType opt_type = TOpt::GetStored(arm.second);
    rec[arm.first] = opt_type;
    lowered.OptTypes.push_back(opt_type);
    if (arm.first < which_name) {
      ++lowered.WhichPos;
    }
  }
  lowered.RecType = TObj::Get(rec);
  lowered.HasGroupRef = Orly::Type::HasGroupRef(AsType());
  assert(lowered.WhichPos <= lowered.OptTypes.size());
  return lowered;
}

void TVariant::Write(std::ostream &out) const {
  out << "Orly::Rt::Variants::TVariant" << AsType().GetMangledName();
}
