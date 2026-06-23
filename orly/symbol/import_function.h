/* <orly/symbol/import_function.h>

   A top-level function symbol that stands for a value imported from another
   package (issue #171). `answer is int get_answer from package <imports/lib>#1;`
   binds `answer` to the symbol `get_answer` in another package. Its result type
   is *declared* at the import site rather than inferred from a body, so the
   importing package type-checks against that type without the other package's
   symbols: it overrides GetReturnType() to return the declared type and
   VerifyRecursiveReturns() to a no-op (there is no body to re-evaluate). It
   carries no body; the codegen stage finds these (via RemoteName) and emits the
   real cross-package call.

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

#include <memory>
#include <string>

#include <orly/pos_range.h>
#include <orly/symbol/function.h>
#include <orly/type.h>

namespace Orly {

  namespace Symbol {

    class TImportFunction
        : public TFunction {
      NO_COPY(TImportFunction);
      public:

      typedef std::shared_ptr<TImportFunction> TPtr;

      /* Construct and register the import function in the given scope (mirrors
         TFunction::New). Returned as a TFunction::TPtr so it slots into the
         ordinary function-reference path. It has no body. */
      static TFunction::TPtr New(const TScopePtr &scope,
                                 const std::string &name,
                                 const TPosRange &pos_range,
                                 const Type::TType &declared_return_type,
                                 const std::string &remote_name);

      virtual ~TImportFunction();

      /* The declared type, not an inference over a (nonexistent) body. */
      virtual Type::TType GetReturnType() const override;

      /* No body to verify; the declared type is authoritative. */
      virtual void VerifyRecursiveReturns() const override;

      /* The symbol's name in the source package (for the codegen stage). */
      const std::string &GetRemoteName() const { return RemoteName; }

      private:

      TImportFunction(const TScopePtr &scope,
                      const std::string &name,
                      const TPosRange &pos_range,
                      const Type::TType &declared_return_type,
                      const std::string &remote_name);

      Type::TType DeclaredReturnType;

      std::string RemoteName;

    };  // TImportFunction

  }  // Symbol

}  // Orly
