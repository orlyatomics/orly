/* <orly/indy/disk/sim/mem_engine.h>

   `Sim::TMemEngine` -- assembles a `Disk::TEngine` backed entirely
   by in-memory simulated devices. Used in tests and by
   `orlyi --mem_sim` to run without touching real block storage.
   Includes the stripe-alignment rounding logic (`round_up_blocks`)
   so requested-MB counts work cleanly with the underlying volume
   layout.

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

#include <orly/indy/disk/util/engine.h>
#include <orly/indy/disk/test_file_service.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Sim {

        /* TODO */
        class TMemEngine {
          NO_COPY(TMemEngine);
          public:

          /* TODO */
          TMemEngine(Base::TScheduler *scheduler, size_t num_mb, size_t num_slow_mb, size_t page_cache_size, size_t num_page_lru, size_t block_cache_size, size_t num_block_lru) {
            const size_t logical_block_size = 512;
            const size_t physical_block_size = 512;
            const size_t num_logical_block_per_stripe = 1024UL;
            /* Per-device capacity has to be a multiple of the stripe size --
               enforced by TVolume's constructor and otherwise throws
               "Device Capacity must be a multiple of the striping factor."
               at server startup. So the total logical-block count has to be
               a multiple of (stripe_size * num_devices). Round the requested
               MB count up to satisfy that, and log if we did, so the server's
               auto-scaled --mem_sim_mb (which is computed from available RAM
               and doesn't know about stripes) doesn't blow up on init. */
            auto round_up_blocks = [num_logical_block_per_stripe](size_t requested, size_t num_devices, const char *label) {
              const size_t alignment = num_logical_block_per_stripe * num_devices;
              const size_t rounded = ((requested + alignment - 1) / alignment) * alignment;
              if (rounded != requested) {
                syslog(LOG_INFO, "TMemEngine: rounded %s logical-block count %zu -> %zu to satisfy stripe alignment (%zu blocks/stripe x %zu devices)",
                       label, requested, rounded, num_logical_block_per_stripe, num_devices);
              }
              return rounded;
            };
            const size_t num_fast_logical_block =
                round_up_blocks(num_mb * ((1024 * 1024) / logical_block_size), NumFastMemDevice, "fast");
            const size_t num_slow_logical_block =
                round_up_blocks(num_slow_mb * ((1024 * 1024) / logical_block_size), NumSlowMemDevice, "slow");
            const size_t min_discard_blocks = 8UL;
            const double high_utilization_threshold = 0.85;
            CacheCb = [this](Util::TCacheInstr cache_instr, const Util::TOffset logical_start_offset, void *buf, size_t count) {
              switch (cache_instr) {
                case Util::TCacheInstr::CacheAll: {
                  assert(PageCache.get());
                  assert(BlockCache.get());
                  assert(logical_start_offset % Util::TPageCache::DataSize == 0);
                  assert(count % Util::TPageCache::DataSize == 0);
                  assert(logical_start_offset % Util::TBlockCache::DataSize == 0);
                  assert(count % Util::TBlockCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TPageCache::DataSize); ++i) {
                    PageCache->Replace((logical_start_offset / Util::TPageCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TPageCache::DataSize));
                  }
                  for (size_t i = 0; i < (count / Util::TBlockCache::DataSize); ++i) {
                    BlockCache->Replace((logical_start_offset / Util::TBlockCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TBlockCache::DataSize));
                  }
                }
                case Util::TCacheInstr::CachePageOnly: {
                  assert(PageCache.get());
                  assert(logical_start_offset % Util::TPageCache::DataSize == 0);
                  assert(count % Util::TPageCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TPageCache::DataSize); ++i) {
                    PageCache->Replace((logical_start_offset / Util::TPageCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TPageCache::DataSize));
                  }
                  break;
                }
                case Util::TCacheInstr::CacheBlockOnly: {
                  assert(BlockCache.get());
                  assert(logical_start_offset % Util::TBlockCache::DataSize == 0);
                  assert(count % Util::TBlockCache::DataSize == 0);
                  for (size_t i = 0; i < (count / Util::TBlockCache::DataSize); ++i) {
                    BlockCache->Replace((logical_start_offset / Util::TBlockCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * Util::TBlockCache::DataSize));
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
            for (size_t i = 0; i < NumFastMemDevice; ++i) {
              FastMemDeviceArray[i] = std::make_unique<Util::TMemoryDevice>(logical_block_size, physical_block_size, num_fast_logical_block / NumFastMemDevice, true /* fsync */, true);
            }
            for (size_t i = 0; i < NumSlowMemDevice; ++i) {
              SlowMemDeviceArray[i] = std::make_unique<Util::TMemoryDevice>(logical_block_size, physical_block_size, num_slow_logical_block / NumSlowMemDevice, true /* fsync */, true);
            }
            FastVolume = std::make_unique<Util::TVolume>(Util::TVolume::TDesc{Util::TVolume::TDesc::Striped, FastMemDeviceArray[0]->GetDesc(), Util::TVolume::TDesc::Fast, 1UL, NumFastMemDevice, num_logical_block_per_stripe, min_discard_blocks, high_utilization_threshold}, CacheCb, scheduler);
            SlowVolume = std::make_unique<Util::TVolume>(Util::TVolume::TDesc{Util::TVolume::TDesc::Striped, SlowMemDeviceArray[0]->GetDesc(), Util::TVolume::TDesc::Slow, 1UL, NumSlowMemDevice, num_logical_block_per_stripe, min_discard_blocks, high_utilization_threshold}, CacheCb, scheduler);
            for (size_t i = 0; i < NumFastMemDevice; ++i) {
              FastVolume->AddDevice(FastMemDeviceArray[i].get(), i);
            }
            for (size_t i = 0; i < NumSlowMemDevice; ++i) {
              SlowVolume->AddDevice(SlowMemDeviceArray[i].get(), i);
            }
            VolMan = std::make_unique<Util::TVolumeManager>(scheduler);
            VolMan->AddNewVolume(FastVolume.get());
            VolMan->AddNewVolume(SlowVolume.get());
            PageCache = std::make_unique<Util::TPageCache>(VolMan.get(), page_cache_size, num_page_lru);
            BlockCache = std::make_unique<Util::TBlockCache>(VolMan.get(), block_cache_size, num_block_lru);
            Engine = std::make_unique<Util::TEngine>(VolMan.get(), PageCache.get(), BlockCache.get(), &TestFileService, false);
          }

          /* TODO */
          Util::TEngine *GetEngine() const {
            return Engine.get();
          }

          /* TODO */
          Util::TPageCache *GetPageCache() const {
            return PageCache.get();
          }

          /* TODO */
          Util::TBlockCache *GetBlockCache() const {
            return BlockCache.get();
          }

          /* TODO */
          Util::TVolumeManager *GetVolMan() const {
            return VolMan.get();
          }

          private:

          static constexpr size_t NumFastMemDevice = 4UL;

          static constexpr size_t NumSlowMemDevice = 1UL;

          /* TODO */
          TTestFileService TestFileService;

          /* TODO */
          std::unique_ptr<Util::TMemoryDevice> FastMemDeviceArray[NumFastMemDevice];
          std::unique_ptr<Util::TMemoryDevice> SlowMemDeviceArray[NumSlowMemDevice];
          std::unique_ptr<Util::TVolume> FastVolume;
          std::unique_ptr<Util::TVolume> SlowVolume;
          std::unique_ptr<Util::TVolumeManager> VolMan;

          /* TODO */
          std::unique_ptr<Util::TPageCache> PageCache;
          std::unique_ptr<Util::TBlockCache> BlockCache;
          std::unique_ptr<Util::TEngine> Engine;

          /* TODO */
          Util::TCacheCb CacheCb;

        };

      }  // Sim

    }  // Disk

  }  // Indy

}  // Orly