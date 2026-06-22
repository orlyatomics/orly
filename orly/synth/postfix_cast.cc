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
#include <orly/expr/as.h>
#include <orly/expr/function_app.h>
#include <orly/expr/if_else.h>
#include <orly/expr/known.h>
#include <orly/expr/obj.h>
#include <orly/expr/obj_member.h>
#include <orly/expr/ref.h>
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
#include <orly/type/impl.h>
#include <orly/type/obj.h>
#include <orly/type/opt.h>
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
    /* The narrow and wide variant types: a subterm typed exactly as the
       narrow variant is a recursion point (it widens to the wide variant via
       a recursive call). */
    Type::TType NarrowVariant;
    Type::TType WideVariant;
    /* The synthesized `widen` function's result def -- the callee of every
       recursive call. Null while pre-flighting support with CanRebuild(). */
    Symbol::TResultDef::TPtr WidenResult;
    TPosRange PosRange;

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
      /* The recursion point: narrow widens to wide via a recursive call. */
      if (src == NarrowVariant && dst == WideVariant) {
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
      /* Container (list / set / seq) and dict of self are not yet synthesized;
         leave them to the plain cast so the type checker reports the canonical
         #104 diagnostic. (They need the fold to map each element through the
         recursive call -- a separate Phase 2 increment.) */
      return false;
    }

    /* True iff every arm of the narrow variant has a payload Rebuild() can
       convert to the wide variant's corresponding arm. */
    bool CanSynthesize(const Type::TVariant *narrow_variant,
                       const Type::TVariant *wide_variant) const {
      const auto &wide_elems = wide_variant->GetElems();
      for (const auto &arm : narrow_variant->GetElems()) {
        auto wide_arm = wide_elems.find(arm.first);
        assert(wide_arm != wide_elems.end());  // guaranteed by IsWidenableTo
        if (!CanRebuild(Type::Unroll(arm.second, NarrowVariant),
                        Type::Unroll(wide_arm->second, WideVariant))) {
          return false;
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
      /* The recursion point: a narrow-variant-typed value widens to the wide
         variant via a recursive call. After Unroll, every top-level
         self-reference in a payload appears as the narrow variant itself. */
      if (src == NarrowVariant && dst == WideVariant) {
        Expr::TFunctionApp::TFunctionAppArgMap args;
        args["t"] = Expr::TFunctionAppArg::New(make());
        return Expr::TFunctionApp::New(Expr::TRef::New(WidenResult, PosRange), args, PosRange);
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
      /* CanRebuild() gates every call, so any other shape is a logic error. */
      assert(false);
      return make();
    }

    /* The synthesized function's body: a `when` over the narrow arms whose
       body rebuilds each arm's payload in the wide type. The recursion points
       inside it (via Rebuild) call back into the function via WidenResult. */
    Expr::TExpr::TPtr BuildBody(const Symbol::TParamDef::TPtr &param,
                                const Type::TVariant *narrow_variant,
                                const Type::TVariant *wide_variant) const {
      std::vector<std::string> tags;
      Expr::TWhen::TExprVec bodies;
      const auto &wide_elems = wide_variant->GetElems();
      for (const auto &arm : narrow_variant->GetElems()) {
        auto wide_arm = wide_elems.find(arm.first);
        assert(wide_arm != wide_elems.end());  // guaranteed by IsWidenableTo
        auto src_payload = Type::Unroll(arm.second, NarrowVariant);
        auto dst_payload = Type::Unroll(wide_arm->second, WideVariant);
        const std::string tag = arm.first;
        /* The arm body reads the active payload via `t.Tag`, rebuilt fresh
           wherever the payload value is read (no shared Expr nodes). */
        auto rebuilt = Rebuild(
            [this, &param, tag]() {
              return Expr::TObjMember::New(Expr::TRef::New(param, PosRange), tag, PosRange);
            },
            src_payload, dst_payload);
        tags.push_back(tag);
        bodies.push_back(Expr::TVariantCtor::New(tag, rebuilt, WideVariant, PosRange));
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
     given narrow->wide recursive-variant pair. The function is added to the
     package scope so it is type-checked (Symbol::TScope::TypeCheck) and
     emitted (CodeGen::TPackage) alongside the user's own top-level functions,
     and recursion resolves to a top-level C++ function. */
  Symbol::TFunction::TPtr GetOrMintWiden(const Symbol::TScope::TPtr &package_scope,
                                         TWidenCache &cache,
                                         const Type::TVariant *narrow_variant,
                                         const Type::TVariant *wide_variant,
                                         const TPosRange &pos_range) {
    auto narrow_type = narrow_variant->AsType();
    auto wide_type = wide_variant->AsType();
    auto key = std::make_pair(narrow_type, wide_type);
    auto found = cache.find(key);
    if (found != cache.end()) {
      return found->second;
    }

    /* A fresh, alphanumeric name -- the C++ emitter prefixes it with 'F'
       (orly/code_gen/export_func.cc), so it must be a valid identifier and
       must not collide with a user function. */
    auto name = Base::AsStr("widenRec", cache.size());
    auto widen = Symbol::TFunction::New(package_scope, name, pos_range);
    auto widen_result = Symbol::TResultDef::New(widen, name, pos_range);
    auto param = Symbol::TGivenParamDef::New(widen, "t", narrow_type, pos_range);

    /* Register before building the body so a self-recursive widening reuses
       this same function rather than recursing here. */
    cache[key] = widen;

    TWidenSynth synth{narrow_type, wide_type, widen_result, pos_range};
    widen->SetExpr(synth.BuildBody(param, narrow_variant, wide_variant));
    return widen;
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
        || !src_variant->IsWidenableTo(dst_variant)
        || !(IsRecursiveVariant(src_variant) || IsRecursiveVariant(dst_variant))) {
      continue;
    }
    /* Pre-flight the payload shapes; an unsupported shape is left for the
       type checker's canonical #104 diagnostic. */
    TWidenSynth probe{src_variant->AsType(), dst_variant->AsType(), nullptr, as->GetPosRange()};
    if (!probe.CanSynthesize(src_variant, dst_variant)) {
      continue;
    }
    as->SetRecursiveWidenFn(
        GetOrMintWiden(package, cache, src_variant, dst_variant, as->GetPosRange()));
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
