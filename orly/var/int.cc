/* <orly/var/int.cc>

   Implements <orly/var/int.h>.

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

#include <orly/var/int.h>

#include <orly/type/int.h>

using namespace Orly;
using namespace Var;

size_t TInt::GetHash() const {
  return Val;
}

Type::TType TInt::GetType() const {
  return Type::TInt::Get();
}

void TInt::Write(std::ostream &stream) const {
  stream << Val << "L";
}

void TInt::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TInt::Touch() {
}

Var::TVar &TInt::Index(const TVar &) {
  throw Rt::TSystemError(HERE, "Index not supported on Int.");
}

TInt &TInt::Add(const TVar &rhs) {
  Val += Var::TVar::TDt<int64_t>::As(rhs);
  return *this;
}

TInt &TInt::And(const TVar &) {
  throw Rt::TSystemError(HERE, "And not supported on Int.");
}

TInt &TInt::Div(const TVar &rhs) {
  Val /= Var::TVar::TDt<int64_t>::As(rhs);
  return *this;
}

TInt &TInt::Exp(const TVar &rhs) {
  Val = static_cast<int64_t>(pow(Val, Var::TVar::TDt<int64_t>::As(rhs)));
  return *this;
}

TInt &TInt::Intersection(const TVar &) {
  throw Rt::TSystemError(HERE, "Intersection not supported on Int.");
}

TInt &TInt::Max(const TVar &rhs) {
  int64_t r = Var::TVar::TDt<int64_t>::As(rhs);
  if (r > Val) {
    Val = r;
  }
  return *this;
}

TInt &TInt::Min(const TVar &rhs) {
  int64_t r = Var::TVar::TDt<int64_t>::As(rhs);
  if (r < Val) {
    Val = r;
  }
  return *this;
}

TInt &TInt::Mod(const TVar &rhs) {
  Val %= Var::TVar::TDt<int64_t>::As(rhs);
  return *this;
}

TInt &TInt::Mult(const TVar &rhs) {
  Val *= Var::TVar::TDt<int64_t>::As(rhs);
  return *this;
}

TInt &TInt::Or(const TVar &) {
  throw Rt::TSystemError(HERE, "Or not supported on Int.");
}

TInt &TInt::Sub(const TVar &rhs) {
  Val -= Var::TVar::TDt<int64_t>::As(rhs);
  return *this;
}

TInt &TInt::SymmetricDiff(const TVar &) {
  throw Rt::TSystemError(HERE, "SymmetricDiff not supported on Int.");
}

TInt &TInt::Union(const TVar &) {
  throw Rt::TSystemError(HERE, "Union not supported on Int.");
}

TInt &TInt::Xor(const TVar &) {
  throw Rt::TSystemError(HERE, "Xor not supported on Int.");
}

TInt::TInt(int64_t that) : Val(that) {}

TInt::~TInt() {}

TVar TInt::Copy() const {
  return TInt::New(Val);
}

TVar TInt::New(int64_t that) {
  return (new TInt(that))->AsVar();
}

TVar::TVar(const int64_t &that) {
  *this = TInt::New(that);
}

TVar::TVar(const int &that) {
  *this = TInt::New(that);
}