/* <orly/indy/disk/file_service_base.h>

   Abstract base for `TFileService` (production) and `TTestFileService`
   (tests). Defines the file-registry API -- `InsertFile`, `RemoveFile`,
   `FindFile`, `AppendFileGenSet`, `ForEachFile` -- plus the `TFileObj`
   value type that describes one (`DataFile` | `DurableFile`) on disk.

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
#include <orly/indy/disk/result.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TFileObj {
        public:

        enum TKind {
          DataFile,
          DurableFile
        };

        inline TFileObj(TKind kind,
                        size_t gen_id,
                        size_t starting_block,
                        size_t starting_block_offset,
                        size_t file_size,
                        size_t num_keys,
                        TSequenceNumber lowest_seq,
                        TSequenceNumber highest_seq);

        TKind Kind;

        size_t GenId;

        size_t StartingBlockId;

        size_t StartingBlockOffset;

        size_t FileSize;

        size_t NumKeys;

        TSequenceNumber LowestSeq;

        TSequenceNumber HighestSeq;

      };  // TFileObj

      class TFileServiceBase {
        NO_COPY(TFileServiceBase);
        public:

        virtual bool FindFile(const Base::TUuid &file_uid,
                              size_t gen_id,
                              size_t &starting_block,
                              size_t &starting_block_offset,
                              size_t &file_length,
                              size_t &num_keys) const = 0;

        virtual void InsertFile(const Base::TUuid &file_uid,
                                TFileObj::TKind kind,
                                size_t gen_id,
                                size_t starting_block_id,
                                size_t starting_block_offset,
                                size_t file_size,
                                size_t num_keys,
                                TSequenceNumber lowest_seq,
                                TSequenceNumber highest_seq,
                                TCompletionTrigger &completion_trigger) = 0;

        virtual void RemoveFile(const Base::TUuid &file_uid,
                                size_t gen_id,
                                TCompletionTrigger &completion_trigger) = 0;

        virtual void AppendFileGenSet(const Base::TUuid &file_uid,
                                      std::vector<TFileObj> &out_vec) = 0;

        virtual bool ForEachFile(const std::function<bool (const Base::TUuid &file_uid, const TFileObj &)> &cb) = 0;

        virtual ~TFileServiceBase() {}

        protected:

        TFileServiceBase() {}

      };  // TFileServiceBase

      /***************
        *** Inline ***
        *************/

      inline TFileObj::TFileObj(TKind kind,
                                size_t gen_id,
                                size_t starting_block_id,
                                size_t starting_block_offset,
                                size_t file_size,
                                size_t num_keys,
                                TSequenceNumber lowest_seq,
                                TSequenceNumber highest_seq)
          : Kind(kind),
            GenId(gen_id),
            StartingBlockId(starting_block_id),
            StartingBlockOffset(starting_block_offset),
            FileSize(file_size),
            NumKeys(num_keys),
            LowestSeq(lowest_seq),
            HighestSeq(highest_seq) {}

    }  // Disk

  }  // Indy

}  // Orly