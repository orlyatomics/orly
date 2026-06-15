/* <orly/code_gen/variant.test.cc>

   Unit test for <orly/code_gen/variant.h>.

   Exercises the *emitter* (`GenVariantHeader`) two ways:

     1. Generates a couple of variant headers and confirms it produces a
        file without throwing.

     2. Pins the emitter's output as a LIVE GOLDEN: the generated header
        for `{ Integer(int) | Deleted }` is compared, byte for byte, to
        the committed snapshot in variant_emit_test/variant.frozen.h that
        orly/code_gen/variant_emit.test.cc compiles and runs against the
        real runtime. Because the snapshot is a curated copy (not produced
        at build time), it used to drift silently from the emitter -- it
        carried a pre-#96 read-back for two releases (#123). This golden
        check fails CI the moment the emitter's output and the snapshot
        diverge.

   The snapshot is curated in exactly two deterministic ways, which this
   test reproduces before comparing: its leading auto-generated comment is
   replaced (so we compare from `#pragma once` on), and the generated
   `<orly/rt/objects/O0.h>` payload include is redirected to the
   co-located O0.frozen.h. To refresh the snapshot after an intentional
   emitter change, run this test with REGEN_VARIANT_GOLDEN=1 in the
   environment; it rewrites variant.frozen.h in place (preserving the
   snapshot's leading comment) and passes.

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

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <base/source_root.h>
#include <orly/type.h>
#include <orly/type/type_czar.h>
#include <orly/type/variant.h>

#include <base/test/kit.h>

using namespace std;
using namespace Orly;
using namespace Orly::CodeGen;
using namespace Orly::Type;

/* The committed snapshot the emit test compiles, relative to the source root. */
static const char *const GoldenRelPath =
    "orly/code_gen/variant_emit_test/variant.frozen.h";

/* Read an entire file into a string (empty if it does not exist). */
static string Slurp(const string &path) {
  ifstream in(path, ios::binary);
  ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/* The portion of a generated header from the first `#pragma once` on --
   i.e. everything but the leading comment, which the snapshot replaces. */
static string BodyFromPragma(const string &s) {
  auto pos = s.find("#pragma once");
  return pos == string::npos ? s : s.substr(pos);
}

/* Apply the snapshot's one content edit: the generated payload include
   points into the build-artifact objects dir; the snapshot redirects it
   to the co-located frozen copy so the emit test is self-contained. */
static string RedirectO0Include(string body) {
  const string from = "#include <orly/rt/objects/O0.h>";
  const string to   = "#include <orly/code_gen/variant_emit_test/O0.frozen.h>";
  auto pos = body.find(from);
  if (pos != string::npos) {
    body.replace(pos, from.size(), to);
  }
  return body;
}

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

/* The live-golden check (#123): the emitter's output for variant1 must
   match the committed snapshot the emit test compiles. */
FIXTURE(VariantGoldenMatchesEmitter) {
  TTypeCzar type_czar;

  auto deleted_payload = TObj::Get({});
  auto variant1 = TVariant::Get({{"Integer", TInt::Get()}, {"Deleted", deleted_payload}});
  GenVariantHeader("/tmp/", variant1);

  const string live = RedirectO0Include(BodyFromPragma(Slurp("/tmp/V2O07Deletedi7Integer.h")));
  const string golden_path = Base::GetSrcRoot() + GoldenRelPath;
  const string golden_full = Slurp(golden_path);
  const string golden = BodyFromPragma(golden_full);

  if (getenv("REGEN_VARIANT_GOLDEN")) {
    /* Preserve the snapshot's leading comment; replace the body. */
    const string comment = golden_full.substr(0, golden_full.find("#pragma once"));
    ofstream out(golden_path, ios::binary | ios::trunc);
    out << comment << live;
    cout << "REGEN_VARIANT_GOLDEN: rewrote " << golden_path << endl;
    return;
  }

  if (live != golden) {
    /* Write a refreshed candidate for inspection and point at the regen path. */
    const string comment = golden_full.substr(0, golden_full.find("#pragma once"));
    ofstream out("/tmp/variant.frozen.h.new", ios::binary | ios::trunc);
    out << comment << live;
    cerr << "\nGenVariantHeader output has drifted from " << GoldenRelPath << ".\n"
         << "A refreshed candidate was written to /tmp/variant.frozen.h.new.\n"
         << "If the change is intentional, regenerate with:\n"
         << "  REGEN_VARIANT_GOLDEN=1 <this test binary>\n"
         << "and rebuild so orly/code_gen/variant_emit.test.cc compiles the new snapshot.\n"
         << endl;
  }
  EXPECT_TRUE(live == golden);
}
