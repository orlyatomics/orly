/* <orly/var/id.cc>

   Implements <orly/var/id.h>.

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

#include <orly/var/id.h>

#include <orly/type/id.h>

using namespace Orly;
using namespace Var;

size_t TId::GetHash() const {
  return Val.GetHash();
}

Type::TType TId::GetType() const {
  return Type::TId::Get();
}

void TId::Write(std::ostream &strm) const {
  strm << "Base::TUuid(\"" << Val << "\")";
}

void TId::Accept(const TVisitor &visitor) const {
  assert(&visitor);
  visitor(this);
}

void TId::Touch() {
}

Var::TVar &TId::Index(const TVar &) {
  throw Rt::TSystemError(HERE, "Index not supported on Id.");
}

TId &TId::Add(const TVar &) {
  throw Rt::TSystemError(HERE, "Add not supported on Id.");
}

TId &TId::And(const TVar &) {
  throw Rt::TSystemError(HERE, "And not supported on Id.");
}

TId &TId::Div(const TVar &) {
  throw Rt::TSystemError(HERE, "Div not supported on Id.");
}

TId &TId::Exp(const TVar &) {
  throw Rt::TSystemError(HERE, "Exp not supported on Id.");
}

TId &TId::Intersection(const TVar &) {
  throw Rt::TSystemError(HERE, "Intersection not supported on Id.");
}

TId &TId::Mod(const TVar &) {
  throw Rt::TSystemError(HERE, "Mod not supported on Id.");
}

TId &TId::Mult(const TVar &) {
  throw Rt::TSystemError(HERE, "Mult not supported on Id.");
}

TId &TId::Or(const TVar &) {
  throw Rt::TSystemError(HERE, "Or not supported on Id.");
}

TId &TId::Sub(const TVar &) {
  throw Rt::TSystemError(HERE, "Sub not supported on Id.");
}

TId &TId::SymmetricDiff(const TVar &) {
  throw Rt::TSystemError(HERE, "SymmetricDiff not supported on Id.");
}

TId &TId::Union(const TVar &) {
  throw Rt::TSystemError(HERE, "Union not supported on Id.");
}

TId &TId::Xor(const TVar &) {
  throw Rt::TSystemError(HERE, "Xor not supported on Id.");
}

TId::TId(const Base::TUuid &that) : Val(that) {}

TId::~TId() {}

TVar TId::Copy() const {
  return TId::New(Val);
}

TVar TId::New(const Base::TUuid &that) {
  return (new TId(that))->AsVar();
}

TVar::TVar(const Base::TUuid &that) {
  *this = TId::New(that);
}