/* <orly/var/mutation.cc>

   Implements <orly/var/mutation.h>

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

#include <orly/var/mutation.h>

#include <orly/rt/mutate.h>
#include <orly/var/new_sabot.h>
#include <orly/var/util.h>
#include <orly/var.h>

using namespace std;
using namespace Orly;
using namespace Orly::Var;

TPtr<TObjChange> TObjChange::New(string key, const TPtr<TChange> &change) {
  return TPtr<TObjChange>(new TObjChange({{key, change}}));
}

/* TODO: The Apply function should be able to be moved into TPartialChange. */
void TObjChange::Apply(TVar &var) const {
   /*
   std::cout << "TObjChange From [";
   void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
   Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, var))->Accept(Sabot::TStateDumper(std::cout));
   std::cout << "]";
   */
  //NOTE: If the assertion fails, there was a type mismatch in the database. Expected an object, didn't get it.
    TObj::TElems elems = var.As<TObj>()->GetVal();
    for(auto &change: GetChanges()) {
      //NOTE: If this throws, then there was a type mismatch in the database. Expected a field, didn't find it.
      change.second->Apply(elems.at(change.first));
    }
    var = TVar::Obj(elems);
    /*
    std::cout << "To [";
    Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, var))->Accept(Sabot::TStateDumper(std::cout));
    std::cout << "]" << std::endl;
    */
}

TObjChange::TObjChange(TChanges &&changes) : TPartialChange(move(changes)) {}


TPtr<TDictChange> TDictChange::New(const Var::TVar &key, const TPtr<TChange> &change) {
  return TPtr<TDictChange>(new TDictChange({{key, change}}));
}

void TDictChange::Apply(TVar &var) const {

  //NOTE: If the assertion fails, there was a type mismatch in the database. Expected an object, didn't get it.
    const TDict *dict = var.As<TDict>();
    TDict::TElems elems = dict->GetVal();
    for(auto &change: GetChanges()) {
      //NOTE: If this throws, then there was a type mismatch in the database. Expected a field, didn't find it.
      change.second->Apply(elems.at(change.first));
    }
    var = TVar::Dict(elems, dict->GetKeyType(), dict->GetValType());
}

TDictChange::TDictChange(TChanges &&changes) : TPartialChange(move(changes)) {}

TPtr<TAddrChange> TAddrChange::New(uint32_t key, const TPtr<TChange> &change) {
  return TPtr<TAddrChange>(new TAddrChange({{key, change}}));
}

void TAddrChange::Apply(TVar &var) const {

  //NOTE: If the assertion fails, there was a type mismatch in the database. Expected an object, didn't get it.
    TAddr::TElems elems = var.As<TAddr>()->GetVal();
    for(auto &change: GetChanges()) {
      //NOTE: If this throws, then there was a type mismatch in the database. Expected a field, didn't find it.
      change.second->Apply(elems.at(change.first).second);
    }
    var = TVar::Addr(elems);
}

TAddrChange::TAddrChange(TChanges &&changes) : TPartialChange(move(changes)) {}

TPtr<TListChange> TListChange::New(uint64_t key, const TPtr<TChange> &change) {
  return TPtr<TListChange>(new TListChange({{key, change}}));
}

void TListChange::Apply(TVar &var) const {

 //NOTE: If the assertion fails, there was a type mismatch in the database. Expected an object, didn't get it.
    const TList *list = var.As<TList>();
    TList::TElems elems = var.As<TList>()->GetVal();
    for(auto &change: GetChanges()) {
      //NOTE: If this throws, then there was a type mismatch in the database. Expected a field, didn't find it.
      change.second->Apply(elems.at(change.first));
    }
    var = TVar::List(elems, list->GetElemType());
}

TListChange::TListChange(TChanges &&changes) : TPartialChange(move(changes)) {}

TPtr<TMutation> TMutation::New(TMutator mutator, const Var::TVar &rhs) {
  return TPtr<TMutation>(new TMutation(mutator, rhs));
}

void TMutation::Apply(Var::TVar &var) const {

  var = Orly::Rt::Mutate(var, Mutator, Rhs);
}

void TMutation::Augment(const TPtr<const TChange> &change) {
  // Combine `change` into `this` so that applying `this` afterwards has
  // the same observable effect as having applied the two changes in
  // sequence. Only valid when both changes are TMutations using the same
  // commutative + associative mutator -- those compose into a single
  // mutation by applying the mutator to their right-hand-sides.
  //
  // For example:  Add(5).Augment(Add(3))   -> Add(8)         (5 + 3)
  //               Or (0xF).Augment(Or(0x10)) -> Or(0x1F)     (0xF | 0x10)
  //               Union({a,b}).Augment(Union({c})) -> Union({a,b,c})
  //
  // Non-commutative or final mutators (Assign, Div, Mod, Exp, Sub) and
  // mixed-mutator pairs (e.g. Add then Mult) still throw -- they can't
  // be safely collapsed into a single mutation without knowing the
  // current value of the target.
  const TMutation *other = dynamic_cast<const TMutation *>(change.get());
  if (!other) {
    throw Rt::TSystemError(HERE, "Conflicting updates to the same key. A partial change cannot augment a full mutation.");
  }
  if (other->Mutator != Mutator) {
    throw Rt::TSystemError(HERE, "Conflicting updates to the same key. Mixed mutators (e.g. += and *=) on the same key cannot be safely collapsed.");
  }
  switch (Mutator) {
    case TMutator::Add:
    case TMutator::Mult:
    case TMutator::And:
    case TMutator::Or:
    case TMutator::Xor:
    case TMutator::Union:
    case TMutator::Intersection:
    case TMutator::SymmetricDiff: {
      // For these: `x OP a; x OP b` is equivalent to `x OP (a OP b)`,
      // so the combined Rhs is `Rt::Mutate(Rhs, Mutator, other->Rhs)`.
      Rhs = Orly::Rt::Mutate(Rhs, Mutator, other->Rhs);
      return;
    }
    case TMutator::Assign: {
      // Assign is `IsFinal()`; per the existing semantics a final
      // mutation shouldn't be augmented at all.
      throw Rt::TSystemError(HERE, "Conflicting updates to the same key. An assignment is final and cannot be augmented.");
    }
    case TMutator::Sub:
    case TMutator::Div:
    case TMutator::Mod:
    case TMutator::Exp: {
      // Sub / Div / Exp can compose with themselves but the combine op
      // differs from the mutator (e.g. two Sub ops compose by *adding*
      // their rhs values, since x - a - b = x - (a + b)). Punted to a
      // future change -- the safe-default here is to throw rather than
      // silently produce a wrong combined value.
      // Mod doesn't compose at all under any reasonable rewrite.
      throw Rt::TSystemError(HERE, "Conflicting updates to the same key. This mutator (Sub / Div / Mod / Exp) does not yet support augmentation.");
    }
  }
  throw Rt::TSystemError(HERE, "Unknown mutator -- TMutation::Augment was not updated when a new TMutator was added.");
}

bool TMutation::IsDelete() const {
  return false;
}

bool TMutation::IsFinal() const {
  return Mutator == TMutator::Assign;
}

TMutation::TMutation(TMutator mutator, const Var::TVar &rhs) : Mutator(mutator), Rhs(rhs) {}

TPtr<TDelete> TDelete::New() {
  return TPtr<TDelete>(new TDelete());
}

void TDelete::Apply(Var::TVar &var) const {
  var = TVar();
}

void TDelete::Augment(const TPtr<const TChange> &) {
  throw Rt::TSystemError(HERE, "Conflicting updates to the same key. Deleted the key, and applied some other change.");
}

bool TDelete::IsDelete() const {
  return true;
}

bool TDelete::IsFinal() const {
  return true;
}

TDelete::TDelete() {}

TPtr<TNew> TNew::New(const Var::TVar &val) {
  return TPtr<TNew>(new TNew(val));
}

void TNew::Apply(Var::TVar &var) const {
  var = Val;
}

void TNew::Augment(const TPtr<const TChange> &) {
  throw Rt::TSystemError(HERE, "Conflicting updates to the same key. Inserted (with new) the key, and applied some other change.");
}

bool TNew::IsDelete() const {
  return false;
}

bool TNew::IsFinal() const {

  return true;
}

TNew::TNew(const Var::TVar &val) : Val(val) {}