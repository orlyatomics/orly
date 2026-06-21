/* <orly/expr/as.h>

   `TAs` -- the `expr as Type` cast operator. Carries the source
   expression and a target `Type::TType`. Code-gen lowers to
   `TUnary{Cast}` (see `orly/rt/postfix_cast.h` for the runtime
   specialisations).

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

#include <cassert>
#include <memory>

#include <base/class_traits.h>
#include <orly/expr/unary.h>

namespace Orly {

  namespace Symbol {

    class TFunction;

  }  // Symbol

  namespace Expr {

    class TAs
        : public TUnary {
      NO_COPY(TAs);
      public:

      typedef std::shared_ptr<TAs> TPtr;

      static TPtr New(const TExpr::TPtr &expr, const Type::TType &type, const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      /* The target type of the cast (the annotation). */
      const Type::TType &GetCastType() const {
        return Type;
      }

      /* The synthesized top-level fold this cast widens through, when this is
         a recursive-variant widening (#104); null otherwise. Set by the synth
         pass Synth::SynthesizeRecursiveVariantWidenings once -- the flat
         reinterpret cast cannot rebuild a recursive variant through its boxed
         self-edges, so codegen emits a call to this fold instead, and
         GetTypeImpl accepts the otherwise-deferred widening. */
      const std::shared_ptr<Symbol::TFunction> &GetRecursiveWidenFn() const {
        return RecursiveWidenFn;
      }

      void SetRecursiveWidenFn(const std::shared_ptr<Symbol::TFunction> &fn) {
        assert(!RecursiveWidenFn);
        RecursiveWidenFn = fn;
      }

      virtual Type::TType GetTypeImpl() const override;

      private:

      TAs(const TExpr::TPtr &expr, const Type::TType &type, const TPosRange &pos_range);

      const Type::TType Type;

      std::shared_ptr<Symbol::TFunction> RecursiveWidenFn;

    };  // TAs

  }  // Expr

}  // Orly
