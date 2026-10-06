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

#include <atomic>
#include <cassert>
#include <optional>

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

        /* Calls ShutDown(). */
        ~TFileService();

        /* Stop the runner and cancel-or-join the scheduler job hosting BGScheduler's loop. The
           destructor does this too; call it first when the destructor will run after the
           scheduler is gone. orlyi destroys the disk engine (and so this) in ~TServer, after
           RunUntilCtrlC has destroyed the scheduler, so TServer::Shutdown() calls this while the
           scheduler is still alive (#648). Idempotent. Call it only once nothing will queue
           more file ops. */
        void ShutDown();

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

        /* Appends the blocks the file service itself owns: both base images (head, chain and
           spare) and the append log (#700). */
        void AppendOwnBlocks(std::vector<size_t> &out) const;

        private:

        typedef std::unordered_map<Base::TUuid, std::unordered_map<size_t, TFileObj>> TFileMap;

        /* Grow a base image's block list toward 'target' blocks. Short of 'required' on a full
           disk, waits for space instead of aborting (#590). */
        void GrowBaseImage(std::vector<size_t> &image_block_vec, size_t target, size_t required);

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

          /* For a removal: the file as the map held it, to put back if the removal fails. */
          std::optional<TFileObj> Removed;

          TKind GetKind() const {
            return Kind;
          }

          const Base::TUuid &GetFileUUID() const {
            return FileUUID;
          }

          private:

          TQueueMembership::TImpl QueueMembership;

          TKind Kind;

          Base::TUuid FileUUID;

          TFileObj FileObj;

          TCompletionTrigger &Trigger;

        };  // TOp

        void Runner();

        /* Complete every op still queued with 'result', without writing anything. */
        void CompleteQueuedOps(TDiskResult result, const char *err_str);

        /* Complete an op whose change did not reach the disk. A removal puts its file back in the
           map: the disk may still name it, so its blocks stay with it (#621). */
        void FailOp(TOp *op, TDiskResult result, const char *err_str);

        /* Load the file map from the base image whose head block is already in 'cur_buf'. On
           success, 'chain_out' holds the image's blocks after the head, in order. */
        bool TryLoadFromBaseImage(size_t base_image_block, const size_t *cur_buf, TBufBlock *cur_buf_block, std::vector<size_t> &chain_out);

        /* Read a base image's head and follow its chain without loading it. Returns true, with
           the blocks after the head in 'chain_out', only if the image was written and every block
           of its chain reads cleanly with the head's version (#610). */
        bool ReadImageChain(size_t head_block_id, std::vector<size_t> &chain_out);

        /* Mark a base image's chain used and append it to the image's block vector, which holds
           only the head on reopen, so the next image written there reuses or frees those blocks.
           If another owner already holds one of them, keeps none (#610). */
        void AdoptImageChain(size_t head_block_id, const std::vector<size_t> &chain, std::vector<size_t> &image_block_vec);

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

        /* The scheduler the BGScheduler host job was queued on, and its
           cancellation handle: the destructor cancels a host that never got
           a worker instead of waiting on a latch it will never push (#462). */
        Base::TScheduler *Scheduler;
        Base::TScheduler::TJobHandle SchedulerHostHandle;

        Util::TVolumeManager *VolMan;

        mutable std::mutex Mutex;

        TFileMap Map;
        size_t NumFiles;

        TFileMap RunnerCopyMap;
        size_t NumRunnerCopyFiles;

        /* Guards the image block vectors, which the runner changes as images grow and shrink,
           for AppendOwnBlocks (#700). Never held across I/O or a yield. */
        mutable std::mutex ImageLock;
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

        /* Set by the runner when a file-map change could not be made durable: an fsync (or a
           write) of the append log or a base image failed (#621). From then on the runner writes
           nothing more and completes every op with an error; the in-memory map keeps serving
           reads, and a restart reloads the map from what reached the disk. */
        std::atomic<bool> Failed;

        /* Pushed by the scheduler job hosting BGScheduler's loop when the
           loop returns; the destructor pops it so BGScheduler (a member) is
           never destroyed while its loop can still touch it (#463; same
           handshake as TDurableManager's SchedulerExitedSem). */
        Base::TEventSemaphore SchedulerExitedSem;

        /* Pushed by Runner() just before it frees its own frame. A runner whose host job was
           cancelled never runs, so the destructor frees its frame instead (#631). */
        Base::TEventSemaphore RunnerExitedSem;

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
