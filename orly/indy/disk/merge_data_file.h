/* <orly/indy/disk/merge_data_file.h>

   Merges a list of data files (identified by `gen_vec`) into a single
   output file. Sequence numbers and the `Mutator` byte are carried
   through so commutative ops still compose at read time (#49 / #53).
   `GetNumNonAssignEntries()` is consulted by `TSafeRepo::MergeFiles`
   to skip the subsequent `TFoldDataFile` pass when the source has
   nothing to fold (#64).

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

#include <optional>

#include <atomic>
#include <stdexcept>

#include <base/class_traits.h>
#include <orly/atom/kit2.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/out_stream.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/util/index_manager.h>
#include <orly/indy/memory_layer.h>
#include <orly/sabot/all.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      template <bool ScanAheadAllowed>
      using TDataDiskArena = TDiskArena<Util::LogicalBlockSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::PageCheckedBlock, DiskArenaMaxCacheSize, ScanAheadAllowed>;

      class TMergeDataFile {
        NO_COPY(TMergeDataFile);
        public:

        TMergeDataFile(Util::TEngine *engine,
                       Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                       const Base::TUuid &file_uuid,
                       const std::vector<size_t> &gen_vec,
                       const Base::TUuid &file_uid,
                       size_t gen_id,
                       TSequenceNumber /*release_up_to*/,
                       DiskPriority priority,
                       size_t max_block_cache_read_slots_allowed,
                       size_t temp_file_consol_thresh,
                       bool can_tail,
                       bool can_tail_tombstone,
                       std::optional<TSequenceNumber> retain_from = std::nullopt,
                       const std::atomic<bool> *cancel = nullptr);

        /* `cancel`, when set and raised mid-merge, makes the constructor throw TMergeCancelled. */

        /* #745 stage 2: `retain_from`, for a merge that may tail (`can_tail`), is the oldest version reads must still
           find whole: the merge keeps each key's versions newer than it, and its state as of it. Without it, a tailing
           merge keeps each key's newest entry only, as before. A merge that can't tail keeps every version anyway. */

        inline size_t GetNumKeys() const {
          return NumKeys;
        }

        /* Aggregate count of entries written with Mutator != Assign.
           Used by TSafeRepo::MergeFiles to skip the TFoldDataFile pass
           when zero -- there's nothing to fold (#64). */
        inline size_t GetNumNonAssignEntries() const {
          return NumNonAssignEntries;
        }

        /* The most entries with Mutator != Assign that any one key has in the output (#988): an upper bound on the
           run a read of that key walks in this file, if it stays unfolded. */
        inline size_t GetMaxKeyNonAssignEntries() const {
          return MaxKeyNonAssignEntries;
        }

        inline TSequenceNumber GetLowestSequence() const {
          return LowestSeq;
        }

        inline TSequenceNumber GetHighestSequence() const {
          return HighestSeq;
        }

        [[nodiscard]] inline size_t GetFileLength() const noexcept {
          return FileLength;
        }

        /* Test only. When true, a merge writes every input's copy of an equal note into its output
           arena, as release merges did before #666 whenever the copies' unused header bits held
           different garbage. A test sets it to build such a file (#674). Nothing else does. */
        static inline std::atomic<bool> KeepsEqualNotes{false};

        private:

        size_t NumKeys;
        size_t NumNonAssignEntries = 0UL;
        size_t MaxKeyNonAssignEntries = 0UL;
        TSequenceNumber LowestSeq;
        TSequenceNumber HighestSeq;
        size_t FileLength = 0UL;

      };  // TMergeDataFile

    }  // Disk

  }  // Indy

}  // Orly