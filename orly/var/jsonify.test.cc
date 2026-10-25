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

#include <limits>
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
#include <orly/var/new_sabot.h>
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
  /* AppendStateJson (#1053) writes the reply text with no tree: byte for byte what the reply path
     formatted from the tree before, appended after whatever the buffer already holds. */
  string old_text = "prefix:", new_text = "prefix:";
  via_var.AppendTo(old_text);
  AppendStateJson(new_text, *state);
  EXPECT_EQ(new_text, old_text);
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
  CheckState(Rt::TSet<int64_t>{});
  CheckState(vector<Rt::TSet<TUuid>>{{a, b}, {}});
  CheckState(Rt::TDict<int64_t, double>{{5, 5.5}, {2, 2.25}});
  CheckState(Rt::TDict<string, vector<int64_t>>{{"x", {1}}, {"y\"z", {}}});
  /* Records, variants, and sets of records containing variants (the M1 entity/page shapes). */
  const auto empty_obj = TVar::Obj(unordered_map<string, TVar>{});
  CheckState(empty_obj);
  const auto rec = TVar::Obj(unordered_map<string, TVar>{
      {"p", TVar(a)},
      {"s", TVar(b)},
      {"v", TVar(string("hello"))}});
  CheckState(rec);
  const auto var_type = Type::TVariant::Get({
      {"Deleted", Type::TObj::Get({})},
      {"Integer", Type::TInt::Get()},
      {"Text", Type::TStr::Get()}});
  const auto v_del = TVar::Variant(var_type, "Deleted", empty_obj);
  const auto v_int = TVar::Variant(var_type, "Integer", TVar(int64_t(99)));
  const auto v_txt = TVar::Variant(var_type, "Text", TVar(string("val")));
  CheckState(v_del);
  CheckState(v_int);
  CheckState(v_txt);
  const auto row1 = TVar::Obj(unordered_map<string, TVar>{{"p", TVar(a)}, {"s", TVar(b)}, {"v", v_int}});
  const auto row2 = TVar::Obj(unordered_map<string, TVar>{{"p", TVar(b)}, {"s", TVar(a)}, {"v", v_txt}});
  const auto row3 = TVar::Obj(unordered_map<string, TVar>{{"p", TVar(a)}, {"s", TVar(a)}, {"v", v_del}});
  CheckState(row1);
  CheckState(TVar::Set(Rt::TSet<TVar>{row1, row2, row3}, row1.GetType()));
  /* Ints past 2^53 lose the same low bits as the tree's double. */
  CheckState(int64_t(1) << 60);
  CheckState(int64_t((1LL << 53) + 1));
  CheckState(std::numeric_limits<int64_t>::min());
  CheckState(-0.0);
  CheckState(1e300);
  CheckState(5e-7);
  /* Maps: string keys verbatim, other keys as their JSON text, sorted, nested values direct. */
  CheckState(Rt::TDict<string, int64_t>{{"b", 2}, {"a", 1}, {"\xc3\xa9", 3}, {"A", 4}, {"", 5}});
  CheckState(Rt::TDict<int64_t, string>{{10, "ten"}, {9, "nine"}, {-1, "minus"}});
  CheckState(Rt::TDict<TUuid, int64_t>{{a, 1}, {b, 2}});
  CheckState(Rt::TDict<tuple<int64_t, string>, bool>{{{1, "x\"y"}, true}, {{0, "z"}, false}});
  CheckState(Rt::TDict<string, Rt::TDict<string, int64_t>>{{"o", {{"i", 1}}}, {"e", {}}});
  CheckState(Rt::TDict<string, int64_t>{});
  /* Two real keys whose text is the same: the tree keeps one, so this map takes the tree path. */
  CheckState(Rt::TDict<double, int64_t>{{1.0000001, 1}, {1.0000002, 2}});
  /* Records holding every other kind, a variant inside an optional, and a record inside a map. */
  const auto big_rec = TVar::Obj(unordered_map<string, TVar>{
      {"id", TVar(a)},
      {"n", TVar(int64_t(3))},
      {"r", TVar(0.1)},
      {"t", TVar(Rt::TSet<int64_t>{3, 1})},
      {"m", TVar(Rt::TDict<string, int64_t>{{"k", 1}})},
      {"o", TVar(Rt::TOpt<int64_t>())},
      {"v", v_txt},
      {"rows", TVar::List(vector<TVar>{row1, row2, row3}, row1.GetType())},
      {"Z", TVar(string("upper sorts first"))},
      {"_", TVar(false)}});
  CheckState(big_rec);
  {
    /* A TVar must reach the sabot as itself (orly/var/new_sabot.h), not as an empty object. */
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    string text;
    AppendStateJson(text, *Sabot::State::TAny::TWrapper(Native::State::New(big_rec, state_alloc)));
    EXPECT_NE(text.find(R"("Z":"upper sorts first")"), string::npos);
    EXPECT_NE(text.find(R"("rows":[{)"), string::npos);
  }
  CheckState(TVar::Opt(Rt::TOpt<TVar>(v_int), var_type));
  CheckState(TVar::Opt(Rt::TOpt<TVar>(), var_type));
  CheckState(TVar::Dict(Rt::TDict<TVar, TVar>{{TVar(string("r1")), row1}, {TVar(string("r2")), row2}},
                        Type::TStr::Get(), row1.GetType()));
  /* A page-sized reply: a list of 1000 rows. */
  vector<TVar> page;
  for (int64_t i = 0; i < 1000; ++i) {
    page.push_back(TVar::Obj(unordered_map<string, TVar>{
        {"p", TVar(i % 2 ? a : b)}, {"s", TVar(int64_t(i))}, {"v", i % 3 ? v_txt : v_int}}));
  }
  CheckState(TVar::List(page, page.front().GetType()));
}
