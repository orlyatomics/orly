/* <orly/indy/disk/file_service.cc>

   Implements <orly/indy/disk/file_service.h>.

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

#include <orly/indy/disk/file_service.h>

#include <chrono>
#include <thread>
#include <unordered_set>

#include <base/mem_aligned_ptr.h>
#include <orly/indy/disk/indy_util_reporter.h>

using namespace std;
using namespace Orly::Indy::Disk;

TFileService::TFileService(Base::TScheduler *scheduler,
                           Fiber::TRunner::TRunnerCons &runner_cons,
                           Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                           Util::TVolumeManager *vol_man,
                           size_t image_1_block_id,
                           size_t image_2_block_id,
                           const std::vector<size_t> &append_log_block_vec,
                           const TFileInitCb &file_init_cb,
                           bool create,
                           bool abort_on_append_log_scan)
    : Frame(nullptr),
      BGScheduler(runner_cons),
      Scheduler(scheduler),
      VolMan(vol_man),
      NumFiles(0UL),
      NumRunnerCopyFiles(0UL),
      AppendLogBlockVec(append_log_block_vec),
      NumAppendLogSectors(AppendLogBlockVec.size() * NumSectorsPerBlock),
      VersionNumber(0UL),
      CurBaseImageCounter(0UL),
      CurRingSector(0UL),
      OpQueue(this),
      ShuttingDown(false),
      Failed(false),
      AbortOnAppendLogScan(abort_on_append_log_scan) {
  /* allocate event pools */
  Image1BlockIdVec.push_back(image_1_block_id);
  Image2BlockIdVec.push_back(image_2_block_id);
  /* The head block of the image the map was loaded from, if any, and the rest of its chain. */
  size_t loaded_image_block = -1;
  std::vector<size_t> loaded_chain;
  if (!create) {
    auto image_1_buf_block = make_unique<TBufBlock>();
    auto image_2_buf_block = make_unique<TBufBlock>();
    /* read both base images and use the one with the higher version number */

    size_t version_1 = 0UL;
    size_t version_2 = 0UL;
    bool both_images_loaded_success = true;
    try {
      TCompletionTrigger trigger;
      VolMan->ReadBlock(HERE, Util::CheckedBlock, Source::FileService, image_1_buf_block->GetData(), image_1_block_id, RealTime, trigger,
                        false /* don't abort on error since we're trying to build a valid image */);
      trigger.Wait();
      const size_t *buf_1 = reinterpret_cast<const size_t *>(image_1_buf_block->GetData());
      version_1 = buf_1[0];
    } catch (const TDiskError &/*err*/) {
      both_images_loaded_success = false;
    }
    try {
      TCompletionTrigger trigger;
      VolMan->ReadBlock(HERE, Util::CheckedBlock, Source::FileService, image_2_buf_block->GetData(), image_2_block_id, RealTime, trigger,
                        false /* don't abort on error since we're trying to build a valid image */);
      trigger.Wait();
      const size_t *buf_2 = reinterpret_cast<const size_t *>(image_2_buf_block->GetData());
      version_2 = buf_2[0];
    } catch (const TDiskError &/*err*/) {
      both_images_loaded_success = false;
    }
    TCompletionTrigger trigger;
    const size_t *buf_1 = reinterpret_cast<const size_t *>(image_1_buf_block->GetData());
    const size_t *buf_2 = reinterpret_cast<const size_t *>(image_2_buf_block->GetData());
    bool process_next_block = true;
    const size_t *cur_buf = nullptr;
    TBufBlock *cur_buf_block = nullptr;
    size_t cur_image_block = -1;
    const size_t *alternate_buf = nullptr;
    TBufBlock *alternate_buf_block = nullptr;
    size_t alternate_image_block = -1;
    //size_t alternate_version_number = 0UL;
    /* Until an image loads, CurBaseImageCounter names the newest image. Once one has loaded, it is
       set to name the other one (below). */
    if (version_1 > version_2) {
      VersionNumber = version_1;
      CurBaseImageCounter = 0UL;
      cur_buf = buf_1;
      cur_buf_block = image_1_buf_block.get();
      cur_image_block = image_1_block_id;
      alternate_buf = buf_2;
      alternate_buf_block = image_2_buf_block.get();
      alternate_image_block = image_2_block_id;
      //alternate_version_number = version_2;
    } else if (version_2 > version_1) {
      VersionNumber = version_2;
      CurBaseImageCounter = 1UL;
      cur_buf = buf_2;
      cur_buf_block = image_2_buf_block.get();
      cur_image_block = image_2_block_id;
      alternate_buf = buf_1;
      alternate_buf_block = image_1_buf_block.get();
      alternate_image_block = image_1_block_id;
      //alternate_version_number = version_1;
    } else if (version_1 == version_2 && version_1 == 0UL) {
      /* we're early on in this file system's life, to the point where only the append log has any data */
      VersionNumber = 0UL;
      CurBaseImageCounter = 0UL;
      process_next_block = false;
      //ZeroImageBlocks(image_1_block_id, image_2_block_id);
    }
    if (process_next_block) {
      if (TryLoadFromBaseImage(cur_image_block, cur_buf, cur_buf_block, loaded_chain)) {
        loaded_image_block = cur_image_block;
      } else {
        /* the load was not successful, at this point we need to:
           - see if the other alternate block loaded successfully
           - see what the first version number (fvn) is in the append log.
           - if the fvn is the next expected version after the alternate image block, then we can
             recover from there... only losing what data was in the most recent image block
           - otherwise this corrupt base image was later followed by append logs and we can not
             recover without greater data loss, so we throw */
        if (!both_images_loaded_success) {
          throw std::runtime_error("File System is corrupt. Base Image was irrecoverable, alternate image was also corrupt");
        }
        auto append_log_first_buf_block = make_unique<TBufBlock>();
        /* read the first append log entry to get its version. */
        assert(AppendLogBlockVec.size());
        size_t block_of_first_append_log = AppendLogBlockVec[0];

        /* read the head sector of the append log to figure out which version number it has. */
        VolMan->Read(HERE,
                     Util::CheckedSector,
                     Source::FileService,
                     append_log_first_buf_block->GetData(),
                     (block_of_first_append_log * Util::PhysicalBlockSize),
                     Util::PhysicalSectorSize,
                     RealTime,
                     trigger,
                     false /* We think of sectors as atomic. If our sector read fails we're favoring abort over recovering an old state. we throw in
                     the trigger and abort on the wait*/);
        try {
          trigger.Wait();
        } catch (const TDiskError &ex) {
          syslog(LOG_ERR, "File System is corrupt. Caught Disk Corruption in first entry of append log.");
          if (AbortOnAppendLogScan) {
            abort();
          }
          throw std::runtime_error("File System is corrupt. Caught Disk Corruption in first entry append log.");
        }
        const size_t *first_append_log_buf = reinterpret_cast<const size_t *>(append_log_first_buf_block->GetData());
        const size_t first_append_log_version = first_append_log_buf[0];
        const size_t alternate_image_block_version = alternate_buf[0];
        if (alternate_image_block_version == 0UL && first_append_log_version == 1UL) {
          /* the alternate block was empty and the append log starts at v1, this means all our changes
             (except the last one in the most recent image block) are recoverable by playing back the
             append log. Start that from nothing: drop whatever the failed load applied, and replay
             from version 1, not from the failed image's version (#616). CurBaseImageCounter still
             names the failed image, which is where the next image goes; writing it to the empty
             one would give both images the same version. */
          Map.clear();
          NumFiles = 0UL;
          RunnerCopyMap.clear();
          NumRunnerCopyFiles = 0UL;
          VersionNumber = 0UL;
        } else if (first_append_log_version == alternate_image_block_version + 1) {
          /* we can recover using the alternate base image block and replaying the append log from there. */
          process_next_block = true;
          VersionNumber = alternate_image_block_version;
          if (!TryLoadFromBaseImage(alternate_image_block, alternate_buf, alternate_buf_block, loaded_chain)) {
            throw std::runtime_error("File System is corrupt. Both base images are irrecoverable");
          }
          loaded_image_block = alternate_image_block;
        } else {
          throw std::runtime_error("File System is corrupt. Current base image is irrecoverable.");
        }
      }
    }
    /* The next base image goes to the image the map was NOT loaded from (#616). While running,
       the file service alternates, so the image being written is never the one the append log
       follows; if that write is cut short, the other image plus the log still load. After a
       restart the same must hold: the append log now starts right after the loaded image, so
       writing over that one would leave neither image the log can be replayed onto. */
    if (loaded_image_block == image_1_block_id) {
      CurBaseImageCounter = 1UL;
    } else if (loaded_image_block == image_2_block_id) {
      CurBaseImageCounter = 0UL;
    }
    /* now spin over the append only log and grab all the valid sectors with their deltas */
    std::vector<std::unique_ptr<TBufBlock>> append_log_buf_vec;
    std::vector<size_t> ring_offset_vec;
    for (auto block_id : AppendLogBlockVec) {
      append_log_buf_vec.emplace_back(new TBufBlock());
      VolMan->Read(HERE,
                   Util::SectorCheckedBlock,
                   Source::FileService,
                   append_log_buf_vec.back()->GetData(),
                   (block_id * Util::PhysicalBlockSize),
                   Util::PhysicalBlockSize,
                   RealTime,
                   trigger,
                   false /* We think of sectors as atomic. If our sector read fails we're favoring abort over recovering an old state. we throw in the
                   trigger and abort on the wait*/);
    }

    for (size_t i = 0; i < NumAppendLogSectors; ++i) {
      ring_offset_vec.push_back((AppendLogBlockVec[i / NumSectorsPerBlock] * Util::PhysicalBlockSize) + ((i % NumSectorsPerBlock) * Util::PhysicalSectorSize));
    }
    try {
      trigger.Wait();
    } catch (const TDiskError &ex) {
      syslog(LOG_ERR, "File System is corrupt. Caught Disk Corruption in append log.");
      if (AbortOnAppendLogScan) {
        abort();
      }
      throw std::runtime_error("File System is corrupt. Caught Disk Corruption in append log.");
    }
    /* if one of the base images was corrupt, and the first version number (fvn) in the append log is greater than our base image version number (bivn) by more than 1, then we're in a corrupt state */ {
      const size_t *buf = reinterpret_cast<size_t *>(append_log_buf_vec[0]->GetData());
      const size_t fvn = buf[0];
      if (!both_images_loaded_success && fvn > VersionNumber && fvn - VersionNumber > 1) {
        throw std::runtime_error("File System is corrupt. Append log is past available base image. The required (alternate) base is corrupt");
      }
    }

    std::lock_guard<std::mutex> lock(Mutex);
    bool done = false;
    size_t expected_version = VersionNumber + 1UL;
    for (const auto &buf_block : append_log_buf_vec) {
      for (size_t i = 0; i < NumSectorsPerBlock; ++i) {
        const size_t offset_in_buf_of_sector = i * Util::PhysicalSectorSize;
        const size_t *buf = reinterpret_cast<size_t *>(buf_block->GetData() + offset_in_buf_of_sector);
        const size_t version = *buf;
        if (version == expected_version) {
          VersionNumber = version;
          ++expected_version;
          buf += 1UL;
          ApplyDeltasToMap(Map, buf, NumFiles);
          ApplyDeltasToMap(RunnerCopyMap, buf, NumRunnerCopyFiles);
          ++CurRingSector;
        } else {
          done = true;
          break;
        }
      }
      if (done) {
        break;
      }
    }
  } else {
    /* make sure we zero out both image blocks, and all the append log blocks */
    ZeroImageBlocks(image_1_block_id, image_2_block_id);
    ZeroAppendLog();
  }
  /* if we reconstructed from a cold image, this is the opportunity for us to give each file a chance to mark its blocks as used. */
  if (!create) {
    std::lock_guard<std::mutex> lock(Mutex);
    bool keep_going = true;
    for (const auto &uid_map : Map) {
      for (const auto &file_pair : uid_map.second) {
        const auto &file = file_pair.second;
        keep_going = file_init_cb(file.Kind, uid_map.first, file_pair.first, file.StartingBlockId, file.StartingBlockOffset, file.FileSize);
        if (!keep_going) {
          break;
        }
      }
      if (!keep_going) {
        break;
      }
    }
  }
  /* Our own blocks next: the rest of each base image's chain (#610). The system block records
     only each image's head, which the engine marks used; a chain block nobody marks looks free,
     and a data file written over it breaks that image. Keep the chain of the image the map was
     loaded from, and of the other image if its chain is intact, since that one is the fallback.
     A broken chain is left alone: its links can't be trusted, and it can't be loaded anyway.
     This runs after the files have marked theirs, so a block both claim stays with the file. */
  if (!create && loaded_image_block != static_cast<size_t>(-1)) {
    for (size_t image : {0UL, 1UL}) {
      const size_t head_block_id = image ? image_2_block_id : image_1_block_id;
      auto &image_block_vec = image ? Image2BlockIdVec : Image1BlockIdVec;
      std::vector<size_t> chain;
      if (head_block_id == loaded_image_block) {
        chain = loaded_chain;
      } else if (!ReadImageChain(head_block_id, chain)) {
        continue;
      }
      AdoptImageChain(head_block_id, chain, image_block_vec);
    }
  }
  SchedulerHostHandle = scheduler->ScheduleCancelable([this, frame_pool_manager] {
    Fiber::LaunchSlowFiberSched(&BGScheduler, frame_pool_manager);
    SchedulerExitedSem.Push();
  });
  Frame = Fiber::TFrame::LocalFramePool->Alloc();
  try {
    Frame->Latch(&BGScheduler, this, static_cast<Fiber::TRunnable::TFunc>(&TFileService::Runner));
  } catch (...) {
    Fiber::TFrame::LocalFramePool->Free(Frame);
    throw;
  }
  //Scheduler->Schedule(std::bind(&TFileService::Runner, this));
}

TFileService::~TFileService() {
  ShutDown();
}

void TFileService::ShutDown() {
  /* The handle is cleared below once the host is cancelled or joined, so a second call (the
     destructor after TServer::Shutdown()) touches neither the scheduler, which may be gone by
     then, nor the exited latch, which was pushed only once (#648). */
  if (!SchedulerHostHandle) {
    return;
  }
  ShuttingDown = true;
  RunSem.Push();
  BGScheduler.ShutDown();
  /* Cancel-or-join the job hosting BGScheduler's loop: the ShutDown()
     above is only a flag, and member destruction below would otherwise
     race a loop still on its way out (#463) -- while a host that never
     got a worker can neither be waited for nor be allowed to start late
     against the dying members (#462). */
  if (!Scheduler->Cancel(SchedulerHostHandle)) {
    SchedulerExitedSem.Pop();
  }
  SchedulerHostHandle = nullptr;
}

void TFileService::InsertFile(const Base::TUuid &file_uid,
                              TFileObj::TKind file_kind,
                              size_t file_gen,
                              size_t starting_block_id,
                              size_t starting_block_offset,
                              size_t file_size,
                              size_t num_keys,
                              TSequenceNumber lowest_seq,
                              TSequenceNumber highest_seq,
                              TCompletionTrigger &trigger) {
  /* Acquire Mutex */ {
    std::lock_guard<std::mutex> lock(Mutex);
    AddToMap(Map,
             file_uid,
             file_kind,
             file_gen,
             starting_block_id,
             starting_block_offset,
             file_size,
             num_keys,
             lowest_seq,
             highest_seq);
    ++NumFiles;
  }  // release Mutex
  TOp *op = new TOp(TOp::InsertFile,
                    file_uid,
                    std::move(TFileObj(file_kind,
                                       file_gen,
                                       starting_block_id,
                                       starting_block_offset,
                                       file_size,
                                       num_keys,
                                       lowest_seq,
                                       highest_seq)),
                    trigger);
  try {
    /*Acquire Queue lock */ {
      std::lock_guard<std::mutex> lock(QueueLock);
      /* No-throws only */
      OpQueue.Insert(op->GetQueueMembership());
    }  // release Queue lock
  } catch (...) {
    /* The op registered itself with the trigger; complete it so a waiter (or the trigger's
       destructor, which waits too) isn't left one completion short (#590). */
    op->Complete(TDiskResult::Error, "file service op not queued");
    delete op;
    throw;
  }
  RunSem.Push();
}

void TFileService::RemoveFile(const Base::TUuid &file_uid,
                              size_t file_gen,
                              TCompletionTrigger &trigger) {
  std::optional<TFileObj> removed;
  /* acquire Mutex */ {
    std::lock_guard<std::mutex> lock(Mutex);
    if (auto uid_iter = Map.find(file_uid); uid_iter != Map.end()) {
      if (auto gen_iter = uid_iter->second.find(file_gen); gen_iter != uid_iter->second.end()) {
        removed = gen_iter->second;
      }
    }
    RemoveFromMap(Map,
                  file_uid,
                  file_gen);
    --NumFiles;
  }  // release Mutex
  TOp *op = new TOp(TOp::RemoveFile,
                    file_uid,
                    std::move(TFileObj(TFileObj::TKind::DataFile /* not used */,
                                       file_gen,
                                       0UL,
                                       0UL,
                                       0UL,
                                       0UL,
                                       0UL,
                                       0UL)),
                    trigger);
  op->Removed = removed;
  try {
    /*Acquire Queue lock */ {
      std::lock_guard<std::mutex> lock(QueueLock);
      /* No-throws only */
      OpQueue.Insert(op->GetQueueMembership());
    }  // release Queue lock
  } catch (...) {
    /* The op registered itself with the trigger; complete it so a waiter (or the trigger's
       destructor, which waits too) isn't left one completion short (#590). */
    op->Complete(TDiskResult::Error, "file service op not queued");
    delete op;
    throw;
  }
  RunSem.Push();
}

bool TFileService::FindFile(const Base::TUuid &file_uid,
                            size_t file_gen,
                            size_t &out_block_id,
                            size_t &out_block_offset,
                            size_t &out_file_size,
                            size_t &out_num_keys) const {
  std::lock_guard<std::mutex> lock(Mutex);
  auto uid_ret = Map.find(file_uid);
  if (uid_ret != Map.end()) {
    const auto &gen_map = uid_ret->second;
    auto gen_ret = gen_map.find(file_gen);
    if (gen_ret != gen_map.end()) {
      const auto &file = gen_ret->second;
      out_block_id = file.StartingBlockId;
      out_block_offset = file.StartingBlockOffset;
      out_file_size = file.FileSize;
      out_num_keys = file.NumKeys;
      return true;
    }
    return false;
  }
  return false;
}

void TFileService::AppendFileGenSet(const Base::TUuid &file_uid,
                                    std::vector<TFileObj> &out_vec) {
  std::lock_guard<std::mutex> lock(Mutex);
  auto uid_ret = Map.find(file_uid);
  if (uid_ret != Map.end()) {
    const auto &gen_map = uid_ret->second;
    for (const auto &fi : gen_map) {
      const auto &file = fi.second;
      out_vec.emplace_back(file);
    }
  }
}

bool TFileService::ForEachFile(const std::function<bool (const Base::TUuid &file_uid, const TFileObj &)> &cb) {
  std::lock_guard<std::mutex> lock(Mutex);
  for (auto uid_map : Map) {
    for (auto file : uid_map.second) {
      if (!cb(uid_map.first, file.second)) {
        return false;
      }
    }
  }
  return true;
}

void TFileService::Runner() {
  assert(Util::PhysicalBlockSize >= Util::PhysicalSectorSize);
  /* allocate event pools */
  if (!Disk::Util::TDiskController::TEvent::LocalEventPool) {
    Disk::Util::TDiskController::TEvent::LocalEventPool = new Base::TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
  }
  std::vector<size_t> ring_offset_vec;
  for (size_t i = 0; i < NumAppendLogSectors; ++i) {
    ring_offset_vec.push_back((AppendLogBlockVec[i / NumSectorsPerBlock] * Util::PhysicalBlockSize) + ((i % NumSectorsPerBlock) * Util::PhysicalSectorSize));
  }
  auto ring_buf = Base::MemAlignedAlloc<size_t>(getpagesize(), getpagesize());
  std::vector<std::unique_ptr<TBufBlock>> image_buf_block_vec;
  image_buf_block_vec.emplace_back(new TBufBlock());
  TCompletionTrigger append_log_flush_trigger;
  /* What the ops of a failed round, and every op after it, are completed with (#621). */
  static constexpr const char *FailedErr = "file service: a file-map change could not be made durable; file-map changes are refused until restart";
  try {
    for (;;) {
      RunSem.Pop();
      if (!ShuttingDown && Failed) {
        CompleteQueuedOps(Error, FailedErr);
      } else if (!ShuttingDown) {
        TOpQueue::TImpl cur_queue(this);
        size_t to_apply = 0UL;
        const size_t cur_version_num = ++VersionNumber;
        /* Acquire Queue lock */ {
          std::lock_guard<std::mutex> lock(QueueLock);
          TOp *op = nullptr;
          memset(ring_buf.get(), 0, Util::PhysicalSectorSize);
          size_t *buf = ring_buf.get();
          *buf = cur_version_num;
          buf += 1;
          for (to_apply = 0; to_apply < NumFilesPerRingBuf; ++to_apply, buf += (EntrySize + OpSize)) {
            op = OpQueue.TryGetFirstMember();
            if (op) {
              op->Apply(RunnerCopyMap, NumRunnerCopyFiles);
              op->Apply(buf);
              op->Remove();
              cur_queue.Insert(op->GetQueueMembership());
            } else {
              break;
            }
            if (to_apply > 0) {
              RunSem.Pop();
            }
          }
        }  // release Queue lock
        try {
          if (CurRingSector == ring_offset_vec.size()) { /* we've filled the current ring buffer */
            TCompletionTrigger trigger;
            /* write out a new base image */
            const size_t blocks_required_for_base_image = std::max(1UL, static_cast<size_t>(ceil(static_cast<double>(NumRunnerCopyFiles) / NumElemPerBaseImageBlock)));
            auto &cur_image_block_vec = (CurBaseImageCounter % 2 == 0) ? Image1BlockIdVec : Image2BlockIdVec;
            /* Each image keeps spare blocks beyond what it needs, so a growing file count rarely
               needs a fresh allocation, and never one that a full disk can refuse while the
               spare lasts (#590). Spare blocks are not written or linked into the chain; after
               a restart they are simply free again. The chain itself is kept across a restart
               (#610), so after one this vector starts out as exactly the image's chain. */
            const size_t spare = std::max(2UL, blocks_required_for_base_image / 8UL);
            const size_t target = blocks_required_for_base_image + spare;
            if (cur_image_block_vec.size() > target + spare) {
              for (size_t i = cur_image_block_vec.size() - 1; i >= target; --i) {
                VolMan->FreeSequentialBlocks(Util::TBlockRange(cur_image_block_vec[i], 1UL));
              }
              cur_image_block_vec.resize(target);
            } else if (cur_image_block_vec.size() < target) {
              GrowBaseImage(cur_image_block_vec, target, blocks_required_for_base_image);
            }
            if (image_buf_block_vec.size() > blocks_required_for_base_image) {
              image_buf_block_vec.resize(blocks_required_for_base_image);
            } else if (image_buf_block_vec.size() < blocks_required_for_base_image) {
              const size_t to_add = blocks_required_for_base_image - image_buf_block_vec.size();
              for (size_t i = 0; i < to_add; ++i) {
                image_buf_block_vec.emplace_back(new TBufBlock());
              }
            }
            const std::unordered_map<size_t, TFileObj> empty_gen_map {};
            auto uid_iter = RunnerCopyMap.cbegin();
            const auto uid_end = RunnerCopyMap.cend();
            auto gen_iter = (uid_iter != uid_end) ? uid_iter->second.cbegin() : empty_gen_map.cbegin();
            auto gen_end = (uid_iter != uid_end) ? uid_iter->second.cend() : empty_gen_map.cend();
            size_t num_done = 0UL;
            std::vector<Util::TBlockRange> flush_block_range_vec;
            for (size_t pos = 0; pos < blocks_required_for_base_image; ++pos) {
              size_t *buf = reinterpret_cast<size_t *>(image_buf_block_vec[pos]->GetData());
              memset(buf, 0, Util::PhysicalBlockSize);
              *buf = cur_version_num;
              buf += 1;
              *buf = (pos != blocks_required_for_base_image - 1) ? cur_image_block_vec[pos + 1] : -1;
              buf += 1;
              for (size_t idx = 0; idx < NumElemPerBaseImageBlock && num_done < NumRunnerCopyFiles; ++idx, ++num_done) {
                assert(uid_iter != uid_end);
                const TFileObj &file = gen_iter->second;
                assert(reinterpret_cast<char *>(buf) < image_buf_block_vec[pos]->GetData() + Util::PhysicalBlockSize);
                uuid_copy(*reinterpret_cast<uuid_t *>(buf), uid_iter->first.GetRaw());
                *(buf + 2) = file.GenId; // file_gen
                *(buf + 3) = file.StartingBlockId; // starting_block_id
                *(buf + 4) = file.StartingBlockOffset; // starting_block_offset
                *(buf + 5) = file.FileSize; // file_size
                *(buf + 6) = file.NumKeys; // num_keys
                *(buf + 7) = file.LowestSeq; // lowest_seq
                *(buf + 8) = file.HighestSeq; // highest_seq
                *(buf + 9) = static_cast<uint64_t>(file.Kind); // file_kind
                buf += EntrySize;
                ++gen_iter;
                if (gen_iter == gen_end) {
                  ++uid_iter;
                  if (uid_iter != uid_end) {
                    gen_iter = uid_iter->second.cbegin();
                    gen_end = uid_iter->second.cend();
                    assert(gen_iter != gen_end);
                  }
                }
              }
              flush_block_range_vec.emplace_back(Util::TBlockRange{cur_image_block_vec[pos], 1UL});
              VolMan->WriteBlock(HERE,
                                 Util::CheckedBlock,
                                 Source::FileService,
                                 image_buf_block_vec[pos]->GetData(),
                                 cur_image_block_vec[pos],
                                 RealTime,
                                 Util::TCacheInstr::NoCache,
                                 trigger);
            }
            assert(num_done == NumRunnerCopyFiles);
            assert(uid_iter == uid_end);
            trigger.Wait();
            VolMan->SyncToDisk(flush_block_range_vec);
            CurRingSector = 0UL;
            ++CurBaseImageCounter;
          } else {
            assert(to_apply > 0);
            assert(ring_offset_vec.size() > CurRingSector);
            assert(ring_offset_vec[CurRingSector] % Util::PhysicalSectorSize == 0);
            VolMan->WriteAndFlush(HERE,
                                  Util::CheckedSector,
                                  Source::FileService,
                                  reinterpret_cast<char *>(ring_buf.get()),
                                  ring_offset_vec[CurRingSector],
                                  Util::PhysicalSectorSize,
                                  RealTime,
                                  Util::TCacheInstr::NoCache,
                                  append_log_flush_trigger);
            ++CurRingSector;
          }
          for(;;) {
            TOp *op = cur_queue.TryGetFirstMember();
            if (op) {
              op->Complete(Success, nullptr);
              delete op;
            } else {
              break;
            }
          }
        } catch (const TDiskServiceShutdown &) {
          /* This round's change may be half written; write nothing after it. */
          Failed = true;
          for(;;) {
            TOp *op = cur_queue.TryGetFirstMember();
            if (op) {
              FailOp(op, ServerShutdown, nullptr);
            } else {
              break;
            }
          }
        } catch (const std::exception &ex) {
          /* A write or sync of the append log or a base image failed, most likely an fsync (#621).
             After a failed fsync, nothing written to that device since its last good sync can be
             trusted to be on disk, and fsync may not report the loss again, so retrying is not
             safe; nor is carrying on, because the next sector or image would build on one whose
             contents are unknown. So: fail this round's ops, and refuse every later change. The
             map in memory still has them and keeps serving reads; a restart reloads what is on
             disk. The runner itself stays up, so no caller is left waiting. */
          syslog(LOG_CRIT, "TFileService: file-map change failed [%s]; refusing all file-map changes until restart", ex.what());
          Failed = true;
          const TDiskResult result = dynamic_cast<const TDiskFailure *>(&ex) ? DiskFailure : Error;
          for(;;) {
            TOp *op = cur_queue.TryGetFirstMember();
            if (op) {
              FailOp(op, result, FailedErr);
            } else {
              break;
            }
          }
          CompleteQueuedOps(Error, FailedErr);
        }
      } else {
        break;
      }
    }
  } catch (const std::exception &ex) {
    CompleteQueuedOps(ServerShutdown, nullptr);
    delete Disk::Util::TDiskController::TEvent::LocalEventPool;
    Disk::Util::TDiskController::TEvent::LocalEventPool = nullptr;
    throw;
  }
  /* Ops queued after the last round would otherwise wait forever. */
  CompleteQueuedOps(ServerShutdown, nullptr);
  delete Disk::Util::TDiskController::TEvent::LocalEventPool;
  Disk::Util::TDiskController::TEvent::LocalEventPool = nullptr;
  Fiber::FreeMyFrame(Fiber::TFrame::LocalFramePool);
}

void TFileService::CompleteQueuedOps(TDiskResult result, const char *err_str) {
  for (;;) {
    TOp *op = nullptr;
    /* Acquire Queue lock */ {
      std::lock_guard<std::mutex> lock(QueueLock);
      op = OpQueue.TryGetFirstMember();
      if (op) {
        op->Remove();
      }
    }  // release Queue lock
    if (!op) {
      break;
    }
    FailOp(op, result, err_str);
  }
}

void TFileService::FailOp(TOp *op, TDiskResult result, const char *err_str) {
  if (op->GetKind() == TOp::RemoveFile && op->Removed) {
    const TFileObj &file = *op->Removed;
    std::lock_guard<std::mutex> lock(Mutex);
    try {
      AddToMap(Map, op->GetFileUUID(), file.Kind, file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize, file.NumKeys,
               file.LowestSeq, file.HighestSeq);
      ++NumFiles;
    } catch (const std::exception &ex) {
      /* Re-inserted since; the map names it already. */
      syslog(LOG_ERR, "TFileService: putting back gen [%ld] after a failed removal: [%s]", file.GenId, ex.what());
    }
  }
  op->Complete(result, err_str);
  delete op;
}

void TFileService::GrowBaseImage(std::vector<size_t> &image_block_vec, size_t target, size_t required) {
  size_t tries = 0UL;
  while (image_block_vec.size() < target) {
    try {
      VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const Util::TBlockRange &block_range) {
        assert(block_range.second == 1UL);
        image_block_vec.push_back(block_range.first);
      }, Util::TAllocClass::Essential);
    } catch (const Util::TDiskFull &ex) {
      if (image_block_vec.size() >= required) {
        /* Only the spare is short; top it up on a later image. */
        return;
      }
      /* The image must grow and nothing is free. Write admission refuses user writes long
         before this (#590), so getting here means the spare and the reserve are both gone.
         None of the queued ops can complete without this image, and the frees that would make
         room wait on those ops, so the only space that can still appear is discard-pending
         blocks. Wait for it rather than abort; reads don't need this loop. */
      if (ShuttingDown) {
        throw TDiskServiceShutdown();
      }
      if ((tries & (tries + 1UL)) == 0UL) {
        syslog(LOG_ERR, "TFileService base image needs [%ld] blocks, has [%ld]: [%s] (try %ld); waiting for space",
               required, image_block_vec.size(), ex.what(), tries + 1UL);
      }
      ++tries;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      Fiber::YieldSlow();
    }
  }
  if (tries) {
    syslog(LOG_ERR, "TFileService base image got its blocks after %ld tries", tries);
  }
}

void TFileService::AddToMap(TFileMap &file_map,
                            const Base::TUuid &file_uid,
                            TFileObj::TKind file_kind,
                            size_t file_gen,
                            size_t starting_block_id,
                            size_t starting_block_offset,
                            size_t file_size,
                            size_t num_keys,
                            TSequenceNumber lowest_seq,
                            TSequenceNumber highest_seq) {
  auto res = file_map.find(file_uid);
  if (res != file_map.end()) {
    auto r = res->second.emplace(file_gen, TFileObj(file_kind,
                                                    file_gen,
                                                    starting_block_id,
                                                    starting_block_offset,
                                                    file_size,
                                                    num_keys,
                                                    lowest_seq,
                                                    highest_seq));
    if (unlikely(!r.second)) {
      stringstream ss;
      ss << file_uid;
      syslog(LOG_ERR, "Trying to insert file that already exists [%s], [%ld]", ss.str().c_str(), file_gen);
      throw std::logic_error("Trying to insert file that already exists");
    }
  } else {
    file_map.emplace(file_uid, std::unordered_map<size_t, TFileObj>{{file_gen, TFileObj(file_kind,
                                                                                        file_gen,
                                                                                        starting_block_id,
                                                                                        starting_block_offset,
                                                                                        file_size,
                                                                                        num_keys,
                                                                                        lowest_seq,
                                                                                        highest_seq)}});
  }
}

void TFileService::RemoveFromMap(TFileMap &file_map,
                                 const Base::TUuid &file_uid,
                                 size_t file_gen) {
  auto res = file_map.find(file_uid);
  if (likely(res != file_map.end())) {
    const size_t num_removed = res->second.erase(file_gen);
    if (unlikely(num_removed != 1)) {
      stringstream ss;
      ss << file_uid;
      syslog(LOG_ERR, "Trying to remove file that does not exist [%s], [%ld]", ss.str().c_str(), file_gen);
      throw std::logic_error("Trying to remove file gen that does not exist");
    }
  } else {
    stringstream ss;
      ss << file_uid;
      syslog(LOG_ERR, "Trying to remove file that does not exist [%s], [%ld]", ss.str().c_str(), file_gen);
    throw std::logic_error("Trying to remove file that does not exist");
  }
}

void TFileService::ApplyDeltasToMap(TFileMap &file_map,
                                    const size_t *buf,
                                    size_t &num_files_in_map) {
  for (size_t i = 0; i < NumFilesPerRingBuf; ++i, buf += (EntrySize + OpSize)) {
    if (buf[0] == 0UL && buf[1] == 0UL) { /* if the first 16 bytes are all 0, then this is the end of the ring buffer log. */
      break;
    } else {
      size_t file_gen, starting_block_id, starting_block_offset, file_size, num_keys;
      TSequenceNumber lowest_seq, highest_seq;
      TFileObj::TKind file_kind = TFileObj::TKind::DataFile;
      uuid_t temp_uid;
      uuid_copy(temp_uid, *reinterpret_cast<const uuid_t *>(buf));
      file_gen = buf[2]; // file_gen
      starting_block_id = buf[3]; // starting_block_id
      starting_block_offset = buf[4]; // starting_block_offset
      file_size = buf[5]; // file_size
      num_keys = buf[6]; // num_keys
      lowest_seq = buf[7]; // lowest_seq
      highest_seq = buf[8]; // highest_seq
      switch (buf[9]) {
        case TFileObj::TKind::DataFile: {
          file_kind = TFileObj::TKind::DataFile;
          break;
        }
        case TFileObj::TKind::DurableFile: {
          file_kind = TFileObj::TKind::DurableFile;
          break;
        }
      }
      switch (buf[10]) {
        case TOp::TKind::InsertFile: {
          AddToMap(file_map, Base::TUuid(temp_uid), file_kind, file_gen, starting_block_id, starting_block_offset, file_size, num_keys, lowest_seq, highest_seq);
          ++num_files_in_map;
          break;
        }
        case TOp::TKind::RemoveFile: {
          RemoveFromMap(file_map, Base::TUuid(temp_uid), file_gen);
          --num_files_in_map;
          break;
        }
      }
    }
  }
}

void TFileService::ApplyImageBlock(TFileMap &file_map,
                                   const size_t *buf,
                                   size_t &num_files_in_map) {
  for (size_t i = 0; i < NumElemPerBaseImageBlock; ++i, buf += EntrySize) {
    if (*buf == 0UL && *(buf + 1) == 0UL) { /* if the first 16 bytes are all 0, then this is the end of the ring buffer log. */
      break;
    } else {
      size_t file_gen, starting_block_id, starting_block_offset, file_size, num_keys;
      TSequenceNumber lowest_seq, highest_seq;
      TFileObj::TKind file_kind = TFileObj::TKind::DataFile;
      uuid_t temp_uid;
      uuid_copy(temp_uid, *reinterpret_cast<const uuid_t *>(buf));
      file_gen = *(buf + 2); // file_gen
      starting_block_id = *(buf + 3); // starting_block_id
      starting_block_offset = *(buf + 4); // starting_block_offset
      file_size = *(buf + 5); // file_size
      num_keys = *(buf + 6); // num_keys
      lowest_seq = *(buf + 7); // lowest_seq
      highest_seq = *(buf + 8); // highest_seq
      switch (buf[9]) {
        case TFileObj::TKind::DataFile: {
          file_kind = TFileObj::TKind::DataFile;
          break;
        }
        case TFileObj::TKind::DurableFile: {
          file_kind = TFileObj::TKind::DurableFile;
          break;
        }
      }
      AddToMap(file_map, Base::TUuid(temp_uid), file_kind, file_gen, starting_block_id, starting_block_offset, file_size, num_keys, lowest_seq, highest_seq);
      ++num_files_in_map;
    }
  }
}

bool TFileService::TryLoadFromBaseImage(size_t base_image_block, const size_t *cur_buf, TBufBlock *cur_buf_block, std::vector<size_t> &chain_out) {
  TCompletionTrigger trigger;
  Map.clear();
  NumFiles = 0UL;
  RunnerCopyMap.clear();
  NumRunnerCopyFiles = 0UL;
  chain_out.clear();
  std::unordered_set<size_t> seen {base_image_block};
  bool process_next_block = true;
  try {
    while (process_next_block) {
      const size_t cur_version_number = cur_buf[0];
      if (VersionNumber != cur_version_number) {
        throw std::runtime_error("Reading partial block image");
      }
      ApplyImageBlock(Map, &cur_buf[2], NumFiles);  // start after the version and next_block data
      ApplyImageBlock(RunnerCopyMap, &cur_buf[2], NumRunnerCopyFiles);  // start after the version and next_block data
      const size_t next_block_id = cur_buf[1];
      process_next_block = next_block_id != static_cast<size_t>(-1);
      if (process_next_block) {
        if (!seen.insert(next_block_id).second) {
          throw std::runtime_error("Base image chain loops");
        }
        chain_out.push_back(next_block_id);
        VolMan->ReadBlock(HERE,
                          Util::CheckedBlock,
                          Source::FileService,
                          cur_buf_block->GetData(),
                          next_block_id,
                          RealTime,
                          trigger,
                          false /* don't abort on error since we're trying to build a valid image */);
        trigger.Wait();
      }
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "TFileService::TryLoadFromBaseImage caught exception while loading from block [%ld], ex: [%s]", base_image_block, ex.what());
    chain_out.clear();
    return false;
  }
  return true;
}

bool TFileService::ReadImageChain(size_t head_block_id, std::vector<size_t> &chain_out) {
  chain_out.clear();
  auto buf_block = make_unique<TBufBlock>();
  const size_t *buf = reinterpret_cast<const size_t *>(buf_block->GetData());
  std::unordered_set<size_t> seen {head_block_id};
  size_t block_id = head_block_id;
  size_t version = 0UL;
  for (;;) {
    try {
      TCompletionTrigger trigger;
      VolMan->ReadBlock(HERE, Util::CheckedBlock, Source::FileService, buf_block->GetData(), block_id, RealTime, trigger,
                        false /* a broken image is reported, not fatal */);
      trigger.Wait();
    } catch (const TDiskError &ex) {
      syslog(LOG_ERR, "TFileService: base image at block [%ld] can't be read at block [%ld]: [%s]; not keeping its chain", head_block_id, block_id, ex.what());
      chain_out.clear();
      return false;
    }
    if (block_id == head_block_id) {
      version = buf[0];
      if (!version) {
        /* Never written: nothing to keep. */
        return false;
      }
    } else if (buf[0] != version) {
      syslog(LOG_ERR, "TFileService: base image at block [%ld] (version [%ld]) is broken at block [%ld] (version [%ld]); not keeping its chain",
             head_block_id, version, block_id, buf[0]);
      chain_out.clear();
      return false;
    } else {
      chain_out.push_back(block_id);
    }
    const size_t next_block_id = buf[1];
    if (next_block_id == static_cast<size_t>(-1)) {
      return true;
    }
    if (!seen.insert(next_block_id).second) {
      syslog(LOG_ERR, "TFileService: base image at block [%ld] has a chain that loops at block [%ld]; not keeping its chain", head_block_id, next_block_id);
      chain_out.clear();
      return false;
    }
    block_id = next_block_id;
  }
}

void TFileService::AdoptImageChain(size_t head_block_id, const std::vector<size_t> &chain, std::vector<size_t> &image_block_vec) {
  assert(image_block_vec.size() == 1UL && image_block_vec.front() == head_block_id);
  size_t marked = 0UL;
  try {
    for (; marked < chain.size(); ++marked) {
      VolMan->MarkBlockRangeUsed(Util::TBlockRange(chain[marked], 1UL));
    }
  } catch (const std::exception &ex) {
    /* Something else already holds one of these blocks. Keep none of them: the next image
       written here gets fresh blocks rather than writing over whoever has that one. */
    syslog(LOG_ERR, "TFileService: base image at block [%ld]: chain block [%ld] is already in use [%s]; not keeping its chain",
           head_block_id, chain[marked], ex.what());
    for (size_t i = 0; i < marked; ++i) {
      VolMan->FreeSequentialBlocks(Util::TBlockRange(chain[i], 1UL));
    }
    return;
  }
  image_block_vec.insert(image_block_vec.end(), chain.begin(), chain.end());
}

void TFileService::ZeroImageBlocks(size_t image_1_block_id, size_t image_2_block_id) {
  auto buf_block = make_unique<TBufBlock>();
  memset(buf_block->GetData(), 0, Util::PhysicalBlockSize);
  TCompletionTrigger trigger;
  /* zero-out image_block 1 */ {
    VolMan->WriteBlock(HERE,
                       Util::CheckedBlock,
                       Source::FileService,
                       buf_block->GetData(),
                       image_1_block_id,
                       RealTime,
                       Util::TCacheInstr::NoCache,
                       trigger);
    trigger.Wait();
  }
  /* zero-out image_block 2 */ {
    VolMan->WriteBlock(HERE,
                       Util::CheckedBlock,
                       Source::FileService,
                       buf_block->GetData(),
                       image_2_block_id,
                       RealTime,
                       Util::TCacheInstr::NoCache,
                       trigger);
    trigger.Wait();
  }
  trigger.Wait();
}

void TFileService::ZeroAppendLog() {
  TCompletionTrigger trigger;
  auto buf_block = make_unique<TBufBlock>();
  memset(buf_block->GetData(), 0, Util::PhysicalBlockSize);
  for (auto block_id : AppendLogBlockVec) { /* zero-out each append block */
    VolMan->Write(HERE,
                  Util::SectorCheckedBlock,
                  Source::FileService,
                  buf_block->GetData(),
                  (block_id * Util::PhysicalBlockSize),
                  Util::PhysicalBlockSize,
                  RealTime,
                  Util::TCacheInstr::NoCache,
                  trigger);
    trigger.Wait();
  }
}
