/* <orly/code_gen/scope.cc>

   Implements <orly/code_gen/scope.h>

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

#include <orly/code_gen/scope.h>

#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <orly/code_gen/effect.h>
#include <orly/code_gen/interner.h>

using namespace Orly::CodeGen;


TIdScope::TPtr TIdScope::New() {
  return TPtr(new TIdScope());
}

TId<TIdKind::Arg> TIdScope::NewArg() {
  return Arg.New();
}

TId<TIdKind::Func> TIdScope::NewFunc() {
  return Func.New();
}

TId<TIdKind::Var> TIdScope::NewVar() {
  return Var.New();
}

TCodeScope::TCodeScope(const TIdScope::TPtr &id_scope)
    : IdScope(id_scope), Interner(new TInterner(this)), Stmts(new TStmtBlock()) {}

TCodeScope::~TCodeScope() {
  delete Interner;
}

void TCodeScope::AddAssertion(const std::string &name, const TInline::TPtr &assertion) {

  Assertions.push_back(std::make_pair(name, assertion));
}

/* Makes a new id, and adds the given inline to our list of locals. */
void TCodeScope::AddLocal(const TInline::TPtr &inline_) {

  //Fast exit if we already have been common subexpression eliminated.
  if(inline_->HasId()) {
    return;
  }
  //Leaves whose emission is already a name or constant gain nothing from a local.
  if(!inline_->IsCseWorthy()) {
    return;
  }
  /* Re-enabled (#297): Locals arrive in interner-hit order, which can invert dependency order
     (a larger expression can be CSE'd before one of its own subexpressions is), so WriteStart
     emits them in dependency order rather than in this arrival order. */
  inline_->SetCommonSubexpressionId(IdScope->NewVar());
  Locals.push_back(inline_);
}

TIdScope::TPtr TCodeScope::GetIdScope() const {

  return IdScope;
}

TInterner *TCodeScope::GetInterner() const {
  return Interner;
}

const TCodeScope::TLocals &TCodeScope::GetLocals() const {

  return Locals;
}

TStmtBlock &TCodeScope::GetStmts() {

  return *Stmts;
}
const TStmtBlock &TCodeScope::GetStmts() const {

  return *Stmts;
}


TId<TIdKind::Arg> TCodeScope::NewArg() {

  return IdScope->NewArg();
}

/* Locals in dependency order: stable Kahn over the AppendDependsOn edges restricted to this
   scope's own locals, insertion order as the tiebreak (#297).  Arrival order alone is not
   emittable: when a larger expression is CSE'd before one of its own subexpressions, the
   subexpression's definition would land after the local that references it.  A cycle among
   same-scope locals is structurally impossible (the inline graph is an immutable expression
   DAG; call-body edges lead out of the scope), so a detected cycle is a hard error. */
TCodeScope::TLocals TCodeScope::OrderedLocals() const {
  const size_t num_locals = Locals.size();
  std::unordered_map<TInline::TPtr, size_t> index_of;
  for (size_t i = 0; i < num_locals; ++i) {
    index_of.emplace(Locals[i], i);
  }
  /* edges[i] = indices of locals that depend on local i; in_degree[i] = i's own dep count. */
  std::vector<std::vector<size_t>> dependents(num_locals);
  std::vector<size_t> in_degree(num_locals, 0UL);
  for (size_t i = 0; i < num_locals; ++i) {
    std::unordered_set<TInline::TPtr> deps;
    Locals[i]->AppendDependsOn(deps);
    for (const auto &dep : deps) {
      auto iter = index_of.find(dep);
      if (iter != index_of.end() && iter->second != i) {
        dependents[iter->second].push_back(i);
        ++in_degree[i];
      }
    }
  }
  std::set<size_t> ready;  // ordered, so ties resolve to insertion order deterministically
  for (size_t i = 0; i < num_locals; ++i) {
    if (in_degree[i] == 0UL) {
      ready.insert(i);
    }
  }
  TLocals ordered;
  ordered.reserve(num_locals);
  while (!ready.empty()) {
    const size_t cur = *ready.begin();
    ready.erase(ready.begin());
    ordered.push_back(Locals[cur]);
    for (size_t dependent : dependents[cur]) {
      if (--in_degree[dependent] == 0UL) {
        ready.insert(dependent);
      }
    }
  }
  if (ordered.size() != num_locals) {
    throw std::logic_error("TCodeScope::OrderedLocals: dependency cycle among CSE locals");
  }
  return ordered;
}

void TCodeScope::WriteStart(TCppPrinter &out) const {

  for(auto &it: OrderedLocals()) {
    out << "auto " << it->GetId() << " = ";
    it->WriteExpr(out);
    out << ';' << Eol;
  }

  if (!Locals.empty()) {
    out << Eol;
  }

  for(auto &it: Assertions) {
    out << "Orly::Rt::Assert(\"" << it.first << "\", " << it.second << ");" << Eol;
  }

  if(!Assertions.empty()) {
    out << Eol;
  }

  if(!Stmts->IsEmpty()) {
    Stmts->Write(out);
    out << Eol;
  }

}
