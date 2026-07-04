/* <jhm/jobs/compile_c_family.cc>

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

#include <jhm/jobs/compile_c_family.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include <base/json.h>
#include <base/split.h>
#include <jhm/env.h>
#include <jhm/file.h>

using namespace Base;
using namespace Jhm;
using namespace Jhm::Job;
using namespace std;
using namespace std::placeholders;
using namespace Util;

vector<string> TCompileCFamily::GetStandardArgs(TFile *input, bool is_cpp, const TEnv &env) {

  // Add options from configuration. Per-file config overrides global config: the file's config
  // falls back to the env config (#340), so one read covers both (and file-level '+' deltas
  // apply on top of the env's flags instead of replacing them).
  vector<string> options;
  input->GetConfig().TryRead({"cmd", is_cpp ? "g++" : "gcc"}, options);

  const auto src_str = AsStr(*env.GetSrc());

  // Add the src and out directories as sources of includes.
  options.push_back("-I" + src_str);
  options.push_back("-I" + AsStr(*env.GetOut()));

  // Let the code know where the root of the tree was (So it can remove the SRC prefix if needed)
  options.push_back("-DSRC_ROOT=\"" + src_str + "/\"");
  return options;
}

static TRelPath GetOutputName(const TRelPath &input, bool is_cpp) {
  assert(input.Path.EndsWith({is_cpp ? "cc" : "c"}));
  return TRelPath(SwapExtension(TPath(input.Path), {"o"}));
}

static std::optional<TRelPath> GetInputName(const TRelPath &output, bool is_cpp) {
  if (output.Path.EndsWith({"o"})) {
    return TRelPath(SwapExtension(TPath(output.Path), {is_cpp ? "cc" : "c"}));
  } else {
    return std::optional<TRelPath>();
  }
}

TJobProducer TCompileCFamily::GetCProducer() {
  return TJobProducer{
    "compile_c",
    {{"o"}},
    bind(GetInputName, _1, false),
    TCompileCFamily::New<false>
  };
}

TJobProducer TCompileCFamily::GetCppProducer() {
  return TJobProducer{
    "compile_cpp",
    {{"o"}},
    bind(GetInputName, _1, true),
    TCompileCFamily::New<true>
  };
}

const char *TCompileCFamily::GetName() {
  if (IsCpp) {
    return "compile_cpp";
  } else {
    return "compile_c";
  }
}

const unordered_set<TFile*> TCompileCFamily::GetNeeds() {

  // Only thing needed is the dep file. The dep file by being done enusures
  // all includes exist for C/C++. Set is constructed in ctor so that we don't construct it all the time.
  return {Need};
}

vector<string> TCompileCFamily::GetCmd() {

  // Build up the gcc call
  // add output, input filenames
  // Tell GCC we're only compiling to a .o
  vector<string> cmd{IsCpp ? "g++" : "gcc", "-o" + GetSoleOutput()->GetPath(), GetInput()->GetPath(), "-c"};

  /* Add standard arguments */ {
    auto std_args = GetStandardArgs(GetInput(), IsCpp, Env);
    for(auto &arg: std_args) {
      cmd.push_back(move(arg));
    }

  }

  // Emit a JSON Compilation Database entry next to the .o output. The
  // collector at jhm.cc end-of-build walks the out tree, reads every
  // *.compdb.json, and unions them into <src>/compile_commands.json
  // for clangd / clang-tidy / IDE consumption. We write per-job rather
  // than accumulating in memory so incremental builds preserve entries
  // for files that weren't touched this run.
  /* compdb entry */ {
    TJson::TArray args;
    args.reserve(cmd.size());
    for (const auto &a : cmd) {
      args.emplace_back(a);
    }
    TJson::TObject entry;
    entry.emplace("directory", AsStr(*Env.GetSrc()));
    entry.emplace("file", GetInput()->GetPath());
    entry.emplace("arguments", TJson(std::move(args)));

    const string out_root = AsStr(*Env.GetOut());
    const std::filesystem::path entry_path =
        std::filesystem::path(out_root) / (GetSoleOutput()->GetPath() + ".compdb.json");
    // GetCmd() runs at queue time, before gcc creates the .o's directory,
    // so we have to ensure the parent exists ourselves.
    std::error_code ec;
    std::filesystem::create_directories(entry_path.parent_path(), ec);
    std::ofstream out(entry_path);
    out << TJson(std::move(entry));
  }

  // Optional compiler launcher (e.g. JHM_COMPILER_LAUNCHER=ccache): prepended
  // to the invocation so the real compile runs as `<launcher> g++ ...`. This
  // is the first-class equivalent of CMake's *_COMPILER_LAUNCHER -- it lets a
  // caching/distributing wrapper sit in front of the compiler without the
  // PATH-symlink masquerade jhm otherwise forces (jhm execs g++/gcc by bare
  // name). Applied only to compiles, not links (ccache caches compiles), and
  // added AFTER the compile-DB entry above so clangd still sees the compiler
  // itself as argument[0]. A single token (the launcher executable); unset or
  // empty leaves the command byte-for-byte unchanged.
  if (const char *launcher = std::getenv("JHM_COMPILER_LAUNCHER"); launcher && *launcher) {
    cmd.insert(cmd.begin(), launcher);
  }

  return cmd;
}


TTimestamp TCompileCFamily::GetCmdTimestamp() const {
  static TTimestamp gcc_timestamp = GetTimestampSearchingPath("gcc");
  static TTimestamp gpp_timestamp = GetTimestampSearchingPath("g++");

  return IsCpp ? gpp_timestamp : gcc_timestamp;
}

bool TCompileCFamily::IsComplete() {

  // Calculate the files which need to be linked against to make a binary with this file.

  TJson::TArray filtered_includes;
  for (const string &include : Need->GetConfig().Read<vector<string>>({"c++","include"})) {
    TFile *include_file = Env.TryGetFileFromPath(include);
    if (include_file) {
      filtered_includes.push_back(AsStr(include_file->GetPath()));
    }
  }

  // is_cpp lets the link job pick g++ vs gcc for the final link (#342): gcc can't resolve
  // C++-only symbols (exception handling, the C++ runtime, name-mangled calls), so linking with it
  // is only safe when nothing in the link set was actually compiled as C++.
  GetSoleOutput()->PushComputedConfig(
      TJson::TObject{{"c++", TJson::TObject{
        {"filtered_includes", move(filtered_includes)},
        {"is_cpp", TJson(IsCpp)}
      }}});

  return true;
}

TCompileCFamily::TCompileCFamily(TEnv &env, TFile *in_file, bool is_cpp)
    : TJob(in_file, {env.GetFile(GetOutputName(in_file->GetRelPath(), is_cpp))}),
      Env(env),
      IsCpp(is_cpp),
      Need(Env.GetFile(TRelPath(AddExtension(TPath(GetInput()->GetRelPath().Path), {"dep"})))) {}
