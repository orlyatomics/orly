/* <orly/indy/util/pool.cc>

   Implements <orly/indy/util/pool.h>.

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

/* Before pool.h: fiber.h says Util::ThrowSystemError, meaning ::Util, which Orly::Indy::Util
   (declared by pool.h) would hide. */
#include <orly/indy/fiber/fiber.h>

#include <orly/indy/util/pool.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string.h>

#include <base/mlock.h>

using namespace std;
using namespace Orly::Indy::Util;

TPool::TPool(size_t block_size, const char *name, size_t block_count)
    : BlockSize(block_size),
      Blob(nullptr),
      FirstBlock(nullptr),
      Name(name),
      NumBlocksUsed(0UL),
      MaxBlocks(0UL),
      Reserve(0UL),
      NumBlocksAdmitted(0UL),
      NumBlocksClaimed(0UL),
      Refusing(false),
      NumMisses(0UL) {
  assert(block_size >= sizeof(void*));
  if (block_count) {
    Init(block_count);
  }
}

void TPool::Init(size_t block_count) {
  assert(MaxBlocks == 0UL);
  MaxBlocks = block_count;
  if (block_count) {
    Blob = malloc(BlockSize * block_count);
    if (!Blob) {
      syslog(LOG_EMERG, "TPool::Init() [%s] bad_alloc while trying to init pool of %ld blocks [%ld bytes]", Name, block_count, (BlockSize * block_count));
      throw std::bad_alloc();
    }
    Base::MlockRaw(Blob, BlockSize * block_count);
    #ifndef NDEBUG
    memset(Blob, 0, BlockSize * block_count);
    #endif
    syslog(LOG_INFO, "TPool [%s] allocated [%ld] bytes for [%ld] blocks of size [%ld]", Name, (BlockSize * block_count), block_count, BlockSize);
    TBlock
        *prev_block = reinterpret_cast<TBlock *>(&FirstBlock),
        *block      = static_cast<TBlock *>(Blob);
    for (size_t i = 0; i < block_count; ++i) {
      prev_block->NextBlock = block;
      block     ->NextBlock = 0;
      prev_block = block;
      block      = reinterpret_cast<TBlock *>(reinterpret_cast<uint8_t *>(block) + BlockSize);
    }
  }
}

TPool::~TPool() {
  if (NumBlocksUsed) {
    syslog(LOG_ERR, "[%ld] Blocks left in [%s] pool", NumBlocksUsed.load(), Name);
  }
  free(Blob);
}

void *TPool::Alloc(size_t size) {
  void *ptr = TryAlloc(size);
  if (ptr) {
    return ptr;
  }
  /* See the header: on a fiber, fail now and let the caller retry where it holds no lock. */
  const bool on_fiber = Orly::Indy::Fiber::TFrame::LocalFrame.Get() != nullptr;
  if (!on_fiber) {
    for (size_t retry = 0; retry < 2000UL && !ptr; ++retry) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ptr = TryAlloc(size);
    }
    if (ptr) {
      return ptr;
    }
  }
  /* A pool that runs dry is retried, so count every miss but log only the 1st, 2nd, 4th,
     8th, ... per pool. */
  const size_t misses = ++NumMisses;
  if ((misses & (misses - 1UL)) == 0UL) {
    syslog(LOG_ERR, "TPool::Alloc() [%s] bad_alloc %s; %ld blocks of %ld in use, %ld misses so far",
           Name, on_fiber ? "on a fiber (not waiting)" : "after 2000 retries", NumBlocksUsed.load(), MaxBlocks, misses);
  }
  throw std::bad_alloc();
}

void TPool::SetReserve(size_t reserve_blocks) {
  std::lock_guard<std::mutex> lock(Mutex);
  Reserve = std::min(reserve_blocks, MaxBlocks);
  Refusing = false;
}

bool TPool::TryAdmit(size_t num_blocks) {
  std::lock_guard<std::mutex> lock(Mutex);
  const size_t reserve = Reserve.load();
  if (reserve) {
    const size_t keep_free = std::min(MaxBlocks, reserve + (Refusing ? reserve / 4UL : 0UL));
    if (NumBlocksUsed + NumBlocksAdmitted + NumBlocksClaimed + num_blocks > MaxBlocks - keep_free) {
      Refusing = true;
      return false;
    }
    Refusing = false;
  }
  NumBlocksAdmitted += num_blocks;
  return true;
}

void TPool::ReleaseAdmitted(size_t num_blocks) {
  std::lock_guard<std::mutex> lock(Mutex);
  assert(NumBlocksAdmitted >= num_blocks);
  NumBlocksAdmitted -= num_blocks;
}

bool TPool::TryClaim(size_t num_blocks) {
  if (!num_blocks) {
    return true;
  }
  std::lock_guard<std::mutex> lock(Mutex);
  if (NumBlocksUsed + NumBlocksAdmitted + NumBlocksClaimed + num_blocks > MaxBlocks) {
    return false;
  }
  NumBlocksClaimed += num_blocks;
  return true;
}

void TPool::ReleaseClaim(size_t num_blocks) {
  if (!num_blocks) {
    return;
  }
  std::lock_guard<std::mutex> lock(Mutex);
  assert(NumBlocksClaimed >= num_blocks);
  NumBlocksClaimed -= num_blocks;
}

void TPool::Free(void *ptr) {
  assert(ptr);
  TBlock *block = static_cast<TBlock *>(ptr);
  std::lock_guard<std::mutex> lock(Mutex);
  block->NextBlock = FirstBlock;
  --NumBlocksUsed;
  FirstBlock = block;
}

void *TPool::TryAlloc(size_t size) {
  assert(size <= BlockSize);
  std::lock_guard<std::mutex> lock(Mutex);
  TBlock *block = FirstBlock;
  if (block) {
    FirstBlock = block->NextBlock;
    ++NumBlocksUsed;
  } else {
    FirstBlock = nullptr;
  }
  return block;
}
