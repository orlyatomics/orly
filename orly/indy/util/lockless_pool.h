/* <orly/indy/util/lockless_pool.h>

   `TLocklessPool` -- a fixed-capacity block allocator with no
   internal locking, relying on atomic operations on the free-list.
   Counterpart of `TPool` (mutex-guarded) and `TGrowingPool`
   (resizable). Used in hot allocation paths where contention is
   the bottleneck.

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
#include <cstddef>
#include <new>
#include <syslog.h>

#include <atomic>

#include <base/class_traits.h>

namespace Orly {

  namespace Indy {

    namespace Util {

      class TLocklessPool {
        NO_COPY(TLocklessPool);
        public:

        TLocklessPool(size_t block_size, const char *name, size_t block_count = 0UL);

        ~TLocklessPool();

        void Init(size_t block_count);

        inline const char *GetName() const;

        inline size_t GetNumBlocksUsed() const;

        inline size_t GetMaxBlocks() const;

        void *Alloc(size_t size) {
          void *ptr = TryAlloc(size);
          if (!ptr) {
            syslog(LOG_EMERG, "TLocklessPool::Alloc() [%s] bad_alloc", Name);
            throw std::bad_alloc();
          }
          NumBlocksUsed.store(NumBlocksUsed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
          return ptr;
        }

        inline void Free(void *ptr);

        inline void *TryAlloc(size_t size);

        private:

        class TBlock {
          NO_COPY(TBlock);
          public:

          TBlock *NextBlock;

        };  // TBlock

        const size_t BlockSize;

        void *Blob;

        TBlock *FirstBlock;

        const char *Name;

        /* Callers serialize Alloc and Free (the pool takes no lock of its own), but the reporting
           port reads this count from its own thread, so it is atomic (#713). Only the reader needs
           that: the writers' load-and-store is not a read-modify-write, and costs nothing more. */
        std::atomic<size_t> NumBlocksUsed;

        size_t MaxBlocks;

      };  // TLocklessPool

      inline const char *TLocklessPool::GetName() const {
        return Name;
      }

      inline size_t TLocklessPool::GetNumBlocksUsed() const {
        return NumBlocksUsed.load(std::memory_order_relaxed);
      }

      inline size_t TLocklessPool::GetMaxBlocks() const {
        return MaxBlocks;
      }

      inline void TLocklessPool::Free(void *ptr) {
        assert(ptr);
        TBlock *block = static_cast<TBlock *>(ptr);
        block->NextBlock = FirstBlock;
        NumBlocksUsed.store(NumBlocksUsed.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
        FirstBlock = block;
      }

      inline void *TLocklessPool::TryAlloc(size_t size) {
        assert(size <= BlockSize);
        TBlock *block = FirstBlock;
        FirstBlock = (block ? block->NextBlock : 0);
        return block;
      }

    }  // Util

  }  // Indy

}  // Orly
