/* <orly/type/canon.test.cc>

   Unit test for <orly/type/canon.h> -- equirecursive equality and
   canonicalization of de Bruijn recursive types (#116 Phase 1), exercised
   directly on hand-built mu-terms with no language surface.

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

#include <orly/type/canon.h>

#include <orly/type.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace Orly;
using namespace Orly::Type;

/* Helpers to build mu-terms tersely. */
static TType Vt(const TVariantElems &e) { return TVariant::Get(e); }
static TType Self(size_t d) { return TSelfRef::Get(d); }

/* `t is <| A(t) |>` -- the minimal self-recursive form. */
FIXTURE(SelfRecursiveIsCanonical) {
  TTypeCzar czar;
  TType t = Vt({{"A", Self(0)}});
  EXPECT_TRUE(Equiv(t, t));
  EXPECT_TRUE(Canon(t) == t);  // already minimal -> identity
}

/* `<| A(<| A(self@1) |>) |>` unfolds to the same infinite tree as
   `<| A(self@0) |>`; Canon must collapse the redundant level. */
FIXTURE(NonMinimalSelfCollapses) {
  TTypeCzar czar;
  TType min = Vt({{"A", Self(0)}});
  TType redundant = Vt({{"A", Vt({{"A", Self(1)}})}});
  EXPECT_TRUE(Equiv(redundant, min));
  EXPECT_TRUE(Canon(redundant) == min);
  EXPECT_TRUE(Canon(redundant) == Canon(min));
}

/* Mutual recursion: a = <| X(<| Y(self@1) |>) |>, b = <| Y(<| X(self@1) |>) |>.
   The accessor a.X unrolls to <| Y(a) |>, which is b -- but a different
   finite term. Canon must reconcile them. */
FIXTURE(MutualRecursionCanonicalizes) {
  TTypeCzar czar;
  TType a = Vt({{"X", Vt({{"Y", Self(1)}})}});
  TType b = Vt({{"Y", Vt({{"X", Self(1)}})}});

  /* `Unroll(a.X)` = <| Y(a) |>. */
  TType ya = Vt({{"Y", a}});

  EXPECT_TRUE(Equiv(ya, b));       // same infinite tree
  EXPECT_FALSE(Equiv(a, b));       // genuinely different types (X- vs Y-headed)
  EXPECT_FALSE(ya == b);           // ... but distinct finite terms before canon

  EXPECT_TRUE(Canon(b) == b);            // b is already minimal
  EXPECT_TRUE(Canon(ya) == Canon(b));    // the crux: a.X canonicalizes to b
  EXPECT_TRUE(Canon(ya) == b);
  EXPECT_FALSE(Canon(a) == Canon(b));    // a and b stay distinct
}

/* A finite, non-recursive type is unchanged, and distinct nested levels
   that are NOT equirecursively equal are not folded. */
FIXTURE(NonRecursiveUnchanged) {
  TTypeCzar czar;
  TType flat = Vt({{"A", TInt::Get()}, {"B", TStr::Get()}});
  EXPECT_TRUE(Canon(flat) == flat);

  TType nested = Vt({{"A", Vt({{"A", TInt::Get()}})}});  // outer A-child != inner
  EXPECT_TRUE(Canon(nested) == nested);
  EXPECT_FALSE(Equiv(nested, Vt({{"A", TInt::Get()}})));
}

/* Recursion through a record payload (the tree shape from #103): a
   redundant unfolding still collapses, and equality holds across forms. */
FIXTURE(RecursionThroughRecord) {
  TTypeCzar czar;
  /* tree = <| Leaf(int) | Node(<{.l: self@0, .r: self@0}>) |> */
  auto node_rec = [](const TType &child) {
    return TObj::Get({{"l", child}, {"r", child}});
  };
  TType tree = Vt({{"Leaf", TInt::Get()}, {"Node", node_rec(Self(0))}});

  /* A once-unrolled form: Node's children are the whole tree spelled out
     one level, with self@1 closing the loop. */
  TType inner = Vt({{"Leaf", TInt::Get()}, {"Node", node_rec(Self(1))}});
  TType unrolled = Vt({{"Leaf", TInt::Get()}, {"Node", node_rec(inner)}});

  EXPECT_TRUE(Equiv(unrolled, tree));
  EXPECT_TRUE(Canon(tree) == tree);
  EXPECT_TRUE(Canon(unrolled) == tree);
}

/* Basic Equiv sanity on leaves. */
FIXTURE(EquivLeaves) {
  TTypeCzar czar;
  EXPECT_TRUE(Equiv(TInt::Get(), TInt::Get()));
  EXPECT_FALSE(Equiv(TInt::Get(), TStr::Get()));
}
