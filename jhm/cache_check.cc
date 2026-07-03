/* <jhm/cache_check.cc>

   Implements <jhm/cache_check.h>.

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

#include <jhm/cache_check.h>

#include <set>
#include <unordered_map>
#include <vector>

#include <base/as_str.h>
#include <jhm/env.h>
#include <jhm/file.h>
#include <jhm/job.h>
#include <jhm/work_finder.h>
#include <base/util/path.h>
#include <base/util/stl.h>

using namespace Base;
using namespace Jhm;
using namespace std;
using namespace Util;

string Jhm::GetCacheFilename(const TFile *file) {
  return AsStr(file->GetPath()) + ".jhm-cache";
}

/* Adds the config files which were actually loaded for the file to the set. */
static void AppendSrcConfigFiles(set<string> &out, const TFile *file) {
  const auto &files = file->GetConfig().GetSrcConfigFiles();
  out.insert(files.begin(), files.end());
}

vector<string> Jhm::BuildConfigFileList(const TEnv &env, TJob *job) {
  set<string> files;
  const auto &env_files = env.GetConfig().GetSrcConfigFiles();
  files.insert(env_files.begin(), env_files.end());
  AppendSrcConfigFiles(files, job->GetInput());
  for (TFile *need : job->GetNeeds()) {
    AppendSrcConfigFiles(files, need);
  }
  for (TFile *out_file : job->GetOutput()) {
    AppendSrcConfigFiles(files, out_file);
  }
  return {files.begin(), files.end()};
}

template<typename TVal>
static auto GrabOne(const std::unordered_set<TVal> &container) {
  assert(container.size() > 0);
  for(const TVal &item : container) {
    return item;
  }
  __builtin_unreachable();
}

static TOptTimestamp GetTimestampOutput(TFile *file) {
  assert(file);
  auto ret = file->GetTimestamp();
  if (!file->IsSrc()) {
    ret = Oldest(ret, TryGetTimestamp(GetCacheFilename(file)));
  } else {
    assert(ret);
  }
  return ret;
}

static auto GetTimestampInput(TFile *file) {
  return *GetTimestampOutput(file);
}

bool TCacheChecker::HasChecked(TJob *job) const {
  return Contains(Checked, job);
}

TFile *TCacheChecker::TryGetOutputFileFromPath(const std::string &filename) {
  TFile *ret = Env.TryGetFileFromPath(filename);
  // If we aren't buildable, try finding the executable form.
  if (ret && !WorkFinder.IsBuildable(ret)) {
    ret = Env.GetFile(TRelPath(AddExtension(TPath(ret->GetRelPath().Path), {""})));
  }
  return ret;
}

bool TCacheChecker::Check(TJob *job) {
  assert(job);
  /* For a job to cache-complete, it must meet several requirements
     1. The environmental config must be older than the newest output
     2. The oldest input or need must be newer than the newest output
     3. Every input/need's job must be finished (If they're in out/)
     4. Every output has a cache file which matches the file. Said cache file has an equal or newer timestamp.
     5. Every output's producer job matches this job.

    NOTES:
      - For jobs with unknown outputs, read the set of outputs in from the out file. If we succeed in finding them all,
        set them for the job.
      - A file is considered to have the timstamp of newest('file', 'file.jhm'), if file.jhm exists */

  // We've now checked this job for cache. Mark it so.
  InsertOrFail(Checked, job);

  // TODO(#350): There will be a lot of redundant stat() calls on input files... Make TFile hold a timestamp which is set
  // by the cache checking process.

  // Input must be finished.
  // Also: Grab the timestamp while we're at it.
  TFile *in = job->GetInput();
  if (!WorkFinder.IsFileDone(in)) {
    return false;
  }

  /* The config files which exist right now for the env and the job's files. Collected along the
     walk below and compared against the list the cache recorded, so a config file disappearing
     (or appearing with an old mtime, which the timestamp checks can't see) forces a rebuild (#339). */
  set<string> current_config_files;
  /* env + input */ {
    const auto &env_files = Env.GetConfig().GetSrcConfigFiles();
    current_config_files.insert(env_files.begin(), env_files.end());
    AppendSrcConfigFiles(current_config_files, in);
  }

  // If config is newer than the source, exit fast.
  auto in_timestamp = GetTimestampInput(in);

  // If the input file is in the source tree, then it's timestamp relative to the config doesn't matter.
  // The newest of the two is considered the file's timestamp for comparison to the output files.
  // NOTE: All job outputs are always not src, so they don't need this check.
  if (in->IsSrc()) {
    in_timestamp = Newest(ConfigTimestamp, in_timestamp);
  } else {
    // If we're in output, we must be newer than the config timestamp, or we need ot be rebuilt
    if (ConfigTimestamp > in_timestamp) {
      return false;
    }
  }

  // If the job's command is new / updated, we need to rebuild
  in_timestamp = Newest(job->GetCmdTimestamp(), in_timestamp);

  // For all known outputs (There is always at least one), choose one at random and load it's cache file / treat it as
  // the magic cache entry.
  TFile *base_out = GrabOne(job->GetOutput());

  TConfig ideal_out;
  /* Read the cache as what we want  / need for build_info sections. */ {
    // Read the cache as our ideal cache contents.
    string cache_filename = GetCacheFilename(base_out);
    if (!ExistsPath(cache_filename.c_str())) {
      return false;
    }
    ideal_out.LoadComputed(cache_filename);
  }

  // Cache the computed config we've loaded so we only load it once.
  unordered_map<TFile *, vector<Base::TJson>> conf_cache;
  // List of expected outputs
  vector<string> output_filename_list;

  try {
    const string in_name = AsStr(job->GetInput()->GetPath());
    // Check the ideal out matches the job
    if (ideal_out.Read<string>({"build_info","job","name"}) != job->GetName() ||
        ideal_out.Read<string>({"build_info","job","input"}) != in_name ||
        ideal_out.Read<string>({"build_info","config_timestamp"}) != ConfigTimestampStr) {
      return false;
    }

    // Every needs must exist, be done. in_timestamp is the newest of all the needs and the input file.
    for (const string &need_filename : ideal_out.Read<vector<string>>({"build_info","job","needs"})) {
      TFile *need = Env.TryGetFileFromPath(need_filename);
      if (!need || !WorkFinder.IsFileDone(need)) {
        return false;
      }
      in_timestamp = Newest(in_timestamp, GetTimestampInput(need));
      AppendSrcConfigFiles(current_config_files, need);
    }

    // Any files which weren't buildable that need to stay not buildable should stay not buildable.
    for (const string &filename : ideal_out.Read<vector<string>>({"build_info","job","anti_needs"})) {
      TFile *anti_need = Env.TryGetFileFromPath(filename);
      if (anti_need && WorkFinder.IsBuildable(anti_need)) {
        return false;
      }
    }

    output_filename_list = ideal_out.Read<vector<string>>({"build_info","job","output"});

    // The output sets should match. We first check here they're the same size
    // Then in the output loop immediately following this, we ensure that one contains every item in the other.
    if (!job->HasUnknownOutputs()) {
      if (output_filename_list.size() != job->GetOutput().size()) {
        return false;
      }
    }


    // Make sure every output is older than the input
    // Also ensure it's basic build info matches.
    for (const string &output_filename : output_filename_list) {
      TFile *output = TryGetOutputFileFromPath(output_filename);
      if (!output) {
        return false;
      }
      if (!Util::ExistsPath(AsStr(output->GetPath()).c_str())) {
        return false;
      }
      if (IsNewer(in_timestamp, GetTimestampOutput(output))) {
        return false;
      }

      // NOTE: Technically all build info should match exactly. But this should be good enough (and faster).
      // Check the ideal out matches the job
      TConfig output_cache;
      output_cache.LoadComputed(GetCacheFilename(output));
      if (output_cache.Read<string>({"build_info","job","name"}) != job->GetName() ||
          output_cache.Read<string>({"build_info","job","input"}) != in_name) {
        return false;
      }

      InsertOrFail(conf_cache, output, output_cache.GetComputed());
      AppendSrcConfigFiles(current_config_files, output);
    }

    // The exact set of config files which fed the cached build must still be the exact set which
    // exists now: a recorded file that's gone (removal) or a present file the cache never saw
    // whose mtime predates the outputs (addition the timestamps can't catch) both invalidate.
    // NOTE: Caches written before this key existed fail the Read below (TNotFound) and rebuild.
    if (ideal_out.Read<vector<string>>({"build_info","config_files"})
        != vector<string>(current_config_files.begin(), current_config_files.end())) {
      return false;
    }
  } catch (const TConfig::TNotFound &ex) {
    // If any of the config files don't contain the requested key(s), then exit / the config must be out of date.
    return false;
  }

  // Wohoo! Everything checked out.
  // If the input job doesn't have it's full output set, add it.
  // For every file in the output set, add the cached config
  for (const string &output_filename : output_filename_list) {
    TFile *out_file = TryGetOutputFileFromPath(output_filename);
    if (job->HasUnknownOutputs()) {
      if (!Contains(job->GetOutput(), out_file)) {
        job->AddOutput(out_file);
      }
    }
    out_file->SetComputed(move(conf_cache.at(out_file)));
  }

  if (job->HasUnknownOutputs()) {
    job->MarkAllOutputsKnown();
  }

  return true;
}
