/* <orly/synth/variant_type.cc>

   Implements <orly/synth/variant_type.h>.

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

#include <orly/synth/variant_type.h>

#include <iomanip>

#include <base/as_str.h>
#include <orly/synth/context.h>
#include <orly/synth/cst_utils.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/name.h>
#include <orly/synth/new_type.h>
#include <orly/pos_range.h>
#include <orly/type/obj.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Synth;

TVariantType::TVariantType(const Package::Syntax::TVariantType *variant_type) {
  assert(variant_type);
  try {
    ForEach<Package::Syntax::TVariantTypeArm>(variant_type->GetVariantTypeArmList(),
        [this](const Package::Syntax::TVariantTypeArm *arm) -> bool {
          /* Resolve the arm's tag name and (optional) payload type. A
             tag-only arm stores a nullptr payload; ComputeSymbolicType
             then substitutes the empty object (the unit type). */
          class TArmVisitor
              : public Package::Syntax::TVariantTypeArm::TVisitor {
            NO_COPY(TArmVisitor);
            public:
            TArmVisitor(std::string &name, TPosRange &pos_range, TType *&payload)
                : Name(name), PosRange(pos_range), Payload(payload) {}
            virtual void operator()(const Package::Syntax::TTagOnlyVariantTypeArm *that) const {
              TName tag(that->GetName());
              Name = tag.GetText();
              PosRange = tag.GetPosRange();
              Payload = nullptr;
            }
            virtual void operator()(const Package::Syntax::TPayloadVariantTypeArm *that) const {
              TName tag(that->GetName());
              Name = tag.GetText();
              PosRange = tag.GetPosRange();
              Payload = NewType(that->GetType());
            }
            private:
            std::string &Name;
            TPosRange &PosRange;
            TType *&Payload;
          };  // TArmVisitor
          std::string name;
          TPosRange pos_range;
          TType *payload = nullptr;
          arm->Accept(TArmVisitor(name, pos_range, payload));
          auto result = Arms.insert(std::make_pair(name, payload));
          if (!result.second) {
            delete payload;
            GetContext().AddError(pos_range,
                                  Base::AsStr("duplicate variant tag ", std::quoted(name)));
          }
          return true;
        });
  } catch (...) {
    Cleanup();
    throw;
  }
}

TVariantType::~TVariantType() {
  Cleanup();
}

void TVariantType::Cleanup() {
  for (auto &arm : Arms) {
    delete arm.second;
  }
}

void TVariantType::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  for (auto &arm : Arms) {
    if (arm.second) {
      arm.second->ForEachRef(cb);
    }
  }
}

Type::TType TVariantType::ComputeSymbolicType() const {
  Type::TVariantElems elems;
  for (auto &arm : Arms) {
    Type::TType payload_type = arm.second
        ? arm.second->GetSymbolicType()
        : Type::TObj::Get(Type::TObj::TElems{});
    auto result = elems.insert(std::make_pair(arm.first, payload_type));
    assert(result.second);
  }
  return Type::TVariant::Get(elems);
}
