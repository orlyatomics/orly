/* <orly/indy/disk/buf_block.h>

   Page-aligned, mlock'd block allocator backing the disk-layer buffer cache.
   `TBufBlock`'s `operator new` / `operator delete` route through the static
   `TPool` free-list so block reuse never returns to the heap. When the pool
   is exhausted, allocators block on a condition variable until another
   block is freed.

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
#include <condition_variable>
#include <mutex>

#include <syslog.h>
#include <unistd.h>

#include <base/class_traits.h>
#include <base/mem_aligned_ptr.h>
#include <base/mlock.h>
#include <base/spin_lock.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TBufBlock {
        NO_COPY(TBufBlock);
        public:

        class TPool {
          NO_COPY(TPool);
          public:

          TPool(size_t block_size, size_t block_count = 0UL)
              : BlockSize(block_size), Blob(nullptr), FirstBlock(nullptr), NumBlocksUsed(0UL), MaxBlocks(0UL) {
            assert(block_size % getpagesize() == 0);
            if (block_count) {
              Init(block_count);
            }
          }

          size_t GetNumBlocksUsed() const {
            return NumBlocksUsed;
          }

          size_t GetMaxBlocks() const {
            return MaxBlocks;
          }

          void Init(size_t block_count) {
            assert(MaxBlocks == 0UL);
            MaxBlocks = block_count;
            if (block_count) {
              Blob = Base::MemAlignedAlloc<TBlock>(getpagesize(), BlockSize * block_count);
              Base::MlockRaw(Blob.get(), BlockSize * block_count);
              #ifndef NDEBUG
              memset(Blob.get(), 0, BlockSize * block_count);
              #endif
              assert(Blob);
              syslog(LOG_INFO, "TBufBlock allocated [%ld] bytes for [%ld] blocks of size [%ld]", (BlockSize * block_count), block_count, BlockSize);
              TBlock
                  *prev_block = reinterpret_cast<TBlock *>(&FirstBlock),
                  *block      = Blob.get();
              for (size_t i = 0; i < block_count; ++i) {
                assert(block);
                prev_block->NextBlock = block;
                block     ->NextBlock = 0;
                prev_block = block;
                block      = reinterpret_cast<TBlock *>(reinterpret_cast<uint8_t *>(block) + BlockSize);
              }
            }
          }

          void *Alloc() {
            //syslog(LOG_INFO, "BufBlock [%ld] / [%ld]", NumBlocksUsed, MaxBlocks);
            void *ptr = TryAlloc();
            while (!ptr) {
              //auto now = std::chrono::system_clock::now();
              //now += std::chrono::milliseconds(1000);
              /* wait for someone to release one for us */ {
                std::unique_lock<std::mutex> lock(Lock);
                while (NumBlocksUsed >= MaxBlocks) {
                  Cond.wait(lock/*, now*/);
                }
              }
              ptr = TryAlloc();
              //syslog(LOG_EMERG, "Bad alloc in buf block pool");
              //throw std::bad_alloc();
            }
            return ptr;
          }

          void Free(void *ptr) {
            assert(ptr);
            TBlock *block = static_cast<TBlock *>(ptr);
            //Base::TSpinLock::TSoftLock lock(SpinLock);
            std::lock_guard<std::mutex> lock(Lock);
            block->NextBlock = FirstBlock;
            --NumBlocksUsed;
            FirstBlock = block;
            Cond.notify_one();
          }

          void *TryAlloc() {
            //Base::TSpinLock::TSoftLock lock(SpinLock);
            std::lock_guard<std::mutex> lock(Lock);
            TBlock *block = FirstBlock;
            FirstBlock = (block ? block->NextBlock : 0);
            if (block) {
              FirstBlock = block->NextBlock;
              ++NumBlocksUsed;
            } else {
              FirstBlock = nullptr;
            }
            return block;
          }

          private:

          struct TBlock {
            TBlock *NextBlock;

          };  // TBlock

          const size_t BlockSize;

          std::unique_ptr<TBlock> Blob;

          TBlock *FirstBlock;

          //Base::TSpinLock SpinLock;
          std::mutex Lock;
          std::condition_variable Cond;

          size_t NumBlocksUsed;

          size_t MaxBlocks;

        };  // TPool

        TBufBlock() {}

        ~TBufBlock() {}

        static void *operator new(size_t /*size*/) {
          return Pool.Alloc();
        }

        static void operator delete(void *ptr) {
          Pool.Free(ptr);
        }

        static TPool Pool;

        char *GetData() const {
          return &Buf;
        }

        private:

        mutable char Buf;

      };  // TBufBlock

    }  // Disk

  }  // Indy

}  // Orly
