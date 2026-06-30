/* <orly/synth/db_keys_expr.h>

   Synth-layer node for `keys <[pattern]>` -- the index-walking
   expression. Holds the address-shaped key pattern plus the
   dereferenced value type. Lowers to `Expr::TDbKeys`, which
   code-gen turns into a `TKeys` runtime call.

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

#include <functional>
#include <vector>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/scope_and_def.h>
#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    class TDbKeysExpr
        : public TExpr {
      NO_COPY(TDbKeysExpr);
      public:

      TDbKeysExpr(const TExprFactory *expr_factory, const Package::Syntax::TDbKeysExpr *db_keys_expr);

      virtual ~TDbKeysExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      TType* GetValueType() {
        return ValueType;
      }

      private:

      class TMember {
        NO_COPY(TMember);
        public:

        virtual ~TMember();

        virtual Expr::TExpr::TPtr Build() const = 0;

        virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb) = 0;

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) = 0;

        TAddrDir GetAddrDir() const {
          return AddrDir;
        }

        protected:

        TMember(TAddrDir addr_dir);

        private:

        TAddrDir AddrDir;

      };  // TMember

      typedef std::vector<TMember *> TMemberVec;

      class TFixedMember
          : public TMember {
        NO_COPY(TFixedMember);
        public:

        TFixedMember(TAddrDir addr_dir, TExpr *expr);

        virtual ~TFixedMember();

        virtual Expr::TExpr::TPtr Build() const;

        virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

        private:

        TExpr *Expr;

      };  // TFixedMember

      class TFreeMember
          : public TMember {
        NO_COPY(TFreeMember);
        public:

        TFreeMember(TAddrDir addr_dir, TType *type, const TPosRange &pos_range);

        virtual ~TFreeMember();

        virtual Expr::TExpr::TPtr Build() const;

        virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

        private:

        TType *Type;

        const TPosRange PosRange;

      };  // TFreeMember

      void Cleanup();

      const Package::Syntax::TDbKeysExpr *DbKeysExpr;

      TType *ValueType;

      TMemberVec Members;

    };  // TDbKeysExpr

  }  // Synth

}  // Orly
