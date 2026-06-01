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
   ops into one. Until this change Augment unconditionally threw -- the
   data-model side of the "field calls compose under contention" story
   the README has always claimed. Concrete consumers (the Tetris merge
   path that collides cross-POV mutations on the same key) are not yet
   wired up; this is the building block they need. */

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