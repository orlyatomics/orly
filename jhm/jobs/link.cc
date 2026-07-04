/* <jhm/jobs/link.cc>

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

#include <jhm/jobs/link.h>

#include <optional>
#include <queue>

#include <base/split.h>
#include <jhm/env.h>
#include <jhm/file.h>

using namespace Base;
using namespace Jhm;
using namespace Jhm::Job;
using namespace std;
using namespace Util;

static TRelPath GetOutputName(const TRelPath &input) {
  assert(input.Path.EndsWith({"o"}));
  return TRelPath(SwapExtension(TPath(input.Path), {""}));
}

static std::optional<TRelPath> GetInputName(const TRelPath &output) {
  // An executable's extension list ends in a bare "" (see base/path.h's design notes), but may
  // have real components before it (e.g. a config-qualified name). Drop only the trailing "" and
  // append "o", preserving any such prefix instead of discarding the whole extension list (#342).
  if (!output.Path.EndsWith({""})) {
    return std::optional<TRelPath>();
  }
  return TRelPath(AddExtension(DropExtension(TPath(output.Path), 1), {"o"}));
}

TJobProducer TLink::GetProducer() {
  return TJobProducer{
    "link",
    {{""}},
    GetInputName,
    TLink::New
  };
}

const char *TLink::GetName() {
  return "link";
}

const unordered_set<TFile*> TLink::GetNeeds() {

  // Seed the link search with the core input file.
  if (ObjFiles.empty()) {
    TFile *input = GetInput();
    ObjToCheck.insert(input);
    ObjFiles.insert(input);
  }

  // We build a queue so we can fill ObjToCheck as we go, and add more to be processed while we iterate.
  std::queue<TFile *> to_check;
  for (TFile *obj: ObjToCheck) {
    to_check.push(obj);
  }
  ObjToCheck.clear();

  // Get all the link objects of every link object until we have a complete set of link objects.
  vector<string> filtered_includes; // Hoisted out of loop
  while (!to_check.empty()) {
    TFile *obj = Pop(to_check);
    if (!Env.IsDone(obj)) {
      ObjToCheck.insert(obj);
      continue;
    }

    // Read out the cached link args, add them to our link set.
    if (!obj->GetConfig().TryRead({"c++","filtered_includes"}, filtered_includes)) {
      continue;
    }

    for(const auto &include: filtered_includes) {
      TFile *include_file = Env.TryGetFileFromPath(include);
      assert(include_file);
      if (!include_file) {
        THROW_ERROR(std::logic_error)
            << "Internal Error; We didn't find the C++ source file which should be in the src tre...";
      }
      TFile *obj_file = Env.GetFile(TRelPath(SwapExtension(TPath(include_file->GetRelPath().Path), {"o"})));
      if (Env.IsBuildable(obj_file)) {
        // Add the link. If it's new, queue it to be checked for new links that we need
        if (ObjFiles.insert(obj_file).second) {
          to_check.push(obj_file);
        }
      } else {
        AntiNeeds.insert(obj_file);
      }
    }
  }

  return ObjFiles;
}

unordered_set<TFile*> TLink::GetAntiNeeds() {
  return AntiNeeds;
}

vector<string> TLink::GetCmd() {

  // Use gcc unless something in the link set was actually compiled as C++ (#342): gcc can't
  // resolve C++-only symbols, so this is only safe because "any object is C++" is a strictly
  // weaker requirement than "gcc would fail" -- a false positive (calling g++ for an all-C link)
  // is harmless, it's only the reverse that would break.
  bool any_cpp = false;
  for (TFile *f : ObjFiles) {
    bool is_cpp = false;
    if (f->GetConfig().TryRead({"c++", "is_cpp"}, is_cpp) && is_cpp) {
      any_cpp = true;
      break;
    }
  }

  vector<string> cmd{any_cpp ? "g++" : "gcc", "-o" + GetSoleOutput()->GetPath()};
  for (auto &flag: Env.GetConfig().Read<vector<string>>({"cmd","ld","flags"})) {
    cmd.push_back(move(flag));
  }

  // Link against every needed object file
  for(TFile *f: ObjFiles) {
    cmd.push_back(f->GetPath());
  }

  // NOTE: every binary links every configured lib regardless of whether its object files actually
  // reference one (#342, declined as an open-ended feature: see the issue rationale). "-Wl,--no-
  // as-needed" in root.jhm's ld flags already keeps the linker from dropping the "unused" ones on
  // its own, so this is the existing, deliberate behavior, not an oversight.
  for (auto &lib: Env.GetConfig().Read<vector<string>>({"cmd","ld","libs"})) {
    cmd.push_back(move(lib));
  }

  return cmd;
}

TTimestamp TLink::GetCmdTimestamp() const {
  static TTimestamp timestamp = GetTimestampSearchingPath("g++");
  return timestamp;
}

bool TLink::IsComplete() {

  return true;
}


TLink::TLink(TEnv &env, TFile *in_file)
    : TJob(in_file, {env.GetFile(GetOutputName(in_file->GetRelPath()))}), Env(env) {}
