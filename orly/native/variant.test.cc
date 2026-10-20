/* <orly/native/variant.test.cc>

   Unit test for <orly/native/variant.h>.

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

#include <orly/native/variant.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <orly/native/all.h>
#include <orly/sabot/state_dumper.h>
#include <orly/sabot/type_dumper.h>
#include <orly/type/obj.h>
#include <orly/type/sabot_to_type.h>
#include <orly/type/type_czar.h>
#include <orly/type/variant.h>
#include <orly/var/new_sabot.h>
#include <orly/var/sabot_to_var.h>
#include <base/test/kit.h>

using namespace std;
using namespace Orly;

static Type::TTypeCzar TypeCzar;

/* The empty object, the payload of a tag-only arm. */
class TNothing {};

/* Shaped like a variant the compiler generates for
   <| Deleted | Integer(int) | Numbers([int]) | Text(str) |>. */
class TShape {
  public:

  size_t Which;
  TNothing VDeleted;
  int64_t VInteger;
  vector<int64_t> VNumbers;
  string VText;

};  // TShape

/* What the compiler generates for TShape. */
namespace Orly {

  namespace Native {

    template <>
    class VariantArms<TShape> final {
      NO_CONSTRUCTION(VariantArms);
      public:
      static size_t GetArmCount() {
        return 4;
      }
      static const char *GetArmName(size_t arm) {
        switch (arm) {
          case 0: return "Deleted";
          case 1: return "Integer";
          case 2: return "Numbers";
          case 3: return "Text";
        }
        throw Sabot::TIdxTooBig();
      }
      static size_t GetWhich(const TShape &val) {
        return val.Which;
      }
      static Sabot::Type::TOpt *NewArmOptType(size_t arm, void *type_alloc) {
        switch (arm) {
          case 0: return Type::For<std::optional<TNothing>>::GetOptType(type_alloc);
          case 1: return Type::For<std::optional<int64_t>>::GetOptType(type_alloc);
          case 2: return Type::For<std::optional<std::vector<int64_t>>>::GetOptType(type_alloc);
          case 3: return Type::For<std::optional<std::string>>::GetOptType(type_alloc);
        }
        throw Sabot::TIdxTooBig();
      }
      static Sabot::State::TAny *NewPayloadState(const TShape &val, void *state_alloc) {
        switch (val.Which) {
          case 0: return State::New(val.VDeleted, state_alloc);
          case 1: return State::New(val.VInteger, state_alloc);
          case 2: return State::New(val.VNumbers, state_alloc);
          case 3: return State::New(val.VText, state_alloc);
        }
        throw Sabot::TIdxTooBig();
      }
    };  // VariantArms<TShape>

    template <>
    class State::Factory<TShape> final {
      NO_CONSTRUCTION(Factory);
      public:
      static Sabot::State::TAny *New(const TShape &val, void *state_alloc) {
        return Variant<TShape>::NewState(val, state_alloc);
      }
    };  // State::Factory<TShape>

    template <>
    class Type::For<TShape> final {
      NO_CONSTRUCTION(For);
      public:
      static Sabot::Type::TAny *GetType(void *type_alloc) {
        return Variant<TShape>::NewType(type_alloc);
      }
      static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {
        return Variant<TShape>::NewType(type_alloc);
      }
    };  // Type::For<TShape>

  }  // Native

}  // Orly

static string Dump(const Sabot::State::TAny &state) {
  ostringstream strm;
  state.Accept(Sabot::TStateDumper(strm));
  return strm.str();
}

static string Dump(const Sabot::Type::TAny &type) {
  ostringstream strm;
  type.Accept(Sabot::TTypeDumper(strm));
  return strm.str();
}

/* Every arm of a native variant must produce exactly the type and state that
   the Var path (Var::SS::TVariantRec) produces for the same value. */
FIXTURE(MatchesVar) {
  const Type::TType empty = Type::TObj::Get({});
  const Type::TType var_type = Type::TVariant::Get({
      { "Deleted", empty }, { "Integer", Type::TInt::Get() },
      { "Numbers", Type::TList::Get(Type::TInt::Get()) }, { "Text", Type::TStr::Get() } });
  TShape vals[4];
  vals[0].Which = 0;
  vals[1].Which = 1;
  vals[1].VInteger = -384;
  vals[2].Which = 2;
  vals[2].VNumbers = { 1, 2, 3 };
  vals[3].Which = 3;
  vals[3].VText = "val";
  const Var::TVar payloads[4] = {
      Var::TVar::Obj(std::unordered_map<std::string, Var::TVar>{}),
      Var::TVar(int64_t(-384)),
      Var::TVar(vector<int64_t>{ 1, 2, 3 }),
      Var::TVar(string("val")) };
  const char *tags[4] = { "Deleted", "Integer", "Numbers", "Text" };
  for (size_t i = 0; i < 4; ++i) {
    const auto expected = Var::TVar::Variant(var_type, tags[i], payloads[i]);
    void *expected_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper expected_state(Var::NewSabot(expected_alloc, expected));
    void *expected_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper expected_type(expected_state->GetType(expected_type_alloc));
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper state(Native::State::New(vals[i], state_alloc));
    void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    Sabot::Type::TAny::TWrapper type(state->GetType(type_alloc));
    EXPECT_EQ(Dump(*state), Dump(*expected_state));
    EXPECT_EQ(Dump(*type), Dump(*expected_type));
    /* The `$which` sentinel reads the record back as the variant type. */
    EXPECT_TRUE(Type::ToType(*type) == var_type);
    EXPECT_TRUE(Var::ToVar(*state) == expected);
  }
}

/* The type of the value, its record type and the type found through
   Native::Type::For all agree, and field names and count are as stored. */
FIXTURE(Type) {
  TShape val;
  val.Which = 1;
  val.VInteger = 7;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Sabot::State::TAny::TWrapper state(Native::State::New(val, state_alloc));
  const auto *rec = dynamic_cast<const Sabot::State::TRecord *>(state.get());
  EXPECT_TRUE(rec);
  EXPECT_EQ(rec->GetElemCount(), 5U);
  void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  Sabot::Type::TRecord::TWrapper rtype(rec->GetRecordType(type_alloc));
  void *pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize());
  Sabot::Type::TRecord::TPin::TWrapper pin(rtype->Pin(pin_alloc));
  EXPECT_EQ(pin->GetElemCount(), 5U);
  const char *names[5] = { "$which", "Deleted", "Integer", "Numbers", "Text" };
  void *elem_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  for (size_t i = 0; i < 5; ++i) {
    string name;
    Sabot::Type::TAny::TWrapper(pin->NewElem(i, name, elem_alloc));
    EXPECT_EQ(name, names[i]);
  }
  bool threw = false;
  try {
    Sabot::Type::TAny::TWrapper(pin->NewElem(5, elem_alloc));
  } catch (const Sabot::TIdxTooBig &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}
