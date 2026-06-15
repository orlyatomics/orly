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
#include <base/path.h>
#include <orly/code_gen/obj.h>
#include <orly/type.h>
#include <orly/type/gen_code.h>
#include <orly/type/obj.h>
#include <orly/type/object_collector.h>
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
    return payload.Is<Type::TSelfRef>() || payload.Is<Type::TObj>();
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
  /* The C++ type in which an arm's payload is stored. */
  auto storage_type = [&](const string &tag, const Type::TType &payload) {
    if (Type::HasFreeSelfRef(payload) && payload.Is<Type::TObj>()) {
      return boxed_name(tag);
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
              << "  private:" << Eol;
          for (const auto &field : fields) {
            out << "  " << subst_storage(field.second) << " V" << field.first << ";" << Eol;
          }
          out << "}; // " << bname << Eol
              << Eol;
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
                 (orly/code_gen/variant_access.cc). */
              const bool record_payload = elem.second.Is<Type::TObj>();
              out << "template <typename TRet>" << Eol
                  << "TRet GetV" << elem.first << "() const {" << Eol
                  << "  assert(this);" << Eol
                  << "  assert(Which == " << idx << ");" << Eol
                  << "  return "
                  << (record_payload
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
        << "  assert(this);" << Eol;
    if (is_recursive) {
      out << "  throw Rt::TSystemError(HERE, \"a recursive variant value cannot be stored or "
             "returned from a package function; see issue #103\");" << Eol
          << "}" << Eol;
    } else {
      out << "  switch (Which) {" << Eol;
      {
        size_t idx = 0;
        for (const auto &elem : elems) {
          out << "    case " << idx << ": return Var::TVar::Variant("
              << "Type::TDt<Rt::Variants::" << class_name << ">::GetType(), \""
              << elem.first << "\", Var::TVar(V" << elem.first << "));" << Eol;
          ++idx;
        }
      }
      out << "  }" << Eol
          << "  assert(false);" << Eol
          << "  throw Rt::TSystemError(HERE, \"variant has no active arm\");" << Eol
          << "}" << Eol;
    }
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
    if (is_recursive) {
      /* A recursive variant is never stored (#103: its fixed-shape record
         type would be infinitely deep), so there is nothing to read back. */
      out << "  virtual void operator()(const State::TRecord &) const override {"
          << " THROW_ERROR(TInvalidConversion); }" << Eol;
    } else {
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
            << "      State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));" << Eol
            << "      Out = Rt::Variants::" << class_name << "::Mk" << elem.first
            << "(AsNative<" << elem.second << ">(*payload_state));" << Eol
            << "    }" << Eol;
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

void Orly::CodeGen::GenVariantInclude(const Type::TType &variant_type, TCppPrinter &out) {
  /* Variant headers are emitted into the same out_dir as record headers
     (orly/rt/objects/) -- their 'V'-prefixed mangled names never collide
     with the 'O'-prefixed record names. */
  out << "#include <orly/rt/objects/" << variant_type.GetMangledName() << ".h>" << Eol;
}
