/* <orly/indy/disk/file_service.h>

   On-disk file registry: maps `(file_uid, gen_id)` to a `TFileObj`
   describing where the file's blocks live on the volume. Inserts and
   removes are queued and applied by a runner fiber so the in-memory
   map stays internally consistent without per-call locking. Reads the
   `image_1`/`image_2`/`append_log` blocks at startup to recover the
   map after a crash. Test counterpart: `test_file_service.h`.

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

#include <cassert>

#include <unistd.h>

#include <base/class_traits.h>
#include <base/event_semaphore.h>
#include <base/scheduler.h>
#include <base/uuid.h>
#include <base/inv_con/unordered_list.h>
#include <orly/indy/disk/buf_block.h>
#include <orly/indy/disk/file_service_base.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TFileService
          : public TFileServiceBase, public Fiber::TRunnable {
        NO_COPY(TFileService);
        public:

        typedef std::function<bool (TFileObj::TKind /*file kind*/,
                                    const Base::TUuid &/*file_uid*/,
                                    size_t /*gen_id*/,
                                    size_t /*starting block id*/,
                                    size_t /*starting block offset*/,
                                    size_t /*file_length*/)> TFileInitCb;

        TFileService(Base::TScheduler *scheduler,
                     Fiber::TRunner::TRunnerCons &runner_cons,
                     Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                     Util::TVolumeManager *vol_man,
                     size_t image_1_block_id,
                     size_t image_2_block_id,
                     const std::vector<size_t> &append_log_block_vec,
                     const TFileInitCb &file_init_cb,
                     bool create = false,
                     bool abort_on_append_log_scan = true);

        ~TFileService();

        virtual void InsertFile(const Base::TUuid &file_uid,
                                TFileObj::TKind file_kind,
                                size_t file_gen,
                                size_t starting_block_id,
                                size_t starting_block_offset,
                                size_t file_size,
                                size_t num_keys,
                                TSequenceNumber lowest_seq,
                                TSequenceNumber highest_seq,
                                TCompletionTrigger &completion_trigger) override;

        virtual void RemoveFile(const Base::TUuid &file_uid,
                                size_t file_gen,
                                TCompletionTrigger &completion_trigger) override;

        virtual bool FindFile(const Base::TUuid &file_uid,
                              size_t file_gen,
                              size_t &out_block_id,
                              size_t &out_block_offset,
                              size_t &out_file_size,
                              size_t &out_num_keys) const override;

        virtual void AppendFileGenSet(const Base::TUuid &file_uid,
                                      std::vector<TFileObj> &out_vec) override;

        virtual bool ForEachFile(const std::function<bool (const Base::TUuid &file_uid, const TFileObj &)> &cb) override;

        inline size_t GetNumFiles() const;

        private:

        typedef std::unordered_map<Base::TUuid, std::unordered_map<size_t, TFileObj>> TFileMap;

        /* Forward Declarations. */
        class TOp;

        typedef InvCon::UnorderedList::TCollection<TFileService, TOp> TOpQueue;

        class TOp {
          NO_COPY(TOp);
          public:

          enum TKind {
            InsertFile,
            RemoveFile
          };

          typedef InvCon::UnorderedList::TMembership<TOp, TFileService> TQueueMembership;

          inline TOp(TKind kind, const Base::TUuid &file_uid, TFileObj &&fil_obj, TCompletionTrigger &trigger);

          inline ~TOp();

          inline void Apply(size_t *buf) const;

          inline void Apply(TFileMap &file_map, size_t &num_elem_in_file_map) const;

          inline TQueueMembership *GetQueueMembership();

          inline void Remove();

          inline void Complete(TDiskResult result, const char *err_str);

          private:

          TQueueMembership::TImpl QueueMembership;

          TKind Kind;

          Base::TUuid FileUUID;

          TFileObj FileObj;

          TCompletionTrigger &Trigger;

        };  // TOp

        void Runner();

        bool TryLoadFromBaseImage(size_t base_image_block, const size_t *cur_buf, TBufBlock *cur_buf_block);

        void ZeroImageBlocks(size_t image_1_block_id, size_t image_2_block_id);

        void ZeroAppendLog();

        static void AddToMap(TFileMap &file_map,
                             const Base::TUuid &file_uid,
                             TFileObj::TKind file_kind,
                             size_t file_gen,
                             size_t starting_block_id,
                             size_t starting_block_offset,
                             size_t file_size,
                             size_t num_keys,
                             TSequenceNumber lowest_seq,
                             TSequenceNumber highest_seq);

        static void RemoveFromMap(TFileMap &file_map,
                                  const Base::TUuid &file_uid,
                                  size_t file_gen);

        static void ApplyDeltasToMap(TFileMap &file_map,
                                     const size_t *buf,
                                     size_t &num_files_in_map);

        static void ApplyImageBlock(TFileMap &file_map,
                                    const size_t *buf,
                                    size_t &num_files_in_map);

        Fiber::TFrame *Frame;
        Fiber::TRunner BGScheduler;

        Util::TVolumeManager *VolMan;

        mutable std::mutex Mutex;

        TFileMap Map;
        size_t NumFiles;

        TFileMap RunnerCopyMap;
        size_t NumRunnerCopyFiles;

        std::vector<size_t> Image1BlockIdVec;
        std::vector<size_t> Image2BlockIdVec;
        std::vector<size_t> AppendLogBlockVec;
        size_t NumAppendLogSectors;
        size_t VersionNumber;
        size_t CurBaseImageCounter;
        size_t CurRingSector;

        std::mutex QueueLock;

        mutable TOpQueue::TImpl OpQueue;

        Base::TEventSemaphore RunSem;
        bool ShuttingDown;

        /* Pushed by the scheduler job hosting BGScheduler's loop when the
           loop returns; the destructor pops it so BGScheduler (a member) is
           never destroyed while its loop can still touch it (#463; same
           handshake as TDurableManager's SchedulerExitedSem). */
        Base::TEventSemaphore SchedulerExitedSem;

        /* A flag used to test abort on append log corruption */
        bool AbortOnAppendLogScan;

        static constexpr size_t VersionSize = sizeof(uint64_t);

        static constexpr size_t NextBlockSize = sizeof(uint64_t);

        static constexpr size_t EntrySize = 10UL;

        static constexpr size_t OpSize = 1UL;

        static constexpr size_t EntryByteSize = EntrySize * sizeof(uint64_t);
        static_assert(sizeof(TFileObj) + sizeof(Base::TUuid) == EntryByteSize, "TFileObj size mismatch");

        static constexpr size_t OpByteSize = sizeof(uint64_t);

        /* Sector size, less version number */
        static constexpr size_t NumFilesPerRingBuf = (Util::LogicalSectorSize - VersionSize) / (EntryByteSize + OpByteSize);

        static constexpr size_t NumSectorsPerBlock = Util::PhysicalBlockSize / Util::PhysicalSectorSize;

        static constexpr size_t NumElemPerBaseImageBlock = (Util::LogicalCheckedBlockSize - VersionSize - NextBlockSize) / EntryByteSize;

      };  // TFileService

      /***************
        *** Inline ***
        *************/

      inline size_t TFileService::GetNumFiles() const {
        return NumFiles;
      }

      inline void TFileService::TOp::Apply(size_t *buf) const {
        uuid_copy(*reinterpret_cast<uuid_t *>(buf), FileUUID.GetRaw());
        buf[2] = FileObj.GenId; // file_gen
        buf[3] = FileObj.StartingBlockId; // starting_block_id
        buf[4] = FileObj.StartingBlockOffset; // starting_block_offset
        buf[5] = FileObj.FileSize; // file_size
        buf[6] = FileObj.NumKeys; // num_keys
        buf[7] = FileObj.LowestSeq; // lowest_seq
        buf[8] = FileObj.HighestSeq; // highest_seq
        buf[9] = static_cast<uint64_t>(FileObj.Kind); // file kind
        buf[10] = static_cast<uint64_t>(Kind);  // op
      }

      inline void TFileService::TOp::Apply(TFileMap &file_map, size_t &num_elem_in_file_map) const {
        switch (Kind) {
          case InsertFile: {
            TFileService::AddToMap(file_map,
                                   FileUUID,
                                   FileObj.Kind,
                                   FileObj.GenId,
                                   FileObj.StartingBlockId,
                                   FileObj.StartingBlockOffset,
                                   FileObj.FileSize,
                                   FileObj.NumKeys,
                                   FileObj.LowestSeq,
                                   FileObj.HighestSeq);
            ++num_elem_in_file_map;
            break;
          }
          case RemoveFile: {
            TFileService::RemoveFromMap(file_map,
                                        FileUUID,
                                        FileObj.GenId);
            --num_elem_in_file_map;
            break;
          }
        }
      }

      inline TFileService::TOp::TOp(TKind kind, const Base::TUuid &file_uid, TFileObj &&file_obj, TCompletionTrigger &trigger)
          : QueueMembership(this), Kind(kind), FileUUID(file_uid), FileObj(file_obj), Trigger(trigger) {
        trigger.WaitForOneMore();
      }

      inline TFileService::TOp::~TOp() {}

      inline TFileService::TOp::TQueueMembership *TFileService::TOp::GetQueueMembership() {
        return &QueueMembership;
      }

      inline void TFileService::TOp::Remove() {
        QueueMembership.Remove();
      }

      inline void TFileService::TOp::Complete(TDiskResult result, const char *err_str) {
        Trigger.Callback(result, err_str);
      }

    }  // Disk

  }  // Indy

}  // Orly
