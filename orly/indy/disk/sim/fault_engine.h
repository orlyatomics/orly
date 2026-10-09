/* <orly/indy/disk/sim/fault_engine.h>

   `Sim::TFaultEngine` -- a `Disk::Util::TEngine` on `TFaultDevice`s (#608), test-only.

   Unlike `TMemEngine`, it runs the real file service, laid out as `TDiskEngine` lays out a real
   disk: a system block naming the two base-image blocks and the append log, written at create
   and read back on reopen. So the Durable images a power loss leaves (`TFaultImage`) can be
   opened by a new engine, which reloads the file map and marks each file's blocks used, as
   `orlyi` does at startup.

   It also counts blocks: `GetLiveBlocks` is what the allocator holds, and
   `CountReferencedBlocks` what the system blocks and the files in the file map account for.
   With no merge or base image in flight, the two are equal; a difference is a leak (or blocks
   still owned by something unfinished).

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

#include <cstdio>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <orly/indy/disk/durable_manager.h>
#include <orly/indy/disk/file_service.h>
#include <orly/indy/disk/in_file.h>
#include <orly/indy/disk/integrity_scrub.h>
#include <orly/indy/disk/open_check.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/fault_device.h>
#include <orly/indy/disk/util/engine.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Sim {

        /* The Durable (or Live) images of an engine's devices, in device order. */
        struct TFaultImage {

          std::vector<std::vector<char>> Devices;

          void Save(const std::string &path) const {
            FILE *f = fopen(path.c_str(), "wb");
            if (!f) {
              throw std::runtime_error("cannot write fault image " + path);
            }
            const size_t num = Devices.size();
            bool ok = fwrite(&num, sizeof(num), 1, f) == 1;
            for (const auto &device : Devices) {
              const size_t size = device.size();
              ok = ok && fwrite(&size, sizeof(size), 1, f) == 1 && fwrite(device.data(), 1, size, f) == size;
            }
            ok = (fclose(f) == 0) && ok;
            if (!ok) {
              throw std::runtime_error("short write of fault image " + path);
            }
          }

          static TFaultImage Load(const std::string &path) {
            TFaultImage image;
            FILE *f = fopen(path.c_str(), "rb");
            if (!f) {
              throw std::runtime_error("cannot read fault image " + path);
            }
            size_t num = 0UL;
            bool ok = fread(&num, sizeof(num), 1, f) == 1;
            for (size_t i = 0UL; ok && i < num; ++i) {
              size_t size = 0UL;
              ok = fread(&size, sizeof(size), 1, f) == 1;
              if (ok) {
                image.Devices.emplace_back(size);
                ok = fread(image.Devices.back().data(), 1, size, f) == size;
              }
            }
            fclose(f);
            if (!ok) {
              throw std::runtime_error("short read of fault image " + path);
            }
            return image;
          }

        };  // TFaultImage

        class TFaultEngine {
          NO_COPY(TFaultEngine);
          public:

          using TFramePoolManager = Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>;

          struct TLayout {

            /* Fast devices, striped, as in TMemEngine; 512-byte logical blocks. */
            size_t NumFastDevices = 4UL;
            size_t FastMbPerDevice = 8UL;

            size_t SlowMb = 4UL;

            /* Blocks of file-service append log: each holds 128 sectors, one per batch of file
               map changes, before the file service writes a base image. */
            size_t AppendLogBlocks = 2UL;

            size_t PageCacheSize = 4096UL;
            size_t BlockCacheSize = 256UL;

            /* What the file service does when its append log fails its check at startup. */
            bool AbortOnAppendLogScan = true;

          };  // TLayout

          /* Creates a new file system, or, given an image, opens the one in it. 'file_init_cb'
             replaces the startup walk of each file's blocks (a file-service-only test can use
             files that have no blocks); with none, data and durable files are walked as
             TDiskEngine walks them. Must run on a fiber. */
          TFaultEngine(Base::TScheduler *scheduler,
                       Fiber::TRunner::TRunnerCons &runner_cons,
                       TFramePoolManager *frame_pool_manager,
                       TFaultPlan *plan,
                       const TLayout &layout,
                       const TFaultImage *image = nullptr,
                       const TFileService::TFileInitCb &file_init_cb = nullptr)
              : Layout(layout), CustomFileInit(static_cast<bool>(file_init_cb)) {
            const size_t num_logical_block_per_stripe = 1024UL;
            const size_t fast_blocks = layout.FastMbPerDevice * ((1024UL * 1024UL) / Util::PhysicalSectorSize);
            const size_t slow_blocks = layout.SlowMb * ((1024UL * 1024UL) / Util::PhysicalSectorSize);
            if (fast_blocks % num_logical_block_per_stripe || slow_blocks % num_logical_block_per_stripe) {
              throw std::logic_error("fault engine device sizes must be a multiple of the stripe");
            }
            if (image && image->Devices.size() != layout.NumFastDevices + 1UL) {
              throw std::logic_error("fault image does not match the layout");
            }
            CacheCb = [this](Util::TCacheInstr cache_instr, const Util::TOffset logical_start_offset, void *buf, size_t count) {
              ApplyCache(cache_instr, logical_start_offset, buf, count);
            };
            for (size_t i = 0UL; i < layout.NumFastDevices; ++i) {
              Devices.push_back(std::make_unique<TFaultDevice>(plan, fast_blocks, image ? &image->Devices[i] : nullptr));
            }
            Devices.push_back(std::make_unique<TFaultDevice>(plan, slow_blocks, image ? &image->Devices.back() : nullptr));
            FastVolume = std::make_unique<Util::TVolume>(
                Util::TVolume::TDesc{Util::TVolume::TDesc::Striped, Devices[0]->GetDesc(), Util::TVolume::TDesc::Fast, 1UL, layout.NumFastDevices,
                                     num_logical_block_per_stripe, 8UL, 0.85}, CacheCb, scheduler);
            SlowVolume = std::make_unique<Util::TVolume>(
                Util::TVolume::TDesc{Util::TVolume::TDesc::Striped, Devices.back()->GetDesc(), Util::TVolume::TDesc::Slow, 1UL, 1UL,
                                     num_logical_block_per_stripe, 8UL, 0.85}, CacheCb, scheduler);
            for (size_t i = 0UL; i < layout.NumFastDevices; ++i) {
              FastVolume->AddDevice(Devices[i].get(), i);
            }
            SlowVolume->AddDevice(Devices.back().get(), 0UL);
            VolMan = std::make_unique<Util::TVolumeManager>(scheduler);
            VolMan->AddNewVolume(FastVolume.get());
            VolMan->AddNewVolume(SlowVolume.get());
            PageCache = std::make_unique<Util::TPageCache>(VolMan.get(), layout.PageCacheSize, 1UL);
            BlockCache = std::make_unique<Util::TBlockCache>(VolMan.get(), layout.BlockCacheSize, 1UL);
            /* The system block, as TDiskEngine lays it out. */
            VolMan->MarkBlockRangeUsed(Util::TBlockRange(SystemBlockId, 1UL));
            std::unique_ptr<TBufBlock> buf_block(new TBufBlock());
            memset(buf_block->GetData(), 0, Util::PhysicalBlockSize);
            size_t *buf = reinterpret_cast<size_t *>(buf_block->GetData());
            if (!image) {
              auto alloc_one = [this]() {
                size_t block_id = 0UL;
                VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::Fast, 1UL, [&](const Util::TBlockRange &range) {
                  block_id = range.first;
                });
                return block_id;
              };
              Image1BlockId = alloc_one();
              Image2BlockId = alloc_one();
              VolMan->TryAllocateSequentialBlocks(Util::TVolume::TDesc::Fast, layout.AppendLogBlocks, [&](const Util::TBlockRange &range) {
                for (size_t i = 0UL; i < range.second; ++i) {
                  AppendLogBlockVec.push_back(range.first + i);
                }
              });
              if (AppendLogBlockVec.size() != layout.AppendLogBlocks) {
                throw std::runtime_error("fault engine: append log not contiguous");
              }
              buf[0] = Image1BlockId;
              buf[1] = Image2BlockId;
              buf[2] = AppendLogBlockVec.size();
              for (size_t i = 0UL; i < AppendLogBlockVec.size(); ++i) {
                buf[3 + i] = AppendLogBlockVec[i];
              }
              TCompletionTrigger trigger;
              VolMan->WriteAndFlush(HERE, Util::CheckedBlock, Source::System, buf_block->GetData(), SystemBlockId * Util::PhysicalBlockSize,
                                    Util::PhysicalBlockSize, RealTime, Util::TCacheInstr::NoCache, trigger);
              trigger.Wait();
            } else {
              TCompletionTrigger trigger;
              VolMan->ReadBlock(HERE, Util::CheckedBlock, Source::System, buf_block->GetData(), SystemBlockId, RealTime, trigger);
              trigger.Wait();
              Image1BlockId = buf[0];
              Image2BlockId = buf[1];
              VolMan->MarkBlockRangeUsed(Util::TBlockRange(Image1BlockId, 1UL));
              VolMan->MarkBlockRangeUsed(Util::TBlockRange(Image2BlockId, 1UL));
              for (size_t i = 0UL; i < buf[2]; ++i) {
                AppendLogBlockVec.push_back(buf[3 + i]);
                VolMan->MarkBlockRangeUsed(Util::TBlockRange(buf[3 + i], 1UL));
              }
            }
            const TFileService::TFileInitCb default_init_cb = [this](TFileObj::TKind file_kind, const Base::TUuid &/*file_uid*/, size_t gen_id,
                                                                     size_t starting_block_id, size_t starting_block_offset, size_t file_length) {
              ForEachFileBlockRange(file_kind, gen_id, starting_block_id, starting_block_offset, file_length, [this](const Util::TBlockRange &range) {
                VolMan->MarkBlockRangeUsed(range);
              });
              return true;
            };
            FileService = std::make_unique<TFileService>(scheduler, runner_cons, frame_pool_manager, VolMan.get(), Image1BlockId, Image2BlockId,
                                                         AppendLogBlockVec, file_init_cb ? file_init_cb : default_init_cb, !image,
                                                         layout.AbortOnAppendLogScan);
            Engine = std::make_unique<Util::TEngine>(VolMan.get(), PageCache.get(), BlockCache.get(), FileService.get(), false);
          }

          ~TFaultEngine() {
            Engine.reset();
            BlockCache.reset();
            PageCache.reset();
            FileService.reset();
          }

          Util::TEngine *GetEngine() const {
            return Engine.get();
          }

          Util::TVolumeManager *GetVolMan() const {
            return VolMan.get();
          }

          TFileService *GetFileService() const {
            return FileService.get();
          }

          TFaultImage GetDurableImage() const {
            TFaultImage image;
            for (const auto &device : Devices) {
              image.Devices.push_back(device->GetDurableImage());
            }
            return image;
          }

          TFaultImage GetLiveImage() const {
            TFaultImage image;
            for (const auto &device : Devices) {
              image.Devices.push_back(device->GetLiveImage());
            }
            return image;
          }

          /* Blocks the allocator holds, not counting freed blocks waiting for discard. */
          size_t GetLiveBlocks() const {
            const Util::TSpace space = VolMan->GetSpace();
            return (space.Used - space.DiscardPending) / Util::PhysicalBlockSize;
          }

          /* Blocks the system block, both base images, the append log and every file in the file
             map account for. A base image's chained and spare blocks are not counted. Walks each
             file on disk, so it must run on a fiber. Needs the default file walk. */
          size_t CountReferencedBlocks() const {
            assert(!CustomFileInit);
            std::vector<std::pair<Base::TUuid, TFileObj>> files;
            FileService->ForEachFile([&files](const Base::TUuid &file_uid, const TFileObj &file) {
              files.emplace_back(file_uid, file);
              return true;
            });
            std::set<size_t> blocks{SystemBlockId, Image1BlockId, Image2BlockId};
            blocks.insert(AppendLogBlockVec.begin(), AppendLogBlockVec.end());
            for (const auto &[file_uid, file] : files) {
              ForEachFileBlockRange(file.Kind, file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize,
                                    [&blocks](const Util::TBlockRange &range) {
                for (size_t i = 0UL; i < range.second; ++i) {
                  blocks.insert(range.first + i);
                }
              });
            }
            return blocks.size();
          }

          /* The open-time consistency check (#700), as orlyi runs it on a TDiskEngine. Must run
             on a fiber. Needs the default file walk. */
          TOpenCheck CheckOpenConsistency() const {
            assert(!CustomFileInit);
            return Disk::CheckOpenConsistency(VolMan.get(), FileService.get(), {SystemBlockId},
                [this](const Base::TUuid &/*file_uid*/, const TFileObj &file, const std::function<void (const Util::TBlockRange &)> &cb) {
                  ForEachFileBlockRange(file.Kind, file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize, cb);
                });
          }

          /* The integrity audit (#748): block accounting, sequence ranges, file decoding,
             and key ordering checks. Must run on a fiber. */
          Disk::TScrubReport RunIntegrityScrub(const Disk::TScrubOptions &options = {},
                                               const std::function<void()> &yield_cb = nullptr) const {
            assert(!CustomFileInit);
            return Disk::RunIntegrityScrub(VolMan.get(), FileService.get(), PageCache.get(), {SystemBlockId},
                [this](const Base::TUuid &/*file_uid*/, const TFileObj &file, const std::function<void (const Util::TBlockRange &)> &cb) {
                  ForEachFileBlockRange(file.Kind, file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize, cb);
                }, options, yield_cb);
          }

          /* Every block range of a file, read from its metadata, as TDiskEngine's startup walk
             does. */
          void ForEachFileBlockRange(TFileObj::TKind file_kind, size_t gen_id, size_t starting_block_id, size_t starting_block_offset,
                                     size_t file_length, const std::function<void (const Util::TBlockRange &)> &cb) const {
            switch (file_kind) {
              case TFileObj::TKind::DataFile: {
                TDataFileReader reader(PageCache.get(), gen_id, starting_block_id, starting_block_offset, file_length);
                TDataFileReader::TInStream in_stream(HERE, Source::System, RealTime, &reader, PageCache.get(),
                                                     (reader.GetStartingBlockOffset() * Util::LogicalBlockSize) + (TData::NumMetaFields * sizeof(size_t)));
                size_t block_id;
                for (size_t i = 0UL; i < reader.GetNumMetaBlocks(); ++i) {
                  in_stream.Read(block_id);
                  cb(Util::TBlockRange(block_id, 1UL));
                }
                size_t num_contig_blocks;
                for (size_t i = 0UL; i < reader.GetNumSequentialBlockPairings(); ++i) {
                  in_stream.Read(block_id);
                  in_stream.Read(num_contig_blocks);
                  cb(Util::TBlockRange(block_id, num_contig_blocks));
                }
                break;
              }
              case TFileObj::TKind::DurableFile: {
                TDurableManager::TSortedInFile sorted_in_file(PageCache.get(), RealTime, gen_id, starting_block_id, starting_block_offset, file_length);
                const size_t num_blocks = sorted_in_file.GetNumBlocks();
                typedef TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, 0UL> TInStream;
                TInStream in_stream(HERE, Source::System, RealTime, &sorted_in_file, PageCache.get(), TDurableManager::TSortedByIdFile::NumMetaFields * sizeof(size_t));
                size_t block_id;
                for (size_t i = 0UL; i < num_blocks; ++i) {
                  in_stream.Read(block_id);
                  cb(Util::TBlockRange(block_id, 1UL));
                }
                break;
              }
            }
          }

          private:

          /* TDiskEngine's reader for a data file's metadata. */
          class TDataFileReader
              : public TReadFile<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage> {
            NO_COPY(TDataFileReader);
            public:

            typedef TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, 0UL> TInStream;

            TDataFileReader(Util::TPageCache *page_cache, size_t gen_id, size_t starting_block_id, size_t starting_block_offset, size_t file_length)
                : TReadFile(HERE, Source::FileRemoval, page_cache, Base::TUuid(), RealTime, gen_id, starting_block_id, starting_block_offset, file_length) {}

            using TReadFile::GetStartingBlockOffset;
            using TReadFile::GetNumMetaBlocks;
            using TReadFile::GetNumSequentialBlockPairings;

          };  // TDataFileReader

          void ApplyCache(Util::TCacheInstr cache_instr, const Util::TOffset logical_start_offset, void *buf, size_t count) {
            const bool page = cache_instr == Util::CacheAll || cache_instr == Util::CachePageOnly || cache_instr == Util::ClearAll || cache_instr == Util::ClearPageOnly;
            const bool block = cache_instr == Util::CacheAll || cache_instr == Util::CacheBlockOnly || cache_instr == Util::ClearAll || cache_instr == Util::ClearBlockOnly;
            const bool clear = cache_instr == Util::ClearAll || cache_instr == Util::ClearPageOnly || cache_instr == Util::ClearBlockOnly;
            if (page) {
              for (size_t i = 0UL; i < count / Util::TPageCache::DataSize; ++i) {
                const size_t id = (logical_start_offset / Util::TPageCache::DataSize) + i;
                if (clear) {
                  PageCache->Clear(id);
                } else {
                  PageCache->Replace(id, reinterpret_cast<uint8_t *>(buf) + (i * Util::TPageCache::DataSize));
                }
              }
            }
            if (block) {
              for (size_t i = 0UL; i < count / Util::TBlockCache::DataSize; ++i) {
                const size_t id = (logical_start_offset / Util::TBlockCache::DataSize) + i;
                if (clear) {
                  BlockCache->Clear(id);
                } else {
                  BlockCache->Replace(id, reinterpret_cast<uint8_t *>(buf) + (i * Util::TBlockCache::DataSize));
                }
              }
            }
          }

          static constexpr size_t SystemBlockId = 0UL;

          const TLayout Layout;

          const bool CustomFileInit;

          size_t Image1BlockId = 0UL;

          size_t Image2BlockId = 0UL;

          std::vector<size_t> AppendLogBlockVec;

          Util::TCacheCb CacheCb;

          std::vector<std::unique_ptr<TFaultDevice>> Devices;

          std::unique_ptr<Util::TVolume> FastVolume;

          std::unique_ptr<Util::TVolume> SlowVolume;

          std::unique_ptr<Util::TVolumeManager> VolMan;

          std::unique_ptr<Util::TPageCache> PageCache;

          std::unique_ptr<Util::TBlockCache> BlockCache;

          std::unique_ptr<TFileService> FileService;

          std::unique_ptr<Util::TEngine> Engine;

        };  // TFaultEngine

      }  // Sim

    }  // Disk

  }  // Indy

}  // Orly
