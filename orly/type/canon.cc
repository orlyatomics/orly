/* <orly/type/canon.cc>

   Implements <orly/type/canon.h>.

   Both walks descend only into CLOSED states: a closed variant's arm,
   unrolled against that variant (orly/type/unroll.h), is itself closed
   (a closed variant has no free self-references, so its arms reference
   only it or variants nested within them). So a single `Unroll` per
   variant level produces the closed child -- no binder stack needed for
   substitution; the stack in `Canon` is only for the fold check.

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

#include <orly/type/canon.h>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include <orly/type.h>
#include <orly/type/unroll.h>

using namespace Orly;
using namespace Orly::Type;

namespace {

  /* A pair of types assumed equal during the coinductive Equiv check,
     keyed by interned-pointer identity (TType::GetHash is the pointer). */
  typedef std::set<std::pair<size_t, size_t>> TAssumed;

  std::pair<size_t, size_t> Key(const TType &s, const TType &t) {
    size_t a = s.GetHash(), b = t.GetHash();
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
  }

  /* The closed child reached by following arm `tag`'s payload of the
     closed variant `v`. */
  TType ArmChild(const TVariant *v, const std::string &tag) {
    return Unroll(v->GetElems().at(tag), v->AsType());
  }

  bool EqHelp(const TType &s, const TType &t, TAssumed &assumed) {
    if (s == t) {
      return true;  // same interned type (covers all leaves: int, str, ...)
    }
    const std::pair<size_t, size_t> key = Key(s, t);
    if (assumed.count(key)) {
      return true;  // coinductive hypothesis
    }

    if (const TVariant *sv = s.TryAs<TVariant>()) {
      const TVariant *tv = t.TryAs<TVariant>();
      if (!tv) { return false; }
      const TVariantElems &se = sv->GetElems();
      const TVariantElems &te = tv->GetElems();
      if (se.size() != te.size()) { return false; }
      for (const auto &p : se) {
        if (!te.count(p.first)) { return false; }
      }
      assumed.insert(key);
      for (const auto &p : se) {
        if (!EqHelp(ArmChild(sv, p.first), ArmChild(tv, p.first), assumed)) {
          return false;
        }
      }
      return true;
    }
    if (const TObj *so = s.TryAs<TObj>()) {
      const TObj *to = t.TryAs<TObj>();
      if (!to) { return false; }
      const TObjElems &se = so->GetElems();
      const TObjElems &te = to->GetElems();
      if (se.size() != te.size()) { return false; }
      for (const auto &p : se) {
        if (!te.count(p.first)) { return false; }
      }
      assumed.insert(key);
      for (const auto &p : se) {
        if (!EqHelp(p.second, te.at(p.first), assumed)) { return false; }
      }
      return true;
    }
    if (const TList *sl = s.TryAs<TList>()) {
      const TList *tl = t.TryAs<TList>();
      if (!tl) { return false; }
      assumed.insert(key);
      return EqHelp(sl->GetElem(), tl->GetElem(), assumed);
    }
    if (const TSet *ss = s.TryAs<TSet>()) {
      const TSet *ts = t.TryAs<TSet>();
      if (!ts) { return false; }
      assumed.insert(key);
      return EqHelp(ss->GetElem(), ts->GetElem(), assumed);
    }
    if (const TOpt *so = s.TryAs<TOpt>()) {
      const TOpt *to = t.TryAs<TOpt>();
      if (!to) { return false; }
      assumed.insert(key);
      return EqHelp(so->GetElem(), to->GetElem(), assumed);
    }
    if (const TDict *sd = s.TryAs<TDict>()) {
      const TDict *td = t.TryAs<TDict>();
      if (!td) { return false; }
      assumed.insert(key);
      return EqHelp(sd->GetKey(), td->GetKey(), assumed)
          && EqHelp(sd->GetVal(), td->GetVal(), assumed);
    }
    /* Other kinds (scalars, ids, self-refs, ...) are leaves: equality is
       interned-pointer identity, already handled at the top. Anything
       reaching here is a kind mismatch or two distinct leaves. */
    return false;
  }

  TType CanonHelp(const TType &s, std::vector<TType> &stack) {
    /* Fold: if s repeats an enclosing binder (a variant we are inside),
       emit a back-reference instead of re-expanding. Innermost binder is
       depth 0. */
    for (size_t d = 0; d < stack.size(); ++d) {
      if (Equiv(s, stack[stack.size() - 1 - d])) {
        return TSelfRef::Get(d);
      }
    }
    if (const TVariant *v = s.TryAs<TVariant>()) {
      stack.push_back(s);
      TVariantElems elems;
      for (const auto &p : v->GetElems()) {
        elems[p.first] = CanonHelp(ArmChild(v, p.first), stack);
      }
      stack.pop_back();
      return TVariant::Get(elems);
    }
    /* Non-binders: recurse into children with the same binder stack and
       no unrolling (a closed state's record fields / container elements
       are themselves closed). */
    if (const TObj *o = s.TryAs<TObj>()) {
      TObjElems elems;
      for (const auto &p : o->GetElems()) {
        elems[p.first] = CanonHelp(p.second, stack);
      }
      return TObj::Get(elems);
    }
    if (const TList *l = s.TryAs<TList>()) {
      return TList::Get(CanonHelp(l->GetElem(), stack));
    }
    if (const TSet *st = s.TryAs<TSet>()) {
      return TSet::Get(CanonHelp(st->GetElem(), stack));
    }
    if (const TOpt *o = s.TryAs<TOpt>()) {
      return TOpt::Get(CanonHelp(o->GetElem(), stack));
    }
    if (const TDict *d = s.TryAs<TDict>()) {
      return TDict::Get(CanonHelp(d->GetKey(), stack), CanonHelp(d->GetVal(), stack));
    }
    return s;  // leaf
  }

}  // namespace

bool Orly::Type::Equiv(const TType &s, const TType &t) {
  TAssumed assumed;
  return EqHelp(s, t, assumed);
}

TType Orly::Type::Canon(const TType &type) {
  std::vector<TType> stack;
  return CanonHelp(type, stack);
}
