/* <orly/indy/disk/util/disk_engine.h>

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

#include <mutex>

#include <base/mem_aligned_ptr.h>
#include <base/util/error.h>
#include <orly/indy/disk/util/disk_util.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/disk/file_service.h>
#include <orly/indy/disk/integrity_scrub.h>
#include <orly/indy/disk/open_check.h>
#include <orly/indy/disk/read_file.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        class TDiskEngine {
          NO_COPY(TDiskEngine);
          public:

          /* The controller, so the server can stop its polling loop (#440). */
          TDiskController *GetController() const {
            return DiskController.get();
          }

          TDiskEngine(Base::TScheduler *scheduler,
                      Fiber::TRunner::TRunnerCons &runner_cons,
                      Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                      const std::vector<size_t> &disk_controller_core_vec,
                      const std::string &instance_name,
                      bool discard_on_create,
                      bool do_fsync,
                      size_t page_cache_size,
                      size_t num_page_lru,
                      size_t block_cache_size,
                      size_t num_block_lru,
                      size_t append_log_mb,
                      bool create = false,
                      bool no_realtime = false,
                      bool durable_acks = false,
                      size_t wal_mb = 256,
                      bool persist_retention = false)
            : Scheduler(scheduler),
              SystemBlockId(0UL),
              FileAppendLogBlocks((append_log_mb * 1024 * 1024) / Util::PhysicalBlockSize) {
            assert(disk_controller_core_vec.size());
            CacheCb = [this](Util::TCacheInstr cache_instr, const Util::TOffset logical_start_offset, void *buf, size_t count) {
              /* Caching a write is best-effort. With every page buffer referenced (many layers under
                 many readers), throwing here failed the write it was caching, and from an out
                 stream's destructor that terminated the server. A page left out is just read from
                 disk later, and it can't leave a stale copy behind: Replace() only needs a new
                 buffer for a page that isn't cached already. */
              const auto replace = [](auto &cache, size_t id, void *data) {
                try {
                  cache.Replace(id, data);
                } catch (const Util::TOutOfCachePages &) {}
              };
              switch (cache_instr) {
                case Util::TCacheInstr::CacheAll: {
                  assert(PageCache.get());
                  assert(BlockCache.get());
                  assert(logical_start_offset % Util::TPageCache::DataSize == 0);
                  assert(count % Util::TPageCache::DataSize == 0);
                  assert(logical_start_offset % Util::TBlockCache::DataSize == 0);
                  assert(count % Util::TBlockCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TPageCache::DataSize); ++i) {
                    replace(*PageCache, (logical_start_offset / Util::TPageCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TPageCache::DataSize));
                  }
                  for (size_t i = 0; i < (count / Util::TBlockCache::DataSize); ++i) {
                    replace(*BlockCache, (logical_start_offset / Util::TBlockCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TBlockCache::DataSize));
                  }
                  break;
                }
                case Util::TCacheInstr::CachePageOnly: {
                  assert(PageCache.get());
                  assert(logical_start_offset % Util::TPageCache::DataSize == 0);
                  assert(count % Util::TPageCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TPageCache::DataSize); ++i) {
                    replace(*PageCache, (logical_start_offset / Util::TPageCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TPageCache::DataSize));
                  }
                  break;
                }
                case Util::TCacheInstr::CacheBlockOnly: {
                  assert(BlockCache.get());
                  assert(logical_start_offset % Util::TBlockCache::DataSize == 0);
                  assert(count % Util::TBlockCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TBlockCache::DataSize); ++i) {
                    replace(*BlockCache, (logical_start_offset / Util::TBlockCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TBlockCache::DataSize));
                  }
                  break;
                }
                case Util::TCacheInstr::ClearAll: {
                  assert(PageCache.get());
                  assert(BlockCache.get());
                  assert(logical_start_offset % Util::TPageCache::DataSize == 0);
                  assert(count % Util::TPageCache::DataSize == 0);
                  assert(logical_start_offset %Util::TBlockCache::DataSize == 0);
                  assert(count % Util::TBlockCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TPageCache::DataSize); ++i) {
                    PageCache->Clear((logical_start_offset / Util::TPageCache::DataSize) + i);
                  }
                  for (size_t i = 0; i < (count / Util::TBlockCache::DataSize); ++i) {
                    BlockCache->Clear((logical_start_offset / Util::TBlockCache::DataSize) + i);
                  }
                  break;
                }
                case Util::TCacheInstr::ClearPageOnly: {
                  assert(PageCache.get());
                  assert(logical_start_offset % Util::TPageCache::DataSize == 0);
                  assert(count % Util::TPageCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TPageCache::DataSize); ++i) {
                    PageCache->Clear((logical_start_offset / Util::TPageCache::DataSize) + i);
                  }
                  break;
                }
                case Util::TCacheInstr::ClearBlockOnly: {
                  assert(BlockCache.get());
                  assert(logical_start_offset %Util::TBlockCache::DataSize == 0);
                  assert(count % Util::TBlockCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TBlockCache::DataSize); ++i) {
                    BlockCache->Clear((logical_start_offset / Util::TBlockCache::DataSize) + i);
                  }
                  break;
                }
                case Util::TCacheInstr::NoCache: {
                  break;
                }
              }
            };
            DiskController = std::make_unique<TDiskController>();
            DiskUtil = std::make_unique<TDiskUtil>(scheduler, DiskController.get(), instance_name, do_fsync, CacheCb, true);
            VolMan = DiskUtil->GetVolumeManager(instance_name);
            std::vector<std::vector<TPersistentDevice *>> device_vec;
            size_t pos = 0UL;
            for (const auto &dev : DiskUtil->GetPersistentDeviceSet()) {
              device_vec.push_back(std::vector<TPersistentDevice *>{dynamic_cast<TPersistentDevice *>(dev.get())});
              Scheduler->Schedule(std::bind(&TDiskController::QueueRunner, DiskController.get(), device_vec.back(), no_realtime, disk_controller_core_vec[pos % disk_controller_core_vec.size()]));
              syslog(LOG_INFO, "Assigning Device [%s] to core [%ld] because pos [%ld]", device_vec.back().front()->GetDevicePath(), disk_controller_core_vec[pos % disk_controller_core_vec.size()], pos);
              ++pos;
            }
            //#endif

            /* Allocate the system block at position 0. */
            VolMan->MarkBlockRangeUsed(TBlockRange(0UL, 1UL));
            SystemBlockId = 0UL;

            PageCache = std::make_unique<Util::TPageCache>(VolMan, page_cache_size, num_page_lru);
            BlockCache = std::make_unique<Util::TBlockCache>(VolMan, block_cache_size, num_block_lru);

            std::unique_ptr<const TBufBlock> buf_block(new TBufBlock());
            memset(buf_block->GetData(), 0, PhysicalBlockSize);
            if (create) {
              if (discard_on_create) {
                VolMan->DiscardAllDevices();
              }
              TBlockRange tmp_range;
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const TBlockRange &block_range) {
                tmp_range = block_range;
              });
              assert(tmp_range.second == 1UL);
              Image1BlockId = tmp_range.first;
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const TBlockRange &block_range) {
                tmp_range = block_range;
              });
              assert(tmp_range.second == 1UL);
              Image2BlockId = tmp_range.first;
              Util::TBlockRange block_range;
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, FileAppendLogBlocks, [&](const TBlockRange &range) {
                block_range = range;
              });
              if (unlikely(block_range.second != FileAppendLogBlocks)) {
                throw std::runtime_error("not enough blocks allocated for the file service append log");
              }
              AppendLogBlockVec.reserve(block_range.second);
              for (size_t i = 0; i < block_range.second; ++i) {
                AppendLogBlockVec.push_back(block_range.first + i);
              }
              /* write the system block with our core information. */
              size_t *buf = reinterpret_cast<size_t *>(buf_block->GetData());
              buf[0] = Image1BlockId;
              buf[1] = Image2BlockId;
              buf[2] = FileAppendLogBlocks;
              if (unlikely((FileAppendLogBlocks * sizeof(size_t)) > (LogicalCheckedBlockSize - (3 * sizeof(size_t))))) {
                throw std::runtime_error("FileAppendLog meta data is too large to fit in system block");
              }
              assert(AppendLogBlockVec.size() == FileAppendLogBlocks);
              for (size_t i = 0; i < FileAppendLogBlocks; ++i) {
                buf[3 + i] = AppendLogBlockVec[i];
              }
              TCompletionTrigger trigger;
              VolMan->WriteAndFlush(HERE,
                                    CheckedBlock,
                                    Source::System,
                                    buf_block->GetData(),
                                    SystemBlockId * PhysicalBlockSize,
                                    PhysicalBlockSize,
                                    RealTime,
                                    TCacheInstr::NoCache,
                                    trigger);
              trigger.Wait();
              if (durable_acks) {
                const size_t wal_blocks = (wal_mb * 1024 * 1024) / Util::PhysicalBlockSize;
                Util::TBlockRange wal_range;
                VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, wal_blocks, [&](const TBlockRange &range) {
                  wal_range = range;
                });
                if (unlikely(wal_range.second != wal_blocks)) {
                  throw std::runtime_error("not enough blocks allocated for WAL ring");
                }
                WalStartBlock = wal_range.first;
                WalNumBlocks = wal_range.second;
                TBlockRange cp0_range, cp1_range;
                WalCheckpointBlocks = DefaultWalCheckpointBlocks();
                VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, WalCheckpointBlocks, [&](const TBlockRange &range) {
                  cp0_range = range;
                });
                if (unlikely(cp0_range.second != WalCheckpointBlocks)) {
                  throw std::runtime_error("not enough blocks allocated for WAL checkpoint slot 0");
                }
                WalCheckpoint0Block = cp0_range.first;
                VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, WalCheckpointBlocks, [&](const TBlockRange &range) {
                  cp1_range = range;
                });
                if (unlikely(cp1_range.second != WalCheckpointBlocks)) {
                  throw std::runtime_error("not enough blocks allocated for WAL checkpoint slot 1");
                }
                WalCheckpoint1Block = cp1_range.first;
                FormatVersion = 1UL;

                /* Zero checkpoint blocks and first WAL block on physical media, and flush (§8.7) */
                for (auto &entry : DiskUtil->GetOrlyDeviceMap()) {
                  entry.second.FormatVersion = FormatVersion;
                  entry.second.WalStartBlock = WalStartBlock;
                  entry.second.WalNumBlocks = WalNumBlocks;
                  entry.second.WalCheckpoint0Block = WalCheckpoint0Block;
                  entry.second.WalCheckpoint1Block = WalCheckpoint1Block;
                  entry.second.WalCheckpointBlocks = WalCheckpointBlocks;
                  Base::TFd fd = open(entry.first.c_str(), O_RDWR);
                  auto zero_buf = Base::MemAlignedAllocZeroInitialized<char>(PhysicalBlockSize, PhysicalBlockSize);
                  ::Util::IfLt0(pwrite(fd, zero_buf.get(), PhysicalBlockSize, (WalCheckpoint0Block + 1UL) * PhysicalBlockSize));
                  ::Util::IfLt0(pwrite(fd, zero_buf.get(), PhysicalBlockSize, (WalCheckpoint1Block + 1UL) * PhysicalBlockSize));
                  ::Util::IfLt0(pwrite(fd, zero_buf.get(), PhysicalBlockSize, (WalStartBlock + 1UL) * PhysicalBlockSize));
                  fsync(fd);
                  TDeviceUtil::ModifyDevice(entry.first.c_str(), entry.second);
                }
              }
            } else {
              TCompletionTrigger trigger;
              VolMan->ReadBlock(HERE, CheckedBlock, Source::System, buf_block->GetData(), SystemBlockId, RealTime, trigger);
              trigger.Wait();
              size_t *buf = reinterpret_cast<size_t *>(buf_block->GetData());
              Image1BlockId = buf[0];
              Image2BlockId = buf[1];
              FileAppendLogBlocks = buf[2];
              if (unlikely((FileAppendLogBlocks * sizeof(size_t)) > (LogicalCheckedBlockSize - (3 * sizeof(size_t))))) {
                throw std::runtime_error("FileAppendLog meta data is too large to fit in system block");
              }
              VolMan->MarkBlockRangeUsed(TBlockRange(Image1BlockId, 1UL)); /* mark block 1 */
              VolMan->MarkBlockRangeUsed(TBlockRange(Image2BlockId, 1UL)); /* mark block 2 */
              AppendLogBlockVec.reserve(FileAppendLogBlocks);
              assert(AppendLogBlockVec.empty());
              for (size_t i = 0; i < FileAppendLogBlocks; ++i) {
                AppendLogBlockVec.push_back(buf[3 + i]);
                VolMan->MarkBlockRangeUsed(TBlockRange(buf[3 + i], 1UL)); /* mark append log blocks */
              }
              if (!DiskUtil->GetOrlyDeviceMap().empty()) {
                const auto &dev_info = DiskUtil->GetOrlyDeviceMap().begin()->second;
                FormatVersion = dev_info.FormatVersion;
                WalStartBlock = dev_info.WalStartBlock;
                WalNumBlocks = dev_info.WalNumBlocks;
                WalCheckpoint0Block = dev_info.WalCheckpoint0Block;
                WalCheckpoint1Block = dev_info.WalCheckpoint1Block;
                WalCheckpointBlocks = dev_info.WalCheckpointBlocks ? dev_info.WalCheckpointBlocks : 1UL;
                IncompatFeatures = dev_info.IncompatFeatures;
              }
              if (const std::string why = Util::TStoreFormat::CheckSupported(FormatVersion, IncompatFeatures); !why.empty()) {
                throw std::runtime_error(why);
              }
              if (FormatVersion >= 1UL) {
                VolMan->MarkBlockRangeUsed(TBlockRange(WalStartBlock, WalNumBlocks));
                VolMan->MarkBlockRangeUsed(TBlockRange(WalCheckpoint0Block, WalCheckpointBlocks));
                VolMan->MarkBlockRangeUsed(TBlockRange(WalCheckpoint1Block, WalCheckpointBlocks));
              }
            }
            TFileService::TFileInitCb file_init_cb = [this](TFileObj::TKind file_kind,
                                                            const Base::TUuid &file_uid,
                                                            size_t gen_id,
                                                            size_t starting_block_id,
                                                            size_t starting_block_offset,
                                                            size_t file_length) -> bool {
              ForEachFileBlockRange(file_kind, file_uid, gen_id, starting_block_id, starting_block_offset, file_length,
                                    [this](const TBlockRange &range) {
                VolMan->MarkBlockRangeUsed(range);
              });
              return true;
            };
            FileService = std::make_unique<TFileService>(scheduler,
                                                         runner_cons,
                                                         frame_pool_manager,
                                                         VolMan,
                                                         Image1BlockId,
                                                         Image2BlockId,
                                                         AppendLogBlockVec,
                                                         file_init_cb,
                                                         create);

            /* Auto-upgrade format version 0 stores when durable_acks is enabled (#755 Stage 3). */
            if (!create && durable_acks && FormatVersion == 0UL) {
              const size_t wal_blocks = (wal_mb * 1024 * 1024) / Util::PhysicalBlockSize;
              Util::TBlockRange wal_range;
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, wal_blocks, [&](const TBlockRange &range) {
                wal_range = range;
              });
              if (unlikely(wal_range.second != wal_blocks)) {
                throw std::runtime_error("cannot upgrade store to format version 1: unable to allocate contiguous WAL range of " + std::to_string(wal_mb) + " MB from free space; start with --durable_acks=false to open as version 0");
              }
              TBlockRange cp0_range, cp1_range;
              const size_t cp_blocks = DefaultWalCheckpointBlocks();
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, cp_blocks, [&](const TBlockRange &range) {
                cp0_range = range;
              });
              if (unlikely(cp0_range.second != cp_blocks)) {
                throw std::runtime_error("cannot upgrade store to format version 1: unable to allocate checkpoint block 0; start with --durable_acks=false to open as version 0");
              }
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::TStorageSpeed::Fast, cp_blocks, [&](const TBlockRange &range) {
                cp1_range = range;
              });
              if (unlikely(cp1_range.second != cp_blocks)) {
                throw std::runtime_error("cannot upgrade store to format version 1: unable to allocate checkpoint block 1; start with --durable_acks=false to open as version 0");
              }
              WalStartBlock = wal_range.first;
              WalNumBlocks = wal_range.second;
              WalCheckpoint0Block = cp0_range.first;
              WalCheckpoint1Block = cp1_range.first;
              WalCheckpointBlocks = cp_blocks;
              FormatVersion = 1UL;

              /* Zero checkpoint blocks and first WAL block on physical media, and flush (§8.7) */
              for (auto &entry : DiskUtil->GetOrlyDeviceMap()) {
                entry.second.FormatVersion = FormatVersion;
                entry.second.WalStartBlock = WalStartBlock;
                entry.second.WalNumBlocks = WalNumBlocks;
                entry.second.WalCheckpoint0Block = WalCheckpoint0Block;
                entry.second.WalCheckpoint1Block = WalCheckpoint1Block;
                entry.second.WalCheckpointBlocks = WalCheckpointBlocks;
                Base::TFd fd = open(entry.first.c_str(), O_RDWR);
                auto zero_buf = Base::MemAlignedAllocZeroInitialized<char>(PhysicalBlockSize, PhysicalBlockSize);
                ::Util::IfLt0(pwrite(fd, zero_buf.get(), PhysicalBlockSize, (WalCheckpoint0Block + 1UL) * PhysicalBlockSize));
                ::Util::IfLt0(pwrite(fd, zero_buf.get(), PhysicalBlockSize, (WalCheckpoint1Block + 1UL) * PhysicalBlockSize));
                ::Util::IfLt0(pwrite(fd, zero_buf.get(), PhysicalBlockSize, (WalStartBlock + 1UL) * PhysicalBlockSize));
                fsync(fd);
                TDeviceUtil::ModifyDevice(entry.first.c_str(), entry.second);
              }
              syslog(LOG_INFO, "upgraded store to format version 1: WAL [%lu, %lu], checkpoints [%lu, %lu]",
                     WalStartBlock, WalNumBlocks, WalCheckpoint0Block, WalCheckpoint1Block);
            }

            /* #745 stage 3: format version 2 is version 1 plus the per-repo retention records (the history horizon,
               save points and window samples, kept in the durable store). A binary that predates it refuses the store
               (above), so it can never drop history a record still promises. Upgrade only when the retention window is
               on: a store that never keeps history stays at version 1 and any binary since #846 opens it. A version 0
               store (no WAL, --durable_acks=false) is left alone, and its retention stays in memory only. */
            if (persist_retention && FormatVersion == 1UL) {
              FormatVersion = 2UL;
              for (auto &entry : DiskUtil->GetOrlyDeviceMap()) {
                entry.second.FormatVersion = FormatVersion;
                TDeviceUtil::ModifyDevice(entry.first.c_str(), entry.second);
              }
              syslog(LOG_INFO, "upgraded store to format version 2: retention records (#745)");
            } else if (persist_retention && FormatVersion >= 3UL && !(IncompatFeatures & Util::TStoreFormat::RetentionFeature)) {
              /* A version 3 store that went there for another feature (#963) before it kept history. */
              EnableIncompatFeature(Util::TStoreFormat::RetentionFeature);
            } else if (persist_retention && FormatVersion == 0UL) {
              syslog(LOG_WARNING, "store format version 0 has no retention records: the retention window will not survive a restart (#745); start with --durable_acks to upgrade");
            }

            Engine =
                std::make_unique<Util::TEngine>(VolMan, PageCache.get(), BlockCache.get(), FileService.get(), true);
          }

          /* Every block range of a file, read from its metadata: what the startup walk marks
             used. Must run on a fiber. */
          void ForEachFileBlockRange(TFileObj::TKind file_kind, const Base::TUuid &file_uid, size_t gen_id, size_t starting_block_id,
                                     size_t starting_block_offset, size_t file_length,
                                     const std::function<void (const TBlockRange &)> &cb) const {
            switch (file_kind) {
              case TFileObj::TKind::DataFile: {
                TDataFileReader reader(PageCache.get(),
                                       file_uid,
                                       RealTime,
                                       gen_id,
                                       starting_block_id,
                                       starting_block_offset,
                                       file_length);


                TDataFileReader::TInStream in_stream(HERE, Source::System, RealTime, &reader, PageCache.get(), (reader.GetStartingBlockOffset() * Disk::Util::LogicalBlockSize) + (TData::NumMetaFields * sizeof(size_t)));
                size_t block_id;
                for (size_t i = 0; i < reader.GetNumMetaBlocks(); ++i) {
                  in_stream.Read(block_id);
                  cb(TBlockRange(block_id, 1UL));
                }
                size_t num_contig_blocks;
                for (size_t i = 0; i < reader.GetNumSequentialBlockPairings(); ++i) {
                  in_stream.Read(block_id);
                  in_stream.Read(num_contig_blocks);
                  cb(TBlockRange(block_id, num_contig_blocks));
                }
                break;
              }
              case TFileObj::TKind::DurableFile: {
                TDurableManager::TSortedInFile sorted_in_file(PageCache.get(),
                                                              RealTime,
                                                              gen_id,
                                                              starting_block_id,
                                                              starting_block_offset,
                                                              file_length);
                const size_t num_blocks = sorted_in_file.GetNumBlocks();
                typedef TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, 0UL> TInStream;
                TInStream in_stream(HERE, Source::System, RealTime, &sorted_in_file, PageCache.get(), TDurableManager::TSortedByIdFile::NumMetaFields * sizeof(size_t));
                size_t block_id;
                for (size_t i = 0; i < num_blocks; ++i) {
                  in_stream.Read(block_id);
                  /* #328 investigated and declined: batching these into ranges would need the
                     durable-file on-disk format to also store contiguous run lengths (like the
                     DataFile case above does via GetNumSequentialBlockPairings()), a format
                     change for a one-time startup scan cost with no demonstrated pain. */
                  cb(TBlockRange(block_id, 1UL));
                }
                break;
              }
            }
          }

          /* The blocks owned by the engine outside the file service (system block, WAL, checkpoints). */
          std::vector<size_t> GetEngineBlocks() const {
            std::vector<size_t> blocks = {SystemBlockId};
            if (FormatVersion >= 1UL && WalNumBlocks > 0) {
              for (size_t i = 0; i < WalCheckpointBlocks; ++i) {
                blocks.push_back(WalCheckpoint0Block + i);
                blocks.push_back(WalCheckpoint1Block + i);
              }
              for (size_t i = 0; i < WalNumBlocks; ++i) {
                blocks.push_back(WalStartBlock + i);
              }
            }
            return blocks;
          }

          /* The open-time consistency check (#700): see <orly/indy/disk/open_check.h>. Must run on
             a fiber. */
          TOpenCheck CheckOpenConsistency() const {
            return Disk::CheckOpenConsistency(VolMan, FileService.get(), GetEngineBlocks(),
                [this](const Base::TUuid &file_uid, const TFileObj &file, const std::function<void (const TBlockRange &)> &cb) {
                  ForEachFileBlockRange(file.Kind, file_uid, file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize, cb);
                });
          }

          /* The integrity audit (#748): block accounting, sequence ranges, file decoding,
             and key ordering checks. Must run on a fiber. */
          TScrubReport RunIntegrityScrub(const TScrubOptions &options = {},
                                         const std::function<void()> &yield_cb = nullptr) const {
            return Disk::RunIntegrityScrub(VolMan, FileService.get(), PageCache.get(), GetEngineBlocks(),
                [this](const Base::TUuid &file_uid, const TFileObj &file, const std::function<void (const TBlockRange &)> &cb) {
                  ForEachFileBlockRange(file.Kind, file_uid, file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize, cb);
                }, options, yield_cb);
          }

          /* The newest store format this binary opens: 1 adds the WAL (#755), 2 the retention records (#745), 3 the
             IncompatFeatures word (#963; see Util::TStoreFormat). */
          static constexpr uint64_t MaxFormatVersion = Util::TStoreFormat::MaxVersion;

          uint64_t GetFormatVersion() const {
            std::lock_guard<std::mutex> lock(FormatMutex);
            return FormatVersion;
          }

          /* The features the store has used (Util::TStoreFormat), whatever its version. */
          uint64_t GetIncompatFeatures() const {
            std::lock_guard<std::mutex> lock(FormatMutex);
            return Util::TStoreFormat::GetFeatures(FormatVersion, IncompatFeatures);
          }

          /* Whether the store keeps the retention records (#745): version 2, or version 3 with the bit. */
          bool IsRetentionPersisted() const {
            return (GetIncompatFeatures() & Util::TStoreFormat::RetentionFeature) != 0UL;
          }

          /* Mark the store as having used `feature`, before it first writes anything that needs it, so no binary that
             doesn't understand it opens the store again (#963).  Takes the store to version 3 on every device and
             syncs each superblock before it returns.  Does nothing if the store has the feature already.  A store with
             no WAL never gets here: every such feature lives in the WAL.  Throws if a superblock can't be written, and
             then the caller must not use the feature. */
          void EnableIncompatFeature(uint64_t feature) {
            std::lock_guard<std::mutex> lock(FormatMutex);
            if (Util::TStoreFormat::GetFeatures(FormatVersion, IncompatFeatures) & feature) {
              return;
            }
            const auto [version, features] = Util::TStoreFormat::WithFeature(FormatVersion, IncompatFeatures, feature);
            for (auto &entry : DiskUtil->GetOrlyDeviceMap()) {
              entry.second.FormatVersion = version;
              entry.second.IncompatFeatures = features;
              TDeviceUtil::ModifyDevice(entry.first.c_str(), entry.second);
            }
            const uint64_t old_version = FormatVersion;
            FormatVersion = version;
            IncompatFeatures = features;
            syslog(LOG_NOTICE, "upgraded store from format version %lu to %lu, features 0x%lx: binaries older than this one will refuse it",
                   old_version, version, features);
          }
          uint64_t GetWalStartBlock() const { return WalStartBlock; }
          uint64_t GetWalNumBlocks() const { return WalNumBlocks; }
          uint64_t GetWalCheckpoint0Block() const { return WalCheckpoint0Block; }
          uint64_t GetWalCheckpoint1Block() const { return WalCheckpoint1Block; }
          /* The length of each checkpoint slot in blocks. */
          uint64_t GetWalCheckpointBlocks() const { return WalCheckpointBlocks; }
          /* The size a new store gives each checkpoint slot: 1 MiB, in blocks. */
          static constexpr size_t DefaultWalCheckpointBlocks() { return (1024UL * 1024UL) / Util::PhysicalBlockSize; }
          bool HasWal() const { return GetFormatVersion() >= 1UL && WalNumBlocks > 0; }
          TPersistentDevice *GetPersistentDevice() const {
            if (!DiskUtil->GetPersistentDeviceSet().empty()) {
              return dynamic_cast<TPersistentDevice *>(DiskUtil->GetPersistentDeviceSet().begin()->get());
            }
            return nullptr;
          }

          Util::TEngine *GetEngine() const {
            return Engine.get();
          }

          Util::TPageCache *GetPageCache() const {
            return PageCache.get();
          }

          Util::TBlockCache *GetBlockCache() const {
            return BlockCache.get();
          }

          Util::TVolumeManager *GetVolMan() const {
            return VolMan;
          }

          /* Stop the file service's background loop while the scheduler hosting it is still
             alive; see TFileService::ShutDown() (#648). */
          void ShutDownFileService() {
            FileService->ShutDown();
          }

          void Report(std::stringstream &ss, double elapsed_time) const {
            DiskController->Report(ss, elapsed_time);
          }

          private:

          class TDataFileReader
              : public TReadFile<LogicalPageSize, LogicalBlockSize, PhysicalBlockSize, CheckedPage> {
            NO_COPY(TDataFileReader);
            public:

            typedef TStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::CheckedPage, 0UL> TInStream;
            typedef Orly::Indy::Disk::TReadFile<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::CheckedPage> TMyReadFile;

            TDataFileReader(Disk::Util::TPageCache *page_cache,
                            const Base::TUuid &file_id,
                            DiskPriority priority,
                            size_t gen_id,
                            size_t starting_block_id,
                            size_t starting_block_offset,
                            size_t file_length)
                : TMyReadFile(HERE,
                              Source::FileRemoval,
                              page_cache,
                              file_id,
                              priority,
                              gen_id,
                              starting_block_id,
                              starting_block_offset,
                              file_length) {}

            virtual ~TDataFileReader() {}

            using TReadFile::GetStartingBlockOffset;
            using TReadFile::GetNumBlocks;
            using TReadFile::GetNumMetaBlocks;
            using TReadFile::GetNumSequentialBlockPairings;
            using TReadFile::GetNumUpdates;

            using TReadFile::FindInHash;

          };  // TDataFileReader

          Base::TScheduler *Scheduler;

          std::unique_ptr<TDiskController> DiskController;

          std::unique_ptr<TDiskUtil> DiskUtil;

          size_t SystemBlockId;
          /* Covers FormatVersion and IncompatFeatures once the engine is open (EnableIncompatFeature). */
          mutable std::mutex FormatMutex;
          uint64_t FormatVersion = 0UL;
          uint64_t IncompatFeatures = 0UL;
          uint64_t WalStartBlock = 0UL;
          uint64_t WalNumBlocks = 0UL;
          uint64_t WalCheckpoint0Block = 0UL;
          uint64_t WalCheckpoint1Block = 0UL;
          uint64_t WalCheckpointBlocks = 1UL;

          std::unique_ptr<TFileService> FileService;
          size_t FileAppendLogBlocks;
          size_t Image1BlockId;
          size_t Image2BlockId;
          std::vector<size_t> AppendLogBlockVec;

          Util::TVolumeManager *VolMan;

          std::unique_ptr<Util::TPageCache> PageCache;
          std::unique_ptr<Util::TBlockCache> BlockCache;
          std::unique_ptr<Util::TEngine> Engine;

          Util::TCacheCb CacheCb;

        };  // TDiskEngine

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly