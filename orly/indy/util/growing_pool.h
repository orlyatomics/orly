/* <orly/indy/util/growing_pool.h>

   `TGrowingPool` -- a block allocator that grows on demand.
   Counterpart of `TPool` (fixed-capacity, mutex-guarded) and
   `TLocklessPool` (fixed-capacity, lock-free). Used wherever the
   disk layer needs many small allocations of a known block size
   without knowing the count up front.

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
#include <mutex>
#include <new>
#include <time.h>
#include <syslog.h>

#include <base/class_traits.h>

namespace Orly {

  namespace Indy {

    namespace Util {

      class TGrowingPool {
        NO_COPY(TGrowingPool);
        public:

        TGrowingPool(size_t block_size, const char *name);

        ~TGrowingPool();

        inline const char *GetName() const;

        inline size_t GetNumBlocksUsed() const;

        inline size_t GetMaxBlocks() const;

        void *Alloc(size_t size) {
          void *ptr = TryAlloc(size);
          if (!ptr) {
            syslog(LOG_EMERG, "TGrowingPool::Alloc() [%s] bad_alloc", Name);
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

        size_t NumBlocksUsed;

        size_t MaxBlocks;

      };  // TGrowingPool

      inline const char *TGrowingPool::GetName() const {
        return Name;
      }

      inline size_t TGrowingPool::GetNumBlocksUsed() const {
        return NumBlocksUsed;
      }

      inline size_t TGrowingPool::GetMaxBlocks() const {
        return MaxBlocks;
      }

    }  // Util

  }  // Indy

}  // Orly
