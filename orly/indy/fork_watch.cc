/* <orly/indy/fork_watch.cc>

   Implements <orly/indy/fork_watch.h>.

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

#include <orly/indy/fork_watch.h>

#include <algorithm>
#include <new>

#include <orly/indy/update.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Indy;

TForkWatch::TForkWatch(const TUuid &pov_id, vector<TUuid> chain, TMode mode, size_t max_keys)
    : PovId(pov_id), Chain(std::move(chain)), Mode(mode), MaxKeys(max_keys), Arena(make_unique<Atom::TSuprena>()) {}

TIndexKey TForkWatch::CopyKey(const TIndexKey &key) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  return TIndexKey(key.GetIndexId(), TKey(Arena.get(), state_alloc, key.GetKey()));
}

void TForkWatch::AddConflict(const TIndexKey &key, bool is_delete, bool raced) {
  Conflicts.push_back(TConflict{++ConflictCount, CopyKey(key), is_delete, raced});
  if (Conflicts.size() > MaxConflicts) {
    Conflicts.pop_front();
  }
}

void TForkWatch::OnAppend(const TUpdate &update, const TUuid &promoted_from) noexcept {
  lock_guard<mutex> lock(Mutex);
  try {
    if (promoted_from == PovId) {
      /* The POV's own update, arriving in its parent: its overwrites and deletes of keys the
         chain changed are conflicts, and its version of those keys now stands. */
      for (TUpdate::TEntryCollection::TCursor csr(update.GetEntryCollection()); csr; ++csr) {
        if (csr->GetMutator() != TMutator::Assign) {
          continue;
        }
        const TIndexKey &key = csr->GetIndexKey();
        auto iter = Changed.find(key);
        if (iter != Changed.end() || Overflowed) {
          /* Tetris tests a refusing POV's update before it promotes it, so finding a conflict here
             in that mode means the parent changed the key after the test. */
          AddConflict(key, csr->GetOp().IsTombstone(), Mode == TMode::Refuse && !PromotingForced);
          if (iter != Changed.end()) {
            Changed.erase(iter);
          }
        }
      }
      return;
    }
    if (promoted_from != TUuid() && find(Chain.begin(), Chain.end(), promoted_from) != Chain.end()) {
      /* Content moving up the chain, which the POV could already see. */
      return;
    }
    if (Overflowed) {
      return;
    }
    for (TUpdate::TEntryCollection::TCursor csr(update.GetEntryCollection()); csr; ++csr) {
      const TIndexKey &key = csr->GetIndexKey();
      if (Changed.find(key) != Changed.end()) {
        continue;
      }
      if (Changed.size() >= MaxKeys) {
        Overflowed = true;
        return;
      }
      Changed.insert(CopyKey(key));
    }
  } catch (const bad_alloc &) {
    /* NO_THROW: the caller is AppendUpdate, on a transaction's commit path.  The watch can no
       longer tell what changed, so it says everything did. */
    Overflowed = true;
  }
}

vector<TForkWatch::TConflict> TForkWatch::FindConflicts(const TUpdate &update) const {
  vector<TConflict> result;
  lock_guard<mutex> lock(Mutex);
  for (TUpdate::TEntryCollection::TCursor csr(update.GetEntryCollection()); csr; ++csr) {
    if (csr->GetMutator() != TMutator::Assign) {
      continue;
    }
    if (Overflowed || Changed.find(csr->GetIndexKey()) != Changed.end()) {
      /* The key stays in the update's arena: callers use it before the update goes. */
      result.push_back(TConflict{0UL, csr->GetIndexKey(), csr->GetOp().IsTombstone(), false});
    }
  }
  return result;
}

bool TForkWatch::IsChanged(const TIndexKey &key) const {
  lock_guard<mutex> lock(Mutex);
  return Overflowed || Changed.find(key) != Changed.end();
}

bool TForkWatch::ShouldBlock(const TUpdate &update, TSequenceNumber seq_num) {
  auto conflicts = FindConflicts(update);
  lock_guard<mutex> lock(Mutex);
  if (conflicts.empty() || (ForcedUpTo && seq_num <= *ForcedUpTo)) {
    BlockingKeys.clear();
    PromotingForced = !conflicts.empty();
    return false;
  }
  /* Copy the keys out of the update into our arena, unless we're already blocked on them: Tetris
     tests a blocked POV every round. */
  bool same = BlockingKeys.size() == conflicts.size();
  for (size_t i = 0; same && i < conflicts.size(); ++i) {
    same = BlockingKeys[i].Key == conflicts[i].Key && BlockingKeys[i].IsDelete == conflicts[i].IsDelete;
  }
  if (!same) {
    BlockingKeys.clear();
    for (const auto &conflict: conflicts) {
      BlockingKeys.push_back(TConflict{0UL, CopyKey(conflict.Key), conflict.IsDelete, false});
    }
  }
  PromotingForced = false;
  return true;
}

void TForkWatch::Force(TSequenceNumber seq_num) {
  lock_guard<mutex> lock(Mutex);
  if (!ForcedUpTo || *ForcedUpTo < seq_num) {
    ForcedUpTo = seq_num;
  }
  BlockingKeys.clear();
}

void TForkWatch::Refork() {
  lock_guard<mutex> lock(Mutex);
  Changed.clear();
  Overflowed = false;
  BlockingKeys.clear();
  /* Copy the conflicts we keep into a new arena, so the old one, and every key it holds, goes. */
  auto old_arena = std::move(Arena);
  Arena = make_unique<Atom::TSuprena>();
  for (auto &conflict: Conflicts) {
    conflict.Key = CopyKey(conflict.Key);
  }
}

TForkWatch::TState TForkWatch::GetState() const {
  lock_guard<mutex> lock(Mutex);
  TState state;
  state.Blocked = !BlockingKeys.empty();
  state.Overflowed = Overflowed;
  state.ChangedKeys = Changed.size();
  state.ConflictCount = ConflictCount;
  state.ForcedUpTo = ForcedUpTo;
  return state;
}

void TForkWatch::ForEachConflict(uint64_t after, const function<void (const TConflict &)> &cb) const {
  lock_guard<mutex> lock(Mutex);
  for (const auto &conflict: Conflicts) {
    if (conflict.Number > after) {
      cb(conflict);
    }
  }
}

void TForkWatch::ForEachBlockingKey(const function<void (const TConflict &)> &cb) const {
  lock_guard<mutex> lock(Mutex);
  for (const auto &conflict: BlockingKeys) {
    cb(conflict);
  }
}
