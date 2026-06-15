/* <orly/type/rec_group.test.cc>

   Unit test for <orly/type/rec_group.h> -- building and navigating
   mutually-recursive type groups (#116 Phase 2a), in the type layer with
   no synth/codegen.

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

#include <orly/type/rec_group.h>

#include <orly/type.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace Orly;
using namespace Orly::Type;

/* A sibling placeholder: reference member `k` within the group. */
static TType Ref(size_t k) { return TGroupRef::Get({}, k); }

/* The member type a group ref resolves to. */
static TType Nav(const TType &arm) { return ResolveGroupRef(arm.As<TGroupRef>()); }

/* `a is <| X(b) |>; b is <| Y(a) |>;` -- member 0 = a, member 1 = b. */
FIXTURE(MutualPair) {
  TTypeCzar czar;
  auto m = MakeRecGroup({ {{"X", Ref(1)}}, {{"Y", Ref(0)}} });
  TType a = m[0], b = m[1];

  /* Distinct members. */
  EXPECT_FALSE(a == b);

  /* The arms hold group refs that navigate to the sibling. */
  TType ax = a.As<TVariant>()->GetElems().at("X");
  TType by = b.As<TVariant>()->GetElems().at("Y");
  EXPECT_TRUE(ax.Is<TGroupRef>());
  EXPECT_TRUE(by.Is<TGroupRef>());
  EXPECT_TRUE(Nav(ax) == b);   // a.X -> b
  EXPECT_TRUE(Nav(by) == a);   // b.Y -> a
}

/* Building the same group again yields the same interned member types. */
FIXTURE(SameGroupDedups) {
  TTypeCzar czar;
  auto m1 = MakeRecGroup({ {{"X", Ref(1)}}, {{"Y", Ref(0)}} });
  auto m2 = MakeRecGroup({ {{"X", Ref(1)}}, {{"Y", Ref(0)}} });
  EXPECT_TRUE(m1[0] == m2[0]);
  EXPECT_TRUE(m1[1] == m2[1]);
}

/* The identity is canonical: supplying the same group with its members in
   the opposite order yields the same member types (structural coherence). */
FIXTURE(MemberOrderIndependent) {
  TTypeCzar czar;
  auto m = MakeRecGroup({ {{"X", Ref(1)}}, {{"Y", Ref(0)}} });   // 0=a(X->b), 1=b(Y->a)
  /* Same group, members swapped: 0=b(Y->a@1), 1=a(X->b@0). */
  auto swapped = MakeRecGroup({ {{"Y", Ref(1)}}, {{"X", Ref(0)}} });
  EXPECT_TRUE(swapped[0] == m[1]);  // the Y-member equals b
  EXPECT_TRUE(swapped[1] == m[0]);  // the X-member equals a
}

/* A structurally different group does not collide. */
FIXTURE(DistinctGroups) {
  TTypeCzar czar;
  auto m  = MakeRecGroup({ {{"X", Ref(1)}}, {{"Y", Ref(0)}} });
  auto ot = MakeRecGroup({ {{"P", Ref(1)}}, {{"Q", Ref(0)}} });
  EXPECT_FALSE(ot[0] == m[0]);
  EXPECT_FALSE(ot[0] == m[1]);
}

/* A three-member cycle a -> b -> c -> a navigates all the way around. */
FIXTURE(ThreeCycle) {
  TTypeCzar czar;
  auto m = MakeRecGroup({ {{"A", Ref(1)}}, {{"B", Ref(2)}}, {{"C", Ref(0)}} });
  TType a = m[0], b = m[1], c = m[2];
  EXPECT_TRUE(Nav(a.As<TVariant>()->GetElems().at("A")) == b);
  EXPECT_TRUE(Nav(b.As<TVariant>()->GetElems().at("B")) == c);
  EXPECT_TRUE(Nav(c.As<TVariant>()->GetElems().at("C")) == a);
}

/* From any one member, the whole group (canonical order) and the member's
   index within it are recoverable -- what codegen needs to enumerate a
   group's sibling classes. */
FIXTURE(GroupMembersFromMember) {
  TTypeCzar czar;
  auto m = MakeRecGroup({ {{"X", Ref(1)}}, {{"Y", Ref(0)}} });

  std::vector<TType> g0, g1;
  size_t i0, i1;
  EXPECT_TRUE(TryGetGroupMembers(m[0], g0, i0));
  EXPECT_TRUE(TryGetGroupMembers(m[1], g1, i1));

  /* Both members see the same group set, and the set holds exactly them. */
  EXPECT_EQ(g0.size(), 2u);
  EXPECT_TRUE(g0 == g1);
  EXPECT_TRUE(g0[i0] == m[0]);
  EXPECT_TRUE(g1[i1] == m[1]);
  EXPECT_TRUE(i0 != i1);

  /* A non-member type is rejected. */
  std::vector<TType> none;
  size_t ni;
  EXPECT_FALSE(TryGetGroupMembers(TInt::Get(), none, ni));
}

/* A member may also carry a non-recursive arm and a record payload. */
FIXTURE(MixedArms) {
  TTypeCzar czar;
  /* expr = <| Lit(int) | Neg(expr) | Pair(<{.l: stmt, .r: stmt}>) |>
     stmt = <| Done | Step(expr) |>  -- members 0=expr, 1=stmt */
  TType pair_rec = TObj::Get({{"l", Ref(1)}, {"r", Ref(1)}});
  auto m = MakeRecGroup({
      {{"Lit", TInt::Get()}, {"Neg", Ref(0)}, {"Pair", pair_rec}},
      {{"Done", TObj::Get({})}, {"Step", Ref(0)}},
  });
  TType expr = m[0], stmt = m[1];
  EXPECT_FALSE(expr == stmt);
  /* expr.Neg -> expr; expr.Pair.l -> stmt; stmt.Step -> expr. */
  EXPECT_TRUE(Nav(expr.As<TVariant>()->GetElems().at("Neg")) == expr);
  TType pair = expr.As<TVariant>()->GetElems().at("Pair");
  EXPECT_TRUE(Nav(pair.As<TObj>()->GetElems().at("l")) == stmt);
  EXPECT_TRUE(Nav(stmt.As<TVariant>()->GetElems().at("Step")) == expr);
}
