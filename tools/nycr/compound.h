/* <tools/nycr/compound.h>

   A declaration of a compound of token.

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

#include <vector>

#include <base/class_traits.h>
#include <tools/nycr/final.h>
#include <tools/nycr/operator.h>
#include <tools/nycr/symbol/compound.h>

namespace Tools {

  namespace Nycr {

    class TOperator;

    class TCompound
        : public TFinal {
      NO_COPY(TCompound);
      protected:

      TCompound(const Syntax::TName *name, const Syntax::TOptSuper *opt_super, const Syntax::TOptRhs *opt_rhs);

      virtual ~TCompound();

      void BuildMembers();

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      virtual Symbol::TCompound *GetSymbolAsCompound() const = 0;

      virtual Symbol::TKind *GetSymbolAsKind() const;

      Symbol::TOperator *TryGetOperator() const {
        return Operator ? Operator->GetSymbolAsOperator() : 0;
      }

      private:

      class TMember {
        NO_COPY(TMember);
        public:

        virtual ~TMember() {}

        virtual void Build(Symbol::TCompound *compound) = 0;

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) = 0;

        protected:

        TMember() {}

      };  // TMember

      class TErrorMember
          : public TMember {
        NO_COPY(TErrorMember);
        public:

        TErrorMember() {}

        virtual void Build(Symbol::TCompound *compound);

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      };  // TErrorMember

      class TMemberWithKind
          : public TMember {
        NO_COPY(TMemberWithKind);
        public:

        TMemberWithKind(const Syntax::TName *kind) : Kind(kind) {}

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

        Symbol::TKind *GetKind() const {
          assert(Kind);
          return Kind->GetSymbolAsKind();
        }

        private:

        TRef<TKind> Kind;

      };  // TMemberWithKind

      class TAnonymousMember : public TMemberWithKind {
        NO_COPY(TAnonymousMember);
        public:

        TAnonymousMember(const Syntax::TAnonymousMember *member)
            : TMemberWithKind(member->GetName()) {}

        virtual void Build(Symbol::TCompound *compound);

      };  // TAnonymousMember

      class TNamedMember
          : public TMemberWithKind {
        NO_COPY(TNamedMember);
        public:

        TNamedMember(const Syntax::TNamedMember *member)
            : TMemberWithKind(member->GetKind()), Name(member->GetName()) {}

        virtual void Build(Symbol::TCompound *compound);

        private:

        const Syntax::TName *Name;

      };  // TNamedMember

      std::vector<TMember *> Members;

      TRef<TOperator> Operator;

    };  // TCompound

    template <>
    struct TDecl::TInfo<TCompound> {
      static const char *Name;
    };

  }  // Nycr

}  // Tools
