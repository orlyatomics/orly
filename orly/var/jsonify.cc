/* <orly/var/jsonify.cc>

   Implements <orly/var/jsonify.h>.

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

#include <cmath>
#include <sstream>

#include <base/json.h>
#include <base/not_implemented.h>
#include <orly/type/orlyify.h>
#include <orly/var.h>
#include <orly/var/sabot_to_var.h>

using namespace std;
using namespace Orly;
using namespace Orly::Var;

void Orly::Var::Jsonify(ostream &strm, const TVar &var) {
  class TJsonifyVisitor : public TVar::TVisitor {
    NO_COPY(TJsonifyVisitor);
    public:
    TJsonifyVisitor(ostream &strm) : Strm(strm) {}
    private:
    virtual void operator()(const TAddr *that) const {
      Strm << '[';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        iter.second.Accept(*this);
        ++num_args;
      }
      Strm << ']';
    }
    virtual void operator()(const TBool *that) const {
      Strm << boolalpha << that->GetVal();
    }
    virtual void operator()(const TDict *that) const {
      Strm << '{';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }

        // We're actually encoding a JSON Object here, since there is no support
        // for dictionaries.  So, if the key is not a string we render its JSON
        // form and emit that as a (properly escaped) string -- bare quotes
        // around a composite key that itself contains strings would produce
        // nested unescaped quotes, i.e. invalid JSON (#276).
        if(iter.first.GetType() != Type::TStr::Get()) {
          ostringstream key_strm;
          iter.first.Accept(TJsonifyVisitor(key_strm));
          Base::TJson::WriteString(Strm, key_strm.str());
        }
        else {
          iter.first.Accept(*this);
        }
        Strm << ':';
        iter.second.Accept(*this);
        ++num_args;
      }
      Strm << '}';
    }
    virtual void operator()(const TErr *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TFree *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TId *that) const {
      Strm << '"' << that->GetVal() << '"';
    }
    virtual void operator()(const TInt *that) const {
      Strm << that->GetVal();
    }
    virtual void operator()(const TList *that) const {
      Strm << '[';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        iter.Accept(*this);
        ++num_args;
      }
      Strm << ']';
    }
    virtual void operator()(const TMutable *that) const {
      that->GetVal().Accept(*this);
    }
    virtual void operator()(const TObj *that) const {
      Strm << '{';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        Strm << '"' << iter.first << "\":";
        iter.second.Accept(*this);
        ++num_args;
      }
      Strm << '}';
    }
    virtual void operator()(const TOpt *that) const {
      if (that->GetVal().IsKnown()) {
        that->GetVal().GetVal().Accept(*this);
      } else {
        Strm << "null";
      }
    }
    virtual void operator()(const TReal *that) const {
      /* JSON has no NaN / Infinity literals; the conventional lenient
         mapping is null (#276). */
      if (isfinite(that->GetVal())) {
        Strm << showpoint << that->GetVal();
      } else {
        Strm << "null";
      }
    }
    virtual void operator()(const TSet *that) const {
      Strm << '[';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        iter.Accept(*this);
        ++num_args;
      }
      Strm << ']';
    }
    virtual void operator()(const TStr *that) const {
      Base::TJson::WriteString(Strm, that->GetVal());
    }
    virtual void operator()(const TTimeDiff *that) const {
      Strm << that->GetVal().count();
    }
    virtual void operator()(const TTimePnt *that) const {
      Strm << Base::Chrono::TimeDiffCast(that->GetVal().time_since_epoch()).count();
    }
    virtual void operator()(const TVariant *that) const {
      /* A variant value serializes as the single-key object
         {"<tag>": <payload>}, byte-consistent with the single-key-record
         storage reuse described in issue #95. */
      Strm << "{\"" << that->GetTag() << "\":";
      that->GetVal().Accept(*this);
      Strm << '}';
    }
    ostream &Strm;
  };
  var.Accept(TJsonifyVisitor(strm));
}

Base::TJson Orly::Var::ToJson(const TVar &var) {
  using TJson = Base::TJson;
  class TToJsonVisitor : public TVar::TVisitor {
    NO_COPY(TToJsonVisitor);
    public:
    TToJsonVisitor(TJson &out) : Out(out) {}
    private:
    virtual void operator()(const TAddr *that) const {
      TJson::TArray arr;
      for (const auto &iter : that->GetVal()) {
        arr.emplace_back(ToJson(iter.second));
      }
      Out = TJson(move(arr));
    }
    virtual void operator()(const TBool *that) const {
      Out = TJson(that->GetVal());
    }
    virtual void operator()(const TDict *that) const {
      TJson::TObject obj;
      for (const auto &iter : that->GetVal()) {
        /* Same key convention as Jsonify (which the wire format inherited
           through the old parse round-trip): a string key is used verbatim;
           any other key type appears as the compact JSON text of the key
           (#276, #377). */
        if (iter.first.GetType() != Type::TStr::Get()) {
          ostringstream key_strm;
          Jsonify(key_strm, iter.first);
          obj[key_strm.str()] = ToJson(iter.second);
        } else {
          obj[ToJson(iter.first).GetString()] = ToJson(iter.second);
        }
      }
      Out = TJson(move(obj));
    }
    virtual void operator()(const TErr *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TFree *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TId *that) const {
      char buf[Base::TUuid::MinBufSize];
      that->GetVal().Format(buf);
      Out = TJson(string(buf));
    }
    virtual void operator()(const TInt *that) const {
      Out = TJson(that->GetVal());
    }
    virtual void operator()(const TList *that) const {
      TJson::TArray arr;
      for (const auto &iter : that->GetVal()) {
        arr.emplace_back(ToJson(iter));
      }
      Out = TJson(move(arr));
    }
    virtual void operator()(const TMutable *that) const {
      Out = ToJson(that->GetVal());
    }
    virtual void operator()(const TObj *that) const {
      TJson::TObject obj;
      for (const auto &iter : that->GetVal()) {
        obj[iter.first] = ToJson(iter.second);
      }
      Out = TJson(move(obj));
    }
    virtual void operator()(const TOpt *that) const {
      if (that->GetVal().IsKnown()) {
        Out = ToJson(that->GetVal().GetVal());
      } else {
        Out = TJson();
      }
    }
    virtual void operator()(const TReal *that) const {
      /* JSON has no NaN / Infinity literals; the conventional lenient
         mapping is null (#276). */
      if (isfinite(that->GetVal())) {
        Out = TJson(that->GetVal());
      } else {
        Out = TJson();
      }
    }
    virtual void operator()(const TSet *that) const {
      TJson::TArray arr;
      for (const auto &iter : that->GetVal()) {
        arr.emplace_back(ToJson(iter));
      }
      Out = TJson(move(arr));
    }
    virtual void operator()(const TStr *that) const {
      Out = TJson(that->GetVal());
    }
    virtual void operator()(const TTimeDiff *that) const {
      Out = TJson(that->GetVal().count());
    }
    virtual void operator()(const TTimePnt *that) const {
      Out = TJson(Base::Chrono::TimeDiffCast(that->GetVal().time_since_epoch()).count());
    }
    virtual void operator()(const TVariant *that) const {
      /* A variant value serializes as the single-key object
         {"<tag>": <payload>}, byte-consistent with the single-key-record
         storage reuse described in issue #95. */
      TJson::TObject obj;
      obj[that->GetTag()] = ToJson(that->GetVal());
      Out = TJson(move(obj));
    }
    TJson &Out;
  };
  TJson result;
  var.Accept(TToJsonVisitor(result));
  return result;
}

namespace {

  /* See Orly::Var::StateToJson.  Each direct case mirrors what ToVar builds for the state and what
     ToJson then makes of that var. */
  class TStateToJsonVisitor final
      : public Sabot::TStateVisitor {
    public:

    TStateToJsonVisitor(Base::TJson &out, bool in_tuple = false)
        : Out(out), InTuple(in_tuple) {}

    void operator()(const Sabot::State::TFree &state) const override       { Fallback(state); }
    void operator()(const Sabot::State::TTombstone &state) const override  { Fallback(state); }
    void operator()(const Sabot::State::TVoid &state) const override       { Fallback(state); }
    void operator()(const Sabot::State::TInt8 &state) const override       { Fallback(state); }
    void operator()(const Sabot::State::TInt16 &state) const override      { Fallback(state); }
    void operator()(const Sabot::State::TInt32 &state) const override      { Fallback(state); }
    void operator()(const Sabot::State::TInt64 &state) const override      { Out = Base::TJson(state.Get()); }
    void operator()(const Sabot::State::TUInt8 &state) const override      { Fallback(state); }
    void operator()(const Sabot::State::TUInt16 &state) const override     { Fallback(state); }
    void operator()(const Sabot::State::TUInt32 &state) const override     { Fallback(state); }
    void operator()(const Sabot::State::TUInt64 &state) const override     { Fallback(state); }
    void operator()(const Sabot::State::TBool &state) const override       { Out = Base::TJson(state.Get()); }
    void operator()(const Sabot::State::TChar &state) const override       { Fallback(state); }
    void operator()(const Sabot::State::TFloat &state) const override      { Fallback(state); }
    void operator()(const Sabot::State::TDouble &state) const override {
      /* As ToJson(TReal): JSON has no NaN / Infinity, so those are null (#276). */
      const double val = state.Get();
      Out = isfinite(val) ? Base::TJson(val) : Base::TJson();
    }
    void operator()(const Sabot::State::TDuration &state) const override {
      Out = Base::TJson(std::chrono::duration_cast<Base::Chrono::TTimeDiff>(state.Get()).count());
    }
    void operator()(const Sabot::State::TTimePoint &state) const override {
      Out = Base::TJson(std::chrono::duration_cast<Base::Chrono::TTimeDiff>(state.Get().time_since_epoch()).count());
    }
    void operator()(const Sabot::State::TUuid &state) const override {
      char buf[Base::TUuid::MinBufSize];
      Base::TUuid(state.Get().GetRaw()).Format(buf);
      Out = Base::TJson(string(buf));
    }
    void operator()(const Sabot::State::TBlob &state) const override       { Fallback(state); }
    void operator()(const Sabot::State::TStr &state) const override {
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      Sabot::State::TStr::TPin::TWrapper pin(state.Pin(pin_alloc));
      Out = Base::TJson(string(pin->GetStart(), pin->GetSize()));
    }
    void operator()(const Sabot::State::TDesc &state) const override {
      /* ToVar takes a desc only as an address member, where ToJson drops the direction. */
      if (!InTuple) {
        Fallback(state);
        return;
      }
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TDesc::TPin::TWrapper pin(state.Pin(pin_alloc));
      Out = Var::StateToJson(*Sabot::State::TAny::TWrapper(pin->NewElem(0, state_alloc)));
    }
    void operator()(const Sabot::State::TOpt &state) const override {
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TOpt::TPin::TWrapper pin(state.Pin(pin_alloc));
      if (!pin->GetElemCount()) {
        Out = Base::TJson();
        return;
      }
      Out = Var::StateToJson(*Sabot::State::TAny::TWrapper(pin->NewElem(0, state_alloc)));
    }
    void operator()(const Sabot::State::TSet &state) const override {
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TSet::TPin::TWrapper pin(state.Pin(pin_alloc));
      const size_t elem_count = pin->GetElemCount();
      Base::TJson::TArray arr;
      arr.reserve(elem_count);
      for (size_t elem_idx = 0; elem_idx < elem_count; ++elem_idx) {
        arr.emplace_back(Var::StateToJson(*Sabot::State::TAny::TWrapper(pin->NewElem(elem_idx, state_alloc))));
      }
      Out = Base::TJson(std::move(arr));
    }
    void operator()(const Sabot::State::TVector &state) const override {
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TVector::TPin::TWrapper pin(state.Pin(pin_alloc));
      const size_t elem_count = pin->GetElemCount();
      Base::TJson::TArray arr;
      arr.reserve(elem_count);
      for (size_t elem_idx = 0; elem_idx < elem_count; ++elem_idx) {
        arr.emplace_back(Var::StateToJson(*Sabot::State::TAny::TWrapper(pin->NewElem(elem_idx, state_alloc))));
      }
      Out = Base::TJson(std::move(arr));
    }
    void operator()(const Sabot::State::TMap &state) const override        { Fallback(state); }
    void operator()(const Sabot::State::TRecord &state) const override {
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      void *type_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize());
      void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      void *elem_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
      Sabot::State::TRecord::TPin::TWrapper pin(state.Pin(pin_alloc));
      Sabot::Type::TRecord::TWrapper record_type(state.GetRecordType(type_alloc));
      Sabot::Type::TRecord::TPin::TWrapper type_pin(record_type->Pin(type_pin_alloc));
      const size_t elem_count = pin->GetElemCount();
      string field_name;
      if (elem_count) {
        Sabot::Type::TAny::TWrapper(type_pin->NewElem(0, field_name, elem_type_alloc));
        /* A variant value (#96) is stored as <{ .$which: int, .Tag0: payload0?, ... }>, with
           fields in asciibetical order so "$which" is field 0.  Emit {"<active_tag>": <payload>}
           directly from the active arm without decoding the inactive arms. */
        if (field_name == "$which") {
          const Sabot::State::TAny::TWrapper which_state(pin->NewElem(0, state_alloc));
          const auto *which_int = dynamic_cast<const Sabot::State::TInt64 *>(which_state.get());
          if (!which_int) {
            Fallback(state);
            return;
          }
          const int64_t which = which_int->Get();
          if (which < 0 || static_cast<size_t>(which) + 1 >= elem_count) {
            Fallback(state);
            return;
          }
          const size_t arm_idx = static_cast<size_t>(which) + 1;
          Sabot::Type::TAny::TWrapper(type_pin->NewElem(arm_idx, field_name, elem_type_alloc));
          const Sabot::State::TAny::TWrapper arm_state(pin->NewElem(arm_idx, state_alloc));
          const auto *arm_opt = dynamic_cast<const Sabot::State::TOpt *>(arm_state.get());
          if (!arm_opt) {
            Fallback(state);
            return;
          }
          void *opt_pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
          void *payload_alloc = alloca(Sabot::State::GetMaxStateSize());
          Sabot::State::TOpt::TPin::TWrapper opt_pin(arm_opt->Pin(opt_pin_alloc));
          if (opt_pin->GetElemCount() != 1) {
            Fallback(state);
            return;
          }
          Base::TJson::TObject obj;
          obj.emplace(std::move(field_name),
                      Var::StateToJson(*Sabot::State::TAny::TWrapper(opt_pin->NewElem(0, payload_alloc))));
          Out = Base::TJson(std::move(obj));
          return;
        }
      }
      Base::TJson::TObject obj;
      for (size_t elem_idx = 0; elem_idx < elem_count; ++elem_idx) {
        if (elem_idx) {
          Sabot::Type::TAny::TWrapper(type_pin->NewElem(elem_idx, field_name, elem_type_alloc));
        }
        obj.emplace(std::move(field_name),
                    Var::StateToJson(*Sabot::State::TAny::TWrapper(pin->NewElem(elem_idx, state_alloc))));
      }
      Out = Base::TJson(std::move(obj));
    }
    void operator()(const Sabot::State::TTuple &state) const override {
      void *pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TTuple::TPin::TWrapper pin(state.Pin(pin_alloc));
      const size_t elem_count = pin->GetElemCount();
      Base::TJson::TArray arr(elem_count);
      for (size_t elem_idx = 0; elem_idx < elem_count; ++elem_idx) {
        Sabot::State::TAny::TWrapper(pin->NewElem(elem_idx, state_alloc))->Accept(TStateToJsonVisitor(arr[elem_idx], true));
      }
      Out = Base::TJson(std::move(arr));
    }

    private:

    void Fallback(const Sabot::State::TAny &state) const {
      Out = Var::ToJson(Var::ToVar(state));
    }

    Base::TJson &Out;

    /* True for a member of an address, where a desc may appear. */
    const bool InTuple;

  };  // TStateToJsonVisitor

}  // namespace

Base::TJson Orly::Var::StateToJson(const Sabot::State::TAny &state) {
  Base::TJson result;
  state.Accept(TStateToJsonVisitor(result));
  return result;
}
