/* <orly/indy/disk/key_filter.h>

   A filter over the prefix hashes one of a data file's hash tables holds, so a read can pass over a file that cannot hold its
   key without building a walker for the file or reading its hash table.

   A data file keeps, for each index and each prefix length, a hash table of the distinct prefixes of that length its current
   keys have (TIndexFile::FindInHash). A read for a key, or for a pattern of defined members followed by free ones, finds the
   file holds nothing for it when no entry in the table for its number of defined members has its hash. This filter answers
   the same question from memory: MayContain(hash) is false only when no entry in the table has that hash, so a file it rules
   out is one FindInHash would have found empty. It may say true for a hash the table lacks, about 0.5% of the time.

   It is a split block Bloom filter: each hash picks one 32-byte block and sets one bit in each of the block's eight 32-bit
   words, so a probe reads one cache line. It takes 12 bits per entry, 1.5 bytes, rounded up to a whole block.

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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TKeyFilter {
        public:

        /* Bits of filter per entry. */
        static constexpr size_t BitsPerEntry = 12UL;

        /* A filter that holds exactly the given hashes (and may report others). With none it reports nothing. */
        explicit TKeyFilter(std::span<const uint64_t> hashes) {
          const size_t num_blocks = (hashes.size() * BitsPerEntry + BlockBits - 1UL) / BlockBits;
          Blocks.resize(num_blocks, TBlock{});
          for (const uint64_t hash : hashes) {
            const uint64_t mixed = Mix(hash);
            TBlock &block = Blocks[PickBlock(mixed)];
            const uint32_t low = static_cast<uint32_t>(mixed);
            for (size_t i = 0; i < WordsPerBlock; ++i) {
              block[i] |= Bit(low, i);
            }
          }
        }

        /* False only when no hash given to the constructor equals this one. */
        [[nodiscard]] bool MayContain(uint64_t hash) const noexcept {
          if (Blocks.empty()) {
            return false;
          }
          const uint64_t mixed = Mix(hash);
          const TBlock &block = Blocks[PickBlock(mixed)];
          const uint32_t low = static_cast<uint32_t>(mixed);
          for (size_t i = 0; i < WordsPerBlock; ++i) {
            if (!(block[i] & Bit(low, i))) {
              return false;
            }
          }
          return true;
        }

        /* The memory the filter's bits take. */
        [[nodiscard]] size_t GetNumBytes() const noexcept {
          return Blocks.size() * sizeof(TBlock);
        }

        private:

        static constexpr size_t WordsPerBlock = 8UL;

        static constexpr size_t BlockBits = WordsPerBlock * 32UL;

        using TBlock = std::array<uint32_t, WordsPerBlock>;

        /* Odd multipliers, one per word, that spread the low half of the mixed hash over the word's 32 bits. */
        static constexpr std::array<uint32_t, WordsPerBlock> Salts{
            0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU, 0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

        /* A key's stored hash need not be well spread (a small integer's may be itself), so mix it first (the 64-bit
           finalizer of MurmurHash3). */
        static constexpr uint64_t Mix(uint64_t hash) noexcept {
          hash ^= hash >> 33;
          hash *= 0xff51afd7ed558ccdULL;
          hash ^= hash >> 33;
          hash *= 0xc4ceb9fe1a85ec53ULL;
          hash ^= hash >> 33;
          return hash;
        }

        /* The high half of the mixed hash, scaled onto the blocks. */
        size_t PickBlock(uint64_t mixed) const noexcept {
          return static_cast<size_t>(((mixed >> 32) * static_cast<uint64_t>(Blocks.size())) >> 32);
        }

        static constexpr uint32_t Bit(uint32_t low, size_t word) noexcept {
          return 1U << ((low * Salts[word]) >> 27);
        }

        std::vector<TBlock> Blocks;

      };  // TKeyFilter

    }  // Disk

  }  // Indy

}  // Orly
