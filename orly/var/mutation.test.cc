/* <orly/var/mutation.test.cc>

   Unit test for <orly/var/mutation.h>

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

#include <orly/var/mutation.h>

#include <orly/type/type_czar.h>
#include <orly/var.h>

#include <base/test/kit.h>

using namespace Orly;
using namespace Orly::Var;

Type::TTypeCzar TypeCzar;

FIXTURE(Typical) {
  TVar val(std::vector<int64_t>{1, 2, 3});
  TListChange::New(0, TMutation::New(TMutator::Assign, TVar(32l)))->Apply(val);

  EXPECT_EQ(val.As<TList>()->GetVal().at(0), TVar(32l));
  EXPECT_EQ(val.As<TList>()->GetVal().at(1), TVar(2l));
  EXPECT_EQ(val.As<TList>()->GetVal().at(2), TVar(3l));
}

/* TMutation::Augment combines two same-mutator commutative+associative
   ops into one. Until #48 Augment unconditionally threw -- the data-model
   side of the "field calls compose under contention" story the README has
   always claimed. The consumers are now wired (the rest of the #49 arc):
   the session deferred-commutative path emits the mutations, the read path
   and TFoldDataFile fold same-mutator runs, and concurrent cross-POV `+=`
   composes end-to-end (validated by examples/wikipedia-pageviews/). See
   docs/architecture.md sec. 5. */

FIXTURE(Augment_AddAdd) {
  /* (x += 5) augmented with (x += 3) is equivalent to (x += 8). */
  auto m = TMutation::New(TMutator::Add, TVar(int64_t(5)));
  m->Augment(TMutation::New(TMutator::Add, TVar(int64_t(3))));

  TVar val(int64_t(100));
  m->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(108)));
}

FIXTURE(Augment_AddAdd_ChainedThree) {
  /* Three Adds compose into one: (5 + 3 + 7) = (15). */
  auto m = TMutation::New(TMutator::Add, TVar(int64_t(5)));
  m->Augment(TMutation::New(TMutator::Add, TVar(int64_t(3))));
  m->Augment(TMutation::New(TMutator::Add, TVar(int64_t(7))));

  TVar val(int64_t(0));
  m->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(15)));
}

FIXTURE(Augment_MultMult) {
  /* (x *= 2) augmented with (x *= 3) is equivalent to (x *= 6). */
  auto m = TMutation::New(TMutator::Mult, TVar(int64_t(2)));
  m->Augment(TMutation::New(TMutator::Mult, TVar(int64_t(3))));

  TVar val(int64_t(10));
  m->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(60)));
}

/* Or/And/Xor in orly's type system apply to TBool (logical), not TInt
   (which doesn't expose bitwise). The mutation-augment logic doesn't
   care -- it just delegates to Rt::Mutate which dispatches to whichever
   TVar type was passed -- but the tests need a type that actually
   supports each op. */

FIXTURE(Augment_OrOr_Bool) {
  /* (x ||= false) then (x ||= true)  ≡  (x ||= true). */
  auto m = TMutation::New(TMutator::Or, TVar(false));
  m->Augment(TMutation::New(TMutator::Or, TVar(true)));

  TVar val(false);
  m->Apply(val);
  EXPECT_EQ(val, TVar(true));
}

FIXTURE(Augment_AndAnd_Bool) {
  /* (x &&= true) then (x &&= false)  ≡  (x &&= false). */
  auto m = TMutation::New(TMutator::And, TVar(true));
  m->Augment(TMutation::New(TMutator::And, TVar(false)));

  TVar val(true);
  m->Apply(val);
  EXPECT_EQ(val, TVar(false));
}

FIXTURE(Augment_XorXor_Bool) {
  /* (x ^= true) then (x ^= true) cancels back to identity. */
  auto m = TMutation::New(TMutator::Xor, TVar(true));
  m->Augment(TMutation::New(TMutator::Xor, TVar(true)));

  TVar val(false);
  m->Apply(val);
  EXPECT_EQ(val, TVar(false));
}

FIXTURE(Augment_MixedMutators_Throws) {
  /* Mixed mutators (Add then Mult) on the same key can't be safely
     collapsed without knowing the target's current value, so Augment
     refuses rather than picking a possibly-wrong combine. */
  auto m = TMutation::New(TMutator::Add, TVar(int64_t(5)));
  auto thrower = [&m] {
    m->Augment(TMutation::New(TMutator::Mult, TVar(int64_t(3))));
  };
  EXPECT_THROW_FUNC(std::runtime_error, thrower);
}

FIXTURE(Augment_Assign_Throws) {
  /* Assign is IsFinal(); per the existing semantics a final mutation
     shouldn't be augmented at all. */
  auto m = TMutation::New(TMutator::Assign, TVar(int64_t(42)));
  auto thrower = [&m] {
    m->Augment(TMutation::New(TMutator::Assign, TVar(int64_t(99))));
  };
  EXPECT_THROW_FUNC(std::runtime_error, thrower);
}

FIXTURE(Augment_Sub_Throws_For_Now) {
  /* Sub composes with itself by *adding* the rhs values (x - a - b = x
     - (a + b)) but that combine-op differs from the mutator. Punted to
     a follow-up; for now Augment refuses. */
  auto m = TMutation::New(TMutator::Sub, TVar(int64_t(5)));
  auto thrower = [&m] {
    m->Augment(TMutation::New(TMutator::Sub, TVar(int64_t(3))));
  };
  EXPECT_THROW_FUNC(std::runtime_error, thrower);
}

FIXTURE(Augment_PartialChange_Throws) {
  /* Augmenting a TMutation with a partial change (e.g. TListChange)
     is structurally meaningless and throws. */
  auto m = TMutation::New(TMutator::Add, TVar(int64_t(5)));
  auto partial = TListChange::New(0, TMutation::New(TMutator::Assign, TVar(int64_t(99))));
  auto thrower = [&m, &partial] { m->Augment(partial); };
  EXPECT_THROW_FUNC(std::runtime_error, thrower);
}

/* Commutative-upsert (#151): applying an identity-default commutative
   mutation onto an EMPTY base (a key that was never created) must seed
   from the monoid identity and apply, instead of tripping the
   `assert(*this)` in TVar::Add / Or / Xor / Union (var/impl.h). Because
   `identity OP rhs == rhs` for each gated op, the seeded result equals
   the RHS, pinned to the RHS's type. This is the exact var-layer fold
   that the Spa test engine's TContext::operator[] (flux_capacitor.h) and
   the indy disk/memory fold both drive when reading back a bare first
   `*<[k]>::(T) OP= v` write -- it used to crash orlyc with
   "Assertion `*this' failed". */

FIXTURE(UpsertEmptyBase_Add) {
  /* 0 (identity) + 5 = 5 on a never-created int key. */
  TVar val;  // empty / default TVar -- the absent-key base.
  EXPECT_FALSE(static_cast<bool>(val));
  TMutation::New(TMutator::Add, TVar(int64_t(5)))->Apply(val);
  EXPECT_TRUE(static_cast<bool>(val));
  EXPECT_EQ(val, TVar(int64_t(5)));
}

FIXTURE(UpsertEmptyBase_Add_ThenAdd) {
  /* The two-step sequence the lang test exercises: empty +=5 then +=3.
     First Apply seeds from identity (->5), second folds normally (->8). */
  TVar val;
  TMutation::New(TMutator::Add, TVar(int64_t(5)))->Apply(val);
  TMutation::New(TMutator::Add, TVar(int64_t(3)))->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(8)));
}

FIXTURE(UpsertEmptyBase_Or_Bool) {
  /* false (identity) | true = true on a never-created bool key. */
  TVar val;
  TMutation::New(TMutator::Or, TVar(true))->Apply(val);
  EXPECT_EQ(val, TVar(true));
}

FIXTURE(UpsertEmptyBase_Xor_Bool) {
  /* false (identity) ^ true = true on a never-created bool key. */
  TVar val;
  TMutation::New(TMutator::Xor, TVar(true))->Apply(val);
  EXPECT_EQ(val, TVar(true));
}

FIXTURE(UpsertEmptyBase_Union_Set) {
  /* {} (identity) U {1,2} = {1,2} on a never-created set key. */
  TVar val;
  TMutation::New(TMutator::Union, TVar(Rt::TSet<int64_t>{1, 2}))->Apply(val);
  EXPECT_EQ(val, TVar(Rt::TSet<int64_t>{1, 2}));
}

FIXTURE(UpsertEmptyBase_SymmetricDiff_Set) {
  /* {} (identity) symdiff {1,2} = {1,2} on a never-created set key. */
  TVar val;
  TMutation::New(TMutator::SymmetricDiff, TVar(Rt::TSet<int64_t>{1, 2}))->Apply(val);
  EXPECT_EQ(val, TVar(Rt::TSet<int64_t>{1, 2}));
}

FIXTURE(UpsertEmptyBase_Min_Int) {
  /* min(7) == 7 on a never-created int key: a first `<?= 7` seeds the
     value directly from the RHS (#213). */
  TVar val;
  EXPECT_FALSE(static_cast<bool>(val));
  TMutation::New(TMutator::Min, TVar(int64_t(7)))->Apply(val);
  EXPECT_TRUE(static_cast<bool>(val));
  EXPECT_EQ(val, TVar(int64_t(7)));
}

FIXTURE(UpsertEmptyBase_Min_ThenMin) {
  /* empty <?= 7 seeds (->7), then <?= 3 folds to min(7,3) == 3. */
  TVar val;
  TMutation::New(TMutator::Min, TVar(int64_t(7)))->Apply(val);
  TMutation::New(TMutator::Min, TVar(int64_t(3)))->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(3)));
}

FIXTURE(UpsertEmptyBase_Max_ThenMax) {
  /* empty >?= 3 seeds (->3), then >?= 7 folds to max(3,7) == 7. */
  TVar val;
  TMutation::New(TMutator::Max, TVar(int64_t(3)))->Apply(val);
  TMutation::New(TMutator::Max, TVar(int64_t(7)))->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(7)));
}

FIXTURE(UpsertEmptyBase_Intersection_Set) {
  /* intersection over the singleton {1,2,3} == {1,2,3} on a never-created
     set key. The identity (universal set) is not representable, but the
     singleton fold is still the RHS, so seeding from the RHS is correct
     (#213). */
  TVar val;
  TMutation::New(TMutator::Intersection, TVar(Rt::TSet<int64_t>{1, 2, 3}))->Apply(val);
  EXPECT_EQ(val, TVar(Rt::TSet<int64_t>{1, 2, 3}));
}

FIXTURE(UpsertEmptyBase_Mult_Int) {
  /* 1 (identity) * 6 == 6 on a never-created int key: a first `*= 6`
     seeds the value directly from the RHS (#213 PR2). Mult's identity is
     1, not the default 0, but the singleton fold is still the RHS, so the
     seed is correct -- a default-construct-then-multiply (0 * 6 = 0) would
     have been wrong, which is why Mult used to throw on an absent key. */
  TVar val;
  EXPECT_FALSE(static_cast<bool>(val));
  TMutation::New(TMutator::Mult, TVar(int64_t(6)))->Apply(val);
  EXPECT_TRUE(static_cast<bool>(val));
  EXPECT_EQ(val, TVar(int64_t(6)));
}

FIXTURE(UpsertEmptyBase_Mult_ThenMult) {
  /* empty *= 6 seeds (->6), then *= 3 folds to 18. */
  TVar val;
  TMutation::New(TMutator::Mult, TVar(int64_t(6)))->Apply(val);
  TMutation::New(TMutator::Mult, TVar(int64_t(3)))->Apply(val);
  EXPECT_EQ(val, TVar(int64_t(18)));
}

FIXTURE(MinMax_Augment_Fold) {
  /* min/max are commutative + associative, so two same-key mutations
     compose: Min(7).Augment(Min(3)) -> Min(3); Max(3).Augment(Max(7))
     -> Max(7). */
  auto lo = TMutation::New(TMutator::Min, TVar(int64_t(7)));
  lo->Augment(TMutation::New(TMutator::Min, TVar(int64_t(3))));
  TVar a(int64_t(5));
  lo->Apply(a);
  EXPECT_EQ(a, TVar(int64_t(3)));  // min(5, min(7,3)) == 3

  auto hi = TMutation::New(TMutator::Max, TVar(int64_t(3)));
  hi->Augment(TMutation::New(TMutator::Max, TVar(int64_t(7))));
  TVar b(int64_t(5));
  hi->Apply(b);
  EXPECT_EQ(b, TVar(int64_t(7)));  // max(5, max(3,7)) == 7
}

FIXTURE(AbsentKeySeed_Membership) {
  /* The seed-from-RHS set (#151/#152/#213): the commutative ops whose
     singleton fold is the RHS and which therefore upsert an absent key. */
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Add));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Or));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Xor));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Union));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::SymmetricDiff));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Min));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Max));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Intersection));
  EXPECT_TRUE(IsAbsentKeySeedRhs(TMutator::Mult));
  /* Deferred (no current demand; identity all-ones): And. */
  EXPECT_FALSE(IsAbsentKeySeedRhs(TMutator::And));
  /* Never seedable. */
  EXPECT_FALSE(IsAbsentKeySeedRhs(TMutator::Assign));
  EXPECT_FALSE(IsAbsentKeySeedRhs(TMutator::Sub));
}