/* <orly/code_gen/variant_emit.test.cc>

   Validates the EMITTED variant code, not the hand-written Var::TVariant.

   The variant code generator (orly/code_gen/variant.cc) is dead code until
   the Phase-3 orlyscript surface exists, so a clean orlyc build does NOT
   prove the *generated* C++ is correct (this is exactly the trap that
   produced the #90 empty-object bug). This test compiles, links and runs a
   frozen verbatim snapshot of GenVariantHeader's output for the variant
   { Integer(int) | Deleted } (variant.frozen.h, regenerable from the live
   emitter which orly/code_gen/variant.test.cc also exercises) against the
   real orly runtime, and checks AsVar()/GetType()/EqEq/Neq/Match/MatchLess.

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

/* Concrete type headers first: the generated variant header's
   TDt<...>::GetType() names TInt::Get()/TObj::Get() etc. (just as a real
   generated O*.h does), which the including translation unit must complete
   -- in the package case that is orly/type.h. */
#include <orly/type.h>

#include <orly/code_gen/variant_emit_test/variant.frozen.h>

#include <orly/type/type_czar.h>
#include <orly/type/variant.h>
#include <orly/var/variant.h>

#include <base/test/kit.h>

using namespace Orly;
using TV = Orly::Rt::Variants::TVariantV2O07Deletedi7Integer;

FIXTURE(EmittedVariantRoundTrips) {
  Type::TTypeCzar type_czar;

  TV i  = TV::MkInteger(int64_t(-384));
  TV i2 = TV::MkInteger(int64_t(-384));
  TV i3 = TV::MkInteger(int64_t(7));
  TV d  = TV::MkDeleted(Rt::Objects::TObjO0());

  /* GetType(): the declared 2-arm variant type. */
  auto declared = Type::TVariant::Get(
      {{"Deleted", Type::TObj::Get({})}, {"Integer", Type::TInt::Get()}});
  EXPECT_TRUE(Type::TDt<TV>::GetType() == declared);

  /* EqEq / Neq. */
  EXPECT_TRUE(i.EqEq(i2));
  EXPECT_FALSE(i.EqEq(i3));
  EXPECT_FALSE(i.EqEq(d));
  EXPECT_TRUE(i.Neq(d));
  EXPECT_FALSE(i.Neq(i2));

  /* Match mirrors EqEq for variants. */
  EXPECT_TRUE(i.Match(i2));
  EXPECT_FALSE(i.Match(d));

  /* MatchLess: arm order (Deleted=0 < Integer=1), then payload. */
  EXPECT_TRUE(d.MatchLess(i));    // Deleted < Integer
  EXPECT_FALSE(i.MatchLess(d));
  EXPECT_TRUE(i.MatchLess(i3));   // -384 < 7
  EXPECT_FALSE(i3.MatchLess(i));  // 7 < -384 is false
  EXPECT_FALSE(i.MatchLess(i2));  // equal -> not less
}

FIXTURE(EmittedVariantAsVar) {
  Type::TTypeCzar type_czar;

  TV i = TV::MkInteger(int64_t(-384));
  TV d = TV::MkDeleted(Rt::Objects::TObjO0());

  Var::TVar v_i = i.AsVar();
  Var::TVar v_d = d.AsVar();

  /* The dynamic var is a real Var::TVariant (NOT the #90 empty-object trap),
     and it carries the FULL declared variant type -- so every arm of one
     declared variant reports the same type, which is what lets a set of
     differently-tagged variants be homogeneous at the Var layer (#95). */
  auto declared = Type::TVariant::Get(
      {{"Deleted", Type::TObj::Get({})}, {"Integer", Type::TInt::Get()}});
  EXPECT_TRUE(v_i.GetType() == declared);
  EXPECT_TRUE(v_d.GetType() == declared);

  /* Var-level equality is consistent with native EqEq. */
  TV i2 = TV::MkInteger(int64_t(-384));
  TV i3 = TV::MkInteger(int64_t(7));
  EXPECT_TRUE(i.AsVar() == i2.AsVar());
  EXPECT_FALSE(i.AsVar() == i3.AsVar());
  EXPECT_FALSE(i.AsVar() == d.AsVar());
}
