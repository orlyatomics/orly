/* <orly/code_gen/variant_parity.test.cc>

   Checks that a GENERATED variant's native sabot (Native::Variant, #946)
   produces exactly the type and state of the AsVar() path it replaced, so
   the bytes written are unchanged.

   par.frozen.h is orlyc's verbatim header for par_t in
   variant_parity_test/parity.orly: a 15-arm variant with a record, a set
   and a dict of records, a closed nested variant, a recursive nested
   variant (which keeps the AsVar() path inside the native outer record),
   id, time_diff, time_pnt, bool, an empty arm, and the tags X_u, Xa and xb,
   whose byte order ('_' < 'a', upper < lower) must be the field order.
   For each value, both paths are dumped (StateDumper and TypeDumper), alone
   and as part of a key, and must match exactly.

   opt.frozen.h is the header for opt_t, a variant whose arms have optional
   payloads (#978). An optional payload keeps the variant on the AsVar() path
   (it has no native sabot), so the check there is that the state, the type
   sabot of its own state, Type::For and the declared type all agree, that an
   arm stored as `T??` tells "arm not taken" from "arm taken, payload
   unknown", and that every state reads back as the value written.

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

/* Concrete type headers first, as in a package (see variant_emit.test.cc). */
#include <orly/type.h>

#include <orly/code_gen/variant_parity_test/opt.frozen.h>
#include <orly/code_gen/variant_parity_test/par.frozen.h>

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <base/uuid.h>
#include <orly/native/all.h>
#include <orly/sabot/state_dumper.h>
#include <orly/sabot/type_dumper.h>
#include <orly/shared_enum.h>
#include <orly/type/sabot_to_type.h>
#include <orly/type/type_czar.h>
#include <orly/var/new_sabot.h>
#include <orly/var/sabot_to_var.h>
#include <base/test/kit.h>

using namespace std;
using namespace Orly;

using TPar = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb;
using TInner = Rt::Variants::TVariantV2i1AO01B;
using TLeaf = Rt::Variants::TVariantV2i4LeafLX04Node;
using TRec = Rt::Objects::TObjO2i1ns4name;

/* The state and type dumps of a state sabot. */
static pair<string, string> Dump(const Sabot::State::TAny &state) {
  ostringstream state_strm, type_strm;
  state.Accept(Sabot::TStateDumper(state_strm));
  void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  Sabot::Type::TAny::TWrapper type(state.GetType(type_alloc));
  type->Accept(Sabot::TTypeDumper(type_strm));
  return { state_strm.str(), type_strm.str() };
}

/* One value of every arm, with edge-case payloads. */
static vector<TPar> MakeValues() {
  const TRec empty_rec(int64_t(-1), string());
  const TRec rec(int64_t(42), string("x"));
  const Base::TUuid id("6a3c1f2e-8b4d-4e5f-9a0b-1c2d3e4f5a6b");
  const Base::Chrono::TTimePnt pnt(Base::Chrono::TTimeDiff(1700000000123456789LL));
  vector<TPar> out;
  out.push_back(TPar::MkBool(false));
  out.push_back(TPar::MkBool(true));
  out.push_back(TPar::MkDict(Rt::TDict<string, TRec>{}));
  out.push_back(TPar::MkDict(Rt::TDict<string, TRec>{ { "", empty_rec }, { "k", rec } }));
  out.push_back(TPar::MkEmpty(Rt::Objects::TObjO0()));
  out.push_back(TPar::MkIdent(id));
  out.push_back(TPar::MkInt(int64_t(-9223372036854775807LL - 1)));
  out.push_back(TPar::MkInt(int64_t(-1)));
  out.push_back(TPar::MkInt(int64_t(0)));
  out.push_back(TPar::MkNested(TInner::MkA(int64_t(-7))));
  out.push_back(TPar::MkNested(TInner::MkB(Rt::Objects::TObjO0())));
  out.push_back(TPar::MkRec(empty_rec));
  out.push_back(TPar::MkRec(rec));
  out.push_back(TPar::MkRecur(TLeaf::MkLeaf(int64_t(-3))));
  out.push_back(TPar::MkRecur(TLeaf::MkNode(vector<TLeaf>{
      TLeaf::MkLeaf(int64_t(1)), TLeaf::MkNode(vector<TLeaf>{ TLeaf::MkLeaf(int64_t(2)) }) })));
  out.push_back(TPar::MkSet(Rt::TSet<TRec>{}));
  out.push_back(TPar::MkSet(Rt::TSet<TRec>{ empty_rec, rec }));
  out.push_back(TPar::MkStr(string()));
  out.push_back(TPar::MkStr(string("hello")));
  out.push_back(TPar::MkTD(Base::Chrono::TTimeDiff(-5)));
  out.push_back(TPar::MkTP(pnt));
  out.push_back(TPar::MkX_u(int64_t(-2)));
  out.push_back(TPar::MkXa(int64_t(3)));
  out.push_back(TPar::Mkxb(int64_t(-4)));
  return out;
}

/* Native::State::New(v) dumps exactly like Var::NewSabot(v.AsVar()), and
   reads back as the same Type and Var. */
FIXTURE(NativeMatchesAsVar) {
  Type::TTypeCzar type_czar;
  const auto values = MakeValues();
  size_t which_seen = 0;
  for (const auto &val : values) {
    const Var::TVar var = val.AsVar();
    void *var_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper var_state(Var::NewSabot(var_alloc, var));
    void *native_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper native_state(Native::State::New(val, native_alloc));
    const auto expected = Dump(*var_state);
    const auto actual = Dump(*native_state);
    EXPECT_EQ(actual.first, expected.first);
    EXPECT_EQ(actual.second, expected.second);
    EXPECT_TRUE(Var::ToVar(*native_state) == var);
    void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper native_type(native_state->GetType(type_alloc));
    EXPECT_TRUE(Type::ToType(*native_type) == Type::TDt<TPar>::GetType());
    which_seen |= size_t(1) << Native::VariantArms<TPar>::GetWhich(val);
  }
  /* Every one of the 15 arms was exercised. */
  EXPECT_EQ(which_seen, (size_t(1) << 15) - 1);
}

/* The type sabot from Native::Type::For matches the declared type's. */
FIXTURE(NativeTypeMatchesDeclared) {
  Type::TTypeCzar type_czar;
  void *native_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  Sabot::Type::TAny::TWrapper native_type(Native::Type::For<TPar>::GetType(native_alloc));
  void *declared_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  Sabot::Type::TAny::TWrapper declared_type(Type::NewSabot(declared_alloc, Type::TDt<TPar>::GetType()));
  ostringstream native_strm, declared_strm;
  native_type->Accept(Sabot::TTypeDumper(native_strm));
  declared_type->Accept(Sabot::TTypeDumper(declared_strm));
  EXPECT_EQ(native_strm.str(), declared_strm.str());
  EXPECT_TRUE(Type::ToType(*native_type) == Type::TDt<TPar>::GetType());
}

/* As part of a key (<[v, n]>), the variant encodes the same both ways. */
FIXTURE(KeyMatchesAsVar) {
  Type::TTypeCzar type_czar;
  for (const auto &val : MakeValues()) {
    const auto key = make_tuple(val, int64_t(-1));
    const Var::TVar var_key = Var::TVar::Addr(vector<pair<TAddrDir, Var::TVar>>{
        { TAddrDir::Asc, val.AsVar() }, { TAddrDir::Asc, Var::TVar(int64_t(-1)) } });
    void *var_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper var_state(Var::NewSabot(var_alloc, var_key));
    void *native_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper native_state(Native::State::New(key, native_alloc));
    const auto expected = Dump(*var_state);
    const auto actual = Dump(*native_state);
    EXPECT_EQ(actual.first, expected.first);
    EXPECT_EQ(actual.second, expected.second);
  }
}

using TOptV = Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L;
using TOptRec = Rt::Objects::TObjO1Pi1a;

/* One value of every arm of opt_t in each of its optional states. */
static vector<TOptV> MakeOptValues() {
  using TOptInt = Rt::TOpt<int64_t>;
  vector<TOptV> out;
  out.push_back(TOptV::MkE(Rt::TOpt<string>()));
  out.push_back(TOptV::MkE(Rt::TOpt<string>(string())));
  out.push_back(TOptV::MkE(Rt::TOpt<string>(string("a"))));
  out.push_back(TOptV::MkF(int64_t(1)));
  out.push_back(TOptV::MkG(TOptInt()));
  out.push_back(TOptV::MkG(TOptInt(int64_t(0))));
  out.push_back(TOptV::MkG(TOptInt(int64_t(5))));
  out.push_back(TOptV::MkH(TOptRec(TOptInt())));
  out.push_back(TOptV::MkH(TOptRec(TOptInt(int64_t(5)))));
  out.push_back(TOptV::MkL(vector<TOptInt>{}));
  out.push_back(TOptV::MkL(vector<TOptInt>{ TOptInt() }));
  out.push_back(TOptV::MkL(vector<TOptInt>{ TOptInt(int64_t(1)), TOptInt(), TOptInt(int64_t(3)) }));
  return out;
}

/* An arm with an optional payload `T?` is stored as `T??`, an arm with a
   plain payload as `T?`, and the stored record is the same for every value. */
FIXTURE(OptArmStoredType) {
  Type::TTypeCzar type_czar;
  void *alloc = alloca(Sabot::Type::GetMaxTypeSize());
  Sabot::Type::TAny::TWrapper type(Type::NewSabot(alloc, Type::TDt<TOptV>::GetType()));
  ostringstream strm;
  type->Accept(Sabot::TTypeDumper(strm));
  /* E: str? -> str??, F: int -> int?, G: int? -> int??, H and L have the optional
     inside the payload only. This is the type release builds stored for E and
     G before #978 (the debug assertion was the only thing that differed). */
  EXPECT_EQ(strm.str(),
            "record($which: int64, E: opt(opt(str)), F: opt(int64), G: opt(opt(int64)), "
            "H: opt(record(a: opt(int64))), L: opt(vector(opt(int64))))");
}

/* Every opt_t state dumps the same through the AsVar() path, the generated
   factory and the declared type, and reads back as the value written. */
FIXTURE(OptArmMatchesAsVarAndRoundTrips) {
  Type::TTypeCzar type_czar;
  const auto values = MakeOptValues();
  vector<string> seen;
  for (const auto &val : values) {
    const Var::TVar var = val.AsVar();
    void *var_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper var_state(Var::NewSabot(var_alloc, var));
    void *native_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper native_state(Native::State::New(val, native_alloc));
    const auto expected = Dump(*var_state);
    const auto actual = Dump(*native_state);
    EXPECT_EQ(actual.first, expected.first);
    EXPECT_EQ(actual.second, expected.second);
    /* The type of the state is the type the declared type lowers to. */
    void *declared_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper declared(Type::NewSabot(declared_alloc, Type::TDt<TOptV>::GetType()));
    ostringstream declared_strm;
    declared->Accept(Sabot::TTypeDumper(declared_strm));
    EXPECT_EQ(actual.second, declared_strm.str());
    void *for_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper for_type(Native::Type::For<TOptV>::GetType(for_alloc));
    ostringstream for_strm;
    for_type->Accept(Sabot::TTypeDumper(for_strm));
    EXPECT_EQ(actual.second, for_strm.str());
    /* Read back. */
    EXPECT_TRUE(Var::ToVar(*native_state) == var);
    void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper native_type(native_state->GetType(type_alloc));
    EXPECT_TRUE(Type::ToType(*native_type) == Type::TDt<TOptV>::GetType());
    /* No two of the values are stored alike. */
    for (const auto &prev : seen) {
      EXPECT_NE(prev, actual.first);
    }
    seen.push_back(actual.first);
  }
}

/* The same, as part of a key. */
FIXTURE(OptArmKeyMatchesAsVar) {
  Type::TTypeCzar type_czar;
  for (const auto &val : MakeOptValues()) {
    const auto key = make_tuple(val, int64_t(-1));
    const Var::TVar var_key = Var::TVar::Addr(vector<pair<TAddrDir, Var::TVar>>{
        { TAddrDir::Asc, val.AsVar() }, { TAddrDir::Asc, Var::TVar(int64_t(-1)) } });
    void *var_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper var_state(Var::NewSabot(var_alloc, var_key));
    void *native_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper native_state(Native::State::New(key, native_alloc));
    const auto expected = Dump(*var_state);
    const auto actual = Dump(*native_state);
    EXPECT_EQ(actual.first, expected.first);
    EXPECT_EQ(actual.second, expected.second);
  }
}
