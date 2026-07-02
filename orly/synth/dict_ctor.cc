/* <orly/synth/dict_ctor.cc>

   Implements <orly/synth/dict_ctor.h>.

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

#include <orly/synth/dict_ctor.h>

#include <cassert>
#include <set>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/expr/dict.h>
#include <orly/expr/literal.h>
#include <orly/synth/cst_utils.h>
#include <orly/synth/get_pos_range.h>

using namespace Orly;
using namespace Orly::Synth;

TDictCtor::TDictCtor(const TExprFactory *expr_factory, const Package::Syntax::TDictCtor *dict_ctor)
    : DictCtor(Base::AssertTrue(dict_ctor)) {
  assert(expr_factory);
  try {
    ForEach<Package::Syntax::TDictMember>(DictCtor->GetDictMemberList(),
        [this, expr_factory](const Package::Syntax::TDictMember *dict_member) -> bool {
          auto result = Members.emplace(std::make_pair(expr_factory->NewExpr(dict_member->GetKey()),
                                                       expr_factory->NewExpr(dict_member->GetValue())));
          assert(result.second);
        return true;
        });
  } catch (...) {
    Cleanup();
    throw;
  }
  if (Members.empty()) {
    throw TCompileError(HERE, GetPosRange(DictCtor), "Dictionary is empty. Use empty constructor instead.");
  }
}

TDictCtor::~TDictCtor() {
  Cleanup();
}

Expr::TExpr::TPtr TDictCtor::Build() const {
  Expr::TDict::TMemberMap members;
  /* Non-constant keys can only collide at runtime, but two equal literal
     keys are a compile-time mistake -- catch them here. */
  std::set<Var::TVar> literal_keys;
  for (auto member : Members) {
    auto key = (member.first)->Build();
    if (auto literal = std::dynamic_pointer_cast<Expr::TLiteral>(key)) {
      if (!literal_keys.insert(literal->GetVal()).second) {
        throw TCompileError(HERE, GetPosRange(DictCtor),
            Base::AsStr("duplicate key ", literal->GetVal(),
                        " in dictionary constructor").c_str());
      }
    }
    auto result = members.emplace(std::make_pair(std::move(key), (member.second)->Build()));
    assert(result.second);
  }
  return Expr::TDict::New(members, GetPosRange(DictCtor));
}

void TDictCtor::Cleanup() {
  for (auto member : Members) {
    delete member.first;
    delete member.second;
  }
}

void TDictCtor::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  for (auto member : Members) {
    (member.first)->ForEachInnerScope(cb);
    (member.second)->ForEachInnerScope(cb);
  }
}

void TDictCtor::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  for (auto member : Members) {
    (member.first)->ForEachRef(cb);
    (member.second)->ForEachRef(cb);
  }
}
