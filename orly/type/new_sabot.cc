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
    /* A *value* of a variant serializes byte-identically to a single-key
       record, and that value-level reuse lives in
       orly/var/new_sabot.h's SS::TObj(const Var::TVariant *) adapter,
       which builds a single-field *record* sabot type from the value's
       one active tag. A variant *type* node, however, generally carries
       many tags (e.g. { A | B | C }) and so has no single record shape
       to map to here -- the stored bytes carry no variant discriminator
       and read-side reconstruction is driven by the call-site ::(T)
       annotation (a Phase 1 read-back item still outstanding; see
       orly/var/sabot_to_var.cc). So this type-sabot cell stays a no-op,
       mirroring TErr/TFunc above. */
    virtual void operator()(const TVariant */*type*/) const override {}
    private:
    Sabot::Type::TAny *&Result;
    void *Buf;
  };
  assert(buf);
  Sabot::Type::TAny *result = nullptr;
  type.Accept(visitor_t(result, buf));
  return result;
}
