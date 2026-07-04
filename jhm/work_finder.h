/* <jhm/work_finder.h>

   Finds and then runs all the work needed to get out all needed files.

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

#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <jhm/cache_check.h>
#include <jhm/job_runner.h>
#include <jhm/naming.h>
#include <base/util/time.h>

namespace Jhm {

  class TEnv;
  class TJob;
  class TFile;

  /* Finds all the currently buildable leaves and queues them for the subproc_runner to fire off. */
  class TWorkFinder {
    NO_COPY(TWorkFinder);
    NO_MOVE(TWorkFinder);

    public:
    TWorkFinder(uint32_t worker_count,
                bool print_cmd,
                Util::TTimestamp config_timestamp,
                TEnv &env)
        : Env(env),
          CacheChecker(*this, env, config_timestamp),
          Runner(worker_count, print_cmd) {}

    /* Adds the jobs needed to produce the file. Returns true if there is work to be done for the file to exist.
       Adds to the ToFinish multimap of the given job if job is specified. */
    bool AddNeededFile(TFile *file, TJob *job = nullptr);

    /* Runs jobs to make all the needed files, queuing more jobs as needed. Returns true on success*/
    bool FinishAll();

    bool IsBuildable(TFile *file);
    bool IsFileDone(TFile *file);

    private:
    /* Writes a single-line status message. */
    void WriteStatusLine() const;

    bool IsDone(TJob *job);

    void ProcessReady();

    /* Process the given result. Returns true if the result of the job is failure / we're shutting down now. */
    bool ProcessResult(TJobRunner::TResult &result);

    /* Queues the given job to run. If the job is already done, returns false. */
    bool Queue(TJob *job);

    /* Returns the producer for a file.
        if file.IsSrc() -> nullptr
        if file is unproducable -> nullptr
        if file is found to have multiple producers -> throw

        NOTE: A job which we never try to get the producer explicitly for can have multiple producers just fine. */
    TJob *TryGetProducer(TFile *file);

    // List of jobs which are ready to go. Only ever touched from FinishAll()'s single thread --
    // the actual n-way parallelism lives in TJobRunner's subprocess pool below, not here -- so a
    // concurrent work-stealing structure (the 2014-era ask, #347) would gain nothing; verified and
    // declined.
    std::queue<TJob *> Ready;

    // Map from producer to consumer job. When the last instance of a consumer job is removed from the multimap, it's
    // time to
    // check for more dependencies. A min-heap (the 2014-era ask, #347) would only pay off if jobs
    // needed priority ordering; nothing here does (every ready job is equally eligible to run
    // immediately), so it's declined -- verified alongside the entry above.
    std::unordered_multimap<TJob *, TJob *> ToFinish;

    // Keeps track of the full set of jobs which have ever been queued to run.
    std::unordered_set<TJob *> All, Running, Finished;

    // A map of jobs to the number of other jobs they need to complete before they should be attempted again.
    std::unordered_map<TJob *, uint64_t> Waiting;

    // Set of output files with jobs which can theoretically be reached via some arbitrary chain of other jobs from some
    // input file.
    std::unordered_map<TFile *, TJob *> Producers;

    // Where inputs come from / outputs go / file lookup.
    TEnv &Env;

    // Decides whether jobs can cache-complete instead of run (#336).
    TCacheChecker CacheChecker;

    // Runs the work queuPops things off work queue
    /* A failed job's partial outputs are deleted in ProcessResult (#346).
       Running jobs in their own process group (the other half of that old
       note) is deliberately NOT done: it would detach children from the
       terminal's group, so Ctrl-C on jhm would orphan running compiles;
       the runner already tracks its children explicitly. */
    // Launch the IO pump, wait/manage all the subprocesses, and notify the work finder / env when things are completed.
    // Calls a standardized callback on completion of each command.
    TJobRunner Runner;
  };
}
