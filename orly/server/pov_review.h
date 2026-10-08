/* <orly/server/pov_review.h>

   Reviewing a POV's changes before they reach its parent (#746): diff, discard, promote, and
   conflict tracking.  See docs/pov-review.md.

   A child POV holds the writes Tetris hasn't yet promoted to its parent: its backlog, all in
   memory.  Paused, a POV keeps them, so the review workflow is:

     new fast private pov from {parent} <{.conflicts: "report"}>;   -- fork, tracking conflicts
     pause {pov};                                                   -- hold its writes back
     ... writes ...
     diff_pov {pov};                                                -- what it changed
     promote_pov {pov};    or    discard_pov {pov};                 -- keep them, or drop them
     review_pov {pov};                                              -- promotion progress, conflicts

   The diff reads the POV's own repo only, in its own sequence order (each repo numbers its
   updates itself, #791), and the parent through the parent's view; it never compares sequence
   numbers across repos.

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

#include <cstdint>

#include <orly/indy/fork_watch.h>
#include <orly/indy/manager.h>
#include <orly/pov_review.h>

namespace Orly {

  namespace Server {

    /* Starts tracking conflicts for the POV whose repo is `repo`, made just now: its fork.  A
       no-op for TConflictMode::None.  Throws for the global POV. */
    void WatchFork(const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, TConflictMode mode);

    /* A page of what the POV whose repo is `repo` changed relative to its parent: its unpromoted
       updates, key by key, in key order.  Throws for the global POV, which has no parent. */
    TPovDiff DiffPov(const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, const TPovDiffOptions &options);

    /* Throws away the POV's unpromoted updates, as of the call, so it reads as its parent again,
       and frees them (once the repo's next memory merge runs, which this queues).  Pauses the
       POV while it works and restores its status after.  Throws for a failed POV. */
    TPovDiscard DiscardPov(Indy::TManager *repo_manager, const Indy::L0::TManager::TPtr<Indy::TRepo> &repo);

    /* Asks for the POV's updates to be promoted: unpauses it.  In refusing mode, first tests every
       unpromoted update against the conflicts, and leaves the POV as it was if any would conflict,
       unless `force`, which lets every update unpromoted now through. */
    TPovPromote PromotePov(Indy::TManager *repo_manager, const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, bool force);

    /* The POV's promotion progress and the conflicts numbered after `after`. */
    TPovReview ReviewPov(const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, uint64_t after = 0UL);

  }  // Server

}  // Orly
