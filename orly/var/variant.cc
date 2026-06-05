/* <orly/var/variant.cc>

   Implements <orly/var/variant.h>.

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

#include <orly/var/variant.h>

#include <base/not_implemented.h>
#include <base/util/stl.h>
#include <orly/type/variant.h>

using namespace std;
using namespace Orly;
using namespace Orly::Var;
using namespace Util;

size_t TVariant::GetHash() const {
  return Hash;
}

Type::TType TVariant::GetType() const {
  /* The full declared variant type (all arms), carried since construction,
     so every value of one declared variant reports the same type -- required
     for set-of-variants homogeneity (orly/var/set.cc). */
  return VariantType;
}

void TVariant::Write(std::ostream &) const {
  NOT_IMPLEMENTED();
}

void TVariant::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TVariant::SetHash() {
  Hash = std::hash<std::string>()(Tag);
  Hash ^= RotatedLeft(Payload.GetHash(), 5);
}

void TVariant::Touch() {
  SetHash();
}

Var::TVar &TVariant::Index(const TVar &) {
  throw Rt::TSystemError(HERE, "Index not supported on Variant.");
}

TVariant &TVariant::Add(const TVar &) {
  throw Rt::TSystemError(HERE, "Add not supported on Variant.");
}

TVariant &TVariant::And(const TVar &) {
  throw Rt::TSystemError(HERE, "And not supported on Variant.");
}

TVariant &TVariant::Div(const TVar &) {
  throw Rt::TSystemError(HERE, "Div not supported on Variant.");
}

TVariant &TVariant::Exp(const TVar &) {
  throw Rt::TSystemError(HERE, "Exp not supported on Variant.");
}

TVariant &TVariant::Intersection(const TVar &) {
  throw Rt::TSystemError(HERE, "Intersection not supported on Variant.");
}

TVariant &TVariant::Mod(const TVar &) {
  throw Rt::TSystemError(HERE, "Mod not supported on Variant.");
}

TVariant &TVariant::Mult(const TVar &) {
  throw Rt::TSystemError(HERE, "Mul not supported on Variant.");
}

TVariant &TVariant::Or(const TVar &) {
  throw Rt::TSystemError(HERE, "Or not supported on Variant.");
}

TVariant &TVariant::Sub(const TVar &) {
  throw Rt::TSystemError(HERE, "Sub not supported on Variant.");
}

TVariant &TVariant::SymmetricDiff(const TVar &) {
  throw Rt::TSystemError(HERE, "SymmetricDiff not supported on Variant.");
}

TVariant &TVariant::Union(const TVar &) {
  throw Rt::TSystemError(HERE, "Union not supported on Variant.");
}

TVariant &TVariant::Xor(const TVar &) {
  throw Rt::TSystemError(HERE, "Xor not supported on Variant.");
}

TVariant::TVariant(const Type::TType &variant_type, const std::string &tag, const TVar &payload)
    : VariantType(variant_type), Tag(tag), Payload(payload) {
  SetHash();
}

TVariant::~TVariant() {}

TVar TVariant::Copy() const {
  return (new TVariant(VariantType, Tag, Payload.Copy()))->AsVar();
}

TVar TVar::Variant(const Type::TType &variant_type, const std::string &tag, const TVar &payload) {
  return (new TVariant(variant_type, tag, payload))->AsVar();
}
