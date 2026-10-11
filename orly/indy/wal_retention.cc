/* <orly/indy/wal_retention.cc>

   Implements <orly/indy/wal_retention.h>.

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

#include <orly/indy/wal_retention.h>

#include <algorithm>
#include <cassert>
#include <sstream>

#include <syslog.h>

#include <orly/indy/disk/wal.h>
#include <orly/indy/transaction_base.h>

using namespace std;
using namespace Orly::Indy;

TWalRetention::TWalRetention(L1::TManager *manager)
    : Manager(manager) {
  assert(manager);
}

void TWalRetention::PinTxn(uint64_t lsn, const vector<TPush> &pushes, const vector<TPop> &pops) {
  /* LSNs start at 1; a pin at 0 would hold the head before the ring's first group for good. */
  assert(lsn);
  if (!lsn) {
    syslog(LOG_ERR, "TWalRetention: refusing to pin LSN 0");
    return;
  }
  /* A promotion pops updates out of a child and pushes them into its parent in one record.  The
     child's own records are what rebuild the child, so whatever the child needed for them, the
     parent now needs for its copy, until that copy reaches a data file. */
  optional<uint64_t> carried;
  for (const auto &pop : pops) {
    auto iter = Repos.find(pop.RepoId);
    if (iter == Repos.end()) {
      continue;
    }
    auto &pins = iter->second.Pins;
    const auto end = pins.upper_bound(pop.Seq);
    for (auto pin = pins.begin(); pin != end; ++pin) {
      carried = carried ? min(*carried, pin->second.Lsn) : pin->second.Lsn;
    }
    pins.erase(pins.begin(), end);
    if (pins.empty()) {
      Repos.erase(iter);
    }
  }
  for (const auto &push : pushes) {
    auto &repo = Repos[push.RepoId];
    repo.IsRoot = push.IsRoot;
    repo.Pins[push.Seq] = TPin{carried ? min(lsn, *carried) : lsn, lsn};
  }
}

uint64_t TWalRetention::AppendTxn(const string &wire, const vector<TPush> &pushes, const vector<TPop> &pops) {
  Disk::TWal *wal = Manager->GetWal();
  assert(wal);
  lock_guard<mutex> lock(Mutex);
  const uint64_t lsn = wal->Append(Disk::TWalRecordType::Txn, wire.data(), wire.size());
  PinTxn(lsn, pushes, pops);
  return lsn;
}

uint64_t TWalRetention::AppendPov(const Base::TUuid &pov_id, const string &body) {
  Disk::TWal *wal = Manager->GetWal();
  assert(wal);
  lock_guard<mutex> lock(Mutex);
  const uint64_t lsn = wal->Append(Disk::TWalRecordType::Pov, body.data(), body.size());
  Povs[pov_id] = TPovPin{lsn, body};
  return lsn;
}

void TWalRetention::BeginReplay() {
  lock_guard<mutex> lock(Mutex);
  Replaying = true;
  ReplayLsn = 0UL;
}

void TWalRetention::SetReplayLsn(uint64_t lsn) {
  lock_guard<mutex> lock(Mutex);
  ReplayLsn = lsn;
}

void TWalRetention::EndReplay() {
  lock_guard<mutex> lock(Mutex);
  Replaying = false;
  ReplayLsn = 0UL;
}

bool TWalRetention::NoteReplayedTxn(const vector<TPush> &pushes, const vector<TPop> &pops) {
  lock_guard<mutex> lock(Mutex);
  /* Before the first replayed record (while recovery restores povs) a transaction comes from no
     record at all, so there is nothing to pin it to. */
  if (!Replaying || !ReplayLsn) {
    return false;
  }
  PinTxn(ReplayLsn, pushes, pops);
  return true;
}

void TWalRetention::NoteRestoredPov(const Base::TUuid &pov_id, uint64_t lsn, const string &body) {
  lock_guard<mutex> lock(Mutex);
  Povs[pov_id] = TPovPin{lsn, body};
}

void TWalRetention::WaitWriteAhead(const Base::TUuid &root_id, TSequenceNumber high_seq) {
  Disk::TWal *wal = Manager->GetWal();
  if (!wal) {
    return;
  }
  /* A transaction applies its pushes and appends its record under the replication queue lock, so
     once we have had the lock every update in the layer being written has its pin. */
  /* barrier */ {
    lock_guard<mutex> barrier(Manager->GetReplicationQueueLock());
  }
  uint64_t needed = 0UL;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    auto iter = Repos.find(root_id);
    if (iter != Repos.end()) {
      const auto &pins = iter->second.Pins;
      const auto end = pins.upper_bound(high_seq);
      for (auto pin = pins.begin(); pin != end; ++pin) {
        needed = max(needed, pin->second.TxnLsn);
      }
    }
  }
  if (needed) {
    wal->WaitForDurable(needed);
  }
}

void TWalRetention::OnRootFlushed(const Base::TUuid &root_id, TSequenceNumber low_seq, TSequenceNumber high_seq) {
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    auto iter = Repos.find(root_id);
    if (iter != Repos.end()) {
      auto &pins = iter->second.Pins;
      pins.erase(pins.lower_bound(low_seq), pins.upper_bound(high_seq));
      if (pins.empty()) {
        Repos.erase(iter);
      }
    }
  }
  /* lock */ {
    lock_guard<mutex> lock(WorkMutex);
    WorkPending = true;
  }
  WorkCv.notify_all();
}

uint64_t TWalRetention::ComputeHeadLocked(Disk::TWal *wal) const {
  uint64_t head = wal ? wal->GetLastAppendedLsn() + 1UL : UINT64_MAX;
  for (const auto &repo : Repos) {
    for (const auto &pin : repo.second.Pins) {
      head = min(head, pin.second.Lsn);
    }
  }
  for (const auto &pov : Povs) {
    head = min(head, pov.second.Lsn);
  }
  return head;
}

void TWalRetention::DropDeadPins() {
  vector<Base::TUuid> repo_ids, pov_ids;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    repo_ids.reserve(Repos.size());
    for (const auto &repo : Repos) {
      repo_ids.push_back(repo.first);
    }
    pov_ids.reserve(Povs.size());
    for (const auto &pov : Povs) {
      pov_ids.push_back(pov.first);
    }
  }
  /* IsLiveRepo takes the manager's own mutex, so never ask with ours held. */
  vector<Base::TUuid> dead_repos, dead_povs;
  for (const auto &id : repo_ids) {
    if (!Manager->IsLiveRepo(id)) {
      dead_repos.push_back(id);
    }
  }
  for (const auto &id : pov_ids) {
    if (!Manager->IsLiveRepo(id)) {
      dead_povs.push_back(id);
    }
  }
  if (dead_repos.empty() && dead_povs.empty()) {
    return;
  }
  /* A repo that has gone takes its unpromoted updates with it, and nothing creates a repo again
     under an old id, so its pins can't be needed. */
  lock_guard<mutex> lock(Mutex);
  for (const auto &id : dead_repos) {
    Repos.erase(id);
  }
  for (const auto &id : dead_povs) {
    Povs.erase(id);
  }
}

uint64_t TWalRetention::ComputeHead() {
  DropDeadPins();
  lock_guard<mutex> lock(Mutex);
  return ComputeHeadLocked(Manager->GetWal());
}

void TWalRetention::CopyPovsForward(Disk::TWal *wal) {
  const size_t limit = wal->GetCapacityBytes() / 2UL;
  /* old lsn, new lsn */
  vector<pair<Base::TUuid, pair<uint64_t, uint64_t>>> copies;
  uint64_t newest = 0UL;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    for (const auto &pov : Povs) {
      if (wal->GetRingBytesBehind(pov.second.Lsn) > limit) {
        const uint64_t lsn = wal->Append(Disk::TWalRecordType::Pov, pov.second.Body.data(), pov.second.Body.size(), Disk::WalRecordFlags::Copied);
        copies.emplace_back(pov.first, make_pair(pov.second.Lsn, lsn));
        newest = lsn;
      }
    }
  }
  if (copies.empty()) {
    return;
  }
  /* The old record holds the head until the copy is durable.  If the ring is too full for the
     copy to land, leave the pins where they are; the next pass copies again. */
  if (!wal->WaitForDurable(newest, chrono::milliseconds(5000))) {
    syslog(LOG_WARNING, "TWalRetention: %ld copied Pov records not durable yet; their pins stay put", copies.size());
    return;
  }
  lock_guard<mutex> lock(Mutex);
  for (const auto &copy : copies) {
    auto iter = Povs.find(copy.first);
    if (iter != Povs.end() && iter->second.Lsn == copy.second.first) {
      iter->second.Lsn = copy.second.second;
    }
  }
  syslog(LOG_INFO, "TWalRetention: copied %ld Pov records forward", copies.size());
}

void TWalRetention::Checkpoint(uint64_t global_flushed_seq, bool force) {
  Disk::TWal *wal = Manager->GetWal();
  if (!wal) {
    return;
  }
  lock_guard<mutex> checkpoint_lock(CheckpointMutex);
  DropDeadPins();
  CopyPovsForward(wal);
  uint64_t head;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    head = ComputeHeadLocked(wal);
  }
  if (!force && LastCheckpointHead && *LastCheckpointHead == head) {
    return;
  }
  if (PayloadProvider) {
    const std::string payload = PayloadProvider(head);
    wal->Checkpoint(head, global_flushed_seq, payload.data(), payload.size());
  } else {
    wal->Checkpoint(head, global_flushed_seq);
  }
  LastCheckpointHead = head;
}

void TWalRetention::SetCheckpointPayload(TPayloadProvider provider) {
  PayloadProvider = std::move(provider);
}

bool TWalRetention::WaitForWork(chrono::milliseconds timeout) {
  unique_lock<mutex> lock(WorkMutex);
  WorkCv.wait_for(lock, timeout, [this] { return Stopping || WorkPending; });
  WorkPending = false;
  return !Stopping;
}

void TWalRetention::Stop() {
  /* lock */ {
    lock_guard<mutex> lock(WorkMutex);
    Stopping = true;
  }
  WorkCv.notify_all();
}
