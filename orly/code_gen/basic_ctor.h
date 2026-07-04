/* <orly/code_gen/basic_ctor.h>

   `TBasicCtor<TContainer>` emits a container literal: addr (vector
   of `(dir, inline)`), dict (map of `inline -> inline`), set
   (set of inlines), list (vector of inlines). `WriteCtorElem` is
   the per-container element formatter (dict elements emit as
   `{key, val}`, addr elements emit just the value because the dir
   lives outside).

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

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <base/split.h>
#include <orly/shared_enum.h>
#include <orly/code_gen/cpp_printer.h>
#include <orly/code_gen/inline.h>

namespace Orly {

  namespace CodeGen {

    //Addr -> vector<pair<dir, inline>>
    typedef std::vector<std::pair<TAddrDir, TInline::TPtr>> TAddrContainer;
    //Dict -> dict<inline:inline>
    typedef std::unordered_map<TInline::TPtr, TInline::TPtr> TDictContainer;
    //Set -> set<inline>
    typedef std::unordered_set<TInline::TPtr> TSetContainer;
    //List -> vector<inline>
    typedef std::vector<TInline::TPtr> TListContainer;

    //list, set
    inline void WriteCtorElem(const TInline::TPtr &elem, TCppPrinter &out) {
      out << elem;
    }

    //addr
    inline void WriteCtorElem(TAddrContainer::const_reference elem, TCppPrinter &out) {
      out << elem.second;
    }

    //dict
    inline void WriteCtorElem(TDictContainer::const_reference elem, TCppPrinter &out) {
      out << '{' << elem.first << ", " << elem.second << '}';
    }

    /* Forward declaration */
    template <typename TContainer>
    class TBasicCtor;

    template <typename TContainer>
    void WriteCtorStart(const TBasicCtor<TContainer> &, TCppPrinter &) {}

    /* Dependency graph */
    inline void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set, const TAddrContainer &container) {
      for (auto iter : container) {
        TInline::AppendDependency(iter.second, dependency_set);
      }
    }

    /* Dependency graph */
    inline void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set, const TDictContainer &container) {
      for (auto iter : container) {
        TInline::AppendDependency(iter.first, dependency_set);
        TInline::AppendDependency(iter.second, dependency_set);
      }
    }

    /* Dependency graph */
    inline void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set, const TSetContainer &container) {
      for (auto iter : container) {
        TInline::AppendDependency(iter, dependency_set);
      }
    }

    /* Dependency graph */
    inline void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set, const TListContainer &container) {
      for (auto iter : container) {
        TInline::AppendDependency(iter, dependency_set);
      }
    }

    template <typename TContainer>
    class TBasicCtor : public TInline {
      NO_COPY(TBasicCtor);
      public:

      typedef std::shared_ptr<const TBasicCtor> TPtr;

      TBasicCtor(const L0::TPackage *package, const Type::TType &ret_type, TContainer &&elements)
          : TInline(package, ret_type), Elements(std::move(elements)) {}

      void WriteExpr(TCppPrinter &out) const {
        WriteCtorStart(*this, out);
        out
          << '(' << GetReturnType() << "{"
        //Iterate over the list, writing out the elements, C++ making us call the right overload.
          << Base::Join(Elements,
                        ", ",
                        [](TCppPrinter &out,
                           typename TContainer::const_reference elem) {
                          //Stupid c++ templates + overloaded funcs.
                          WriteCtorElem(elem, out);
                        })
          << "})";
      }

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        Orly::CodeGen::AppendDependsOn(dependency_set, Elements);
      }

      size_t GetNumElements() const {
        return Elements.size();
      }

      private:
      TContainer Elements;

    }; // TBasicCtor

    inline void WriteCtorStart(const TBasicCtor<TDictContainer> &ctor, TCppPrinter &out) {
      out << "DictCtor<" << ctor.GetNumElements() << '>';
    }

  } // CodeGen

} // Orly
