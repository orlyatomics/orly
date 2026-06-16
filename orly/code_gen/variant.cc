/* <orly/code_gen/variant.cc>

   Implements <orly/code_gen/variant.h>

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

#include <orly/code_gen/variant.h>

#include <cstddef>
#include <functional>
#include <iterator>

#include <base/as_str.h>
#include <base/code_location.h>
#include <base/path.h>
#include <orly/rt/runtime_error.h>
#include <orly/code_gen/obj.h>
#include <orly/type.h>
#include <orly/type/gen_code.h>
#include <orly/type/obj.h>
#include <orly/type/group_ref.h>
#include <orly/type/object_collector.h>
#include <orly/type/rec_group.h>
#include <orly/type/self_ref.h>
#include <orly/type/unroll.h>
#include <orly/type/util.h>
#include <orly/type/variant.h>

using namespace std;
using namespace Orly;
using namespace Orly::CodeGen;
using namespace Orly::Type;

/* The generated native variant struct represents a value as:

     size_t  Which;          // index of the active arm, in asciibetical
                             // tag order (the order TVariantElems iterates)
     <PayloadT0> V<tag0>;     // payload storage, one field per arm
     <PayloadT1> V<tag1>;
     ...

   Only the field named by the active arm (Which) is meaningful; the
   others hold default-constructed junk. A tag-only arm (e.g. `Deleted`)
   has the empty-object payload type (`Orly::Rt::Objects::TObjO`, the unit
   value), so its field still default-constructs to the empty record --
   `Var::TVar(<empty-obj>)` yields `<{}>`.

   On disk a variant is a fixed-shape record `<{.$which:int, .Tag:payload?,
   ...}>` (issue #96, see orly/var/new_sabot.h): a discriminant plus one
   optional payload per arm, only the active arm's optional set. So
   `Deleted` stores as `<{.$which: 0, .Deleted: <{}>?, .Integer: <empty> }>`.
   Every value of one declared variant shares that record type, which is
   what makes a stored SET of differently-tagged variants homogeneous.

   This mirrors the record generator (orly/code_gen/obj.cc): one payload
   field per arm rather than a C++ union, so that nested records/variants
   (which are not trivially destructible) need no manual lifetime
   management. Construction surface is Phase 3; here we emit one static
   factory per arm and a default + copy ctor.

   Ordering (EqEq/Neq/Match/MatchLess) compares Which first, then the
   active arm's payload. Because Which follows asciibetical tag order,
   this is consistent with the Var-level ordering in orly/var/impl.cc,
   which compares by tag-string then payload. */

void Orly::CodeGen::GenVariantHeader(const std::string &out_dir, const Type::TType &variant_type) {

  /* A member of a mutually-recursive group (#116) is emitted with its
     siblings in one combined header; route it there. */
  {
    std::vector<Type::TType> group_members;
    size_t group_index;
    if (Type::TryGetGroupMembers(variant_type, group_members, group_index)) {
      GenVariantGroupHeader(out_dir, variant_type);
      return;
    }
  }

  auto var_name = variant_type.GetMangledName();
  Base::TPath path(out_dir, var_name, vector<string>{"h"});
  auto class_name = "TVariant" + var_name;
  auto core_type = variant_type.As<Type::TVariant>();
  const auto &elems = core_type->GetElems();

  /* Recursive-variant bookkeeping (issue #103). A recursive arm's payload
     contains self-references and cannot be stored by value; it's stored
     boxed instead:
       - payload IS the self-reference: `Rt::TBox<class_name>`;
       - payload is a record with self-referential fields: a private
         adjacent struct (`<class_name>_Boxed<Tag>`) declared before this
         class and defined after it, holding each self field as a TBox.
     The construction/access surface stays in terms of the UNROLLED
     payload type (self -> the variant itself); the recursive-arm factory
     and getter are member templates so this header never has to include
     the unrolled record's header (which includes this one back). */
  /* True for the arms whose payload needs boxing: a FREE self-reference
     in the payload is bound by THIS variant. (An arm whose payload merely
     embeds some other, complete recursive variant has only bound
     references and stores by value like any payload.) */
  auto arm_is_recursive = [](const Type::TType &payload) {
    return Type::HasFreeSelfRef(payload);
  };
  /* A box or boxed-struct payload compares via direct method calls (its
     Rt:: dispatch specializations aren't declared at the point the
     variant's member bodies need them); a CONTAINER of boxes (#116) has
     no methods and goes through the Rt:: machinery, which resolves fine
     in the variant's member bodies (complete-class context). */
  auto arm_has_methods = [](const Type::TType &payload) {
    /* A self-ref boxes to TBox (methods), a record/open-variant payload to
       an inline struct (methods). A container of boxes has none. */
    return payload.Is<Type::TSelfRef>() || payload.Is<Type::TObj>()
        || (payload.Is<Type::TVariant>() && Type::HasFreeSelfRef(payload));
  };
  bool is_recursive = false;
  for (const auto &elem : elems) {
    if (arm_is_recursive(elem.second)) {
      is_recursive = true;
      break;
    }
  }
  auto boxed_name = [&class_name](const string &tag) {
    return class_name + "_Boxed" + tag;
  };
  /* The name of the inline struct that stores an open nested-variant arm
     payload (#116 Phase 1). Keyed by the open variant's mangled name and
     qualified by the outer class, so it is unique within this header and
     dedups across arms with the same nested shape. */
  auto inline_name = [&class_name](const Type::TType &open_variant) {
    return class_name + "_NV" + open_variant.GetMangledName();
  };
  /* True for an arm payload that is an open nested variant -- a variant
     (transitively) holding a free self-reference bound by an enclosing
     variant. Stored as an inline struct (see inline_name) rather than a
     standalone header, since its native shape depends on the binder.
     v1 (Phase 1) supports this only as a direct arm payload. */
  auto arm_is_nested_variant = [](const Type::TType &payload) {
    return payload.Is<Type::TVariant>() && Type::HasFreeSelfRef(payload);
  };
  /* Prints a type with every free self-reference replaced by its boxed
     storage, elementwise through the containers the placement rules
     allow (#116): `[self]` -> `std::vector<Rt::TBox<V>>`, `self?` ->
     `Orly::Rt::TOpt<Rt::TBox<V>>`, etc. A self-free subtree prints
     verbatim. Used for payload-record fields and container payloads;
     a record payload's own storage is the adjacent boxed struct. */
  std::function<string (const Type::TType &)> subst_storage =
      [&](const Type::TType &t) -> string {
    if (!Type::HasFreeSelfRef(t)) {
      return Base::AsStr(t);
    }
    if (t.Is<Type::TSelfRef>()) {
      return Base::AsStr("Rt::TBox<", class_name, '>');
    }
    if (const auto *list = t.TryAs<Type::TList>()) {
      return Base::AsStr("std::vector<", subst_storage(list->GetElem()), '>');
    }
    if (const auto *set = t.TryAs<Type::TSet>()) {
      return Base::AsStr("Orly::Rt::TSet<", subst_storage(set->GetElem()), '>');
    }
    if (const auto *opt = t.TryAs<Type::TOpt>()) {
      return Base::AsStr("Orly::Rt::TOpt<", subst_storage(opt->GetElem()), '>');
    }
    if (const auto *dict = t.TryAs<Type::TDict>()) {
      /* The placement rules keep keys self-free. */
      return Base::AsStr("Orly::Rt::TDict<", Base::AsStr(dict->GetKey()), ", ",
                         subst_storage(dict->GetVal()), '>');
    }
    /* Placement validation (orly/synth/type_def.cc) bans free refs under
       anything else. */
    assert(false);
    return Base::AsStr(t);
  };
  /* Like subst_storage, but maps a free self-reference to the variant itself
     (`class_name`) rather than to its boxed storage -- i.e. the UNROLLED type,
     expressed through containers WITHOUT ever naming a separate unrolled
     record/variant class. This is the device that lets the read-back
     (Sabot::TToNativeVisitor) and write-back (AsVar) name only the variant and
     self-free types: naming the unrolled payload record directly would pull in
     its header, which includes this one back -- exactly the cycle the boxed
     member-template getter/factory design avoids (#115). A self-free subtree
     prints verbatim; a record/variant payload is handled field-by-field by the
     boxed struct's own AsVar/FromState, never through this helper. */
  std::function<string (const Type::TType &)> unrolled_over_self =
      [&](const Type::TType &t) -> string {
    if (!Type::HasFreeSelfRef(t)) {
      return Base::AsStr(t);
    }
    if (t.Is<Type::TSelfRef>()) {
      /* Fully qualified: this string is used both in namespace
         Orly::Rt::Variants (AsVar/FromState) and Orly::Sabot
         (TToNativeVisitor), where the bare class name is not in scope. */
      return Base::AsStr("Orly::Rt::Variants::", class_name);
    }
    if (const auto *list = t.TryAs<Type::TList>()) {
      return Base::AsStr("std::vector<", unrolled_over_self(list->GetElem()), '>');
    }
    if (const auto *set = t.TryAs<Type::TSet>()) {
      return Base::AsStr("Orly::Rt::TSet<", unrolled_over_self(set->GetElem()), '>');
    }
    if (const auto *opt = t.TryAs<Type::TOpt>()) {
      return Base::AsStr("Orly::Rt::TOpt<", unrolled_over_self(opt->GetElem()), '>');
    }
    if (const auto *dict = t.TryAs<Type::TDict>()) {
      return Base::AsStr("Orly::Rt::TDict<", Base::AsStr(dict->GetKey()), ", ",
                         unrolled_over_self(dict->GetVal()), '>');
    }
    assert(false);
    return Base::AsStr(t);
  };
  /* The C++ type in which an arm's payload is stored. */
  auto storage_type = [&](const string &tag, const Type::TType &payload) {
    if (Type::HasFreeSelfRef(payload) && payload.Is<Type::TObj>()) {
      return boxed_name(tag);
    }
    if (arm_is_nested_variant(payload)) {
      return inline_name(payload);
    }
    return subst_storage(payload);
  };

  TCppPrinter out(AsStr(path));
  out << "/* <" << var_name << ".h>" << Eol
      << Eol
      << "   This file was auto-generated by the Orly compiler. */" << Eol
      << Eol
      << "#pragma once" << Eol
      << Eol
      << "#include <cassert>" << Eol
      << "#include <cstddef>" << Eol
      << "#include <cstdint>" << Eol
      << "#include <string>" << Eol
      << Eol
      << "#include <orly/rt/tuple.h>" << Eol
      << "#include <orly/rt/containers.h>" << Eol
      << "#include <orly/rt/obj.h>" << Eol
      << "#include <orly/type/variant.h>" << Eol
      << "#include <orly/var/impl.h>" << Eol
      // AsVar() builds Var::TVar(<payload-field>) which, for record/variant
      // payloads, resolves to the templated TVar(const TCompound &) ctor whose
      // definition lives in <orly/var/obj.h>.
      << "#include <orly/var/obj.h>" << Eol
      // Native::State::Factory<Var::TVar> (write-side serialization, M2).
      << "#include <orly/var/new_sabot.h>" << Eol
      // Sabot::AsNative + the TToNativeVisitor we specialize for read-back (M2).
      << "#include <orly/sabot/to_native.h>" << Eol
      // Native::Type::For (we specialize it so a set of variants has a proper
      // element type, #96) and Orly::Type::NewSabot (the variant->record-type
      // sabot conversion it routes through).
      << "#include <orly/native/type.h>" << Eol
      << "#include <orly/type/new_sabot.h>" << Eol;
  if (is_recursive) {
    // Rt::TBox / DeepBox / DeepUnbox (recursive arm storage, #103/#116),
    // Type::TSelfRef (the TDt specialization reconstructs the declared
    // payload map), and <utility> for the std::declval the boxed structs'
    // ToUnrolled uses to name TRet's field types.
    out << "#include <utility>" << Eol
        << "#include <orly/rt/box.h>" << Eol
        << "#include <orly/type/self_ref.h>" << Eol;
  }

  /* Pull in any record/variant payload types we reference. */ {
    unordered_set<TType> obj_set;
    for (const auto &elem : elems) {
      CollectObjects(elem.second, obj_set);
    }
    if (obj_set.size()) {
      out << Eol
          << "/* Needed payload objects */" << Eol;
    }
    for (const auto &obj : obj_set) {
      // Record payloads are emitted by the record generator; variant payloads
      // (nested variants) by this generator. Route each include correctly.
      if (obj.Is<Type::TVariant>()) {
        GenVariantInclude(obj, out);
      } else {
        GenObjInclude(obj, out);
      }
    }
  }

  out << Eol;

  /* namespace Orly::Rt::Variants */ {
    TNamespacePrinter nsp(vector<string>{"Orly", "Rt"}, out);
    /* namespace Variants */ {
      TNamespacePrinter nsp(vector<string>{"Variants"}, out);
      if (is_recursive) {
        /* The boxed structs below hold TBox fields of the variant, and the
           variant holds the boxed structs by value, so: forward-declare the
           variant, declare the boxed structs (comparison bodies deferred --
           they need the variant complete), then define the variant. */
        out << "class " << class_name << ";" << Eol
            << Eol;
        for (const auto &elem : elems) {
          const auto &payload = elem.second;
          if (!arm_is_recursive(payload) || !payload.Is<Type::TObj>()) {
            continue;
          }
          auto bname = boxed_name(elem.first);
          const auto &fields = payload.As<Type::TObj>()->GetElems();
          out << "/* Boxed storage for the self-referential payload record of arm `"
              << elem.first << "` (#103): each self field holds the variant via Rt::TBox," << Eol
              << "   elementwise through containers (#116). */" << Eol
              << "class " << bname << " {" << Eol
              << "  public:" << Eol
              << "  " << bname << "() {}" << Eol
              << Eol
              << "  /* From the unrolled payload record (deduced at the call site);" << Eol
              << "     self fields box via Rt::DeepBox. */" << Eol
              << "  template <typename TRec>" << Eol
              << "  explicit " << bname << "(const TRec &rec)";
          if (!fields.empty()) {
            out << " : ";
          }
          out << Base::Join(fields,
                            ", ",
                            [&subst_storage](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              strm << 'V' << it.first << "(Rt::DeepBox<" << subst_storage(it.second)
                                   << ">(rec.GetV" << it.first << "()))";
                            })
              << " {}" << Eol
              << Eol
              << "  /* Back to the unrolled payload record. */" << Eol
              << "  template <typename TRet>" << Eol
              << "  TRet ToUnrolled() const {" << Eol
              << "    return TRet("
              << Base::Join(fields,
                            ", ",
                            [](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              strm << "Rt::DeepUnbox<decltype(std::declval<TRet>().GetV"
                                   << it.first << "())>(V" << it.first << ')';
                            })
              << ");" << Eol
              << "  }" << Eol
              << Eol
              << "  /* Structural comparisons; defined after " << class_name << " (they" << Eol
              << "     need it complete). EqEq is Match: a recursive payload compares" << Eol
              << "     structurally all the way down. */" << Eol
              << "  bool EqEq(const " << bname << " &that) const;" << Eol
              << "  bool Match(const " << bname << " &that) const;" << Eol
              << "  bool MatchLess(const " << bname << " &that) const;" << Eol
              << "  size_t GetHash() const;" << Eol
              << Eol
              << "  /* Storage read/write (#115). Defined after " << class_name << " (they" << Eol
              << "     recurse through it). AsVar builds a dynamic Var::TObj from the" << Eol
              << "     fields (self fields unbox + recurse); FromState rebuilds the boxed" << Eol
              << "     record from a stored sabot record. Both name only " << class_name << " and" << Eol
              << "     self-free field types -- never the unrolled record (no header cycle). */" << Eol
              << "  Var::TVar AsVar() const;" << Eol
              << "  static " << bname << " FromState(const Orly::Sabot::State::TRecord &state);" << Eol
              << Eol
              << "  private:" << Eol;
          for (const auto &field : fields) {
            out << "  " << subst_storage(field.second) << " V" << field.first << ";" << Eol;
          }
          out << "}; // " << bname << Eol
              << Eol;
        }
        /* Inline structs for open nested-variant arm payloads (#116 Phase 1):
           a variant arm whose payload is itself a variant holding a free
           self-reference. The nested variant's native shape depends on its
           binder (the outer), so it cannot get a standalone header keyed by
           its binder-agnostic mangled name -- it is emitted inline here,
           mirroring the _Boxed record structs. v1 supports a single nesting
           level: the nested variant's arms are tag-only, scalar, or a self-
           reference to the outer (depth 1 -> Rt::TBox<outer>). */
        {
          unordered_set<string> emitted_nv;
          for (const auto &elem : elems) {
            if (!arm_is_nested_variant(elem.second)) { continue; }
            auto nvname = inline_name(elem.second);
            if (!emitted_nv.insert(nvname).second) { continue; }
            const auto &nv_elems = elem.second.As<Type::TVariant>()->GetElems();
            auto nv_field = [&class_name](const Type::TType &p) {
              return p.Is<Type::TSelfRef>() ? Base::AsStr("Rt::TBox<", class_name, '>')
                                            : Base::AsStr(p);
            };
            out << "/* Inline storage for the open nested-variant payload of arm `"
                << elem.first << "` (#116). */" << Eol
                << "class " << nvname << " {" << Eol
                << "  public:" << Eol
                << "  " << nvname << "() : Which(0) {}" << Eol
                << Eol
                << "  /* From the unrolled closed nested variant (deduced); self arms box. */" << Eol
                << "  template <typename TClosed>" << Eol
                << "  explicit " << nvname << "(const TClosed &c) : Which(c.GetWhich()) {" << Eol
                << "    switch (c.GetWhich()) {" << Eol;
            {
              size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "      case " << i << ": V" << ne.first << " = Rt::DeepBox<"
                    << nv_field(ne.second) << ">(c.GetV" << ne.first << "()); break;" << Eol;
                ++i;
              }
            }
            out << "    }" << Eol
                << "  }" << Eol
                << Eol
                << "  /* Back to the unrolled closed nested variant. */" << Eol
                << "  template <typename TRet>" << Eol
                << "  TRet ToUnrolled() const {" << Eol
                << "    switch (Which) {" << Eol;
            {
              size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "      case " << i << ": return TRet::Mk" << ne.first
                    << "(Rt::DeepUnbox<decltype(std::declval<TRet>().GetV" << ne.first
                    << "())>(V" << ne.first << "));" << Eol;
                ++i;
              }
            }
            out << "    }" << Eol
                << "    assert(false);" << Eol
                << "    throw Rt::TSystemError(HERE, \"nested variant has no active arm\");" << Eol
                << "  }" << Eol
                << Eol
                << "  /* Structural comparisons; defined after " << class_name
                << " (they deref TBox<" << class_name << "> fields). */" << Eol
                << "  bool EqEq(const " << nvname << " &that) const;" << Eol
                << "  bool Match(const " << nvname << " &that) const;" << Eol
                << "  bool MatchLess(const " << nvname << " &that) const;" << Eol
                << "  size_t GetHash() const;" << Eol
                << Eol
                << "  /* Storage read/write (#115), deferred until " << class_name
                << " is complete (they recurse through it via the self arm). The"
                << " open nested variant is itself a variant value, so AsVar" << Eol
                << "     builds a Var::TVariant; FromState rebuilds it from the"
                << " stored #96 record. */" << Eol
                << "  Var::TVar AsVar() const;" << Eol
                << "  static " << nvname << " FromState(const Orly::Sabot::State::TRecord &state);" << Eol
                << Eol
                << "  private:" << Eol
                << "  size_t Which;" << Eol;
            for (const auto &ne : nv_elems) {
              out << "  " << nv_field(ne.second) << " V" << ne.first << ";" << Eol;
            }
            out << "}; // " << nvname << Eol
                << Eol;
          }
        }
      }
      out << "class " << class_name << " {" << Eol;
      /* Class body */ {
        TIndent indent(out);
        out << "public:" << Eol;

        /* Default ctor: arm 0, default payloads (set/dict mandate it). */
        out << class_name << "() : Which(0) {}" << Eol
            << Eol;

        /* Read-back from storage is handled by the Sabot::TToNativeVisitor
           specialization emitted below (a stored variant is a single-key
           record; the visitor reads the one key, dispatches on its tag, and
           builds this native variant via the per-arm static factories). */

        /* One static factory per arm, named `Mk<Tag>`. The `Mk` prefix
           keeps the method name from colliding with a function-like macro
           that happens to share the tag's spelling -- e.g. an arm named
           `S` would otherwise expand the `S(x)` stringize macro from
           <base/code_location.h> (#119). This mirrors the `V` prefix the
           record/variant generators already use on field/payload storage
           for the same reason. The other tag-derived names (GetV<Tag>,
           V<Tag>, <class>_Boxed<Tag>) are already prefixed and safe.
           A recursive arm's factory takes the UNROLLED payload (deduced
           at the call site -- a template, so this header needn't include
           the unrolled record's header) and stores it boxed. */
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second)) {
              out << "template <typename TPayload>" << Eol
                  << "static " << class_name << " Mk" << elem.first
                  << "(const TPayload &vv) {" << Eol
                  << "  " << class_name << " out;" << Eol
                  << "  out.Which = " << idx << ";" << Eol
                  << "  out.V" << elem.first << " = Rt::DeepBox<"
                  << storage_type(elem.first, elem.second) << ">(vv);" << Eol
                  << "  return out;" << Eol
                  << "}" << Eol;
            } else {
              out << "static " << class_name << " Mk" << elem.first
                  << "(const " << elem.second << " &vv) {" << Eol
                  << "  " << class_name << " out;" << Eol
                  << "  out.Which = " << idx << ";" << Eol
                  << "  out.V" << elem.first << " = vv;" << Eol
                  << "  return out;" << Eol
                  << "}" << Eol;
            }
            ++idx;
          }
        }

        /* Read-back factory for a boxed-record arm (#115): construct directly
           from the already-boxed payload struct (which the storage read path
           rebuilds via <Boxed>::FromState), bypassing the unrolled-payload
           Mk<Tag> above so the header never names the unrolled record. */
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second) && elem.second.Is<Type::TObj>()) {
              out << "static " << class_name << " MkBoxed" << elem.first
                  << "(const " << boxed_name(elem.first) << " &vv) {" << Eol
                  << "  " << class_name << " out;" << Eol
                  << "  out.Which = " << idx << ";" << Eol
                  << "  out.V" << elem.first << " = vv;" << Eol
                  << "  return out;" << Eol
                  << "}" << Eol;
            }
            ++idx;
          }
        }
        /* Read-back factory for a nested-variant arm (#115/#125): construct
           directly from the already-built inline struct (rebuilt by its
           FromState), bypassing the closed-payload Mk<Tag> so the header never
           names the closed nested variant. */
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_nested_variant(elem.second)) {
              out << "static " << class_name << " MkNV" << elem.first
                  << "(const " << inline_name(elem.second) << " &vv) {" << Eol
                  << "  " << class_name << " out;" << Eol
                  << "  out.Which = " << idx << ";" << Eol
                  << "  out.V" << elem.first << " = vv;" << Eol
                  << "  return out;" << Eol
                  << "}" << Eol;
            }
            ++idx;
          }
        }
        out << Eol;

        /* Copy ctor. */
        out << class_name << "(const " << class_name << " &that) : Which(that.Which)";
        for (const auto &elem : elems) {
          out << ", V" << elem.first << "(that.V" << elem.first << ')';
        }
        out << " {}" << Eol
            << Eol;

        /* AsVar(): build a Var::TVariant for the active arm. Declared here,
           defined out-of-line below (it needs the full declared type from the
           TDt specialization, which the value must carry so a set of
           differently-tagged variants is homogeneous -- see #95). */
        out << "Var::TVar AsVar() const;" << Eol
            << Eol;

        /* GetHash(): hash the active arm's tag-index and payload. */
        out << "size_t GetHash() const {" << Eol
            << "  assert(this);" << Eol
            << "  switch (Which) {" << Eol;
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second) && arm_has_methods(elem.second)) {
              out << "    case " << idx << ": return std::hash<size_t>()(" << idx
                  << ") ^ V" << elem.first << ".GetHash();" << Eol;
            } else {
              out << "    case " << idx << ": return std::hash<size_t>()(" << idx
                  << ") ^ std::hash<" << storage_type(elem.first, elem.second)
                  << ">()(V" << elem.first << ");" << Eol;
            }
            ++idx;
          }
        }
        out << "  }" << Eol
            << "  assert(false);" << Eol
            << "  return 0;" << Eol
            << "}" << Eol
            << Eol;

        /* EqEq(): same arm and equal payload. */
        out << "bool EqEq(const " << class_name << " &that) const {" << Eol
            << "  assert(this);" << Eol
            << "  assert(&that);" << Eol
            << "  if (Which != that.Which) { return false; }" << Eol
            << "  switch (Which) {" << Eol;
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second)) {
              if (arm_has_methods(elem.second)) {
                /* Direct method call (not Rt:: dispatch): the boxed types'
                   comparisons are declared above and defined after this
                   class, which keeps every name resolvable right here. */
                out << "    case " << idx << ": return V" << elem.first
                    << ".EqEq(that.V" << elem.first << ");" << Eol;
              } else {
                /* Container of boxes: EqEq is Match (a recursive payload
                   compares structurally all the way down), and Rt::Match
                   uniformly returns bool through the containers. */
                out << "    case " << idx << ": return Rt::Match(V" << elem.first
                    << ", that.V" << elem.first << ");" << Eol;
              }
            } else {
              out << "    case " << idx << ": return Rt::EqEq(V" << elem.first
                  << ", that.V" << elem.first << ");" << Eol;
            }
            ++idx;
          }
        }
        out << "  }" << Eol
            << "  assert(false);" << Eol
            << "  return false;" << Eol
            << "}" << Eol
            << Eol;

        /* Neq(): negation of EqEq. */
        out << "bool Neq(const " << class_name << " &that) const {" << Eol
            << "  assert(this);" << Eol
            << "  assert(&that);" << Eol
            << "  return !EqEq(that);" << Eol
            << "}" << Eol
            << Eol;

        /* Match(): structural equality; same shape as EqEq for variants. */
        out << "bool Match(const " << class_name << " &that) const {" << Eol
            << "  assert(this);" << Eol
            << "  assert(&that);" << Eol
            << "  if (Which != that.Which) { return false; }" << Eol
            << "  switch (Which) {" << Eol;
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second) && arm_has_methods(elem.second)) {
              out << "    case " << idx << ": return V" << elem.first
                  << ".Match(that.V" << elem.first << ");" << Eol;
            } else {
              out << "    case " << idx << ": return Rt::Match(V" << elem.first
                  << ", that.V" << elem.first << ");" << Eol;
            }
            ++idx;
          }
        }
        out << "  }" << Eol
            << "  assert(false);" << Eol
            << "  return false;" << Eol
            << "}" << Eol
            << Eol;

        /* MatchLess(): order by active arm index, then the active payload.
           Consistent with the Var-level tag-then-payload ordering. */
        out << "bool MatchLess(const " << class_name << " &that) const {" << Eol
            << "  assert(this);" << Eol
            << "  assert(&that);" << Eol
            << "  if (Which != that.Which) { return Which < that.Which; }" << Eol
            << "  switch (Which) {" << Eol;
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second) && arm_has_methods(elem.second)) {
              out << "    case " << idx << ": return V" << elem.first
                  << ".MatchLess(that.V" << elem.first << ");" << Eol;
            } else {
              out << "    case " << idx << ": return Rt::MatchLess(V" << elem.first
                  << ", that.V" << elem.first << ");" << Eol;
            }
            ++idx;
          }
        }
        out << "  }" << Eol
            << "  assert(false);" << Eol
            << "  return false;" << Eol
            << "}" << Eol
            << Eol;

        /* Accessors. The per-arm payload getter asserts the arm is active. */
        out << "size_t GetWhich() const {" << Eol
            << "  assert(this);" << Eol
            << "  return Which;" << Eol
            << "}" << Eol;
        {
          size_t idx = 0;
          for (const auto &elem : elems) {
            if (arm_is_recursive(elem.second)) {
              /* TRet is the UNROLLED payload type; the code generator
                 supplies it explicitly at the access site
                 (orly/code_gen/variant_access.cc). A record or open-variant
                 payload is stored in an inline struct that converts via
                 ToUnrolled; a self-ref / container of boxes unboxes via
                 Rt::DeepUnbox. */
              const bool uses_inline_struct =
                  elem.second.Is<Type::TObj>() || arm_is_nested_variant(elem.second);
              out << "template <typename TRet>" << Eol
                  << "TRet GetV" << elem.first << "() const {" << Eol
                  << "  assert(this);" << Eol
                  << "  assert(Which == " << idx << ");" << Eol
                  << "  return "
                  << (uses_inline_struct
                          ? Base::AsStr('V', elem.first, ".template ToUnrolled<TRet>()")
                          : Base::AsStr("Rt::DeepUnbox<TRet>(V", elem.first, ')'))
                  << ";" << Eol
                  << "}" << Eol;
            } else {
              out << elem.second << " GetV" << elem.first << "() const {" << Eol
                  << "  assert(this);" << Eol
                  << "  assert(Which == " << idx << ");" << Eol
                  << "  return V" << elem.first << ";" << Eol
                  << "}" << Eol;
            }
            ++idx;
          }
        }

        out << Eol
            << "private:" << Eol
            << "size_t Which;" << Eol;
        for (const auto &elem : elems) {
          out << storage_type(elem.first, elem.second) << " V" << elem.first << ";" << Eol;
        }

      } // Class body
      out << "}; // " << class_name << Eol;

      /* The boxed structs' comparison bodies, deferred to here: they
         dereference their TBox fields, which needs the variant complete. */
      if (is_recursive) {
        for (const auto &elem : elems) {
          const auto &payload = elem.second;
          if (!arm_is_recursive(payload) || !payload.Is<Type::TObj>()) {
            continue;
          }
          auto bname = boxed_name(elem.first);
          const auto &fields = payload.As<Type::TObj>()->GetElems();
          auto match_term = [](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
            if (it.second.Is<Type::TSelfRef>()) {
              strm << 'V' << it.first << ".Match(that.V" << it.first << ')';
            } else {
              strm << "Rt::Match(V" << it.first << ", that.V" << it.first << ')';
            }
          };
          auto less_term = [](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
            if (it.second.Is<Type::TSelfRef>()) {
              strm << 'V' << it.first << ".MatchLess(that.V" << it.first << ')';
            } else {
              strm << "Rt::MatchLess(V" << it.first << ", that.V" << it.first << ')';
            }
          };
          out << Eol
              << "inline bool " << bname << "::EqEq(const " << bname << " &that) const {" << Eol
              << "  return Match(that);" << Eol
              << "}" << Eol
              << Eol
              << "inline bool " << bname << "::Match(const " << bname << " &that) const {" << Eol
              << "  return ";
          if (fields.empty()) {
            out << "true";
          }
          out << Base::Join(fields, " && ", match_term) << ';' << Eol
              << "}" << Eol
              << Eol
              << "inline bool " << bname << "::MatchLess(const " << bname << " &that) const {" << Eol
              << "  return ";
          if (fields.empty()) {
            out << "false";
          } else {
            /* less(a) || (match(a) && (less(b) || (match(b) && less(c)))) */
            size_t emitted = 0;
            for (auto iter = fields.begin(); iter != fields.end(); ++iter) {
              if (std::next(iter) == fields.end()) {
                less_term(out, *iter);
              } else {
                less_term(out, *iter);
                out << " || (";
                match_term(out, *iter);
                out << " && (";
                ++emitted;
              }
            }
            out << string(2 * emitted, ')');
          }
          out << ';' << Eol
              << "}" << Eol
              << Eol
              << "inline size_t " << bname << "::GetHash() const {" << Eol
              << "  return ";
          if (fields.empty()) {
            out << "0";
          }
          out << Base::Join(fields,
                            " ^ ",
                            [&subst_storage](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              if (it.second.Is<Type::TSelfRef>()) {
                                strm << 'V' << it.first << ".GetHash()";
                              } else {
                                strm << "std::hash<" << subst_storage(it.second)
                                     << ">()(V" << it.first << ')';
                              }
                            })
              << ';' << Eol
              << "}" << Eol;

          /* AsVar (#115): build a dynamic Var::TObj from the fields. A self /
             container-of-self field unboxes to its UNROLLED form (naming only
             this variant) and Var::TVar recurses into it (records/variants have
             AsVar); a self-free field converts directly. */
          out << Eol
              << "inline Var::TVar " << bname << "::AsVar() const {" << Eol
              << "  return Var::TVar::Obj(std::unordered_map<std::string, Var::TVar>{"
              << Base::Join(fields,
                            ", ",
                            [&](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              strm << "{\"" << it.first << "\", Var::TVar(";
                              if (Type::HasFreeSelfRef(it.second)) {
                                strm << "Rt::DeepUnbox<" << unrolled_over_self(it.second)
                                     << ">(V" << it.first << ')';
                              } else {
                                strm << 'V' << it.first;
                              }
                              strm << ")}";
                            })
              << "});" << Eol
              << "}" << Eol;

          /* FromState (#115): rebuild this boxed record from a stored sabot
             record. Walk the fields by name; a self / container-of-self field
             is reconstructed in its UNROLLED form (AsNative recurses through
             this variant) then re-boxed, a self-free field deserializes
             directly. */
          out << Eol
              << "inline " << bname << " " << bname
              << "::FromState(const Orly::Sabot::State::TRecord &state) {" << Eol
              << "  " << bname << " out;" << Eol
              << "  void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());" << Eol
              << "  Sabot::Type::TRecord::TWrapper rtype(state.GetRecordType(type_alloc));" << Eol
              << "  void *type_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize());" << Eol
              << "  Sabot::Type::TRecord::TPin::TWrapper tpin(rtype->Pin(type_pin_alloc));" << Eol
              << "  void *state_pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());" << Eol
              << "  Sabot::State::TRecord::TPin::TWrapper spin(state.Pin(state_pin_alloc));" << Eol
              << "  const size_t elem_count = tpin->GetElemCount();" << Eol
              << "  void *etype_alloc = alloca(Sabot::Type::GetMaxTypeSize());" << Eol
              << "  void *estate_alloc = alloca(Sabot::State::GetMaxStateSize());" << Eol
              << "  std::string field_name;" << Eol
              << "  for (size_t i = 0; i < elem_count; ++i) {" << Eol
              << "    Sabot::Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));" << Eol
              << "    Sabot::State::TAny::TWrapper fstate(spin->NewElem(i, estate_alloc));" << Eol;
          for (const auto &field : fields) {
            out << "    if (field_name == \"" << field.first << "\") { out.V" << field.first << " = ";
            if (Type::HasFreeSelfRef(field.second)) {
              out << "Rt::DeepBox<" << subst_storage(field.second) << ">(Sabot::AsNative<"
                  << unrolled_over_self(field.second) << ">(*fstate)); }" << Eol;
            } else {
              out << "Sabot::AsNative<" << field.second << ">(*fstate); }" << Eol;
            }
          }
          out << "  }" << Eol
              << "  return out;" << Eol
              << "}" << Eol;
        }
        /* Inline nested-variant structs' comparison bodies (deferred: they
           deref TBox<outer> fields, so the outer must be complete). */
        {
          unordered_set<string> done_nv;
          for (const auto &elem : elems) {
            if (!arm_is_nested_variant(elem.second)) { continue; }
            auto nvname = inline_name(elem.second);
            if (!done_nv.insert(nvname).second) { continue; }
            const auto &nv_elems = elem.second.As<Type::TVariant>()->GetElems();
            auto nv_field = [&class_name](const Type::TType &p) {
              return p.Is<Type::TSelfRef>() ? Base::AsStr("Rt::TBox<", class_name, '>')
                                            : Base::AsStr(p);
            };
            out << Eol
                << "inline bool " << nvname << "::EqEq(const " << nvname
                << " &that) const { return Match(that); }" << Eol
                << "inline bool " << nvname << "::Match(const " << nvname << " &that) const {" << Eol
                << "  if (Which != that.Which) { return false; }" << Eol
                << "  switch (Which) {" << Eol;
            {
              size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "    case " << i << ": return Rt::Match(V" << ne.first
                    << ", that.V" << ne.first << ");" << Eol;
                ++i;
              }
            }
            out << "  }" << Eol
                << "  assert(false);" << Eol
                << "  return false;" << Eol
                << "}" << Eol
                << "inline bool " << nvname << "::MatchLess(const " << nvname << " &that) const {" << Eol
                << "  if (Which != that.Which) { return Which < that.Which; }" << Eol
                << "  switch (Which) {" << Eol;
            {
              size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "    case " << i << ": return Rt::MatchLess(V" << ne.first
                    << ", that.V" << ne.first << ");" << Eol;
                ++i;
              }
            }
            out << "  }" << Eol
                << "  assert(false);" << Eol
                << "  return false;" << Eol
                << "}" << Eol
                << "inline size_t " << nvname << "::GetHash() const {" << Eol
                << "  switch (Which) {" << Eol;
            {
              size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "    case " << i << ": return std::hash<size_t>()(" << i
                    << ") ^ std::hash<" << nv_field(ne.second) << ">()(V" << ne.first << ");" << Eol;
                ++i;
              }
            }
            out << "  }" << Eol
                << "  assert(false);" << Eol
                << "  return 0;" << Eol
                << "}" << Eol;

            /* AsVar (#115): the open nested variant is itself a variant value;
               build a Var::TVariant for the active arm, carrying the nested
               variant's (open, TSelfRef-bearing) type so it nests correctly
               inside the outer's encoding. A self arm unboxes to the outer and
               Var::TVar recurses. */
            out << "inline Var::TVar " << nvname << "::AsVar() const {" << Eol
                << "  switch (Which) {" << Eol;
            { size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "    case " << i << ": return Var::TVar::Variant(";
                Type::GenCode(out.GetOstream(), elem.second);
                out << ", \"" << ne.first << "\", Var::TVar(";
                if (ne.second.Is<Type::TSelfRef>()) {
                  out << "Rt::DeepUnbox<" << class_name << ">(V" << ne.first << ')';
                } else {
                  out << 'V' << ne.first;
                }
                out << "));" << Eol;
                ++i;
              }
            }
            out << "  }" << Eol
                << "  assert(false);" << Eol
                << "  throw Rt::TSystemError(HERE, \"nested variant has no active arm\");" << Eol
                << "}" << Eol;

            /* FromState (#115): rebuild the inline struct from the nested
               variant's stored #96 record -- read `$which`, read the active
               arm's payload, box a self arm. */
            out << "inline " << nvname << " " << nvname
                << "::FromState(const Orly::Sabot::State::TRecord &state) {" << Eol
                << "  " << nvname << " out;" << Eol
                << "  void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());" << Eol
                << "  Sabot::Type::TRecord::TWrapper rtype(state.GetRecordType(type_alloc));" << Eol
                << "  void *type_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize());" << Eol
                << "  Sabot::Type::TRecord::TPin::TWrapper tpin(rtype->Pin(type_pin_alloc));" << Eol
                << "  void *state_pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());" << Eol
                << "  Sabot::State::TRecord::TPin::TWrapper spin(state.Pin(state_pin_alloc));" << Eol
                << "  const size_t elem_count = tpin->GetElemCount();" << Eol
                << "  void *etype_alloc = alloca(Sabot::Type::GetMaxTypeSize());" << Eol
                << "  void *estate_alloc = alloca(Sabot::State::GetMaxStateSize());" << Eol
                << "  std::string field_name;" << Eol
                << "  size_t which = static_cast<size_t>(-1);" << Eol;
            for (const auto &ne : nv_elems) {
              out << "  size_t idx_" << ne.first << " = static_cast<size_t>(-1);" << Eol;
            }
            out << "  for (size_t i = 0; i < elem_count; ++i) {" << Eol
                << "    Sabot::Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));" << Eol
                << "    if (field_name == \"$which\") {" << Eol
                << "      Sabot::State::TAny::TWrapper ws(spin->NewElem(i, estate_alloc));" << Eol
                << "      which = static_cast<size_t>(Sabot::AsNative<int64_t>(*ws));" << Eol
                << "    }" << Eol;
            for (const auto &ne : nv_elems) {
              out << "    else if (field_name == \"" << ne.first << "\") { idx_"
                  << ne.first << " = i; }" << Eol;
            }
            out << "  }" << Eol
                << "  void *opt_pin_alloc = alloca(Sabot::State::GetMaxStatePinSize());" << Eol
                << "  void *payload_alloc = alloca(Sabot::State::GetMaxStateSize());" << Eol;
            { size_t i = 0;
              for (const auto &ne : nv_elems) {
                out << "  if (which == " << i << ") {" << Eol
                    << "    out.Which = " << i << ";" << Eol
                    << "    Sabot::State::TAny::TWrapper arm_state(spin->NewElem(idx_" << ne.first << ", estate_alloc));" << Eol
                    << "    const Sabot::State::TOpt *opt = dynamic_cast<const Sabot::State::TOpt *>(arm_state.get());" << Eol
                    << "    if (opt) {" << Eol
                    << "      Sabot::State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));" << Eol
                    << "      Sabot::State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));" << Eol
                    << "      out.V" << ne.first << " = ";
                if (ne.second.Is<Type::TSelfRef>()) {
                  out << "Rt::DeepBox<" << nv_field(ne.second) << ">(Sabot::AsNative<"
                      << class_name << ">(*payload_state));" << Eol;
                } else {
                  out << "Sabot::AsNative<" << nv_field(ne.second) << ">(*payload_state);" << Eol;
                }
                out << "    }" << Eol
                    << "  }" << Eol;
                ++i;
              }
            }
            out << "  return out;" << Eol
                << "}" << Eol;
          }
        }
      }
    } // namespace Variants

    /* EqEq / Neq / Match / MatchLess dispatch structs, mirroring records. */
    out << Eol
        << "template <>" << Eol
        << "struct EqEqStruct<Variants::" << class_name << ", Variants::" << class_name << "> {" << Eol
        << "  static bool Do(const Variants::" << class_name << " &lhs, const Variants::"
        << class_name << " &rhs) {" << Eol
        << "    return lhs.EqEq(rhs);" << Eol
        << "  }" << Eol
        << "}; // EqEqStruct" << Eol
        << Eol
        << "template <>" << Eol
        << "struct NeqStruct<Variants::" << class_name << ", Variants::" << class_name << "> {" << Eol
        << "  static bool Do(const Variants::" << class_name << " &lhs, const Variants::"
        << class_name << " &rhs) {" << Eol
        << "    return lhs.Neq(rhs);" << Eol
        << "  }" << Eol
        << "}; // NeqStruct" << Eol
        << Eol
        << "template <>" << Eol
        << "inline bool Match(const Variants::" << class_name << " &lhs, const Variants::"
        << class_name << " &rhs) {" << Eol
        << "  return lhs.Match(rhs);" << Eol
        << "}" << Eol
        << "template <>" << Eol
        << "inline bool MatchLess(const Variants::" << class_name << " &lhs, const Variants::"
        << class_name << " &rhs) {" << Eol
        << "  return lhs.MatchLess(rhs);" << Eol
        << "}" << Eol;
  } // namespace Orly::Rt

  out << Eol;

  /* namespace Orly::Type: TDt<...>::GetType() -> TVariant::Get(...). */ {
    TNamespacePrinter nsp(vector<string>{"Orly", "Type"}, out);
    out << "template <>" << Eol
        << "struct TDt<Rt::Variants::" << class_name << "> {" << Eol
        << Eol
        << "  static TType GetType() {" << Eol
        << "    return TVariant::Get({"
        << Base::Join(elems,
                      ", ",
                      [](TCppPrinter &out, const TVariant::TElems::value_type &it) {
                        out << "{\"" << it.first << "\", ";
                        Type::GenCode(out.GetOstream(), it.second);
                        out << '}';
                      })
        << "});" << Eol
        << "  }" << Eol
        << Eol
        << "};" << Eol;
  } // namespace Orly::Type

  out << Eol;

  /* Out-of-line AsVar(): build a Var::TVariant for the active arm, carrying
     the FULL declared variant type (from TDt above) so a set of differently-
     tagged variants is homogeneous at the Var layer (#95). Routes through the
     single-key-record encoding via TVar::Variant; the payload becomes
     Var::TVar(<active field>). No empty-object trap (#90): a tag-only arm
     still carries its empty-record payload.

     A RECURSIVE variant (#103) cannot cross into the dynamic-var/sabot world
     in v1 (the sabot type vocabulary cannot express the recursion), so its
     AsVar() throws: storing one or returning one from a package function is
     reported at runtime rather than silently mis-encoded. */ {
    TNamespacePrinter nsp(vector<string>{"Orly", "Rt", "Variants"}, out);
    out << "inline Var::TVar " << class_name << "::AsVar() const {" << Eol
        << "  assert(this);" << Eol
        << "  switch (Which) {" << Eol;
    {
      size_t idx = 0;
      for (const auto &elem : elems) {
        const auto &payload = elem.second;
        out << "    case " << idx << ": ";
        {
          out << "return Var::TVar::Variant("
              << "Type::TDt<Rt::Variants::" << class_name << ">::GetType(), \""
              << elem.first << "\", ";
          if (arm_is_nested_variant(payload)) {
            /* Nested-variant arm (#125): the inline struct is itself a variant
               value and builds its own Var::TVariant. */
            out << "V" << elem.first << ".AsVar()";
          } else if (arm_is_recursive(payload) && payload.Is<Type::TObj>()) {
            /* Boxed-record arm: the boxed struct builds its own Var::TObj,
               recursing into self fields -- without naming the unrolled record. */
            out << "V" << elem.first << ".AsVar()";
          } else if (arm_is_recursive(payload)) {
            /* Self-ref / container-of-self / opt-of-self arm: unbox to the
               unrolled value (naming only this variant) and let Var::TVar
               recurse into it. */
            out << "Var::TVar(Rt::DeepUnbox<" << unrolled_over_self(payload)
                << ">(V" << elem.first << "))";
          } else {
            out << "Var::TVar(V" << elem.first << ")";
          }
          out << ")";
        }
        out << ";" << Eol;
        ++idx;
      }
    }
    out << "  }" << Eol
        << "  assert(false);" << Eol
        << "  throw Rt::TSystemError(HERE, \"variant has no active arm\");" << Eol
        << "}" << Eol;
  } // namespace Orly::Rt::Variants

  out << Eol;

  /* Read-back (#96): a stored variant is the fixed-shape record
       <{ .$which: int, .Tag0: payload0?, .Tag1: payload1?, ... }>
     (see orly/var/new_sabot.h). The generic TToNativeVisitor walks fixed
     record fields, which this struct doesn't expose, so specialize it: read
     `$which` to find the active arm, read that arm's optional payload, and
     build this native variant via the per-arm static factory (payload
     deserialized with AsNative). The inactive arms' empty optionals are
     ignored. */ {
    TNamespacePrinter nsp(vector<string>{"Orly", "Sabot"}, out);
    out << "template <>" << Eol
        << "class TToNativeVisitor<Rt::Variants::" << class_name << "> final : public TStateVisitor {" << Eol
        << "  NO_COPY(TToNativeVisitor);" << Eol
        << "  public:" << Eol
        << "  TToNativeVisitor(Rt::Variants::" << class_name << " &out) : Out(out) {}" << Eol;
    {
      static const char *const kNonRecord[] = {
          "TFree", "TTombstone", "TVoid", "TInt8", "TInt16", "TInt32", "TInt64",
          "TUInt8", "TUInt16", "TUInt32", "TUInt64", "TBool", "TChar", "TFloat",
          "TDouble", "TDuration", "TTimePoint", "TUuid", "TBlob", "TStr", "TDesc",
          "TOpt", "TSet", "TVector", "TMap", "TTuple"};
      for (const char *const s : kNonRecord) {
        out << "  virtual void operator()(const State::" << s
            << " &) const override { THROW_ERROR(TInvalidConversion); }" << Eol;
      }
    }
    {
    out << "  virtual void operator()(const State::TRecord &state) const override {" << Eol
        << "    void *type_alloc = alloca(Type::GetMaxTypeSize());" << Eol
        << "    Type::TRecord::TWrapper rtype(state.GetRecordType(type_alloc));" << Eol
        << "    void *type_pin_alloc = alloca(Type::GetMaxTypePinSize());" << Eol
        << "    Type::TRecord::TPin::TWrapper tpin(rtype->Pin(type_pin_alloc));" << Eol
        << "    void *state_pin_alloc = alloca(State::GetMaxStatePinSize());" << Eol
        << "    State::TRecord::TPin::TWrapper spin(state.Pin(state_pin_alloc));" << Eol
        << "    const size_t elem_count = tpin->GetElemCount();" << Eol
        << "    void *etype_alloc = alloca(Type::GetMaxTypeSize());" << Eol
        << "    void *estate_alloc = alloca(State::GetMaxStateSize());" << Eol
        << "    std::string field_name;" << Eol
        << "    size_t which = static_cast<size_t>(-1);" << Eol;
    /* One field-index slot per arm; the loop below records where each arm's
       optional payload field lives. */
    for (const auto &elem : elems) {
      out << "    size_t idx_" << elem.first << " = static_cast<size_t>(-1);" << Eol;
    }
    out << "    for (size_t i = 0; i < elem_count; ++i) {" << Eol
        << "      Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));" << Eol
        << "      if (field_name == \"$which\") {" << Eol
        << "        State::TAny::TWrapper which_state(spin->NewElem(i, estate_alloc));" << Eol
        << "        which = static_cast<size_t>(AsNative<int64_t>(*which_state));" << Eol
        << "      }" << Eol;
    for (const auto &elem : elems) {
      out << "      else if (field_name == \"" << elem.first << "\") { idx_"
          << elem.first << " = i; }" << Eol;
    }
    out << "    }" << Eol
        << "    void *opt_pin_alloc = alloca(State::GetMaxStatePinSize());" << Eol
        << "    void *payload_alloc = alloca(State::GetMaxStateSize());" << Eol;
    {
      size_t idx = 0;
      for (const auto &elem : elems) {
        out << "    " << (idx == 0 ? "if" : "else if") << " (which == " << idx << ") {" << Eol
            << "      if (idx_" << elem.first << " == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }" << Eol
            << "      State::TAny::TWrapper arm_state(spin->NewElem(idx_" << elem.first << ", estate_alloc));" << Eol
            << "      const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());" << Eol
            << "      if (!opt) { THROW_ERROR(TInvalidConversion); }" << Eol
            << "      State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));" << Eol
            << "      if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }" << Eol
            << "      State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));" << Eol;
        const auto &payload = elem.second;
        if (arm_is_nested_variant(payload)) {
          /* Nested-variant arm (#125): rebuild the inline struct from the
             stored nested #96 record, then construct the arm -- without naming
             the closed nested variant. */
          out << "      const State::TRecord *prec = dynamic_cast<const State::TRecord *>(payload_state.get());" << Eol
              << "      if (!prec) { THROW_ERROR(TInvalidConversion); }" << Eol
              << "      Out = Rt::Variants::" << class_name << "::MkNV" << elem.first
              << "(Rt::Variants::" << inline_name(payload) << "::FromState(*prec));" << Eol;
        } else if (arm_is_recursive(payload) && payload.Is<Type::TObj>()) {
          /* Boxed-record arm: rebuild the boxed struct from the stored sabot
             record (recursing through this variant), then construct the arm --
             without the header ever naming the unrolled record. */
          out << "      const State::TRecord *prec = dynamic_cast<const State::TRecord *>(payload_state.get());" << Eol
              << "      if (!prec) { THROW_ERROR(TInvalidConversion); }" << Eol
              << "      Out = Rt::Variants::" << class_name << "::MkBoxed" << elem.first
              << "(Rt::Variants::" << boxed_name(elem.first) << "::FromState(*prec));" << Eol;
        } else if (arm_is_recursive(payload)) {
          /* Self-ref / container-of-self / opt-of-self arm: reconstruct the
             unrolled payload (naming only this variant) and box it via Mk<Tag>. */
          out << "      Out = Rt::Variants::" << class_name << "::Mk" << elem.first
              << "(AsNative<" << unrolled_over_self(payload) << ">(*payload_state));" << Eol;
        } else {
          out << "      Out = Rt::Variants::" << class_name << "::Mk" << elem.first
              << "(AsNative<" << payload << ">(*payload_state));" << Eol;
        }
        out << "    }" << Eol;
        ++idx;
      }
    }
    out << "    else { THROW_ERROR(TInvalidConversion); }" << Eol
        << "  }" << Eol;
    }
    out << "  private:" << Eol
        << "  Rt::Variants::" << class_name << " &Out;" << Eol
        << "}; // TToNativeVisitor" << Eol;
  } // namespace Orly::Sabot

  out << Eol;

  /* namespace Orly::Native: write-side serialization (#96).
     The generic State::Factory<TVal>/Type::For<TVal> assume a fixed-shape
     record built from registered native fields; a variant registers none, so
     without these specializations a variant's state serializes as an empty
     record and its type sabot is an empty record (which breaks a SET of
     variants -- the empty-record element type mismatches the variant values).

     - State::Factory routes the VALUE through AsVar() -> Var::TVariant ->
       Var::NewSabot, which reuses the SS::TObj(const Var::TVariant *) adapter
       (the fixed-shape variant-record encoding).
     - Type::For routes the TYPE through Orly::Type::NewSabot on the declared
       variant type, which the type-sabot visitor (orly/type/new_sabot.cc)
       maps to the same fixed-shape record. Both stay in lock-step. */ {
    TNamespacePrinter nsp(vector<string>{"Orly", "Native"}, out);
    out << "template <>" << Eol
        << "class State::Factory<Rt::Variants::" << class_name << "> final {" << Eol
        << "  NO_CONSTRUCTION(Factory);" << Eol
        << "  public:" << Eol
        << "  static Sabot::State::TAny *New(const Rt::Variants::" << class_name
        << " &val, void *state_alloc) {" << Eol
        << "    return State::Factory<Var::TVar>::New(val.AsVar(), state_alloc);" << Eol
        << "  }" << Eol
        << "}; // State::Factory<" << class_name << ">" << Eol
        << Eol
        << "template <>" << Eol
        << "class Type::For<Rt::Variants::" << class_name << "> final {" << Eol
        << "  NO_CONSTRUCTION(For);" << Eol
        << "  public:" << Eol
        << "  static Sabot::Type::TAny *GetType(void *type_alloc) {" << Eol
        << "    return Orly::Type::NewSabot(type_alloc, Orly::Type::TDt<Rt::Variants::"
        << class_name << ">::GetType());" << Eol
        << "  }" << Eol
        << "  static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {" << Eol
        << "    return dynamic_cast<Sabot::Type::TRecord *>(GetType(type_alloc));" << Eol
        << "  }" << Eol
        << "}; // Type::For<" << class_name << ">" << Eol;
  } // namespace Orly::Native

  out << Eol;

  /* std::hash specialization. */
  out << "namespace std {" << Eol;
  {
    TIndent indent(out);
    out << "template<>" << Eol
        << "struct hash<Orly::Rt::Variants::" << class_name << "> {" << Eol << Eol
        << "  typedef size_t return_type;" << Eol
        << "  typedef Orly::Rt::Variants::" << class_name << " argument_type;" << Eol << Eol
        << "  size_t operator()(const argument_type &v) const {" << Eol
        << "    return v.GetHash();" << Eol
        << "  }" << Eol << Eol
        << "}; // hash" << Eol;
  }
  out << "} // std" << Eol;
}

/* True iff `t` contains a TGroupRef anywhere (a reference into a mutually-
   recursive group) -- i.e. the arm is recursive. Supported payload shapes:
   a direct group ref, a record of group refs, and list/set/opt/dict-value of
   them (subst_storage walks these); a group ref under any other compound
   throws a clear #116 error there. */
static bool HasGroupRefDeep(const Type::TType &t) {
  if (t.Is<Type::TGroupRef>()) {
    return true;
  }
  if (const auto *v = t.TryAs<Type::TVariant>()) {
    for (const auto &a : v->GetElems()) { if (HasGroupRefDeep(a.second)) return true; }
    return false;
  }
  if (const auto *o = t.TryAs<Type::TObj>()) {
    for (const auto &f : o->GetElems()) { if (HasGroupRefDeep(f.second)) return true; }
    return false;
  }
  if (const auto *l = t.TryAs<Type::TList>()) { return HasGroupRefDeep(l->GetElem()); }
  if (const auto *s = t.TryAs<Type::TSet>())  { return HasGroupRefDeep(s->GetElem()); }
  if (const auto *p = t.TryAs<Type::TOpt>())  { return HasGroupRefDeep(p->GetElem()); }
  if (const auto *d = t.TryAs<Type::TDict>()) {
    return HasGroupRefDeep(d->GetKey()) || HasGroupRefDeep(d->GetVal());
  }
  return false;
}

void Orly::CodeGen::GenVariantGroupHeader(const std::string &out_dir, const Type::TType &member_type) {
  vector<Type::TType> members;
  size_t self_index;
  bool ok = Type::TryGetGroupMembers(member_type, members, self_index);
  assert(ok);
  (void)ok; (void)self_index;

  const Type::TType &primary = members[0];
  auto prim_name = primary.GetMangledName();
  auto class_of = [](const Type::TType &m) { return "TVariant" + m.GetMangledName(); };

  /* Non-primary members: a one-line shim including the combined header, so
     the existing "#include <...<mangled>.h>" convention still resolves. */
  if (!(member_type == primary)) {
    auto mem_name = member_type.GetMangledName();
    Base::TPath path(out_dir, mem_name, vector<string>{"h"});
    TCppPrinter out(AsStr(path));
    out << "/* <" << mem_name << ".h>" << Eol
        << Eol
        << "   This file was auto-generated by the Orly compiler." << Eol
        << "   Member of a mutually-recursive variant group (#116); the group" << Eol
        << "   is defined in the combined header included below. */" << Eol
        << Eol
        << "#pragma once" << Eol
        << Eol
        << "#include <orly/rt/objects/" << prim_name << ".h>" << Eol;
    return;
  }

  /* Primary member: emit the combined header defining every member class.
     A recursive arm holds a reference into the group -- a TGroupRef leaf,
     which (#116) may be a direct sibling/self payload, a field of the
     payload record, or under list/set/opt/dict-value within those. Because
     a group ref resolves to a CONCRETE statically-known sibling class, it
     boxes to Rt::TBox<sibling>; the rest reuses the #103 machinery (boxed
     payload-record struct, DeepBox/DeepUnbox, member-template factory and
     getter over the unrolled payload type) -- here the getter's TRet has
     concrete sibling fields, so the decltype deduction is well-formed. */
  auto is_rec_arm = [](const Type::TType &payload) { return HasGroupRefDeep(payload); };
  auto sibling_of = [](const Type::TType &gr) {
    return Type::ResolveGroupRef(gr.As<Type::TGroupRef>());
  };
  auto boxed_name = [&class_of](const Type::TType &m, const string &tag) {
    return class_of(m) + "_Boxed" + tag;
  };
  /* Print a type with every group ref replaced by its boxed sibling storage,
     elementwise through the containers placement allows. */
  std::function<string (const Type::TType &)> subst_storage =
      [&](const Type::TType &t) -> string {
    if (!HasGroupRefDeep(t)) {
      return Base::AsStr(t);
    }
    if (t.Is<Type::TGroupRef>()) {
      return Base::AsStr("Rt::TBox<", class_of(sibling_of(t)), '>');
    }
    if (const auto *list = t.TryAs<Type::TList>()) {
      return Base::AsStr("std::vector<", subst_storage(list->GetElem()), '>');
    }
    if (const auto *set = t.TryAs<Type::TSet>()) {
      return Base::AsStr("Orly::Rt::TSet<", subst_storage(set->GetElem()), '>');
    }
    if (const auto *opt = t.TryAs<Type::TOpt>()) {
      return Base::AsStr("Orly::Rt::TOpt<", subst_storage(opt->GetElem()), '>');
    }
    if (const auto *dict = t.TryAs<Type::TDict>()) {
      return Base::AsStr("Orly::Rt::TDict<", Base::AsStr(dict->GetKey()), ", ",
                         subst_storage(dict->GetVal()), '>');
    }
    /* A group ref nested in a record-within-a-container or a nested variant
       has no native shape yet (the #116 remainder). */
    throw Orly::Rt::TSystemError(HERE, "this mutually-recursive payload shape is not yet "
                                       "supported (#116): a group reference may appear as an "
                                       "arm payload, a payload-record field, or under "
                                       "list/set/opt/dict-value within those");
  };
  /* An arm whose payload record holds group refs is stored as an adjacent
     boxed struct (the variant analog of #103's _Boxed record storage). */
  auto arm_is_record = [&is_rec_arm](const Type::TType &payload) {
    return payload.Is<Type::TObj>() && is_rec_arm(payload);
  };
  /* The C++ type an arm's payload is stored in. */
  auto storage_type = [&](const Type::TType &m, const string &tag, const Type::TType &payload) {
    return arm_is_record(payload) ? boxed_name(m, tag) : subst_storage(payload);
  };
  /* A box or boxed-struct payload compares via direct method calls; a
     CONTAINER of boxes has no methods and routes through Rt:: dispatch. */
  auto arm_has_methods = [&arm_is_record](const Type::TType &payload) {
    return payload.Is<Type::TGroupRef>() || arm_is_record(payload);
  };

  Base::TPath path(out_dir, prim_name, vector<string>{"h"});
  TCppPrinter out(AsStr(path));
  out << "/* <" << prim_name << ".h>" << Eol
      << Eol
      << "   This file was auto-generated by the Orly compiler." << Eol
      << "   Combined header for a mutually-recursive variant group (#116)." << Eol
      << "   Members: ";
  for (size_t i = 0; i < members.size(); ++i) {
    out << (i ? ", " : "") << class_of(members[i]);
  }
  out << ". */" << Eol
      << Eol
      << "#pragma once" << Eol
      << Eol
      << "#include <cassert>" << Eol
      << "#include <cstddef>" << Eol
      << "#include <cstdint>" << Eol
      << "#include <string>" << Eol
      << "#include <utility>" << Eol
      << Eol
      << "#include <orly/rt/tuple.h>" << Eol
      << "#include <orly/rt/containers.h>" << Eol
      << "#include <orly/rt/obj.h>" << Eol
      << "#include <orly/rt/box.h>" << Eol
      << "#include <orly/type/variant.h>" << Eol
      // MakeRecGroup, for the runtime TType reconstruction GenCode emits for a
      // member (e.g. in a payload record's TDt::GetType) -- reachable from any
      // file that includes a member via its shim header (#116).
      << "#include <orly/type/rec_group.h>" << Eol
      << "#include <orly/var/impl.h>" << Eol
      << "#include <orly/var/obj.h>" << Eol;

  /* Needed payload objects: scalar/closed-type arms only. Group-ref arms
     resolve to sibling member classes defined in this very file, so they
     must NOT be included; CollectObjects pulls siblings in (via the
     TGroupRef collector case), so drop every member type afterward. */
  {
    unordered_set<Type::TType> obj_set;
    for (const auto &m : members) {
      for (const auto &arm : m.As<Type::TVariant>()->GetElems()) {
        if (!is_rec_arm(arm.second)) {
          CollectObjects(arm.second, obj_set);
        }
      }
    }
    for (const auto &m : members) { obj_set.erase(m); }
    if (!obj_set.empty()) {
      out << Eol << "/* Needed payload objects */" << Eol;
    }
    for (const auto &obj : obj_set) {
      if (obj.Is<Type::TVariant>()) {
        GenVariantInclude(obj, out);
      } else {
        GenObjInclude(obj, out);
      }
    }
  }

  out << Eol;

  /* namespace Orly::Rt::Variants */ {
    TNamespacePrinter nsp(vector<string>{"Orly", "Rt"}, out);
    /* namespace Variants */ {
      TNamespacePrinter nsp(vector<string>{"Variants"}, out);

      /* Forward-declare every member: cross-edges are TBox<sibling>. */
      for (const auto &m : members) {
        out << "class " << class_of(m) << ";" << Eol;
      }
      out << Eol;

      /* Boxed payload-record structs (one per record arm, any member): each
         group-ref field stored boxed, with templated ctor-from-unrolled and
         ToUnrolled, comparison bodies deferred until the members complete. */
      for (const auto &m : members) {
        for (const auto &elem : m.As<Type::TVariant>()->GetElems()) {
          if (!arm_is_record(elem.second)) { continue; }
          auto bname = boxed_name(m, elem.first);
          const auto &fields = elem.second.As<Type::TObj>()->GetElems();
          out << "class " << bname << " {" << Eol
              << "  public:" << Eol
              << "  " << bname << "() {}" << Eol
              << "  template <typename TRec>" << Eol
              << "  explicit " << bname << "(const TRec &rec)";
          if (!fields.empty()) { out << " : "; }
          out << Base::Join(fields, ", ",
                            [&subst_storage](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              strm << 'V' << it.first << "(Rt::DeepBox<" << subst_storage(it.second)
                                   << ">(rec.GetV" << it.first << "()))";
                            })
              << " {}" << Eol
              << "  template <typename TRet>" << Eol
              << "  TRet ToUnrolled() const {" << Eol
              << "    return TRet("
              << Base::Join(fields, ", ",
                            [](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              strm << "Rt::DeepUnbox<decltype(std::declval<TRet>().GetV"
                                   << it.first << "())>(V" << it.first << ')';
                            })
              << ");" << Eol
              << "  }" << Eol
              << "  bool EqEq(const " << bname << " &that) const;" << Eol
              << "  bool Match(const " << bname << " &that) const;" << Eol
              << "  bool MatchLess(const " << bname << " &that) const;" << Eol
              << "  size_t GetHash() const;" << Eol
              << "  private:" << Eol;
          for (const auto &field : fields) {
            out << "  " << subst_storage(field.second) << " V" << field.first << ";" << Eol;
          }
          out << "}; // " << bname << Eol << Eol;
        }
      }

      /* One class per member. */
      for (const auto &m : members) {
        auto cm = class_of(m);
        const auto &elems = m.As<Type::TVariant>()->GetElems();

        out << "class " << cm << " {" << Eol;
        { TIndent indent(out);
          out << "public:" << Eol;
          out << cm << "() : Which(0) {}" << Eol << Eol;

          /* Static per-arm factories. A recursive arm takes the UNROLLED
             payload (deduced at the call site) and stores it boxed. */
          { size_t idx = 0;
            for (const auto &elem : elems) {
              if (is_rec_arm(elem.second)) {
                out << "template <typename TPayload>" << Eol
                    << "static " << cm << " Mk" << elem.first << "(const TPayload &vv) {" << Eol
                    << "  " << cm << " out;" << Eol
                    << "  out.Which = " << idx << ";" << Eol
                    << "  out.V" << elem.first << " = Rt::DeepBox<"
                    << storage_type(m, elem.first, elem.second) << ">(vv);" << Eol
                    << "  return out;" << Eol
                    << "}" << Eol;
              } else {
                out << "static " << cm << " Mk" << elem.first << "(const " << elem.second << " &vv) {" << Eol
                    << "  " << cm << " out;" << Eol
                    << "  out.Which = " << idx << ";" << Eol
                    << "  out.V" << elem.first << " = vv;" << Eol
                    << "  return out;" << Eol
                    << "}" << Eol;
              }
              ++idx;
            }
          }
          out << Eol;

          /* Copy ctor. */
          out << cm << "(const " << cm << " &that) : Which(that.Which)";
          for (const auto &elem : elems) {
            out << ", V" << elem.first << "(that.V" << elem.first << ')';
          }
          out << " {}" << Eol << Eol;

          /* AsVar(): mutually-recursive values are package-internal in v1
             (storage/marshaling deferred, like #103 before #115). */
          out << "Var::TVar AsVar() const;" << Eol << Eol;

          /* GetHash(). */
          out << "size_t GetHash() const {" << Eol
              << "  assert(this);" << Eol
              << "  switch (Which) {" << Eol;
          { size_t idx = 0;
            for (const auto &elem : elems) {
              if (is_rec_arm(elem.second) && arm_has_methods(elem.second)) {
                out << "    case " << idx << ": return std::hash<size_t>()(" << idx
                    << ") ^ V" << elem.first << ".GetHash();" << Eol;
              } else {
                out << "    case " << idx << ": return std::hash<size_t>()(" << idx
                    << ") ^ std::hash<" << storage_type(m, elem.first, elem.second)
                    << ">()(V" << elem.first << ");" << Eol;
              }
              ++idx;
            }
          }
          out << "  }" << Eol << "  assert(false);" << Eol << "  return 0;" << Eol << "}" << Eol << Eol;

          /* EqEq(). */
          out << "bool EqEq(const " << cm << " &that) const {" << Eol
              << "  assert(this);" << Eol << "  assert(&that);" << Eol
              << "  if (Which != that.Which) { return false; }" << Eol
              << "  switch (Which) {" << Eol;
          { size_t idx = 0;
            for (const auto &elem : elems) {
              if (is_rec_arm(elem.second)) {
                if (arm_has_methods(elem.second)) {
                  out << "    case " << idx << ": return V" << elem.first << ".EqEq(that.V" << elem.first << ");" << Eol;
                } else {
                  out << "    case " << idx << ": return Rt::Match(V" << elem.first << ", that.V" << elem.first << ");" << Eol;
                }
              } else {
                out << "    case " << idx << ": return Rt::EqEq(V" << elem.first << ", that.V" << elem.first << ");" << Eol;
              }
              ++idx;
            }
          }
          out << "  }" << Eol << "  assert(false);" << Eol << "  return false;" << Eol << "}" << Eol << Eol;

          /* Neq(). */
          out << "bool Neq(const " << cm << " &that) const { return !EqEq(that); }" << Eol << Eol;

          /* Match(). */
          out << "bool Match(const " << cm << " &that) const {" << Eol
              << "  if (Which != that.Which) { return false; }" << Eol
              << "  switch (Which) {" << Eol;
          { size_t idx = 0;
            for (const auto &elem : elems) {
              if (is_rec_arm(elem.second) && arm_has_methods(elem.second)) {
                out << "    case " << idx << ": return V" << elem.first << ".Match(that.V" << elem.first << ");" << Eol;
              } else {
                out << "    case " << idx << ": return Rt::Match(V" << elem.first << ", that.V" << elem.first << ");" << Eol;
              }
              ++idx;
            }
          }
          out << "  }" << Eol << "  assert(false);" << Eol << "  return false;" << Eol << "}" << Eol << Eol;

          /* MatchLess(). */
          out << "bool MatchLess(const " << cm << " &that) const {" << Eol
              << "  if (Which != that.Which) { return Which < that.Which; }" << Eol
              << "  switch (Which) {" << Eol;
          { size_t idx = 0;
            for (const auto &elem : elems) {
              if (is_rec_arm(elem.second) && arm_has_methods(elem.second)) {
                out << "    case " << idx << ": return V" << elem.first << ".MatchLess(that.V" << elem.first << ");" << Eol;
              } else {
                out << "    case " << idx << ": return Rt::MatchLess(V" << elem.first << ", that.V" << elem.first << ");" << Eol;
              }
              ++idx;
            }
          }
          out << "  }" << Eol << "  assert(false);" << Eol << "  return false;" << Eol << "}" << Eol << Eol;

          /* Accessors. A recursive arm's getter is a member template over the
             UNROLLED payload type (supplied by variant_access.cc): a record
             payload converts via the boxed struct's ToUnrolled; a direct
             group ref / container of group refs unboxes via Rt::DeepUnbox. */
          out << "size_t GetWhich() const { assert(this); return Which; }" << Eol;
          { size_t idx = 0;
            for (const auto &elem : elems) {
              if (is_rec_arm(elem.second)) {
                const bool uses_inline_struct = arm_is_record(elem.second);
                out << "template <typename TRet>" << Eol
                    << "TRet GetV" << elem.first << "() const {" << Eol
                    << "  assert(this); assert(Which == " << idx << ");" << Eol
                    << "  return "
                    << (uses_inline_struct
                            ? Base::AsStr('V', elem.first, ".template ToUnrolled<TRet>()")
                            : Base::AsStr("Rt::DeepUnbox<TRet>(V", elem.first, ')'))
                    << ";" << Eol
                    << "}" << Eol;
              } else {
                out << elem.second << " GetV" << elem.first << "() const {" << Eol
                    << "  assert(this); assert(Which == " << idx << ");" << Eol
                    << "  return V" << elem.first << ";" << Eol
                    << "}" << Eol;
              }
              ++idx;
            }
          }

          out << Eol << "private:" << Eol << "size_t Which;" << Eol;
          for (const auto &elem : elems) {
            out << storage_type(m, elem.first, elem.second) << " V" << elem.first << ";" << Eol;
          }
        } // class body
        out << "}; // " << cm << Eol << Eol;
      }

      /* Deferred boxed-record comparison bodies (need the member classes
         complete: they deref the TBox<sibling> fields). */
      for (const auto &m : members) {
        for (const auto &elem : m.As<Type::TVariant>()->GetElems()) {
          if (!arm_is_record(elem.second)) { continue; }
          auto bname = boxed_name(m, elem.first);
          const auto &fields = elem.second.As<Type::TObj>()->GetElems();
          auto match_term = [](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
            if (it.second.Is<Type::TGroupRef>()) {
              strm << 'V' << it.first << ".Match(that.V" << it.first << ')';
            } else {
              strm << "Rt::Match(V" << it.first << ", that.V" << it.first << ')';
            }
          };
          auto less_term = [](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
            if (it.second.Is<Type::TGroupRef>()) {
              strm << 'V' << it.first << ".MatchLess(that.V" << it.first << ')';
            } else {
              strm << "Rt::MatchLess(V" << it.first << ", that.V" << it.first << ')';
            }
          };
          out << "inline bool " << bname << "::EqEq(const " << bname << " &that) const { return Match(that); }" << Eol
              << "inline bool " << bname << "::Match(const " << bname << " &that) const {" << Eol
              << "  return ";
          if (fields.empty()) { out << "true"; }
          out << Base::Join(fields, " && ", match_term) << ';' << Eol
              << "}" << Eol
              << "inline bool " << bname << "::MatchLess(const " << bname << " &that) const {" << Eol
              << "  return ";
          if (fields.empty()) {
            out << "false";
          } else {
            size_t emitted = 0;
            for (auto iter = fields.begin(); iter != fields.end(); ++iter) {
              if (std::next(iter) == fields.end()) {
                less_term(out, *iter);
              } else {
                less_term(out, *iter);
                out << " || (";
                match_term(out, *iter);
                out << " && (";
                ++emitted;
              }
            }
            out << string(2 * emitted, ')');
          }
          out << ';' << Eol
              << "}" << Eol
              << "inline size_t " << bname << "::GetHash() const {" << Eol
              << "  return ";
          if (fields.empty()) { out << "0"; }
          out << Base::Join(fields, " ^ ",
                            [&subst_storage](TCppPrinter &strm, const Type::TObj::TElems::value_type &it) {
                              if (it.second.Is<Type::TGroupRef>()) {
                                strm << 'V' << it.first << ".GetHash()";
                              } else {
                                strm << "std::hash<" << subst_storage(it.second)
                                     << ">()(V" << it.first << ')';
                              }
                            })
              << ';' << Eol
              << "}" << Eol;
        }
      }
      out << Eol;

      /* Out-of-line throwing AsVar for every member. */
      for (const auto &m : members) {
        auto cm = class_of(m);
        out << "inline Var::TVar " << cm << "::AsVar() const {" << Eol
            << "  throw Rt::TSystemError(HERE, \"a mutually-recursive variant value cannot yet be "
               "stored or returned from a package function; see issue #116\");" << Eol
            << "}" << Eol;
      }
      out << Eol;
    } // namespace Variants

    /* Dispatch structs per member, in Orly::Rt (where the primary templates
       live -- a specialization inside Variants would not specialize them). */
    for (const auto &m : members) {
      auto cm = "Variants::" + class_of(m);
      out << "template <> struct EqEqStruct<" << cm << ", " << cm << "> {" << Eol
          << "  static bool Do(const " << cm << " &lhs, const " << cm << " &rhs) { return lhs.EqEq(rhs); }" << Eol
          << "};" << Eol
          << "template <> struct NeqStruct<" << cm << ", " << cm << "> {" << Eol
          << "  static bool Do(const " << cm << " &lhs, const " << cm << " &rhs) { return lhs.Neq(rhs); }" << Eol
          << "};" << Eol
          << "template <> inline bool Match(const " << cm << " &lhs, const " << cm << " &rhs) { return lhs.Match(rhs); }" << Eol
          << "template <> inline bool MatchLess(const " << cm << " &lhs, const " << cm << " &rhs) { return lhs.MatchLess(rhs); }" << Eol;
    }
  } // namespace Orly::Rt

  out << Eol;

  /* std::hash per member. */
  out << "namespace std {" << Eol;
  for (const auto &m : members) {
    auto cm = class_of(m);
    out << "template <> struct hash<Orly::Rt::Variants::" << cm << "> {" << Eol
        << "  typedef size_t return_type;" << Eol
        << "  typedef Orly::Rt::Variants::" << cm << " argument_type;" << Eol
        << "  size_t operator()(const argument_type &v) const { return v.GetHash(); }" << Eol
        << "};" << Eol;
  }
  out << "} // std" << Eol;
}

void Orly::CodeGen::GenVariantInclude(const Type::TType &variant_type, TCppPrinter &out) {
  /* Variant headers are emitted into the same out_dir as record headers
     (orly/rt/objects/) -- their 'V'-prefixed mangled names never collide
     with the 'O'-prefixed record names. */
  out << "#include <orly/rt/objects/" << variant_type.GetMangledName() << ".h>" << Eol;
}
