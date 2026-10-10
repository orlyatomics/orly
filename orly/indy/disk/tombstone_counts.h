/* <orly/indy/disk/tombstone_counts.h>

   How many of a data file's current keys are tombstones, by stretch of rank, so a count can
   pass over a stretch of the file without reading each key (#749).

   A file's current keys sit in a sorted array of fixed-size entries, so the keys in a range
   number the difference of two ranks. Some of those entries are tombstones, though, which a
   count must leave out. This keeps the number of tombstones in each block of Stride entries,
   and counts a stretch as the sum over the blocks it covers plus a scan of the entries it
   covers in the blocks at its two ends.

   A block's number is found the first time a count covers any of it, by reading the block's
   entries, and then kept for the life of the file (the file never changes). So a count costs
   about as much as walking its stretch the first time, and the second time it reads only
   the blocks at the ends of the stretch, and those only when they hold a tombstone.

   Stride is 2048. It trades the work of the edge scans, at most Stride - 1 entries at each end
   (2047 entries of 64 bytes, 128 KB, and none when the end block holds no tombstone), against
   memory and the sum over whole blocks: 4 bytes per 2048 keys, 20 KB for a 10M key file, and
   about 5k additions to count all of it. Smaller blocks cut the edge scans but not below the
   cost of the two binary searches a stretch already pays, while the sum and the memory grow;
   larger ones make the first count of a short stretch read a whole block it mostly does not
   need (256 KB at 4096) and the edge scans of every later one longer.

   A count may run on any fiber of any runner at once with another on the same file. The
   numbers are atomics with a sentinel for "not read yet". Two counts that find a block unread
   both read it and store the same number, so nothing locks and nothing waits on disk while
   holding a lock.

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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <base/class_traits.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TTombstoneCounts {
        NO_COPY(TTombstoneCounts);
        public:

        /* The entries per block. See the top of the file. */
        static constexpr size_t Stride = 2048UL;

        /* For num_keys current keys, none of them read yet. */
        explicit TTombstoneCounts(size_t num_keys)
            : NumKeys(num_keys), NumBlocks((num_keys + Stride - 1UL) / Stride), Counts(std::make_unique<std::atomic<uint32_t>[]>(NumBlocks)) {
          for (size_t block = 0UL; block < NumBlocks; ++block) {
            Counts[block].store(Unknown, std::memory_order_relaxed);
          }
        }

        /* The current keys this covers. */
        size_t GetNumKeys() const {
          return NumKeys;
        }

        /* The tombstones among the entries of rank [begin, end). `scan(b, e)` must return the
           tombstones among the entries of rank [b, e), by reading them; it is called only for
           whole blocks not read before, and for the parts of the two end blocks the stretch
           covers when those blocks hold a tombstone. */
        template <typename TScan>
        size_t Count(size_t begin, size_t end, const TScan &scan) const {
          assert(begin <= end);
          assert(end <= NumKeys);
          if (begin == end) {
            return 0UL;
          }
          const size_t first_block = begin / Stride, last_block = (end - 1UL) / Stride;
          if (first_block == last_block) {
            const size_t in_block = GetBlock(first_block, scan);
            return (!in_block || (begin == GetBlockBegin(first_block) && end == GetBlockEnd(first_block))) ? in_block : scan(begin, end);
          }
          size_t total = 0UL;
          /* The whole blocks run [full_begin, full_end). */
          size_t full_begin = first_block, full_end = last_block + 1UL;
          if (begin != GetBlockBegin(first_block)) {
            if (GetBlock(first_block, scan)) {
              total += scan(begin, GetBlockEnd(first_block));
            }
            ++full_begin;
          }
          if (end != GetBlockEnd(last_block)) {
            if (GetBlock(last_block, scan)) {
              total += scan(GetBlockBegin(last_block), end);
            }
            --full_end;
          }
          for (size_t block = full_begin; block < full_end; ++block) {
            total += GetBlock(block, scan);
          }
          return total;
        }

        /* True iff the block's number has been read. For tests. */
        bool IsKnown(size_t block) const {
          assert(block < NumBlocks);
          return Counts[block].load(std::memory_order_relaxed) != Unknown;
        }

        private:

        /* Marks a block not read yet. A block holds at most Stride tombstones. */
        static constexpr uint32_t Unknown = UINT32_MAX;

        static_assert(Stride < Unknown, "a block's count must fit beside the sentinel");

        size_t GetBlockBegin(size_t block) const {
          return block * Stride;
        }

        size_t GetBlockEnd(size_t block) const {
          return std::min(NumKeys, (block + 1UL) * Stride);
        }

        /* The tombstones in the block, reading it if no count has yet. */
        template <typename TScan>
        size_t GetBlock(size_t block, const TScan &scan) const {
          assert(block < NumBlocks);
          uint32_t count = Counts[block].load(std::memory_order_relaxed);
          if (count == Unknown) {
            count = static_cast<uint32_t>(scan(GetBlockBegin(block), GetBlockEnd(block)));
            assert(count <= Stride);
            /* Relaxed is enough: the number depends only on the file, which never changes, so a
               racing count that stores it too stores the same number. */
            Counts[block].store(count, std::memory_order_relaxed);
          }
          return count;
        }

        const size_t NumKeys;

        const size_t NumBlocks;

        /* The tombstones in each block, or Unknown. */
        const std::unique_ptr<std::atomic<uint32_t>[]> Counts;

      };  // TTombstoneCounts

    }  // Disk

  }  // Indy

}  // Orly
