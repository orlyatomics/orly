/* <orly/type/rec_group.cc>

   Implements <orly/type/rec_group.h>.

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

#include <orly/type/rec_group.h>

#include <algorithm>
#include <cassert>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>

#include <base/hash.h>
#include <orly/type.h>
#include <orly/type/canon.h>

using namespace std;
using namespace Orly;
using namespace Orly::Type;

namespace {

  /* The registry: a group's canonical identity -> its member types, in
     canonical (group-identity) order. Populated by MakeRecGroup, read by
     ResolveGroupRef. Single-process compiler; isomorphic groups share an
     entry because the identity is canonical. */
  unordered_map<TGroupId, vector<TType>> &Registry() {
    static unordered_map<TGroupId, vector<TType>> registry;
    return registry;
  }

  /* Reverse map: a member type -> (its group identity, its canonical index),
     so codegen can recover a member's whole group from the member alone.
     Each interned member type is canonical and thus unique to one group. */
  unordered_map<TType, pair<TGroupId, size_t>> &MemberIndex() {
    static unordered_map<TType, pair<TGroupId, size_t>> member_index;
    return member_index;
  }

  /* A placeholder sibling reference in an input equation: TGroupRef with
     the empty (not-yet-known) group identity, carrying only the member
     index. */
  const TGroupRef *AsPlaceholder(const TType &t) {
    const TGroupRef *gr = t.TryAs<TGroupRef>();
    return (gr && gr->GetGroup().empty()) ? gr : nullptr;
  }

  /* --- Inlining: a member's de Bruijn form (siblings expanded, back-edges
         minted as TSelfRef), used only to compute the canonical identity. --- */

  TType InlineType(const TType &t, const vector<TVariantElems> &members,
                   size_t cur_depth, map<size_t, size_t> &binder_depth);

  /* Inline a variant whose binder sits at `depth`. */
  TType InlineVariant(const TVariantElems &elems, const vector<TVariantElems> &members,
                      size_t depth, map<size_t, size_t> &binder_depth) {
    TVariantElems out;
    for (const auto &arm : elems) {
      out[arm.first] = InlineType(arm.second, members, depth, binder_depth);
    }
    return TVariant::Get(out);
  }

  TType InlineType(const TType &t, const vector<TVariantElems> &members,
                   size_t cur_depth, map<size_t, size_t> &binder_depth) {
    if (const TGroupRef *gr = AsPlaceholder(t)) {
      size_t m = gr->GetIndex();
      auto it = binder_depth.find(m);
      if (it != binder_depth.end()) {
        return TSelfRef::Get(cur_depth - it->second);  // back-edge
      }
      /* Expand member m: entering its variant adds a binder. */
      binder_depth[m] = cur_depth + 1;
      TType result = InlineVariant(members[m], members, cur_depth + 1, binder_depth);
      binder_depth.erase(m);
      return result;
    }
    if (const TVariant *v = t.TryAs<TVariant>()) {
      return InlineVariant(v->GetElems(), members, cur_depth + 1, binder_depth);
    }
    if (const TObj *o = t.TryAs<TObj>()) {
      TObjElems out;
      for (const auto &f : o->GetElems()) {
        out[f.first] = InlineType(f.second, members, cur_depth, binder_depth);
      }
      return TObj::Get(out);
    }
    if (const TList *l = t.TryAs<TList>()) {
      return TList::Get(InlineType(l->GetElem(), members, cur_depth, binder_depth));
    }
    if (const TSet *s = t.TryAs<TSet>()) {
      return TSet::Get(InlineType(s->GetElem(), members, cur_depth, binder_depth));
    }
    if (const TOpt *o = t.TryAs<TOpt>()) {
      return TOpt::Get(InlineType(o->GetElem(), members, cur_depth, binder_depth));
    }
    if (const TDict *d = t.TryAs<TDict>()) {
      return TDict::Get(InlineType(d->GetKey(), members, cur_depth, binder_depth),
                        InlineType(d->GetVal(), members, cur_depth, binder_depth));
    }
    return t;  // scalar / intra-member TSelfRef -- copied as-is
  }

  /* Member k's canonical inlined form (its de Bruijn type, minimized). */
  TType MemberCanonInlined(const vector<TVariantElems> &members, size_t k) {
    map<size_t, size_t> binder_depth{{k, 1}};
    return Canon(InlineVariant(members[k], members, 1, binder_depth));
  }

  /* --- Reindexing: replace placeholder sibling refs with real TGroupRefs
         into the canonical group identity. --- */

  TType Reindex(const TType &t, const TGroupId &gid, const vector<size_t> &pos) {
    if (const TGroupRef *gr = AsPlaceholder(t)) {
      return TGroupRef::Get(gid, pos[gr->GetIndex()]);
    }
    if (const TVariant *v = t.TryAs<TVariant>()) {
      TVariantElems out;
      for (const auto &arm : v->GetElems()) {
        out[arm.first] = Reindex(arm.second, gid, pos);
      }
      return TVariant::Get(out);
    }
    if (const TObj *o = t.TryAs<TObj>()) {
      TObjElems out;
      for (const auto &f : o->GetElems()) {
        out[f.first] = Reindex(f.second, gid, pos);
      }
      return TObj::Get(out);
    }
    if (const TList *l = t.TryAs<TList>()) {
      return TList::Get(Reindex(l->GetElem(), gid, pos));
    }
    if (const TSet *s = t.TryAs<TSet>()) {
      return TSet::Get(Reindex(s->GetElem(), gid, pos));
    }
    if (const TOpt *o = t.TryAs<TOpt>()) {
      return TOpt::Get(Reindex(o->GetElem(), gid, pos));
    }
    if (const TDict *d = t.TryAs<TDict>()) {
      return TDict::Get(Reindex(d->GetKey(), gid, pos), Reindex(d->GetVal(), gid, pos));
    }
    return t;
  }

}  // namespace

vector<TType> Orly::Type::MakeRecGroup(const vector<TVariantElems> &members) {
  const size_t n = members.size();
  assert(n > 0);

  /* 1. Each member's canonical inlined form. */
  vector<TType> canon_inlined(n);
  for (size_t i = 0; i < n; ++i) {
    canon_inlined[i] = MemberCanonInlined(members, i);
  }

  /* 2. Canonical order: sort member indices by their inlined form's mangled
     name (structural and run-stable), so isomorphic groups -- regardless of
     the order their members are supplied -- yield the same identity. */
  vector<size_t> order(n);
  iota(order.begin(), order.end(), 0);
  sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return canon_inlined[a].GetMangledName() < canon_inlined[b].GetMangledName();
  });

  /* group identity = inlined forms in canonical order; pos[k] = member k's
     position in that order. */
  TGroupId gid(n);
  vector<size_t> pos(n);
  for (size_t r = 0; r < n; ++r) {
    gid[r] = canon_inlined[order[r]];
    pos[order[r]] = r;
  }

  /* 3. Member types: real TGroupRefs into the canonical identity. */
  vector<TType> result(n);
  for (size_t i = 0; i < n; ++i) {
    TVariantElems arms;
    for (const auto &arm : members[i]) {
      arms[arm.first] = Reindex(arm.second, gid, pos);
    }
    result[i] = TVariant::Get(arms);
  }

  /* 4. Register, indexed by canonical position, for navigation. */
  vector<TType> by_pos(n);
  for (size_t i = 0; i < n; ++i) {
    by_pos[pos[i]] = result[i];
  }
  Registry()[gid] = by_pos;
  for (size_t r = 0; r < n; ++r) {
    MemberIndex()[by_pos[r]] = {gid, r};
  }

  return result;
}

TType Orly::Type::ResolveGroupRef(const TGroupRef *group_ref) {
  assert(group_ref);
  const auto it = Registry().find(group_ref->GetGroup());
  assert(it != Registry().end());
  assert(group_ref->GetIndex() < it->second.size());
  return it->second[group_ref->GetIndex()];
}

bool Orly::Type::TryGetGroupMembers(const TType &member, vector<TType> &members,
                                    size_t &index) {
  const auto it = MemberIndex().find(member);
  if (it == MemberIndex().end()) {
    return false;
  }
  members = Registry().at(it->second.first);
  index = it->second.second;
  return true;
}
