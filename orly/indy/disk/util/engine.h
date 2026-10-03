/* <orly/indy/disk/util/engine.h>

   The disk-layer's resource hub. `TEngine` bundles a `TVolumeManager`
   (where blocks live), two `TCache`s (`TPageCache` / `TBlockCache`),
   and a `TFileServiceBase` (the file registry) behind a single handle.
   Passed around as `Util::TEngine *` to nearly every disk-layer
   consumer -- readers, writers, walkers, mergers -- so they don't
   each have to take five constructor arguments.

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
#include <orly/indy/disk/file_service_base.h>
#include <orly/indy/disk/util/cache.h>
#include <orly/indy/disk/util/volume_manager.h>
#include <orly/indy/util/block_vec.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        typedef TCache<PhysicalPageSize> TPageCache;
        typedef TCache<PhysicalBlockSize> TBlockCache;

        class TEngine {
          NO_COPY(TEngine);
          public:

          TEngine(TVolumeManager *vol_man,
                  TPageCache *page_cache,
                  TBlockCache *block_cache,
                  TFileServiceBase *file_service,
                  bool is_disk_engine)
              : VolMan(vol_man),
                PageCache(page_cache),
                BlockCache(block_cache),
                FileService(file_service),
                IsDiskBasedEngine(is_disk_engine) {}

          ~TEngine() {}

          bool FindFile(const Base::TUuid &file_uid, size_t gen_id, size_t &starting_block, size_t &starting_block_offset, size_t &file_length, size_t &num_keys) {
            return FileService->FindFile(file_uid, gen_id, starting_block, starting_block_offset, file_length, num_keys);
          }

          void InsertFile(const Base::TUuid &file_uid,
                          TFileObj::TKind file_kind,
                          size_t gen_id,
                          size_t starting_block_id,
                          size_t starting_block_offset,
                          size_t file_size,
                          size_t num_keys,
                          TSequenceNumber lowest_seq,
                          TSequenceNumber highest_seq,
                          TCompletionTrigger &completion_trigger) {
            FileService->InsertFile(file_uid, file_kind, gen_id, starting_block_id, starting_block_offset, file_size, num_keys, lowest_seq, highest_seq, completion_trigger);
          }

          void RemoveFile(const Base::TUuid &file_uid, size_t gen_id, TCompletionTrigger &completion_trigger) {
            FileService->RemoveFile(file_uid, gen_id, completion_trigger);
          }

          void AppendFileGenSet(const Base::TUuid &file_uid, std::vector<TFileObj> &out_vec) {
            return FileService->AppendFileGenSet(file_uid, out_vec);
          }

          bool ForEachFile(const std::function<bool (const Base::TUuid &file_uid, const TFileObj &)> &cb) {
            return FileService->ForEachFile(cb);
          }

          size_t ReserveBlock(TVolume::TDesc::TStorageSpeed storage_speed) {
            TBlockRange range;
            VolMan->TryAllocateSequentialBlocks(storage_speed, 1UL, [&](const TBlockRange &block_range) {
              range = block_range;
            });
            assert(range.second == 1UL);
            return range.first;
          }

          void AppendReserveBlocks(TVolume::TDesc::TStorageSpeed storage_speed, size_t num_blocks, std::vector<size_t> &append_vec,
                                   TAllocClass alloc_class = TAllocClass::Ordinary) {
            assert(num_blocks > 0);
            size_t left = num_blocks;
            try {
              while (left > 0) {
                VolMan->TryAllocateSequentialBlocks(storage_speed, left, [&](const TBlockRange &range) {
                  for (size_t i = 0; i < range.second; ++i) {
                    assert(GetPageCache()->AssertNoRefCount((range.first + i) * 16));
                    assert(GetBlockCache()->AssertNoRefCount(range.first + i));
                    append_vec.push_back(range.first + i);
                  }
                  left -= range.second;
                }, alloc_class);
              }
            } catch (const std::exception &/*ex*/) {
              /* Hand back what this call reserved, not just forget it: the caller only owns
                 what was in append_vec before the call (#590). */
              const auto first_new = append_vec.begin() + (append_vec.size() - (num_blocks - left));
              for (auto iter = first_new; iter != append_vec.end(); ++iter) {
                FreeBlock(*iter);
              }
              append_vec.erase(first_new, append_vec.end());
              throw;
            }
          }

          /* On failure (TDiskFull once every volume is full), the blocks this call reserved are
             freed again and append_vec is left as it was (#590). */
          void AppendReserveBlocks(TVolume::TDesc::TStorageSpeed storage_speed, size_t num_blocks, Indy::Util::TBlockVec &append_vec,
                                   TAllocClass alloc_class = TAllocClass::Ordinary) {
            assert(num_blocks > 0);
            const size_t orig_size = append_vec.Size();
            size_t left = num_blocks;
            try {
              while (left > 0) {
                VolMan->TryAllocateSequentialBlocks(storage_speed, left, [&](const TBlockRange &range) {
                  append_vec.PushBack(range);
                  #ifndef NDEBUG
                  for (size_t i = 0; i < range.second; ++i) {
                    assert(GetPageCache()->AssertNoRefCount((range.first + i) * 16));
                    assert(GetBlockCache()->AssertNoRefCount(range.first + i));
                  }
                  #endif
                  left -= range.second;
                }, alloc_class);
              }
            } catch (const std::exception &/*ex*/) {
              const size_t num_new = append_vec.Size() - orig_size;
              if (num_new) {
                append_vec.ForEachSeqRangeInRange([this](size_t block_id, size_t num_seq_blocks) -> bool {
                  FreeSeqBlocks(block_id, num_seq_blocks);
                  return true;
                }, orig_size, append_vec.Size());
                append_vec.Trim(num_new);
              }
              throw;
            }
          }

          void FreeBlock(size_t block_id) {
            VolMan->FreeSequentialBlocks(TBlockRange(block_id, 1UL));
          }

          void FreeSeqBlocks(size_t block_id, size_t num_blocks) {
            VolMan->FreeSequentialBlocks(TBlockRange(block_id, num_blocks));
          }

          /* Free every block in block_vec and empty it. For a file builder giving up before its
             file reached the file map (#590). */
          void FreeAllBlocks(Indy::Util::TBlockVec &block_vec) {
            if (block_vec.Size()) {
              for (const auto &iter : block_vec.GetSeqBlockMap()) {
                FreeSeqBlocks(iter.second.first, iter.second.second);
              }
              block_vec.Trim(block_vec.Size());
            }
          }

          inline TVolumeManager *GetVolMan() const {
            return VolMan;
          }

          template <size_t PhysicalCacheSize>
          inline Util::TCache<PhysicalCacheSize> *GetCache() const {
            return TCacheGetter<PhysicalCacheSize>::Get(this);
          }

          inline TPageCache *GetPageCache() const {
            return PageCache;
          }

          inline TBlockCache *GetBlockCache() const {
            return BlockCache;
          }

          inline bool IsDiskBased() const {
            return IsDiskBasedEngine;
          }

          private:

          template <size_t PhysicalCacheSize>
          class TCacheGetter {
            NO_CONSTRUCTION(TCacheGetter);
            public:

            static Util::TCache<PhysicalCacheSize> *Get(TEngine *) {
              //static_assert(false, "TCacheGetter not specialized on the cache size you are trying to get");
            }

          };  // TCacheGetter

          TVolumeManager *VolMan;

          TPageCache *PageCache;

          TBlockCache *BlockCache;

          TFileServiceBase *FileService;

          bool IsDiskBasedEngine;

        };  // TEngine

        template <>
        class TEngine::TCacheGetter<4096> {
          public:
          static Util::TCache<4096> *Get(const TEngine *engine) {
            return engine->PageCache;
          }
        };

        template <>
        class TEngine::TCacheGetter<65536> {
          public:
          static Util::TCache<65536> *Get(const TEngine *engine) {
            return engine->BlockCache;
          }
        };

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly