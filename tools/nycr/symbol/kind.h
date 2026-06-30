/* <tools/nycr/symbol/kind.h>

   A kind of token.

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
#include <unordered_set>

#include <base/class_traits.h>
#include <tools/nycr/symbol/name.h>

namespace Tools {

  namespace Nycr {

    namespace Symbol {

      class TBase;
      class TKeyword;
      class TLanguage;
      class TOperator;
      class TRule;

      class TKind {
        NO_COPY(TKind);
        public:

        class TAnyBase {
          NO_COPY(TAnyBase);
          public:

          typedef std::unordered_set<TKind *> TSubKinds;

          const TSubKinds &GetSubKinds() const {
            return SubKinds;
          }

          protected:

          TAnyBase() {}

          virtual ~TAnyBase();

          virtual TBase *GetBase() = 0;

          virtual bool HasBase(const TBase *target) = 0;

          TSubKinds SubKinds;

          friend class TKind;

        };  // TBase

        class TVisitor {
          public:

          virtual ~TVisitor();

          virtual void operator()(const TBase *that) const = 0;

          virtual void operator()(const TKeyword *that) const = 0;

          virtual void operator()(const TLanguage *that) const = 0;

          virtual void operator()(const TOperator *that) const = 0;

          virtual void operator()(const TRule *that) const = 0;

          protected:

          TVisitor() {}

        };  // TVisitor

        virtual ~TKind();

        virtual void Accept(const TVisitor &visitor) const = 0;

        TBase *GetBase() const {
          return Base ? Base->GetBase() : 0;
        }

        const TName &GetName() const {
          return Name;
        }

        bool HasBase(const TBase *target) const {
          return Base ? Base->HasBase(target) : false;
        }

        protected:

        TKind(const TName &name, TAnyBase *base);

        private:

        TName Name;

        TAnyBase *Base;

      };  // TKind

    }  // Symbol

  }  // Nycr

}  // Tools