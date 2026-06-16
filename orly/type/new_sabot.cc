/* <orly/type/new_sabot.cc>

   Implements <orly/type/new_sabot.h>.

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

#include <orly/type/unroll.h>
#include <orly/type/util.h>

#include <cassert>

using namespace std;
using namespace Orly;

Orly::Type::TTypeTranslationError::TTypeTranslationError()
    : logic_error("could not translate from orly type to sabot type") {}

Sabot::Type::TAny *Orly::Type::NewSabot(void *buf, const TType &type) {
  auto result = TryNewSabot(buf, type);
  if (!result) {
    throw TTypeTranslationError();
  }
  return result;
}

Sabot::Type::TAny *Orly::Type::TryNewSabot(void *buf, const Type::TType &type) {
  class visitor_t final : public Type::TType::TVisitor {
    public:
    visitor_t(Sabot::Type::TAny *&result, void *buf) : Result(result), Buf(buf) {}
    // no translation
    virtual void operator()(const TAny      */*type*/) const override {}
    virtual void operator()(const TErr      */*type*/) const override {}
    virtual void operator()(const TFunc     */*type*/) const override {}
    virtual void operator()(const TSeq      */*type*/) const override {}
    // nullary
    virtual void operator()(const TBool     */*type*/) const override { Result = new (Buf) Sabot::Type::TBool(); }
    virtual void operator()(const TId       */*type*/) const override { Result = new (Buf) Sabot::Type::TUuid(); }
    virtual void operator()(const TInt      */*type*/) const override { Result = new (Buf) Sabot::Type::TInt64(); }
    virtual void operator()(const TReal     */*type*/) const override { Result = new (Buf) Sabot::Type::TDouble(); }
    virtual void operator()(const TStr      */*type*/) const override { Result = new (Buf) Sabot::Type::TStr(); }
    virtual void operator()(const TTimeDiff */*type*/) const override { Result = new (Buf) Sabot::Type::TDuration(); }
    virtual void operator()(const TTimePnt  */*type*/) const override { Result = new (Buf) Sabot::Type::TTimePoint(); }
    // unary and binary
    virtual void operator()(const TMutable *type) const override {
      Result = TryNewSabot(Buf, type->GetVal());
    }
    virtual void operator()(const TList *type) const override {
      Result = new (Buf) ST::TVector(type->GetElem());
    }
    virtual void operator()(const TOpt *type) const override {
      Result = new (Buf) ST::TOpt(type->GetElem());
    }
    virtual void operator()(const TSet *type) const override {
      Result = new (Buf) ST::TSet(type->GetElem());
    }
    virtual void operator()(const TDict *type) const override {
      Result = new (Buf) ST::TMap(type->GetKey(), type->GetVal());
    }
    // n-ary
    virtual void operator()(const TAddr *type) const override {
      Result = new (Buf) ST::TTuple(type);
    }
    virtual void operator()(const TObj *type) const override {
      Result = new (Buf) ST::TRecord(type);
    }
    /* A variant serializes (both value and type) as the fixed-shape record
         <{ .$which:int, .Tag0:payload0?, .Tag1:payload1?, ... }>
       (issue #96) -- a discriminant plus one optional payload field per arm.
       The variant TYPE therefore maps to that record type, kept in lock-step
       with the value encoder SS::TObj(const Var::TVariant *) in
       orly/var/new_sabot.h. This is what gives a stored SET of differently-
       tagged variants a single, homogeneous element type; sabot_to_type
       reverses the mapping (the `$which` sentinel field, un-expressible in
       orlyscript, marks the record as a variant). */
    virtual void operator()(const TVariant *type) const override {
      /* A self-recursive variant (issue #103/#115) encodes to the same #96
         fixed-shape record; the cycle is broken by emitting a finite
         Sabot::Type::TSelfRef leaf at each recursion point (the TSelfRef case
         below), so the record is no longer infinitely deep.

         A MUTUAL-group variant (its arms carry Type::TGroupRef, #116) is not
         yet storable: lowering it would require inlining the group to its de
         Bruijn form first. Those still return no translation (the TGroupRef
         case below), so MakeRecGroup members keep failing translation cleanly
         until that follow-on lands. */
      if (HasGroupRef(type->AsType())) {
        return;
      }
      TObjElems rec;
      rec["$which"] = TInt::Get();
      for (const auto &arm: type->GetElems()) {
        rec[arm.first] = TOpt::Get(arm.second);
      }
      Result = new (Buf) ST::TRecord(TObj::Get(rec).As<TObj>());
    }
    /* The recursion point of a self-recursive variant (issue #115): emit the
       finite sabot back-reference leaf carrying the de Bruijn depth.  This is
       what stops the fixed-shape record encoding from being infinitely deep. */
    virtual void operator()(const TSelfRef *type) const override {
      Result = new (Buf) Sabot::Type::TSelfRef(type->GetDepth());
    }
    /* A mutual group's cross/self reference (#116) -- not yet translatable;
       see the TVariant case above. */
    virtual void operator()(const TGroupRef */*type*/) const override {}
    private:
    Sabot::Type::TAny *&Result;
    void *Buf;
  };
  assert(buf);
  Sabot::Type::TAny *result = nullptr;
  type.Accept(visitor_t(result, buf));
  return result;
}
