/* <orly/type/equal_visitor.h>

   `TCommutativeInfixVisitor` specialisation that pre-rejects every
   mixed-type pair with `TExprError` (since you can't compare a
   string to an int) and leaves only same-type pairs as pure-virtual
   hooks. Base for the operator visitors that only make sense within
   one type (see `comp_visitor.h`, `set_ops_visitor.h`,
   `logical_ops_visitor.h`, `div_visitor.h`, `exp_visitor.h`).

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

#include <orly/type/commutative_infix_visitor.h>

namespace Orly {

  namespace Type {

    class TEqualVisitor
        : public TCommutativeInfixVisitor {
      NO_COPY(TEqualVisitor);
      protected:

      TEqualVisitor(TType &type, const TPosRange &pos_range)
          : TCommutativeInfixVisitor(type, pos_range) {}

      virtual void operator()(const TAddr     *, const TAddr     *) const = 0;
      virtual void operator()(const TBool     *, const TBool     *) const = 0;
      virtual void operator()(const TDict     *, const TDict     *) const = 0;
      virtual void operator()(const TId       *, const TId       *) const = 0;
      virtual void operator()(const TInt      *, const TInt      *) const = 0;
      virtual void operator()(const TList     *, const TList     *) const = 0;
      virtual void operator()(const TObj      *, const TObj      *) const = 0;
      virtual void operator()(const TReal     *, const TReal     *) const = 0;
      virtual void operator()(const TSet      *, const TSet      *) const = 0;
      virtual void operator()(const TStr      *, const TStr      *) const = 0;
      virtual void operator()(const TTimeDiff *, const TTimeDiff *) const = 0;
      virtual void operator()(const TTimePnt  *, const TTimePnt  *) const = 0;

      private:

      virtual void operator()(const TAddr     *, const TBool     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TDict     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TId       *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TInt      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TList     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TObj      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TAddr     *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TDict     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TId       *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TInt      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TList     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TObj      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TBool     *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TId       *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TInt      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TList     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TObj      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TDict     *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TInt      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TList     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TObj      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TId       *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TList     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TObj      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TInt      *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TObj      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TList     *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TObj      *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TObj      *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TObj      *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TObj      *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TObj      *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TObj      *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TVariant  *, const TReal     *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TVariant  *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TVariant  *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TVariant  *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TVariant  *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      /* Same-variant diagonal: not pure-virtual (unlike the other same-type pairs) so the many
         TEqualVisitor subclasses (div/exp/mult/sub/logical/set-ops/sort/...) don't each need a
         TVariant override. Operators for which variants are meaningful (== / !=) override this in
         comp_visitor.h; everything else inherits this throw. */
      virtual void operator()(const TVariant  *, const TVariant  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TReal     *, const TSet      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TReal     *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TReal     *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TReal     *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TSet      *, const TStr      *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TSet      *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TSet      *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TStr      *, const TTimeDiff *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TStr      *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }
      virtual void operator()(const TTimeDiff *, const TTimePnt  *) const { throw TExprError(HERE, PosRange); }

    };  // TEqualVisitor

  }  // Type

}  // Orly
