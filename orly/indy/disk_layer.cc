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

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalker(const TIndexKey &from,
                                                        const TIndexKey &to) const {
  return Repo->NewPresentWalkerFile(GenId, from, to);
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalker(const TIndexKey &key, bool /*exact_point*/) const {
  return Repo->NewPresentWalkerFile(GenId, key);
}

unique_ptr<TUpdateWalker> TDiskLayer::NewUpdateWalker(TSequenceNumber from) const {
  return Repo->NewUpdateWalkerFile(GenId, from);
}