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

#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
#include <orly/indy/disk/wal.h>
#include <orly/indy/replication.h>
#include <orly/indy/transaction_base.h>

using namespace std;
using namespace Orly::Indy;

TWalRetention::TWalRetention(L1::TManager *manager)
    : Manager(manager) {
  assert(manager);
}

vector<Disk::TPromotionDebt> TWalRetention::GetPopDebtsLocked(const vector<TPop> &pops) const {
  vector<Disk::TPromotionDebt> debts;
  for (const auto &pop : pops) {
    auto iter = Repos.find(pop.RepoId);
    if (iter == Repos.end()) {
      continue;
    }
    const auto &pins = iter->second.Pins;
    const auto end = pins.upper_bound(pop.Seq);
    for (auto pin = pins.begin(); pin != end; ++pin) {
      if (pin->second.Debt.Levels) {
        debts.push_back(pin->second.Debt);
      }
    }
  }
  return debts;
}

void TWalRetention::OweWalLocked(Disk::TWal *wal) {
  if (!wal || DebtWal == wal) {
    return;
  }
  Disk::TWal::TPromotionDebtChange change;
  for (const auto &repo : Repos) {
    for (const auto &pin : repo.second.Pins) {
      if (pin.second.Debt.Levels) {
        change.Owed.push_back(pin.second.Debt);
      }
    }
  }
  wal->ChangePromotionDebt(change);
  DebtWal = wal;
  if (!change.Owed.empty()) {
    syslog(LOG_INFO, "TWalRetention: %ld unpromoted updates are owed %ld KiB of promotion room", change.Owed.size(), wal->GetPromotionDebtBytes() >> 10);
  }
}

void TWalRetention::RepayDroppedLocked(vector<Disk::TPromotionDebt> dropped, size_t already_repaid) {
  assert(already_repaid <= dropped.size());
  if (!DebtWal || dropped.size() <= already_repaid) {
    return;
  }
  dropped.erase(dropped.begin(), dropped.begin() + static_cast<ptrdiff_t>(already_repaid));
  Disk::TWal::TPromotionDebtChange change;
  change.Repaid = std::move(dropped);
  DebtWal->ChangePromotionDebt(change);
}

size_t TWalRetention::GetPromotionCredit(const vector<TPop> &pops) {
  lock_guard<mutex> lock(Mutex);
  OweWalLocked(Manager->GetWal());
  size_t credit = 0UL;
  for (const auto &debt : GetPopDebtsLocked(pops)) {
    credit += debt.GroupBytes * debt.Levels;
  }
  return credit;
}

vector<Disk::TPromotionDebt> TWalRetention::PinTxn(uint64_t lsn, const vector<TPush> &pushes, const vector<TPop> &pops,
                                                    const vector<TStatusChange> &statuses, size_t debt_group_bytes) {
  vector<Disk::TPromotionDebt> dropped;
  /* LSNs start at 1; a pin at 0 would hold the head before the ring's first group for good. */
  assert(lsn);
  if (!lsn) {
    syslog(LOG_ERR, "TWalRetention: refusing to pin LSN 0");
    return dropped;
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
      if (pin->second.Debt.Levels) {
        dropped.push_back(pin->second.Debt);
      }
    }
    pins.erase(pins.begin(), end);
    if (pins.empty()) {
      Repos.erase(iter);
    }
  }
  for (const auto &push : pushes) {
    auto &repo = Repos[push.RepoId];
    repo.IsRoot = push.IsRoot;
    const Disk::TPromotionDebt debt{push.Levels ? debt_group_bytes : 0UL, push.Levels};
    auto [pin, is_new] = repo.Pins.try_emplace(push.Seq, TPin{carried ? min(lsn, *carried) : lsn, lsn, debt});
    if (!is_new) {
      if (pin->second.Debt.Levels) {
        dropped.push_back(pin->second.Debt);
      }
      pin->second = TPin{carried ? min(lsn, *carried) : lsn, lsn, debt};
    }
  }
  /* A paused repo needs the record that paused it until it is unpaused (#961).  A later record
     that pauses it again (a copy, or a pause replayed twice) moves the pin up to itself. */
  for (const auto &status : statuses) {
    if (status.Paused) {
      Paused[status.RepoId] = TStatusPin{lsn};
    } else {
      Paused.erase(status.RepoId);
    }
  }
  return dropped;
}

optional<TSequenceNumber> TWalRetention::LowestPinnedSeqLocked(const Base::TUuid &repo_id) const {
  auto iter = Repos.find(repo_id);
  if (iter == Repos.end() || iter->second.Pins.empty()) {
    return nullopt;
  }
  return iter->second.Pins.begin()->first;
}

uint64_t TWalRetention::AppendTxn(const string &wire, const vector<TPush> &pushes, const vector<TPop> &pops,
                                  const vector<TStatusChange> &statuses, Disk::TWalReservation *reservation) {
  Disk::TWal *wal = Manager->GetWal();
  assert(wal);
  lock_guard<mutex> lock(Mutex);
  OweWalLocked(wal);
  /* CopyPausesForward rebuilds a pause's sequence number from the update pins; say so if they ever
     disagree with the repo, since a copy carrying the wrong one would be discarded on replay. */
  for (const auto &status : statuses) {
    if (status.Paused && status.Seq != LowestPinnedSeqLocked(status.RepoId)) {
      syslog(LOG_WARNING, "TWalRetention: paused repo's lowest unpromoted update is %ld, but its pins say %ld (#961)",
             static_cast<long>(status.Seq.value_or(EmptyRepoSequenceNumber)),
             static_cast<long>(LowestPinnedSeqLocked(status.RepoId).value_or(EmptyRepoSequenceNumber)));
    }
  }
  /* The pops' pins are the ones PinTxn drops, since nothing changes the pins between here and there
     with Mutex held; so the record repays exactly the room they were owed. */
  const size_t debt_group_bytes = Disk::TWal::GetPromotionGroupBytes(wire.size());
  Disk::TWal::TPromotionDebtChange debt;
  debt.Repaid = GetPopDebtsLocked(pops);
  for (const auto &push : pushes) {
    if (push.Levels) {
      debt.Owed.push_back(Disk::TPromotionDebt{debt_group_bytes, push.Levels});
    }
  }
  const uint64_t lsn = (reservation && *reservation)
      ? wal->Append(*reservation, Disk::TWalRecordType::Txn, wire.data(), wire.size(), 0U, debt)
      : wal->Append(Disk::TWalRecordType::Txn, wire.data(), wire.size(), 0U, debt);
  RepayDroppedLocked(PinTxn(lsn, pushes, pops, statuses, debt_group_bytes), debt.Repaid.size());
  return lsn;
}

uint64_t TWalRetention::AppendPov(const Base::TUuid &pov_id, const string &body, bool keep_headroom) {
  Disk::TWal *wal = Manager->GetWal();
  assert(wal);
  /* Reserve first, so a pov a client asked for can't take the headroom; then the append into the
     reservation can't be refused. */
  Disk::TWalReservation reservation = wal->Reserve(body.size(), keep_headroom ? Disk::TWal::TRoom::KeepHeadroom : Disk::TWal::TRoom::All);
  lock_guard<mutex> lock(Mutex);
  const uint64_t lsn = wal->Append(reservation, Disk::TWalRecordType::Pov, body.data(), body.size());
  Povs[pov_id] = TPovPin{lsn, body};
  return lsn;
}

void TWalRetention::BeginReplay() {
  lock_guard<mutex> lock(Mutex);
  Replaying = true;
  ReplayLsn = 0UL;
}

void TWalRetention::SetReplayLsn(uint64_t lsn, size_t record_bytes) {
  lock_guard<mutex> lock(Mutex);
  ReplayLsn = lsn;
  ReplayRecordBytes = record_bytes;
}

void TWalRetention::NoteWalAttached() {
  lock_guard<mutex> lock(Mutex);
  OweWalLocked(Manager->GetWal());
}

void TWalRetention::EndReplay() {
  lock_guard<mutex> lock(Mutex);
  Replaying = false;
  ReplayLsn = 0UL;
  ReplayRecordBytes = 0UL;
}

bool TWalRetention::NoteReplayedTxn(const vector<TPush> &pushes, const vector<TPop> &pops, const vector<TStatusChange> &statuses) {
  lock_guard<mutex> lock(Mutex);
  /* Before the first replayed record (while recovery restores povs) a transaction comes from no
     record at all, so there is nothing to pin it to. */
  if (!Replaying || !ReplayLsn) {
    return false;
  }
  RepayDroppedLocked(PinTxn(ReplayLsn, pushes, pops, statuses, Disk::TWal::GetPromotionGroupBytes(ReplayRecordBytes)), 0UL);
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
      const auto begin = pins.lower_bound(low_seq), end = pins.upper_bound(high_seq);
      vector<Disk::TPromotionDebt> dropped;
      for (auto pin = begin; pin != end; ++pin) {
        if (pin->second.Debt.Levels) {
          dropped.push_back(pin->second.Debt);
        }
      }
      pins.erase(begin, end);
      /* A root's updates have no promotions ahead, so this repays nothing unless a repo stopped
         being a root's child. */
      RepayDroppedLocked(std::move(dropped), 0UL);
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
  for (const auto &pause : Paused) {
    head = min(head, pause.second.Lsn);
  }
  return head;
}

void TWalRetention::DropDeadPins() {
  vector<Base::TUuid> repo_ids, pov_ids, paused_ids;
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
    paused_ids.reserve(Paused.size());
    for (const auto &pause : Paused) {
      paused_ids.push_back(pause.first);
    }
  }
  /* IsLiveRepo takes the manager's own mutex, so never ask with ours held. */
  vector<Base::TUuid> dead_repos, dead_povs, dead_paused;
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
  for (const auto &id : paused_ids) {
    if (!Manager->IsLiveRepo(id)) {
      dead_paused.push_back(id);
    }
  }
  if (dead_repos.empty() && dead_povs.empty() && dead_paused.empty()) {
    return;
  }
  /* A repo that has gone takes its unpromoted updates with it, and nothing creates a repo again
     under an old id, so its pins can't be needed. */
  lock_guard<mutex> lock(Mutex);
  vector<Disk::TPromotionDebt> dropped;
  for (const auto &id : dead_repos) {
    auto iter = Repos.find(id);
    if (iter == Repos.end()) {
      continue;
    }
    for (const auto &pin : iter->second.Pins) {
      if (pin.second.Debt.Levels) {
        dropped.push_back(pin.second.Debt);
      }
    }
    Repos.erase(iter);
  }
  /* Their promotions will never come, so neither does the room they were owed. */
  RepayDroppedLocked(std::move(dropped), 0UL);
  for (const auto &id : dead_povs) {
    Povs.erase(id);
  }
  for (const auto &id : dead_paused) {
    Paused.erase(id);
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
        uint64_t lsn;
        try {
          lsn = wal->Append(Disk::TWalRecordType::Pov, pov.second.Body.data(), pov.second.Body.size(), Disk::WalRecordFlags::Copied);
        } catch (const Disk::TWalRingFull &) {
          /* No room for more copies this pass (#891); the pins of those not copied stay put, and
             the next pass tries again. */
          break;
        }
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

void TWalRetention::CopyPausesForward(Disk::TWal *wal) {
  const size_t limit = wal->GetCapacityBytes() / 2UL;
  /* repo id, (old lsn, new lsn) */
  vector<pair<Base::TUuid, pair<uint64_t, uint64_t>>> copies;
  uint64_t newest = 0UL;
  /* A transaction applies its changes and appends its record under the replication queue lock,
     so with it held the update pins are what the repos hold, and the copy lands in the record
     stream exactly where the repo is in that state: recovery, replaying up to the copy, has
     rebuilt the repo with that same lowest sequence number, so the copy follows (see
     FollowsStatusChange) and pauses it.  The order is the usual one: the queue lock, then ours,
     then the TWal's. */
  /* lock */ {
    lock_guard<mutex> queue_lock(Manager->GetReplicationQueueLock());
    lock_guard<mutex> lock(Mutex);
    for (const auto &pause : Paused) {
      if (wal->GetRingBytesBehind(pause.second.Lsn) <= limit) {
        continue;
      }
      /* The record a pause itself writes: one pause, carrying the repo's lowest unpromoted
         sequence number (TTransaction::Prepare, TReplicationStreamer::PushTransaction).  An old
         binary reads it as just that. */
      L1::TTransaction::TReplica replica;
      replica.Pause(pause.first, LowestPinnedSeqLocked(pause.first));
      TReplicationStreamer streamer;
      streamer.PushTransaction(replica);
      auto recorder = make_shared<Io::TRecorder>();
      /* serialize */ {
        Io::TBinaryOutputOnlyStream strm(recorder);
        streamer.Write(strm);
        strm.Flush();
      }
      string wire;
      recorder->CopyOut(wire);
      uint64_t lsn;
      try {
        lsn = wal->Append(Disk::TWalRecordType::Txn, wire.data(), wire.size());
      } catch (const Disk::TWalRingFull &) {
        /* As for the Pov records: the pins not moved stay put, and the next pass tries again. */
        break;
      }
      copies.emplace_back(pause.first, make_pair(pause.second.Lsn, lsn));
      newest = lsn;
    }
  }
  if (copies.empty()) {
    return;
  }
  if (!wal->WaitForDurable(newest, chrono::milliseconds(5000))) {
    syslog(LOG_WARNING, "TWalRetention: %ld copied pause records not durable yet; their pins stay put", copies.size());
    return;
  }
  lock_guard<mutex> lock(Mutex);
  for (const auto &copy : copies) {
    /* Unpaused (or paused again) since: that record decides, and the copy changes nothing. */
    auto iter = Paused.find(copy.first);
    if (iter != Paused.end() && iter->second.Lsn == copy.second.first) {
      iter->second.Lsn = copy.second.second;
    }
  }
  syslog(LOG_INFO, "TWalRetention: copied %ld pause records forward", copies.size());
}

void TWalRetention::Checkpoint(uint64_t global_flushed_seq, bool force) {
  Disk::TWal *wal = Manager->GetWal();
  if (!wal) {
    return;
  }
  lock_guard<mutex> checkpoint_lock(CheckpointMutex);
  DropDeadPins();
  CopyPovsForward(wal);
  CopyPausesForward(wal);
  uint64_t head;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    head = ComputeHeadLocked(wal);
  }
  /* After the head, so a record appended since is at or past what the floor returns. */
  if (HeadFloor) {
    head = min(head, HeadFloor());
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

void TWalRetention::SetHeadFloor(std::function<uint64_t ()> floor) {
  HeadFloor = std::move(floor);
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
