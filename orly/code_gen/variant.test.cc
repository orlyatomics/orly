/* <orly/code_gen/variant.test.cc>

   Unit test for <orly/code_gen/variant.h>.

   This exercises the *emitter* (it runs GenVariantHeader and confirms it
   produces a file without throwing). It does NOT compile the emitted C++;
   that validation is performed out-of-band in Phase 2 (see the emitted
   /tmp/VO7Deletedi7Integer.h compiled against the runtime headers). The
   emitter is otherwise dead code until the Phase-3 language surface exists.

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

#include <orly/code_gen/variant.h>

#include <orly/type.h>
#include <orly/type/type_czar.h>
#include <orly/type/variant.h>

#include <base/test/kit.h>

using namespace Orly;
using namespace Orly::CodeGen;
using namespace Orly::Type;

FIXTURE(GenVariantHeader) {
  TTypeCzar type_czar;

  /* A 2-arm flat variant { Integer(int) | Deleted } -- the #95 milestone
     shape. Deleted is tag-only: empty-object (unit) payload. */
  auto deleted_payload = TObj::Get({});
  auto variant1 = TVariant::Get({{"Integer", TInt::Get()}, {"Deleted", deleted_payload}});
  GenVariantHeader("/tmp/", variant1);

  /* A variant whose payload is itself a record, to exercise the
     payload-object include path. */
  auto rec = TObj::Get({{"x", TInt::Get()}, {"y", TBool::Get()}});
  auto variant2 = TVariant::Get({{"Point", rec}, {"None", deleted_payload}});
  GenVariantHeader("/tmp/", variant2);
}
