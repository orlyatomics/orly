/* <jhm/cache_check.h>

   Checks whether a job's existing outputs are still good, so the job can be
   cache-completed instead of run.

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

#include <string>
#include <unordered_set>
#include <vector>

#include <base/class_traits.h>
#include <base/util/time.h>

namespace Jhm {

  class TEnv;
  class TFile;
  class TJob;
  class TWorkFinder;

  /* Name of the cache file which stores the build info for the given output file. */
  std::string GetCacheFilename(const TFile *file);

  /* Sorted, deduped list of every config file which existed and was loaded for the env and for the
     job's input, needs, and outputs. Recorded in the job's cache and recomputed on cache check, so
     removing a config file (not just adding one) invalidates the cache (#339). */
  std::vector<std::string> BuildConfigFileList(const TEnv &env, TJob *job);

  /* Decides whether a job actually needs to run or whether the output that
     already exists is good enough (a "cache completion"). Pulled out of the
     work finder (#336): scheduling lives there, cache validation lives here.
     The checker talks back to the work finder for file done-ness /
     buildability and to the env for file lookup. */
  class TCacheChecker final {
    NO_COPY(TCacheChecker);
    NO_MOVE(TCacheChecker);

    public:
    TCacheChecker(TWorkFinder &work_finder, TEnv &env, Util::TTimestamp config_timestamp)
        : ConfigTimestamp(config_timestamp),
          ConfigTimestampStr(Util::ToStr(config_timestamp)),
          Env(env),
          WorkFinder(work_finder) {}

    /* String form of the environment config timestamp, as stored in cache files. */
    const std::string &GetConfigTimestampStr() const {
      return ConfigTimestampStr;
    }

    /* True iff Check() has already been called for the job. */
    bool HasChecked(TJob *job) const;

    /* Checks to see if we actually need to run the job or if the output that already exists is good enough.
       Returns true if the job cache-completes; the work finder marks it finished. Call at most once per job
       (guard with HasChecked). */
    bool Check(TJob *job);

    private:
    TFile *TryGetOutputFileFromPath(const std::string &filename);

    // Keeps track of jobs we've tried cache completing.
    std::unordered_set<TJob *> Checked;

    const Util::TTimestamp ConfigTimestamp;
    const std::string ConfigTimestampStr;
    TEnv &Env;
    TWorkFinder &WorkFinder;
  };
}
