/* <orly/indy/disk/present_walk_file.h>

   Read-side walker for the "present" (current) state of a data file:
   yields each (key, value) at its latest sequence number, skipping
   superseded history entries. `TWalkerKey` identifies one walk's
   position by `(file_id, gen_id, index_id)` for use as a cache or
   registry key. Consumed by the `TPresentWalker` paths in
   `orly/indy/repo.cc`.

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
#include <cassert>

#include <base/class_traits.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/tombstone_counts.h>
#include <orly/indy/present_walker.h>
#include <orly/sabot/all.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TWalkerKey {
        public:

        TWalkerKey(const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id) : FileId(file_id), GenId(gen_id), IndexId(index_id) {}

        TWalkerKey(Base::TUuid &&file_id, size_t gen_id, Base::TUuid &&index_id) : FileId(std::move(file_id)), GenId(gen_id), IndexId(std::move(index_id)) {}

        Base::TUuid FileId;

        size_t GenId;

        Base::TUuid IndexId;

        inline size_t GetHash() const {
          return GenId ^ FileId.GetHash()^ IndexId.GetHash();
        }

        inline bool operator==(const TWalkerKey &that) const {
          return FileId == that.FileId && GenId == that.GenId && IndexId == that.IndexId;
        }

        inline bool operator!=(const TWalkerKey &that) const {
          return FileId != that.FileId || GenId != that.GenId || IndexId != that.IndexId;
        }

      };  // TWalkerKey

    }  // Disk

  }  // Indy

}  // Orly

namespace std {

  /* A standard hasher for Orly::Indy::Disk::TWalkerKey. */
  template <>
  struct hash<Orly::Indy::Disk::TWalkerKey> {
    typedef size_t result_type;
    typedef Orly::Indy::Disk::TWalkerKey argument_type;
    size_t operator()(const Orly::Indy::Disk::TWalkerKey &that) const {
      return that.GetHash();
    }
  };

}  // std

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TLocalWalkerCache {
        NO_COPY(TLocalWalkerCache);
        public:

        TLocalWalkerCache() : LoaderCollection(this) {}

        /* Forward Declaration */
        class TLoaderObj;

        class TPresentWalkFile
            : public TPresentWalker {
          NO_COPY(TPresentWalkFile);
          public:

          static constexpr size_t PhysicalCachePageSize = Util::PhysicalBlockSize / (Util::LogicalBlockSize / Util::LogicalPageSize);

          using TArena = TDiskArena<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, DiskArenaMaxCacheSize, true>;

          using TInStream = TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, 0UL>;
          using TMyReadFile = Orly::Indy::Disk::TReadFile<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage>;

          TPresentWalkFile(Util::TEngine *engine,
                           const Base::TUuid &file_id,
                           size_t gen_id,
                           const Base::TUuid &index_id,
                           TLoaderObj *loader_obj)
              : Orly::Indy::TPresentWalker(Match),
                IndexId(index_id),
                MyReadFile(TLocalReadFileCache<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage>::Cache->Get(engine, file_id, gen_id)),
                IndexFile(nullptr),
                MainArena(MyReadFile, engine->GetCache<PhysicalCachePageSize>(), RealTime),
                Stream(HERE, Source::PresentWalk, RealTime, MyReadFile, engine->GetPageCache(), 0),
                IndexStream(HERE, Source::PresentWalk, RealTime, MyReadFile, engine->GetPageCache(), 0),
                Valid(true), Cached(false), LoaderObj(loader_obj), NextWalker(nullptr) {
            auto pos = MyReadFile->GetIndexByIdMap().find(IndexId);
            if (pos != MyReadFile->GetIndexByIdMap().end()) {
              IndexFile = pos->second.get();
            }
            /* The unique ptr to index_file is a temporary solution. */
            if (IndexFile) {
              IndexArena = std::make_unique<TArena>(IndexFile, engine->GetCache<PhysicalCachePageSize>(), RealTime);
              Item.KeyArena = IndexArena.get();
              Item.OpArena = &MainArena;
            }
          }

          struct TDeleter {

            void operator ()(TPresentWalkFile *p) const {
              p->LoaderObj->Free(p);
            };

          };  // TDeleter

          /* Look for this specific key. If it's free we'll return the range. This will never fall back to a binary search. If we don't have the prefix
             hash for your key, then there's no way we'll find it in a binary search. */
          void Init(const TKey &from) {
            SearchKind = Match;
            ResetHistory();
            EndRank = NoRank;
            if (IndexFile) {
              From = from;
              Cached = false;
              if ((Valid = IndexFile->FindInHash(From, Offset, IndexStream, IndexArena.get()))) {
                Stream.GoTo(Offset);
              }
              if (Valid) {
                Refresh();
              }
            } else {
              Valid = false;
              Cached = true;
            }
          }

          /* Look for your key range. This can fall back to a binary search. `from` is a fully defined key; `to` is either one too or a pattern
             whose free members are its rightmost ones, in which case the range runs to the end of the pattern's matches (#735). In case
             where the start of the range exists as a key in this particular file, we'll be able to hash to it and iterate from there. If it does not
             exist, we'll find the first key larger than the given start key using a binary search and iterate from there. */
          void Init(const TKey &from, const TKey &to) {
            SearchKind = Range;
            ResetHistory();
            EndRank = NoRank;
            /* Walkers are pooled (TLoaderObj), so clear what a previous walk left behind: when neither search below finds a
               start, the walk must come up empty rather than resume wherever the last one stopped (#735). */
            Valid = false;
            if (IndexFile) {
              From = from;
              To = to;
              Cached = false;
              size_t offset = 0U;
              bool success = IndexFile->FindInHash(From, Offset, IndexStream, IndexArena.get());
              if (success) {
                Valid = true;
                Stream.GoTo(Offset);
              } else if (From != To && IndexFile->BinaryLowerBoundOnKey(From, offset, IndexStream, IndexArena.get())) {
                Stream.GoTo(offset);
                Valid = true;
              }
              if (Valid) {
                Refresh();
              }
            } else {
              Valid = false;
              Cached = true;
            }
          }

          virtual ~TPresentWalkFile() {}

          /* True iff. we have an item. */
          inline virtual operator bool() const {
            //Refresh();
            return Valid;
          }

          /* The current item. */
          inline virtual const TItem &operator*() const {
            //Refresh();
            assert(Cached);
            assert(Valid);
            //std::cout << "Returning [" << Indy::TKey(Item.Key, Item.KeyArena) << "] = [" << Indy::TKey(Item.Op, Item.OpArena) << "]" << std::endl;
            return Item;
          }

          /* Walk to the next item, if any. */
          inline virtual TPresentWalker &operator++() {
            Cached = false;
            Refresh();
            return *this;
          }

          /* What follows lets a count pass over a stretch of this file without reading each key (#749). A key's rank is its
             place in the file's sorted array of current keys for the index. The walk's current key is always a current entry
             when these are called, never a replayed history entry. */

          /* The current keys the file holds for the index. */
          size_t GetNumRanks() const {
            return IndexFile ? IndexFile->GetNumCurKeys() : 0UL;
          }

          /* Move to the next current key, passing over the rest of this key's entries (its history). */
          void NextKey() {
            assert(Valid);
            ResetHistory();
            Cached = false;
            Refresh();
          }

          /* Pass over the current key and every later one in the walk's range that orders before `bound` (every later one
             when `bound` is null), and return how many keys that was and how many of them were tombstones. `bound` must order
             after the current key. The walk then stands on the first key not passed over, if any. */
          std::pair<size_t, size_t> SkipBelow(const TItem *bound, const TTombstoneCounts &counts) {
            assert(Valid);
            assert(counts.GetNumKeys() == GetNumRanks());
            const size_t rank = GetRank();
            const size_t end = GetEndRank(rank);
            const size_t target = bound ? LowerBound(*bound, rank + 1UL, end) : end;
            const size_t tombstones = counts.Count(rank, target, [this](size_t begin, size_t limit) { return CountTombstones(begin, limit); });
            ResetHistory();
            if (target < end) {
              Stream.GoTo(IndexFile->GetByteOffsetOfKeyIndex() + (target * TData::KeyEntrySize));
              Cached = false;
              Refresh();
            } else {
              Valid = false;
            }
            return {target - rank, tombstones};
          }

          private:

          /* Where a key stands against the walk's range: before it (a Match walk passes over such keys), in it, or past it. */
          enum class TPlace { Before, In, Past };

          /* Marks EndRank as not found yet. */
          static constexpr size_t NoRank = static_cast<size_t>(-1);

          TPlace Place(const Atom::TCore &key) const {
            switch (SearchKind) {
              case Match: {
                Sabot::TMatchResult res = From.GetCore().PrefixMatch(From.GetArena(), key, IndexArena.get());
                #ifndef NDEBUG
                void *key_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
                void *search_state_alloc = reinterpret_cast<uint8_t *>(key_state_alloc) + Sabot::State::GetMaxStateSize();
                Sabot::State::TAny::TWrapper key_state(From.GetCore().NewState(From.GetArena(), key_state_alloc));
                Sabot::State::TAny::TWrapper cur_state(key.NewState(IndexArena.get(), search_state_alloc));
                assert(res == MatchPrefixState(*key_state, *cur_state));
                #endif
                switch (res) {
                  case Sabot::TMatchResult::Unifies: {
                    /* we found a match. */
                    return TPlace::In;
                  }
                  case Sabot::TMatchResult::NoMatch: {
                    /* we're past our specific value, or in the case of a free the possible range of entries that we can unify with */
                    return TPlace::Past;
                  }
                  case Sabot::TMatchResult::PrefixMatch: {
                    /* keep iterating till we find a unify. */
                    return (TKey(From.GetCore(), From.GetArena()) < TKey(key, IndexArena.get())) ? TPlace::Past : TPlace::Before;
                  }
                }
                break;
              }
              case Range: {
                /* A key unifying with To is in range (To may be a pattern, #735); otherwise it is past the range once it orders
                   after To. */
                return (!UnifiesWithTo(key) && TKey(key, IndexArena.get()) > To) ? TPlace::Past : TPlace::In;
              }
            }
            assert(false);
            return TPlace::Past;
          }

          /* The rank of the current key. */
          size_t GetRank() const {
            const size_t offset = Stream.GetOffset() - IndexFile->GetByteOffsetOfKeyIndex();
            assert(offset >= TData::KeyEntrySize && offset % TData::KeyEntrySize == 0UL);
            return (offset / TData::KeyEntrySize) - 1UL;
          }

          /* The key of the given rank. */
          Atom::TCore GetKeyAt(size_t rank) const {
            assert(rank < GetNumRanks());
            Atom::TCore core;
            IndexStream.GoTo(IndexFile->GetByteOffsetOfKeyIndex() + (rank * TData::KeyEntrySize) + sizeof(TSequenceNumber));
            IndexStream.Read(&core, sizeof(core));
            return core;
          }

          /* The rank of the first key past the walk's range, given the current key's. No key in the range comes after one past
             it: a count ranks only a pattern of defined members followed by free ones, whose matches are one run of the index. */
          size_t GetEndRank(size_t rank) const {
            if (EndRank == NoRank) {
              size_t lo = rank + 1UL, hi = GetNumRanks();
              while (lo < hi) {
                const size_t mid = lo + ((hi - lo) / 2UL);
                if (Place(GetKeyAt(mid)) == TPlace::Past) {
                  hi = mid;
                } else {
                  lo = mid + 1UL;
                }
              }
              EndRank = lo;
            }
            assert(EndRank > rank);
            return EndRank;
          }

          /* The first rank in [lo, hi) whose key does not order before `bound`, or hi. */
          size_t LowerBound(const TItem &bound, size_t lo, size_t hi) const {
            TItem probe;
            probe.KeyArena = IndexArena.get();
            while (lo < hi) {
              const size_t mid = lo + ((hi - lo) / 2UL);
              probe.Key = GetKeyAt(mid);
              if (Atom::IsLt(probe.CompareKeys(bound))) {
                lo = mid + 1UL;
              } else {
                hi = mid;
              }
            }
            return lo;
          }

          /* The tombstones among the current keys of rank [begin, limit), by reading them. */
          size_t CountTombstones(size_t begin, size_t limit) const {
            typename TMyReadFile::TIndexFile::TKeyItem entry;
            static_assert(sizeof(entry) == TData::KeyEntrySize, "a current key entry is read whole");
            size_t count = 0UL;
            IndexStream.GoTo(IndexFile->GetByteOffsetOfKeyIndex() + (begin * TData::KeyEntrySize));
            for (size_t rank = begin; rank < limit; ++rank) {
              IndexStream.Read(&entry, sizeof(entry));
              count += entry.Value.IsTombstone() ? 1UL : 0UL;
            }
            return count;
          }

          void Refresh() const {
            assert (!Cached);
            assert(Valid);
            /* #227 / #49: replay the current key's history increments (set up
               on the previous current-key yield) before advancing to the next
               current key, so the fold sees every commutative increment. */
            if (HistRemaining && HistCursor && *HistCursor) {
              const auto &h = **HistCursor;
              Item.SequenceNumber = h.SeqNum;
              Item.Key = h.Key;
              Item.Op = h.Value;
              Item.Mutator = h.Mutator;
              /* Item.KeyArena / Item.OpArena were set in the ctor and are the
                 same arenas the history cores live in. UpdateId stays zero
                 (disk entries do not participate in cross-repo dedup -- the
                 child-POV release filter in TRepo::StepMergeMem keeps released
                 duplicates off disk; see #227). */
              ++(*HistCursor);
              --HistRemaining;
              Cached = true;
              return;
            }
            HistRemaining = 0UL;
            HistCursor.reset();
            for (;Stream.GetOffset() < IndexFile->GetByteOffsetOfHistoryIndex();) {
              assert(Valid);
              Stream.Read(&Item.SequenceNumber, sizeof(TSequenceNumber) + sizeof(Atom::TCore) * 2);
              /* Read NumHistKeys + OffsetOfHistKeys (2 size_t) -- needed to
                 replay this key's history below -- then read TMutator + skip
                 its 4 bytes of trailing alignment padding (struct ends at
                 sizeof(uint64_t) past the 2 size_t). The mutator goes onto
                 Item.Mutator so context.cc's fold can see it. */
              size_t num_hist_keys = 0UL, offset_of_hist_keys = 0UL;
              Stream.Read(&num_hist_keys, sizeof(size_t));
              Stream.Read(&offset_of_hist_keys, sizeof(size_t));
              Stream.Read(&Item.Mutator, sizeof(TMutator));
              Stream.Skip(sizeof(uint64_t) - sizeof(TMutator));
              Cached = true;
              switch (Place(Item.Key)) {
                case TPlace::In: {
                  ArmHistory(num_hist_keys, offset_of_hist_keys);
                  return;
                }
                case TPlace::Past: {
                  Valid = false;
                  return;
                }
                case TPlace::Before: {
                  break;
                }
              }
            }
            Valid = false;
          }

          /* True iff the key (in the index arena) unifies with To. */
          bool UnifiesWithTo(const Atom::TCore &key) const {
            void *to_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
            void *cur_state_alloc = static_cast<uint8_t *>(to_state_alloc) + Sabot::State::GetMaxStateSize();
            return MatchPrefixState(
                *Sabot::State::TAny::TWrapper(To.GetCore().NewState(To.GetArena(), to_state_alloc)),
                *Sabot::State::TAny::TWrapper(key.NewState(IndexArena.get(), cur_state_alloc))) == Sabot::TMatchResult::Unifies;
          }

          /* Forget any history replay a previous walk left armed. Walkers are
             pooled, and one abandoned part-way through a key's history (a
             `take` that stopped early) would otherwise replay that key's
             entries into the next walk's first item (#735). */
          void ResetHistory() {
            HistRemaining = 0UL;
            HistCursor.reset();
          }

          /* #227 / #49: after yielding a current key that is a deferred
             commutative mutator, set up a cursor over its history entries so
             the next Refresh() replays them into the fold. No-op for Assign
             (latest-wins) keys and keys with no history. */
          void ArmHistory(size_t num_hist_keys, size_t offset_of_hist_keys) const {
            if (Item.Mutator != TMutator::Assign && num_hist_keys) {
              HistCursor = std::make_unique<typename TMyReadFile::TIndexFile::THistoryKeyCursor>(
                  IndexFile, offset_of_hist_keys / TData::KeyHistorySize);
              HistRemaining = num_hist_keys;
            }
          }

          Base::TUuid IndexId;

          TMyReadFile *MyReadFile;

          typename TMyReadFile::TIndexFile *IndexFile;

          TArena MainArena;
          std::unique_ptr<TArena> IndexArena;

          TKey From;

          TKey To;

          mutable TInStream Stream;

          mutable TInStream IndexStream;

          mutable bool Valid;

          mutable bool Cached;

          mutable TItem Item;

          mutable size_t Offset;

          /* #227 / #49: when the current key just yielded is a deferred
             commutative mutator, its earlier increments live as HISTORY
             entries in this file (PushKey makes a key's first entry current
             and the rest history). The read-time fold (context.cc
             ApplyDeferredFold) needs every increment, so after yielding the
             current entry we replay this key's history entries through the
             same TItem. Without this, a commutative key that has been merged
             to disk reads back as only its single current increment. */
          mutable std::unique_ptr<typename TMyReadFile::TIndexFile::THistoryKeyCursor> HistCursor;
          mutable size_t HistRemaining = 0UL;

          /* The rank of the first key past the walk's range, once a count has needed it; NoRank until then (#749). */
          mutable size_t EndRank = NoRank;

          TLoaderObj *LoaderObj;

          TPresentWalkFile *NextWalker;

          friend class TLoaderObj;
          friend class TPresentWalkFileWrapper;

        };  // TPresentWalkFile

        TPresentWalkFile *Get(Util::TEngine *engine,
                              const Base::TUuid &file_id,
                              size_t gen_id,
                              const Base::TUuid &index_id) {
          TLoaderObj *loader = LoaderCollection.TryGetFirstMember(TWalkerKey(file_id, gen_id, index_id));
          if (!loader) {
            loader = new TLoaderObj(this, file_id, gen_id, index_id);
          }
          return loader->GetNewFile(engine, file_id, gen_id, index_id);
        }

        void Clear(const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id) {
          TLoaderObj *loader = LoaderCollection.TryGetFirstMember(TWalkerKey(file_id, gen_id, index_id));
          delete loader;
        }

        /* Fiber-safe: see TFiberSafeLocal (#554, #578). */
        struct TCacheTag;
        inline static Fiber::TFiberSafeLocal<TLocalWalkerCache, TCacheTag> Cache;

        class TLoaderObj {
          NO_COPY(TLoaderObj);
          public:

          typedef InvCon::UnorderedMultimap::TMembership<TLoaderObj, TLocalWalkerCache, TWalkerKey> TCacheMembership;

          TLoaderObj(TLocalWalkerCache *cache, const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id)
              : FreeQueue(nullptr),
                CacheMembership(this, TWalkerKey(file_id, gen_id, index_id), &cache->LoaderCollection) {}

          ~TLoaderObj() {
            for (TPresentWalkFile *file = FreeQueue; file;) {
              TPresentWalkFile *to_delete = file;
              file = file->NextWalker;
              delete to_delete;
            }
          }

          TPresentWalkFile *GetNewFile(Util::TEngine *engine, const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id) {
            if (!FreeQueue) {
              return new TPresentWalkFile(engine, file_id, gen_id, index_id, this);
            } else {
              TPresentWalkFile *ret = FreeQueue;
              FreeQueue = ret->NextWalker;
              return ret;
            }
          }

          void Free(TPresentWalkFile *walker) {
            walker->NextWalker = FreeQueue;
            FreeQueue = walker;
          }

          private:

          TPresentWalkFile *FreeQueue;

          typename TCacheMembership::TImpl CacheMembership;

        };  // TLoaderObj

        private:

        typedef InvCon::UnorderedMultimap::TCollection<TLocalWalkerCache, TLoaderObj, TWalkerKey> TLoaderCollection;

        mutable typename TLoaderCollection::TImpl LoaderCollection;

      };  // TLocalWalkerCache

      class TPresentWalkFileWrapper
          : public TPresentWalker {
        NO_COPY(TPresentWalkFileWrapper);
        public:

        TPresentWalkFileWrapper(Util::TEngine *engine,
                                const Base::TUuid &file_id,
                                size_t gen_id,
                                const Base::TUuid &index_id,
                                const TKey &from,
                                const TKey &to)
            : TPresentWalker(Match),
          File(TLocalWalkerCache::Cache->Get(engine, file_id, gen_id, index_id)) {
          File->Init(from, to);
        }

        TPresentWalkFileWrapper(Util::TEngine *engine,
                                const Base::TUuid &file_id,
                                size_t gen_id,
                                const Base::TUuid &index_id,
                                const TKey &key)
            : TPresentWalker(Match),
          File(TLocalWalkerCache::Cache->Get(engine, file_id, gen_id, index_id)) {
          File->Init(key);
        }

        virtual ~TPresentWalkFileWrapper() {
          assert(File);
          assert(File->LoaderObj);
          File->LoaderObj->Free(File);
        }

        /* True iff. we have an item. */
        inline virtual operator bool() const {
          return static_cast<bool>(*File);
        }

        /* The current item. */
        inline virtual const TItem &operator*() const {
          return **File;
        }

        /* Walk to the next item, if any. */
        inline virtual TPresentWalker &operator++() {
          ++(*File);
          return *this;
        }

        /* See TPresentWalkFile (#749). */
        size_t GetNumRanks() const {
          return File->GetNumRanks();
        }

        void NextKey() {
          File->NextKey();
        }

        std::pair<size_t, size_t> SkipBelow(const TItem *bound, const TTombstoneCounts &counts) {
          return File->SkipBelow(bound, counts);
        }

        private:

        TLocalWalkerCache::TPresentWalkFile *File;

      };  // TPresentWalkFileWrapper

    }  // Disk

  }  // Indy

}  // Orly
