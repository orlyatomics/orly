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

        /* Takes a block. On a miss, off a fiber (startup, tests), retries for up to 2 s.

           On a fiber a miss throws std::bad_alloc at once (#607). Sleeping here held up every
           other fiber on the runner for up to 2 s (the #584 starvation class), and parking the
           fiber instead isn't safe: callers allocate while holding plain mutexes, which a
           parked fiber keeps, so another fiber on the same thread that wants one blocks the
           thread for good. The callers that can wait retry where they hold no lock: the Tetris
           player yields and plays the round again, the memory merge rolls back and is queued
           again, and a write is refused before it allocates (TryAdmit). */
        void *Alloc(size_t size);

        void Free(void *ptr);

        void *TryAlloc(size_t size);

        /* Writer admission (#607). The last Reserve blocks are kept for the merges and the
           Tetris promotions that free the pool: a writer first asks TryAdmit for the blocks it
           is about to take, and is refused once the blocks in use plus those already promised
           to writers would leave fewer than Reserve free. Allocations themselves are never
           restricted, so the merges can always use the reserve.

           Once refusing, a writer is admitted again only when a further Reserve / 4 is free, so
           admission doesn't flap at the edge. A reserve of 0 refuses nothing, but still counts
           what writers hold. */
        void SetReserve(size_t reserve_blocks);

        inline size_t GetReserve() const;

        /* What TryAdmit saw when it refused a writer (#719), for the refusal message. */
        struct TRefusal {
          size_t Used = 0UL;
          size_t Admitted = 0UL;
          size_t Claimed = 0UL;
          size_t Asked = 0UL;
          size_t Limit = 0UL;
        };

        /* Promises num_blocks to a writer, or returns false and promises nothing. A writer that
           was admitted calls ReleaseAdmitted with the same count once it has allocated. On a
           refusal, fills *refusal (if given) with the counts it compared. */
        bool TryAdmit(size_t num_blocks, TRefusal *refusal = nullptr);

        void ReleaseAdmitted(size_t num_blocks);

        /* Claims for the copies that free the pool (#607): a memory merge or a Tetris promotion
           claims every block its copy will take before it copies anything, and releases the
           claim once the copy is done. A claim is granted only if the blocks in use, those
           promised to writers and those already claimed leave room for all of it, so two
           copies never each hold part of what they need while both wait for the rest. A copy
           that is refused waits holding nothing. Writers count claims too (TryAdmit). Blocks a
           claimant has already allocated count twice until it releases its claim, which errs
           towards refusing. */
        bool TryClaim(size_t num_blocks);

        void ReleaseClaim(size_t num_blocks);

        inline size_t GetNumBlocksClaimed() const;

        inline size_t GetNumBlocksAdmitted() const;

        inline bool IsRefusing() const;

        /* How many allocations have failed, ever. */
        inline size_t GetNumMisses() const;

        /* Wakes writers waiting at admission when room comes back (#765). Once armed, the first
           Free, ReleaseAdmitted or ReleaseClaim that leaves the blocks in use, promised and
           claimed at or below level disarms the watch and, outside the pool's lock, calls the
           process's room callback. One watch per pool; arming again replaces the level.

           A waiter arms the watch and then retries TryAdmit: a release that lands before the
           retry is seen by it, and one that lands after sees the armed level, so no wake is
           lost. */
        void ArmRoomWake(size_t level);

        void DisarmRoomWake();

        /* The level the watch is armed at, or NoRoomWake. */
        inline size_t GetRoomWakeLevel() const;

        static constexpr size_t NoRoomWake = static_cast<size_t>(-1);

        /* The callback every pool's watch calls (#765). It runs on whatever thread released the
           blocks, possibly holding that caller's locks (but not this pool's), so it must only
           signal: take no lock but its own leaf one and never wait. Null turns it off. */
        static void SetRoomCallback(void (*callback)());

        private:

        /* Called with Mutex held after the counts went down: disarms the watch and returns true
           if they reached its level. */
        inline bool TakeRoomWake();

        static void CallRoomCallback();

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

        /* See SetReserve. Written under Mutex, read without it by the reporter. */
        std::atomic<size_t> Reserve;

        std::atomic<size_t> NumBlocksAdmitted;

        std::atomic<size_t> NumBlocksClaimed;

        std::atomic<bool> Refusing;

        std::atomic<size_t> NumMisses;

        /* See ArmRoomWake. Written under Mutex or by a waiter arming it; read under Mutex. */
        std::atomic<size_t> RoomWakeLevel;

        static std::atomic<void (*)()> RoomCallback;

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

      inline size_t TPool::GetReserve() const {
        return Reserve.load();
      }

      inline size_t TPool::GetNumBlocksAdmitted() const {
        return NumBlocksAdmitted.load();
      }

      inline size_t TPool::GetNumBlocksClaimed() const {
        return NumBlocksClaimed.load();
      }

      inline bool TPool::IsRefusing() const {
        return Refusing.load();
      }

      inline size_t TPool::GetNumMisses() const {
        return NumMisses.load();
      }

      inline size_t TPool::GetRoomWakeLevel() const {
        return RoomWakeLevel.load();
      }

      inline bool TPool::TakeRoomWake() {
        const size_t level = RoomWakeLevel.load(std::memory_order_relaxed);
        if (level == NoRoomWake || NumBlocksUsed + NumBlocksAdmitted + NumBlocksClaimed > level) {
          return false;
        }
        RoomWakeLevel = NoRoomWake;
        return true;
      }

    }  // Util

  }  // Indy

}  // Orly
