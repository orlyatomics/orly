/* <orly/indy/disk/key_filter.h>

   Which keys a data file cannot hold, kept in memory, so a point read can pass over the file
   without opening its index.

   A point read of a fully bound key looks in every disk layer for it, and each look costs a
   walker, a stream and a probe of the file's on-disk hash index, even in the layers that do not
   hold the key, which with hundreds of layers is nearly all of them. TKeyFilter is a Bloom filter
   of the hashes of a file's current keys in one index: it says "not here" for about 99% of the
   keys a file doesn't hold, and never for one it does.

   It is blocked: every key sets and tests bits of a single 64-byte block, so a test reads one
   cache line. 10 bits a key, 7 bits set per key, about 1% false positives (a little more than an
   unblocked filter's 0.8%, for one cache miss instead of seven).

   What it hashes is exactly what the file's hash index holds for the key, TKey::GetHash() of the
   whole key, which is all TReadFile::TIndexFile::FindInHash compares before it reads a key back. A
   key the index would find therefore always passes. A key with free members isn't looked up by
   its own hash (the index looks up its bound prefix), so TFileKeyFilter lets it through, as it
   does a key whose shape isn't the index's.

   Nothing here is on disk. A file's filter is made while the file is written, from the hashes the
   writer computes anyway, and lives as long as the file's disk layer. A file opened from disk has
   none, and is read as it was before. A filter never changes once made, so any number of reads
   may test it at once without a lock.

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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <orly/indy/key.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      /* A blocked Bloom filter over 64-bit hashes. */
      class TKeyFilter {
        NO_COPY(TKeyFilter);
        public:

        static constexpr size_t BitsPerKey = 10UL;

        static constexpr size_t NumProbes = 7UL;

        /* An empty filter: holds nothing. */
        TKeyFilter() noexcept = default;

        /* A filter holding the given hashes. Throws std::bad_alloc. */
        explicit TKeyFilter(const std::vector<uint64_t> &hashes);

        TKeyFilter(TKeyFilter &&) noexcept = default;

        TKeyFilter &operator=(TKeyFilter &&) noexcept = default;

        /* False only if no hash the filter was made from equals this one. */
        [[nodiscard]] bool MayContain(uint64_t hash) const noexcept;

        [[nodiscard]] size_t GetNumBytes() const noexcept {
          return NumBlocks * sizeof(TBlock);
        }

        private:

        /* One cache line of bits. */
        struct alignas(64) TBlock {
          uint64_t Words[8];
        };

        static_assert(sizeof(TBlock) == 64);

        std::unique_ptr<TBlock[]> Blocks;

        size_t NumBlocks = 0UL;

      };  // TKeyFilter

      /* The filters of every index in one data file. */
      class TFileKeyFilter {
        NO_COPY(TFileKeyFilter);
        public:

        /* One index of the file. Arity is the number of members its keys have. Without a filter
           (one was too large to make, or failed to allocate), every key of the index may be there. */
        struct TIndex {
          Base::TUuid IndexId;
          size_t Arity;
          bool HasFilter;
          TKeyFilter Filter;
        };

        /* What a read asks of every file, worked out once per read. */
        class TProbe {
          public:

          /* The key need not outlive the probe, but its index id must. */
          explicit TProbe(const TIndexKey &key);

          private:

          const Base::TUuid *IndexId;

          /* The key's member count, when it is a tuple with no free member; else 0, and only the
             index id is tested. */
          size_t Arity;

          uint64_t Hash;

          friend class TFileKeyFilter;

        };  // TProbe

        /* `indexes` holds every index the file has, each once. */
        explicit TFileKeyFilter(std::vector<TIndex> &&indexes) noexcept;

        ~TFileKeyFilter();

        /* False only if the file cannot hold the probe's key: its index isn't in the file, or the
           key is fully bound, of the index's arity, and the index's filter says no. */
        [[nodiscard]] bool MayContain(const TProbe &probe) const noexcept;

        [[nodiscard]] size_t GetNumBytes() const noexcept {
          return NumBytes;
        }

        /* Every live file filter's bytes, and their number, in this process. */
        [[nodiscard]] static size_t GetTotalBytes() noexcept;

        [[nodiscard]] static size_t GetTotalFiles() noexcept;

        /* Test only. When false, reads ignore every file's filter (TRepo::TPresentWalker), so a
           test can compare a filtered read with an unfiltered one. Nothing else clears it. */
        static std::atomic<bool> Enabled;

        /* Most keys an index's filter is made for. A writer collects 8 bytes a key until the
           index is written, and an index with more keys gets no filter. */
        static constexpr size_t MaxKeysPerIndex = 1UL << 25;

        private:

        std::vector<TIndex> Indexes;

        size_t NumBytes;

      };  // TFileKeyFilter

      /* Collects the hashes of one index's current keys as a writer emits them, and makes the
         index's filter at the end. Never throws: when it can't keep a hash it drops them all, and
         the index gets no filter. */
      class TKeyFilterBuilder {
        NO_COPY(TKeyFilterBuilder);
        public:

        TKeyFilterBuilder() = default;

        void Push(uint64_t hash) noexcept;

        /* The index's entry, with a filter if every hash was kept. Leaves the builder empty. */
        [[nodiscard]] TFileKeyFilter::TIndex Make(const Base::TUuid &index_id, size_t arity) noexcept;

        private:

        std::vector<uint64_t> Hashes;

        bool Dropped = false;

      };  // TKeyFilterBuilder

    }  // Disk

  }  // Indy

}  // Orly
