/* <orly/var/addr.cc>

   Implements <orly/var/addr.h>.

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

#include <orly/var/addr.h>

#include <base/split.h>
#include <orly/type/addr.h>
#include <orly/var/int.h>

using namespace Orly;
using namespace Var;

size_t TAddr::GetHash() const {
  return Hash;
}

Type::TType TAddr::GetType() const {
  /* Derived from the element vars on demand -- the old parallel TypeVec member was pure
     redundancy (#384). */
  std::vector<std::pair<TDir, Type::TType>> type_vec;
  type_vec.reserve(Val.size());
  for (const auto &elem : Val) {
    type_vec.push_back(std::make_pair(elem.first, elem.second.GetType()));
  }
  return Type::TAddr::Get(type_vec);
}

void TAddr::Write(std::ostream &strm) const {
  strm
    << GetType() << '('
    << Base::Join(Val,
                  ", ",
                  [](std::ostream &strm, TAddrType::const_reference elem) {
                    strm << elem.second;
                  })
    << ')';
}

void TAddr::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TAddr::SetHash() {
  Hash = 0;
  size_t rotate = 0;
  for (auto iter = Val.begin(); iter != Val.end(); ++iter, rotate += 5) {
    Hash ^= Util::RotatedLeft(iter->second.GetHash(), rotate + ToInt(iter->first));
  }
}

void TAddr::Touch() {
  SetHash();
}

Var::TVar &TAddr::Index(const TVar &key) {
  int64_t idx = TVar::TDt<int64_t>::As(key);
  if (idx < 0 || idx >= static_cast<int64_t>(Val.size())) {
    throw Rt::TSystemError(HERE, "Dynamic addr index out of bounds.");
  }
  return Val[idx].second;
}

TAddr &TAddr::Add(const TVar &) {
  throw Rt::TSystemError(HERE, "Add not supported on Addr.");
}

TAddr &TAddr::And(const TVar &) {
  throw Rt::TSystemError(HERE, "And not supported on Addr.");
}

TAddr &TAddr::Div(const TVar &) {
  throw Rt::TSystemError(HERE, "Div not supported on Addr.");
}

TAddr &TAddr::Exp(const TVar &) {
  throw Rt::TSystemError(HERE, "Exp not supported on Addr.");
}

TAddr &TAddr::Intersection(const TVar &) {
  throw Rt::TSystemError(HERE, "Intersection not supported on Addr.");
}

TAddr &TAddr::Mod(const TVar &) {
  throw Rt::TSystemError(HERE, "Mod not supported on Addr.");
}

TAddr &TAddr::Mult(const TVar &) {
  throw Rt::TSystemError(HERE, "Mult not supported on Addr.");
}

TAddr &TAddr::Or(const TVar &) {
  throw Rt::TSystemError(HERE, "Or not supported on Addr.");
}

TAddr &TAddr::Sub(const TVar &) {
  throw Rt::TSystemError(HERE, "Sub not supported on Addr.");
}

TAddr &TAddr::SymmetricDiff(const TVar &) {
  throw Rt::TSystemError(HERE, "SymmetricDiff not supported on Addr.");
}

TAddr &TAddr::Union(const TVar &) {
  throw Rt::TSystemError(HERE, "Union not supported on Addr.");
}

TAddr &TAddr::Xor(const TVar &) {
  throw Rt::TSystemError(HERE, "Xor not supported on Addr.");
}

TAddr::TAddr(const TAddrType &that)
    : Val(that) {
  SetHash();
}

TAddr::~TAddr() {}

TVar TAddr::Copy() const {
  TAddrType copy_vec;
  for (auto &iter : Val) {
    copy_vec.push_back(std::make_pair(iter.first, iter.second.Copy()));
  }
  return (new TAddr(copy_vec))->AsVar();
}

TVar TVar::Addr(const TAddr::TAddrType &that) {
  return (new TAddr(that))->AsVar();
}
