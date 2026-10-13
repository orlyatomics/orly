/* <orly/indy/repo.cc>

   Implements <orly/indy/repo.h>.

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

#include <orly/indy/repo.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <thread>

#include <base/debug_log.h>
#include <orly/server/version_unavailable.h>
#include <orly/indy/disk/open_check.h>
#include <orly/indy/disk/util/hash_util.h>
#include <orly/indy/disk/wal.h>
#include <orly/indy/wal_retention.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Util;

class TReader
    : public TReadFile<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::CheckedPage> {
  NO_COPY(TReader);
  public:

  typedef TStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::CheckedPage, 0UL> TInStream;
  typedef Orly::Indy::Disk::TReadFile<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::CheckedPage> TMyReadFile;

  TReader(Disk::Util::TEngine *engine, const Base::TUuid &file_id, DiskPriority priority, size_t gen_id)
      : TMyReadFile(HERE, Source::FileRemoval, engine, file_id, priority, gen_id) {}

  virtual ~TReader() {}

  using TReadFile::GetStartingBlockOffset;
  using TReadFile::GetNumBlocks;
  using TReadFile::GetNumMetaBlocks;
  using TReadFile::GetNumSequentialBlockPairings;
  using TReadFile::GetNumUpdates;

  using TReadFile::FindInHash;
};

/* Counts the updates and entries a memory merge copies out of 'layers' (#607): every update
   past 'bound', or every update when 'all', with its entries. */
static void CountCopy(const std::vector<TMemoryLayer *> &layers, TSequenceNumber bound, bool all,
                      size_t &updates, size_t &entries) {
  for (const TMemoryLayer *layer : layers) {
    for (TMemoryLayer::TUpdateCollection::TCursor csr(layer->GetUpdateCollection()); csr; ++csr) {
      if (all || csr->GetSequenceNumber() > bound) {
        ++updates;
        for (TUpdate::TEntryCollection::TCursor entry(csr->GetEntryCollection()); entry; ++entry) {
          ++entries;
        }
      }
    }
  }
}

/* A Data Layer pool block for the disk layer that a memory merge's file becomes, taken before
   the file is written (#607). Once the file map has the file, the merge can't be undone, so
   recording the file mustn't depend on an allocation that can fail. */
class TDiskLayerSlot {
  NO_COPY(TDiskLayerSlot);
  public:

  TDiskLayerSlot()
      : Mem(TDiskLayer::operator new(sizeof(TDiskLayer))) {}

  ~TDiskLayerSlot() {
    if (Mem) {
      TDiskLayer::operator delete(Mem, sizeof(TDiskLayer));
    }
  }

  /* TDiskLayer's constructor doesn't throw. */
  TDiskLayer *Make(L0::TManager *manager, L0::TManager::TRepo *repo, size_t gen_id, size_t num_keys,
                   TSequenceNumber lowest_seq, TSequenceNumber highest_seq) {
    assert(Mem);
    TDiskLayer *layer = ::new (Mem) TDiskLayer(manager, repo, gen_id, num_keys, lowest_seq, highest_seq);
    Mem = nullptr;
    return layer;
  }

  private:

  void *Mem;

};  // TDiskLayerSlot

void TRepo::LeaseVersion(TSequenceNumber at) {
  if (!GetRetentionVersions() && !GetRetentionSeconds()) {
    return;
  }
  lock_guard<mutex> lock(RetentionLock);
  Leases[at] = chrono::steady_clock::now() + LeaseTtl;
}

void TRepo::SampleNewestLocked(chrono::steady_clock::time_point now, TSequenceNumber newest, chrono::steady_clock::duration window) {
  const auto spacing = max<chrono::steady_clock::duration>(chrono::seconds(1), window / 1024);
  if (NewestSamples.empty() || now - NewestSamples.back().first >= spacing) {
    NewestSamples.emplace_back(now, newest);
  }
}

TSequenceNumber TRepo::GetNewestSequenceNumber() {
  optional<TSequenceNumber> seq_start, seq_limit;
  TSequenceNumber next_seq = 0UL;
  GetSnapshot(seq_start, seq_limit, next_seq);
  return seq_limit ? *seq_limit : (next_seq ? next_seq - 1UL : 0UL);
}

TSequenceNumber TRepo::ResolveTime(chrono::system_clock::time_point when) {
  if (GetParentRepo()) {
    throw invalid_argument("a read at a time is of the global POV only");
  }
  const size_t seconds = GetRetentionSeconds();
  if (!seconds) {
    throw runtime_error("at time needs the retention window to be in seconds: start the server with --retention_seconds");
  }
  const TSequenceNumber newest = GetNewestSequenceNumber();
  const TSequenceNumber oldest = GetHistoryHorizon();
  const auto system_now = chrono::system_clock::now();
  if (when > system_now) {
    throw Server::TTimeUnavailable("that time is in the future", oldest, newest);
  }
  lock_guard<mutex> lock(RetentionLock);
  const auto steady_now = chrono::steady_clock::now();
  /* A sample now, so that a time just before it needn't resolve to one a spacing ago. */
  SampleNewestLocked(steady_now, newest, chrono::seconds(seconds));
  const auto target = steady_now - chrono::duration_cast<chrono::steady_clock::duration>(system_now - when);
  /* The newest sample at or before the time. */
  auto it = upper_bound(NewestSamples.begin(), NewestSamples.end(), target,
                        [](const chrono::steady_clock::time_point &t, const auto &sample) { return t < sample.first; });
  if (it == NewestSamples.begin()) {
    throw Server::TTimeUnavailable("that time is older than the oldest the retention window has samples for", oldest, newest);
  }
  return prev(it)->second;
}

Base::TUuid TRepo::HoldVersion(TSequenceNumber at, chrono::seconds ttl) {
  if (GetParentRepo()) {
    throw invalid_argument("a version is held of the global POV only");
  }
  const TSequenceNumber newest = GetNewestSequenceNumber();
  if (at > newest || at < GetHistoryHorizon()) {
    throw Server::TVersionUnavailable(at, GetHistoryHorizon(), newest);
  }
  const Base::TUuid id(Base::TUuid::Best);
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    if (!GetRetentionVersions() && !GetRetentionSeconds() && SavePoints.empty()) {
      throw runtime_error("hold version needs retention: with none, merges drop history as soon as they can; start the server with --retention_versions or --retention_seconds");
    }
    Holds.emplace(id, make_pair(at, chrono::steady_clock::now() + ttl));
  }  // release Retention lock
  /* A merge that began before the hold may have dropped the version meanwhile. */
  if (at < GetHistoryHorizon()) {
    ReleaseHold(id);
    throw Server::TVersionUnavailable(at, GetHistoryHorizon(), newest);
  }
  return id;
}

bool TRepo::ReleaseHold(const Base::TUuid &id) {
  lock_guard<mutex> lock(RetentionLock);
  const auto it = Holds.find(id);
  if (it == Holds.end()) {
    return false;
  }
  const bool live = it->second.second > chrono::steady_clock::now();
  Holds.erase(it);
  return live;
}

optional<TSequenceNumber> TRepo::GetRetainFrom() {
  const size_t versions = GetRetentionVersions();
  const size_t seconds = GetRetentionSeconds();
  if (GetParentRepo()) {
    return nullopt;
  }
  if (!versions && !seconds) {
    /* #745 stage 3: a save point keeps history with the window off. */
    lock_guard<mutex> lock(RetentionLock);
    if (SavePoints.empty() && Holds.empty()) {
      return nullopt;
    }
  }
  optional<TSequenceNumber> seq_start, seq_limit;
  TSequenceNumber next_seq = 0UL;
  GetSnapshot(seq_start, seq_limit, next_seq);
  const TSequenceNumber newest = next_seq ? next_seq - 1UL : 0UL;
  const auto now = chrono::steady_clock::now();
  TSequenceNumber retain_from = newest;
  lock_guard<mutex> lock(RetentionLock);
  if (versions) {
    retain_from = min(retain_from, (newest >= versions) ? newest - versions + 1UL : 0UL);
  }
  if (seconds) {
    /* Sample the newest version now and then (at most about 1024 samples a window), and start the window at the
       newest sample at least `seconds` old: every version newer than it was newest within the window. Without one
       that old (the window is longer than this process has sampled), keep everything. */
    const auto window = chrono::seconds(seconds);
    SampleNewestLocked(now, newest, window);
    while (NewestSamples.size() > 1UL && NewestSamples[1].first <= now - window) {
      NewestSamples.pop_front();
    }
    retain_from = min(retain_from, (NewestSamples.front().first <= now - window) ? NewestSamples.front().second : 0UL);
  }
  for (auto it = Leases.begin(); it != Leases.end();) {
    it = (it->second <= now) ? Leases.erase(it) : next(it);
  }
  if (!Leases.empty()) {
    retain_from = min(retain_from, Leases.begin()->first);
  }
  for (auto it = Holds.begin(); it != Holds.end();) {
    if (it->second.second <= now) {
      it = Holds.erase(it);
    } else {
      retain_from = min(retain_from, it->second.first);
      ++it;
    }
  }
  for (const auto &save_point : SavePoints) {
    retain_from = min(retain_from, save_point.second);
  }
  return max(retain_from, GetHistoryHorizon());
}

/* #745 stage 3: retention records. */

namespace {

  /* Constants the record ids are derived with: the repo id's bytes, xor these. */
  constexpr uint8_t LocalRecordMask = 0x5AU, ShippedRecordMask = 0xA5U;

  /* What a record starts with, and its layout's version. */
  constexpr uint64_t RecordMagic = 0x3174655279726C4FUL;  // "OrlyRet1", little-endian

  void PutU64(string &out, uint64_t val) {
    char buf[sizeof(val)];
    memcpy(buf, &val, sizeof(val));
    out.append(buf, sizeof(val));
  }

  bool GetU64(const string &in, size_t &pos, uint64_t &val) {
    if (pos > in.size() || in.size() - pos < sizeof(val)) {
      return false;
    }
    memcpy(&val, in.data() + pos, sizeof(val));
    pos += sizeof(val);
    return true;
  }

}  // namespace

Base::TUuid TRepo::GetRetentionRecordId(const Base::TUuid &repo_id, bool shipped) {
  uuid_t raw;
  memcpy(raw, repo_id.GetRaw(), sizeof(raw));
  for (auto &byte : raw) {
    byte ^= shipped ? ShippedRecordMask : LocalRecordMask;
  }
  return Base::TUuid(raw);
}

string TRepo::TRetentionRecord::Encode() const {
  string out;
  PutU64(out, RecordMagic);
  PutU64(out, Horizon);
  PutU64(out, Versions);
  PutU64(out, Seconds);
  PutU64(out, Samples.size());
  for (const auto &[when, seq] : Samples) {
    PutU64(out, static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(when.time_since_epoch()).count()));
    PutU64(out, seq);
  }
  PutU64(out, SavePoints.size());
  for (const auto &[name, seq] : SavePoints) {
    PutU64(out, name.size());
    out.append(name);
    PutU64(out, seq);
  }
  return out;
}

bool TRepo::TRetentionRecord::TryDecode(const string &blob, TRetentionRecord &out) {
  TRetentionRecord rec;
  size_t pos = 0UL;
  uint64_t magic = 0UL, versions = 0UL, seconds = 0UL, count = 0UL;
  if (!GetU64(blob, pos, magic) || magic != RecordMagic || !GetU64(blob, pos, rec.Horizon) || !GetU64(blob, pos, versions) ||
      !GetU64(blob, pos, seconds) || !GetU64(blob, pos, count) || count > blob.size()) {
    return false;
  }
  rec.Versions = versions;
  rec.Seconds = seconds;
  for (uint64_t i = 0UL; i < count; ++i) {
    uint64_t nanos = 0UL, seq = 0UL;
    if (!GetU64(blob, pos, nanos) || !GetU64(blob, pos, seq)) {
      return false;
    }
    rec.Samples.emplace_back(chrono::system_clock::time_point(chrono::duration_cast<chrono::system_clock::duration>(chrono::nanoseconds(static_cast<int64_t>(nanos)))), seq);
  }
  if (!GetU64(blob, pos, count) || count > blob.size()) {
    return false;
  }
  for (uint64_t i = 0UL; i < count; ++i) {
    uint64_t len = 0UL, seq = 0UL;
    if (!GetU64(blob, pos, len) || len > blob.size() - pos) {
      return false;
    }
    string name = blob.substr(pos, len);
    pos += len;
    if (!GetU64(blob, pos, seq)) {
      return false;
    }
    rec.SavePoints[name] = seq;
  }
  if (pos != blob.size()) {
    return false;
  }
  out = move(rec);
  return true;
}

void TRepo::NoteLoadedFromDisk(TSequenceNumber highest) {
  RaiseHistoryHorizon(highest);
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    if (!PersistRecords) {
      return;
    }
    RestartClaim = highest;
  }  // release Retention lock
  /* Reads at older versions wait on the record, so read it now if there is a store to read it from. */
  if (!GetParentRepo()) {
    ResolveRecord();
  }
}

void TRepo::ResolveRecord() {
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    if (RecordResolved || !Manager->HasRetentionStore()) {
      return;
    }
  }
  string blob;
  const bool found = Manager->TryLoadRetentionRecord(GetRetentionRecordId(GetId(), false), blob);
  lock_guard<mutex> lock(RetentionLock);
  if (!RecordResolved) {
    ResolveRecordLocked(found, blob);
  }
}

void TRepo::ResolveRecordLocked(bool found, const string &blob) {
  assert(!RecordResolved);
  RecordResolved = true;
  /* Without a record, what the files say: their newest version for a repo loaded from disk, nothing for a new one. */
  DurableHorizon = RestartClaim ? *RestartClaim : 0UL;
  RecordOn = GetRetentionVersions() || GetRetentionSeconds();
  if (!found) {
    return;
  }
  TRetentionRecord rec;
  if (!TRetentionRecord::TryDecode(blob, rec)) {
    /* Trust the files alone, and write a good record over it. */
    Log(LOG_ERR, "retention", "unreadable retention record; keeping the horizon the files give (#745)");
    RecordOn = true;
    return;
  }
  RecordOn = true;
  DurableHorizon = rec.Horizon;
  SavePoints = rec.SavePoints;
  /* The window's samples, in this process's steady clock. */
  if (NewestSamples.empty()) {
    const auto steady_now = chrono::steady_clock::now();
    const auto system_now = chrono::system_clock::now();
    for (const auto &[when, seq] : rec.Samples) {
      if (when <= system_now) {
        NewestSamples.emplace_back(steady_now - chrono::duration_cast<chrono::steady_clock::duration>(system_now - when), seq);
      }
    }
  }
  /* Every merge before the restart kept every version from the record's horizon on (none passed it, see
     GetMergeRetainFrom), and none since has dropped any (none passes DurableHorizon either, and it was the restart
     claim until now), so the horizon may come down to it. Only from the restart claim: anything else that raised it
     meanwhile (a synced file) must stand. */
  if (RestartClaim && rec.Horizon < *RestartClaim) {
    TSequenceNumber expected = *RestartClaim;
    HistoryHorizon.compare_exchange_strong(expected, rec.Horizon);
  }
}

void TRepo::PollPendingSaveLocked() {
  if (!PendingSem || !PendingSem->Pop()) {
    return;
  }
  delete PendingSem;
  PendingSem = nullptr;
  DurableHorizon = max(DurableHorizon, PendingHorizon);
  NotedStuckSave = false;
  /* On disk here; now the slaves may have it. Fire and forget: a slave only ever holds to less than it. */
  if (Manager->ShipsRetention()) {
    Manager->TrySaveRetentionRecord(GetRetentionRecordId(GetId(), true), PendingBlob, nullptr, true);
  }
  PendingBlob.clear();
}

bool TRepo::StartSaveLocked(TSequenceNumber horizon) {
  assert(!PendingSem);
  TRetentionRecord rec;
  rec.Horizon = horizon;
  rec.Versions = GetRetentionVersions();
  rec.Seconds = GetRetentionSeconds();
  const auto steady_now = chrono::steady_clock::now();
  const auto system_now = chrono::system_clock::now();
  for (const auto &[when, seq] : NewestSamples) {
    rec.Samples.emplace_back(system_now - chrono::duration_cast<chrono::system_clock::duration>(steady_now - when), seq);
  }
  rec.SavePoints = SavePoints;
  string blob = rec.Encode();
  auto *sem = new Base::TEventSemaphore(0UL, true);
  bool saved = false;
  try {
    saved = Manager->TrySaveRetentionRecord(GetRetentionRecordId(GetId(), false), blob, sem, false);
  } catch (...) {
    delete sem;
    throw;
  }
  if (!saved) {
    delete sem;
    return false;
  }
  PendingSem = sem;
  PendingHorizon = horizon;
  PendingBlob = move(blob);
  return true;
}

optional<TSequenceNumber> TRepo::GetMergeRetainFrom() {
  if (GetParentRepo() || !PersistRecords) {
    return GetRetainFrom();
  }
  ResolveRecord();
  /* A slave holds to the horizon its master ships too. Read here, holding no lock. */
  optional<TRetentionRecord> shipped;
  const bool is_slave = !Manager->ShipsRetention();
  if (is_slave) {
    string blob;
    TRetentionRecord rec;
    if (Manager->TryLoadRetentionRecord(GetRetentionRecordId(GetId(), true), blob) && TRetentionRecord::TryDecode(blob, rec)) {
      shipped = move(rec);
    }
  }
  if (shipped) {
    /* Its save points hold this node's history too, and go with it into this node's own record. */
    lock_guard<mutex> lock(RetentionLock);
    SavePoints = shipped->SavePoints;
  }
  const optional<TSequenceNumber> desired = GetRetainFrom();
  optional<TSequenceNumber> seq_start, seq_limit;
  TSequenceNumber next_seq = 0UL;
  GetSnapshot(seq_start, seq_limit, next_seq);
  const TSequenceNumber newest = next_seq ? next_seq - 1UL : 0UL;
  lock_guard<mutex> lock(RetentionLock);
  if (!RecordResolved) {
    /* No store to read the record from yet (the server is still starting): a record may promise anything the files
       hold, so keep all of it. */
    return 0UL;
  }
  if (!RecordOn) {
    return desired;
  }
  PollPendingSaveLocked();
  TSequenceNumber want = desired ? *desired : max(newest, GetHistoryHorizon());
  if (shipped) {
    want = min(want, shipped->Horizon);
  } else if (is_slave) {
    /* Nothing from the master yet: it may promise any version this node holds, so keep them all. */
    want = 0UL;
    if (!NotedNoShipped) {
      NotedNoShipped = true;
      Log(LOG_WARNING, "retention", "no retention record from the master yet; this slave keeps all history until one arrives (#745)");
    }
  }
  /* What a record claims is kept from must never be below the horizon: on a slave, want can be (the shipped horizon
     is below what a synced file raised it to), and a restart would trust the record over the files. */
  const TSequenceNumber save_horizon = max(want, GetHistoryHorizon());
  if (save_horizon > DurableHorizon) {
    if (!PendingSem && !StartSaveLocked(save_horizon) && !NotedStuckSave) {
      NotedStuckSave = true;
      Log(LOG_WARNING, "retention", "cannot save the retention record; merges keep history from the last saved horizon on (#745)");
    }
  }
  const TSequenceNumber retain_from = min(want, DurableHorizon);
  MaxRetainFrom = max(MaxRetainFrom, retain_from);
  return retain_from;
}

void TRepo::SaveRecordAndWait() {
  /* Any save started once the change is in memory carries it; wait for one to reach the disk. */
  bool started = false;
  for (;;) {
    /* acquire Retention lock */ {
      lock_guard<mutex> lock(RetentionLock);
      PollPendingSaveLocked();
      if (!PendingSem) {
        if (started) {
          return;
        }
        if (!StartSaveLocked(max(DurableHorizon, GetHistoryHorizon()))) {
          throw runtime_error("cannot save the retention record (#745)");
        }
        started = true;
      }
    }  // release Retention lock
    this_thread::sleep_for(chrono::milliseconds(1));
  }
}

void TRepo::NoteHistoryHorizonRaised() {
  if (GetParentRepo() || !PersistRecords) {
    return;
  }
  ResolveRecord();
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    PollPendingSaveLocked();
    if (!RecordResolved || !RecordOn || DurableHorizon >= GetHistoryHorizon()) {
      return;
    }
  }  // release Retention lock
  /* A record on disk below the horizon would, after a restart, bring the horizon down over versions that are gone. */
  SaveRecordAndWait();
}

void TRepo::SetSavePoint(const string &name) {
  if (!GetParentRepo()) {
    SetSavePoint(name, GetNewestSequenceNumber());
    return;
  }
  const TSequenceNumber newest = GetNewestSequenceNumber();
  lock_guard<mutex> lock(RetentionLock);
  PovSavePoints[name] = newest;
}

TSequenceNumber TRepo::GetPovSavePoint(const string &name) {
  TSequenceNumber at;
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    const auto it = PovSavePoints.find(name);
    if (it == PovSavePoints.end()) {
      throw runtime_error("no save point named \"" + name + "\" on this POV");
    }
    at = it->second;
  }  // release Retention lock
  optional<TSequenceNumber> seq_start, seq_limit;
  TSequenceNumber next_seq = 0UL;
  GetSnapshot(seq_start, seq_limit, next_seq);
  /* Every write at or below promoted_up_to has left this repo; with none left, all of them have. */
  const TSequenceNumber promoted_up_to = seq_start ? (*seq_start ? *seq_start - 1UL : 0UL) : (next_seq ? next_seq - 1UL : 0UL);
  if (promoted_up_to > at) {
    throw runtime_error("version unavailable: the writes after save point \"" + name + "\" have been promoted, so it can no longer be read");
  }
  return at;
}

void TRepo::SetSavePoint(const string &name, TSequenceNumber at) {
  if (GetParentRepo()) {
    throw invalid_argument("a save point of a POV with a parent is made at its newest version");
  }
  if (!PersistRecords) {
    throw runtime_error("save points need persisted retention on a root repo (#745)");
  }
  if (!Manager->ShipsRetention()) {
    throw runtime_error("save points are set on the master (#745)");
  }
  ResolveRecord();
  optional<TSequenceNumber> seq_start, seq_limit;
  TSequenceNumber next_seq = 0UL;
  GetSnapshot(seq_start, seq_limit, next_seq);
  const TSequenceNumber newest = next_seq ? next_seq - 1UL : 0UL;
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    if (!RecordResolved) {
      throw runtime_error("no retention record store yet (#745)");
    }
    /* A merge may already have dropped what is older than what one was handed, or than the horizon. And the
       record's horizon, saved or being saved, may have gone to the slaves, which may drop what is older than it
       before this save point reaches them; at or above it, every node keeps the version. */
    const TSequenceNumber floor = max({MaxRetainFrom, GetHistoryHorizon(), DurableHorizon, PendingSem ? PendingHorizon : 0UL});
    if (at < floor || at > newest) {
      throw runtime_error("save point version " + to_string(at) + " is outside the versions kept, [" + to_string(floor) + ", " + to_string(newest) + "] (#745)");
    }
    SavePoints[name] = at;
    RecordOn = true;
  }  // release Retention lock
  SaveRecordAndWait();
}

bool TRepo::DeleteSavePoint(const string &name) {
  if (GetParentRepo()) {
    lock_guard<mutex> lock(RetentionLock);
    return PovSavePoints.erase(name) != 0UL;
  }
  /* acquire Retention lock */ {
    lock_guard<mutex> lock(RetentionLock);
    if (!SavePoints.erase(name)) {
      return false;
    }
  }  // release Retention lock
  if (PersistRecords && !GetParentRepo() && Manager->ShipsRetention()) {
    SaveRecordAndWait();
  }
  return true;
}

map<string, TSequenceNumber> TRepo::GetSavePoints() {
  if (GetParentRepo()) {
    lock_guard<mutex> lock(RetentionLock);
    return PovSavePoints;
  }
  if (PersistRecords && !GetParentRepo()) {
    ResolveRecord();
  }
  lock_guard<mutex> lock(RetentionLock);
  return SavePoints;
}

void TRepo::AddImportLayer(TMemoryLayer *mem_layer, Base::TEventSemaphore &sem, Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed) {
  //assert(!ParentRepo);
  TDataLayer *new_layer = nullptr;
  if (IsSafeRepo()) {
    TSequenceNumber lower_seq_bound;
    /* acquire Data lock */ {
      std::lock_guard<std::mutex> lock(DataLock);
      lower_seq_bound = ReleasedUpTo;
    }  // release Data lock
    size_t num_keys = 0UL;
    TSequenceNumber saved_low_seq = 0UL, saved_high_seq = 0UL;
    size_t gen_id = WriteFile(mem_layer, storage_speed, saved_low_seq, saved_high_seq, num_keys, lower_seq_bound);
    /* acquire DataLayer lock */ {
      std::lock_guard<std::mutex> lock(DataLock);
      new_layer = new TDiskLayer(Manager, this, gen_id, num_keys, saved_low_seq, saved_high_seq);
      delete mem_layer;
    }  // release DataLayer lock
  } else {
    new_layer = mem_layer;
  }
  size_t total_layers = 0UL;
  sem.Pop();
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> lock(MappingLock);
    TMapping *cur_mapping = MappingCollection.TryGetLastMember();
    cur_mapping->Incr();
    try {
      TMapping *new_mapping = new TMapping(this);
      assert(cur_mapping);
      for (TMapping::TEntryCollection::TCursor cur_csr(cur_mapping->GetEntryCollection()); cur_csr; ++cur_csr) {
        ++total_layers;
        new TMapping::TEntry(new_mapping, cur_csr->GetLayer());
      }
      new TMapping::TEntry(new_mapping, new_layer);
      ++total_layers;
      cur_mapping->Decr();
    } catch (...) {
      cur_mapping->Decr();
      throw;
    }
  }
  if (ParentRepo && !InTetris) {
    if (auto *tm = Manager->GetTetrisManager()) {
      tm->Join((*ParentRepo)->GetId(), GetId());
      InTetris = true;
    }
  }
  if (total_layers >= 3) {
    EnqueueMergeDisk();
  }
}

void TRepo::AddFileToRepo(size_t gen_id, TSequenceNumber saved_low_seq, TSequenceNumber saved_high_seq, size_t num_keys) {
  TDiskLayer *new_disk = nullptr;
  /* acquire DataLayer lock */ {
    std::lock_guard<std::mutex> lock(DataLock);
    new_disk = new TDiskLayer(Manager, this, gen_id, num_keys, saved_low_seq, saved_high_seq);
  }  // release DataLayer lock
  size_t total_layers = 0UL;
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> lock(MappingLock);
    TMapping *cur_mapping = MappingCollection.TryGetLastMember();
    cur_mapping->Incr();
    try {
      TMapping *new_mapping = new TMapping(this);
      assert(cur_mapping);
      for (TMapping::TEntryCollection::TCursor cur_csr(cur_mapping->GetEntryCollection()); cur_csr; ++cur_csr) {
        ++total_layers;
        new TMapping::TEntry(new_mapping, cur_csr->GetLayer());
      }
      new TMapping::TEntry(new_mapping, new_disk);
      ++total_layers;
      cur_mapping->Decr();
    } catch (...) {
      cur_mapping->Decr();
      throw;
    }
  }
  if (total_layers >= 3) {
    EnqueueMergeDisk();
  }
}

void TRepo::ReleaseDirtyPin() {
  /* Under DataLock, like every other MakeDirty()/RemoveFromDirty(): a merge may be dropping or
     taking the same pin on another runner. */
  std::lock_guard<std::mutex> lock(DataLock);
  RemoveFromDirty();
}

void TRepo::ReleaseUpdate(TSequenceNumber seq_num, bool ensure_or_discard) {
  /* #227: publish ReleasedUpTo UNDER DataLock. StepMergeMem reads it (also
     under DataLock) as the high-water mark below which a child repo DROPS its
     mem entries -- on the contract that everything <= ReleasedUpTo is already
     promoted to the parent. The store was previously done lock-free, so the
     merge could read a racy/half-published ReleasedUpTo and drop child entries
     whose promotion wasn't yet visible, losing ~50% of a concurrent fresh-key
     burst (only surfaced once the merge scheduler fix made StepMergeMem run).
     Serializing it with the seq-number state the merge snapshots closes that
     window. */
  /* Acquire Data lock */ {
    std::lock_guard<std::mutex> lock(DataLock);
    assert(seq_num < NextUpdate);
    assert(seq_num == ReleasedUpTo + 1L || ensure_or_discard);
    if (!ensure_or_discard || seq_num == ReleasedUpTo + 1L) {
      ReleasedUpTo = seq_num;
      /* This repo's memory merge can now drop its copies up to here (#607). Queue it:
         otherwise only this repo's next write queued it, and a merge that ran before this
         release had left them in place. A repo whose backlog Tetris drained kept the released
         copies in the update pools until a write that memory admission might refuse until
         they were freed. Queuing an already queued repo does nothing. */
      EnqueueMergeMem();
    }
    if ((HighestSeqNum && seq_num == *HighestSeqNum) || (!HighestSeqNum && seq_num == NextUpdate - 1)) {
      if (!IsSafeRepo()) {
        RemoveFromDirty();
      }
    }
  }
}

unique_ptr<Indy::TPresentWalker> TRepo::NewPresentWalker(const std::unique_ptr<TView> &view,
                                                         const TIndexKey &from,
                                                         const TIndexKey &to,
                                                         bool ignore_tombstone) {
  assert(view);
  return make_unique<TPresentWalker>(view, from, to, ignore_tombstone);
}

unique_ptr<Indy::TPresentWalker> TRepo::NewPresentWalker(const std::unique_ptr<TView> &view,
                                                         const TIndexKey &key,
                                                         bool ignore_tombstone,
                                                         bool exact_point) {
  assert(view);
  return make_unique<TPresentWalker>(view, key, ignore_tombstone, exact_point);
}

unique_ptr<Indy::TUpdateWalker> TRepo::NewUpdateWalker(const std::unique_ptr<TView> &view,
                                                       TSequenceNumber from,
                                                       const std::optional<TSequenceNumber> &to) {
  assert(view);
  return make_unique<TUpdateWalker>(view, from, to);
}

unique_ptr<Indy::TUpdateWalker> TRepo::NewUpdateWalker(const std::unique_ptr<TView> &view,
                                                       TSequenceNumber from) {
  return NewUpdateWalker(view, from, std::optional<TSequenceNumber>());
}

TRepo::TView::TView(const L0::TManager::TPtr<TRepo> &repo)
    : TView::TView(repo.Get()) {}

TRepo::TView::TView(TRepo *repo)
    : Repo(repo) {
  assert(Repo);
  /* acquire Data lock */ {
    std::lock_guard<std::mutex> lock(Repo->DataLock);
    Snapshot();
  }  // release DataLayer lock
}

TRepo::TView::TView(TRepo *repo, TDataLockHeld)
    : Repo(repo) {
  assert(Repo);
  /* Caller holds Repo->DataLock (#237); snapshot without re-locking. */
  Snapshot();
}

TRepo::TView::TView(TRepo *repo, std::optional<TSequenceNumber> at)
    : Repo(repo), At(at) {
  assert(Repo);
  /* acquire Data lock */ {
    std::lock_guard<std::mutex> lock(Repo->DataLock);
    Snapshot();
  }  // release DataLayer lock
  if (At && UpperBound && *UpperBound > *At) {
    UpperBound = *At;
  }
}

void TRepo::TView::Snapshot() {
  Mapping = Repo->AcquireCurrentMapping();
  CurrentMemoryLayer = Repo->CurMemoryLayer;
  assert(CurrentMemoryLayer);
  CurrentMemoryLayer->Incr();
  LowerBound = Repo->LowestSeqNum;
  UpperBound = Repo->HighestSeqNum;
  Newest = UpperBound;
  NextId = Repo->NextUpdate;
}

TRepo::TView::~TView() {
  assert(CurrentMemoryLayer);
  assert(Repo);
  std::lock_guard<std::mutex> lock(Repo->MappingLock);
  CurrentMemoryLayer->Decr();
  Mapping->Decr();
}

const TMemoryLayer *TRepo::TView::GetCurMem() const noexcept {
  return CurrentMemoryLayer;
}

const TRepo::TMapping *TRepo::TView::GetMapping() const noexcept {
  return Mapping;
}

const std::optional<TSequenceNumber> &TRepo::TView::GetLower() const noexcept {
  return LowerBound;
}

const std::optional<TSequenceNumber> &TRepo::TView::GetUpper() const noexcept {
  return UpperBound;
}

TRepo::TRepo(L0::TManager *manager,
             const TUuid &repo_id,
             const TTtl &ttl,
             const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo)
    : L0::TManager::TRepo(manager, repo_id, ttl, Normal),
      CurMemoryLayer(new TMemoryLayer(manager)),
      ParentRepo(parent_repo),
      NextUpdate(1U),
      ReleasedUpTo(0U),
      InTetris(false),
      PromotionHoldCount(0UL),
      PauseCount(0UL) {
  PersistRecords = manager->IsRetentionPersisted();
  try {
    /* acquire Mapping lock */ {
      std::lock_guard<std::mutex> lock(MappingLock);
      new TMapping(this);
    }  // release Mapping lock
  } catch (...) {
    delete CurMemoryLayer;
    throw;
  }
}

TRepo::TRepo(L0::TManager *manager,
             const Base::TUuid &repo_id,
             const TTtl &ttl,
             const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
             const std::optional<TSequenceNumber> &lowest,
             const std::optional<TSequenceNumber> &highest,
             TSequenceNumber next_update,
             TStatus status)
    : L0::TManager::TRepo(manager, repo_id, ttl, status),
      CurMemoryLayer(new TMemoryLayer(manager)),
      ParentRepo(parent_repo),
      LowestSeqNum(lowest),
      HighestSeqNum(highest),
      NextUpdate(next_update),
      ReleasedUpTo(lowest ? *lowest : 0UL),
      InTetris(false),
      PromotionHoldCount(0UL),
      PauseCount(0UL) {
  /* No entry counts for a backlog loaded from disk (#628). */
  BacklogEntriesKnown = !lowest;
  PersistRecords = manager->IsRetentionPersisted();
  try {
    /* acquire Mapping lock */ {
      std::lock_guard<std::mutex> lock(MappingLock);
      new TMapping(this);
    }  // release Mapping lock
  } catch (...) {
    delete CurMemoryLayer;
    throw;
  }
  /* #769: a backlog loaded from disk counts toward the unpromoted total. Only once nothing
     above can throw: a constructor that throws runs no destructor to take it back out. */
  RecountBacklog();
}

TRepo::~TRepo() {
  /* A repo that dies with updates still in its current memory layer is
     normally a lifecycle bug -- somebody destroyed it before its data was
     merged away.  The exception is a sanctioned discard (#521): when the
     manager drops an expired or torn-down pov's repo, dying with unmerged
     data is the ttl contract working -- e.g. a ttl=0 pov whose owner
     disconnected while its last write was parked on a replication stall.
     Log the drop so the data loss stays visible in release builds too. */
  assert(CurMemoryLayer->IsEmpty() || IsDiscardSanctioned());
  if (!CurMemoryLayer->IsEmpty()) {
    Log(LOG_INFO, "discarding", "dropping unmerged updates still in the memory layer of an expired pov (#521)");
  }
  /* A repo joined to Tetris is pinned by its player's TChild ptr, so reaching
     this destructor with InTetris set means the players -- and possibly the
     whole TetrisManager -- are already gone (server teardown deletes the
     TetrisManager before the repo manager); there is no player left to Part
     from, and calling into the manager here would touch freed memory.
     Outside a sanctioned discard, that state is still a lifecycle bug. */
  assert(!InTetris || IsDiscardSanctioned());
  /* #769: whatever backlog dies with us is no longer anyone's to promote. */
  UnpromotedUpdates -= CountedBacklogUpdates;
  UnpromotedEntries -= CountedBacklogEntries;
  DroppedUpdates += CountedBacklogUpdates;
  /* #691: not marked for delete, so the file stays in the file map. */
  delete UnpublishedMergeDisk;
  delete CurMemoryLayer;
  /* #745 stage 3: a retention record save still in flight holds its sem, which the durable writer pushes once the
     record is on disk. If it hasn't yet, the sem must outlive this repo: leave it (one eventfd, at teardown). */
  if (PendingSem && PendingSem->Pop()) {
    delete PendingSem;
  }
}

std::atomic<size_t> TRepo::PeakBacklogEntries {0UL};
std::atomic<size_t> TRepo::PeakBacklogUpdates {0UL};
std::atomic<size_t> TRepo::UnpromotedUpdates {0UL};
std::atomic<size_t> TRepo::UnpromotedEntries {0UL};
std::atomic<size_t> TRepo::PromotedUpdates {0UL};
std::atomic<size_t> TRepo::DroppedUpdates {0UL};

void TRepo::RecountBacklog() noexcept {
  if (!ParentRepo) {
    return;
  }
  const size_t updates = (LowestSeqNum && HighestSeqNum && *HighestSeqNum >= *LowestSeqNum)
      ? static_cast<size_t>(*HighestSeqNum - *LowestSeqNum) + 1UL : 0UL;
  const size_t entries = BacklogEntriesKnown ? BacklogEntries : 0UL;
  /* Unsigned and modular, so adding the difference either way keeps the sum exact. */
  UnpromotedUpdates += updates - CountedBacklogUpdates;
  UnpromotedEntries += entries - CountedBacklogEntries;
  CountedBacklogUpdates = updates;
  CountedBacklogEntries = entries;
}

TRepo::TBacklogReservation::~TBacklogReservation() {
  if (!Reserved && !OversizedWaiting) {
    return;
  }
  std::lock_guard<std::mutex> lock(Repo->DataLock);
  if (Reserved) {
    assert(Repo->ReservedBacklogUpdates >= 1UL);
    assert(Repo->ReservedBacklogEntries >= NumEntries);
    --Repo->ReservedBacklogUpdates;
    Repo->ReservedBacklogEntries -= NumEntries;
  }
  if (OversizedWaiting) {
    assert(Repo->OversizedBacklogWaiters);
    --Repo->OversizedBacklogWaiters;
  }
}

bool TRepo::TBacklogReservation::TryReserve(size_t update_cap, size_t entry_cap, size_t num_entries) {
  assert(!Reserved);
  std::lock_guard<std::mutex> lock(Repo->DataLock);
  size_t backlog = 0UL;
  if (Repo->LowestSeqNum && Repo->HighestSeqNum && *Repo->HighestSeqNum >= *Repo->LowestSeqNum) {
    backlog = static_cast<size_t>(*Repo->HighestSeqNum - *Repo->LowestSeqNum) + 1UL;
  }
  const size_t updates = backlog + Repo->ReservedBacklogUpdates;
  /* While the entry counts are unknown (see BacklogEntriesKnown), the cap in updates still holds. */
  const size_t entries = (Repo->BacklogEntriesKnown ? Repo->BacklogEntries : 0UL) + Repo->ReservedBacklogEntries;
  const bool oversized = num_entries > entry_cap;
  const bool fits = updates < update_cap
      && (oversized ? !entries : entries + num_entries <= entry_cap && !Repo->OversizedBacklogWaiters);
  if (!fits) {
    /* Read under the same lock as the backlog: AppendUpdate joins the parent's Tetris and
       PopLowest parts from it under DataLock too. */
    Undrainable = !Repo->ParentRepo || (Repo->GetStatus() == Normal && !Repo->InTetris && backlog);
    if (oversized && !OversizedWaiting) {
      OversizedWaiting = true;
      ++Repo->OversizedBacklogWaiters;
    }
    return false;
  }
  if (OversizedWaiting) {
    OversizedWaiting = false;
    --Repo->OversizedBacklogWaiters;
  }
  ++Repo->ReservedBacklogUpdates;
  Repo->ReservedBacklogEntries += num_entries;
  NumEntries = num_entries;
  Reserved = true;
  return true;
}

void TRepo::AddForkWatch(const std::shared_ptr<TForkWatch> &watch) {
  assert(watch);
  std::lock_guard<std::mutex> lock(DataLock);
  ForkWatches.push_back(watch);
}

void TRepo::SetOwnForkWatch(const std::shared_ptr<TForkWatch> &watch) {
  std::lock_guard<std::mutex> lock(OwnForkWatchLock);
  OwnForkWatch = watch;
}

std::shared_ptr<TForkWatch> TRepo::GetOwnForkWatch() const {
  std::lock_guard<std::mutex> lock(OwnForkWatchLock);
  return OwnForkWatch;
}

void TRepo::JoinTetris() {
  std::lock_guard<std::mutex> lock(DataLock);
  if (LowestSeqNum && ParentRepo && Status == Normal && !InTetris) {
    if (auto *tm = Manager->GetTetrisManager()) {
      try {
        tm->Join((*ParentRepo)->GetId(), GetId());
        InTetris = true;
      } catch (const std::exception &ex) {
        syslog(LOG_ERR, "JoinTetris: deferred Tetris join under memory pressure: %s", ex.what());
      }
    }
  }
}

std::optional<TSequenceNumber> TRepo::AppendUpdate(TUpdate *update, TSequenceNumber &next_update, const Base::TUuid &promoted_from) noexcept {
  std::optional<TSequenceNumber> new_seq;
  /* acquire Data lock */ {
    std::lock_guard<std::mutex> lock(DataLock);
    HighestSeqNum = NextUpdate;
    ++NextUpdate;
    next_update = NextUpdate;
    if (!LowestSeqNum) {
      LowestSeqNum = *HighestSeqNum;
    }
    new_seq = HighestSeqNum;

    update->SetSequenceNumber(*new_seq);
    assert(CurMemoryLayer);
    bool was_empty = CurMemoryLayer->IsEmpty();
    const size_t size_before = CurMemoryLayer->GetSize();
    CurMemoryLayer->Insert(update);
    /* #857: count the update's logical bytes (key plus value bytes). */
    AddLogicalBytes(update->ComputeLogicalBytes());
    /* #628: count the update's entries for the writer backpressure. Only a child's backlog is
       ever popped, so a root doesn't keep counts. */
    if (ParentRepo && BacklogEntriesKnown) {
      const size_t num_entries = CurMemoryLayer->GetSize() - size_before;
      try {
        BacklogEntryCounts.push_back(static_cast<uint32_t>(std::min<size_t>(num_entries, std::numeric_limits<uint32_t>::max())));
        BacklogEntries += num_entries;
        /* #721: the reporting port's peak backlog. */
        const size_t depth = static_cast<size_t>(*HighestSeqNum - *LowestSeqNum) + 1UL;
        for (size_t peak = PeakBacklogEntries.load(); BacklogEntries > peak && !PeakBacklogEntries.compare_exchange_weak(peak, BacklogEntries);) {}
        for (size_t peak = PeakBacklogUpdates.load(); depth > peak && !PeakBacklogUpdates.compare_exchange_weak(peak, depth);) {}
      } catch (const std::bad_alloc &) {
        /* NO_THROW: stop counting until the backlog drains; the cap in updates still holds. */
        BacklogEntriesKnown = false;
      }
    }
    /* #746: show the update to the fork watches of the POVs under review below us, under
       DataLock, so each sees our changes in our order.  Forget the watches that have gone. */
    if (!ForkWatches.empty()) {
      std::erase_if(ForkWatches, [update, &promoted_from](const std::weak_ptr<TForkWatch> &weak) {
        auto watch = weak.lock();
        if (!watch) {
          return true;
        }
        watch->OnAppend(*update, promoted_from);
        return false;
      });
    }
    RecountBacklog();
    if (was_empty) {
      EnqueueMergeMem();
    }
    MakeDirty();
    /* Join the parent's Tetris player so this child's updates get promoted.
       This is the one allocating call reachable from the noexcept commit path
       (~TPusher -> AppendUpdate): TTetrisManager::Join does a std::map::insert
       and may construct a new player. Under memory pressure that throws
       bad_alloc; letting it escape a noexcept function terminates the whole
       server (#250). The update is already durable in CurMemoryLayer above
       (reads see it), so on failure we log and leave InTetris false -- the
       child→parent promotion-join is merely deferred and the next AppendUpdate
       to this repo retries it, rather than crashing the process.

       Gating on !InTetris (instead of the old "first update only" flag)
       preserves the success-path semantics -- a repo joins once while it has
       un-promoted updates and re-joins after PopLowest drains and Parts it --
       and additionally retries a previously-failed join. */
    if (ParentRepo && Status == Normal && !InTetris) {
      if (auto *tm = Manager->GetTetrisManager()) {
        try {
          tm->Join((*ParentRepo)->GetId(), GetId());
          InTetris = true;
        } catch (const std::exception &ex) {
          syslog(LOG_ERR, "AppendUpdate: deferred Tetris join under memory pressure: %s", ex.what());
        }
      }
    }
  }  // release Data lock
  return new_seq;
}

std::optional<TSequenceNumber> TRepo::PopLowest(TSequenceNumber &next_update) noexcept {
  /* Tetris pops a Normal repo; a discard pops a paused one, which has left its parent's Tetris
     (#746). */
  assert(Status != Failed);
  assert(LowestSeqNum);
  assert(HighestSeqNum);
  std::optional<TSequenceNumber> popped_seq;
  /* acquire Data lock */ {
    std::lock_guard<std::mutex> lock(DataLock);
    popped_seq = LowestSeqNum;
    next_update = NextUpdate;
    /* #628 */
    if (BacklogEntriesKnown && !BacklogEntryCounts.empty()) {
      assert(BacklogEntries >= BacklogEntryCounts.front());
      BacklogEntries -= BacklogEntryCounts.front();
      BacklogEntryCounts.pop_front();
    }
    if (*LowestSeqNum < *HighestSeqNum) {
      ++(*LowestSeqNum);
    } else {
      LowestSeqNum.reset();
      HighestSeqNum.reset();
      /* Drained: whatever was unknown is gone with it (#628). */
      BacklogEntryCounts.clear();
      BacklogEntries = 0UL;
      BacklogEntriesKnown = true;
    }
    /* #769: before the Part below, so whoever sees no player left also sees the count drop. */
    RecountBacklog();
    ++PromotedUpdates;
    if (!LowestSeqNum) {
      if (ParentRepo && InTetris) {
        if (auto *tm = Manager->GetTetrisManager()) {
          tm->Part((*ParentRepo)->GetId(), GetId());
        }
        InTetris = false;
      }
    }
  }  // release Data lock
  return popped_seq;
}

std::shared_ptr<TUpdate> TRepo::GetLowestUpdate() {
  /* Hold DataLock across the whole read. The memtable walk below reads the
     repo's in-memory update-list linkage, which AppendUpdate (TCollection
     insert) and PopLowest mutate under DataLock; without sharing the lock the
     walk races the insert (#237 -- the race the #234 commutative fast-lane
     amplifies by promoting many children per round). The TView is built with
     the DataLockHeld tag so it snapshots without re-locking the non-recursive
     DataLock; no walker machinery re-acquires DataLock, and ~TView /
     AcquireCurrentMapping take only MappingLock, preserving the established
     DataLock->MappingLock order, so this cannot deadlock. */
  std::lock_guard<std::mutex> lock(DataLock);
  assert(LowestSeqNum);
  assert(HighestSeqNum);
  auto view = make_unique<TRepo::TView>(this, TView::TDataLockHeld{});
  auto walker_ptr = NewUpdateWalker(view, *LowestSeqNum, *LowestSeqNum);
  auto &walker = *walker_ptr;
  assert(walker);
  if (walker) {
    const TUpdateWalker::TItem &item = *walker;
    /* Split entries by mutator: Assign-tagged entries go through the
       TOpByKey ctor (existing semantics); non-Assign entries go in as
       `more`, each under its own mutator, so the mutator is preserved. This is
       the #49 phase-2 plumbing on the Tetris-merge / GetLowestUpdate
       path -- without it, deferred Add entries get rebuilt as Assigns
       and concurrent `+= n` loses updates again. */
    TUpdate::TOpByKey op_by_key;
    std::vector<TUpdate::TNewEntry> more;
    for (const auto &iter : item.EntryVec) {
      if (iter.Mutator == TMutator::Assign) {
        op_by_key.insert(make_pair(iter.IndexKey, TKey(iter.Op, item.MainArena)));
      } else {
        more.emplace_back(iter.IndexKey, TKey(iter.Op, item.MainArena), iter.Mutator);
      }
    }
    /* Claim the copy before making it (#607; see TPool::TryClaim). */
    TUpdate::TCopyClaim claim;
    if (!claim.TryAcquire(1UL, item.EntryVec.size())) {
      throw std::bad_alloc();
    }
    return TUpdate::NewUpdate(op_by_key, more, TKey(item.Metadata, item.MainArena), TKey(item.Id, item.MainArena));
  }
  return nullptr;
}

std::optional<TSequenceNumber> TRepo::ChangeStatus(TStatus status, TSequenceNumber &next_update) noexcept {
  switch (Status) {
    case Normal : {
      assert(status != Normal);  // you probably meant to unpause something. if it's not already paused you may have forgotten your pause!
      Status = status;
      break;
    }
    case Paused : {
      assert(status == Normal);  // you probably didn't meant to pause an already paused repo
      Status = status;
      break;
    }
    case Failed : {
      assert(status == Failed);
      break;
    }
  }
  /* Tetris */
  switch (Status) {
    case Normal : {
      /* Rejoin the parent's Tetris if, and only if, we have updates it hasn't promoted: the
         writes made while we were paused.  AppendUpdate doesn't join a paused repo, so nobody
         else will wake Tetris for them.  This test used to be inverted (`!LowestSeqNum`, from
         2014), which stranded those writes until the next write joined us, and joined an empty
         repo for nothing (#635).  An empty repo joins on its next AppendUpdate, as always. */
      std::lock_guard<std::mutex> lock(DataLock);
      if (LowestSeqNum && ParentRepo && !InTetris) {
        /* As in AppendUpdate (#250): Join may allocate, and we're on the NO_THROW commit path.
           On failure stay out of Tetris; the next AppendUpdate retries the join. */
        if (auto *tm = Manager->GetTetrisManager()) {
          try {
            tm->Join((*ParentRepo)->GetId(), GetId());
            InTetris = true;
          } catch (const std::exception &ex) {
            syslog(LOG_ERR, "ChangeStatus: deferred Tetris join under memory pressure: %s", ex.what());
          }
        }
      }
      break;
    }
    case Paused : {
      if (ParentRepo && InTetris) {
        if (auto *tm = Manager->GetTetrisManager()) {
          tm->Part((*ParentRepo)->GetId(), GetId());
        }
        InTetris = false;
      }
      break;
    }
    case Failed : {
      if (ParentRepo && InTetris) {
        if (auto *tm = Manager->GetTetrisManager()) {
          tm->Part((*ParentRepo)->GetId(), GetId());
        }
        InTetris = false;
      }
      break;
    }
  }
  next_update = NextUpdate;
  return LowestSeqNum;
}

class TUpdateSortComparator {
  public:

  TUpdateSortComparator() {}

  bool operator()(const TUpdate *lhs, const TUpdate *rhs) const {
    assert(lhs);
    assert(rhs);
    return lhs->GetSequenceNumber() < rhs->GetSequenceNumber();
  }

};  // TUpdateSortComparator

bool TRepo::HasDiskMergeCandidate(TMapping *mapping) const {
  assert(mapping);
  TDataLayer *prev = nullptr;
  for (TMapping::TEntryCollection::TCursor csr(mapping->GetEntryCollection()); csr; ++csr) {
    TDataLayer *layer = csr->GetLayer();
    if (prev
        && prev->GetKind() == TDataLayer::TKind::Disk
        && layer->GetKind() == TDataLayer::TKind::Disk
        && Disk::Util::SuggestGeneration(prev->GetSize()) <= Disk::Util::SuggestGeneration(layer->GetSize())) {
      return true;
    }
    prev = layer;
  }
  return false;
}

/* Merges back off and retry about once a second while the disk stays full, so let at most one
   line about it through every 10s. */
static bool ShouldLogDiskFullMerge() {
  static std::atomic<int64_t> next_ns(0);
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t next = next_ns.load();
  return now_ns >= next && next_ns.compare_exchange_strong(next, now_ns + 10'000'000'000L);
}

std::chrono::milliseconds TRepo::NextDiskFullBackoff(std::atomic<size_t> &streak, const char *merge_kind, const char *err, const char *what_ran_out) {
  /* 100ms, doubling to 3.2s. The merge queues are ordered by due time, so a repo backing off
     holds up no other repo's merges. */
  constexpr size_t max_shift = 5UL;
  const size_t prev = streak++;
  const size_t shift = std::min(prev, max_shift);
  const std::chrono::milliseconds backoff(100L << shift);
  if (ShouldLogDiskFullMerge()) {
    syslog(LOG_ERR, "%s out of %s [%s] (try %ld); inputs handed back, retrying in %ldms", merge_kind, what_ran_out, err, prev + 1UL, static_cast<long>(backoff.count()));
  }
  return backoff;
}

void TRepo::EndDiskFullStreak(std::atomic<size_t> &streak, const char *merge_kind) {
  const size_t retries = streak.exchange(0UL);
  if (retries && ShouldLogDiskFullMerge()) {
    syslog(LOG_ERR, "%s succeeded after %ld retries for disk or pool space", merge_kind, retries);
  }
}

/* Test-only; empty in production. See repo.h. */
std::function<void (TRepo *)> TRepo::OnMergeMemSealedForTest;

atomic<size_t> TRepo::ReadPrepFrameBudget{numeric_limits<size_t>::max()};

atomic<size_t> TRepo::ReadPrepFramesInUse{0UL};

void TRepo::StepMergeMem() {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;
  /* #584: set while a failure would leave the repo exactly as we found it, so
     that running out of pool space retries the merge later instead of
     aborting the server. That covers the in-memory copy phase: until the root
     writes a file or the new mapping is published, the merge has only built
     copies, and the layers it is merging are untouched. */
  bool can_retry = true;
  /* #691: finish a merge whose publish is still waiting before starting another. */ {
    std::lock_guard<std::mutex> lock(MemMergeLock);
    if (UnpublishedMergeDisk) {
      try {
        PublishMemMerge(UnpublishedMergeLayers, UnpublishedMergeDisk);
      } catch (const std::bad_alloc &) {
        DeferUnpublishedMemMerge();
        return;
      }
      if (UnpublishedMergeTries > 1UL) {
        syslog(LOG_ERR, "StepMergeMem: published the file it wrote after %ld tries", UnpublishedMergeTries);
      }
      UnpublishedMergeDisk = nullptr;
      UnpublishedMergeLayers.clear();
      UnpublishedMergeTries = 0UL;
      /* The merge below may find nothing to do and skip this. */
      CheckRemoveDirty();
    }
  }
  try {
    /*** If the current memory layer is not empty, add it to the mapping layer and create a new current memory layer ***/
    /* acquire DataLayer lock */ {
      std::lock_guard<std::mutex> lock(DataLock);
      assert(CurMemoryLayer);
      if (!CurMemoryLayer->IsEmpty()) {
        /* Allocate the replacement first: if that fails, nothing has changed
           yet (the old code had already retired CurMemoryLayer into the
           mapping and left the repo with none). */
        TMemoryLayer *const next_mem = new TMemoryLayer(Manager);
        /* AddMapping undoes itself if it fails (#607), so this stays retryable. */
        try {
          AddMapping(CurMemoryLayer);
        } catch (...) {
          delete next_mem;
          throw;
        }
        CurMemoryLayer = next_mem;
        //EnqueueMergeMem();
      }
    }  // release DataLayer lock
    if (OnMergeMemSealedForTest) {
      OnMergeMemSealedForTest(this);
    }

    /*** find the memory layers that we can merge and then flush to disk if we're a safe repo ***/

    /* acquire MemMerge lock */ {
      std::lock_guard<std::mutex> lock(MemMergeLock);

      /* grab the current mapping */ {
        TMergeHold hold(this);
        TMapping *mapping = hold.GetMapping();
        TMemoryLayer *new_mem = nullptr;
        TDiskLayer *new_disk = nullptr;
        TSequenceNumber lower_seq_bound;
        /* acquire DataLayer lock */ {
          std::lock_guard<std::mutex> lock(DataLock);
          lower_seq_bound = ReleasedUpTo;
          assert(lower_seq_bound < NextUpdate);
          new_mem = new TMemoryLayer(Manager);
        }  // release DataLayer lock
        assert(new_mem);
        std::vector<TMemoryLayer *> mem_to_merge_vec;
        /* A half-written file can't be rolled back (#584), except when it fails for lack of disk
           space: TDataFile throws TDiskFull before the file reaches the file map, having freed
           its blocks and left the updates' persistence notifications pending, so the layers can
           go back and the merge retry (#590). */
        auto write_file = [&](TMemoryLayer *layer, TSequenceNumber &saved_low_seq, TSequenceNumber &saved_high_seq, size_t &num_keys) {
          can_retry = false;
          try {
            return WriteFile(layer, storage_speed, saved_low_seq, saved_high_seq, num_keys, lower_seq_bound);
          } catch (const Disk::Util::TDiskFull &) {
            can_retry = true;
            throw;
          } catch (const Disk::TDataFileAllocFailed &) {
            /* Same as running out of disk, for memory (#607). */
            can_retry = true;
            throw;
          }
        };
        try {
          for (TMapping::TEntryCollection::TCursor csr(mapping->GetEntryCollection(), InvCon::TOrient::Rev); csr; ++csr) {
            TDataLayer *layer = csr->GetLayer();
            if (layer->GetKind() != TDataLayer::Mem || layer->GetMarkedTaken()) {
              break;
            }
            assert(!layer->GetMarkedTaken());
            layer->MarkTaken();
            mem_to_merge_vec.push_back(reinterpret_cast<TMemoryLayer *>(layer));
          }
          //syslog(LOG_INFO, "Layout Disk=[%ld]\tMem=[%ld]\tToMerge=[%ld]\t\t\tTaken=[%ld]", num_disk, total_count - num_disk, mem_to_merge_vec.size(), taken);
          /* #607: claim the whole copy before making any of it (TPool::TryClaim). The safe root
             writes several layers as one file by first copying them into one layer; if that copy
             isn't granted, it writes the oldest layer by itself instead, which copies nothing.
             That way the root can always flush what Tetris has promoted into it, however full
             the pools are, and each promotion it makes room for shrinks some child's
             unpromoted backlog, which is all a child's merge copies. A child or fast repo whose
             copy isn't granted waits, holding nothing, and tries again on its next pass. */
          TUpdate::TCopyClaim copy_claim;
          if (mem_to_merge_vec.size() >= 2) {
            size_t updates = 0UL, entries = 0UL;
            CountCopy(mem_to_merge_vec, lower_seq_bound, !ParentRepo, updates, entries);
            if (!copy_claim.TryAcquire(updates, entries)) {
              if (!IsSafeRepo() || ParentRepo) {
                throw std::bad_alloc();
              }
              for (size_t i = 0; i + 1 < mem_to_merge_vec.size(); ++i) {
                mem_to_merge_vec[i]->UnmarkTaken();
              }
              mem_to_merge_vec.erase(mem_to_merge_vec.begin(), mem_to_merge_vec.end() - 1);
              static std::atomic<size_t> single_flushes(0UL);
              const size_t n = ++single_flushes;
              if ((n & (n - 1UL)) == 0UL) {
                syslog(LOG_ERR, "StepMergeMem: no room to combine the global repo's memory layers; flushing the oldest alone (%ld times so far)", n);
              }
            }
          }
          /* #904: from here the merge reads only the layers it took. */
          hold.Narrow(mem_to_merge_vec);
          if (mem_to_merge_vec.size() == 1) {
              //syslog(LOG_INFO, "mem_to_merge_vec.size() == 1");
              if (IsSafeRepo() && !ParentRepo && !reinterpret_cast<TMemoryLayer *>(mem_to_merge_vec[0])->IsEmpty()) {
                /* #227: ONLY the root repo (safe, no parent -- the global pov)
                   flushes to disk. The root is the durable home of record, so
                   its entries must NOT be dropped (nothing else holds them);
                   write the whole layer. Child POVs deliberately fall through
                   to the in-memory path below: a child's durable home is its
                   parent (its writes are Tetris-promoted up), so its own
                   disk copy is always redundant. Crucially, never letting a
                   child reach disk is what keeps reads correct -- a child copy
                   that landed on disk would carry a zero UpdateId and so escape
                   the cross-repo Tetris-window dedup in context.cc, making
                   commutative `+=` double-count. Keeping child data in memory
                   (where the walker reports the real UpdateId) lets that dedup
                   work. See the RunMergeMem deadline fix that first made any of
                   this code actually run. */
                TMemoryLayer *const src = reinterpret_cast<TMemoryLayer *>(mem_to_merge_vec[0]);
                size_t num_keys = 0U;
                TSequenceNumber saved_low_seq = 0UL, saved_high_seq = 0UL;
                TDiskLayerSlot disk_slot;
                /* WriteFile keeps the WAL's write-ahead rule and releases what the WAL kept for
                   these updates (#755). */
                size_t gen_id = write_file(src, saved_low_seq, saved_high_seq, num_keys);
                {
                  std::lock_guard<std::mutex> lock(Manager->MergeMemCPULock);
                  Manager->MergeMemAverageKeysCalc.Push(num_keys);
                }
                new_disk = disk_slot.Make(Manager, this, gen_id, num_keys, saved_low_seq, saved_high_seq);
                delete new_mem;
                new_mem = nullptr;
              } else {
                /* #227: not the flushing root. This is a child POV (safe or
                   fast) or a non-safe root. Keep data in memory; for a child,
                   drop entries already released to the parent so they do not
                   linger to duplicate the parent's on-disk (zero-UpdateId) copy
                   and defeat the cross-repo dedup. */
                TMemoryLayer *const src = reinterpret_cast<TMemoryLayer *>(mem_to_merge_vec[0]);
                const bool empty = (src->GetSize() == 0UL);
                const TSequenceNumber last_seq = empty ? 0UL : src->GetUpdateCollection()->TryGetLastMember()->GetSequenceNumber();
                const TSequenceNumber first_seq = empty ? 0UL : src->GetUpdateCollection()->TryGetFirstMember()->GetSequenceNumber();
                if (!empty && last_seq <= lower_seq_bound) {
                  /* every entry already released -> drop the whole layer. */
                  DEBUG_LOG("Skipping because of lower_seq_bound [%ld]", lower_seq_bound);
                  delete new_mem;
                  new_mem = nullptr;
                } else if (empty || !ParentRepo || first_seq > lower_seq_bound) {
                  /* nothing to drop (root keeps everything; or a child whose
                     entries are all still unreleased) -> leave the single layer
                     in place with no copy, as the original fast path did. */
                  src->UnmarkTaken();
                  delete new_mem;
                  hold.Release();
                  return;
                } else {
                  /* child with a mix of released + unreleased entries -> keep
                     only the unreleased remainder in memory; the released ones
                     live in the parent. */
                  size_t updates = 0UL, entries = 0UL;
                  CountCopy({src}, lower_seq_bound, false, updates, entries);
                  if (!copy_claim.TryAcquire(updates, entries)) {
                    throw std::bad_alloc();
                  }
                  std::unordered_map<const TUpdate *, TUpdate *> update_remap;
                  for (TMemoryLayer::TUpdateCollection::TCursor csr(src->GetUpdateCollection()); csr; ++csr) {
                    if (csr->GetSequenceNumber() > lower_seq_bound) {
                      TUpdate *new_update = TUpdate::ShallowCopy(&*csr, state_alloc);
                      auto ret = update_remap.insert(make_pair(&*csr, new_update));
                      assert(ret.second);
                      new_mem->ImporterAppendUpdate(new_update);
                    }
                  }
                  std::vector<TMemoryLayer::TCopiedEntry> copies;
                  for (TMemoryLayer::TEntryCollection::TCursor csr(src->GetEntryCollection()); csr; ++csr) {
                    if (csr->GetSequenceNumber() > lower_seq_bound) {
                      const auto iter = update_remap.find(csr->GetUpdate());
                      assert(iter != update_remap.end());
                      copies.push_back({&*csr, iter->second});
                    }
                  }
                  new_mem->ImporterAppendCopies(copies, state_alloc);
                  copy_claim.Release();
                  /* new_mem (unreleased remainder) is added to the mapping
                     below; src is MarkForDelete'd there. */
                }
              }
            } else if(mem_to_merge_vec.size() >= 2) {
              //syslog(LOG_INFO, "mem_to_merge_vec.size() >= 2");
              TUpdateSortComparator comparator;
              TCopyMergeSorter<TUpdate *, size_t, TUpdateSortComparator> update_sorter(comparator);
              TCopyMergeSorter<TUpdate *, size_t, TUpdateSortComparator>::TMergeElement *update_sorter_alloc = 0;
              TKeySorter<size_t> entry_sorter;
              TKeySorter<size_t>::TMergeElement *entry_sorter_alloc = 0;
              /* sorter alloca scope */ {
                std::unordered_map<const TUpdate *, TUpdate *> update_remap;
                update_sorter_alloc = reinterpret_cast<TCopyMergeSorter<TUpdate *, size_t, TUpdateSortComparator>::TMergeElement *>(alloca(sizeof(TCopyMergeSorter<TUpdate *, size_t, TUpdateSortComparator>::TMergeElement) * mem_to_merge_vec.size()));
                entry_sorter_alloc = reinterpret_cast<TKeySorter<size_t>::TMergeElement *>(alloca(sizeof(TKeySorter<size_t>::TMergeElement) * mem_to_merge_vec.size()));
                std::vector<TMemoryLayer::TUpdateCollection::TCursor> update_csr_vec;
                std::vector<TMemoryLayer::TEntryCollection::TCursor> entry_csr_vec;
                size_t update_pos = 0UL;
                size_t entry_pos = 0UL;
                for (auto layer : mem_to_merge_vec) {
                  assert(layer->GetMarkedTaken());
                  TMemoryLayer::TUpdateCollection::TCursor update_csr(reinterpret_cast<TMemoryLayer *>(layer)->GetUpdateCollection());
                  TMemoryLayer::TEntryCollection::TCursor entry_csr(reinterpret_cast<TMemoryLayer *>(layer)->GetEntryCollection());
                  if (update_csr) {
                    update_csr_vec.push_back(update_csr);
                    new (update_sorter_alloc + update_pos) TCopyMergeSorter<TUpdate *, size_t, TUpdateSortComparator>::TMergeElement(&update_sorter, &*update_csr, update_pos);
                    ++update_pos;
                  }
                  if (entry_csr) {
                    entry_csr_vec.push_back(entry_csr);
                    new (entry_sorter_alloc + entry_pos) TKeySorter<size_t>::TMergeElement(&entry_sorter, entry_csr->GetKey(), entry_csr->GetSequenceNumber(), entry_pos);
                    ++entry_pos;
                  }
                }
                while (!update_sorter.IsEmpty()) {
                  size_t pos;
                  TUpdate *cur_update = update_sorter.Pop(pos);
                  auto &csr = update_csr_vec[pos];
                  assert(cur_update == &*csr);
                  /* #227: only a CHILD repo may drop released entries (the
                     parent retains them). The ROOT (no parent) is the durable
                     home and must keep everything, else flushing released
                     entries to disk would lose them. */
                  if (!ParentRepo || csr->GetSequenceNumber() > lower_seq_bound) {
                    TUpdate *new_update = TUpdate::ShallowCopy(cur_update, state_alloc);
                    auto ret = update_remap.insert(make_pair(cur_update, new_update));
                    assert(ret.second);
                    new_mem->ImporterAppendUpdate(new_update);
                  }
                  ++csr;
                  if (csr) {
                    new (update_sorter_alloc + pos) TCopyMergeSorter<TUpdate *, size_t, TUpdateSortComparator>::TMergeElement(&update_sorter, &*csr, pos);
                  }
                }
                std::vector<TMemoryLayer::TCopiedEntry> copies;
                while(!entry_sorter.IsEmpty()) {
                  size_t pos = entry_sorter.Pop();
                  auto &csr = entry_csr_vec[pos];
                  /* #227: see the update-loop above -- root keeps all, child drops released. */
                  if (!ParentRepo || csr->GetSequenceNumber() > lower_seq_bound) {
                    const auto iter = update_remap.find(csr->GetUpdate());
                    assert(iter != update_remap.end());
                    copies.push_back({&*csr, iter->second});
                  }
                  ++csr;
                  if (csr) {
                    new (entry_sorter_alloc + pos) TKeySorter<size_t>::TMergeElement(&entry_sorter, csr->GetKey(), csr->GetSequenceNumber(), pos);
                  }
                }
                new_mem->ImporterAppendCopies(copies, state_alloc);
              }  // end sorter alloca scope
              copy_claim.Release();
              if (IsSafeRepo() && !ParentRepo) {
                /* #227: ONLY the root flushes (see the size==1 path above).
                   A safe ROOT always consumes new_mem here; it may come out
                   EMPTY only if the whole layer was empty, but free/null it
                   either way. Safe CHILD repos do NOT flush -- their (filtered,
                   unreleased) new_mem stays in memory and is added to the
                   mapping below, exactly like a fast repo. */
                if (!new_mem->IsEmpty()) {
                  size_t num_keys = 0U;
                  TSequenceNumber saved_low_seq = 0UL, saved_high_seq = 0UL;
                  TDiskLayerSlot disk_slot;
                  size_t gen_id = write_file(new_mem, saved_low_seq, saved_high_seq, num_keys);
                  {
                    std::lock_guard<std::mutex> lock(Manager->MergeMemCPULock);
                    Manager->MergeMemAverageKeysCalc.Push(num_keys);
                  }
                  new_disk = disk_slot.Make(Manager, this, gen_id, num_keys, saved_low_seq, saved_high_seq);
                }
                delete new_mem;
                new_mem = nullptr;
              }
            } else {
              //syslog(LOG_INFO, "mem_to_merge_vec.size() == 0");
              delete new_mem;
              hold.Release();
              return;
            }
          if (new_mem) {
            /* #227: new_mem survives to the mapping only for repos that do
               NOT flush to disk -- fast repos (!IsSafeRepo) and safe CHILD
               repos (ParentRepo), which keep their unreleased data in
               memory so the cross-repo dedup keeps working. The safe ROOT
               always flushed + nulled new_mem above. */
            assert(!IsSafeRepo() || ParentRepo);
            assert(new_mem->IsEmpty() == (new_mem->GetSize() == 0UL));
            if (new_mem->IsEmpty()) {
              DEBUG_LOG("New mem is empty, resetting to nullptr");
              delete new_mem;
              new_mem = nullptr;
            }
          }
          /* Publish the new mapping. Building it takes blocks from the mapping pools; if that
             fails, the half-built mapping is deleted before the lock is released, so it is as if
             the publish never started (#607). With only memory layers involved, the merge then
             rolls back like any copy-phase failure. A file already written can't be taken back:
             its updates have been told they are persisted, and the file map has it. So the
             merged layers stay taken, and the publish is retried by this repo's next memory merge
             (#691), leaving the merge runner to the other repos. */
          try {
            PublishMemMerge(mem_to_merge_vec, new_disk ? static_cast<TDataLayer *>(new_disk) : static_cast<TDataLayer *>(new_mem));
          } catch (const std::bad_alloc &) {
            if (!new_disk) {
              /* Nothing written, so can_retry is still set: roll back below. */
              throw;
            }
            assert(!new_mem);
            UnpublishedMergeLayers.swap(mem_to_merge_vec);
            UnpublishedMergeDisk = new_disk;
            UnpublishedMergeTries = 0UL;
            DeferUnpublishedMemMerge();
            hold.Release();
            return;
          }
          /* Published: from here on there is nothing to roll back. */
          can_retry = false;
        } catch (const exception &ex) {
          /* Retryable shortages are logged, rate-limited, where they are retried. */
          if (!can_retry || !(dynamic_cast<const Disk::Util::TDiskFull *>(&ex) || dynamic_cast<const std::bad_alloc *>(&ex))) {
            syslog(LOG_ERR, "Caught exception in StepMergeMem [%s]", ex.what());
          }
          if (can_retry) {
            /* Roll back: drop the partial copy and hand the layers back. */
            delete new_mem;
            assert(!new_disk);
            for (TMemoryLayer *layer : mem_to_merge_vec) {
              layer->UnmarkTaken();
            }
          }
          hold.Release();
          throw;
        }
        hold.Release();
      }  // release the current mapping
      EndDiskFullStreak(MergeMemDiskFullStreak, "StepMergeMem");
      CheckRemoveDirty();
    }  // release MemMerge lock
  } catch (const std::bad_alloc &) {
    if (!can_retry) {
      /* Not from the pools any more (#607): the copy, the file's disk layer and the publish are
         all covered above. What is left is a heap allocation failing in TDataFile after the
         file map took the file. The merged layers stay taken, so their data is never written
         twice; they keep serving reads from memory, and later merges skip them. */
      syslog(LOG_ERR, "StepMergeMem: out of memory after its file reached the file map; its layers stay in memory");
      EnqueueMergeMem();
      return;
    }
    /* Out of pool space before anything was published (#584). The layers
       that would free it are still queued for the layer cleaner, so try
       again on the next merge cycle rather than abort. Logged rate-limited: on a fiber the
       pools fail at once (#607), so this can come round every merge cycle. */
    static std::atomic<size_t> pool_misses(0UL);
    const size_t misses = ++pool_misses;
    if ((misses & (misses - 1UL)) == 0UL) {
      syslog(LOG_ERR, "StepMergeMem out of pool space; merge rolled back, will retry (%ld times so far)", misses);
    }
    EnqueueMergeMem();
  } catch (const Disk::Util::TDiskFull &ex) {
    if (!can_retry) {
      syslog(LOG_EMERG, "StepMergeMem caught error [%s] after publishing; aborting", ex.what());
      abort();
    }
    /* Out of disk space before anything was published (#590): the layers are back, and the
       disk merges or the discard runner may free space. Until writes are refused when space is
       low, new writes keep landing in memory meanwhile. */
    EnqueueMergeMemAfter(NextDiskFullBackoff(MergeMemDiskFullStreak, "StepMergeMem", ex.what()));
  } catch (const std::exception &ex) {
    syslog(LOG_EMERG, "StepMergeMem caught error [%s]", ex.what());
    abort();
  }
}

void TRepo::PublishMemMerge(const std::vector<TMemoryLayer *> &merged_layers, TDataLayer *new_layer) {
  std::lock_guard<std::mutex> lock(MappingLock);
  TMapping *cur_mapping = MappingCollection.TryGetLastMember();
  assert(cur_mapping);
  cur_mapping->Incr();
  TMapping *new_mapping = nullptr;
  size_t total_disk_layers = 0U;
  std::vector<TDataLayer *> merged;
  try {
    merged.reserve(merged_layers.size());
    new_mapping = new TMapping(this);
    for (TMapping::TEntryCollection::TCursor cur_csr(cur_mapping->GetEntryCollection()); cur_csr; ++cur_csr) {
      assert(cur_csr->GetLayer() != new_layer);
      bool found = false;
      for (auto layer : merged_layers) {
        if (layer == cur_csr->GetLayer()) {
          found = true;
          merged.push_back(layer);
          break;
        }
      }
      if (!found) {
        new TMapping::TEntry(new_mapping, cur_csr->GetLayer());
        if (cur_csr->GetLayer()->GetKind() == TDataLayer::Disk && !cur_csr->GetLayer()->GetMarkedTaken()) {
          ++total_disk_layers;
        }
      }
    }
    if (new_layer) {
      new TMapping::TEntry(new_mapping, new_layer);
    } else {
      DEBUG_LOG("We cleaned away everything, not adding anything to the mapping");
      /* we've cleaned away everything */
    }
  } catch (...) {
    /* The new mapping is the last member, so delete it first; then the Decr leaves
       cur_mapping current, as it was. */
    delete new_mapping;
    cur_mapping->Decr();
    throw;
  }
  for (TDataLayer *layer : merged) {
    layer->MarkForDelete();
  }
  total_disk_layers += (new_layer && new_layer->GetKind() == TDataLayer::Disk) ? 1UL : 0UL;
  const bool has_merge_candidate = HasDiskMergeCandidate(new_mapping);
  cur_mapping->Decr();
  /* #325: skip the pass when its scan would provably find nothing mergeable;
     see HasDiskMergeCandidate. */
  if (total_disk_layers >= 3 && has_merge_candidate) {
    EnqueueMergeDisk();
  }
}

void TRepo::DeferUnpublishedMemMerge() {
  assert(UnpublishedMergeDisk);
  const size_t tries = ++UnpublishedMergeTries;
  if ((tries & (tries - 1UL)) == 0UL) {
    syslog(LOG_ERR, "StepMergeMem: no pool room to publish the file it wrote (try %ld); retrying later", tries);
  }
  if (Manager->IsShuttingDown()) {
    /* Give up, but leave the merged layers taken: their data is in the file, which the file map
       has, and a second copy must never be written. The repo keeps serving them from memory
       until it goes away. */
    syslog(LOG_ERR, "StepMergeMem: shutting down with a written file unpublished; its layers stay in memory");
    delete UnpublishedMergeDisk;
    UnpublishedMergeDisk = nullptr;
    UnpublishedMergeLayers.clear();
    return;
  }
  /* 2ms, doubling to 1s, as the wait on the runner did. The merge queue is ordered by due
     time, so this holds up no other repo's merge. */
  EnqueueMergeMemAfter(std::chrono::milliseconds(std::min<size_t>(1UL << std::min<size_t>(tries, 10UL), 1000UL)));
}

size_t TRepo::AddMapping(TDataLayer *layer) {
  size_t total = 0;
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> lock(MappingLock);
    TMapping *last = MappingCollection.TryGetLastMember();
    last->Incr();
    assert(last);
    TMapping *new_mapping = nullptr;
    try {
      new_mapping = new TMapping(this);
      for (TMapping::TEntryCollection::TCursor csr(last->GetEntryCollection()); csr; ++csr) {
        new TMapping::TEntry(new_mapping, csr->GetLayer());
        ++total;
      }
      TMapping::TEntry *new_entry = new TMapping::TEntry(new_mapping, layer);
      assert(new_mapping->GetEntryCollection()->TryGetLastMember() == new_entry);
      ++total;
      last->Decr();
      assert(MappingCollection.TryGetLastMember() == new_mapping);
    } catch (const std::exception &ex) {
      /* The new mapping became the current one when it was constructed, so a half-built one
         would hide layers from every reader. Delete it (which makes `last` current again)
         before dropping our hold on `last`: the repo is then as it was (#607). */
      delete new_mapping;
      last->Decr();
      if (!dynamic_cast<const std::bad_alloc *>(&ex)) {
        syslog(LOG_ERR, "Error in TRepo::AddMapping [%s]", ex.what());
      }
      throw;
    }
  }  // release Mapping lock
  return total;
}

TRepo::TPresentWalker::TPresentWalker(const unique_ptr<TView> &view,
                                      const TIndexKey &from,
                                      const TIndexKey &to,
                                      bool ignore_tombstone)
    : Orly::Indy::TPresentWalker(Range),
      From(from),
      To(to),
      View(view),
      Lower(View->GetLower() ? *View->GetLower() : 0UL),
      Upper(View->GetUpper() ? *View->GetUpper() : 0UL),
      MinHeap(View->GetNumEntries() + 1UL),
      Valid(false),
      IgnoreTombstone(ignore_tombstone) {
  if (View->GetLower() && View->GetUpper()) {
    size_t pos = 0UL;
    for (TMapping::TEntryCollection::TCursor mapping_csr(View->GetMapping()->GetEntryCollection()); mapping_csr; ++mapping_csr) {
      const TDataLayer *layer = mapping_csr->GetLayer();
      if (!Reads(layer)) {
        continue;
      }
      /* Reading at a version (#745), a disk file seeks each key's entry as of the version; the window filter drops
         a memory layer's newer entries. */
      WalkerVec.emplace_back(View->GetAt() && layer->GetKind() == TDataLayer::Disk
          ? static_cast<const TDiskLayer *>(layer)->NewPresentWalkerAt(From, To, *View->GetAt())
          : layer->NewPresentWalker(From, To));
      Indy::TPresentWalker &walker = *WalkerVec.back();
      if (walker) {
        MinHeap.Insert(*walker, pos);
      }
      ++pos;
    }
    assert(View->GetCurMem());
    WalkerVec.emplace_back(View->GetCurMem()->NewPresentWalker(From, To));
    Indy::TPresentWalker &mem_walker = *WalkerVec.back();
    if (mem_walker) {
      MinHeap.Insert(*mem_walker, pos);
    }
    Valid = static_cast<bool>(MinHeap);
    Init();
  }
}

TRepo::TPresentWalker::TPresentWalker(const unique_ptr<TView> &view,
                                      const TIndexKey &key,
                                      bool ignore_tombstone,
                                      bool exact_point)
    : Orly::Indy::TPresentWalker(Match),
      From(key),
      View(view),
      Lower(View->GetLower() ? *View->GetLower() : 0UL),
      Upper(View->GetUpper() ? *View->GetUpper() : 0UL),
      MinHeap(View->GetNumEntries() + 1UL),
      Valid(false),
      IgnoreTombstone(ignore_tombstone),
      ExactPoint(exact_point) {
  if (View->GetLower() && View->GetUpper()) {
    /* Build layer i's sub-walker into WalkerVec[i], fanned out over at most
       MaxPrepFibers prep fibers. A task that can't get a frame is built inline
       here instead, so an empty frame pool slows a read down rather than
       failing it (#762). Every started task is waited for before anything
       unwinds: the preps point into this object and this stack frame. */
    vector<TDataLayer *> layers;
    layers.reserve(View->GetNumEntries());
    for (TMapping::TEntryCollection::TCursor mapping_csr(View->GetMapping()->GetEntryCollection()); mapping_csr; ++mapping_csr) {
      if (Reads(mapping_csr->GetLayer())) {
        layers.push_back(mapping_csr->GetLayer());
      }
    }
    WalkerVec.resize(layers.size());
    const size_t num_tasks = min(layers.size(), MaxPrepFibers);
    Fiber::TSync sync;
    vector<TRunnablePrep> prep_vec;
    prep_vec.reserve(num_tasks);
    exception_ptr error;
    try {
      size_t task = 0UL;
      for (; task < num_tasks; ++task) {
        prep_vec.emplace_back(this, &layers, task, num_tasks, &sync);
        if (!prep_vec.back().TryStart()) {
          prep_vec.pop_back();
          break;
        }
      }
      for (; task < num_tasks; ++task) {
        BuildLayerWalkers(layers, task, num_tasks);
      }
    } catch (...) {
      error = current_exception();
    }
    sync.Sync(true);
    for (const auto &prep : prep_vec) {
      if (!error && prep.GetError()) {
        error = prep.GetError();
      }
    }
    if (error) {
      rethrow_exception(error);
    }
    size_t pos = 0UL;
    for (auto &walker_ptr : WalkerVec) {
      assert(walker_ptr);
      Indy::TPresentWalker &walker = *walker_ptr;
      if (walker) {
        MinHeap.Insert(*walker, pos);
      }
      ++pos;
    }
    assert(View->GetCurMem());
    WalkerVec.emplace_back(View->GetCurMem()->NewPresentWalker(From, ExactPoint));
    Indy::TPresentWalker &mem_walker = *WalkerVec.back();
    if (mem_walker) {
      MinHeap.Insert(*mem_walker, pos);
    }
    Valid = static_cast<bool>(MinHeap);
    Init();
  }
}

void TRepo::TPresentWalker::BuildLayerWalkers(const vector<TDataLayer *> &layers, size_t task, size_t stride) {
  for (size_t i = task; i < layers.size(); i += stride) {
    /* See the range ctor (#745). */
    WalkerVec[i] = View->GetAt() && layers[i]->GetKind() == TDataLayer::Disk
        ? static_cast<const TDiskLayer *>(layers[i])->NewPresentWalkerAt(From, *View->GetAt())
        : layers[i]->NewPresentWalker(From, ExactPoint);
  }
}

bool TRepo::TPresentWalker::Reads(const TDataLayer *layer) const noexcept {
  return !View->GetAt() || layer->GetKind() != TDataLayer::Disk || layer->GetLowestSeq() <= *View->GetAt();
}

TRepo::TUpdateWalker::TUpdateWalker(const unique_ptr<TView> &view,
                                    TSequenceNumber from,
                                    const std::optional<TSequenceNumber> &to)
    : From(from),
      To(to),
      View(view),
      SorterAlloc(nullptr),
      Valid(false) {
  assert(View);
  if (View->GetLower() && View->GetUpper()) {
    size_t num_walkers = 0U;
    assert(View->GetMapping());
    assert(View->GetMapping()->GetEntryCollection());
    for (TMapping::TEntryCollection::TCursor mapping_csr(View->GetMapping()->GetEntryCollection()); mapping_csr; ++mapping_csr) {
      assert(mapping_csr->GetLayer());
      WalkerMap[mapping_csr->GetLayer()] = make_pair(mapping_csr->GetLayer()->NewUpdateWalker(From), num_walkers);
      ++num_walkers;
    }
    assert(View->GetCurMem());
    WalkerMap[View->GetCurMem()] = make_pair(View->GetCurMem()->NewUpdateWalker(From), num_walkers);
    SorterAlloc = reinterpret_cast<TMergeSorter<TSequenceNumber, const TDataLayer *>::TMergeElement *>(malloc(sizeof(TMergeSorter<TSequenceNumber, const TDataLayer *>::TMergeElement) * WalkerMap.size()));
    if (!SorterAlloc) {
      syslog(LOG_EMERG, "bad alloc in TRepo::TUpdateWalker");
      throw std::bad_alloc();
    }
    try {
      for (auto &iter : WalkerMap) {
        Indy::TUpdateWalker &walker = *iter.second.first;
        if (walker) {
          assert(iter.second.second < WalkerMap.size());
          new (SorterAlloc + iter.second.second) TMergeSorter<TSequenceNumber, const TDataLayer *>::TMergeElement(&MergeSorter, (*walker).SequenceNumber, iter.first);
        }
      }
      Valid = !MergeSorter.IsEmpty();
      Refresh();
    } catch (...) {
      MergeSorter.Clear();
      assert(MergeSorter.IsEmpty());
      free(SorterAlloc);
      SorterAlloc = nullptr;
      throw;
    }
  }
}

TRepo::TUpdateWalker::~TUpdateWalker() {
  MergeSorter.Clear();
  assert(MergeSorter.IsEmpty());
  free(SorterAlloc);
}

TRepo::TUpdateWalker::operator bool() const noexcept {
  return Valid;
}

const TUpdateWalker::TItem &TRepo::TUpdateWalker::operator*() const {
  assert(Valid);
  return Item;
}

TRepo::TUpdateWalker &TRepo::TUpdateWalker::operator++() {
  assert(Valid);
  Valid = !MergeSorter.IsEmpty();
  Refresh();
  return *this;
}

void TRepo::TUpdateWalker::Refresh() {
  bool done = false;
  while (Valid && !done) {
    const TDataLayer *layer;
    /*TSequenceNumber seq_num = */MergeSorter.Pop(layer);
    pair<unique_ptr<Indy::TUpdateWalker>, size_t> &found = WalkerMap.find(layer)->second;
    Indy::TUpdateWalker &walker = *(found.first);
    if (To && (*walker).SequenceNumber > *To) {
      Valid = false;
      break;
    }
    if ((*walker).SequenceNumber >= *View->GetLower() && (*walker).SequenceNumber <= *View->GetUpper()) {
      Item = *walker;
      done = true;
    }
    ++walker;
    if (walker) {
      assert(found.second < WalkerMap.size());
      new (SorterAlloc + found.second) TMergeSorter<TSequenceNumber, const TDataLayer *>::TMergeElement(&MergeSorter, (*walker).SequenceNumber, layer);
    }
    if (!done) {
      Valid = !MergeSorter.IsEmpty();
    }
  }
}

TRepo::TMapping *TRepo::AcquireCurrentMapping() {
  TMapping *mapping = 0;
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> lock(MappingLock);
    mapping = MappingCollection.TryGetLastMember();
    mapping->Incr();
    assert(mapping);
    return mapping;
  }  // release Mapping lock
}

void TRepo::ReleaseMapping(TMapping *mapping) {
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> lock(MappingLock);
    mapping->Decr();
  }  // release Mapping lock
}

void TRepo::CheckRemoveDirty() {
  /* Decide under DataLock, which AppendUpdate holds while it makes the repo dirty, and count the
     current memory layer too.  StepMergeMem seals that layer into the mapping before it merges,
     so an update appended while the merge runs lands in a fresh current layer the mapping
     doesn't have.  Looking at the mapping alone, the merge dropped the repo's self-pin with that
     update still unreleased, so an expired pov could be discarded while the update's pop had yet
     to complete (#665).  The caller pins the repo (StepQueuedMergeMem, #614), so dropping the
     pin here never destroys the repo under its own lock; ReleaseUpdate does the same.  Lock
     order DataLock -> MappingLock, as in GetLowestUpdate. */
  std::lock_guard<std::mutex> data_lock(DataLock);
  TMapping *mapping = AcquireCurrentMapping();
  bool found_non_empty_mem = !CurMemoryLayer->IsEmpty();
  try {
    for (TMapping::TEntryCollection::TCursor csr(mapping->GetEntryCollection()); csr; ++csr) {
      if (csr->GetLayer()->GetKind() == TDataLayer::TKind::Mem && !dynamic_cast<TMemoryLayer *>(csr->GetLayer())->IsEmpty()) {
        found_non_empty_mem = true;
      }
    }
    if (!found_non_empty_mem) {
      RemoveFromDirty();
    } else {
      EnqueueMergeMem();
    }
  } catch (...) {
    ReleaseMapping(mapping);
    throw;
  }
  ReleaseMapping(mapping);
}

TFastRepo::TFastRepo(L0::TManager *manager,
                     const TUuid &repo_id,
                     const TTtl &ttl,
                     const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo)
    : TRepo(manager,
            repo_id,
            ttl,
            parent_repo) {}

TFastRepo::TFastRepo(L0::TManager *manager,
                     const Base::TUuid &repo_id,
                     const TTtl &ttl,
                     const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                     const std::optional<TSequenceNumber> &lowest,
                     const std::optional<TSequenceNumber> &highest,
                     TSequenceNumber next_update,
                     TStatus status)
    : TRepo(manager,
            repo_id,
            ttl,
            parent_repo,
            lowest,
            highest,
            next_update,
            status) {}

TFastRepo::~TFastRepo() {
  PreDtor();
}

bool TFastRepo::IsSafeRepo() const noexcept {
  return false;
}

void TFastRepo::StepMergeDisk(size_t /*block_slots_available*/) {
  assert(false); /* Fast repo should never have disk files to merge. */
}

void TFastRepo::StepTail(size_t /*block_slots_available*/) {
  assert(false);
}

size_t TFastRepo::AddSyncedFileToRepo(size_t /*starting_block_id*/,
                                      size_t /*starting_block_offset*/,
                                      size_t /*file_length*/,
                                      TSequenceNumber /*low_saved*/,
                                      TSequenceNumber /*high_saved*/,
                                      size_t /*num_keys*/) {
  throw std::logic_error("TFastRepo does not support disk files");
}

TSafeRepo::TSafeRepo(L0::TManager *manager,
                     const TUuid &repo_id,
                     const TTtl &ttl,
                     const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo)
    : TRepo(manager,
            repo_id,
            ttl,
            parent_repo),
      NextGenId(1U) {
  #ifndef NDEBUG
  std::vector<Disk::TFileObj> file_vec;
  Manager->GetFileGenSet(repo_id, file_vec);
  assert(!file_vec.size());
  #endif
}

TSafeRepo::TSafeRepo(L0::TManager *manager,
                     const Base::TUuid &repo_id,
                     const TTtl &ttl,
                     const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                     const std::optional<TSequenceNumber> &lowest,
                     const std::optional<TSequenceNumber> &highest,
                     TSequenceNumber next_update,
                     TStatus status)
    : TRepo(manager,
            repo_id,
            ttl,
            parent_repo,
            lowest,
            highest,
            next_update,
            status),
      NextGenId(0UL) {
  std::vector<Disk::TFileObj> file_vec;
  Manager->GetFileGenSet(repo_id, file_vec);
  size_t max_gen_id = 0UL;
  /* we need to sort these first to make sure we add them in sequence. */
  std::sort(file_vec.begin(), file_vec.end(), [](const Disk::TFileObj &lhs, const Disk::TFileObj &rhs) -> bool {
    return lhs.LowestSeq < rhs.LowestSeq;
  });
  try {
    /* acquire Mapping lock */ {
      std::lock_guard<std::mutex> lock(MappingLock);
      TMapping *mapping = new TMapping(this);
      for (const auto &iter : file_vec) {
        max_gen_id = std::max(max_gen_id, iter.GenId);
        new TMapping::TEntry(mapping, new TDiskLayer(Manager, this, iter.GenId, iter.NumKeys, iter.LowestSeq, iter.HighestSeq));
      }
    }  // release Mapping lock
  } catch (...) {
    MappingCollection.DeleteEachMember();
    throw;
  }
  NextGenId = max_gen_id + 1UL;
}

TSafeRepo::~TSafeRepo() {
  PreDtor();
}

void TSafeRepo::StepTail(size_t block_slots_available) {
  assert(!GetParentRepo());
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;
  try {
    /* Tail a merge disk file if available */ {
      /* grab the current mapping */ {
        TMapping *mapping = AcquireCurrentMapping();
        assert(mapping);
        TDiskLayer *new_merge_disk = 0;
        size_t gen_id_to_tail = 0UL;
        TDiskLayer *gen_layer_to_tail = nullptr;
        size_t num_keys = 0U;
        TSequenceNumber lowest_seq = numeric_limits<uint64_t>::max(), highest_seq = 0UL;
        /* The tailed file's disk layer, taken before the file is written (#627). */
        std::optional<TDiskLayerSlot> disk_slot;
        auto hand_back = [&] {
          if (gen_layer_to_tail) {
            std::lock_guard<std::mutex> lock(MergeLock);
            gen_layer_to_tail->UnmarkTaken();
          }
          ReleaseMapping(mapping);
        };
        try {
          /* acquire Merge lock */ {
            std::lock_guard<std::mutex> lock(MergeLock);
            TDataLayer *lowest_layer = mapping->GetEntryCollection()->TryGetFirstMember() ? mapping->GetEntryCollection()->TryGetFirstMember()->GetLayer() : nullptr;
            if (lowest_layer && lowest_layer->GetKind() == TDataLayer::TKind::Disk && !lowest_layer->GetMarkedTaken()) {
              num_keys += lowest_layer->GetSize();
              lowest_seq = std::min(lowest_seq, lowest_layer->GetLowestSeq());
              highest_seq = std::max(highest_seq, lowest_layer->GetHighestSeq());
              gen_layer_to_tail = reinterpret_cast<TDiskLayer *>(lowest_layer);
              gen_id_to_tail = reinterpret_cast<TDiskLayer *>(lowest_layer)->GetGenId();
              lowest_layer->MarkTaken();
            }
          }
          if (gen_layer_to_tail) {
            syslog(LOG_INFO, "Tailing file [%ld] with [%ld] num keys", gen_id_to_tail, num_keys);
            disk_slot.emplace();
            size_t gen_id = MergeFiles(std::vector<size_t>{gen_id_to_tail}, storage_speed, block_slots_available, Manager->GetTempFileConsolThresh(), lowest_seq, highest_seq, num_keys, GetReleasedUpTo(), IsTailingAllowed(), true);
            {
              std::lock_guard<std::mutex> lock(Manager->MergeDiskCPULock);
              Manager->MergeDiskAverageKeysCalc.Push(num_keys);
            }
            new_merge_disk = disk_slot->Make(Manager, this, gen_id, num_keys, lowest_seq, highest_seq);
          }
        } catch (const Disk::Util::TDiskFull &ex) {
          /* #590: as in StepMergeDisk, hand the layer back. Tailing runs once per request
             (TServer::TailGlobalPov), not from a queue, so report it and don't retry. */
          hand_back();
          syslog(LOG_ERR, "StepTail out of disk space [%s]; file [%ld] left untailed", ex.what(), gen_id_to_tail);
          return;
        } catch (const std::bad_alloc &ex) {
          /* #627: likewise for pool space. MergeFiles has written nothing it keeps. */
          hand_back();
          syslog(LOG_ERR, "StepTail out of pool space [%s]; file [%ld] left untailed", ex.what(), gen_id_to_tail);
          return;
        } catch (const std::exception &ex) {
          syslog(LOG_EMERG, "StepTail [1083] caught error [%s]", ex.what());
          ReleaseMapping(mapping);
          throw;
        } catch (...) {
          syslog(LOG_EMERG, "StepTail [1088] caught error");
          ReleaseMapping(mapping);
          throw;
        }
        if (gen_layer_to_tail) {
          assert(new_merge_disk);
          try {
            PublishDiskMerge({gen_layer_to_tail}, new_merge_disk);
          } catch (const std::bad_alloc &ex) {
            /* #627: the half-built mapping is gone; drop the tailed file and hand the layer
               back. */
            DiscardDiskMerge({gen_layer_to_tail}, new_merge_disk);
            ReleaseMapping(mapping);
            syslog(LOG_ERR, "StepTail out of pool space publishing [%s]; file [%ld] left untailed", ex.what(), gen_id_to_tail);
            return;
          } catch (const std::exception &ex) {
            syslog(LOG_EMERG, "StepTail [1176] caught error [%s]", ex.what());
            ReleaseMapping(mapping);
            throw;
          } catch (...) {
            syslog(LOG_EMERG, "StepTail [1180] caught error");
            ReleaseMapping(mapping);
            throw;
          }
        }
        ReleaseMapping(mapping);
      }
    }  // done flushing a tailed file
  } catch (const std::exception &ex) {
    syslog(LOG_EMERG, "StepTail [1188] caught error [%s]", ex.what());
    abort();
  }
}

void TSafeRepo::PublishDiskMerge(const std::vector<TDiskLayer *> &input_layers, TDiskLayer *merged_layer) {
  assert(merged_layer);
  std::lock_guard<std::mutex> lock(MappingLock);
  TMapping *cur_mapping = MappingCollection.TryGetLastMember();
  assert(cur_mapping);
  cur_mapping->Incr();
  TMapping *new_mapping = nullptr;
  size_t total_disk_layers = 0U;
  try {
    new_mapping = new TMapping(this);
    for (TMapping::TEntryCollection::TCursor cur_csr(cur_mapping->GetEntryCollection()); cur_csr; ++cur_csr) {
      assert(cur_csr->GetLayer() != merged_layer);
      if (std::find(input_layers.begin(), input_layers.end(), cur_csr->GetLayer()) == input_layers.end()) {
        new TMapping::TEntry(new_mapping, cur_csr->GetLayer());
        if (cur_csr->GetLayer()->GetKind() == TDataLayer::Disk) {
          ++total_disk_layers;
        }
      }
    }
    new TMapping::TEntry(new_mapping, merged_layer);
    ++total_disk_layers;
  } catch (...) {
    /* The new mapping became the last member, so the current one, when it was constructed.
       Deleting it (and the entries it has) makes cur_mapping current again, as in
       TRepo::AddMapping (#607). */
    delete new_mapping;
    cur_mapping->Decr();
    throw;
  }
  for (TDiskLayer *layer : input_layers) {
    layer->MarkForDelete();
  }
  const bool has_merge_candidate = HasDiskMergeCandidate(new_mapping);
  cur_mapping->Decr();
  /* #325: skip the pass when its scan would provably find nothing mergeable; see
     HasDiskMergeCandidate. */
  if (total_disk_layers >= 3 && has_merge_candidate) {
    EnqueueMergeDisk();
  }
}

void TSafeRepo::DiscardDiskMerge(const std::vector<TDiskLayer *> &input_layers, TDiskLayer *merged_layer) {
  assert(merged_layer);
  const size_t gen_id = merged_layer->GetGenId();
  /* Not marked for delete, so its destructor leaves the file alone. */
  delete merged_layer;
  try {
    RemoveFile(gen_id, false);
  } catch (const std::exception &ex) {
    /* Then the file's blocks stay used until a restart, which drops a file whose sequence range
       another file covers (ReConstructFromDisk). Its data is all in the inputs. */
    syslog(LOG_ERR, "could not remove unpublished merge file [%ld]: [%s]", gen_id, ex.what());
  }
  /* acquire Merge lock */ {
    std::lock_guard<std::mutex> lock(MergeLock);
    for (TDiskLayer *layer : input_layers) {
      layer->UnmarkTaken();
    }
  }  // release Merge lock
}

void TSafeRepo::StepMergeDisk(size_t block_slots_available) {
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;
  try {
    /* Flush a merge disk file if available */ {
      /* grab the current mapping */ {
        TMergeHold hold(this);
        TMapping *mapping = hold.GetMapping();
        assert(mapping);
        TDiskLayer *new_merge_disk = 0;
        std::vector<size_t> gen_id_vec;
        std::vector<TDiskLayer *> gen_layer_vec;
        size_t num_keys = 0U;
        TSequenceNumber lowest_seq = numeric_limits<uint64_t>::max(), highest_seq = 0UL;
        /* The merged file's disk layer, taken before the file is written (#627): once the file
           is written, failing to record it would mean undoing the write. */
        std::optional<TDiskLayerSlot> disk_slot;
        /* MergeFiles has freed whatever it reserved, so hand the inputs back and try again later.
           Unmarking makes this pair mergeable again without a mapping install, which is what
           normally re-runs the merge gate (see HasDiskMergeCandidate), so re-enqueue
           explicitly. */
        auto hand_back = [&](const char *what_ran_out, const char *err) {
          /* acquire Merge lock */ {
            std::lock_guard<std::mutex> lock(MergeLock);
            for (TDiskLayer *layer : gen_layer_vec) {
              layer->UnmarkTaken();
            }
          }  // release Merge lock
          hold.Release();
          EnqueueMergeDiskAfter(NextDiskFullBackoff(MergeDiskDiskFullStreak, "StepMergeDisk", err, what_ran_out));
        };
        /* #692: set when an eligible pair is skipped because it can't fit. */
        bool skipped_unfit = false;
        try {
          //std::map<size_t, std::vector<TDiskLayer *>> gen_to_gen_id_map;
          /* #692: a merge allocates up to the size of its inputs while they stay live (see
             MergeFiles), so take only a pair that fits in the room above the data floor that other
             merges haven't claimed. A pair that can't fit would write until it hit the floor, fail,
             and be retried forever, and, taken first, would keep a pair that fits from merging. */
          std::unordered_map<const TDataLayer *, size_t> file_bytes;
          for (TMapping::TEntryCollection::TCursor csr(mapping->GetEntryCollection()); csr; ++csr) {
            TDataLayer *layer = csr->GetLayer();
            size_t block_id, block_offset, file_size, file_keys;
            if (layer->GetKind() == TDataLayer::TKind::Disk
                && Manager->GetEngine()->FindFile(GetId(), reinterpret_cast<TDiskLayer *>(layer)->GetGenId(), block_id, block_offset, file_size, file_keys)) {
              file_bytes[layer] = file_size;
            }
          }
          auto *const vol_man = Manager->GetEngine()->GetVolMan();
          const size_t available = vol_man->GetSpace().GetAvailable();
          const size_t unavailable = vol_man->GetDataFloor() + vol_man->GetPendingClaims();
          const size_t room = available > unavailable ? available - unavailable : 0UL;
          /* acquire Merge lock */ {
            std::lock_guard<std::mutex> lock(MergeLock);
            for (TMapping::TEntryCollection::TCursor csr(mapping->GetEntryCollection()); csr; ++csr) {
              TDataLayer *lhs_layer = csr->GetLayer();
              TDataLayer *rhs_layer = csr->TryGetNextMember() ? csr->TryGetNextMember()->GetLayer() : nullptr;
              /* if i'm a disk layer, so is my neighbor on the right, neither of us are marked as taken, and i'm in the same or lower gen set than my neigbor */
              if ((lhs_layer && rhs_layer)  // i have a neighbor
                  && (lhs_layer->GetKind() == TDataLayer::TKind::Disk)  // I'm a disk layer
                  && (rhs_layer->GetKind() == TDataLayer::TKind::Disk)  // my neighbor is a disk layer
                  && (!lhs_layer->GetMarkedTaken())  // I'm not marked taken
                  && (!rhs_layer->GetMarkedTaken())  // my neighbor is not marked taken
                  && (Disk::Util::SuggestGeneration(lhs_layer->GetSize()) <= Disk::Util::SuggestGeneration(rhs_layer->GetSize()))  // in in the same or lower gen set than my neighbor
                  ) {
                auto bytes_of = [&file_bytes](const TDataLayer *layer) {
                  const auto iter = file_bytes.find(layer);
                  return iter == file_bytes.end() ? 0UL : iter->second;
                };
                const size_t pair_bytes = bytes_of(lhs_layer) + bytes_of(rhs_layer);
                if (pair_bytes > room) {
                  skipped_unfit = true;
                  continue;
                }
                lowest_seq = std::min(lowest_seq, lhs_layer->GetLowestSeq());
                highest_seq = std::max(highest_seq, lhs_layer->GetHighestSeq());
                num_keys += lhs_layer->GetSize();
                gen_layer_vec.push_back(reinterpret_cast<TDiskLayer *>(lhs_layer));
                gen_id_vec.push_back(reinterpret_cast<TDiskLayer *>(lhs_layer)->GetGenId());
                lhs_layer->MarkTaken();
                lowest_seq = std::min(lowest_seq, rhs_layer->GetLowestSeq());
                highest_seq = std::max(highest_seq, rhs_layer->GetHighestSeq());
                num_keys += rhs_layer->GetSize();
                gen_layer_vec.push_back(reinterpret_cast<TDiskLayer *>(rhs_layer));
                gen_id_vec.push_back(reinterpret_cast<TDiskLayer *>(rhs_layer)->GetGenId());
                rhs_layer->MarkTaken();
                EnqueueMergeDisk();
                break;
              }
            }
          }  // release Merge lock
          /* #904: from here the merge reads only the pair. */
          hold.Narrow(gen_layer_vec);
          if (gen_id_vec.empty() && skipped_unfit) {
            /* Nothing fits: write nothing, and look again after a backoff. No claim is noted for
               write admission, which already keeps its reserve free: a pair that can't fit even
               with writes held off would otherwise hold them off for good. */
            hold.Release();
            EnqueueMergeDiskAfter(NextDiskFullBackoff(MergeDiskDiskFullStreak, "StepMergeDisk", "no mergeable pair fits in the free space", "disk space"));
            return;
          }
          if (gen_id_vec.size() > 0) {
            disk_slot.emplace();
            /* Merge as a tail merge, dropping each input's superseded versions (#592). Otherwise
               every version of every key survives every merge, and disk use grows with every write
               ever made. --prune_merge_history turns this off.

               MergeFiles tails only a repo with no parent, whose history no one reads back. A
               slave join walks a view that pins the files it started from, and live replication
               ships transactions, not files. A child's unpromoted updates are what Tetris reads,
               so its merges keep everything.

               Tombstones stay: this pair need not be the oldest files, so a tombstone may still be
               hiding an older version. */
            size_t gen_id = MergeFiles(gen_id_vec, storage_speed, block_slots_available, Manager->GetTempFileConsolThresh(), lowest_seq, highest_seq, num_keys, GetReleasedUpTo(), IsMergePruningAllowed(), false);
            {
              std::lock_guard<std::mutex> lock(Manager->MergeDiskCPULock);
              Manager->MergeDiskAverageKeysCalc.Push(num_keys);
            }
            new_merge_disk = disk_slot->Make(Manager, this, gen_id, num_keys, lowest_seq, highest_seq);
          }
        } catch (const Disk::Util::TDiskFull &ex) {
          /* #590 */
          hand_back("disk space", ex.what());
          return;
        } catch (const std::bad_alloc &ex) {
          /* #627: the same, for pool space. Nothing is written yet: the disk layer slot comes
             before MergeFiles, and MergeFiles keeps no file when it throws. */
          hand_back("pool space", ex.what());
          return;
        } catch (const std::exception &ex) {
          syslog(LOG_EMERG, "StepMergeDisk [1113] caught error [%s]", ex.what());
          hold.Release();
          throw;
        } catch (...) {
          syslog(LOG_EMERG, "StepMergeDisk [1117] caught error");
          hold.Release();
          throw;
        }
        if (gen_id_vec.size() > 0) {
          assert(new_merge_disk);
          assert(gen_layer_vec.size() == gen_id_vec.size());
          try {
            PublishDiskMerge(gen_layer_vec, new_merge_disk);
          } catch (const std::bad_alloc &ex) {
            /* #627: the merged file is written, but no reader has it, and the inputs still hold
               all its data. Drop it and retry the merge later, as when it runs out of disk. */
            DiscardDiskMerge(gen_layer_vec, new_merge_disk);
            hold.Release();
            EnqueueMergeDiskAfter(NextDiskFullBackoff(MergeDiskDiskFullStreak, "StepMergeDisk", ex.what(), "pool space"));
            return;
          } catch (const std::exception &ex) {
            syslog(LOG_EMERG, "StepMergeDisk [1176] caught error [%s]", ex.what());
            hold.Release();
            throw;
          } catch (...) {
            syslog(LOG_EMERG, "StepMergeDisk [1180] caught error");
            hold.Release();
            throw;
          }
        }
        hold.Release();
        if (!gen_id_vec.empty()) {
          EndDiskFullStreak(MergeDiskDiskFullStreak, "StepMergeDisk");
        }
      }
    }  // done flushing a merge file
  } catch (const std::exception &ex) {
    syslog(LOG_EMERG, "StepMergeDisk [1188] caught error [%s]", ex.what());
    abort();
  }
}

size_t TSafeRepo::AddSyncedFileToRepo(size_t starting_block_id,
                                      size_t starting_block_offset,
                                      size_t file_length,
                                      TSequenceNumber low_saved,
                                      TSequenceNumber high_saved,
                                      size_t num_keys) {
  size_t gen_id = GetNextGenId();
  /* wait for file entry to flush */ {
    TCompletionTrigger trigger;
    syslog(LOG_INFO, "Adding file gen_id=[%ld], starting_block_id=[%ld], starting_block_offset=[%ld], file_length=[%ld], num_keys=[%ld], low_saved=[%ld], high_saved=[%ld]",
           gen_id, starting_block_id, starting_block_offset, file_length, num_keys, low_saved, high_saved);
    Manager->GetEngine()->InsertFile(GetId(), TFileObj::TKind::DataFile, gen_id, starting_block_id, starting_block_offset, file_length, num_keys, low_saved, high_saved, trigger);
    trigger.Wait();
  }
  /* A synced file may be the output of a merge that dropped history, and nothing here says whether it was (#745). */
  RaiseHistoryHorizon(high_saved);
  NoteHistoryHorizonRaised();
  AddFileToRepo(gen_id, low_saved, high_saved, num_keys);
  return gen_id;
}

/* The blocks a data file occupies, from its meta blocks. */
static void ReadFileBlocks(Disk::Util::TEngine *engine, const Base::TUuid &repo_id, size_t gen_id, Orly::Indy::Util::TBlockVec &block_vec) {
  TReader reader(engine, repo_id, Low, gen_id);
  try {
    TReader::TInStream in_stream(HERE, Source::FileRemoval, Low, &reader, engine->GetPageCache(), (reader.GetStartingBlockOffset() * Disk::Util::LogicalBlockSize) + (TData::NumMetaFields * sizeof(size_t)));
    size_t block_id;
    for (size_t i = 0; i < reader.GetNumMetaBlocks(); ++i) {
      in_stream.Read(block_id);
      block_vec.PushBack(block_id);
    }
    size_t num_contig_blocks;
    for (size_t i = 0; i < reader.GetNumSequentialBlockPairings(); ++i) {
      in_stream.Read(block_id);
      in_stream.Read(num_contig_blocks);
      block_vec.PushBack(std::make_pair(block_id, num_contig_blocks));
    }
    assert(block_vec.Size() == reader.GetNumBlocks());
  } catch (const std::exception &ex) {
    stringstream ss;
    ss << repo_id;
    syslog(LOG_ERR, "ReadFileBlocks [%s][%ld] caught error [%s] with NumBlocks=[%ld], NumMetaBlocks=[%ld], NumSequentialBlocks=[%ld], BlockVec.Size=[%ld], StartingBlockOffset=[%ld]",
           ss.str().c_str(), gen_id, ex.what(), reader.GetNumBlocks(), reader.GetNumMetaBlocks(), reader.GetNumSequentialBlockPairings(), block_vec.Size(), reader.GetStartingBlockOffset());
    throw;
  }
}

TSafeRepo *TSafeRepo::ReConstructFromDisk(L0::TManager *manager,
                                          const Base::TUuid &repo_id,
                                          const TDeadline &deadline) {
  std::vector<Disk::TFileObj> file_vec;
  manager->GetFileGenSet(repo_id, file_vec);
  std::optional<TSequenceNumber> lowest;
  std::optional<TSequenceNumber> highest;
  TSequenceNumber next_update = 1L;
  size_t max_gen_id = 0UL;
  TParentRepo parent_repo;
  std::vector<size_t> gen_id_vec_to_remove;
  /* Remove any files that are obsolete due to merging: the inputs of a merge whose output
     reached the file map before a crash let them be removed. A merge output records the whole
     sequence range of its inputs, so each input lies inside it.

     A fold output written before #618 recorded only the range of the updates it kept, which
     can leave an input overlapping it without lying inside it. Live files never overlap, so
     such a pair is a merge's output and one of its inputs, and the input is the older file:
     gen ids only grow. Drop it too. An input that a narrowed range misses altogether looks
     like any other file and can't be told apart; that takes a crash in the merge's last step,
     with such a store, and a first reopen by this code.

     Each pair is logged, by kind (#700): a nested pair is what a crash leaves, anything else
     is a store from before #618 or corruption. */ {
    std::string repo_desc; {
      stringstream ss;
      ss << repo_id;
      repo_desc = ss.str();
    }
    for (const auto &problem : Disk::FindSeqRangeProblems(file_vec)) {
      syslog(problem.Kind == Disk::TSeqRangeProblem::Nested ? LOG_WARNING : LOG_ERR, "ReConstructFromDisk: repo [%s]: %s",
             repo_desc.c_str(), problem.Describe().c_str());
    }
    for (;;) {
      bool found_dup = false;
      for (auto cur = file_vec.begin(); cur != file_vec.end(); ++cur) {
        const auto &file = *cur;
        for (const auto &that_file : file_vec) {
          if (file.GenId == that_file.GenId) {
            continue;
          }
          const bool inside = file.LowestSeq >= that_file.LowestSeq && file.HighestSeq <= that_file.HighestSeq;
          const bool overlaps = file.LowestSeq <= that_file.HighestSeq && that_file.LowestSeq <= file.HighestSeq;
          const bool contains = that_file.LowestSeq >= file.LowestSeq && that_file.HighestSeq <= file.HighestSeq;
          if (inside || (overlaps && !contains && file.GenId < that_file.GenId)) {
            syslog(inside ? LOG_WARNING : LOG_ERR,
                   "ReConstructFromDisk: repo [%s]: dropping file [%ld] (seq [%ld -> %ld]) as a merge input left behind by file [%ld] (seq [%ld -> %ld])%s",
                   repo_desc.c_str(), file.GenId, file.LowestSeq, file.HighestSeq, that_file.GenId, that_file.LowestSeq, that_file.HighestSeq,
                   inside ? "" : "; the ranges only partly overlap, so updates outside the overlap go with it");
            gen_id_vec_to_remove.push_back(file.GenId);
            file_vec.erase(cur);
            found_dup = true;
            break;
          }
        }
        if (found_dup) {
          break;
        }
      }
      if (!found_dup) {
        break;
      }
    }
    /* What is left must be strictly monotone: the release form of the neighbour check in
       TManager::TRepo::TMapping::TEntry, which only asserts (#700). */
    for (const auto &problem : Disk::FindSeqRangeProblems(file_vec)) {
      syslog(LOG_ERR, "ReConstructFromDisk: repo [%s]: after dropping merge leftovers, %s", repo_desc.c_str(), problem.Describe().c_str());
    }
  }
  for (const auto &file : file_vec) {
    max_gen_id = std::max(max_gen_id, file.GenId);
    syslog(LOG_INFO, "File [%ld] has [%ld] keys with seq range [%ld -> %ld]", file.GenId, file.NumKeys, file.LowestSeq, file.HighestSeq);
    lowest = !lowest ? file.LowestSeq : std::min(*lowest, file.LowestSeq);
    highest = !highest ? file.HighestSeq : std::max(*highest, file.HighestSeq);
    next_update = std::max(next_update, *highest + 1UL);
  }
  /* Remove the leftovers and free their blocks, as TSafeRepo::RemoveFile does (#620). The
     engine's startup walk of the file map marked their blocks used, so dropping the entries
     alone would hold those blocks until the next restart.

     Read each file's block list while its entry is still in the file map, and free the blocks
     only once the removal is durable: a block freed before then could be reused by a new file
     while a crash still left the old entry pointing at it. No other entry references these
     blocks: the startup walk refuses to mark a block used twice
     (TVolume::TStrategy::MarkBlockRangeUsed), and since then only free blocks have been
     allocated. A file whose block list can't be read is removed without freeing, as before. */
  std::vector<std::pair<size_t, size_t>> ranges_to_free;
  for (size_t gen_id : gen_id_vec_to_remove) {
    try {
      Util::TBlockVec block_vec;
      ReadFileBlocks(manager->GetEngine(), repo_id, gen_id, block_vec);
      for (const auto &iter : block_vec.GetSeqBlockMap()) {
        ranges_to_free.push_back(iter.second);
      }
    } catch (const std::exception &ex) {
      syslog(LOG_ERR, "ReConstructFromDisk: removing leftover file [%ld] without freeing its blocks, which could not be read: [%s]", gen_id, ex.what());
    }
  }
  TCompletionTrigger trigger;
  for (size_t gen_id : gen_id_vec_to_remove) {
    manager->GetEngine()->RemoveFile(repo_id, gen_id, trigger);
  }
  trigger.Wait();
  for (const auto &range : ranges_to_free) {
    manager->GetEngine()->FreeSeqBlocks(range.first, range.second);
  }
  /* 'deadline' is an absolute time point; convert it to the relative ttl TSafeRepo expects
     rather than reinterpreting its raw epoch tick count as a ttl (which produced an
     effectively-infinite ttl for any real deadline). */
  TTtl repo_ttl = std::chrono::duration_cast<TTtl>(deadline - TDeadline::clock::now());
  TSafeRepo *safe_repo = new TSafeRepo(manager, repo_id, repo_ttl, parent_repo, lowest, highest, next_update, TStatus::Normal);
  if (highest) {
    safe_repo->NoteDurable(*highest);
    /* The files found may be merges that dropped history, and nothing in them records which were, so only the newest
       version is known to be whole (#745), unless the retention record says more (stage 3). */
    safe_repo->NoteLoadedFromDisk(*highest);
  }
  return safe_repo;
}

bool TSafeRepo::IsSafeRepo() const noexcept {
  return true;
}

size_t TSafeRepo::MergeFiles(const std::vector<size_t> &gen_id_vec,
                             Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                             size_t max_block_cache_read_slots_allowed,
                             size_t temp_file_consol_thresh,
                             TSequenceNumber &out_saved_low_seq,
                             TSequenceNumber &out_saved_high_seq,
                             size_t &out_num_keys,
                             TSequenceNumber release_up_to,
                             bool can_tail,
                             bool can_tail_tombstone) {
  /* Until it finishes, this merge allocates up to the size of its inputs while they stay live.
     Write admission keeps that much free on top of its reserve, and, if the merge fails for
     space, keeps it free for its retry (#590). */
  size_t input_bytes = 0UL;
  for (size_t gen_id : gen_id_vec) {
    size_t block_id, block_offset, file_size, num_keys;
    if (Manager->GetEngine()->FindFile(GetId(), gen_id, block_id, block_offset, file_size, num_keys)) {
      input_bytes += file_size;
    }
  }
  Disk::Util::TVolumeManager::TClaim claim(Manager->GetEngine()->GetVolMan(), input_bytes);
  size_t intermediate_gen_id = GetNextGenId();
  /* The callers apply their own flags: --allow_tailing for StepTail, --prune_merge_history for
     StepMergeDisk. */
  bool my_can_tail = can_tail && !static_cast<bool>(GetParentRepo());
  bool my_can_tail_tombstone = my_can_tail && can_tail_tombstone && (gen_id_vec.size() == 1);
  /* #745 stage 2: the oldest version this merge keeps whole, with the retention window on; nothing otherwise. The
     horizon rises to it (or to the merge's newest version, if lower) rather than to the newest. */
  /* Stage 3: never past the horizon the retention record has on disk (GetMergeRetainFrom). */
  const std::optional<TSequenceNumber> retain_from = GetMergeRetainFrom();
  const auto horizon_after = [&retain_from](TSequenceNumber high) {
    return retain_from ? std::min(*retain_from, high) : high;
  };
  /* Phase A: standard merge. Produces a data file at intermediate_gen_id
     with same-mutator commutative runs still expanded -- correct but
     unbounded in entries-per-key under contention. */
  TMergeDataFile merge_data_file(Manager->GetEngine(), storage_speed, GetId(), gen_id_vec, GetId(), intermediate_gen_id, release_up_to, Low, max_block_cache_read_slots_allowed, temp_file_consol_thresh, my_can_tail, my_can_tail_tombstone, retain_from);
  /* Fast path (#64): if the merge produced no non-Assign entries,
     there's nothing to fold and the intermediate file IS the final
     output. Skip the TFoldDataFile read+write+remove cycle entirely.
     Most workloads are Assign-only and hit this path. A test can also ask for the unfolded
     output (#666). */
  if (merge_data_file.GetNumNonAssignEntries() == 0UL || !FoldMergedFiles) {
    out_num_keys = merge_data_file.GetNumKeys();
    out_saved_low_seq = merge_data_file.GetLowestSequence();
    out_saved_high_seq = merge_data_file.GetHighestSequence();
    /* A pruning merge keeps only each key's newest entry, so versions below its newest are gone (#745). The caller
       publishes the file after this returns. */
    if (my_can_tail) {
      RaiseHistoryHorizon(horizon_after(out_saved_high_seq));
    }
    /* #857: count bytes written to disk in this merge. */
    AddMergeBytes(merge_data_file.GetFileLength());
    return intermediate_gen_id;
  }
  /* Phase B (#55): fold same-mutator commutative runs in the just-merged
     file via TMutation::Augment-equivalent logic in typed space. Output
     entries are promoted to Assign(folded value), bringing read
     amplification back to O(1) per key. */
  /* #627: a fold that can't get the memory it needs keeps the merge unfolded. Folding only bounds
     read amplification: the unfolded file is a correct result, as on the fast path above, and a
     later merge that takes it in folds again. Since #745 §9.6 the fold takes nothing from the
     update pools (it streams the merged file key by key), so it no longer claims pool space up
     front (#858); only a heap allocation failure lands here. */
  const auto keep_unfolded = [&]() {
    static std::atomic<size_t> fold_misses(0UL);
    const size_t misses = ++fold_misses;
    if ((misses & (misses - 1UL)) == 0UL) {
      syslog(LOG_ERR, "MergeFiles: no memory to fold %ld keys; keeping the merge unfolded (%ld times so far)", merge_data_file.GetNumKeys(), misses);
    }
    out_num_keys = merge_data_file.GetNumKeys();
    out_saved_low_seq = merge_data_file.GetLowestSequence();
    out_saved_high_seq = merge_data_file.GetHighestSequence();
    if (my_can_tail) {
      RaiseHistoryHorizon(horizon_after(out_saved_high_seq));
    }
    /* #857: count bytes written to disk in this merge. */
    AddMergeBytes(merge_data_file.GetFileLength());
    return intermediate_gen_id;
  };
  size_t final_gen_id = GetNextGenId();
  size_t fold_file_length = 0UL;
  try {
    TFoldDataFile fold_data_file(Manager->GetEngine(), storage_speed, GetId(), intermediate_gen_id, final_gen_id, Low, temp_file_consol_thresh, retain_from);
    out_num_keys = fold_data_file.GetNumKeys();
    out_saved_low_seq = fold_data_file.GetLowestSequence();
    out_saved_high_seq = fold_data_file.GetHighestSequence();
    fold_file_length = fold_data_file.GetFileLength();
  } catch (const std::bad_alloc &) {
    /* The fold's own output freed itself (TDataFile, before the file map has it). */
    return keep_unfolded();
  } catch (const std::exception &ex) {
    /* The fold's own output frees itself (it is a TDataFile), but the intermediate file is
       complete and in the file map, and no layer owns it, so nothing else would ever remove it
       (#590). */
    try {
      RemoveFile(intermediate_gen_id, false);
    } catch (const std::exception &remove_ex) {
      syslog(LOG_ERR, "MergeFiles could not remove intermediate file [%ld] after [%s]: [%s]", intermediate_gen_id, ex.what(), remove_ex.what());
    }
    throw;
  }
  /* Reclaim the intermediate file's blocks. */
  RemoveFile(intermediate_gen_id, false);
  /* The fold leaves each key one entry, its value as of the newest version merged, so older versions are gone
     (#745), but for those the retention window keeps. The caller publishes the file after this returns. */
  RaiseHistoryHorizon(horizon_after(out_saved_high_seq));
  /* #857: count bytes written to disk in this merge (Phase A intermediate + Phase B final). */
  AddMergeBytes(merge_data_file.GetFileLength() + fold_file_length);
  return final_gen_id;
}

void TSafeRepo::ClearLocalFileCaches(size_t gen_id) {
  const Base::TUuid &repo_id = GetId();
  Disk::TLocalWalkerCache::Cache->Clear(repo_id, gen_id);
  Disk::TLocalReadFileCache<Disk::Util::LogicalPageSize,
    Disk::Util::LogicalBlockSize,
    Disk::Util::PhysicalBlockSize,
    Disk::Util::CheckedPage, true>::Cache->Clear(repo_id, gen_id);
}

void TSafeRepo::RemoveFile(size_t gen_id, bool caches_cleared) {
  Util::TBlockVec block_vec;
  ReadFileBlocks(Manager->GetEngine(), GetId(), gen_id, block_vec);
  /* Now we can go to each scheduler and remove anything they have cached about this file... */
  if (!caches_cleared) {
    Manager->ForEachScheduler([this, gen_id](Fiber::TRunner *runner) {
      Fiber::TRunner *cur_runner = Fiber::TRunner::LocalRunner;
      Fiber::SwitchTo(runner);
      ClearLocalFileCaches(gen_id);
      Fiber::SwitchTo(cur_runner);
      return true;
    });
  }
  Disk::TCompletionTrigger completion_trigger;
  try {
    Manager->GetEngine()->RemoveFile(GetId(), gen_id, completion_trigger);
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "TSafeRepo::RemoveFile error [%s]", ex.what());
    throw;
  }
  /* wait for the file to be removed from the file map */ {
    completion_trigger.Wait();
  }
  for (const auto &iter : block_vec.GetSeqBlockMap()) {
    Manager->GetEngine()->FreeSeqBlocks(iter.second.first, iter.second.second);
  }
}

size_t TSafeRepo::WriteFile(TMemoryLayer *memory_layer,
                            Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                            TSequenceNumber &out_saved_low_seq,
                            TSequenceNumber &out_saved_high_seq,
                            size_t &out_num_keys,
                            TSequenceNumber release_up_to) {
  /* A root's data file is where its updates end up, so the WAL can let go of what it kept for
     them once the file is durable.  But the file mustn't get there first: a transaction that also
     wrote to a child pov, which only the WAL rebuilds, would come back half applied (#755). */
  const auto &parent = GetParentRepo();
  TWalRetention *retention = (parent && *parent) ? nullptr : Manager->GetWalRetention();
  if (retention && memory_layer->IsEmpty()) {
    retention = nullptr;
  }
  /* The layer's own bounds, not Size-checked GetLowestSeq(): an update with no entries still
     has a record holding the head. */
  const TSequenceNumber layer_low_seq = retention ? memory_layer->GetUpdateCollection()->TryGetFirstMember()->GetSequenceNumber() : 0UL;
  const TSequenceNumber layer_high_seq = retention ? memory_layer->GetUpdateCollection()->TryGetLastMember()->GetSequenceNumber() : 0UL;
  if (retention) {
    retention->WaitWriteAhead(GetId(), layer_high_seq);
  }
  size_t gen_id = GetNextGenId();
  TDataFile data_file(Manager->GetEngine(), storage_speed, memory_layer, GetId(), gen_id, Manager->GetTempFileConsolThresh(), release_up_to, Medium/*, !static_cast<bool>(GetParentRepo())*/);
  out_num_keys = data_file.GetNumKeys();
  out_saved_low_seq = data_file.GetLowestSequence();
  out_saved_high_seq = data_file.GetHighestSequence();
  /* #857: count bytes written to disk in this memtable flush. */
  AddFlushBytes(data_file.GetFileLength());
  /* TDataFile has synced the file and waited for its entry to be written. */
  NoteDurable(out_saved_high_seq);
  if (retention) {
    retention->OnRootFlushed(GetId(), layer_low_seq, layer_high_seq);
  }
  return gen_id;
}

std::unique_ptr<Orly::Indy::TPresentWalker> TSafeRepo::NewPresentWalkerFile(size_t gen_id,
                                                                            const TIndexKey &index_from,
                                                                            const TIndexKey &index_to,
                                                                            std::optional<TSequenceNumber> at) const {
  return make_unique<Disk::TPresentWalkFileWrapper>(
      Manager->GetEngine(), GetId(), gen_id, index_from.GetIndexId(), index_from.GetKey(), index_to.GetKey(), at);
}

std::unique_ptr<Orly::Indy::TPresentWalker> TSafeRepo::NewPresentWalkerFile(size_t gen_id,
                                                                            const TIndexKey &index_key,
                                                                            std::optional<TSequenceNumber> at) const {
  return make_unique<Disk::TPresentWalkFileWrapper>(
      Manager->GetEngine(), GetId(), gen_id, index_key.GetIndexId(), index_key.GetKey(), at);
}

std::unique_ptr<Orly::Indy::TUpdateWalker> TSafeRepo::NewUpdateWalkerFile(size_t gen_id, TSequenceNumber from) const {
  return make_unique<Disk::TUpdateWalkFile>(Manager->GetEngine(), GetId(), gen_id, from);
}