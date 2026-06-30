/* <orly/indy/util/pool.h>

   `TPool` -- a fixed-capacity block allocator with mutex-guarded
   alloc / free. The "boring middle" of the pool family:
   `TLocklessPool` is faster under contention but requires atomic
   ops; `TGrowingPool` is more flexible but pays for the grow logic.

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
#include <cstddef>
#include <mutex>
#include <new>
#include <thread>
#include <syslog.h>

#include <base/class_traits.h>

namespace Orly {

  namespace Indy {

    namespace Util {

      class TPool {
        NO_COPY(TPool);
        public:

        TPool(size_t block_size, const char *name, size_t block_count = 0UL);

        ~TPool();

        void Init(size_t block_count);

        inline const char *GetName() const;

        inline size_t GetNumBlocksUsed() const;

        inline size_t GetMaxBlocks() const;

        void *Alloc(size_t size) {
          void *ptr = TryAlloc(size);
          if (!ptr) {
            size_t retry = 2000UL;
            while (!ptr && retry) {
              --retry;
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
              ptr = TryAlloc(size);
            }
            if (ptr) {
              return ptr;
            }
            syslog(LOG_EMERG, "TPool::Alloc() [%s] bad_alloc after %ld retries", Name, 2000UL);
            throw std::bad_alloc();
          }
          return ptr;
        }

        void Free(void *ptr);

        void *TryAlloc(size_t size);

        private:

        class TBlock {
          NO_COPY(TBlock);
          public:

          TBlock *NextBlock;

        };  // TBlock

        const size_t BlockSize;

        void *Blob;

        TBlock *FirstBlock;

        std::mutex Mutex;

        const char *Name;

        std::atomic<size_t> NumBlocksUsed;

        size_t MaxBlocks;

      };  // TPool

      inline const char *TPool::GetName() const {
        return Name;
      }

      inline size_t TPool::GetNumBlocksUsed() const {
        return NumBlocksUsed.load();
      }

      inline size_t TPool::GetMaxBlocks() const {
        return MaxBlocks;
      }

    }  // Util

  }  // Indy

}  // Orly
