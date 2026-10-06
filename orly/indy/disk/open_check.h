/* <orly/indy/disk/open_check.h>

   Consistency checks of a store's disk state, run at open in every build (#700).

   Block accounting: every block the volumes hold (marked used and not waiting for discard)
   should belong to exactly one owner: the system block, the file service's base images and
   append log, or a data or durable file in the file map. Two kinds of mismatch:

     - Leaked: held, but nothing owns it (#620's class). It wastes space and corrupts nothing,
       so it is logged, loudly, and the store opens.
     - Unheld: owned by a file still in the map or by the file service, but free or waiting for
       discard (#610's class); or owned twice. The next allocation can hand such a block to a
       new file, which writes over live data, and a discard can erase it. That is corruption in
       the making, so the open is refused (TOpenCheckFailed) unless the check is turned off.

   Sequence ranges: the data files of one repo must cover disjoint sequence ranges, each with
   lowest <= highest. TSafeRepo::ReConstructFromDisk drops a merge's leftover inputs, which lie
   inside its output; anything else that overlaps is reported.

   Cost: one read of each file's block list (already in the page cache after the startup walk)
   and one pass over each volume's block map, plus a bit per block.

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

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <base/uuid.h>
#include <orly/indy/disk/file_service_base.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TFileService;

      namespace Util {
        class TVolumeManager;
      }

      /* One problem with the sequence ranges of a repo's data files. */
      struct TSeqRangeProblem {

        enum TKind {

          /* lowest > highest. */
          Inverted,

          /* One file's range lies inside another's: a merge's input left behind by a crash,
             which reload drops. Expected after a crash; anything else that does this is a bug. */
          Nested,

          /* The ranges overlap, but neither lies inside the other. A fold output written before
             #618, or corruption. */
          PartialOverlap

        };

        TKind Kind;

        /* The file, and the one it overlaps (equal to GenId for Inverted). */
        size_t GenId;
        size_t OtherGenId;

        /* "inverted", "nested" or "partial overlap". */
        const char *GetKindName() const;

        /* A one-line description. */
        std::string Describe() const;

      };  // TSeqRangeProblem

      /* Every problem with 'files', the data files of one repo; durable files are skipped.
         O(n log n): sorted by lowest, each file is compared with the one reaching furthest so
         far, so a file overlapping several is reported once. Empty when the ranges are
         disjoint and none is inverted, that is, when they are strictly monotone. */
      std::vector<TSeqRangeProblem> FindSeqRangeProblems(const std::vector<TFileObj> &files);

      /* Calls 'cb' with each block range a file occupies, read from its metadata. */
      using TForEachFileBlockRange = std::function<void (const Base::TUuid &file_uid, const TFileObj &file,
                                                         const std::function<void (const std::pair<size_t, size_t> &range)> &cb)>;

      /* What CheckOpenConsistency found. */
      struct TOpenCheck {

        size_t NumFiles = 0UL;

        /* Blocks owned, counting each once, and blocks held. */
        size_t NumOwned = 0UL;
        size_t NumHeld = 0UL;

        /* Held, owned by nothing. */
        std::vector<size_t> Leaked;

        /* Owned, but free or waiting for discard. */
        std::vector<size_t> Unheld;

        /* Owned more than once. */
        std::vector<size_t> Shared;

        /* Files whose block lists could not be read, with the error. Their blocks show up as
           leaked. */
        std::vector<std::string> Unreadable;

        /* Sequence range problems, by repo. */
        std::vector<std::pair<Base::TUuid, TSeqRangeProblem>> SeqProblems;

        /* A disk merge held a space claim while the check ran. Its output holds blocks before
           it reaches the file map, so leaked blocks may be that output. */
        bool MergeRunning = false;

        double Seconds = 0.0;

        /* True if nothing was found that makes continuing unsafe: no unheld or shared blocks. */
        bool IsSafe() const {
          return Unheld.empty() && Shared.empty();
        }

        /* True if nothing at all was found. */
        bool IsClean() const {
          return IsSafe() && Leaked.empty() && Unreadable.empty() && SeqProblems.empty();
        }

      };  // TOpenCheck

      /* Thrown by ReportOpenCheck when continuing would corrupt data. */
      class TOpenCheckFailed
          : public std::runtime_error {
        public:

        explicit TOpenCheckFailed(const std::string &msg)
            : std::runtime_error(msg) {}

      };  // TOpenCheckFailed

      /* Checks the block accounting and sequence ranges described above. 'engine_blocks' are
         the blocks the engine owns outside the file service (the system block). Must run on a
         fiber, since it reads files; meant for a quiet store. To keep a running merge or file
         removal from being reported, an unheld block counts only if its owner is still in the
         map and the block is still not held when re-checked, and a leaked block only if no file
         added since the first look owns it and it is still held. */
      TOpenCheck CheckOpenConsistency(Util::TVolumeManager *vol_man,
                                      TFileService *file_service,
                                      const std::vector<size_t> &engine_blocks,
                                      const TForEachFileBlockRange &for_each_file_block_range);

      /* Logs what 'check' found: one LOG_INFO line when it is clean, LOG_ERR lines for each
         kind of problem (sequence ranges: LOG_WARNING for a nested pair). Throws
         TOpenCheckFailed when !check.IsSafe(). */
      void ReportOpenCheck(const TOpenCheck &check);

    }  // Disk

  }  // Indy

}  // Orly
