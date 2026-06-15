/* <orly/type/group_ref.cc>

   Implements <orly/type/group_ref.h>.

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

#include <orly/type/group_ref.h>

using namespace Orly::Type;

IMPL_INTERNED_TYPE(TGroupRef, TGroupId, size_t);

TGroupRef::~TGroupRef() {}

void TGroupRef::Write(std::ostream &out) const {
  /* Like TSelfRef, not a C++ type name -- a group reference resolves to a
     member type (orly/type/rec_group.h) before any native use, so reaching
     a generated file means a resolution was missed. Diagnostic form only. */
  out << "groupref@" << GetIndex();
}
