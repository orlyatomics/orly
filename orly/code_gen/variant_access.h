/* <orly/code_gen/variant_access.h>

   Code-gen inline nodes for the #95 Phase 3 M4 variant-destructuring
   surface, lowering to the primitives the generated native variant
   struct (Phase 2, GenVariantHeader) already exposes:

     - TVariantIs    -- the `expr is <Tag>` predicate. Emits
                        `((operand).GetWhich() == <idx>)`, a bool, where
                        <idx> is the asciibetical index of <Tag>.
     - TVariantMember -- the `expr.<Tag>` guarded payload accessor. Emits
                        `(operand).GetV<Tag>()`, the arm's payload (valid
                        at runtime only when the active arm is <Tag>; the
                        generated GetV<Tag>() asserts Which == idx).

   Near-mirrors of TVariantCtor (orly/code_gen/variant_ctor.h): each wraps
   the (already-lowered) operand inline.

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

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <orly/code_gen/cpp_printer.h>
#include <orly/code_gen/inline.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace CodeGen {

    /* `expr is <Tag>` -> `((operand).GetWhich() == <which>)`. */
    class TVariantIs : public TInline {
      NO_COPY(TVariantIs);
      public:

      TVariantIs(const L0::TPackage *package,
                 const Type::TType &type,
                 const TInline::TPtr &operand,
                 size_t which);

      void WriteExpr(TCppPrinter &out) const;

      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        dependency_set.insert(Operand);
        Operand->AppendDependsOn(dependency_set);
      }

      private:
        TInline::TPtr Operand;
        size_t Which;
    }; // TVariantIs

    /* `expr.<Tag>` -> `(operand).GetV<Tag>()`. */
    class TVariantMember : public TInline {
      NO_COPY(TVariantMember);
      public:

      TVariantMember(const L0::TPackage *package,
                     const Type::TType &type,
                     const TInline::TPtr &operand,
                     const std::string &tag);

      void WriteExpr(TCppPrinter &out) const;

      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        dependency_set.insert(Operand);
        Operand->AppendDependsOn(dependency_set);
      }

      private:
        TInline::TPtr Operand;
        std::string Tag;
    }; // TVariantMember

    /* Exhaustive `(e) when { Tag: body; ... }` (#95 Phase 4) -> a nested
       ternary on the operand's active arm:
         ((op).GetWhich() == w0) ? (b0) : ((op).GetWhich() == w1) ? (b1) : (bN)
       The last arm is unconditional (exhaustiveness is checked at type
       time, so the trailing arm always applies if none earlier did). */
    class TVariantWhen : public TInline {
      NO_COPY(TVariantWhen);
      public:

      typedef std::vector<std::pair<size_t, TInline::TPtr>> TArmVec;

      TVariantWhen(const L0::TPackage *package,
                   const Type::TType &type,
                   const TInline::TPtr &operand,
                   const TArmVec &arms);

      void WriteExpr(TCppPrinter &out) const;

      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        dependency_set.insert(Operand);
        Operand->AppendDependsOn(dependency_set);
        for (const auto &arm : Arms) {
          dependency_set.insert(arm.second);
          arm.second->AppendDependsOn(dependency_set);
        }
      }

      private:
        TInline::TPtr Operand;
        TArmVec Arms;
    }; // TVariantWhen

    /* `narrow_val as wide_t` -- widening a narrow variant to a superset
       variant (#104). The narrow and wide native structs have different
       `Which` numbering (the asciibetical arm index shifts when arms are
       inserted) and different field sets, so this is a value rebuild, not a
       reinterpret: a nested ternary on the source's active arm calls the
       DESTINATION struct's `Mk<Tag>` factory, which assigns the
       destination's own `Which` -- so the `$which` remap falls out for free:
         ((op).GetWhich()==sw0) ? Dst::Mk<t0>((op).GetV<t0>())
           : ... : Dst::Mk<tN>((op).GetV<tN>())
       where `sw*` are the SOURCE arm indices and `Dst` is this inline's
       (wide) result type. v1 handles non-recursive, non-optional widening
       only (the type checker rejects the other cases, see orly/expr/as.cc),
       so every `GetV<Tag>()` is a plain accessor and every arm of the source
       has a same-payload arm in the destination. */
    class TVariantWiden : public TInline {
      NO_COPY(TVariantWiden);
      public:

      /* (source arm `Which` index, tag name). */
      typedef std::vector<std::pair<size_t, std::string>> TArmVec;

      TVariantWiden(const L0::TPackage *package,
                    const Type::TType &type,
                    const TInline::TPtr &operand,
                    const TArmVec &arms);

      void WriteExpr(TCppPrinter &out) const;

      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        dependency_set.insert(Operand);
        Operand->AppendDependsOn(dependency_set);
      }

      private:
        TInline::TPtr Operand;
        TArmVec Arms;
    }; // TVariantWiden

  } // CodeGen

} // Orly
