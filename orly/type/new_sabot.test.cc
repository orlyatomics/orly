/* <orly/type/new_sabot.test.cc>

   Unit test for <orly/type/new_sabot.h>.

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

#include <orly/type/new_sabot.h>

#include <cassert>
#include <cstdlib>
#include <sstream>
#include <string>

#include <orly/sabot/compare_types.h>
#include <orly/sabot/type_dumper.h>
#include <orly/native/all.h>
#include <orly/type/group_ref.h>
#include <orly/type/rec_group.h>
#include <orly/type/sabot_to_type.h>
#include <orly/type/self_ref.h>
#include <orly/type/type_czar.h>
#include <orly/type/variant.h>
#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;

static Type::TTypeCzar TypeCzar;

static string ToString(const Type::TType &type) {
  ostringstream strm;
  NewSabot(alloca(1000), type)->Accept(Sabot::TTypeDumper(strm));
  return strm.str();
}

FIXTURE(Typical) {
  EXPECT_EQ(ToString(Type::TBool::Get()), "bool");
  EXPECT_EQ(ToString(Type::TId::Get()), "uuid");
  EXPECT_EQ(ToString(Type::TInt::Get()), "int64");
  EXPECT_EQ(ToString(Type::TReal::Get()), "double");
  EXPECT_EQ(ToString(Type::TStr::Get()), "str");
  EXPECT_EQ(ToString(Type::TTimeDiff::Get()), "duration");
  EXPECT_EQ(ToString(Type::TTimePnt::Get()), "time_point");
  EXPECT_EQ(ToString(Type::TList::Get(Type::TReal::Get())), "vector(double)");
  EXPECT_EQ(ToString(Type::TOpt::Get(Type::TReal::Get())), "opt(double)");
  EXPECT_EQ(ToString(Type::TSet::Get(Type::TReal::Get())), "set(double)");
  EXPECT_EQ(ToString(Type::TDict::Get(Type::TReal::Get(), Type::TStr::Get())), "map(double, str)");
  EXPECT_EQ(ToString(Type::TAddr::Get({ { TAddrDir::Asc, Type::TReal::Get() } })), "tuple(double)");
  EXPECT_EQ(ToString(Type::TAddr::Get({ { TAddrDir::Desc, Type::TReal::Get() } })), "tuple(desc(double))");
  EXPECT_EQ(ToString(Type::TObj::Get({ { "x", Type::TReal::Get() }, { "y", Type::TReal::Get() } })), "record(x: double, y: double)");
}

/* Recursive sum types (issue #115).  A self-recursive variant translates to its
   #96 fixed-shape record with a finite self_ref leaf at each recursion point,
   and round-trips back to the identical interned orly type. */

/* tree is <| Leaf(int) | Branch(<{ .l: tree, .r: tree }>) |> */
static Type::TType MakeTree() {
  Type::TVariantElems arms;
  arms["Leaf"] = Type::TInt::Get();
  arms["Branch"] = Type::TObj::Get({ { "l", Type::TSelfRef::Get(0) }, { "r", Type::TSelfRef::Get(0) } });
  return Type::TVariant::Get(arms);
}

/* json is <| Null | N(int) | Arr([json]) | Obj({str: json}) |> -- self-ref under
   a list and under a dict value (the #120 container family). */
static Type::TType MakeJson() {
  Type::TVariantElems arms;
  arms["Null"] = Type::TObj::Get({});
  arms["N"] = Type::TInt::Get();
  arms["Arr"] = Type::TList::Get(Type::TSelfRef::Get(0));
  arms["Obj"] = Type::TDict::Get(Type::TStr::Get(), Type::TSelfRef::Get(0));
  return Type::TVariant::Get(arms);
}

static Type::TType RoundTrip(const Type::TType &type) {
  return Type::ToType(*Sabot::Type::TAny::TWrapper(NewSabot(alloca(1000), type)));
}

FIXTURE(RecursiveVariantTranslates) {
  /* The cycle becomes a finite self_ref leaf -- no infinite materialization. */
  const string dump = ToString(MakeTree());
  EXPECT_TRUE(dump.find("$which") != string::npos);
  EXPECT_TRUE(dump.find("self_ref(0)") != string::npos);
}

FIXTURE(RecursiveVariantRoundTrips) {
  /* orly type -> sabot -> orly type is the identity on the interned type. */
  const Type::TType tree = MakeTree();
  EXPECT_TRUE(RoundTrip(tree) == tree);

  const Type::TType json = MakeJson();
  EXPECT_TRUE(RoundTrip(json) == json);

  /* Distinct recursive types stay distinct through translation. */
  EXPECT_FALSE(RoundTrip(tree) == json);

  /* A back-reference at de Bruijn depth 1 -- an inner variant whose arm refers
     to the OUTER variant (the #125 nested-variant shape) -- carries its depth
     through translation too. */
  Type::TVariantElems outer;
  outer["X"] = Type::TVariant::Get({ { "Y", Type::TSelfRef::Get(1) } });
  const Type::TType nested = Type::TVariant::Get(outer);
  EXPECT_TRUE(RoundTrip(nested) == nested);
}

/* A MUTUALLY-recursive group (#116): `a is <|X(b)|>; b is <|Y(a)|>;`. Each
   member translates for storage (issue #115) by inlining to its canonical
   de Bruijn form -- so it no longer fails translation -- consistently (a stored
   set stays homogeneous) and distinctly per member. */
FIXTURE(MutualGroupTranslates) {
  auto m = Type::MakeRecGroup({ { { "X", Type::TGroupRef::Get({}, 1) } },
                                { { "Y", Type::TGroupRef::Get({}, 0) } } });
  const Type::TType a = m[0], b = m[1];

  void *ba = alloca(1000), *ba2 = alloca(1000), *bb = alloca(1000);
  Sabot::Type::TAny::TWrapper a_sabot(NewSabot(ba, a));
  Sabot::Type::TAny::TWrapper a_sabot2(NewSabot(ba2, a));
  Sabot::Type::TAny::TWrapper b_sabot(NewSabot(bb, b));

  /* Translates (no exception) and is finite/consistent for the same member. */
  EXPECT_TRUE(Atom::IsEq(Sabot::CompareTypes(*a_sabot, *a_sabot2)));
  /* Distinct members are distinct stored types. */
  EXPECT_FALSE(Atom::IsEq(Sabot::CompareTypes(*a_sabot, *b_sabot)));

  /* The stored type is the member's canonical inlined de Bruijn form, and
     round-trips back to it. */
  EXPECT_TRUE(RoundTrip(a) == Type::InlinedMemberType(a));
}

FIXTURE(RecursiveVariantCompareTypes) {
  /* The sabot-level CompareTypes agrees with orly-type identity for recursive
     types (set homogeneity / key ordering rides on this). */
  void *a = alloca(1000), *b = alloca(1000), *c = alloca(1000);
  Sabot::Type::TAny::TWrapper tree_a(NewSabot(a, MakeTree()));
  Sabot::Type::TAny::TWrapper tree_b(NewSabot(b, MakeTree()));
  EXPECT_TRUE(Atom::IsEq(Sabot::CompareTypes(*tree_a, *tree_b)));

  Sabot::Type::TAny::TWrapper json_c(NewSabot(c, MakeJson()));
  EXPECT_FALSE(Atom::IsEq(Sabot::CompareTypes(*tree_a, *json_c)));
}
