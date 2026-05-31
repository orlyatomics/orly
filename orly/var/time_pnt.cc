/* <orly/var/time_pnt.cc>

   Implements <orly/var/time_pnt.h>.

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

#include <orly/var/time_pnt.h>

#include <orly/type/time_pnt.h>

using namespace Orly;
using namespace Orly::Var;

size_t TTimePnt::GetHash() const {
  return std::hash<Base::Chrono::TTimePnt>()(Val);
}

Type::TType TTimePnt::GetType() const {
  return Type::TTimePnt::Get();
}

void TTimePnt::Write(std::ostream &strm) const {
  strm
    << "Base::Chrono::TTimePnt(Base::Chrono::TTimeDiff("
    << Base::Chrono::TimeDiffCast(Val.time_since_epoch()).count() << "))";
}

void TTimePnt::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TTimePnt::Touch() {
}

Var::TVar &TTimePnt::Index(const TVar &) {
  throw Rt::TSystemError(HERE, "Index not supported on time_pnt.");
}

TTimePnt &TTimePnt::Add(const TVar &) {
  throw Rt::TSystemError(HERE, "Add not supported on time_pnt.");
}

TTimePnt &TTimePnt::And(const TVar &) {
  throw Rt::TSystemError(HERE, "And not supported on time_pnt.");
}

TTimePnt &TTimePnt::Div(const TVar &) {
  throw Rt::TSystemError(HERE, "Div not supported on time_pnt.");
}

TTimePnt &TTimePnt::Exp(const TVar &) {
  throw Rt::TSystemError(HERE, "Exp not supported on time_pnt.");
}

TTimePnt &TTimePnt::Intersection(const TVar &) {
  throw Rt::TSystemError(HERE, "Intersection not supported on time_pnt.");
}

TTimePnt &TTimePnt::Mod(const TVar &) {
  throw Rt::TSystemError(HERE, "Mod not supported on time_pnt.");
}

TTimePnt &TTimePnt::Mult(const TVar &) {
  throw Rt::TSystemError(HERE, "Mult not supported on time_pnt.");
}

TTimePnt &TTimePnt::Or(const TVar &) {
  throw Rt::TSystemError(HERE, "Or not supported on time_pnt.");
}

TTimePnt &TTimePnt::Sub(const TVar &) {
  throw Rt::TSystemError(HERE, "Sub not supported on time_pnt.");
}

TTimePnt &TTimePnt::SymmetricDiff(const TVar &) {
  throw Rt::TSystemError(HERE, "SymmetricDiff not supported on time_pnt.");
}

TTimePnt &TTimePnt::Union(const TVar &) {
  throw Rt::TSystemError(HERE, "Union not supported on time_pnt.");
}

TTimePnt &TTimePnt::Xor(const TVar &) {
  throw Rt::TSystemError(HERE, "Xor not supported on time_pnt.");
}

TTimePnt::TTimePnt(const Base::Chrono::TTimePnt &that) : Val(that) {}

TTimePnt::~TTimePnt() {}

TVar TTimePnt::Copy() const {
  return TTimePnt::New(Val);
}

TVar TTimePnt::New(const Base::Chrono::TTimePnt &that) {
  return (new TTimePnt(that))->AsVar();
}

TVar::TVar(const Base::Chrono::TTimePnt &that) {
  *this = TTimePnt::New(that);
}