/* <orly/indy/disk/in_file.h>

   The read side of the disk layer. `TInFile` is the minimal abstract
   interface for "something we can read pages from"; `TStream<>` is the
   templated cursor that does byte-by-byte reads against the page cache
   (with optional read-ahead). `TData` collects the on-disk layout
   constants (entry sizes, meta-field counts) consumed by readers and
   writers in lockstep -- changes here ripple through `read_file.h`,
   `data_file.cc`, and the merge/walker classes.

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

#include <base/class_traits.h>
#include <base/mini_cache.h>
#include <orly/atom/kit2.h>
#include <orly/atom/suprena.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      /* Size definition class. */
      class TData {
        NO_CONSTRUCTION(TData);
        public:

        /* SequenceNumber, Key, Value, Num History Keys, Offset of History Keys,
           Mutator (+ trailing alignment padding to 8 bytes).
           Kept in sync with TKeyItem in read_file.h via static_assert there. */
        static const size_t KeyEntrySize = sizeof(TSequenceNumber) + sizeof(Atom::TCore) + sizeof(Atom::TCore) + sizeof(size_t) + sizeof(size_t) + sizeof(uint64_t);

        /* SequenceNumber, Key, Value, Mutator (+ trailing alignment padding to 8 bytes).
           Kept in sync with THistoryKeyItem in read_file.h via static_assert there. */
        static const size_t KeyHistorySize = sizeof(TSequenceNumber) + sizeof(Atom::TCore) + sizeof(Atom::TCore) + sizeof(uint64_t);

        static const size_t HashEntrySize = sizeof(Atom::TCore) + sizeof(size_t);

        static const size_t UpdateEntrySize = sizeof(TSequenceNumber) + sizeof(Atom::TCore) + sizeof(Atom::TCore) + sizeof(size_t) + sizeof(size_t);

        static const size_t UpdateKeyPtrSize = sizeof(size_t);

        static const size_t NumMetaFields = 10U;
        /*
           1.  # of blocks
           2.  # of meta-blocks (n)
           3.  # of #block / block_id pairings (number of sequential blocks starting at) (m)
           4.  # updates
           5.  # index segments (p)
           6.  # of arena notes
           7.  # of arena bytes
           8.  # of arena type boundaries
           9.  offset of main arena
           10.  offset of update index

        */

        static const size_t NumIndexMetaFields = 8U;
        /*
           Offset of Arena
           # arena notes
           # arena bytes
           # arena type boundaries
           # Current Keys
           # History Keys
           Current Key Offset
           # of hash indexes (n)

           (n) (size_t) -> (size_t) hash index offset -> num hash fields pairings
        */

        static const uint8_t NullCore[sizeof(Atom::TCore)];

        static const Atom::TCore TombstoneCore;

        static Atom::TCore GetTombstoneCore() {
          void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
          Atom::TSuprena suprena;
          return Atom::TCore(Native::TTombstone::Tombstone, &suprena, state_alloc);
        }

      };  // TData

      class TInFile {
        NO_COPY(TInFile);
        public:

        virtual size_t GetFileLength() const = 0;

        virtual size_t GetStartingBlock() const = 0;

        virtual void ReadMeta(size_t offset, size_t &out) const = 0;

        virtual size_t FindPageIdOfByte(size_t offset) const = 0;

        protected:

        TInFile() {}

        virtual ~TInFile() {}

      };  // TInFile

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, size_t MaxLocalCacheSize, bool ScanAheadAllowed = true>
      class TStream {
        NO_COPY(TStream);
        public:

        static constexpr size_t DataChunkSize = Util::GetLogicalDataChunkSize<BufKind>();
        static constexpr size_t PhysicalDataChunkSize = Util::GetPhysicalDataChunkSize<BufKind>();
        static constexpr size_t PhysicalCachePageSize = PhysicalBlockSize / (BlockSize / CachePageSize);

        TStream(const Base::TCodeLocation &code_location /* DEBUG */, uint8_t util_src, DiskPriority priority, const TInFile *file, Util::TCache<PhysicalCachePageSize> *cache, size_t byte_offset/*, bool scan_ahead_allowed = true*/)
            : TStream(code_location, util_src, priority, file->GetFileLength(), file, cache, byte_offset/*, scan_ahead_allowed*/) {}

        TStream(const Base::TCodeLocation &code_location /* DEBUG */, uint8_t util_src, DiskPriority priority, size_t end_of_stream, const TInFile *file, Util::TCache<PhysicalCachePageSize> *cache, size_t byte_offset/*, bool scan_ahead_allowed = true*/)
            : File(file),
              Cache(cache),
              EndOfStream(end_of_stream),
              LoadedPageId(0UL),
              ByteOffset(byte_offset),
              MainSlot(0),
              DataSlot(0),
              BufData(nullptr),
              FetchCount(0U),
              NumSequentialFetch(0UL),
              PrefetchedTo(0UL),
              LastFetched(0UL),
              Priority(priority),
              DiskResult(Success),
              DiskErrStr(nullptr),
              CodeLocation(code_location),
              UtilSrc(util_src) {
          assert(File);
          assert(ByteOffset <= EndOfStream);
          if (ByteOffset < EndOfStream) {
            size_t page_id_of_byte_index = FindPageIdOfByte(ByteOffset);
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
            FetchBuf(page_id_of_byte_index, ByteOffset);
            ++FetchCount;
          }
        }

        virtual ~TStream() {
          AsyncTrigger.Wait();
          if (MaxLocalCacheSize > 0) {
            /* do-little: LocalBufCache releases cache state correctly */
          } else if (MainSlot) {
            Cache->Release(MainSlot, LoadedPageId);
          }
        }

        void Read(size_t &out) {
          assert(ByteOffset + sizeof(size_t) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(size_t))) {
            //out = *reinterpret_cast<const size_t *>(BufData + OffsetInPage);
          if (OffsetInChunk <= (DataChunkSize - sizeof(size_t))) {
            out = *reinterpret_cast<const size_t *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
            ByteOffset += sizeof(size_t);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            Read(&out, sizeof(size_t));
          }
        }

        void Read(int64_t &out) {
          assert(ByteOffset + sizeof(int64_t) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(int64_t))) {
            //out = *reinterpret_cast<const int64_t *>(BufData + OffsetInPage);
          if (OffsetInChunk <= (DataChunkSize - sizeof(int64_t))) {
            out = *reinterpret_cast<const int64_t *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
            ByteOffset += sizeof(int64_t);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            Read(&out, sizeof(int64_t));
          }
        }

        void Read(uint8_t &out) {
          assert(ByteOffset + sizeof(uint8_t) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(uint8_t))) {
            //out = *reinterpret_cast<const uint8_t *>(BufData + OffsetInPage);
          if (OffsetInChunk <= (DataChunkSize - sizeof(uint8_t))) {
            out = *reinterpret_cast<const uint8_t *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
            ByteOffset += sizeof(uint8_t);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            Read(&out, sizeof(uint8_t));
          }
        }

        void Read(uint16_t &out) {
          assert(ByteOffset + sizeof(uint16_t) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(uint16_t))) {
            //out = *reinterpret_cast<const uint16_t *>(BufData + OffsetInPage);
          if (OffsetInChunk <= (DataChunkSize - sizeof(uint16_t))) {
            out = *reinterpret_cast<const uint16_t *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
            ByteOffset += sizeof(uint16_t);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            Read(&out, sizeof(uint16_t));
          }
        }

        void Read(uint32_t &out) {
          assert(ByteOffset + sizeof(uint32_t) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(uint32_t))) {
            //out = *reinterpret_cast<const uint32_t *>(BufData + OffsetInPage);
          if (OffsetInChunk <= (DataChunkSize - sizeof(uint32_t))) {
            out = *reinterpret_cast<const uint32_t *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
            ByteOffset += sizeof(uint32_t);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            Read(&out, sizeof(uint32_t));
          }
        }

        void Read(bool &out) {
          assert(ByteOffset + sizeof(bool) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(bool))) {
            //out = *reinterpret_cast<const bool *>(BufData + OffsetInPage);
          if (OffsetInChunk <= (DataChunkSize - sizeof(bool))) {
            out = *reinterpret_cast<const bool *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
            ByteOffset += sizeof(bool);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            Read(&out, sizeof(bool));
          }
        }

        void Read(Base::TUuid &out) {
          assert(ByteOffset + sizeof(uuid_t) <= EndOfStream);
          //if (OffsetInPage <= (CachePageSize - sizeof(uuid_t))) {
          if (OffsetInChunk <= (DataChunkSize - sizeof(uuid_t))) {
            uuid_t id;
            //memcpy(&id, reinterpret_cast<const uuid_t *>(BufData + OffsetInPage), sizeof(uuid_t));
            memcpy(&id, reinterpret_cast<const uuid_t *>(BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk), sizeof(uuid_t));
            out = id;
            ByteOffset += sizeof(uuid_t);
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
          } else {
            uuid_t id;
            Read(&id, sizeof(uuid_t));
            out = id;
          }
        }

        void Read(void *buf, size_t len) {
          assert(ByteOffset + len <= EndOfStream);
          size_t left = len;
          char *ptr = reinterpret_cast<char *>(buf);
          while (left > 0) {
            //size_t do_now = std::min(CachePageSize - OffsetInPage, left);
            //memcpy(ptr, BufData + OffsetInPage, do_now);
            size_t do_now = std::min(DataChunkSize - OffsetInChunk, left);
            memcpy(ptr, BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk, do_now);
            ByteOffset += do_now;
            CheckBuf();
            OffsetInPage = ByteOffset % CachePageSize;
            ChunkInPage = OffsetInPage / DataChunkSize;
            OffsetInChunk = OffsetInPage % DataChunkSize;
            left -= do_now;
            ptr += do_now;
          }
        }

        inline void Skip(size_t num_bytes) {
          assert(ByteOffset + num_bytes <= EndOfStream);
          ByteOffset += num_bytes;
          CheckBuf();
          OffsetInPage = ByteOffset % CachePageSize;
          ChunkInPage = OffsetInPage / DataChunkSize;
          OffsetInChunk = OffsetInPage % DataChunkSize;
        }

        inline void GoTo(size_t offset) {
          assert(offset <= EndOfStream);
          assert(offset < EndOfStream);
          ByteOffset = offset;
          CheckBuf();
          OffsetInPage = ByteOffset % CachePageSize;
          ChunkInPage = OffsetInPage / DataChunkSize;
          OffsetInChunk = OffsetInPage % DataChunkSize;
        }

        inline operator bool() const {
          return ByteOffset < EndOfStream;
        }

        inline size_t GetOffset() const {
          return ByteOffset;
        }

        size_t GetLoadedPageId() const {
          return LoadedPageId;
        }

        Util::TPageCache::TSlot *GetLoadedMainSlot() const {
          assert(MainSlot);
          return MainSlot;
        }

        Util::TPageCache::TSlot *GetLoadedDataSlot() const {
          assert(DataSlot);
          return DataSlot;
        }

        size_t GetOffsetInChunk() const {
          return OffsetInChunk;
        }
        size_t GetChunkInPage() const {
          return ChunkInPage;
        }

        size_t GetFetchCount() const {
          return FetchCount;
        }

        inline size_t FindPageIdOfByte(size_t offset) const {
          return File->FindPageIdOfByte(offset);
        }

        const char *GetData() const {
          //return (BufData + OffsetInPage);
          return (BufData + (ChunkInPage * PhysicalDataChunkSize) + OffsetInChunk);
        }

        void ReleaseBuf() {
          ByteOffset = -1;
          if (MainSlot) {
            Cache->Release(MainSlot, LoadedPageId);
            MainSlot = nullptr;
            DataSlot = nullptr;
          }
        }

        private:

        void PrefetchNextPage(size_t offset) {
          ++NumSequentialFetch;
          size_t num_keep_prefetched = 0UL;
          if (NumSequentialFetch > 16) {
            /* 17 - 32 sequential reads; keep 32 ahead */
            num_keep_prefetched = 32UL;
          } else if (NumSequentialFetch > 4) {
            /* 5 - 16 sequential reads; keep 8 ahead */
            num_keep_prefetched = 8UL;
          } else if (NumSequentialFetch > 1) {
            /* 2 - 4 sequential reads; keep 4 ahead */
            num_keep_prefetched = 4UL;
          }
          if (num_keep_prefetched > 0) {
            const size_t starting_offset = std::max(PrefetchedTo, offset / CachePageSize);
            const size_t ending_offset = std::min((offset / CachePageSize) + num_keep_prefetched, (EndOfStream - 1) / CachePageSize);
            const size_t num_pages_to_prefetch = ending_offset - starting_offset;
            if (num_pages_to_prefetch >= num_keep_prefetched / 2) {
              PrefetchedTo = ending_offset;
              /* === We're using these variables to figure out how many async pages we can grab at once === */
              size_t consec_starting_page_id = -1;
              size_t consec_next_page_id = -1;
              size_t num_consec = 0UL;
              /* === We're using these variables to figure out how many async pages we can grab at once === */
              for (size_t i = starting_offset; i < ending_offset; ++i) {
                size_t cur_offset = i * CachePageSize;
                assert(cur_offset < EndOfStream);
                size_t page_id = FindPageIdOfByte(cur_offset);

                if (page_id == consec_next_page_id) {
                  /* this page logically follows the previous one... we could grab them together */
                  ++consec_next_page_id;
                  ++num_consec;
                } else {
                  if (num_consec > 0UL) {
                    Cache->AsyncMultiGet(CodeLocation, Priority, Cache, BufKind, UtilSrc, consec_starting_page_id, num_consec, true, AsyncTrigger);
                  }
                  /* this page does not follow logically, we'll have to do a separate request for this one */
                  num_consec = 0UL;
                  consec_starting_page_id = page_id;
                  consec_next_page_id = consec_starting_page_id + 1UL;
                }
              }
              if (num_consec > 0UL) {
                Cache->AsyncMultiGet(CodeLocation, Priority, Cache, BufKind, UtilSrc, consec_starting_page_id, num_consec, true, AsyncTrigger);
              }
            }
          }
        }

        inline void FetchBuf(size_t page_id, size_t offset) {
          const size_t prev_loaded_page_id = LoadedPageId;
          LoadedPageId = page_id;
          LoadedByteStart = (offset / CachePageSize) * CachePageSize;
          LoadedByteEnd = LoadedByteStart + CachePageSize;
          /* if this offset_block comes sequentially after the one we were on, then we record a sequential access. */
          if (ScanAheadAllowed) {
            if ((offset / CachePageSize) == (LastFetched + 1)) {
              PrefetchNextPage(offset);
            } else {
              NumSequentialFetch = 0UL;
              PrefetchedTo = offset / CachePageSize;
            }
          }
          if (MaxLocalCacheSize > 0) {
            const TSlotStruct *pos = LocalBufCache.TryGet(page_id);
            if (pos) {
              MainSlot = pos->MainSlot;
              DataSlot = pos->DataSlot;
              BufData = DataSlot->KnownGetData(Cache);
            } else {
              try {
                MainSlot = Cache->Get(page_id, DataSlot);
                BufData = DataSlot->SyncGetData(CodeLocation, Priority, Cache, BufKind, UtilSrc, page_id, SyncTrigger);
                LocalBufCache.Emplace(std::forward_as_tuple(page_id), std::forward_as_tuple(page_id, Cache, MainSlot, DataSlot));
              } catch (const Disk::TDiskFailure &err) {
                MainSlot = nullptr;
                DataSlot = nullptr;
                throw;
              } catch (const Disk::TDiskServiceShutdown &err) {
                MainSlot = nullptr;
                DataSlot = nullptr;
                throw;
              }
            }
          } else {
            if (MainSlot) {
              Cache->Release(MainSlot, prev_loaded_page_id);
            }
            try {
              MainSlot = Cache->Get(page_id, DataSlot);
              BufData = DataSlot->SyncGetData(CodeLocation, Priority, Cache, BufKind, UtilSrc, page_id, SyncTrigger);
            } catch (const Disk::TDiskFailure &err) {
              MainSlot = nullptr;
              DataSlot = nullptr;
              throw;
            } catch (const Disk::TDiskServiceShutdown &err) {
              MainSlot = nullptr;
              DataSlot = nullptr;
              throw;
            }
          }
          LastFetched = offset / CachePageSize;
        }

        inline void CheckBuf() {
          if ((ByteOffset >= LoadedByteEnd || ByteOffset < LoadedByteStart) && ByteOffset < EndOfStream) {
            size_t page_id_of_byte_index = FindPageIdOfByte(ByteOffset);
            FetchBuf(page_id_of_byte_index, ByteOffset);
            ++FetchCount;
          }
        }

        const TInFile *File;

        Util::TCache<PhysicalCachePageSize> *Cache;

        const size_t EndOfStream;

        size_t LoadedPageId;

        size_t LoadedByteStart;
        size_t LoadedByteEnd;

        size_t ByteOffset;

        size_t OffsetInPage;
        size_t OffsetInChunk;
        size_t ChunkInPage;

        typename Util::TCache<PhysicalCachePageSize>::TSlot *MainSlot;
        typename Util::TCache<PhysicalCachePageSize>::TSlot *DataSlot;
        const char *BufData;

        struct TSlotStruct {
          TSlotStruct(size_t page_id,
                      Util::TCache<PhysicalCachePageSize> *cache,
                      typename Util::TCache<PhysicalCachePageSize>::TSlot *main_slot,
                      typename Util::TCache<PhysicalCachePageSize>::TSlot *data_slot)
              : PageId(page_id),
                Cache(cache),
                MainSlot(main_slot),
                DataSlot(data_slot) {}
          ~TSlotStruct() {
            Cache->Release(MainSlot, PageId);
          }
          const size_t PageId;
          Util::TCache<PhysicalCachePageSize> *const Cache;
          typename Util::TCache<PhysicalCachePageSize>::TSlot *const MainSlot;
          typename Util::TCache<PhysicalCachePageSize>::TSlot *const DataSlot;
        };
        Base::TMiniCache<MaxLocalCacheSize, size_t, TSlotStruct> LocalBufCache;

        size_t FetchCount;

        size_t NumSequentialFetch;

        size_t PrefetchedTo;

        size_t LastFetched;

        TCompletionTrigger SyncTrigger;
        TCompletionTrigger AsyncTrigger;

        DiskPriority Priority;

        TDiskResult DiskResult;
        const char *DiskErrStr;

        const Base::TCodeLocation CodeLocation;
        const uint8_t UtilSrc;

      };  // TStream

    }  // Disk

  }  // Indy

}  // Orly