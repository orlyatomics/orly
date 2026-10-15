/* <orly/indy/disk/fold_data_file.h>

   Post-merge compaction-time fold pass for same-mutator commutative
   runs.

   Reads a source data file (typically the just-produced output of
   TMergeDataFile), folds runs of same-key entries that share a
   commutative+associative mutator (Add, Mult, And, Or, Xor, Union,
   Intersection, SymmetricDiff -- the same set TMutation::Augment
   validates), and writes a new data file in which each fold collapses
   to a single Assign entry holding the resolved value.

   This is phase 4 of #49. The read-path fold in
   TContext::TPresentWalker::ApplyDeferredFold handles un-compacted
   runs correctly already; this rewrite pass exists to bring read
   amplification back to O(1) per key on workloads (the
   wikipedia-pageviews demo's hot counters being the motivating one)
   where many deferred-Add entries accumulate.

   Architecture: doesn't touch the in-flight merge state machine in
   TMergeDataFile (which is forward-only and tied to a complex
   offset-remap pipeline). Instead it iterates the source file via
   the existing TReader cursors into typed space (Var::TVar via
   Sabot::ToNative), one key at a time, applies the fold via
   Rt::Mutate, and hands the surviving entries to TDataFile as an
   entry source (no memory layer, no update pools).

   Output entries are always promoted to TMutator::Assign:
   identity-zero (commutative-mutator identity) folded onto the
   accumulated RHS gives the same value as Assign(folded_RHS), and
   Assign keeps disk-side readers that aren't Mutator-aware (#53)
   working unchanged.

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

#include <atomic>
#include <optional>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <orly/atom/kit2.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/disk/util/volume_manager.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      /* Compaction-time fold pass. Reads a source data file at
         (file_uuid, source_gen_id), folds same-mutator commutative
         runs, writes a new data file at (file_uuid, dest_gen_id).
         The source file is not modified or removed -- the caller
         (TSafeRepo::MergeFiles) is responsible for the cleanup.

         The fold walks the source key by key, holding one key's versions
         at a time, and writes its output straight from the heap: it takes
         nothing from the update pools (#745 §9.6, which retires the pool
         claim of #858). */
      class TFoldDataFile {
        NO_COPY(TFoldDataFile);
        public:

        TFoldDataFile(Util::TEngine *engine,
                      Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                      const Base::TUuid &file_uuid,
                      size_t source_gen_id,
                      size_t dest_gen_id,
                      DiskPriority priority,
                      size_t temp_file_consol_thresh,
                      std::optional<TSequenceNumber> retain_from = std::nullopt,
                      const std::atomic<bool> *cancel = nullptr);

        /* #745 stage 2: with `retain_from`, the fold keeps every version newer than it (reads at any version from
           `retain_from` on stay whole) and folds only the ones at or below it. Within the window, every
           KeyframeInterval-th delta of a run whose value the file determines is written as an Assign of that value,
           so a read replays at most KeyframeInterval deltas. Without it, each key keeps one entry, as before. */
        static constexpr size_t KeyframeInterval = 64UL;

        inline size_t GetNumKeys() const {
          return NumKeys;
        }

        /* The sequence range the output's file-map entry records: the source's whole range,
           which covers every entry the fold kept (#618). */
        inline TSequenceNumber GetLowestSequence() const {
          return LowestSeq;
        }

        inline TSequenceNumber GetHighestSequence() const {
          return HighestSeq;
        }

        [[nodiscard]] inline size_t GetFileLength() const noexcept {
          return FileLength;
        }

        private:

        size_t NumKeys;
        TSequenceNumber LowestSeq;
        TSequenceNumber HighestSeq;
        size_t FileLength = 0UL;

      };  // TFoldDataFile

    }  // Disk

  }  // Indy

}  // Orly
