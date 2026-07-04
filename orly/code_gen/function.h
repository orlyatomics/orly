/* <orly/code_gen/function.h>

   Abstract base for every emitted C++ function. Leaf classes are
   `TTopFunc` (instantiable at package scope), `TInnerFunc` (named
   lambda inside a top-level function), and `TImplicitFunc` (the
   synthesised callback for `map` / `filter` / `reduce` / `sort` /
   `while`). `TArg` represents one named parameter with its `TRef`
   (the inline reference used inside the body). Argument maps are
   always ordered by name -- callers must pass args asciibetically.

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

#include <unordered_set>

#include <orly/code_gen/cpp_printer.h>
#include <orly/code_gen/scope.h>
#include <orly/code_gen/id.h>
#include <orly/code_gen/inline.h>
#include <orly/code_gen/inline_scope.h>
#include <orly/code_gen/package_base.h>
#include <orly/symbol/function.h>

namespace Orly {

  namespace CodeGen {

    class TInlineFunc;

    /* Base class for code gen functions. See leaf classes TTopFunc, TInnerFunc, TImplicitFunc for instantiable
       instanecs. */
    class TFunction : public std::enable_shared_from_this<TFunction> {
      public:

      /* Emits the given child functions -- forward decls, then definitions --
         with the definitions ordered so a function that uses a sibling is
         emitted after that sibling. Shared by WriteBody and TWith::Write
         (#311; the TWith copy had drifted and lacked the dependency
         ordering). A static member (not a free function) so it can reach
         TFunction::Body. */
      static void WriteOrderedChildFuncs(const std::unordered_set<std::shared_ptr<TInlineFunc>> &child_funcs,
                                         TCppPrinter &out);

      /* Collects a TInnerFunc for every where-clause function under `expr`
         into `child_funcs`. Shared by the TFunction ctor and TWith (#311). */
      static void GatherChildFuncs(const L0::TPackage *package,
                                   const Expr::TExpr::TPtr &expr,
                                   const TIdScope::TPtr &id_scope,
                                   std::unordered_set<std::shared_ptr<TInlineFunc>> &child_funcs);

      NO_COPY(TFunction);
      public:

      typedef std::shared_ptr<TFunction> TPtr;
      typedef std::map<std::string, Type::TType> TNamedArgs;

      class TArg {
        NO_COPY(TArg);
        public:
        /* An inline for a function argument.

          TRefs aren't interned using the interner because that would make variable aliases like 'v0 = a0'. Rather
          every arg has one, and only one TRef which it gives pointers to to anyone who asks for an inline version. */
        class TRef : public TInline {
          NO_COPY(TRef);
          public:

          typedef std::shared_ptr<const TRef> TPtr;

          void WriteExpr(TCppPrinter &out) const;

          /* Dependency graph */
          virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &/*dependency_set*/) const override {
          }

          /* An arg ref's emission is already a name; a CSE local for it is noise (#297). */
          virtual bool IsCseWorthy() const override {
            return false;
          }

          private:
          TRef(const L0::TPackage *package, const TArg *arg);

          const TArg *Arg;

          friend class TArg;
        }; // TRef

        typedef std::shared_ptr<TArg> TPtr;

        static TPtr New(const L0::TPackage *package, TId<TIdKind::Arg> &&id, const Type::TType &type);

        const TId<TIdKind::Arg> &GetId() const;
        const TRef::TPtr &GetRef() const;
        Type::TType GetType() const;

        private:
        TArg(const L0::TPackage *package, TId<TIdKind::Arg> &&id, const Type::TType &type);
        TId<TIdKind::Arg> Id;
        Type::TType Type;
        TRef::TPtr Ref;
      }; // TArg

      //A map so that we absolutely order the arguments.
      typedef std::map<std::string, TArg::TPtr> TArgs;

      virtual ~TFunction() {}

      void AddChild(const std::shared_ptr<TInlineFunc> &func);

      /* Build the function body, if necessary. Virtual so TSymbolFunc can emit a
         cross-package call body for an imported value (#171) instead of walking a
         placeholder expr. */
      virtual void Build();

      TArg::TRef::TPtr GetArg(const std::string &name) const;
      const TArgs &GetArgs() const;

      TCodeScope *GetCodeScope();

      /* Get the original name of the function. */
      virtual std::string GetName() const = 0;

      virtual Type::TType GetReturnType() const = 0;
      virtual Type::TType GetType() const = 0;
      bool HasArgs() const;

      /* Return true if the function is top level, which means it needs context passed as a parameter. */
      virtual bool IsTopLevel() const = 0;

      /* Write the function's arguments out in C++. */
      void WriteArgs(TCppPrinter &out) const;

      /* Writes body of function. */
      void WriteBody(TCppPrinter &out) const;

      /* Writes forward declaration (Ex. void FuncFoobar() or std::function<void ()> foobar) */
      virtual void WriteDecl(TCppPrinter &out) const = 0;

      /* Writes the standard C++ definition of this function. */
      virtual void WriteDef(TCppPrinter &out) const = 0;

      /* Write out the C++ code gen name of the function. */
      virtual void WriteName(TCppPrinter &out) const = 0;

      /* Access to body's DependsOn. */
      const TInline::TPtr &GetBody() const {
        return Body;
      }

      protected:
      TFunction(const L0::TPackage *package, const TIdScope::TPtr &id_scope);

      void PostCtor(const TNamedArgs &args, const Expr::TExpr::TPtr &expr, bool keep_mutable, bool implicit=false);

      /* Install a body directly, bypassing the expr-walking Build(). Used by a
         subclass that synthesizes its body (e.g. an import's cross-package call). */
      void SetBody(const TInline::TPtr &body);

      TCodeScope CodeScope;
      std::unordered_set<std::shared_ptr<TInlineFunc>> ChildFuncs;

      const L0::TPackage *Package;

      private:
      TArgs Args;
      TInline::TPtr Body;
      Expr::TExpr::TPtr Expr;
      bool Implicit, KeepMutable;

      friend class TCall;
    }; // TFunction

  } // CodeGen

} // Orly