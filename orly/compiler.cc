/* <orly/compiler.cc>

   Implements <orly/compiler.h>

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

#include <orly/compiler.h>

#include <cassert>
#include <memory>
#include <mutex>
#include <queue>
#include <unistd.h>
#include <unordered_map>

#include <base/as_str.h>
#include <base/split.h>
#include <base/source_root.h>
#include <base/subprocess.h>
#include <orly/code_gen/package.h>
#include <orly/orly.package.cst.h>
#include <orly/symbol/import_function.h>
#include <orly/synth/context.h>
#include <orly/synth/package.h>

using namespace Base;
using namespace std;
using namespace Orly;
using namespace Orly::Compiler;
using namespace Jhm;

/* Compiles a orlyscript package. On construction it builds to the symbolic layer. It can be asked to emit the
   necessary C++ */
class TPackageBuilder {
  NO_COPY(TPackageBuilder);
  public:

  //TODO: Version report?
  TPackageBuilder(const TRelPath &rel_path) : RelPath(rel_path) {}
  TPackageBuilder(TPackageBuilder &&that)
      : RelPath(move(that.RelPath)), Cst(move(that.Cst)), Synth(move(that.Synth)) {}

  void BuildSymbols(const TTree &src_root) {
    Cst = Package::Syntax::TPackage::ParseFile(AsStr(src_root.GetAbsPath(RelPath)).c_str());
    if (!Cst.HasErrors() && !Synth::GetContext().HasErrors()) {
      assert(Cst.Get());
      Synth = make_unique<Synth::TPackage>(Package::TName{RelPath.Path.ToNamespaceIncludingName()}, &*Cst.Get(), false);
    }
  }

  bool HasErrors() {
    return Cst.HasErrors() || Synth::GetContext().HasErrors();
  }

  void PrintErrors(ostream &out) const {
    Cst.PrintErrors(out);
    Synth::GetContext().PrintErrors(out);
  }

  //TODO: be able to use temp filenames again?
  /* Generates the '.h' interfaces, as well as the '.cc' implementations. */
  void GenerateIntermediateCode(const TTree &out_root) {
    //Generate the CodeGen representation
    CodeGen::TPackage cg(Synth->GetSymbol());
    //Spit out the cc file for the package
    cg.Emit(out_root);

    //Spit out the language object files as necessary (Everything in the header to simplify linking.)
    //TODO: Don't re-emit already emitted object headers.
    cg.EmitObjectHeaders(GetSrcRoot() + "orly/rt/objects");
  }

  Package::TName GetName() const {
    assert(Synth);
    return Synth->GetName();
  }

  unsigned int GetVersion() const {
    assert(Synth);

    return Synth->GetVersion();
  }

  void TypeCheck() const {
    assert(Synth);
    Synth->GetSymbol()->TypeCheck();
  }

  /* The source-relative paths of the packages this one imports (#171). Each
     import lowered to a Symbol::TImportFunction carrying its source package name
     (e.g. `imports/lib`); convert that to the package's `.orly` rel path so the
     compile driver can build it as a dependency. Call after BuildSymbols. */
  std::vector<TRelPath> GetImportPackages() const {
    assert(Synth);
    std::vector<TRelPath> ret;
    for (const auto &func : Synth->GetSymbol()->GetFunctions()) {
      auto import_func = dynamic_cast<const Symbol::TImportFunction *>(func.get());
      if (!import_func) {
        continue;
      }
      const auto &parts = import_func->GetPackageName().Name;
      assert(!parts.empty());
      Base::TPath path(
          Base::TPath::TStrList(parts.begin(), parts.end() - 1),  // namespace
          parts.back(),                                           // name
          Base::TPath::TStrList{"orly"});                         // extension
      ret.emplace_back(move(path));
    }
    return ret;
  }

  private:
  TRelPath RelPath;

  Tools::Nycr::TContextBuilt<Package::Syntax::TPackage> Cst;
  //note: symbolic is owned by synth, and we don't have a good reason to pull it out at the moment.
  unique_ptr<Synth::TPackage> Synth;

};  // TPackageBuilder


//Note: This should probably be promted to a compile management class.
//TODO: Reintroduce machine mode, not saving cc. Also reintroduce syntax check only and semantic check only compilation.
/* Returns the versioned package name of the final build target. */
Package::TVersionedName Orly::Compiler::Compile(
      TPath core_file,
      const TTree &out_tree,
      bool debug_cc,
      bool machine_mode,
      bool semantic_only,
      ostream &out_strm) {
  /* Nabbed by Compile() to prevent multiple threads from trying to compile. */
  static mutex Compiling;

  lock_guard<mutex> lock_compiling(Compiling);
  Synth::GetContext().ClearErrors();

  /* Find the package-tree root: the nearest ancestor directory holding an
     `__orly__` marker. Inside a tree, every package (the core file and any it
     imports) is named relative to that root, so cross-package refs like
     `package <imports/lib>#1` resolve against one shared src tree. Outside a
     tree (no `__orly__`), keep the legacy behavior -- the file's own directory
     is the root and its package name is just the file name -- and imports are
     rejected (see the dependency loop below). */
  std::string core_dir;
  for (const auto &part : core_file.Namespace) {
    core_dir += '/';
    core_dir += part;
  }
  bool found_root = false;
  TTree root_tree = TTree::Find(core_dir, "__orly__", found_root);

  /* Copy the namespace for the no-root fallback tree: we still need core_file's
     namespace below to compute core_rel, so it must not be moved out here. */
  TTree src_tree = found_root ? root_tree : TTree(std::vector<std::string>(core_file.Namespace));
  /* core_rel = the core file relative to src_tree: strip the root's namespace
     components (none when there is no root, leaving just the file name). */
  size_t strip = found_root ? root_tree.Root.size() : core_file.Namespace.size();
  core_file.Namespace.erase(core_file.Namespace.begin(), core_file.Namespace.begin() + strip);
  const TRelPath core_rel(move(core_file));
  //NOTE: As of here, core_file has nothing left in it.

  typedef unordered_map<TRelPath, unique_ptr<TPackageBuilder>> TPackageMap;
  TPackageMap packages;
  packages.insert(make_pair(core_rel, make_unique<TPackageBuilder>(core_rel)));

  bool failed = false;

  /* extra */ {
    queue<TRelPath> todo;
    todo.push(core_rel);

    do {
      auto cur = todo.front();
      todo.pop();

      if (cur.Path.Extension.size() != 1 || cur.Path.Extension[0] != "orly") {
        out_strm << "Invalid package name. Package names may not contain a '.'. This means, for example, a source file can only be 'a.orly', not 'a.foo.orly'" << endl;
        failed = true;
        break;
      }

      auto &builder = packages[cur];
      if (machine_mode) {
        out_strm << "MM_NOTICE: Synth + Symbols" << endl;
      }
      builder->BuildSymbols(src_tree);
      if (builder->HasErrors()) {
        out_strm << "Errors in: " << cur << endl;
        builder->PrintErrors(out_strm);
        failed = true;
        break;
      }
      if (machine_mode) {
        // TODO: UNCOMMENT
        // out_strm << "MM_NOTICE: TypeCheck" << endl;
      }
      builder->TypeCheck();
      if (builder->HasErrors()) {
        out_strm << "Errors in: " << cur << endl;
        builder->PrintErrors(out_strm);
        failed = true;
        break;
      }
      /* Imports (#171): enqueue each imported package as a dependency to build.
         The end-of-compile link step then links every package in the map. */
      for (const auto &dep : builder->GetImportPackages()) {
        if (packages.count(dep)) {
          continue;  // already queued / built (dedup; the dependency graph may share packages)
        }
        if (!found_root) {
          out_strm << "Cross-package imports require an Orly package tree (an __orly__ file at its root)." << endl;
          failed = true;
          break;
        }
        packages.insert(make_pair(dep, make_unique<TPackageBuilder>(dep)));
        todo.push(dep);
      }
      if (failed) {
        break;
      }
      if (machine_mode) {
        out_strm << "MM_NOTICE: Code Gen" << endl;
      }
      //TODO: We only need to generate the C++ if we don't already have up to date C++. Note that doing this means we need
      //      to always gen to a temp file, then move it into place to be atomic in case someone uses Ctrl-C.
      builder->GenerateIntermediateCode(out_tree);
      if (packages[cur]->HasErrors()) {
        //TODO: It would be nice not to have this duplication.
        out_strm << "Errors in: " << cur << endl;
        builder->PrintErrors(out_strm);
        failed = true;
        break;
      }
    } while (!todo.empty());
  }

  if(failed) {
    throw TCompileFailure(HERE, "Compiling Orly language");
  }

  auto versioned_name = Package::TVersionedName{
      packages[core_rel]->GetName(), packages[core_rel]->GetVersion()};

  if (semantic_only) {
    return versioned_name;
  }

  /* extra */ {
    if(machine_mode) {
      out_strm << "MM_NOTICE: Compiling C++" << endl;
    }

    vector<string> gcc_cmd{"g++", "-std=c++23", "-xc++", "-I" + GetSrcRoot(), "-fPIC", "-shared", "-o",
                           AsStr(out_tree.GetAbsPath(SwapExtension(
                               TPath(core_rel.Path), {to_string(packages[core_rel]->GetVersion()), "so"}))),
                           "-iquote", AsStr(out_tree),
                           AsStr(out_tree.GetAbsPath(SwapExtension(TPath(core_rel.Path), {"link", "cc"})))};
    // Take all the packages needed directly or indirectly by the compilation and link them together in one swoop.
    for (const auto &package : packages) {
      gcc_cmd.emplace_back(AsStr(out_tree.GetAbsPath(SwapExtension(TPath(package.first.Path), {"cc"}))));
    }

    /* Warning configuration applied to both debug and release.
       Deliberately NO -Werror -- warnings come overwhelmingly from the
       orly headers themselves (e.g. -Waddress-of-packed-member on
       TCore's union, -Wclass-memaccess on packed serialization layouts)
       rather than from anything the user's .orly file caused us to
       emit. Errror'ing on them just breaks `orlyc` for users whenever
       a new gcc release adds a warning category, with no recourse
       except waiting for a fix in this repo. Print warnings, but don't
       fail the compile.
       The -Wno-* list mirrors root.jhm so generated-code compilation
       sees the same noise filter as the in-tree build. */
    const char *common_warning_args[] = {
        "-Wall", "-Wextra",
        "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-result",
        "-Wno-parentheses", "-Wno-type-limits",
        "-Wno-address-of-packed-member", "-Wno-deprecated-declarations",
        "-Wno-class-memaccess", "-Wno-stringop-overflow", "-Wno-stringop-overread",
        "-Wno-array-bounds", "-Wno-restrict", "-Wno-pessimizing-move",
        "-Wno-redundant-move", "-Wno-mismatched-new-delete",
        "-Wno-dangling-reference", "-Wno-tautological-compare",
        "-Wno-implicit-fallthrough", "-Wno-overloaded-virtual",
        "-Wno-deprecated", "-Wno-deprecated-copy", "-Wno-c++20-compat",
        "-Wno-volatile", "-Wno-narrowing", "-Wno-maybe-uninitialized",
        "-Wno-init-list-lifetime", "-Wno-deprecated-enum-enum-conversion",
        "-Wno-deprecated-enum-float-conversion", "-Wno-int-in-bool-context",
        "-Wno-uninitialized", "-Wno-sign-compare", "-Wno-empty-body",
        "-Wno-misleading-indentation", "-Wno-terminate", "-Wno-noexcept-type",
        "-Wno-comment", "-Wno-reorder", "-Wno-strict-overflow",
        "-Wno-shift-negative-value", "-Wno-zero-as-null-pointer-constant",
        "-Wno-write-strings", "-Wno-multichar", "-Wno-int-to-pointer-cast",
        "-Wno-pointer-arith", "-Wno-format", "-Wno-format-security",
        "-Wno-ignored-qualifiers", "-Wno-cast-function-type",
        "-Wno-aligned-new", "-Wno-placement-new",
        "-Wno-format-truncation", "-Wno-format-overflow",
        "-Wno-nonnull-compare", "-Wno-nonnull",
    };
    for (auto &arg : common_warning_args) {
      gcc_cmd.emplace_back(arg);
    }
    if (debug_cc) {
      gcc_cmd.push_back("-g");
    } else {
      // TODO: Better optimization flags.
      gcc_cmd.push_back("-O2");
      gcc_cmd.push_back("-DNDEBUG");
    }

    TPump pump;
    auto subproc = TSubprocess::New(pump, gcc_cmd);
    auto status = subproc->Wait();
    if (status || failed) {
      if (debug_cc) {
        EchoOutput(subproc->TakeStdOutFromChild());
        EchoOutput(subproc->TakeStdErrFromChild());
      }

      //NOTE: use '-d' to get the error messages.
      out_strm << "Error while compiling an Intermediate Representation. See a Orly team member with your Orly code for support" << endl;
      throw TCompileFailure(HERE, "Compiling C++ and linking");
    }
  }

  /* TODO
  if(!save_cc) {
    unlink(filenames.GetCcPath().c_str());
  }
  */

  return versioned_name;
}
