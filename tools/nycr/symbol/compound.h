/* <tools/nycr/symbol/compound.h>

   A production-based token.

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
#include <map>
#include <ostream>
#include <vector>

#include <base/class_traits.h>
#include <tools/nycr/symbol/final.h>

namespace Tools {

  namespace Nycr {

    namespace Symbol {

      class TOperator;

      class TCompound
          : public TFinal {
        NO_COPY(TCompound);
        public:

        class TMember {
          public:

          virtual ~TMember();

          TCompound *GetCompound() const {
            return Compound;
          }

          virtual const TName &GetName() const = 0;

          virtual const TKind *TryGetKind() const = 0;

          virtual void WriteRhs(std::ostream &strm) const = 0;

          virtual void WriteXml(std::ostream &strm) const = 0;

          protected:

          TMember()
              : Compound(0) {}

          void SetCompound(TCompound *compound);

          private:

          TCompound *Compound;

        };  // TMember

        typedef std::map<TName, TMember *> TMembersByName;

        typedef std::vector<TMember *> TMembersInOrder;

        const TMembersByName &GetMembersByName() const {
          return MembersByName;
        }

        const TMembersInOrder &GetMembersInOrder() const {
          return MembersInOrder;
        }

        void SetOperator(TOperator *oper) {
          Operator = oper;
        }

        TOperator *TryGetOperator() const {
          return Operator;
        }

        protected:

        TCompound(const TName &name, TAnyBase *base)
            : TFinal(name, base), Operator(0) {}

        private:

        void OnJoin(TMember *member);

        void OnPart(TMember *member);

        TMembersByName MembersByName;

        TMembersInOrder MembersInOrder;

        TOperator *Operator;

      };  // TCompound

    }  // Symbol

  }  // Nycr

}  // Tools
