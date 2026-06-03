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
   Sabot::ToNative), applies the fold via Rt::Mutate, materialises
   the result into a fresh TMemoryLayer, and serialises that layer
   via TDataFile.

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
         (TSafeRepo::MergeFiles) is responsible for the cleanup. */
      class TFoldDataFile {
        NO_COPY(TFoldDataFile);
        public:

        TFoldDataFile(Util::TEngine *engine,
                      Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                      const Base::TUuid &file_uuid,
                      size_t source_gen_id,
                      size_t dest_gen_id,
                      DiskPriority priority,
                      size_t temp_file_consol_thresh);

        inline size_t GetNumKeys() const {
          return NumKeys;
        }

        inline TSequenceNumber GetLowestSequence() const {
          return LowestSeq;
        }

        inline TSequenceNumber GetHighestSequence() const {
          return HighestSeq;
        }

        private:

        size_t NumKeys;
        TSequenceNumber LowestSeq;
        TSequenceNumber HighestSeq;

      };  // TFoldDataFile

    }  // Disk

  }  // Indy

}  // Orly
