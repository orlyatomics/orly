/* <jhm/jobs/bison.cc>

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

#include <jhm/jobs/bison.h>
#include <filesystem>
#include <optional>

#include <jhm/env.h>
#include <jhm/file.h>
#include <jhm/jobs/util.h>

using namespace Base;
using namespace Jhm;
using namespace Jhm::Job;
using namespace std;
using namespace Util;

const static vector<vector<string>> OutExtensions = {
  {"bison","cc"},
  {"bison", "hh"}
};

/* Bison writes its outputs (the .cc, the .hh, and the -rall report)
   incrementally in place, so a dep scan racing this job could read a
   truncated header (#406).  Generate into this temp directory next to the
   target instead; IsComplete() renames everything into place, so a reader
   only ever sees a missing file or a complete one. */
static string GetTmpDir(const string &cc_path) {
  return cc_path + ".tmp.d";
}

TJobProducer TBison::GetProducer() {

  return TJobProducer{
    "bison",
    OutExtensions,
    [](const TRelPath &output) { return TryGetInputName(output, OutExtensions, {"y"}); },
    TBison::New
  };
}

const char *TBison::GetName() {
  return "bison";
}

const unordered_set<TFile*> TBison::GetNeeds() {
  return unordered_set<TFile*>();
}

vector<string> TBison::GetCmd() {
  TFile *primary_output = GetOutputWithExtension(GetOutput(), {"cc"});
  const string &cc_path = primary_output->GetPath();
  const string tmp_dir = GetTmpDir(cc_path);
  const size_t slash = cc_path.rfind('/');
  assert(slash != string::npos);
  /* Clear any leftovers from an earlier failed run. */
  filesystem::remove_all(tmp_dir);
  filesystem::create_directory(tmp_dir);
  /* --file-prefix-map keeps the #line directives and the header's include
     guard pointing at the final paths, so the output is byte-identical to an
     in-place run.  The '#include "<name>.bison.hh"' bison emits into the .cc
     uses the basename only, which the temp directory preserves. */
  return vector<string>{"bison", "-rall",
                        "--file-prefix-map=" + tmp_dir + "/=" + cc_path.substr(0, slash + 1),
                        "-o" + tmp_dir + cc_path.substr(slash),
                        GetInput()->GetPath()};
}


TTimestamp TBison::GetCmdTimestamp() const {
  static TTimestamp timestamp = GetTimestampSearchingPath("bison");
  return timestamp;
}

bool TBison::IsComplete() {
  TFile *cc = GetOutputWithExtension(GetOutput(), {"cc"});
  /* Atomically move everything bison generated into place (see GetTmpDir);
     rename within a directory can't leave a partially written file visible. */
  const filesystem::path tmp_dir = GetTmpDir(cc->GetPath());
  const filesystem::path out_dir = tmp_dir.parent_path();
  for (const auto &entry : filesystem::directory_iterator(tmp_dir)) {
    filesystem::rename(entry.path(), out_dir / entry.path().filename());
  }
  filesystem::remove(tmp_dir);
  cc->PushComputedConfig(
      TJson::TObject{{"cmd", TJson::TObject{{"g++", Env.GetConfig().GetEntry({"cmd", "bison", "g++"})}}}});
  return true;
}


TBison::TBison(TEnv &env, TFile *in_file)
    : TJob(in_file, GetOutputSet(OutExtensions, env, in_file->GetRelPath())), Env(env) {}