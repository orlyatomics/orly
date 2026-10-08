/* <orly/code_gen/obj.h>

   Interface for generating object headers.

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

#include <map>
#include <string>

#include <orly/code_gen/cpp_printer.h>
#include <orly/code_gen/inline.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace CodeGen {

    DEFINE_ERROR(TGenObjError, std::runtime_error, "error generating object");

    class TObjCtor : public TInline {
      NO_COPY(TObjCtor);
      public:

      typedef std::map<std::string, TInline::TPtr> TArgs;

      TObjCtor(const L0::TPackage *package, const Type::TType &type, TArgs &&args);

      void WriteExpr(TCppPrinter &out) const;

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        for (const auto &iter : Args) {
          AppendDependency(iter.second, dependency_set);
        }
      }

      private:
        TArgs Args;
    }; // TObjCtor


    /* The base name (no extension) of the header generated for a record or variant type under
       orly/rt/objects: its mangled name, which is also its C++ class suffix, while that fits in a
       file name. The mangled name spells out every field name and nested type, so a nested type
       outgrows the 255-byte limit (#815). Past a threshold the name is "H" and a 128-bit hash of the
       mangled name in hex, which is just as stable: packages share these headers. */
    std::string ObjHeaderName(const Type::TType &type);

    void GenObjHeader(const std::string &out_dir, const Type::TType &obj_type);
    void GenObjInclude(const Type::TType &obj, TCppPrinter &strm);

  } // CodeGen

} // Orly
