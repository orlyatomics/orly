/* <orly/indy/disk/indy_util_reporter.h>

   I/O accounting for the disk layer. `TIndyUtilReporter` implements
   `TUtilizationReporter` by tallying bytes and operations per `Source`
   (`DataFile`, `MergeDataFile`, `DurableSortFile`, `PresentWalk`,
   `UpdateWalk`, ...) split by `SyncRead` / `AsyncRead` / `Write`.
   `Report()` emits the accumulated counters into a stringstream for
   status pages and oncall dashboards.

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
#include <cassert>
#include <sstream>

#include <base/timer.h>
#include <orly/indy/disk/utilization_reporter.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      enum Source : uint8_t {
        BlockService,
        DataFileArena,
        DataFileHash,
        DataFileHashIndex,
        DataFileHistory,
        DataFileKey,
        DataFileMeta,
        DataFileNoteIndex,
        DataFileOther,
        DataFileRemapIndex,
        DataFileUpdate,
        DataFileUpdateIndex,
        DiskArena,
        DurableFetch,
        DurableMergeFileData,
        DurableMergeFileHash,
        DurableMergeFileHashIndex,
        DurableMergeFileOther,
        DurableMergeFileScan,
        DurableSortFileData,
        DurableSortFileHash,
        DurableSortFileHashIndex,
        DurableSortFileOther,
        FileRemoval,
        FileService,
        FileSync,
        MergeDataFileArena,
        MergeDataFileHash,
        MergeDataFileHashIndex,
        MergeDataFileHistory,
        MergeDataFileKey,
        MergeDataFileMeta,
        MergeDataFileOther,
        MergeDataFileRemapIndex,
        MergeDataFileScan,
        MergeDataFileTailIndex,
        MergeDataFileUpdate,
        MergeDataFileUpdateIndex,
        PresentWalk,
        RepoLoader,
        SlaveSlush,
        System,
        UpdateScoop,
        UpdateWalk,
      };

      class TIndyUtilReporter : public TUtilizationReporter {
        NO_COPY(TIndyUtilReporter);
        public:

        TIndyUtilReporter() = default;

        virtual ~TIndyUtilReporter() {}

        virtual void Push(uint8_t /*source*/, TUtilizationReporter::TKind /*kind*/, size_t /*num_bytes*/, DiskPriority /*priority*/);

        virtual void Report(std::stringstream &ss);

        private:

        const char *GetName(uint8_t) const;

        Base::TTimer ReportTimer;

        static const size_t NumFields = UpdateWalk + 1;

        std::atomic<size_t> SyncReadNumByte[NumFields];
        std::atomic<size_t> AsyncReadNumByte[NumFields];
        std::atomic<size_t> WriteNumByte[NumFields];
        std::atomic<size_t> SyncReadOps[NumFields];
        std::atomic<size_t> AsyncReadOps[NumFields];
        std::atomic<size_t> WriteOps[NumFields];

      };  // TIndyUtilReporter

    }  // Disk

  }  // Indy

}  // Orly