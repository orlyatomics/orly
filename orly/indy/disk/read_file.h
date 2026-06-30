/* <orly/indy/disk/read_file.h>

   The reader half of the data-file format: parses meta, key, history,
   arena, hash, and update indexes off a `TInFile` cursor and exposes
   them via `ForEachIndex`, `FindInHash`, the typed `T*Stream`s, and
   the per-index `TDiskArena`. `TFileKey` is the `(file_id, gen_id)`
   identity used by callers like the page cache. Large header; the
   public template `TReadFile` starts past the helper declarations.

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

#include <algorithm>
#include <ostream>

#include <base/class_traits.h>
#include <base/inv_con/unordered_multimap.h>
#include <base/web/daemonize.h>
#include <orly/atom/kit2.h>
#include <orly/indy/disk/in_file.h>
#include <orly/indy/disk/indy_util_reporter.h>
#include <orly/indy/disk/util/cache.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/update.h>
#include <orly/sabot/match_prefix_state.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TFileKey {
        public:

        TFileKey(const Base::TUuid &file_id, size_t gen_id) : FileId(file_id), GenId(gen_id) {}

        TFileKey(Base::TUuid &&file_id, size_t gen_id) : FileId(std::move(file_id)), GenId(gen_id) {}

        Base::TUuid FileId;

        size_t GenId;

        inline size_t GetHash() const {
          return GenId ^ FileId.GetHash();
        }

        inline bool operator==(const TFileKey &that) const {
          return FileId == that.FileId && GenId == that.GenId;
        }

        inline bool operator!=(const TFileKey &that) const {
          return FileId != that.FileId || GenId != that.GenId;
        }

      };  // TFileKey

    }  // Disk

  }  // Indy

}  // Orly

namespace std {

  /* A standard hasher for Orly::Indy::Disk::TFileKey. */
  template <>
  struct hash<Orly::Indy::Disk::TFileKey> {
    typedef size_t result_type;
    typedef Orly::Indy::Disk::TFileKey argument_type;
    size_t operator()(const Orly::Indy::Disk::TFileKey &that) const {
      return that.GetHash();
    }
  };

}  // std

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TArenaInFile
          : public TInFile {
        NO_COPY(TArenaInFile);
        public:

        virtual size_t GetByteOffsetOfArena() const = 0;

        virtual size_t GetNumArenaNotes() const = 0;

        virtual Atom::TCore::TOffset GetNumBytesOfArena() const = 0;

        protected:

        TArenaInFile() {}

        virtual ~TArenaInFile() {}

      };

      static constexpr size_t DiskArenaMaxCacheSize = 128;

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, size_t StreamLocalCacheSize, bool ScanAheadAllowed>
      class TDiskArena
          : public Atom::TCore::TArena {
        NO_COPY(TDiskArena);
        private:

        public:

        static constexpr size_t DataChunkSize = Util::GetLogicalDataChunkSize<BufKind>();
        static constexpr size_t PhysicalDataChunkSize = Util::GetPhysicalDataChunkSize<BufKind>();
        static constexpr size_t PhysicalCachePageSize = PhysicalBlockSize / (BlockSize / CachePageSize);

        class TCursor {
          NO_COPY(TCursor);
          public:

          TCursor(TDiskArena *context, Atom::TCore::TOffset start_offset, Atom::TCore::TOffset end_offset)
              : Arena(context),
                Pin(nullptr),
                PinAlloc(nullptr),
                Offset(start_offset),
                EndOffset(end_offset) {
            //assert(end_offset <= context->GetNumBytesOfArena());
            PinAlloc = reinterpret_cast<Atom::TCore::TArena::TFinalPin *>(malloc(sizeof(Atom::TCore::TArena::TFinalPin)));
            if (PinAlloc == 0) {
              syslog(LOG_EMERG, "bad alloc in TDiskArena::TCursor");
              throw std::bad_alloc();
            }
            try {
              Refresh();
            } catch(...) {
              if (Pin) {
                Pin->TPin::~TPin();
              }
              free(PinAlloc);
              throw;
            }
          }

          ~TCursor() {
            if (Pin) {
              Pin->TPin::~TPin();
            }
            free(PinAlloc);
          }

          operator bool() const {
            return Pin != nullptr;
          }

          const Atom::TCore::TNote *operator*() const {
            assert(Pin);
            assert(Pin->GetNote());
            return Pin->GetNote();
          }

          const Atom::TCore::TNote *operator->() const {
            assert(Pin);
            assert(Pin->GetNote());
            return Pin->GetNote();
          }

          TCursor &operator++() {
            Refresh();
            return *this;
          }

          Atom::TCore::TOffset GetOffset() const {
            return Offset;
          }

          inline TDiskArena *GetArena() const {
            return Arena;
          }

          private:

          void Refresh() {
            if (Pin) {
              Offset += sizeof(Atom::TCore::TNote) + Pin->GetNote()->GetRawSize();
              Pin->TPin::~TPin();
            }
            if (Offset < EndOffset) {
              Pin = Arena->Pin(Offset, PinAlloc);
            } else {
              Pin = nullptr;
            }
          }

          TDiskArena *Arena;

          TDiskArena::TFinalPin *Pin;

          TDiskArena::TFinalPin *PinAlloc;

          Atom::TCore::TOffset Offset;

          Atom::TCore::TOffset EndOffset;

        };  // TCursor

        TDiskArena(TArenaInFile *file, Util::TCache<PhysicalCachePageSize> *cache, DiskPriority priority)
            : TArena(true),
              File(file),
              Priority(priority),
              Cache(cache),
              StartOffset(file->GetByteOffsetOfArena()),
              Stream(HERE, Source::DiskArena, priority, StartOffset + file->GetNumBytesOfArena(), file, cache, StartOffset),
              NumNotes(file->GetNumArenaNotes()) {
          assert(file);
        }

        virtual ~TDiskArena() {
        }

        inline virtual void ReleaseNote(const Atom::TCore::TNote *note, Atom::TCore::TOffset offset, void *data1, void *data2, void *data3);

        inline virtual const Atom::TCore::TNote *TryAcquireNote(Atom::TCore::TOffset offset, void *&data1, void *&data2, void *&data3);

        inline virtual const Atom::TCore::TNote *TryAcquireNote(Atom::TCore::TOffset offset, size_t known_size, void *&data1, void *&data2, void *&data3);

        Atom::TCore::TOffset GetNumBytesOfArena() const {
          return File->GetNumBytesOfArena();
        }

        private:

        TArenaInFile *File;

        DiskPriority Priority;

        Util::TCache<PhysicalCachePageSize> *Cache;

        TCompletionTrigger SyncTrigger;

        size_t StartOffset;

        TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, StreamLocalCacheSize, ScanAheadAllowed> Stream;

        size_t NumNotes;

      };  // TDiskArena

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, bool ScanAheadAllowed = true>
      class TReadFile
          : public TArenaInFile {
        NO_COPY(TReadFile);
        public:

        static constexpr size_t PagesInBlock = BlockSize / CachePageSize;
        static constexpr size_t PhysicalCachePageSize = PhysicalBlockSize / (BlockSize / CachePageSize);

        inline virtual size_t GetFileLength() const {
          return FileLength;
        }

        inline virtual size_t GetStartingBlock() const {
          return StartingBlockId;
        }

        virtual void ReadMeta(size_t , size_t &) const {
          throw std::logic_error("Deprecated");
        }

        inline virtual size_t FindPageIdOfByte(size_t offset) const {
          assert(offset < FileLength);
          size_t num_blocks_into_file = offset / BlockSize;
          if (num_blocks_into_file == StartingBlockOffset) {
            return (StartingBlockId * PagesInBlock) + (offset % BlockSize) / CachePageSize;
          } else {
            auto upper = OffsetBlockByBlockId.upper_bound(num_blocks_into_file);
            --upper;
            assert(upper != OffsetBlockByBlockId.end());
            return ((upper->second + (num_blocks_into_file - upper->first)) * PagesInBlock) + (offset % BlockSize) / CachePageSize;
          }
        }

        static __thread size_t HashHitCount;

        protected:

        TReadFile(const Base::TCodeLocation &code_location,
                  uint8_t util_src,
                  Util::TCache<PhysicalCachePageSize> *cache,
                  const Base::TUuid &/*file_id*/,
                  DiskPriority priority,
                  size_t gen_id,
                  size_t starting_block_id,
                  size_t starting_block_offset,
                  size_t file_length)
            : Cache(cache),
              StartingBlockId(starting_block_id),
              StartingBlockOffset(starting_block_offset),
              FileLength(file_length),
              Priority(priority),
              GenId(gen_id),
              CodeLocation(code_location),
              UtilSrc(util_src) {
          Init();
        }

        TReadFile(const Base::TCodeLocation &code_location,
                  uint8_t util_src,
                  Util::TEngine *engine,
                  const Base::TUuid &file_id,
                  DiskPriority priority,
                  size_t gen_id)
            : Cache(engine->GetCache<PhysicalCachePageSize>()),
              Priority(priority),
              GenId(gen_id),
              CodeLocation(code_location),
              UtilSrc(util_src) {
          size_t num_keys;
          if (!engine->FindFile(file_id, gen_id, StartingBlockId, StartingBlockOffset, FileLength, num_keys)) {
            std::ostringstream ss;
            ss << file_id;
            syslog(LOG_ERR, "TReadFile() Can not find file %s[%ld]", ss.str().c_str(), gen_id);
            throw std::runtime_error("Could not find file.");
          }
          Init();
        }

        private:

        void Init() {
          assert(StartingBlockOffset * BlockSize < FileLength);
          TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, MaxMetaCacheSize> in_stream(CodeLocation, UtilSrc, Priority, this, Cache, StartingBlockOffset * BlockSize);
          /*
            # of blocks
            # of meta-blocks (n)
            # of #block / block_id pairings (number of sequential blocks starting at) (m)
            # updates
            # index segments (p)
            # of arena notes
            # of arena bytes
            # of arena type boundaries
            offset of main arena
            offset of update index
          */
          in_stream.Read(NumBlocks);
          in_stream.Read(NumMetaBlocks);
          in_stream.Read(NumSequentialBlockPairings);
          in_stream.Read(NumUpdates);
          in_stream.Read(NumIndexSegments);
          in_stream.Read(NumMainArenaNotes);
          in_stream.Read(NumMainArenaBytes);
          in_stream.Read(NumMainArenaTypeBoundaries);
          in_stream.Read(ByteOffsetOfMainArena);
          in_stream.Read(ByteOffsetOfUpdateIndex);

          /* meta block ids */
          size_t at_offset_block = NumBlocks - NumMetaBlocks;
          for (size_t i = 0; i < NumMetaBlocks; ++i) {
            size_t block_id;
            in_stream.Read(block_id);
            OffsetBlockByBlockId.insert(std::make_pair(at_offset_block, block_id));
            ++at_offset_block;
          }
          at_offset_block = 0UL;
          /* starting block id pairings */
          for (size_t i = 0; i < NumSequentialBlockPairings; ++i) {
            size_t block_id, num_blocks;
            in_stream.Read(block_id);
            in_stream.Read(num_blocks);
            OffsetBlockByBlockId.insert(std::make_pair(at_offset_block, block_id));
            at_offset_block += num_blocks;
          }
          /* index segment offsets */
          Base::TUuid index_id;
          size_t offset;
          for (size_t i = 0; i < NumIndexSegments; ++i) {
            in_stream.Read(index_id);
            in_stream.Read(offset);
            //std::cout << "Index [" << index_id << "] @ [" << offset << "]" << std::endl;
            IndexOffsetById.emplace(index_id, offset);
            auto idx_ptr = std::make_unique<TIndexFile>(this, index_id, offset, Priority);
            IndexByOffset.insert(std::make_pair(offset, idx_ptr.get()));
            IndexById.emplace(index_id, std::move(idx_ptr));
          }
          for (size_t i = 0; i < NumMainArenaTypeBoundaries; ++i) {
            in_stream.Read(offset);
            MainArenaTypeBoundaryOffsetVec.emplace_back(offset);
          }
        }

        protected:

        virtual ~TReadFile() {}

        inline size_t GetNumBlocks() const {
          return NumBlocks;
        }

        inline size_t GetStartingBlockOffset() const {
          return StartingBlockOffset;
        }

        inline size_t GetNumMetaBlocks() const {
          return NumMetaBlocks;
        }

        inline size_t GetNumSequentialBlockPairings() const {
          return NumSequentialBlockPairings;
        }

        inline size_t GetByteOffsetOfArena() const {
          return ByteOffsetOfMainArena;
        }

        inline size_t GetNumArenaNotes() const {
          return NumMainArenaNotes;
        }

        inline size_t GetNumBytesOfArena() const {
          return NumMainArenaBytes;
        }

        inline const std::vector<size_t> &GetTypeBoundaryOffsetVec() const {
          return MainArenaTypeBoundaryOffsetVec;
        }

        inline size_t GetNumUpdates() const {
          return NumUpdates;
        }

        inline size_t GetGenId() const {
          return GenId;
        }

        inline size_t GetByteOffsetOfUpdateIndex() const {
          return ByteOffsetOfUpdateIndex;
        }

        void ForEachIndex(const std::function<void (const Base::TUuid &, size_t)> &cb) const {
          for (const auto &idx : IndexOffsetById) {
            cb(idx.first, idx.second);
          }
        }

        bool FindInHash(const Base::TUuid &index_id, const TKey &key, size_t &out_offset) const {
          auto ret = IndexById.find(index_id);
          if (ret != IndexById.end()) {
            return ret->second->FindInHash(key, out_offset);
          }
          return false;
        }

        public:

        class TIndexFile
            : public TArenaInFile {
          NO_COPY(TIndexFile);
          public:

          using TArena = TDiskArena<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, DiskArenaMaxCacheSize, ScanAheadAllowed>;

          using TInStream = TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, 0UL /* local cache size */>;

          /* On-disk current-key entry. Read/written verbatim via
             InStream.Read(&Item, sizeof(Item)) and the equivalent write
             path, so layout MUST stay in sync with TData::KeyEntrySize
             in in_file.h / TDataFile::KeyEntrySize in data_file.h
             (validated by the static_assert below). */
          class TKeyItem {
            public:

            TSequenceNumber SeqNum;
            Atom::TCore Key;
            Atom::TCore Value;
            size_t NumHistKeys;
            size_t OffsetOfHistKeys;
            /* Phase 1 of #49: default-Assign for every in-tree write
               path. 4 bytes; trailing alignment padding pushes the
               struct size up by 8 (see size constants in in_file.h). */
            TMutator Mutator;

          };  // TKeyItem

          /* On-disk history-key entry. See note on TKeyItem above. */
          class THistoryKeyItem {
            public:

            TSequenceNumber SeqNum;
            Atom::TCore Key;
            Atom::TCore Value;
            /* See TKeyItem::Mutator. */
            TMutator Mutator;

          };  // THistoryKeyItem

          class TKeyCursor {
            NO_COPY(TKeyCursor);
            public:

            TKeyCursor(TIndexFile *idx_file, size_t start_num = 0UL)
                : Cur(start_num),
                  Limit(idx_file->NumCurKeys),
                  InStream(idx_file->File->CodeLocation, idx_file->File->UtilSrc, idx_file->File->Priority, idx_file, idx_file->File->Cache, idx_file->ByteOffsetOfKeyIndex + (Cur * TData::KeyEntrySize)) {
              if (Cur < Limit) {
                InStream.Read(&Item, sizeof(Item));
                assert(Item.Key.IsTuple());
              }
            }

            /* True iff. we have an item. */
            operator bool() const {
              return Cur < Limit;
            }

            /* The current item. */
            const TKeyItem &operator*() const {
              assert(Cur < Limit);
              return Item;
            }

            /* Walk to the next item, if any. */
            TKeyCursor &operator++() {
              ++Cur;
              if (likely(Cur < Limit)) {
                InStream.Read(&Item, sizeof(Item));
                assert(Item.Key.IsTuple());
              }
              return *this;
            }

            private:

            TKeyItem Item;
            static_assert(sizeof(TKeyItem) == TData::KeyEntrySize, "TKeyItem is not the same size as a current key entry in a data file");

            size_t Cur;

            size_t Limit;

            TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, 0UL /* local cache size */> InStream;

          };  // TKeyCursor

          class THistoryKeyCursor {
            NO_COPY(THistoryKeyCursor);
            public:

            THistoryKeyCursor(TIndexFile *idx_file, size_t start_num = 0UL)
                : Cur(start_num),
                  Limit(idx_file->NumHistKeys),
                  InStream(idx_file->File->CodeLocation,
                           idx_file->File->UtilSrc,
                           idx_file->File->Priority,
                           idx_file,
                           idx_file->File->Cache,
                           idx_file->ByteOffsetOfKeyIndex + (idx_file->NumCurKeys * TData::KeyEntrySize) + (Cur * TData::KeyHistorySize)) {
              if (Cur < Limit) {
                InStream.Read(&Item, sizeof(Item));
                assert(Item.Key.IsTuple());
              }
            }

            /* True iff. we have an item. */
            operator bool() const {
              return Cur < Limit;
            }

            /* The current item. */
            const THistoryKeyItem &operator*() const {
              assert(Cur < Limit);
              return Item;
            }

            /* Walk to the next item, if any. */
            THistoryKeyCursor &operator++() {
              ++Cur;
              if (Cur < Limit) {
                InStream.Read(&Item, sizeof(Item));
                assert(Item.Key.IsTuple());
              }
              return *this;
            }

            private:

            THistoryKeyItem Item;
            static_assert(sizeof(THistoryKeyItem) == TData::KeyHistorySize, "THistoryKeyItem is not the same size as a history key entry in a data file");

            size_t Cur;

            size_t Limit;

            TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, 0UL /* local cache size */> InStream;

          };  // THistoryKeyCursor

          TIndexFile(const TReadFile *file, const Base::TUuid &index_id, DiskPriority priority)
              : TIndexFile(file, index_id, file->IndexOffsetById.find(index_id)->second , priority) {}

          TIndexFile(const TReadFile *file, const Base::TUuid &index_id, size_t index_meta_offset, DiskPriority /*priority*/)
              : File(file),
                IndexId(index_id) {
            TStream<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, 0UL /* local cache size */> in_stream(File->CodeLocation, File->UtilSrc, File->Priority, File, File->Cache, index_meta_offset);
            in_stream.Read(ArenaByteOffset);
            in_stream.Read(NumArenaNotes);
            in_stream.Read(NumArenaBytes);
            in_stream.Read(NumArenaTypeBoundaries);
            in_stream.Read(NumCurKeys);
            in_stream.Read(NumHistKeys);
            in_stream.Read(ByteOffsetOfKeyIndex);
            in_stream.Read(NumHashTables);
            assert(NumArenaBytes > 0UL);

            size_t offset, num_hash_fields;
            for (size_t i = 0; i < NumHashTables; ++i) {
              in_stream.Read(offset);
              in_stream.Read(num_hash_fields);
              //std::cout << "Hash table @ [" << offset << "] has [" << num_hash_fields <<"] entries" << std::endl;
              NumHashFieldsByOffset.emplace_back(offset, num_hash_fields);
            }
            for (size_t i = 0; i < NumArenaTypeBoundaries; ++i) {
              in_stream.Read(offset);
              ArenaTypeBoundaryByOffset.emplace_back(offset);
            }
          }

          bool FindInHash(const TKey &key, size_t &out_offset, TInStream &in_stream, TArena *file_arena) const {
            assert(key.GetCore().IsTuple());
            const Atom::TCore &core = key.GetCore();
            const Atom::TCore::TOffset *const off = core.TryGetOffset();
            assert(off);
            Atom::TCore::TArena *const arena = key.GetArena();
            void *pin_alloc = alloca(sizeof(Atom::TCore::TArena::TFinalPin));
            Atom::TCore::TArena::TFinalPin::TWrapper pin(arena->Pin(*off,
                                                                    sizeof(Atom::TCore::TNote) + (sizeof(Atom::TCore) * *core.TryGetElemCount()),
                                                                    pin_alloc));
            const size_t num_defined = pin->GetNote()->GetTupleNumNonFree();
            if (num_defined != 0) {
              const std::pair<size_t, size_t> &idx = NumHashFieldsByOffset[num_defined - 1];
              const size_t byte_offset_of_hash_table = idx.first;
              const size_t num_hash_fields = idx.second;
              const size_t hash_to_look_for = key.GetHash();
              const size_t modded_hash = hash_to_look_for % num_hash_fields;

              void *key_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
              void *other_state_alloc = reinterpret_cast<uint8_t *>(key_state_alloc) + Sabot::State::GetMaxStateSize();
              Sabot::State::TAny::TWrapper key_state(core.NewState(arena, key_state_alloc));

              in_stream.GoTo(byte_offset_of_hash_table + (modded_hash * TData::HashEntrySize));
              Atom::TCore cur_core;
              size_t cur_hash = 0;
              for (size_t i = modded_hash; i < num_hash_fields; ++i) {
                in_stream.Read(&cur_core, sizeof(Atom::TCore));
                in_stream.Read(out_offset);
                if (memcmp(&cur_core, &TData::NullCore, sizeof(Atom::TCore)) != 0) {
                  assert(cur_core.IsTuple());
                  cur_hash = cur_core.ForceGetStoredHash();
                  if (cur_hash == hash_to_look_for) {
                    Sabot::TMatchResult res;
                    if (IsPrefixMatch((res = MatchPrefixState(*key_state, *Sabot::State::TAny::TWrapper(cur_core.NewState(file_arena, other_state_alloc)))))) {
                      return true;
                    }
                  } else if (cur_hash % num_hash_fields > modded_hash) {
                    return false;
                  }
                } else {
                  return false;
                }
              }
              in_stream.GoTo(byte_offset_of_hash_table);
              for (size_t i = 0; i < modded_hash; ++i) {
                in_stream.Read(&cur_core, sizeof(Atom::TCore));
                in_stream.Read(out_offset);
                if (memcmp(&cur_core, &TData::NullCore, sizeof(Atom::TCore)) != 0) {
                  cur_hash = cur_core.ForceGetStoredHash();
                  if (cur_hash == hash_to_look_for) {
                    Sabot::TMatchResult res;
                    if (IsPrefixMatch((res = MatchPrefixState(*key_state, *Sabot::State::TAny::TWrapper(cur_core.NewState(file_arena, other_state_alloc)))))) {
                      return true;
                    }
                  } else if (cur_hash % num_hash_fields > modded_hash) {
                    return false;
                  }
                } else {
                  return false;
                }
              }
              syslog(LOG_ERR, "TReadFile::TIndexFile::FindInHash() Should not happen, implies hash table is completely full, modded_hash = [%ld], num_hash_fields[%ld]", modded_hash, num_hash_fields);
              in_stream.GoTo(byte_offset_of_hash_table);
              for (size_t i = 0; i < num_hash_fields; ++i) {
                in_stream.Read(&cur_core, sizeof(Atom::TCore));
                in_stream.Read(out_offset);
                if (memcmp(&cur_core, &TData::NullCore, sizeof(Atom::TCore)) != 0) {
                  std::cout << "[" << i << "] = [" << Indy::TKey(cur_core, file_arena) << "] @ [" << out_offset << "]" << std::endl;
                } else {
                  std::cout << "[" << i << "] = NULL" << std::endl;
                }
              }
              throw std::logic_error("TReadFile::TIndexFile::FindInHash() Should not happen, implies hash table is completely full");
            } else {
              out_offset = ByteOffsetOfKeyIndex;
              return NumCurKeys > 0;
            }
          }

          bool BinaryLowerBoundOnKey(const TKey &key, size_t &out_offset, TInStream &in_stream, TArena *file_arena) const {
            assert(NumCurKeys > 0);
            size_t first = 0U;
            size_t it = 0;
            size_t step;
            int64_t count = NumCurKeys;
            Atom::TCore core;
            while (count > 0) {
              it = first;
              step = count / 2;
              it += step;
              out_offset = ByteOffsetOfKeyIndex + (it * TData::KeyEntrySize);
              in_stream.GoTo(out_offset + sizeof(TSequenceNumber));
              in_stream.Read(&core, sizeof(Atom::TCore));
              if (TKey(core, file_arena) < key) {
                first = ++it;
                count -= step + 1;
              } else {
                count = step;
              }
            }
            out_offset = ByteOffsetOfKeyIndex + (it * TData::KeyEntrySize);
            return first < NumCurKeys;
          }

          Indy::TKey GetKey(size_t n, TInStream &in_stream, TArena *file_arena) const {
            assert(n < NumCurKeys);
            Atom::TCore core;
            in_stream.GoTo(ByteOffsetOfKeyIndex + (n * TData::KeyEntrySize) + sizeof(TSequenceNumber));
            in_stream.Read(&core, sizeof(core));
            return Indy::TKey(core, file_arena);
          }

          inline const Base::TUuid &GetIndexId() const {
            return IndexId;
          }

          inline virtual Atom::TCore::TOffset GetNumBytesOfArena() const override {
            return NumArenaBytes;
          }

          inline const std::vector<size_t> &GetTypeBoundaryOffsetVec() const {
            return ArenaTypeBoundaryByOffset;
          }

          inline size_t GetNumCurKeys() const {
            return NumCurKeys;
          }

          inline size_t GetNumHistKeys() const {
            return NumHistKeys;
          }

          inline size_t GetGenId() const {
            return File->GenId;
          }

          inline size_t GetByteOffsetOfKeyIndex() const {
            return ByteOffsetOfKeyIndex;
          }

          inline size_t GetByteOffsetOfHistoryIndex() const {
            return ByteOffsetOfKeyIndex + (NumCurKeys * TData::KeyEntrySize);
          }

          inline const std::vector<std::pair<size_t, size_t>> &GetNumHashFieldsByOffset() const {
            return NumHashFieldsByOffset;
          }

          private:

          inline virtual size_t GetByteOffsetOfArena() const override {
            return ArenaByteOffset;
          }

          inline virtual size_t GetNumArenaNotes() const override {
            return NumArenaNotes;
          }

          inline virtual size_t GetFileLength() const override {
            assert(File);
            return File->GetFileLength();
          }

          inline virtual size_t GetStartingBlock() const override {
            assert(File);
            return File->GetStartingBlock();
          }

          inline virtual void ReadMeta(size_t offset, size_t &out) const override {
            assert(File);
            return File->ReadMeta(offset, out);
          }

          inline virtual size_t FindPageIdOfByte(size_t offset) const override {
            assert(File);
            return File->FindPageIdOfByte(offset);
          }

          const TReadFile *File;

          Base::TUuid IndexId;

          size_t ArenaByteOffset;
          size_t NumArenaNotes;
          size_t NumArenaBytes;
          size_t NumArenaTypeBoundaries;
          size_t NumCurKeys;
          size_t NumHistKeys;
          size_t ByteOffsetOfKeyIndex;
          size_t NumHashTables;

          std::vector<std::pair<size_t, size_t>> NumHashFieldsByOffset;

          std::vector<size_t> ArenaTypeBoundaryByOffset;

        };  // TIndexFile

        inline const std::unordered_map<Base::TUuid, std::unique_ptr<TIndexFile>> &GetIndexByIdMap() {
          return IndexById;
        }

        protected:

        //Util::TPageCache *PageCache;
        Util::TCache<PhysicalCachePageSize> *Cache;

        size_t StartingBlockId;

        size_t StartingBlockOffset;

        size_t FileLength;

        size_t NumBlocks;
        size_t NumMetaBlocks;
        size_t NumSequentialBlockPairings;
        size_t NumUpdates;
        size_t NumIndexSegments;
        size_t NumMainArenaNotes;
        size_t NumMainArenaBytes;
        size_t NumMainArenaTypeBoundaries;
        size_t ByteOffsetOfMainArena;
        size_t ByteOffsetOfUpdateIndex;

        static constexpr size_t MaxMetaCacheSize = 64;

        std::map<size_t, size_t> OffsetBlockByBlockId;

        std::unordered_map<Base::TUuid, size_t> IndexOffsetById;

        std::unordered_map<Base::TUuid, std::unique_ptr<TIndexFile>> IndexById;

        std::map<size_t, TIndexFile *> IndexByOffset;

        std::vector<size_t> MainArenaTypeBoundaryOffsetVec;

        DiskPriority Priority;

        size_t GenId;

        const Base::TCodeLocation CodeLocation;
        const uint8_t UtilSrc;

      };  // TReadFile

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, bool ScanAheadAllowed = true>
      class TLocalReadFileCache {
        NO_COPY(TLocalReadFileCache);
        public:

        class TLocalReadFile
            : public TReadFile<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, ScanAheadAllowed> {
          NO_COPY(TLocalReadFile);
          public:

          TLocalReadFile(Util::TEngine *engine,
                         const Base::TUuid &file_id,
                         size_t gen_id)
              : TReadFile<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, ScanAheadAllowed>(HERE,
                                                                                                  Source::PresentWalk,
                                                                                                  engine,
                                                                                                  file_id,
                                                                                                  RealTime,
                                                                                                  gen_id) {}

          virtual ~TLocalReadFile() {}


        };  // TLocalReadFile

        TLocalReadFileCache() : LoaderCollection(this) {}

        TLocalReadFile *Get(Util::TEngine *engine,
                            const Base::TUuid &file_id,
                            size_t gen_id) {
          TLoaderObj *loader = LoaderCollection.TryGetFirstMember(TFileKey(file_id, gen_id));
          if (!loader) {
            loader = new TLoaderObj(this, engine, file_id, gen_id);
          }
          return loader->GetFile();
        }

        void Clear(const Base::TUuid &file_id, size_t gen_id) {
          TLoaderObj *loader = LoaderCollection.TryGetFirstMember(TFileKey(file_id, gen_id));
          delete loader;
        }

        static __thread TLocalReadFileCache *Cache;

        private:

        class TLoaderObj {
          NO_COPY(TLoaderObj);
          public:

          typedef InvCon::UnorderedMultimap::TMembership<TLoaderObj, TLocalReadFileCache, TFileKey> TCacheMembership;

          TLoaderObj(TLocalReadFileCache *cache, Util::TEngine *engine, const Base::TUuid &file_id, size_t gen_id) : CacheMembership(this, TFileKey(file_id, gen_id), &cache->LoaderCollection) {
            File = std::make_unique<TLocalReadFile>(engine, file_id, gen_id);
            /* if frames have joined the waiting queue, schedule them */
            for (auto frame : FrameWaitingVec) {
              Fiber::TRunner::LocalRunner->Schedule(frame);
            }
            FrameWaitingVec.clear();
          }

          TLocalReadFile *GetFile() {
            if (!File) {
              FrameWaitingVec.emplace_back(Fiber::TFrame::LocalFrame);
              Fiber::Wait();
            }
            return File.get();
          }

          private:

          std::unique_ptr<TLocalReadFile> File;

          std::vector<Fiber::TFrame *> FrameWaitingVec;

          typename TCacheMembership::TImpl CacheMembership;

        };  // TLoaderObj

        typedef InvCon::UnorderedMultimap::TCollection<TLocalReadFileCache, TLoaderObj, TFileKey> TLoaderCollection;

        mutable typename TLoaderCollection::TImpl LoaderCollection;

      };  // TLocalReadFileCache


      /*** Inline ***/

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, size_t LocalCacheSize, bool ScanAheadAllowed>
      inline void TDiskArena<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, LocalCacheSize, ScanAheadAllowed>::ReleaseNote(const Atom::TCore::TNote *note, Atom::TCore::TOffset offset, void *data1, void */*data2*/, void *data3) {
        if (data1) { /* This data fit in the block, release the block. */
          Cache->Release(reinterpret_cast<typename Util::TCache<PhysicalCachePageSize>::TSlot *>(data1), reinterpret_cast<size_t>(data3));
          //File->GetService()->ReleaseBuf(reinterpret_cast<TPageCache::TObj *>(data));
        } else { /* the data did not fit in the block, free the buffer we allocated. */
          assert(offset / DataChunkSize != (offset + note->GetRawSize() + sizeof(Atom::TCore::TNote)) / DataChunkSize);
          free(const_cast<Atom::TCore::TNote *>(note));
        }
      }

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, size_t LocalCacheSize, bool ScanAheadAllowed>
      inline const Atom::TCore::TNote *TDiskArena<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, LocalCacheSize, ScanAheadAllowed>::TryAcquireNote(Atom::TCore::TOffset offset, void *&data1, void *&data2, void *&data3) {
        const size_t note_offset = StartOffset + offset;
        assert(note_offset < File->GetFileLength());
        Stream.GoTo(note_offset);
        size_t loaded_page_id = Stream.GetLoadedPageId();
        size_t note_size;
        //if (Stream.GetOffsetInPage() <= (CachePageSize - sizeof(Atom::TCore::TNote))) {
        if (Stream.GetOffsetInChunk() <= (DataChunkSize - sizeof(Atom::TCore::TNote))) {
          note_size = sizeof(Atom::TCore::TNote) + reinterpret_cast<const Atom::TCore::TNote *>(Stream.GetData())->GetRawSize();
        } else {
          Atom::TCore::TNote *temp_note = reinterpret_cast<Atom::TCore::TNote *>(alloca(sizeof(Atom::TCore::TNote)));
          Stream.Read(temp_note, sizeof(Atom::TCore::TNote));
          note_size = sizeof(Atom::TCore::TNote) + temp_note->GetRawSize();
        }

        /* try to just point at the memory in the cache block if the note is contiguous. */
        if (note_offset / DataChunkSize == (note_offset + note_size) / DataChunkSize) {
          try {
            typename Util::TCache<PhysicalCachePageSize>::TSlot *data_slot;
            typename Util::TCache<PhysicalCachePageSize>::TSlot *const main_slot = Cache->Get(loaded_page_id, data_slot);
            data1 = main_slot;
            data2 = data_slot;
            data3 = reinterpret_cast<void *>(loaded_page_id);
            data_slot->SyncGetData(HERE, Priority, Cache, BufKind, Source::DiskArena, loaded_page_id, SyncTrigger);
            SyncTrigger.Wait();
          } catch (const Disk::TDiskFailure &err) {
            data1 = nullptr;
            throw;
          } catch (const Disk::TDiskServiceShutdown &err) {
            data1 = nullptr;
            throw;
          }
          assert(note_offset / DataChunkSize == (note_offset + note_size) / DataChunkSize);

          Atom::TCore::TNote *ret = reinterpret_cast<Atom::TCore::TNote *>(const_cast<char *>(reinterpret_cast<typename Util::TCache<PhysicalCachePageSize>::TSlot *>(data2)->KnownGetData(Cache)) + (((note_offset % CachePageSize) / DataChunkSize) * PhysicalDataChunkSize) + (note_offset % DataChunkSize));
          return ret;
        } else {
          data1 = nullptr;
          Atom::TCore::TNote *note_ptr = nullptr;
          if ((note_ptr = reinterpret_cast<Atom::TCore::TNote *>(malloc(note_size))) == 0) {
            syslog(LOG_EMERG, "bad alloc in TDiskArena::TryAcquireNote [%ld]", note_size);
            throw std::bad_alloc();
          }
          try {
            assert(note_offset + note_size <= File->GetFileLength());
            Stream.GoTo(note_offset);
            Stream.Read(note_ptr, note_size);
          } catch (...) {
            free(note_ptr);
            note_ptr = nullptr;
          }
          assert(note_offset / DataChunkSize != (note_offset + note_size) / DataChunkSize);
          return note_ptr;
        }
        throw;
      }

      template <size_t CachePageSize, size_t BlockSize, size_t PhysicalBlockSize, Util::TBufKind BufKind, size_t LocalCacheSize, bool ScanAheadAllowed>
      inline const Atom::TCore::TNote *TDiskArena<CachePageSize, BlockSize, PhysicalBlockSize, BufKind, LocalCacheSize, ScanAheadAllowed>::TryAcquireNote(Atom::TCore::TOffset offset, const size_t note_size, void *&data1, void *&data2, void *&data3) {
        const size_t note_offset = StartOffset + offset;
        Stream.GoTo(note_offset);
        #ifndef NDEBUG
        size_t stream_note_size;
        if (Stream.GetOffsetInChunk() <= (DataChunkSize - sizeof(Atom::TCore::TNote))) {
          stream_note_size = sizeof(Atom::TCore::TNote) + reinterpret_cast<const Atom::TCore::TNote *>(Stream.GetData())->GetRawSize();
        } else {
          Atom::TCore::TNote *temp_note = reinterpret_cast<Atom::TCore::TNote *>(alloca(sizeof(Atom::TCore::TNote)));
          Stream.Read(temp_note, sizeof(Atom::TCore::TNote));
          stream_note_size = sizeof(Atom::TCore::TNote) + temp_note->GetRawSize();
          Stream.GoTo(note_offset);
        }
        assert(note_size <= stream_note_size); /* this means the hinted note size is larger than the one stored on disk. A smaller size is considered ok as this might be a prefix tuple. */
        #endif
        /* try to just point at the memory in the cache block if the note is contiguous. */
        if (note_offset / DataChunkSize == (note_offset + note_size) / DataChunkSize) {
          try {
            const size_t loaded_page_id = Stream.GetLoadedPageId();
            typename Util::TCache<PhysicalCachePageSize>::TSlot *data_slot;
            typename Util::TCache<PhysicalCachePageSize>::TSlot *const main_slot = Cache->Get(loaded_page_id, data_slot);
            data1 = main_slot;
            data2 = data_slot;
            data3 = reinterpret_cast<void *>(loaded_page_id);
          } catch (const Disk::TDiskFailure &err) {
            data1 = nullptr;
            throw;
          } catch (const Disk::TDiskServiceShutdown &err) {
            data1 = nullptr;
            throw;
          }
          assert(note_offset / DataChunkSize == (note_offset + note_size) / DataChunkSize);
          Atom::TCore::TNote *ret = reinterpret_cast<Atom::TCore::TNote *>(const_cast<char *>(reinterpret_cast<typename Util::TCache<PhysicalCachePageSize>::TSlot *>(data2)->KnownGetData(Cache)) + (((note_offset % CachePageSize) / DataChunkSize) * PhysicalDataChunkSize) + (note_offset % DataChunkSize));
          return ret;
        } else {
          data1 = nullptr;
          Atom::TCore::TNote *note_ptr = nullptr;
          if ((note_ptr = reinterpret_cast<Atom::TCore::TNote *>(malloc(note_size))) == 0) {
            syslog(LOG_EMERG, "bad alloc in TDiskArena::TryAcquireNote [%ld]", note_size);
            throw std::bad_alloc();
          }
          try {
            assert(note_offset + note_size <= File->GetFileLength());
            Stream.GoTo(note_offset);
            Stream.Read(note_ptr, note_size);
          } catch (...) {
            free(note_ptr);
            note_ptr = nullptr;
          }
          assert(note_offset / DataChunkSize != (note_offset + note_size) / DataChunkSize);
          return note_ptr;
        }
        throw;
      }

    }  // Disk

  }  // Indy

}  // Orly
