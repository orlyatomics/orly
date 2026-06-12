/* <orly/type/self_ref.cc>

   Implements <orly/type/self_ref.h>.

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

#include <orly/type/self_ref.h>

using namespace Orly::Type;

IMPL_INTERNED_TYPE(TSelfRef, size_t);

TSelfRef::~TSelfRef() {}

void TSelfRef::Write(std::ostream &out) const {
  /* Unlike the other leaves this is NOT a C++ type name: a self-reference
     has no standalone native representation -- the variant code generator
     substitutes the box type (Rt::TBox<...>) before printing, so reaching
     a generated file means a substitution was missed (and `self@N` won't
     compile, loudly). In diagnostics it reads as the self-reference. */
  size_t depth = GetDepth();
  out << "self";
  if (depth) {
    out << '@' << depth;
  }
}
