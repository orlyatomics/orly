/* <orly/synth/postfix_cast.cc>

   Implements <orly/synth/postfix_cast.h>.

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

#include <orly/synth/postfix_cast.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <base/as_str.h>
#include <base/assert_true.h>
#include <base/hash.h>
#include <orly/expr/addr.h>
#include <orly/expr/addr_member.h>
#include <orly/expr/as.h>
#include <orly/expr/function_app.h>
#include <orly/expr/if_else.h>
#include <orly/expr/known.h>
#include <orly/expr/obj.h>
#include <orly/expr/obj_member.h>
#include <orly/expr/ref.h>
#include <orly/expr/sequence_of.h>
#include <orly/expr/unknown.h>
#include <orly/expr/variant.h>
#include <orly/expr/walker.h>
#include <orly/expr/when.h>
#include <orly/symbol/function.h>
#include <orly/symbol/given_param_def.h>
#include <orly/symbol/result_def.h>
#include <orly/symbol/test/test.h>
#include <orly/symbol/test/test_case_block.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/new_type.h>
#include <orly/type/addr.h>
#include <orly/type/dict.h>
#include <orly/type/impl.h>
#include <orly/type/list.h>
#include <orly/type/obj.h>
#include <orly/type/opt.h>
#include <orly/type/rec_group.h>
#include <orly/type/seq.h>
#include <orly/type/set.h>
#include <orly/type/unroll.h>
#include <orly/type/unwrap.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Synth;

namespace {

  /* True iff `v` is a recursive variant (some arm payload contains a free
     self-reference or a group reference) -- mirrors the recursion test in
     orly/expr/as.cc. */
  bool IsRecursiveVariant(const Type::TVariant *v) {
    for (const auto &arm : v->GetElems()) {
      if (Type::HasFreeSelfRef(arm.second) || Type::HasGroupRef(arm.second)) {
        return true;
      }
    }
    return false;
  }

  /* Synthesizes the body of a recursive widening fold (#104). Given the
     source (narrow) and target (wide) recursive variant types, it produces
     the expression that converts a value of the narrow type into the wide
     one, applying a recursive `widen(.t: ...)` call at each recursion point.

     The fold is exactly the one a user writes by hand (which works since
     #157):
       widen = ((t) when {
         Leaf(n):   wide.Leaf(n);
         Branch(b): wide.Branch(<{.l: widen(.t: b.l), .r: widen(.t: b.r)}>);
       }) where { t = given::(narrow); };
     so type-check, recursion resolution, and codegen are all already proven;
     we only assemble the Expr/Symbol tree it lowers to -- as a TOP-LEVEL
     package function, because recursion is only supported for top-level
     functions (an inline / where-bound recursive function is rejected). */
  struct TWidenSynth {
    /* The widening's member correspondence: each narrow (member) variant type
       mapped to the wide variant it widens to. A single self-recursive def has
       one entry (`narrow -> wide`); a mutually-recursive GROUP (#104) has one
       per member. A subterm typed as a narrow member -- after Unroll resolves
       its self-/group-edges -- is a recursion point: it widens to the
       corresponding wide member via a call to that member's fold. */
    std::unordered_map<Type::TType, Type::TType> Corr;
    /* Each narrow member's synthesized fold result def -- the callee of a
       recursive call into that member. Empty while pre-flighting CanRebuild(). */
    std::unordered_map<Type::TType, Symbol::TResultDef::TPtr> Folds;
    TPosRange PosRange;
    /* For container-of-self payloads only: the package scope that minted
       helpers are added to, the base name for them, and two per-fold caches:
       PairHelpers (keyed by a dict's key type) for the helper that rebuilds
       one key/value pair, and ElemHelpers (keyed by a list/set element type)
       for the helper that rebuilds one element whose own shape is not the
       bare recursion point (a NESTED container of self). All null/empty while
       pre-flighting with CanRebuild(). */
    Symbol::TScope::TPtr PackageScope;
    std::unordered_map<Type::TType, Symbol::TResultDef::TPtr> *PairHelpers;
    std::unordered_map<Type::TType, Symbol::TResultDef::TPtr> *ElemHelpers;
    std::string NameStem;

    /* True iff Rebuild() can convert a value of narrow subterm type `src`
       into wide type `dst`. A pure structural pre-flight (no Expr built, no
       function minted) so an unsupported payload shape is left for the plain
       `as` cast -- whose type-checker emits the canonical "widening of
       recursive variants is not yet supported (#104)" diagnostic. */
    bool CanRebuild(const Type::TType &src, const Type::TType &dst) const {
      /* Identical subterm (interned): no recursion inside -- passes through. */
      if (src == dst) {
        return true;
      }
      /* The recursion point: a narrow member widens to its wide correspondent
         via a recursive call into that member's fold. */
      auto corr = Corr.find(src);
      if (corr != Corr.end() && corr->second == dst) {
        return true;
      }
      /* Record of self: rebuildable iff every field is. */
      if (const auto *src_obj = src.TryAs<Type::TObj>()) {
        const auto *dst_obj = dst.TryAs<Type::TObj>();
        if (!dst_obj) {
          return false;
        }
        const auto &dst_elems = dst_obj->GetElems();
        for (const auto &field : src_obj->GetElems()) {
          auto dst_field = dst_elems.find(field.first);
          if (dst_field == dst_elems.end() || !CanRebuild(field.second, dst_field->second)) {
            return false;
          }
        }
        return true;
      }
      /* Optional of self (`Node(self?)`, or `self?` as a record field):
         rebuildable iff the optional's element is. The element is a recursion
         point (the only self-placement under an optional that #116 permits),
         reached by unwrapping the known case (see Rebuild). */
      if (const auto *src_opt = src.TryAs<Type::TOpt>()) {
        const auto *dst_opt = dst.TryAs<Type::TOpt>();
        return dst_opt && CanRebuild(src_opt->GetElem(), dst_opt->GetElem());
      }
      /* Nested variant of self (a payload that is itself a variant carrying a
         self-reference, e.g. `Node(<| Some(self) | None |>)`). The outer
         Unroll has already turned its self-edges into the narrow variant, so
         it is a payload-widening: identical tag set, each arm's payload
         rebuilt (Some's becomes the recursion point). A nested variant that is
         itself recursive would need its own fold -- defer it. */
      if (const auto *src_var = src.TryAs<Type::TVariant>()) {
        const auto *dst_var = dst.TryAs<Type::TVariant>();
        if (!dst_var || IsRecursiveVariant(src_var) || IsRecursiveVariant(dst_var)) {
          return false;
        }
        const auto &dst_elems = dst_var->GetElems();
        if (src_var->GetElems().size() != dst_elems.size()) {
          return false;
        }
        for (const auto &arm : src_var->GetElems()) {
          auto dst_arm = dst_elems.find(arm.first);
          if (dst_arm == dst_elems.end() || !CanRebuild(arm.second, dst_arm->second)) {
            return false;
          }
        }
        return true;
      }
      /* List / set of self (`Node([self])`, `Node({self})`): rebuildable iff
         the element is. The fold maps a per-element widening over the elements
         (`f(.t: **xs) as wide-container`, the implicit element-wise map orly
         applies when a function is handed a sequence) and collects the result.
         When the element is the recursion point the map function is the fold
         itself; when it is a NESTED container of self (`[[self]]`) a per-element
         helper rebuilds it -- see Rebuild / GetOrMintElemHelper. */
      if (const auto *src_list = src.TryAs<Type::TList>()) {
        const auto *dst_list = dst.TryAs<Type::TList>();
        return dst_list && CanRebuild(src_list->GetElem(), dst_list->GetElem());
      }
      if (const auto *src_set = src.TryAs<Type::TSet>()) {
        const auto *dst_set = dst.TryAs<Type::TSet>();
        return dst_set && CanRebuild(src_set->GetElem(), dst_set->GetElem());
      }
      /* Dict of self (`Node({key: self})`): the key is self-free (so identical
         narrow/wide), the value rebuildable -- the recursion point, or a nested
         container of self as the value (`{key: [self]}`). */
      if (const auto *src_dict = src.TryAs<Type::TDict>()) {
        const auto *dst_dict = dst.TryAs<Type::TDict>();
        return dst_dict && src_dict->GetKey() == dst_dict->GetKey()
            && CanRebuild(src_dict->GetVal(), dst_dict->GetVal());
      }
      /* Any other payload shape (a bare seq of self -- not a legal placement
         anyway) is not yet synthesized; leave it to the plain cast so the type
         checker reports the canonical #104 diagnostic. */
      return false;
    }

    /* True iff every member's every arm has a payload Rebuild() can convert to
       the corresponding wide member's arm. For a single def Corr has one
       member; for a mutually-recursive group, all members must synthesize (the
       folds call one another). Each arm payload is Unrolled with ITS member as
       the binder, so a self-/group-edge resolves to a recursion point. */
    bool CanSynthesize() const {
      for (const auto &member : Corr) {
        const auto *narrow_variant = member.first.TryAs<Type::TVariant>();
        const auto *wide_variant = member.second.TryAs<Type::TVariant>();
        if (!narrow_variant || !wide_variant) {
          return false;
        }
        const auto &wide_elems = wide_variant->GetElems();
        for (const auto &arm : narrow_variant->GetElems()) {
          auto wide_arm = wide_elems.find(arm.first);
          if (wide_arm == wide_elems.end()) {  // guaranteed by VariantWidensTo
            return false;
          }
          if (!CanRebuild(Type::Unroll(arm.second, member.first),
                          Type::Unroll(wide_arm->second, member.second))) {
            return false;
          }
        }
      }
      return true;
    }

    /* Convert a value of narrow subterm type `src` to wide type `dst`. The
       two differ only at recursion points; everything else is identical and
       passes through unchanged.

       `make` builds a FRESH accessor expression for the value each time it is
       called -- the caller must not share Expr nodes (each node may have only
       one parent), so a record that reads its source twice (`.l` and `.r`)
       rebuilds the access path per field rather than reusing one node. */
    Expr::TExpr::TPtr Rebuild(const std::function<Expr::TExpr::TPtr ()> &make,
                              const Type::TType &src,
                              const Type::TType &dst) const {
      /* Identical subterm (interned): no recursion inside -- pass through.
         Covers all non-recursive parts (scalars, sibling fields, etc.). */
      if (src == dst) {
        return make();
      }
      /* The recursion point: a narrow-member-typed value widens to its wide
         correspondent via a recursive call into that member's fold. After
         Unroll, a self-/group-edge in a payload appears as the narrow member
         type itself, which Corr maps to the wide member it widens to. */
      auto corr = Corr.find(src);
      if (corr != Corr.end() && corr->second == dst) {
        Expr::TFunctionApp::TFunctionAppArgMap args;
        args["t"] = Expr::TFunctionAppArg::New(make());
        return Expr::TFunctionApp::New(Expr::TRef::New(Folds.at(src), PosRange), args, PosRange);
      }
      /* Record of self (the common `Branch(<{.l: self, .r: self}>)` shape):
         rebuild field by field, each reading the source record afresh. */
      if (const auto *src_obj = src.TryAs<Type::TObj>()) {
        const auto *dst_obj = dst.As<Type::TObj>();
        const auto &dst_elems = dst_obj->GetElems();
        Expr::TObj::TMemberMap members;
        for (const auto &field : src_obj->GetElems()) {
          auto dst_field = dst_elems.find(field.first);
          assert(dst_field != dst_elems.end());
          const std::string name = field.first;
          members[field.first] = Rebuild(
              [this, &make, name]() {
                return Expr::TObjMember::New(make(), name, PosRange);
              },
              field.second, dst_field->second);
        }
        return Expr::TObj::New(members, PosRange);
      }
      /* Optional of self: widen the inner value only when it is known,
         otherwise pass the unknown through.

           (make() is known) ? (widen(known make()) as wide?) : (unknown wide)

         The recursive-variant shape rules (#116) only let a self-reference sit
         under an optional as the optional's element itself (`Node(self?)`, or
         `self?` as a record field -- a record *under* an optional is rejected),
         so the optional's element is always a recursion point. The known branch
         widens it -- the recursive call's result is the deferred TAny
         placeholder -- and the `as wide?` cast both lifts the bare widened value
         into an optional and resolves the deferred result via the (TAny, TOpt)
         cast arm (#104). The unknown branch is `unknown wide` (TUnknown takes
         the element type, yielding `wide?`), so both branches agree on `wide?`. */
      if (const auto *src_opt = src.TryAs<Type::TOpt>()) {
        const auto *dst_opt = dst.As<Type::TOpt>();
        auto widened = Rebuild(
            [this, &make]() { return Expr::TKnown::New(make(), PosRange); },
            src_opt->GetElem(), dst_opt->GetElem());
        return Expr::TIfElse::New(
            Expr::TAs::New(widened, dst, PosRange),
            Expr::TIsKnown::New(make(), PosRange),
            Expr::TUnknown::New(dst_opt->GetElem(), PosRange),
            PosRange);
      }
      /* Nested variant of self: a `when` that rebuilds each arm in the wide
         nested variant (the recursion point falls inside an arm whose payload
         is the narrow variant). */
      if (const auto *src_var = src.TryAs<Type::TVariant>()) {
        const auto *dst_var = dst.As<Type::TVariant>();
        const auto &dst_elems = dst_var->GetElems();
        std::vector<std::string> tags;
        Expr::TWhen::TExprVec bodies;
        for (const auto &arm : src_var->GetElems()) {
          auto dst_arm = dst_elems.find(arm.first);
          assert(dst_arm != dst_elems.end());
          const std::string tag = arm.first;
          auto rebuilt = Rebuild(
              [this, &make, tag]() {
                return Expr::TObjMember::New(make(), tag, PosRange);
              },
              arm.second, dst_arm->second);
          tags.push_back(tag);
          bodies.push_back(Expr::TVariantCtor::New(tag, rebuilt, dst, PosRange));
        }
        return Expr::TWhen::New(make(), tags, bodies, PosRange);
      }
      /* List / set of self: map a per-element widening over the elements and
         collect back into the wide container.

           f(.t: **make()) as wide-container

         `**make()` turns the narrow container into a sequence of its elements;
         handing that sequence to `f` applies it element-wise (orly's implicit
         map over a sequence argument), and the `as` collect resolves the
         (deferred) element results back into the wide container. The map
         function `f` is the fold itself when the element is the recursion
         point, or a minted per-element helper when the element is a nested
         container of self (`[[self]]`). Both take a parameter named "t". */
      if (src.Is<Type::TList>() || src.Is<Type::TSet>()) {
        const auto &src_elem = src.Is<Type::TList>()
            ? src.As<Type::TList>()->GetElem() : src.As<Type::TSet>()->GetElem();
        const auto &dst_elem = dst.Is<Type::TList>()
            ? dst.As<Type::TList>()->GetElem() : dst.As<Type::TSet>()->GetElem();
        auto elem_corr = Corr.find(src_elem);
        auto map_fn = (elem_corr != Corr.end() && elem_corr->second == dst_elem)
            ? Folds.at(src_elem) : GetOrMintElemHelper(src_elem, dst_elem);
        Expr::TFunctionApp::TFunctionAppArgMap args;
        args["t"] = Expr::TFunctionAppArg::New(Expr::TSequenceOf::New(make(), PosRange));
        auto mapped = Expr::TFunctionApp::New(Expr::TRef::New(map_fn, PosRange), args, PosRange);
        return Expr::TAs::New(mapped, dst, PosRange);
      }
      /* Dict of self: map a per-pair helper over the key/value sequence and
         collect back into the wide dict.

           pairHelper(.p: **make()) as wide-dict

         `**make()` turns the dict into a sequence of `<[key, value]>` pairs;
         the minted pairHelper rebuilds each pair, recursing into the value
         (the recursion point). The `as` collects the sequence of widened pairs
         back into the wide dict, resolving the deferred element results. */
      if (const auto *src_dict = src.TryAs<Type::TDict>()) {
        const auto *dst_dict = dst.As<Type::TDict>();
        auto pair_helper = GetOrMintPairHelper(src_dict->GetKey(),
                                               src_dict->GetVal(), dst_dict->GetVal());
        Expr::TFunctionApp::TFunctionAppArgMap args;
        args["p"] = Expr::TFunctionAppArg::New(Expr::TSequenceOf::New(make(), PosRange));
        auto mapped = Expr::TFunctionApp::New(Expr::TRef::New(pair_helper, PosRange), args, PosRange);
        return Expr::TAs::New(mapped, dst, PosRange);
      }
      /* CanRebuild() gates every call, so any other shape is a logic error. */
      assert(false);
      return make();
    }

    /* Look up -- or, on a miss, mint -- the top-level helper that rebuilds one
       `<[key, value]>` pair of a dict-of-self payload, widening the value:

         pairHelper = ((p) <[p.0, widen(.t: p.1)]>) where {
           p = given::(<[key, narrow]>);
         };

       It is a SIBLING top-level function (not nested in the widening fold):
       recursion is only resolved for top-level functions, and a nested helper
       calling the enclosing fold hits the unbound-enclosing-function
       limitation. Cached per key type within this fold. The value rebuild is
       the recursion point, or a nested container of self (`{key: [self]}`). */
    Symbol::TResultDef::TPtr GetOrMintPairHelper(const Type::TType &key_type,
                                                 const Type::TType &val_src,
                                                 const Type::TType &val_dst) const {
      auto found = PairHelpers->find(key_type);
      if (found != PairHelpers->end()) {
        return found->second;
      }
      auto pair_type = Type::TAddr::Get({{TAddrDir::Asc, key_type}, {TAddrDir::Asc, val_src}});
      auto name = Base::AsStr(NameStem, "Pair", PairHelpers->size());
      auto helper = Symbol::TFunction::New(PackageScope, name, PosRange);
      auto result = Symbol::TResultDef::New(helper, name, PosRange);
      auto param = Symbol::TGivenParamDef::New(helper, "p", pair_type, PosRange);
      /* Register before building the body (mirrors GetOrMintWiden). */
      (*PairHelpers)[key_type] = result;
      /* Body: <[p.0, <widened p.1>]> -- a fresh accessor per read (no shared
         Expr nodes). Rebuild emits the value's widening (the recursion point's
         `widen(.t: p.1)` call, or a nested-container map). */
      auto key_member = Expr::TAddrMember::New(Expr::TRef::New(param, PosRange), 0, PosRange);
      auto val_widened = Rebuild(
          [this, &param]() {
            return Expr::TAddrMember::New(Expr::TRef::New(param, PosRange), 1, PosRange);
          },
          val_src, val_dst);
      Expr::TAddr::TMemberVec members{{TAddrDir::Asc, key_member},
                                      {TAddrDir::Asc, val_widened}};
      helper->SetExpr(Expr::TAddr::New(members, PosRange));
      return result;
    }

    /* Look up -- or, on a miss, mint -- the top-level helper that rebuilds one
       element of a list/set whose element is itself a NESTED container of self
       (`[[self]]`, `[{self}]`, `[{key: self}]`, `[self?]`, ...):

         elemHelper = ((t) <widened t>) where { t = given::(elem_src); };

       The body is the element's own Rebuild -- recursing through the inner
       container (minting further helpers as needed) down to the recursion
       point, which calls the fold. Like the pair helper it must be a sibling
       top-level function, and takes a parameter named "t" so the same call
       shape works whether the map function is the fold or this helper. Cached
       per element type within this fold. */
    Symbol::TResultDef::TPtr GetOrMintElemHelper(const Type::TType &elem_src,
                                                 const Type::TType &elem_dst) const {
      auto found = ElemHelpers->find(elem_src);
      if (found != ElemHelpers->end()) {
        return found->second;
      }
      auto name = Base::AsStr(NameStem, "Elem", ElemHelpers->size());
      auto helper = Symbol::TFunction::New(PackageScope, name, PosRange);
      auto result = Symbol::TResultDef::New(helper, name, PosRange);
      auto param = Symbol::TGivenParamDef::New(helper, "t", elem_src, PosRange);
      /* Register before building the body (mirrors GetOrMintWiden), so a
         self-referential element shape reuses this helper rather than looping. */
      (*ElemHelpers)[elem_src] = result;
      auto body = Rebuild(
          [this, &param]() { return Expr::TRef::New(param, PosRange); },
          elem_src, elem_dst);
      helper->SetExpr(body);
      return result;
    }

    /* The synthesized function's body: a `when` over the narrow arms whose
       body rebuilds each arm's payload in the wide type. The recursion points
       inside it (via Rebuild) call back into the member folds via Folds. */
    Expr::TExpr::TPtr BuildBody(const Symbol::TParamDef::TPtr &param,
                                const Type::TVariant *narrow_variant,
                                const Type::TVariant *wide_variant) const {
      auto narrow_type = narrow_variant->AsType();
      auto wide_type = wide_variant->AsType();
      std::vector<std::string> tags;
      Expr::TWhen::TExprVec bodies;
      const auto &wide_elems = wide_variant->GetElems();
      for (const auto &arm : narrow_variant->GetElems()) {
        auto wide_arm = wide_elems.find(arm.first);
        assert(wide_arm != wide_elems.end());  // guaranteed by VariantWidensTo
        auto src_payload = Type::Unroll(arm.second, narrow_type);
        auto dst_payload = Type::Unroll(wide_arm->second, wide_type);
        const std::string tag = arm.first;
        /* The arm body reads the active payload via `t.Tag`, rebuilt fresh
           wherever the payload value is read (no shared Expr nodes). */
        auto rebuilt = Rebuild(
            [this, &param, tag]() {
              return Expr::TObjMember::New(Expr::TRef::New(param, PosRange), tag, PosRange);
            },
            src_payload, dst_payload);
        tags.push_back(tag);
        bodies.push_back(Expr::TVariantCtor::New(tag, rebuilt, wide_type, PosRange));
      }
      return Expr::TWhen::New(Expr::TRef::New(param, PosRange), tags, bodies, PosRange);
    }
  };  // TWidenSynth

  /* The (narrow, wide) -> synthesized fold cache for one package's synth
     pass, so a narrow->wide widening appearing at several `as` sites mints a
     single shared fold. */
  typedef std::unordered_map<std::pair<Type::TType, Type::TType>,
                             Symbol::TFunction::TPtr> TWidenCache;

  /* Look up -- or, on a miss, mint -- the top-level `widen` fold for the
     given narrow->wide widening, returning the fold for the narrow member
     being cast. For a single self-recursive def this is one fold; for a
     mutually-recursive GROUP (#104) it mints one fold per member, wired to
     call one another, and returns the cast member's. Each fold is added to the
     package scope so it is type-checked (Symbol::TScope::TypeCheck) and emitted
     (CodeGen::TPackage) alongside the user's own top-level functions, and
     recursion resolves to a top-level C++ function.

     `corr` is the member correspondence VariantWidensTo computed for the cast
     (one entry for a single def, all members for a group). */
  Symbol::TFunction::TPtr GetOrMintWiden(const Symbol::TScope::TPtr &package_scope,
                                         TWidenCache &cache,
                                         const Type::TType &narrow_type,
                                         const Type::TType &wide_type,
                                         const std::unordered_map<Type::TType, Type::TType> &corr,
                                         const TPosRange &pos_range) {
    auto key = std::make_pair(narrow_type, wide_type);
    auto found = cache.find(key);
    if (found != cache.end()) {
      return found->second;  // whole group already minted
    }

    /* Mint a fold (function + result def + param) for every member up front,
       so a member's body can call any sibling -- including itself -- through a
       resolved result def. Names are fresh alphanumeric identifiers; the C++
       emitter prefixes them with 'F' (orly/code_gen/export_func.cc), so they
       must be valid identifiers that cannot collide with a user function. */
    std::unordered_map<Type::TType, Symbol::TResultDef::TPtr> folds;
    std::unordered_map<Type::TType,
                       std::pair<Symbol::TFunction::TPtr, Symbol::TParamDef::TPtr>> minted;
    for (const auto &member : corr) {
      auto name = Base::AsStr("widenRec", cache.size());
      auto widen = Symbol::TFunction::New(package_scope, name, pos_range);
      auto widen_result = Symbol::TResultDef::New(widen, name, pos_range);
      auto param = Symbol::TGivenParamDef::New(widen, "t", member.first, pos_range);
      cache[std::make_pair(member.first, member.second)] = widen;
      folds[member.first] = widen_result;
      minted[member.first] = {widen, param};
    }

    /* Build each member's body with the shared correspondence + fold maps. The
       container helper caches are shared across the group so a payload shape
       appearing in several members mints a single helper. The helper names are
       stemmed off the cast member's fold name. */
    std::unordered_map<Type::TType, Symbol::TResultDef::TPtr> pair_helpers;
    std::unordered_map<Type::TType, Symbol::TResultDef::TPtr> elem_helpers;
    TWidenSynth synth{corr, folds, pos_range, package_scope,
                      &pair_helpers, &elem_helpers, minted.at(narrow_type).first->GetName()};
    for (const auto &member : corr) {
      const auto &fn = minted.at(member.first).first;
      const auto &param = minted.at(member.first).second;
      fn->SetExpr(synth.BuildBody(param, member.first.As<Type::TVariant>(),
                                  member.second.As<Type::TVariant>()));
    }
    return cache.at(key);
  }

  /* Collect every `Expr::TAs` reachable from `root` (including inside inner /
     where-bound functions) into `casts`. */
  void CollectCasts(const Expr::TExpr::TPtr &root, std::vector<Expr::TAs *> &casts) {
    if (!root) {
      return;
    }
    Expr::ForEachExpr(root, [&casts](const Expr::TExpr::TPtr &expr) -> bool {
      if (auto *as = dynamic_cast<Expr::TAs *>(expr.get())) {
        casts.push_back(as);
      }
      return false;  // keep recursing
    }, /* include_inner_funcs */ true);
  }

  void CollectCastsInTestBlock(const Symbol::Test::TTestCaseBlock::TPtr &block,
                               std::vector<Expr::TAs *> &casts) {
    if (!block) {
      return;
    }
    for (const auto &test_case : block->GetTestCases()) {
      CollectCasts(test_case->GetExpr(), casts);
      CollectCastsInTestBlock(test_case->GetOptTestCaseBlock(), casts);
    }
  }

}  // namespace

void Synth::SynthesizeRecursiveVariantWidenings(const Symbol::TPackage::TPtr &package) {
  assert(package);

  /* Phase 1 -- collect candidate casts. Done before any minting so the
     package's function set is not mutated while it is being iterated. */
  std::vector<Expr::TAs *> casts;
  for (const auto &func : package->GetFunctions()) {
    CollectCasts(func->GetExpr(), casts);
  }
  for (const auto &test : package->GetTests()) {
    CollectCastsInTestBlock(test->GetTestCaseBlock(), casts);
  }

  /* Phase 2 -- for each recursive-variant widening with a synthesizable
     payload shape, mint (or reuse) its fold and annotate the cast. */
  TWidenCache cache;
  for (auto *as : casts) {
    /* The source type is now safe to query: every def has finished its build
       passes. A genuinely ill-typed operand throws here rather than in
       TypeCheck; swallow it so the diagnostic is reported in its usual place. */
    Type::TType src_type;
    try {
      src_type = as->GetExpr()->GetType();
    } catch (...) {
      continue;
    }
    const Type::TVariant *src_variant = Type::Unwrap(src_type).TryAs<Type::TVariant>();
    const Type::TVariant *dst_variant = Type::Unwrap(as->GetCastType()).TryAs<Type::TVariant>();
    if (!src_variant || !dst_variant || src_variant == dst_variant
        || !(IsRecursiveVariant(src_variant) || IsRecursiveVariant(dst_variant))) {
      continue;
    }
    /* The member correspondence for this widening: one entry for a single
       self-recursive def, all members for a mutually-recursive group. Also the
       widenability gate -- a non-widenable cast yields an empty/false result
       and is left for the type checker's diagnostic. */
    auto narrow_type = src_variant->AsType();
    auto wide_type = dst_variant->AsType();
    std::unordered_map<Type::TType, Type::TType> corr;
    if (!Type::VariantWidensTo(narrow_type, wide_type, corr)) {
      continue;
    }
    /* Pre-flight the payload shapes; an unsupported shape is left for the
       type checker's canonical #104 diagnostic. */
    TWidenSynth probe{corr, {}, as->GetPosRange(), nullptr, nullptr, nullptr, std::string()};
    if (!probe.CanSynthesize()) {
      continue;
    }
    as->SetRecursiveWidenFn(
        GetOrMintWiden(package, cache, narrow_type, wide_type, corr, as->GetPosRange()));
  }
}

TPostfixCast::TPostfixCast(const TExprFactory *expr_factory, const Package::Syntax::TPostfixCast *postfix_cast)
    : PostfixCast(Base::AssertTrue(postfix_cast)), Lhs(nullptr), Rhs(nullptr) {
  assert(expr_factory);
  try {
    Lhs = expr_factory->NewExpr(PostfixCast->GetExpr());
    Rhs = NewType(PostfixCast->GetType());
  } catch (...) {
    delete Lhs;
    delete Rhs;
    throw;
  }
}

TPostfixCast::~TPostfixCast() {
  delete Lhs;
  delete Rhs;
}

Expr::TExpr::TPtr TPostfixCast::Build() const {
  /* Always lower to a plain `Expr::TAs`. A recursive-variant widening is
     turned into a call to a synthesized fold afterwards, by the package-level
     pass Synth::SynthesizeRecursiveVariantWidenings (#104) -- it cannot be
     done here because detecting the widening needs the source expression's
     type, which is not resolvable until every def has finished building. */
  return Expr::TAs::New(Lhs->Build(), Rhs->GetSymbolicType(), GetPosRange(PostfixCast));
}

void TPostfixCast::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  Lhs->ForEachInnerScope(cb);
}

void TPostfixCast::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  Lhs->ForEachRef(cb);
  Rhs->ForEachRef(cb);
}
