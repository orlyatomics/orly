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
#include <atomic>
#include <cassert>
#include <cstring>
#include <sstream>

#include <syslog.h>
#include <uuid/uuid.h>

#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
#include <orly/indy/disk/wal.h>
#include <orly/indy/repo.h>
#include <orly/indy/replication.h>
#include <orly/indy/status.h>
#include <orly/indy/transaction_base.h>

using namespace std;
using namespace Orly::Indy;

/* The RepoCopy body (#963): little-endian fixed-width fields, then each wire with its length. */
namespace {

  void PutU32(string &out, uint32_t val) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<char>((val >> (8 * i)) & 0xFFU));
    }
  }

  void PutU64(string &out, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
      out.push_back(static_cast<char>((val >> (8 * i)) & 0xFFU));
    }
  }

  void PutUuid(string &out, const Base::TUuid &id) {
    const uuid_t &raw = id.GetRaw();
    out.append(reinterpret_cast<const char *>(raw), sizeof(uuid_t));
  }

  class TCopyReader {
    public:

    TCopyReader(const char *data, size_t size) : Data(data), Size(size) {}

    uint8_t U8() {
      Need(1UL);
      return static_cast<uint8_t>(Data[Pos++]);
    }

    uint32_t U32() {
      Need(4UL);
      uint32_t val = 0U;
      for (int i = 0; i < 4; ++i) {
        val |= static_cast<uint32_t>(static_cast<uint8_t>(Data[Pos++])) << (8 * i);
      }
      return val;
    }

    uint64_t U64() {
      Need(8UL);
      uint64_t val = 0UL;
      for (int i = 0; i < 8; ++i) {
        val |= static_cast<uint64_t>(static_cast<uint8_t>(Data[Pos++])) << (8 * i);
      }
      return val;
    }

    Base::TUuid Uuid() {
      uuid_t raw;
      Need(sizeof(raw));
      memcpy(raw, Data + Pos, sizeof(raw));
      Pos += sizeof(raw);
      return Base::TUuid(raw);
    }

    string Bytes(size_t len) {
      Need(len);
      string out(Data + Pos, len);
      Pos += len;
      return out;
    }

    bool AtEnd() const {
      return Pos == Size;
    }

    private:

    void Need(size_t len) const {
      if (len > Size - Pos) {
        throw Disk::TWalCorruptError("WAL RepoCopy record is cut short");
      }
    }

    const char *Data;
    size_t Size;
    size_t Pos = 0UL;
  };

  /* The record ~TTransaction would write for this replica. */
  string SerializeReplica(const L1::TTransaction::TReplica &replica) {
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
    return wire;
  }

  /* Rate-limited: the 1st, 2nd, 4th, 8th... time. */
  bool ShouldLog(atomic<size_t> &count) {
    const size_t n = ++count;
    return (n & (n - 1UL)) == 0UL;
  }

  /* About how big a RepoCopy part grows before the next update starts another. */
  constexpr size_t CopyPartBytes = 1UL << 20;

}  // namespace

string TWalRepoCopyPart::Encode() const {
  string out;
  PutU32(out, Magic);
  out.push_back(static_cast<char>(Version));
  out.push_back(static_cast<char>(Paused ? PausedFlag : 0U));
  PutUuid(out, RepoId);
  PutUuid(out, SetId);
  PutU32(out, Part);
  PutU32(out, Count);
  PutU64(out, Start);
  PutU64(out, Limit);
  PutU64(out, Next);
  PutU32(out, static_cast<uint32_t>(Wires.size()));
  for (const auto &wire : Wires) {
    PutU64(out, wire.size());
    out.append(wire);
  }
  return out;
}

TWalRepoCopyPart TWalRepoCopyPart::Decode(const char *data, size_t size) {
  TCopyReader reader(data, size);
  if (reader.U32() != Magic) {
    throw Disk::TWalCorruptError("WAL RepoCopy record has the wrong magic number");
  }
  const uint8_t version = reader.U8();
  if (version != Version) {
    throw Disk::TWalCorruptError("WAL RepoCopy record has unknown version " + to_string(version));
  }
  TWalRepoCopyPart part;
  part.Paused = (reader.U8() & PausedFlag) != 0U;
  part.RepoId = reader.Uuid();
  part.SetId = reader.Uuid();
  part.Part = reader.U32();
  part.Count = reader.U32();
  part.Start = reader.U64();
  part.Limit = reader.U64();
  part.Next = reader.U64();
  const uint32_t num_wires = reader.U32();
  part.Wires.reserve(num_wires);
  for (uint32_t i = 0U; i < num_wires; ++i) {
    part.Wires.push_back(reader.Bytes(reader.U64()));
  }
  if (!reader.AtEnd() || part.Part >= part.Count) {
    throw Disk::TWalCorruptError("WAL RepoCopy record is malformed");
  }
  return part;
}

TWalRetention::TWalRetention(L1::TManager *manager)
    : Manager(manager) {
  assert(manager);
}

void TWalRetention::AddPinLocked(TRepoPins &repo, TSequenceNumber seq, const TPin &pin, vector<Disk::TPromotionDebt> *repaid) {
  auto [iter, is_new] = repo.Pins.try_emplace(seq, pin);
  if (!is_new) {
    /* Replay pushing an update it had already (the reconcile discards it): the old pin goes. */
    ErasePinsLocked(repo, iter, next(iter), repaid);
    iter = repo.Pins.emplace(seq, pin).first;
  }
  repo.Footprint += GetFootprint(iter->second);
}

void TWalRetention::ErasePinsLocked(TRepoPins &repo, map<TSequenceNumber, TPin>::iterator begin, map<TSequenceNumber, TPin>::iterator end,
                                    vector<Disk::TPromotionDebt> *repaid) {
  for (auto pin = begin; pin != end; ++pin) {
    if (repaid && pin->second.Debt.Levels) {
      repaid->push_back(pin->second.Debt);
      if (pin->second.CopyOwed) {
        repaid->push_back(GetCopyDebt(pin->second));
      }
    }
    repo.Footprint -= min(repo.Footprint, GetFootprint(pin->second));
  }
  repo.Pins.erase(begin, end);
}

size_t TWalRetention::GetPausedFootprintLocked() const {
  size_t footprint = 0UL;
  for (const auto &pause : Paused) {
    auto iter = Repos.find(pause.first);
    if (iter != Repos.end()) {
      footprint += iter->second.Footprint;
    }
  }
  return footprint;
}

void TWalRetention::OweWalLocked(Disk::TWal *wal) {
  if (!wal || DebtWal == wal) {
    return;
  }
  Disk::TWal::TPromotionDebtChange change;
  size_t copies = 0UL;
  for (const auto &repo : Repos) {
    for (const auto &pin : repo.second.Pins) {
      if (pin.second.Debt.Levels) {
        change.Owed.push_back(pin.second.Debt);
        if (pin.second.CopyOwed) {
          change.Owed.push_back(GetCopyDebt(pin.second));
          ++copies;
        }
      }
    }
  }
  wal->ChangePromotionDebt(change);
  DebtWal = wal;
  if (!change.Owed.empty()) {
    syslog(LOG_INFO, "TWalRetention: %ld unpromoted updates are owed %ld KiB of promotion room (%ld of them copy room too, #963)",
           change.Owed.size() - copies, wal->GetPromotionDebtBytes() >> 10, copies);
  }
}

void TWalRetention::RepayLocked(vector<Disk::TPromotionDebt> repaid) {
  if (!DebtWal || repaid.empty()) {
    return;
  }
  Disk::TWal::TPromotionDebtChange change;
  change.Repaid = std::move(repaid);
  DebtWal->ChangePromotionDebt(change);
}

Disk::TWal::TPromotionDebtChange TWalRetention::PlanTxnLocked(const vector<TPush> &pushes, const vector<TPop> &pops,
                                                              const vector<TStatusChange> &statuses, size_t debt_group_bytes,
                                                              bool have_seqs) const {
  Disk::TWal::TPromotionDebtChange change;
  /* Paused once the transaction is in, and before. */
  unordered_map<Base::TUuid, pair<bool, bool>> status_of;
  for (const auto &status : statuses) {
    auto [iter, is_new] = status_of.try_emplace(status.RepoId, Paused.count(status.RepoId) != 0UL, false);
    iter->second.second = status.Paused;
  }
  const auto ends_paused = [&](const Base::TUuid &repo_id) {
    auto iter = status_of.find(repo_id);
    return iter != status_of.end() ? iter->second.second : Paused.count(repo_id) != 0UL;
  };
  const auto repay = [&](const TPin &pin) {
    if (pin.Debt.Levels) {
      change.Repaid.push_back(pin.Debt);
      if (pin.CopyOwed) {
        change.Repaid.push_back(GetCopyDebt(pin));
      }
    }
  };
  /* The pins the transaction drops: by its pops, and by a push replacing one (replay only). */
  unordered_map<Base::TUuid, TSequenceNumber> popped_up_to;
  for (const auto &pop : pops) {
    auto iter = Repos.find(pop.RepoId);
    if (iter == Repos.end()) {
      continue;
    }
    const auto &pins = iter->second.Pins;
    const auto end = pins.upper_bound(pop.Seq);
    for (auto pin = pins.begin(); pin != end; ++pin) {
      repay(pin->second);
    }
    auto &up_to = popped_up_to[pop.RepoId];
    up_to = max(up_to, pop.Seq);
  }
  const auto is_dropped = [&](const Base::TUuid &repo_id, TSequenceNumber seq) {
    auto iter = popped_up_to.find(repo_id);
    return iter != popped_up_to.end() && seq <= iter->second;
  };
  unordered_set<Base::TUuid> pushed_repos;
  for (const auto &push : pushes) {
    if (have_seqs) {
      auto repo = Repos.find(push.RepoId);
      if (repo != Repos.end() && !is_dropped(push.RepoId, push.Seq)) {
        auto pin = repo->second.Pins.find(push.Seq);
        if (pin != repo->second.Pins.end()) {
          repay(pin->second);
        }
      }
    }
    if (push.Levels) {
      change.Owed.push_back(Disk::TPromotionDebt{debt_group_bytes, push.Levels});
      if (ends_paused(push.RepoId)) {
        change.Owed.push_back(Disk::TPromotionDebt{debt_group_bytes, 1UL});
      }
    }
  }
  /* A pause owes the copy room of the updates its repo holds, all at once; an unpause repays it. */
  for (const auto &item : status_of) {
    const bool was_paused = item.second.first, now_paused = item.second.second;
    if (was_paused == now_paused) {
      continue;
    }
    auto repo = Repos.find(item.first);
    if (repo == Repos.end()) {
      continue;
    }
    for (const auto &pin : repo->second.Pins) {
      if (is_dropped(item.first, pin.first) || !pin.second.Debt.Levels) {
        continue;
      }
      if (have_seqs) {
        bool replaced = false;
        for (const auto &push : pushes) {
          replaced = replaced || (push.RepoId == item.first && push.Seq == pin.first);
        }
        if (replaced) {
          continue;
        }
      }
      if (now_paused && !pin.second.CopyOwed) {
        change.Owed.push_back(GetCopyDebt(pin.second));
      } else if (!now_paused && pin.second.CopyOwed) {
        change.Repaid.push_back(GetCopyDebt(pin.second));
      }
    }
  }
  return change;
}

size_t TWalRetention::GetPromotionCredit(const vector<TPop> &pops) {
  lock_guard<mutex> lock(Mutex);
  OweWalLocked(Manager->GetWal());
  size_t credit = 0UL;
  for (const auto &debt : PlanTxnLocked({}, pops, {}, 0UL).Repaid) {
    credit += debt.GroupBytes * debt.Levels;
  }
  return credit;
}

TWalRetention::TRoomPlan TWalRetention::PlanRoom(const vector<TPlannedPush> &pushes, const vector<TPop> &pops,
                                                 const vector<TStatusChange> &statuses, size_t wire_size, bool enforce_cap) {
  Disk::TWal *wal = Manager->GetWal();
  lock_guard<mutex> lock(Mutex);
  OweWalLocked(wal);
  const size_t debt_group_bytes = Disk::TWal::GetPromotionGroupBytes(wire_size);
  vector<TPush> planned;
  planned.reserve(pushes.size());
  for (const auto &push : pushes) {
    planned.push_back(TPush{push.RepoId, 0UL, false, push.Levels});
  }
  const auto change = PlanTxnLocked(planned, pops, statuses, debt_group_bytes, false);
  TRoomPlan plan;
  for (const auto &debt : change.Repaid) {
    plan.Credit += debt.GroupBytes * debt.Levels;
  }
  plan.Owes = change.Owed;
  if (enforce_cap && wal) {
    /* What the transaction adds to the paused repos' hold: its pushes into a repo it leaves paused,
       and the updates of a repo it pauses. */
    size_t added = 0UL;
    unordered_map<Base::TUuid, bool> ends_paused;
    for (const auto &status : statuses) {
      ends_paused[status.RepoId] = status.Paused;
    }
    const auto is_paused = [&](const Base::TUuid &repo_id) {
      auto iter = ends_paused.find(repo_id);
      return iter != ends_paused.end() ? iter->second : Paused.count(repo_id) != 0UL;
    };
    for (const auto &push : pushes) {
      if (push.Levels && is_paused(push.RepoId)) {
        added += debt_group_bytes * (push.Levels + 1UL);
      }
    }
    for (const auto &item : ends_paused) {
      if (item.second && !Paused.count(item.first)) {
        auto repo = Repos.find(item.first);
        if (repo != Repos.end()) {
          added += repo->second.Footprint;
        }
      }
    }
    if (added) {
      const size_t held = GetPausedFootprintLocked();
      const size_t cap = GetPausedCapLocked(wal);
      if (held + added > cap) {
        static atomic<size_t> refusals(0UL);
        if (ShouldLog(refusals)) {
          syslog(LOG_ERR, "WAL: paused repos hold %ld KiB of the ring, as much as they may (%ld KiB); refused a write adding %ld KiB to them (#963); %ld refused so far",
                 held >> 10, cap >> 10, added >> 10, refusals.load());
        }
        throw Disk::TWalPausedFull("the WAL ring's room for paused POVs' unpromoted updates is used up (" + to_string(held >> 10) + " of " +
                                   to_string(cap >> 10) + " KiB); refused a write into a paused POV, and nothing was appended; retry once a paused POV is unpaused or discarded");
      }
    }
  }
  return plan;
}

size_t TWalRetention::GetPausedCapBytes() {
  Disk::TWal *wal = Manager->GetWal();
  lock_guard<mutex> lock(Mutex);
  return GetPausedCapLocked(wal);
}

uint64_t TWalRetention::GetUnpausedFloorLocked() const {
  uint64_t floor = UINT64_MAX;
  for (const auto &[id, repo] : Repos) {
    if (!Paused.contains(id)) {
      for (const auto &pin : repo.Pins) {
        floor = min(floor, pin.second.Lsn);
      }
    }
  }
  return floor;
}

size_t TWalRetention::GetPausedCapLocked(Disk::TWal *wal) const {
  if (PausedCapBytesForTest) {
    return *PausedCapBytesForTest;
  }
  if (!wal) {
    return 0UL;
  }
  const uint64_t floor = GetUnpausedFloorLocked();
  return PausedCapBytes(wal->GetCapacityBytes(), wal->GetHeadroomBytes(), floor == UINT64_MAX ? 0UL : wal->GetRingBytesBehind(floor));
}

size_t TWalRetention::GetPausedFootprintBytes() {
  lock_guard<mutex> lock(Mutex);
  return GetPausedFootprintLocked();
}

void TWalRetention::SetPausedCapBytesForTest(size_t bytes) {
  lock_guard<mutex> lock(Mutex);
  PausedCapBytesForTest = bytes;
}

void TWalRetention::SetIncompatGate(function<void ()> gate) {
  lock_guard<mutex> lock(Mutex);
  IncompatGate = std::move(gate);
}

void TWalRetention::PinTxn(uint64_t lsn, const vector<TPush> &pushes, const vector<TPop> &pops,
                           const vector<TStatusChange> &statuses, size_t debt_group_bytes) {
  /* LSNs start at 1; a pin at 0 would hold the head before the ring's first group for good. */
  assert(lsn);
  if (!lsn) {
    syslog(LOG_ERR, "TWalRetention: refusing to pin LSN 0");
    return;
  }
  unordered_map<Base::TUuid, pair<bool, bool>> status_of;
  for (const auto &status : statuses) {
    auto [iter, is_new] = status_of.try_emplace(status.RepoId, Paused.count(status.RepoId) != 0UL, false);
    iter->second.second = status.Paused;
  }
  const auto ends_paused = [&](const Base::TUuid &repo_id) {
    auto iter = status_of.find(repo_id);
    return iter != status_of.end() ? iter->second.second : Paused.count(repo_id) != 0UL;
  };
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
    ErasePinsLocked(iter->second, pins.begin(), end, nullptr);
    if (pins.empty()) {
      Repos.erase(iter);
    }
  }
  /* The repos whose status changes: their pins owe their copy room, or stop owing it.  Before the
     pushes, which are set as the repo ends up. */
  for (const auto &item : status_of) {
    if (item.second.first == item.second.second) {
      continue;
    }
    auto repo = Repos.find(item.first);
    if (repo == Repos.end()) {
      continue;
    }
    for (auto &pin : repo->second.Pins) {
      pin.second.CopyOwed = item.second.second && pin.second.Debt.Levels;
    }
    if (!item.second.second) {
      repo->second.ReoweAfter.reset();
    }
  }
  for (const auto &push : pushes) {
    auto &repo = Repos[push.RepoId];
    repo.IsRoot = push.IsRoot;
    const Disk::TPromotionDebt debt{push.Levels ? debt_group_bytes : 0UL, push.Levels};
    AddPinLocked(repo, push.Seq, TPin{carried ? min(lsn, *carried) : lsn, lsn, debt, push.Levels && ends_paused(push.RepoId)}, nullptr);
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
  /* Nothing changes the pins between the plan and PinTxn with Mutex held, so the record repays and
     owes exactly what PinTxn does to them. */
  const size_t debt_group_bytes = Disk::TWal::GetPromotionGroupBytes(wire.size());
  const Disk::TWal::TPromotionDebtChange debt = PlanTxnLocked(pushes, pops, statuses, debt_group_bytes);
  const uint64_t lsn = (reservation && *reservation)
      ? wal->Append(*reservation, Disk::TWalRecordType::Txn, wire.data(), wire.size(), 0U, debt)
      : wal->Append(Disk::TWalRecordType::Txn, wire.data(), wire.size(), 0U, debt);
  PinTxn(lsn, pushes, pops, statuses, debt_group_bytes);
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
  PendingPovMoves.erase(pov_id);
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
  const size_t debt_group_bytes = Disk::TWal::GetPromotionGroupBytes(ReplayRecordBytes);
  if (DebtWal) {
    DebtWal->ChangePromotionDebt(PlanTxnLocked(pushes, pops, statuses, debt_group_bytes));
  }
  PinTxn(ReplayLsn, pushes, pops, statuses, debt_group_bytes);
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
      vector<Disk::TPromotionDebt> dropped;
      /* A root's updates have no promotions ahead, so this repays nothing unless a repo stopped
         being a root's child. */
      ErasePinsLocked(iter->second, pins.lower_bound(low_seq), pins.upper_bound(high_seq), &dropped);
      RepayLocked(std::move(dropped));
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
    /* Their promotions will never come, so neither does the room they were owed, nor (if the repo
       was paused) the room for their copy. */
    ErasePinsLocked(iter->second, iter->second.Pins.begin(), iter->second.Pins.end(), &dropped);
    Repos.erase(iter);
    PendingCopies.erase(id);
  }
  RepayLocked(std::move(dropped));
  for (const auto &id : dead_povs) {
    Povs.erase(id);
    PendingPovMoves.erase(id);
  }
  for (const auto &id : dead_paused) {
    Paused.erase(id);
    PendingPauseMoves.erase(id);
    PendingCopies.erase(id);
  }
}

uint64_t TWalRetention::ComputeHead() {
  DropDeadPins();
  lock_guard<mutex> lock(Mutex);
  return ComputeHeadLocked(Manager->GetWal());
}

TWalRetention::TPressure TWalRetention::GetPressure(Disk::TWal *wal) {
  const size_t refusals = wal->GetRingFullRefusals();
  lock_guard<mutex> lock(Mutex);
  if (refusals == SeenRingFullRefusals) {
    return {};
  }
  SeenRingFullRefusals = refusals;
  /* No copy moves the pins of a repo that isn't paused; Pov records, paused repos and pause
     records below them all can be. */
  const uint64_t floor = GetUnpausedFloorLocked();
  const uint64_t head = ComputeHeadLocked(wal);
  if (head >= floor) {
    return {};
  }
  /* Copy only what is more than a sixteenth of the ring behind: what was just written gives back
     nothing for what its copy would take. */
  const size_t sixteenth = wal->GetCapacityBytes() / 16UL;
  const size_t behind = wal->GetRingBytesBehind(head);
  const size_t kept = max(sixteenth, floor == UINT64_MAX ? 0UL : wal->GetRingBytesBehind(floor));
  if (behind <= kept) {
    return {};
  }
  /* What the copies would write: a group of each paused update (its copy room, owed already) and
     the Pov and pause records. */
  size_t cost = 0UL;
  for (const auto &pause : Paused) {
    if (pause.second.Lsn < floor && wal->GetRingBytesBehind(pause.second.Lsn) > sixteenth) {
      cost += Disk::WalAlignment;
    }
    auto repo = Repos.find(pause.first);
    if (repo == Repos.end() || repo->second.Pins.empty()) {
      continue;
    }
    uint64_t oldest = pause.second.Lsn;
    for (const auto &pin : repo->second.Pins) {
      oldest = min(oldest, pin.second.Lsn);
    }
    if (oldest < floor && wal->GetRingBytesBehind(oldest) > sixteenth) {
      for (const auto &pin : repo->second.Pins) {
        cost += GetCopyDebt(pin.second).GroupBytes;
      }
    }
  }
  for (const auto &pov : Povs) {
    if (pov.second.Lsn < floor && wal->GetRingBytesBehind(pov.second.Lsn) > sixteenth) {
      cost += pov.second.Body.size() + Disk::WalAlignment;
    }
  }
  /* Only for a real gain, of a sixteenth of the ring more than the copies take, so copies under
     pressure can't feed on the room they make: once copied, they are less than that behind. */
  if (behind - kept <= sixteenth + cost) {
    return {};
  }
  return TPressure{floor, sixteenth};
}

void TWalRetention::CopyPovsForward(Disk::TWal *wal, const TPressure &pressure) {
  const size_t limit = wal->GetCapacityBytes() / 2UL;
  /* Moves the pins of the copies that are durable now; those that aren't yet stay pending, and
     aren't copied again meanwhile (#963 (c)): a copy slow to land is not appended over and over. */
  const auto finish = [this, wal] {
    const uint64_t durable = wal->GetDurableLsn();
    size_t moved = 0UL;
    for (auto move = PendingPovMoves.begin(); move != PendingPovMoves.end();) {
      if (move->second.NewLsn > durable) {
        ++move;
        continue;
      }
      auto iter = Povs.find(move->first);
      if (iter != Povs.end() && iter->second.Lsn == move->second.OldLsn) {
        iter->second.Lsn = move->second.NewLsn;
        ++moved;
      }
      move = PendingPovMoves.erase(move);
    }
    return moved;
  };
  size_t copied = 0UL;
  uint64_t newest = 0UL;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    finish();
    for (const auto &pov : Povs) {
      if (PendingPovMoves.count(pov.first)) {
        continue;
      }
      if (const size_t behind = wal->GetRingBytesBehind(pov.second.Lsn); behind <= limit && !pressure.Copies(pov.second.Lsn, behind)) {
        continue;
      }
      uint64_t lsn;
      try {
        lsn = wal->Append(Disk::TWalRecordType::Pov, pov.second.Body.data(), pov.second.Body.size(), Disk::WalRecordFlags::Copied);
      } catch (const Disk::TWalRingFull &) {
        /* No room for more copies this pass (#891); the pins of those not copied stay put, and
           the next pass tries again. */
        break;
      }
      PendingPovMoves[pov.first] = TPendingMove{pov.second.Lsn, lsn};
      newest = lsn;
      ++copied;
    }
  }
  if (!copied) {
    return;
  }
  /* The old record holds the head until the copy is durable.  If the ring is too full for the
     copy to land, leave the pins where they are; a later pass moves them once it has. */
  if (!wal->WaitForDurable(newest, chrono::milliseconds(5000))) {
    syslog(LOG_WARNING, "TWalRetention: %ld copied Pov records not durable yet; their pins move once they are", copied);
    return;
  }
  lock_guard<mutex> lock(Mutex);
  syslog(LOG_INFO, "TWalRetention: copied %ld Pov records forward", finish());
}

void TWalRetention::CopyPausesForward(Disk::TWal *wal, const TPressure &pressure) {
  const size_t limit = wal->GetCapacityBytes() / 2UL;
  const auto finish = [this, wal] {
    const uint64_t durable = wal->GetDurableLsn();
    size_t moved = 0UL;
    for (auto move = PendingPauseMoves.begin(); move != PendingPauseMoves.end();) {
      if (move->second.NewLsn > durable) {
        ++move;
        continue;
      }
      /* Unpaused (or paused again) since: that record decides, and the copy changes nothing. */
      auto iter = Paused.find(move->first);
      if (iter != Paused.end() && iter->second.Lsn == move->second.OldLsn) {
        iter->second.Lsn = move->second.NewLsn;
        ++moved;
      }
      move = PendingPauseMoves.erase(move);
    }
    return moved;
  };
  size_t copied = 0UL;
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
    finish();
    for (const auto &pause : Paused) {
      if (PendingPauseMoves.count(pause.first)) {
        continue;
      }
      if (const size_t behind = wal->GetRingBytesBehind(pause.second.Lsn); behind <= limit && !pressure.Copies(pause.second.Lsn, behind)) {
        continue;
      }
      /* Once copies are allowed, a paused repo holding updates is copied whole, its pause with it
         (CopyPausedReposForward). */
      if (RepoCopyAllowed && LowestPinnedSeqLocked(pause.first)) {
        continue;
      }
      /* The record a pause itself writes: one pause, carrying the repo's lowest unpromoted
         sequence number (TTransaction::Prepare, TReplicationStreamer::PushTransaction).  An old
         binary reads it as just that. */
      L1::TTransaction::TReplica replica;
      replica.Pause(pause.first, LowestPinnedSeqLocked(pause.first));
      const string wire = SerializeReplica(replica);
      uint64_t lsn;
      try {
        lsn = wal->Append(Disk::TWalRecordType::Txn, wire.data(), wire.size());
      } catch (const Disk::TWalRingFull &) {
        /* As for the Pov records: the pins not moved stay put, and the next pass tries again. */
        break;
      }
      PendingPauseMoves[pause.first] = TPendingMove{pause.second.Lsn, lsn};
      newest = lsn;
      ++copied;
    }
  }
  if (!copied) {
    return;
  }
  if (!wal->WaitForDurable(newest, chrono::milliseconds(5000))) {
    syslog(LOG_WARNING, "TWalRetention: %ld copied pause records not durable yet; their pins move once they are", copied);
    return;
  }
  lock_guard<mutex> lock(Mutex);
  syslog(LOG_INFO, "TWalRetention: copied %ld pause records forward", finish());
}

void TWalRetention::MoveCopiedPinsLocked(const Base::TUuid &repo_id, const TPendingCopy &copy) {
  auto repo = Repos.find(repo_id);
  if (repo != Repos.end()) {
    for (const auto &[seq, old_lsn] : copy.OldLsns) {
      auto pin = repo->second.Pins.find(seq);
      /* Popped, or pushed again, since: that record decides. */
      if (pin != repo->second.Pins.end() && pin->second.Lsn == old_lsn) {
        pin->second.Lsn = copy.FirstLsn;
      }
    }
    repo->second.ReoweAfter = max(repo->second.ReoweAfter.value_or(0UL), copy.ReoweAfter);
  }
  auto pause = Paused.find(repo_id);
  if (pause != Paused.end() && pause->second.Lsn == copy.OldPauseLsn) {
    pause->second.Lsn = copy.FirstLsn;
  }
}

bool TWalRetention::CopyRepoForward(Disk::TWal *wal, const Base::TUuid &repo_id) {
  static atomic<size_t> refusals(0UL);
  /* TryOpenLiveRepo takes the manager's own mutex, so with nothing of ours held.  Holding the
     pointer keeps the repo live until we're done with it. */
  auto base_repo = Manager->TryOpenLiveRepo(repo_id);
  if (!base_repo) {
    return false;
  }
  auto *repo = dynamic_cast<TRepo *>(base_repo.Get());
  if (!repo) {
    return false;
  }
  TPendingCopy pending;
  /* With the queue lock held no transaction changes the repo or its pins, so the copy holds
     exactly the repo's unpromoted updates as of where it lands in the record stream. */
  /* lock */ {
    lock_guard<mutex> queue_lock(Manager->GetReplicationQueueLock());
    TSequenceNumber start, limit;
    /* lock */ {
      lock_guard<mutex> lock(Mutex);
      auto pins = Repos.find(repo_id);
      auto pause = Paused.find(repo_id);
      if (pins == Repos.end() || pins->second.Pins.empty() || pause == Paused.end() || PendingCopies.count(repo_id)) {
        return false;
      }
      start = pins->second.Pins.begin()->first;
      limit = pins->second.Pins.rbegin()->first;
      if (pins->second.Pins.size() != limit - start + 1UL) {
        if (ShouldLog(refusals)) {
          syslog(LOG_ERR, "TWalRetention: paused repo's pins aren't a run of sequence numbers [%ld, %ld] (%ld pins); not copied (#963)",
                 start, limit, pins->second.Pins.size());
        }
        return false;
      }
      pending.OldPauseLsn = pause->second.Lsn;
      pending.ReoweAfter = pause->second.Lsn;
      for (const auto &[seq, pin] : pins->second.Pins) {
        pending.OldLsns[seq] = pin.Lsn;
        pending.ReoweAfter = max({pending.ReoweAfter, pin.Lsn, pin.TxnLsn});
      }
    }
    const auto &repo_start = repo->GetSequenceNumberStart();
    const auto &repo_limit = repo->GetSequenceNumberLimit();
    if (repo->GetStatus() != Orly::Indy::Paused || repo_start != start || repo_limit != limit) {
      if (ShouldLog(refusals)) {
        syslog(LOG_ERR, "TWalRetention: paused repo holds [%ld, %ld], status %d, but its pins say [%ld, %ld]; not copied (#963)",
               static_cast<long>(repo_start.value_or(0UL)), static_cast<long>(repo_limit.value_or(0UL)), static_cast<int>(repo->GetStatus()),
               start, limit);
      }
      return false;
    }
    /* One transaction for each update, pushing it at its own sequence number (a transaction
       pushes into a repo once), then the pause, at the lowest. */
    vector<string> wires;
    wires.reserve(limit - start + 2UL);
    try {
      for (TSequenceNumber seq = start; seq <= limit; ++seq) {
        const auto update = repo->GetUpdate(seq);
        if (!update) {
          if (ShouldLog(refusals)) {
            syslog(LOG_ERR, "TWalRetention: paused repo has no update %ld in [%ld, %ld]; not copied (#963)", seq, start, limit);
          }
          return false;
        }
        L1::TTransaction::TReplica replica;
        replica.Push(repo_id, update.get())->SetSequenceNumber(seq);
        wires.push_back(SerializeReplica(replica));
      }
    } catch (const bad_alloc &) {
      if (ShouldLog(refusals)) {
        syslog(LOG_ERR, "TWalRetention: no memory to read a paused repo's updates [%ld, %ld]; not copied (#963)", start, limit);
      }
      return false;
    }
    /* pause */ {
      L1::TTransaction::TReplica replica;
      replica.Pause(repo_id, start);
      wires.push_back(SerializeReplica(replica));
    }
    vector<TWalRepoCopyPart> parts;
    size_t part_bytes = 0UL;
    for (auto &wire : wires) {
      if (parts.empty() || (part_bytes && part_bytes + wire.size() > CopyPartBytes)) {
        parts.emplace_back();
        part_bytes = 0UL;
      }
      part_bytes += wire.size();
      parts.back().Wires.push_back(std::move(wire));
    }
    const Base::TUuid set_id(Base::TUuid::Random);
    vector<string> bodies;
    vector<size_t> sizes;
    bodies.reserve(parts.size());
    for (size_t i = 0UL; i < parts.size(); ++i) {
      auto &part = parts[i];
      part.RepoId = repo_id;
      part.SetId = set_id;
      part.Part = static_cast<uint32_t>(i);
      part.Count = static_cast<uint32_t>(parts.size());
      part.Start = start;
      part.Limit = limit;
      part.Next = repo->GetNextSequenceNumber();
      part.Paused = true;
      bodies.push_back(part.Encode());
      sizes.push_back(bodies.back().size());
    }
    lock_guard<mutex> lock(Mutex);
    OweWalLocked(wal);
    auto pins = Repos.find(repo_id);
    if (pins == Repos.end()) {
      return false;
    }
    /* The copy spends the copy room its pins are owed: what was kept so it would have room. */
    vector<Disk::TPromotionDebt> spend;
    vector<TSequenceNumber> spent;
    for (const auto &[seq, pin] : pins->second.Pins) {
      if (pin.CopyOwed) {
        spend.push_back(GetCopyDebt(pin));
        spent.push_back(seq);
      }
    }
    vector<Disk::TWalReservation> reservations;
    try {
      reservations = wal->ReserveGroup(sizes, spend);
    } catch (const Disk::TWalRingFull &ex) {
      if (ShouldLog(refusals)) {
        syslog(LOG_WARNING, "TWalRetention: no room yet to copy a paused repo's %ld updates (%ld parts) forward: %s (#963)",
               limit - start + 1UL, parts.size(), ex.what());
      }
      return false;
    }
    /* ReserveGroup repaid what it spent.  From here, a copy that fails partway owes it again: an
       incomplete set counts for nothing, and the pins stay where they are. */
    for (const auto seq : spent) {
      pins->second.Pins[seq].CopyOwed = false;
    }
    try {
      for (size_t i = 0UL; i < bodies.size(); ++i) {
        const uint64_t lsn = wal->Append(reservations[i], Disk::TWalRecordType::RepoCopy, bodies[i].data(), bodies[i].size());
        if (!i) {
          pending.FirstLsn = lsn;
        }
        pending.LastLsn = lsn;
      }
    } catch (const exception &ex) {
      for (const auto seq : spent) {
        pins->second.Pins[seq].CopyOwed = true;
      }
      Disk::TWal::TPromotionDebtChange change;
      change.Owed = spend;
      wal->ChangePromotionDebt(change);
      syslog(LOG_ERR, "TWalRetention: copying a paused repo forward failed partway: %s (#963)", ex.what());
      return false;
    }
    PendingCopies[repo_id] = pending;
  }
  /* Not holding the queue lock, which WaitWriteAhead takes. */
  if (!wal->WaitForDurable(pending.LastLsn, chrono::milliseconds(5000))) {
    syslog(LOG_WARNING, "TWalRetention: a paused repo's copy (LSN %ld to %ld) is not durable yet; its pins move once it is (#963)",
           pending.FirstLsn, pending.LastLsn);
    return true;
  }
  lock_guard<mutex> lock(Mutex);
  auto iter = PendingCopies.find(repo_id);
  if (iter != PendingCopies.end() && iter->second.LastLsn == pending.LastLsn) {
    MoveCopiedPinsLocked(repo_id, iter->second);
    PendingCopies.erase(iter);
  }
  return true;
}

void TWalRetention::CopyPausedReposForward(Disk::TWal *wal, const TPressure &pressure) {
  const size_t limit = wal->GetCapacityBytes() / 2UL;
  vector<Base::TUuid> candidates;
  function<void ()> gate;
  bool allowed;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    const uint64_t durable = wal->GetDurableLsn();
    for (auto copy = PendingCopies.begin(); copy != PendingCopies.end();) {
      if (copy->second.LastLsn <= durable) {
        MoveCopiedPinsLocked(copy->first, copy->second);
        copy = PendingCopies.erase(copy);
      } else {
        ++copy;
      }
    }
    for (const auto &pause : Paused) {
      auto repo = Repos.find(pause.first);
      if (repo == Repos.end() || repo->second.Pins.empty() || PendingCopies.count(pause.first)) {
        continue;
      }
      uint64_t oldest = pause.second.Lsn;
      for (const auto &pin : repo->second.Pins) {
        oldest = min(oldest, pin.second.Lsn);
      }
      size_t cost = 0UL;
      if (pressure.Restart) {
        for (const auto &pin : repo->second.Pins) {
          cost += GetCopyDebt(pin.second).GroupBytes;
        }
      }
      if (const size_t behind = wal->GetRingBytesBehind(oldest); behind > limit || (pressure.Copies(oldest, behind) && behind > cost)) {
        candidates.push_back(pause.first);
      }
    }
    gate = IncompatGate;
    allowed = RepoCopyAllowed;
  }
  if (candidates.empty()) {
    return;
  }
  if (!allowed) {
    /* Before the first copy: a binary that would skip it must refuse the store instead.  With no
       gate to mark the store, nothing is copied (SetIncompatGate). */
    if (!gate) {
      static atomic<bool> logged(false);
      if (!logged.exchange(true)) {
        syslog(LOG_ERR, "TWalRetention: nothing marks the store as holding paused repos' copies, so none is copied (#963)");
      }
      return;
    }
    try {
      gate();
    } catch (const exception &ex) {
      static atomic<size_t> failures(0UL);
      if (ShouldLog(failures)) {
        syslog(LOG_ERR, "TWalRetention: could not mark the store as holding paused repos' copies, so none is copied: %s (#963)", ex.what());
      }
      return;
    }
    syslog(LOG_NOTICE, "TWalRetention: the store now holds copies of paused repos; a binary from before #963 will refuse it");
    lock_guard<mutex> lock(Mutex);
    RepoCopyAllowed = true;
  }
  size_t copied = 0UL;
  for (const auto &id : candidates) {
    copied += CopyRepoForward(wal, id) ? 1UL : 0UL;
  }
  if (copied) {
    syslog(LOG_INFO, "TWalRetention: copied %ld of %ld paused repos forward (#963)", copied, candidates.size());
  }
}

void TWalRetention::ReoweCopyRoomLocked(uint64_t head) {
  Disk::TWal::TPromotionDebtChange change;
  for (auto &[id, repo] : Repos) {
    if (!repo.ReoweAfter || *repo.ReoweAfter >= head) {
      continue;
    }
    repo.ReoweAfter.reset();
    if (!Paused.count(id)) {
      continue;
    }
    for (auto &[seq, pin] : repo.Pins) {
      if (pin.Debt.Levels && !pin.CopyOwed) {
        pin.CopyOwed = true;
        change.Owed.push_back(GetCopyDebt(pin));
      }
    }
  }
  if (DebtWal && !change.Owed.empty()) {
    DebtWal->ChangePromotionDebt(change);
  }
}

void TWalRetention::Checkpoint(uint64_t global_flushed_seq, bool force) {
  CheckpointWith(global_flushed_seq, force, nullopt);
}

void TWalRetention::CheckpointForRestart(uint64_t global_flushed_seq) {
  Disk::TWal *wal = Manager->GetWal();
  if (!wal) {
    return;
  }
  uint64_t floor;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    floor = GetUnpausedFloorLocked();
  }
  CheckpointWith(global_flushed_seq, true, TPressure{floor, 0UL, true});
}

void TWalRetention::CheckpointWith(uint64_t global_flushed_seq, bool force, const optional<TPressure> &restart) {
  Disk::TWal *wal = Manager->GetWal();
  if (!wal) {
    return;
  }
  lock_guard<mutex> checkpoint_lock(CheckpointMutex);
  DropDeadPins();
  const TPressure pressure = restart ? *restart : GetPressure(wal);
  CopyPovsForward(wal, pressure);
  CopyPausedReposForward(wal, pressure);
  CopyPausesForward(wal, pressure);
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
  if (restart) {
    syslog(LOG_INFO, "TWalRetention: clean-stop checkpoint at head %ld, %ld KiB behind the end (#REOPEN)", head, wal->GetRingBytesBehind(head) / 1024UL);
  }
  if (PayloadProvider) {
    const std::string payload = PayloadProvider(head);
    wal->Checkpoint(head, global_flushed_seq, payload.data(), payload.size());
  } else {
    wal->Checkpoint(head, global_flushed_seq);
  }
  LastCheckpointHead = head;
  /* The ring has the room of the records the head passed back: owe the copies theirs again. */
  lock_guard<mutex> lock(Mutex);
  ReoweCopyRoomLocked(head);
}

TWalRetention::TReplayPlan TWalRetention::PlanReplay(const vector<TReplayRecord> &records) {
  struct TSet {
    vector<optional<TWalRepoCopyPart>> Parts;
    uint64_t FirstLsn = 0UL;
    bool Bad = false;
  };
  unordered_map<Base::TUuid, TSet> sets;
  for (const auto &rec : records) {
    if (rec.Type != Disk::TWalRecordType::RepoCopy) {
      continue;
    }
    auto part = TWalRepoCopyPart::Decode(rec.Data, rec.Size);
    auto &set = sets[part.SetId];
    if (set.Parts.empty()) {
      set.Parts.resize(part.Count);
    }
    const auto &first = find_if(set.Parts.begin(), set.Parts.end(), [](const auto &p) { return p.has_value(); });
    if (set.Parts.size() != part.Count || set.Parts[part.Part] ||
        (first != set.Parts.end() && ((*first)->RepoId != part.RepoId || (*first)->Start != part.Start || (*first)->Limit != part.Limit))) {
      set.Bad = true;
      continue;
    }
    if (!part.Part) {
      set.FirstLsn = rec.Lsn;
    }
    set.Parts[part.Part] = std::move(part);
  }
  /* The newest complete set of each repo. */
  unordered_map<Base::TUuid, TSet *> chosen;
  for (auto &[set_id, set] : sets) {
    if (set.Bad || !all_of(set.Parts.begin(), set.Parts.end(), [](const auto &p) { return p.has_value(); })) {
      continue;
    }
    auto &best = chosen[set.Parts.front()->RepoId];
    if (!best || best->FirstLsn < set.FirstLsn) {
      best = &set;
    }
  }
  TReplayPlan plan;
  for (auto &[repo_id, set] : chosen) {
    auto &parts = plan.Copies[set->FirstLsn];
    for (auto &part : set->Parts) {
      parts.push_back(std::move(*part));
    }
    plan.Skip.insert(repo_id);
  }
  return plan;
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
