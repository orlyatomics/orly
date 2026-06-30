/* <orly/indy/disk/update_walk_file.h>

   Read-side walker that yields a data file's updates in sequence order
   from a starting sequence number. Used by master->slave replication
   and by the `TUpdateWalker` consumers in `orly/indy/repo.cc` to scan
   changes. The recorded `Mutator` byte is preserved on read-back (#53)
   so commutative ops remain commutative when replayed downstream.

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

#include <base/class_traits.h>
#include <orly/atom/kit2.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/update_walker.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      static constexpr size_t UpdateWalkerLocalCacheSize = 64;

      class TUpdateWalkFile
          : public TReadFile<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage>,
            public TUpdateWalker {
        NO_COPY(TUpdateWalkFile);
        public:

        using TArena = TDiskArena<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, DiskArenaMaxCacheSize, true>;

        typedef TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, UpdateWalkerLocalCacheSize> TInStream;
        typedef Orly::Indy::Disk::TReadFile<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage> TMyReadFile;

        TUpdateWalkFile(Util::TEngine *engine,
                        const Base::TUuid &file_id,
                        size_t gen_id,
                        TSequenceNumber from)
            : TMyReadFile(HERE, Source::UpdateWalk, engine, file_id, RealTime, gen_id),
              MainArena(this, engine->GetCache<PhysicalCachePageSize>(), RealTime),
              From(from),
              NumScanned(0U),
              IndexStream(HERE, Source::UpdateWalk, RealTime, this, engine->GetPageCache(), GetByteOffsetOfUpdateIndex()),
              BucketStream(HERE, Source::UpdateWalk, RealTime, this, engine->GetPageCache(), 0),
              EntryStream(HERE, Source::UpdateWalk, RealTime, this, engine->GetPageCache(), 0),
              Valid(true), Cached(false) {
          for (const auto &idx_pair : IndexById) {
            ArenaByIndexId.emplace(idx_pair.first, std::make_unique<TArena>(idx_pair.second.get(), engine->GetCache<PhysicalCachePageSize>(), RealTime));
          }
          Refresh();
        }

        virtual ~TUpdateWalkFile() {}

        /* True iff. we have an item. */
        virtual operator bool() const {
          Refresh();
          return Valid;
        }

        /* The current item. */
        virtual const TItem &operator*() const {
          Refresh();
          assert(Valid);
          return Item;
        }

        /* Walk to the next item, if any. */
        virtual TUpdateWalker &operator++() {
          Cached = false;
          Refresh();
          return *this;
        }

        private:

        void Refresh() const {
          if (Valid && !Cached) {
            if (NumScanned < GetNumUpdates()) {
              size_t offset_of_bucket;
              size_t num_key_ptr;
              IndexStream.Read(Item.SequenceNumber);
              for (; NumScanned < GetNumUpdates() && Item.SequenceNumber < From;) {
                IndexStream.Skip(TData::UpdateEntrySize - sizeof(Item.SequenceNumber));
                IndexStream.Read(Item.SequenceNumber);
                ++NumScanned;
              }
              if (NumScanned >= GetNumUpdates()) {
                Valid = false;
                return;
              }
              IndexStream.Read(&Item.Metadata, sizeof(Atom::TCore));
              IndexStream.Read(&Item.Id, sizeof(Atom::TCore));
              IndexStream.Read(offset_of_bucket);
              IndexStream.Read(num_key_ptr);
              BucketStream.GoTo(offset_of_bucket);
              Item.EntryVec.clear();
              Atom::TCore key;
              Atom::TCore val;
              Item.MainArena = &MainArena;
              for (size_t i = 0; i < num_key_ptr; ++i) {
                size_t offset_of_key;
                BucketStream.Read(offset_of_key);
                auto ret = IndexByOffset.upper_bound(offset_of_key);
                --ret; /* we're one past the file we're interested in */
                assert(ArenaByIndexId.find(ret->second->GetIndexId()) != ArenaByIndexId.end());
                Atom::TCore::TArena *const key_arena = ArenaByIndexId.find(ret->second->GetIndexId())->second.get();
                EntryStream.GoTo(offset_of_key + sizeof(TSequenceNumber));
                EntryStream.Read(&key, sizeof(Atom::TCore));
                EntryStream.Read(&val, sizeof(Atom::TCore));
                /* The bucket stores raw offsets without a per-entry type
                   flag, but the writer places current entries in the
                   range [ByteOffsetOfKeyIndex, ByteOffsetOfHistoryIndex)
                   and history entries at >= ByteOffsetOfHistoryIndex.
                   Use that boundary (per index) to pick the right
                   trailing-Mutator offset for the read:
                     TKeyItem (current):    Mutator follows
                                            NumHistKeys + OffsetOfHistKeys
                                            (2 * sizeof(size_t) bytes).
                     THistoryKeyItem:       Mutator follows Value directly.
                   Trailing 4 bytes after Mutator are alignment padding
                   (see KeyEntrySize / KeyHistorySize in data_file.h). */
                if (offset_of_key < ret->second->GetByteOffsetOfHistoryIndex()) {
                  EntryStream.Skip(sizeof(size_t) * 2U);
                }
                TMutator mutator = TMutator::Assign;
                EntryStream.Read(&mutator, sizeof(TMutator));
                Item.EntryVec.emplace_back(TIndexKey(ret->second->GetIndexId(), TKey(key, key_arena)), val, mutator);
              }
              Cached = true;
              ++NumScanned;
            } else {
              Valid = false;
            }
          }
        }

        mutable TArena MainArena;
        std::unordered_map<Base::TUuid, std::unique_ptr<TArena>> ArenaByIndexId;

        TSequenceNumber From;

        mutable size_t NumScanned;

        mutable TInStream IndexStream;

        mutable TInStream BucketStream;

        mutable TInStream EntryStream;

        mutable bool Valid;

        mutable bool Cached;

        mutable TItem Item;

      };  // TUpdateWalkFile

    }  // Disk

  }  // Indy

}  // Orly