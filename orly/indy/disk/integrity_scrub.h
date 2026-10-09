/* <orly/indy/disk/integrity_scrub.h>

   Online and offline integrity audit of a store (#748).

   Verifies:
     - Block accounting & sequence ranges (#700): every live file's blocks are owned
       exactly once, none are free or waiting for discard, and sequence ranges within each
       repo are consistent and disjoint.
     - File contents decode: every data file and durable file decodes, with checksums verified
       where the format has them (Murmur checksums per page/block).
     - Key ordering: within each data file index, current keys are verified to be in strictly
       ascending order.
     - Durable entries: every durable file entry decodes with valid UUID, sequence number,
       deadline, and payload size.

   Can be run:
     - Offline via `orlyi --check_only` against a stopped store.
     - Online in the background on a serving store (Low priority I/O, rate-limited, yielding
       periodically), triggered via the reporting port (`POST /scrub` or `GET /scrub`) or
       periodically via `--scrub_interval`.

   Findings are reported via syslog, on the HTTP reporting port, and formatted for stdout
   by Describe() ending in RESULT: ok | problems | UNSAFE.

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
#include <optional>
#include <string>
#include <vector>

#include <base/uuid.h>
#include <orly/indy/disk/file_service_base.h>
#include <orly/indy/disk/open_check.h>
#include <orly/indy/disk/priority.h>
#include <orly/indy/disk/util/engine.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TFileService;

      /* One problem found during an integrity scrub. */
      struct TScrubProblem {

        enum TKind {
          BlockAccounting,
          SeqRange,
          UnreadableFile,
          CorruptPage,
          BadChecksum,
          KeyOrderViolation,
          FormatError
        };

        TKind Kind;
        std::optional<Base::TUuid> FileUid;
        std::optional<size_t> GenId;
        std::string Details;

        const char *GetKindName() const;
        std::string Describe() const;

      };  // TScrubProblem

      struct TScrubOptions {
        DiskPriority Priority = Low;
        size_t YieldEveryNKeys = 256UL;
        size_t MaxProblemsReported = 16UL;
        bool CheckBlockAccounting = true;
        bool CheckContents = true;
      };

      struct TScrubReport {

        size_t NumFiles = 0UL;
        size_t NumDataFiles = 0UL;
        size_t NumDurableFiles = 0UL;
        size_t NumBlocksScrubbed = 0UL;
        size_t NumKeysScrubbed = 0UL;
        size_t NumHistoryKeysScrubbed = 0UL;
        size_t NumDurableEntriesScrubbed = 0UL;

        TOpenCheck OpenCheck;
        std::vector<TScrubProblem> Problems;

        double Seconds = 0.0;

        bool IsSafe() const;
        bool IsClean() const;
        bool HasFatalCorruption() const;

        std::string Describe(size_t max_problems = 16UL) const;
        void Report() const;

      };  // TScrubReport

      TScrubReport RunIntegrityScrub(Util::TVolumeManager *vol_man,
                                     TFileService *file_service,
                                     Util::TPageCache *page_cache,
                                     const std::vector<size_t> &engine_blocks,
                                     const TForEachFileBlockRange &for_each_file_block_range,
                                     const TScrubOptions &options = {},
                                     const std::function<void()> &yield_cb = nullptr);

    }  // Disk

  }  // Indy

}  // Orly
