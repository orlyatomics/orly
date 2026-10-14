/* <orly/var/jsonify.test.cc>

   Unit test for <orly/var/jsonify.h>.

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

#include <orly/var/jsonify.h>

#include <tuple>
#include <vector>

#include <base/as_str.h>
#include <base/json.h>
#include <orly/desc.h>
#include <orly/native/all.h>
#include <orly/rt/containers.h>
#include <orly/rt/mutable.h>
#include <orly/rt/opt.h>
#include <orly/type/type_czar.h>
#include <orly/var.h>
#include <orly/var/sabot_to_var.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Var;

static Type::TTypeCzar TypeCzar;

/* The wire contract Var::ToJson replaces (#377): jsonify to text, then
   TJson::Parse the text back.  For every var the direct tree must equal
   what the old round-trip produced. */
static TJson ViaRoundTrip(const TVar &var) {
  return TJson::Parse(AsStrFunc(&Var::Jsonify, var));
}

FIXTURE(MatchesTheOldRoundTrip) {
  vector<TVar> samples;
  samples.emplace_back(true);
  samples.emplace_back(false);
  samples.emplace_back(int64_t(42));
  samples.emplace_back(int64_t(-7));
  /* Reals exactly representable within the text hop's six significant
     digits, so both paths agree (full-precision reals are pinned in the
     next fixture). */
  samples.emplace_back(2.5);
  samples.emplace_back(-0.25);
  samples.emplace_back(string("hello"));
  samples.emplace_back(string("quote\" slash\\ newline\n"));
  samples.emplace_back(string(""));
  samples.emplace_back(TUuid("1b4e28ba-2fa1-11d2-883f-b9a761bde3fb"));
  samples.emplace_back(Base::Chrono::TTimeDiff(12345));
  samples.emplace_back(Base::Chrono::TTimePnt(Base::Chrono::TTimeDiff(1719878400)));
  samples.emplace_back(vector<int64_t>{5, 2, 1});
  samples.emplace_back(vector<string>{"a", "b\"c"});
  samples.emplace_back(Rt::TSet<int64_t>{5, 2, 1});
  /* Non-string dict keys take the compact-JSON-text key convention. */
  samples.emplace_back(Rt::TDict<int64_t, double>{{5, 5.5}, {2, 2.25}});
  samples.emplace_back(Rt::TDict<string, int64_t>{{"x", 1}, {"y\"z", 2}});
  samples.emplace_back(tuple<int64_t, string, bool>(5, string("addr"), true));
  samples.emplace_back(Rt::TOpt<int64_t>(9));
  samples.emplace_back(Rt::TOpt<int64_t>());
  samples.emplace_back(Rt::TMutable<std::tuple<int64_t>, int64_t>(Rt::TOpt<std::tuple<int64_t>>(std::make_tuple(int64_t(7))), 5));
  for (const auto &var : samples) {
    EXPECT_TRUE(ToJson(var) == ViaRoundTrip(var));
  }
}

FIXTURE(RealsKeepFullPrecision) {
  /* The one deliberate divergence from the old round-trip: the text hop
     clipped reals to the ostream's six significant digits; the direct
     tree keeps the whole double. */
  const double val = 123456789.123;
  EXPECT_EQ(ToJson(TVar(val)).GetNumber(), val);
  /* Non-finite reals still map to null, the lenient JSON convention
     (#276). */
  EXPECT_TRUE(ToJson(TVar(std::nan(""))) == TJson());
  EXPECT_TRUE(ToJson(TVar(1.0 / 0.0)) == TJson());
}

/* StateToJson (the reply path's converter) must equal ToJson(ToVar(state)) for every state,
   both the kinds it builds directly and the ones it hands to that path. */
template <typename TVal>
static void CheckState(const TVal &val) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Sabot::State::TAny::TWrapper state(Native::State::New(val, state_alloc));
  const TJson direct = StateToJson(*state), via_var = ToJson(ToVar(*state));
  EXPECT_TRUE(direct == via_var);
  EXPECT_EQ(AsStr(direct), AsStr(via_var));
}

FIXTURE(StateToJsonMatchesToVar) {
  const TUuid a("1b4e28ba-2fa1-11d2-883f-b9a761bde3fb"), b("00000000-0000-0000-0000-000000000000");
  CheckState(int64_t(42));
  CheckState(int64_t(-7));
  CheckState(true);
  CheckState(2.5);
  CheckState(123456789.123);
  CheckState(std::nan(""));
  CheckState(1.0 / 0.0);
  CheckState(string("quote\" slash\\ newline\n\x01 caf\xc3\xa9"));
  CheckState(string(""));
  CheckState(a);
  CheckState(Base::Chrono::TTimeDiff(12345));
  CheckState(Base::Chrono::TTimePnt(Base::Chrono::TTimeDiff(1719878400)));
  CheckState(vector<int64_t>{5, 2, 1});
  CheckState(vector<string>{});
  CheckState(vector<vector<TUuid>>{{a, b}, {}, {b}});
  CheckState(vector<tuple<string, TUuid, TUuid, string, TUuid, TUuid>>{
      {"vt", a, b, "text value 1", b, a}, {"vt", b, a, "x\"y", a, a}});
  CheckState(tuple<int64_t, TDesc<int64_t>, double>(5, TDesc<int64_t>(1), 5.5));
  CheckState(tuple<TDesc<string>, bool>(TDesc<string>(string("d")), false));
  CheckState(vector<tuple<TDesc<TUuid>, int64_t>>{{TDesc<TUuid>(a), 1}, {TDesc<TUuid>(b), 2}});
  CheckState(Rt::TOpt<int64_t>(9));
  CheckState(Rt::TOpt<int64_t>());
  CheckState(Rt::TOpt<vector<TUuid>>(vector<TUuid>{a}));
  CheckState(Rt::TSet<int64_t>{5, 2, 1});
  CheckState(vector<Rt::TSet<TUuid>>{{a, b}});
  CheckState(Rt::TDict<int64_t, double>{{5, 5.5}, {2, 2.25}});
  CheckState(Rt::TDict<string, vector<int64_t>>{{"x", {1}}, {"y\"z", {}}});
}
