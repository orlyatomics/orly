/* <orly/indy/disk_layer.h>

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
#include <atomic>
#include <cstdint>
#include <cassert>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <base/class_traits.h>
#include <base/inv_con/ordered_list.h>
#include <base/uuid.h>
#include <orly/indy/disk/key_filter.h>
#include <orly/indy/disk/tombstone_counts.h>
#include <orly/indy/manager_base.h>
#include <orly/indy/update.h>

namespace Orly {

  namespace Indy {

    class TDiskLayer
        : public L0::TManager::TRepo::TDataLayer {
      NO_COPY(TDiskLayer);
      public:

      TDiskLayer(L0::TManager *manager,
                 L0::TManager::TRepo *repo,
                 size_t gen_id,
                 size_t num_keys,
                 TSequenceNumber lowest_seq,
                 TSequenceNumber highest_seq);

      ~TDiskLayer() override;

      std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &from,
                                                             const TIndexKey &to) const override;

      /* exact_point is ignored: disk layers already seek via TIndexManager
         (hashed), so there is no head-scan to avoid (#257). */
      std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &key, bool exact_point = false) const override;

      /* Walkers that read the file as of sequence number `at`: each key yields its newest entry at or below it (#745). */
      std::unique_ptr<Indy::TPresentWalker> NewPresentWalkerAt(const TIndexKey &from, const TIndexKey &to, TSequenceNumber at) const;

      std::unique_ptr<Indy::TPresentWalker> NewPresentWalkerAt(const TIndexKey &key, TSequenceNumber at) const;

      std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalker(TSequenceNumber from) const override;

      [[nodiscard]] TKind GetKind() const noexcept override;

      [[nodiscard]] size_t GetGenId() const noexcept;

      [[nodiscard]] size_t GetNumKeys() const noexcept;

      [[nodiscard]] size_t GetSize() const noexcept override;

      [[nodiscard]] TSequenceNumber GetLowestSeq() const noexcept override;

      [[nodiscard]] TSequenceNumber GetHighestSeq() const noexcept override;

      void ClearLocalCaches() override;

      /* The tombstone counts of this file's num_keys current keys in the index, made on first
         use and shared by every count that passes over the file, on any runner, for the file's
         life (#749). Reads nothing from disk: the counts fill in as counts read the file. */
      [[nodiscard]] std::shared_ptr<const Disk::TTombstoneCounts> GetTombstoneCounts(const Base::TUuid &index_id, size_t num_keys) const;

      /* False when this file holds no key in the index whose first num_defined members hash to `hash` (TKey::GetHash of a
         key, or of a pattern whose free members trail num_defined defined ones), so a read for such a key or pattern can
         pass over the file without building a walker for it. A walker would find nothing there: its first step is
         FindInHash in the same table. True when the file may hold one.

         The first call for an index and a num_defined reads that hash table once, with no lock held, and keeps a filter
         over it (Disk::TKeyFilter, 1.5 bytes per entry) for the file's life. Calls that come while it is being made, or
         for more than MaxKeyFilters pairs, or for a table bigger than TSafeRepo::MaxKeyFilterSlots, say true. */
      [[nodiscard]] bool MayContain(const Base::TUuid &index_id, size_t num_defined, uint64_t hash) const;

      /* The (index, num_defined) pairs one file keeps filters for. */
      static constexpr size_t MaxKeyFilters = 8UL;

      private:

      /* The tombstone counts by index. Made on first use, so a file no count passes over pays
         one pointer for it. */
      struct TTombstoneCache {
        std::mutex Mutex;
        std::vector<std::pair<Base::TUuid, std::shared_ptr<const Disk::TTombstoneCounts>>> ByIndex;
      };

      mutable std::atomic<TTombstoneCache *> TombstoneCache = nullptr;

      /* The filters for MayContain. Made on first use, so a file no read checks pays one pointer for it. An entry is
         claimed under Mutex and published by NumEntries; its filter is made after, with no lock held, and published by
         Ready. A null filter that is ready means the file may hold anything. */
      struct TKeyFilterCache {
        struct TEntry {
          Base::TUuid IndexId;
          size_t NumDefined = 0UL;
          std::unique_ptr<const Disk::TKeyFilter> Filter;
          std::atomic<bool> Ready = false;
        };
        std::mutex Mutex;
        std::atomic<size_t> NumEntries = 0UL;
        std::array<TEntry, MaxKeyFilters> Entries;
      };
      mutable std::atomic<TKeyFilterCache *> KeyFilterCache = nullptr;

      L0::TManager::TRepo *Repo;

      /* Set once ClearLocalCaches has run on every runner, so the destructor
         can skip that tour (#584). */
      bool CachesCleared = false;

      size_t GenId;

      size_t NumKeys;

      TSequenceNumber LowestSeq, HighestSeq;

    };  // TDiskLayer

    inline L0::TManager::TRepo::TDataLayer::TKind TDiskLayer::GetKind() const noexcept {
      return L0::TManager::TRepo::TDataLayer::Disk;
    }

    inline size_t TDiskLayer::GetGenId() const noexcept {
      return GenId;
    }

    inline size_t TDiskLayer::GetNumKeys() const noexcept {
      return NumKeys;
    }

    inline size_t TDiskLayer::GetSize() const noexcept {
      return NumKeys;
    }

    inline TSequenceNumber TDiskLayer::GetLowestSeq() const noexcept {
      return LowestSeq;
    }

    inline TSequenceNumber TDiskLayer::GetHighestSeq() const noexcept {
      return HighestSeq;
    }

  }  // Indy

}  // Orly

