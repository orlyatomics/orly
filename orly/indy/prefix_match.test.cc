/* <orly/indy/prefix_match.test.cc>

   Unit test for Atom::TCore::PrefixMatch (<orly/atom/kit2.h>), checked against the reference
   Sabot::MatchPrefixState. The disk walker's Match search calls PrefixMatch on every key it reads
   and, in a debug build, asserts that it agrees with MatchPrefixState (orly#792).

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

#include <orly/indy/key.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include <orly/atom/suprena.h>
#include <orly/desc.h>
#include <orly/native/all.h>
#include <orly/sabot/match_prefix_state.h>

#include <base/test/kit.h>

using namespace std;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;

using TMatch = Sabot::TMatchResult;

static const char *Name(TMatch r) {
  switch (r) {
    case TMatch::NoMatch: return "NoMatch";
    case TMatch::PrefixMatch: return "PrefixMatch";
    case TMatch::Unifies: return "Unifies";
  }
  return "?";
}

/* The pattern and the key live in separate arenas, as they do in the walker (the pattern in the
   caller's arena, the key in the disk file's), so the quick comparison can't fall back on offsets.
   Returns TCore::PrefixMatch's answer after checking it against MatchPrefixState's. */
template <typename TPat, typename TKeyVal>
static TMatch Check(const char *label, const TPat &pat, const TKeyVal &key) {
  TSuprena pat_arena, key_arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
  void *state_alloc_2 = static_cast<uint8_t *>(state_alloc) + Sabot::State::GetMaxStateSize();
  TKey p(pat, &pat_arena, state_alloc), k(key, &key_arena, state_alloc);
  TMatch got = p.GetCore().PrefixMatch(p.GetArena(), k.GetCore(), k.GetArena());
  TMatch want = Sabot::MatchPrefixState(
      *Sabot::State::TAny::TWrapper(p.GetCore().NewState(p.GetArena(), state_alloc)),
      *Sabot::State::TAny::TWrapper(k.GetCore().NewState(k.GetArena(), state_alloc_2)));
  if (got != want) {
    cout << label << ": TCore::PrefixMatch = " << Name(got) << ", MatchPrefixState = " << Name(want) << endl;
  }
  EXPECT_TRUE(got == want);
  return got;
}

static const Native::TFree<int64_t> FreeInt;

/* A string too long to sit directly in a core. */
static const string LongStr = "a string well past direct storage in a core";

FIXTURE(DescBeforeFree) {
  using D = TDesc<int64_t>;
  /* The issue's case: keys @ <['d', desc g, free::(int)]>. */
  EXPECT_TRUE(Check("'d', desc 3, free vs 'd', desc 3, 1", make_tuple(string("d"), D(3), FreeInt), make_tuple(string("d"), D(3), int64_t(1))) == TMatch::Unifies);
  EXPECT_TRUE(Check("desc 3, free vs desc 3, 5", make_tuple(D(3), FreeInt), make_tuple(D(3), int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("1, desc 3, free vs 1, desc 3, 9", make_tuple(int64_t(1), D(3), FreeInt), make_tuple(int64_t(1), D(3), int64_t(9))) == TMatch::Unifies);
  EXPECT_TRUE(Check("desc 2, free vs desc 3, 5", make_tuple(D(2), FreeInt), make_tuple(D(3), int64_t(5))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("desc 4, free vs desc 3, 5", make_tuple(D(4), FreeInt), make_tuple(D(3), int64_t(5))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("'d', desc 3, free vs 'e', desc 3, 1", make_tuple(string("d"), D(3), FreeInt), make_tuple(string("e"), D(3), int64_t(1))) == TMatch::NoMatch);
}

FIXTURE(DescWholeKeys) {
  using D = TDesc<int64_t>;
  EXPECT_TRUE(Check("desc 3, 4 vs desc 3, 5", make_tuple(D(3), int64_t(4)), make_tuple(D(3), int64_t(5))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("desc 3, 5 vs desc 3, 5", make_tuple(D(3), int64_t(5)), make_tuple(D(3), int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("desc 3, 4, free vs desc 3, 5, 9", make_tuple(D(3), int64_t(4), FreeInt), make_tuple(D(3), int64_t(5), int64_t(9))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("desc 3, 5, free vs desc 3, 5, 9", make_tuple(D(3), int64_t(5), FreeInt), make_tuple(D(3), int64_t(5), int64_t(9))) == TMatch::Unifies);
}

FIXTURE(MixedAscDesc) {
  using DI = TDesc<int64_t>;
  using DS = TDesc<string>;
  EXPECT_TRUE(Check("1, desc 'x', 2, desc 3, free vs same + 7",
                    make_tuple(int64_t(1), DS("x"), int64_t(2), DI(3), FreeInt),
                    make_tuple(int64_t(1), DS("x"), int64_t(2), DI(3), int64_t(7))) == TMatch::Unifies);
  EXPECT_TRUE(Check("1, desc 'x', 2, desc 3, free vs desc 'y'",
                    make_tuple(int64_t(1), DS("x"), int64_t(2), DI(3), FreeInt),
                    make_tuple(int64_t(1), DS("y"), int64_t(2), DI(3), int64_t(7))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("1, desc 'x', 2, desc 3, free vs desc 4",
                    make_tuple(int64_t(1), DS("x"), int64_t(2), DI(3), FreeInt),
                    make_tuple(int64_t(1), DS("x"), int64_t(2), DI(4), int64_t(7))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("desc long str, free vs desc long str, 5",
                    make_tuple(DS(LongStr), FreeInt), make_tuple(DS(LongStr), int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("desc <[1, long str]>, free vs same, 5",
                    make_tuple(TDesc<tuple<int64_t, string>>(make_tuple(int64_t(1), LongStr)), FreeInt),
                    make_tuple(TDesc<tuple<int64_t, string>>(make_tuple(int64_t(1), LongStr)), int64_t(5))) == TMatch::Unifies);
}

FIXTURE(NestedTuples) {
  using TInner = tuple<int64_t, string>;
  EXPECT_TRUE(Check("<[1, long str]>, free vs <[1, long str]>, 5",
                    make_tuple(TInner(1, LongStr), FreeInt), make_tuple(TInner(1, LongStr), int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("<[1, long str]>, free vs <[2, long str]>, 5",
                    make_tuple(TInner(1, LongStr), FreeInt), make_tuple(TInner(2, LongStr), int64_t(5))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("<[1, desc 3]>, free vs <[1, desc 3]>, 5",
                    make_tuple(make_tuple(int64_t(1), TDesc<int64_t>(3)), FreeInt),
                    make_tuple(make_tuple(int64_t(1), TDesc<int64_t>(3)), int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("<[1, desc 3]>, free vs <[1, desc 4]>, 5",
                    make_tuple(make_tuple(int64_t(1), TDesc<int64_t>(3)), FreeInt),
                    make_tuple(make_tuple(int64_t(1), TDesc<int64_t>(4)), int64_t(5))) == TMatch::NoMatch);
  /* A free nested inside a member unifies with any value there. */
  EXPECT_TRUE(Check("<[1, free]>, 2 vs <[1, 9]>, 2",
                    make_tuple(make_tuple(int64_t(1), FreeInt), int64_t(2)),
                    make_tuple(make_tuple(int64_t(1), int64_t(9)), int64_t(2))) == TMatch::Unifies);
  EXPECT_TRUE(Check("<[1, free]>, 2 vs <[3, 9]>, 2",
                    make_tuple(make_tuple(int64_t(1), FreeInt), int64_t(2)),
                    make_tuple(make_tuple(int64_t(3), int64_t(9)), int64_t(2))) == TMatch::NoMatch);
}

FIXTURE(StringsAndBlobs) {
  const Native::TBlob blob(reinterpret_cast<const uint8_t *>(LongStr.data()), LongStr.size());
  Native::TBlob other_blob = blob;
  other_blob.back() ^= 1;
  EXPECT_TRUE(Check("short str, free vs short str, 5", make_tuple(string("edge"), FreeInt), make_tuple(string("edge"), int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("long str, free vs long str, 5", make_tuple(LongStr, FreeInt), make_tuple(LongStr, int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("long str, free vs other long str, 5", make_tuple(LongStr, FreeInt), make_tuple(LongStr + "!", int64_t(5))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("long blob, free vs long blob, 5", make_tuple(blob, FreeInt), make_tuple(blob, int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("long blob, free vs other blob, 5", make_tuple(blob, FreeInt), make_tuple(other_blob, int64_t(5))) == TMatch::NoMatch);
  EXPECT_TRUE(Check("[1, 2], free vs [1, 2], 5",
                    make_tuple(vector<int64_t>{1, 2}, FreeInt), make_tuple(vector<int64_t>{1, 2}, int64_t(5))) == TMatch::Unifies);
  EXPECT_TRUE(Check("[1, 2], free vs [1, 3], 5",
                    make_tuple(vector<int64_t>{1, 2}, FreeInt), make_tuple(vector<int64_t>{1, 3}, int64_t(5))) == TMatch::NoMatch);
}

FIXTURE(Arity) {
  EXPECT_TRUE(Check("long str vs long str, 5 (shorter pattern)", make_tuple(LongStr), make_tuple(LongStr, int64_t(5))) == TMatch::PrefixMatch);
  EXPECT_TRUE(Check("desc 3 vs desc 3, 5 (shorter pattern)", make_tuple(TDesc<int64_t>(3)), make_tuple(TDesc<int64_t>(3), int64_t(5))) == TMatch::PrefixMatch);
}
