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

using namespace std;
using namespace Base;
using namespace Orly::Indy;

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
  if (GetMarkedForDelete()) {
    assert(Repo->IsSafeRepo());
    try {
      Repo->RemoveFile(GenId);
    } catch (const Disk::TDiskServiceShutdown &/*ex*/) {
      /*ignore, we're shutting down by force! */
    }
  }
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalker(const TIndexKey &from,
                                                        const TIndexKey &to) const {
  return Repo->NewPresentWalkerFile(GenId, from, to);
}

unique_ptr<TPresentWalker> TDiskLayer::NewPresentWalker(const TIndexKey &key) const {
  return Repo->NewPresentWalkerFile(GenId, key);
}

unique_ptr<TUpdateWalker> TDiskLayer::NewUpdateWalker(TSequenceNumber from) const {
  return Repo->NewUpdateWalkerFile(GenId, from);
}