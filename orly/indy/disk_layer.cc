/* <orly/indy/disk_layer.cc>

   Implements <orly/indy/disk_layer.h>.

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

#include <orly/indy/disk_layer.h>

#include <syslog.h>

#include <optional>

#include <orly/indy/memory_layer.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy;

/* Data layers of both kinds share one pool, whose blocks most callers size for a memory layer. */
static_assert(sizeof(TDiskLayer) <= sizeof(TMemoryLayer), "a disk layer must fit a data layer pool block");

TDiskLayer::TDiskLayer(L0::TManager *manager,
                       L0::TManager::TRepo *repo,
                       size_t gen_id,
                       size_t num_keys,
                       TSequenceNumber lowest_seq,
                       TSequenceNumber highest_seq)
    : TDataLayer(manager),
      Repo(repo),
      GenId(gen_id),
      NumKeys(num_keys),
      LowestSeq(lowest_seq),
      HighestSeq(highest_seq) {
  assert(HighestSeq >= LowestSeq);
}

TDiskLayer::~TDiskLayer() {
  delete TombstoneCache.load(memory_order_acquire);
  delete KeyFilterCache.load(memory_order_acquire);
  if (GetMarkedForDelete()) {
    assert(Repo->IsSafeRepo());
    try {
      Repo->RemoveFile(GenId, CachesCleared);
    } catch (const Disk::TDiskServiceShutdown &/*ex*/) {
      /*ignore, we're shutting down by force! */
    } catch (const std::exception &ex) {
      /* A destructor must never let an exception escape: doing so calls
         std::terminate and brings down the whole server. This one ran on
         layer-cleanup (TManager::RunLayerCleaner deleting a marked-for-delete
         disk layer). RemoveFile only reclaims the blocks of a file that is
         already being deleted, so a failure here -- e.g. a device-capacity or
         I/O error surfacing during cleanup -- is at worst a recoverable block
         leak, never data loss. Log and swallow rather than terminate. */
      syslog(LOG_ERR, "TDiskLayer::~TDiskLayer RemoveFile(%ld) failed: %s", GenId, ex.what());
    } catch (...) {
      syslog(LOG_ERR, "TDiskLayer::~TDiskLayer RemoveFile(%ld) failed: unknown exception", GenId);
    }
  }
}

void TDiskLayer::ClearLocalCaches() {
  /* Only a layer on its way out may drop its file's caches; a live one is
     still being read through them. */
  if (GetMarkedForDelete()) {
    Repo->ClearLocalFileCaches(GenId);
    CachesCleared = true;
  }
}

shared_ptr<const Disk::TTombstoneCounts> TDiskLayer::GetTombstoneCounts(const TUuid &index_id, size_t num_keys) const {
  TTombstoneCache *cache = TombstoneCache.load(memory_order_acquire);
  if (!cache) {
    auto made = make_unique<TTombstoneCache>();
    if (TombstoneCache.compare_exchange_strong(cache, made.get(), memory_order_acq_rel)) {
      cache = made.release();
    }
  }
  /* Held only to find or add an entry; nothing here waits on disk or yields the fiber. */
  lock_guard<mutex> lock(cache->Mutex);
  for (const auto &[id, counts] : cache->ByIndex) {
    if (id == index_id) {
      assert(counts->GetNumKeys() == num_keys);
      return counts;
    }
  }
  cache->ByIndex.emplace_back(index_id, make_shared<const Disk::TTombstoneCounts>(num_keys));
  return cache->ByIndex.back().second;
}

bool TDiskLayer::MayContain(const TUuid &index_id, size_t num_defined, uint64_t hash) const {
  if (num_defined == 0UL) {
    return true;
  }
  TKeyFilterCache *cache = KeyFilterCache.load(memory_order_acquire);
  if (!cache) {
    auto made = make_unique<TKeyFilterCache>();
    if (KeyFilterCache.compare_exchange_strong(cache, made.get(), memory_order_acq_rel)) {
      cache = made.release();
    }
  }
  const auto try_entry = [&](const TKeyFilterCache::TEntry &entry) -> optional<bool> {
    if (entry.NumDefined != num_defined || entry.IndexId != index_id) {
      return nullopt;
    }
    if (!entry.Ready.load(memory_order_acquire)) {
      return true;
    }
    return !entry.Filter || entry.Filter->MayContain(hash);
  };
  const size_t num_entries = cache->NumEntries.load(memory_order_acquire);
  for (size_t i = 0; i < num_entries; ++i) {
    if (const optional<bool> answer = try_entry(cache->Entries[i])) {
      return *answer;
    }
  }
  if (num_entries == MaxKeyFilters) {
    return true;
  }
  TKeyFilterCache::TEntry *mine = nullptr;
  {
    /* Held only to claim an entry; the filter is made after it is released, since making it reads the file. */
    lock_guard<mutex> lock(cache->Mutex);
    const size_t now = cache->NumEntries.load(memory_order_relaxed);
    for (size_t i = num_entries; i < now; ++i) {
      if (const optional<bool> answer = try_entry(cache->Entries[i])) {
        return *answer;
      }
    }
    if (now == MaxKeyFilters) {
      return true;
    }
    mine = &cache->Entries[now];
    mine->IndexId = index_id;
    mine->NumDefined = num_defined;
    cache->NumEntries.store(now + 1UL, memory_order_release);
  }
  try {
    unique_ptr<const Disk::TKeyFilter> filter = Repo->MakeKeyFilter(GenId, index_id, num_defined);
    if (filter) {
      const size_t num_bytes = filter->GetNumBytes();
      if (cache->NumBytes.fetch_add(num_bytes, memory_order_relaxed) + num_bytes > MaxKeyFilterBytes) {
        cache->NumBytes.fetch_sub(num_bytes, memory_order_relaxed);
        filter.reset();
      }
    }
    mine->Filter = std::move(filter);
  } catch (...) {
    /* Leave the entry saying "may hold anything" and fail this read as building its walker would have. */
    mine->Ready.store(true, memory_order_release);
    throw;
  }
  mine->Ready.store(true, memory_order_release);
  return !mine->Filter || mine->Filter->MayContain(hash);
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalker(const TIndexKey &from,
                                                        const TIndexKey &to) const {
  return Repo->NewPresentWalkerFile(GenId, from, to);
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalker(const TIndexKey &key, bool /*exact_point*/) const {
  return Repo->NewPresentWalkerFile(GenId, key);
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalkerAt(const TIndexKey &from, const TIndexKey &to, TSequenceNumber at) const {
  return Repo->NewPresentWalkerFile(GenId, from, to, at);
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalkerAt(const TIndexKey &key, TSequenceNumber at) const {
  return Repo->NewPresentWalkerFile(GenId, key, at);
}

unique_ptr<TUpdateWalker> TDiskLayer::NewUpdateWalker(TSequenceNumber from) const {
  return Repo->NewUpdateWalkerFile(GenId, from);
}