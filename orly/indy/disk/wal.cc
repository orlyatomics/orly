/* <orly/indy/disk/wal.cc>

   Implements <orly/indy/disk/wal.h>.

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

#include <orly/indy/disk/wal.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <string>
#include <iostream>

#include <syslog.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace DiskUtil = Orly::Indy::Disk::Util;

/* Compile-time Castagnoli CRC32C table (0x82F63B78) */
static constexpr uint32_t GenerateCrc32cEntry(uint32_t byte) {
  uint32_t crc = byte;
  for (int j = 0; j < 8; ++j) {
    crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78U : 0U);
  }
  return crc;
}

struct TCrc32cTable {
  uint32_t Entries[256];
  constexpr TCrc32cTable() : Entries() {
    for (uint32_t i = 0; i < 256; ++i) {
      Entries[i] = GenerateCrc32cEntry(i);
    }
  }
};

static constexpr TCrc32cTable Crc32cLookupTable{};

uint32_t ComputeCrc32c(const void *data, size_t length, uint32_t initial) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  uint32_t crc = ~initial;
  for (size_t i = 0; i < length; ++i) {
    crc = (crc >> 8) ^ Crc32cLookupTable.Entries[(crc ^ p[i]) & 0xFF];
  }
  return ~crc;
}

uint64_t ComputeChecksum64(const void *data, size_t length, uint64_t seed) {
  const size_t word_count = (length + sizeof(uint64_t) - 1) / sizeof(uint64_t);
  if (word_count < 32) {
    uint64_t stack_words[33] = {0};
    memcpy(stack_words, data, length);
    stack_words[word_count] = static_cast<uint64_t>(length);
    return Base::Murmur(stack_words, word_count + 1, seed);
  }
  std::vector<uint64_t> words(word_count + 1, 0UL);
  memcpy(words.data(), data, length);
  words[word_count] = static_cast<uint64_t>(length);
  return Base::Murmur(words.data(), words.size(), seed);
}

static inline size_t AlignToWal(size_t bytes) {
  return (bytes + WalAlignment - 1UL) & ~(WalAlignment - 1UL);
}

static void EnsureLocalEventPool() {
  if (DiskUtil::TDiskController::TEvent::DiskEventPoolManager && !DiskUtil::TDiskController::TEvent::LocalEventPool) {
    DiskUtil::TDiskController::TEvent::LocalEventPool = new Base::TThreadLocalGlobalPoolManager<DiskUtil::TDiskController::TEvent>::TThreadLocalPool(DiskUtil::TDiskController::TEvent::DiskEventPoolManager.get());
  }
}

struct TDeviceWaitState {
  std::mutex Mutex;
  std::condition_variable Cv;
  bool Done = false;
  bool Ok = false;
};

static bool SyncDeviceWrite(Util::TDevice *device, const Base::TCodeLocation &loc,
                            void *buf, Util::TOffset offset, size_t size) {
  EnsureLocalEventPool();
  auto state = std::make_shared<TDeviceWaitState>();

  device->Write(loc, DiskUtil::FullPage, 0, buf, offset, size,
                DiskPriority::RealTime, false, offset,
                [state](TDiskResult res, const char */*err*/) {
                  std::lock_guard<std::mutex> lk(state->Mutex);
                  state->Ok = (res == Success);
                  state->Done = true;
                  state->Cv.notify_one();
                });

  std::unique_lock<std::mutex> lk(state->Mutex);
  state->Cv.wait(lk, [&state] { return state->Done; });
  return state->Ok;
}

static bool SyncDeviceRead(Util::TDevice *device, const Base::TCodeLocation &loc,
                           void *buf, Util::TOffset offset, size_t size) {
  EnsureLocalEventPool();
  auto state = std::make_shared<TDeviceWaitState>();

  device->Read(loc, DiskUtil::FullPage, 0, buf, offset, size,
               DiskPriority::RealTime, false,
               [state](TDiskResult res, const char */*err*/) {
                 std::lock_guard<std::mutex> lk(state->Mutex);
                 state->Ok = (res == Success);
                 state->Done = true;
                 state->Cv.notify_one();
               });

  std::unique_lock<std::mutex> lk(state->Mutex);
  state->Cv.wait(lk, [&state] { return state->Done; });
  return state->Ok;
}

TWal::TWal(Util::TDevice *device, const TConfig &config, uint64_t start_lsn, uint64_t start_group_num, uint32_t start_lap, uint64_t start_ring_offset, uint64_t previous_checksum, uint64_t head_lsn, uint64_t head_group_num, uint32_t head_lap, uint64_t head_ring_offset, uint64_t head_chain,
           const std::vector<TGroupLocation> &retained_groups)
    : Device(device),
      Config(config),
      AutoSealOnQuiet(config.AutoSealOnQuiet),
      NextLsn(start_lsn > 0 ? start_lsn - 1 : 0),
      NextGroupNum(start_group_num > 0 ? start_group_num - 1 : 0),
      CurrentLap(start_lap),
      CurrentRingOffset(start_ring_offset),
      PreviousGroupChecksum(previous_checksum),
      HighestSyncedLsn(NextLsn),
      HighestSealedLsn(NextLsn),
      DurableLsn(NextLsn),
      HeadLsn(head_lsn > 0UL ? head_lsn : start_lsn),
      HeadGroupNum(head_group_num > 0UL ? head_group_num : (start_group_num > 0UL ? start_group_num : 1UL)),
      HeadLap(head_group_num > 0UL ? head_lap : start_lap),
      HeadRingOffset(head_group_num > 0UL ? head_ring_offset : start_ring_offset),
      HeadChain(head_group_num > 0UL ? head_chain : previous_checksum),
      CheckpointedHeadLap(HeadLap),
      CheckpointedHeadRingOffset(HeadRingOffset),
      Stopping(false),
      Failed(false),
      MaxGroupPayload(std::min<size_t>(64UL * 1024UL, config.CapacityBytes > sizeof(TGroupHeader) ? config.CapacityBytes - sizeof(TGroupHeader) : 0UL)),
      MaxBatchGroupBytes(AlignToWal(sizeof(TGroupHeader) + MaxGroupPayload)),
      HeadroomBytes((config.CapacityBytes / 8UL) / WalAlignment * WalAlignment),
      ReservedGroupBytesSum(0UL),
      PendingGroupBytesSum(0UL),
      Unsealed(false) {
  assert(Device != nullptr);
  assert(Config.CapacityBytes >= WalAlignment);
  assert(Config.CapacityBytes % WalAlignment == 0);
  assert(Config.BaseOffset % WalAlignment == 0);
  /* A group of the most payload the leader batches still fits the ring. */
  assert(MaxBatchGroupBytes <= Config.CapacityBytes);

  /* The groups recovery found from the head on are still needed until a checkpoint retires them.
     Without them a checkpoint after a restart would name the write position as the head and the
     next scan would skip every record before it.  A seal-only group carries no LSN of its own: it
     retires with the records before it. */
  uint64_t last_lsn_so_far = (HeadLsn > 0UL) ? HeadLsn - 1UL : 0UL;
  for (const auto &loc : retained_groups) {
    TGroupSummary summary;
    summary.GroupNum = loc.GroupNum;
    summary.FirstLsn = loc.FirstLsn;
    summary.LastLsn = (loc.FirstLsn > 0UL) ? loc.LastLsn : last_lsn_so_far;
    summary.Lap = loc.Lap;
    summary.Offset = loc.Offset;
    summary.Chain = loc.Chain;
    last_lsn_so_far = std::max(last_lsn_so_far, summary.LastLsn);
    ActiveGroups.push_back(summary);
  }
  /* Each group's header checksum is the next one's chain; the last one's is where we go on from. */
  for (size_t i = 0; i + 1 < ActiveGroups.size(); ++i) {
    ActiveGroups[i].HeaderChecksum = ActiveGroups[i + 1].Chain;
  }
  if (!ActiveGroups.empty()) {
    ActiveGroups.back().HeaderChecksum = previous_checksum;
  }

  LeaderThread = std::thread(&TWal::LeaderMain, this);
  SyncerThread = std::thread(&TWal::SyncerMain, this);
}

TWal::~TWal() {
  Stop();
  /* A reservation points back at its ring and gives its room back when it goes, so every one must
     go first (#891): the server destroys the transactions holding them (the sessions', Tetris's
     and the repos') before it resets the WAL. */
  assert(ReservedGroupBytes.empty());
}

void TWal::Stop() {
  {
    std::lock_guard<std::mutex> lock(Mutex);
    if (Stopping) {
      return;
    }
    Stopping = true;
  }
  SpaceCv.notify_all();
  LeaderCv.notify_all();
  if (LeaderThread.joinable()) {
    LeaderThread.join();
  }
  SyncerCv.notify_all();
  if (SyncerThread.joinable()) {
    SyncerThread.join();
  }
}

TWalReservation::TWalReservation(TWalReservation &&that) noexcept
    : Wal(that.Wal), Size(that.Size), GroupBytes(that.GroupBytes), Owes(std::move(that.Owes)) {
  that.Wal = nullptr;
}

TWalReservation &TWalReservation::operator=(TWalReservation &&that) noexcept {
  if (this != &that) {
    Release();
    Wal = that.Wal;
    Size = that.Size;
    GroupBytes = that.GroupBytes;
    Owes = std::move(that.Owes);
    that.Wal = nullptr;
  }
  return *this;
}

void TWalReservation::Release() {
  if (Wal) {
    Wal->ReleaseReservation(*this);
    Wal = nullptr;
  }
}

size_t TWal::GetRecordGroupBytes(size_t size) {
  return AlignToWal(sizeof(TGroupHeader) + sizeof(TRecordHeader) + size);
}

size_t TWal::GetPromotionGroupBytes(size_t size) {
  return GetRecordGroupBytes(size + PromotionSlackBytes) + WalAlignment;
}

/* The room a set of promotion debts is owed, and the biggest group one of them may need. */
static size_t SumDebtBytes(const std::vector<TPromotionDebt> &debts) {
  size_t sum = 0UL;
  for (const auto &debt : debts) {
    sum += debt.GroupBytes * debt.Levels;
  }
  return sum;
}

static size_t MaxDebtGroupBytes(const std::vector<TPromotionDebt> &debts) {
  size_t biggest = 0UL;
  for (const auto &debt : debts) {
    if (debt.Levels) {
      biggest = std::max(biggest, debt.GroupBytes);
    }
  }
  return biggest;
}

/* Why the promise holds (#891).  Owed (OwedBytesLocked) is what the records taken but not written
   need, each group with a page for a seal-only group that may follow it: the groups the leader will
   build from PendingRecords, which it builds exactly as QueueRecordLocked predicts; for each
   reservation, a group of its own; and a page more if a group with records has been written since
   the last seal-only one.  A reserved record appended costs no more than its charge: it either
   joins the last predicted group, adding at most its own group bytes, or starts a group of its own.
   No group the leader builds is bigger than MaxGroupLocked(0), and every group it gives a position
   to takes at least its own size off Owed: a group with records in it is the first predicted one,
   and takes its size and its seal page, of which the page moves to Unsealed; a seal-only group is
   written only once every group with records before it is synced, so it uses up that page.  Usable (UsableBytesLocked) loses no more than each group's size
   as groups are placed, and only grows as checkpoints move the head on.  So once Owed fits within
   Usable it always does, and with the head standing still every record taken gets a position and
   the seal that makes it durable.  The admission check is that Owed, with the new record's charge,
   fits within Usable. */

size_t TWal::OwedBytesLocked() const {
  return ReservedGroupBytesSum + ReservedGroupBytes.size() * WalAlignment +
         PendingGroupBytesSum + PendingGroupPayloads.size() * WalAlignment + (Unsealed ? WalAlignment : 0UL);
}

size_t TWal::MaxGroupLocked(size_t extra, size_t biggest_extra) const {
  const size_t sum = ReservedGroupBytesSum + PendingGroupBytesSum + extra;
  size_t biggest = std::min(extra, biggest_extra);
  if (!ReservedGroupBytes.empty()) {
    biggest = std::max(biggest, *ReservedGroupBytes.rbegin());
  }
  if (!PendingGroupBytes.empty()) {
    biggest = std::max(biggest, *PendingGroupBytes.rbegin());
  }
  return std::max(WalAlignment, std::min(sum, std::max(MaxBatchGroupBytes, biggest)));
}

size_t TWal::UsableBytesLocked(size_t max_group) const {
  if (CurrentLap == CheckpointedHeadLap + 1U) {
    /* Free from the write position up to the head, and no further: wrapping again would pass it. */
    return CheckpointedHeadRingOffset > CurrentRingOffset ? CheckpointedHeadRingOffset - CurrentRingOffset : 0UL;
  }
  if (CurrentLap == CheckpointedHeadLap) {
    /* Free from the write position to the end of the ring, then from its start up to the head.  A
       group that doesn't fit before the end wastes what is left there, which is less than the
       group. */
    const size_t tail = Config.CapacityBytes > CurrentRingOffset ? Config.CapacityBytes - CurrentRingOffset : 0UL;
    const size_t both = tail + CheckpointedHeadRingOffset;
    return std::max(tail, both > max_group ? both - max_group : 0UL);
  }
  return 0UL;
}

/* Promotion room (#959) is promised the same way, as though the promotions were records already
   taken: Owed and the debt together fit within Usable, so long as the head stands still, and a
   record may be admitted only if that stays true once it is in.  A promotion may count the debt its
   pops repay as free: that room was being kept for it.  The record and whatever its pushes owe at
   the next level up then take no more than that (GetPromotionGroupBytes leaves the slack for it),
   so the promise still holds once the retention repays the pops.  The groups the debt is owed may
   be bigger than any taken yet, so where a group wraps, allow for the biggest of them too. */
bool TWal::AdmitsLocked(size_t group_bytes, TRoom room, size_t credit, size_t new_debt, size_t new_debt_group, size_t biggest_group) const {
  const size_t debt = DebtBytesSum - std::min(credit, DebtBytesSum) + new_debt;
  const size_t needed = OwedBytesLocked() + group_bytes + WalAlignment + debt;
  size_t max_group = std::max(MaxGroupLocked(group_bytes, biggest_group), new_debt_group);
  if (!DebtGroupBytes.empty()) {
    max_group = std::max(max_group, *DebtGroupBytes.rbegin());
  }
  const size_t usable = UsableBytesLocked(max_group);
  if (room == TRoom::KeepHeadroom) {
    return usable >= HeadroomBytes && needed <= usable - HeadroomBytes;
  }
  return needed <= usable;
}

void TWal::ThrowRingFullLocked(size_t size, TRoom room) const {
  static std::atomic<size_t> refusals(0UL);
  const size_t n = ++refusals;
  RingFullRefusals.fetch_add(1UL, std::memory_order_relaxed);
  const size_t owed = OwedBytesLocked();
  const size_t usable = UsableBytesLocked(MaxGroupLocked(GetRecordGroupBytes(size)));
  /* LOG_ERR, not WARNING: the default mask drops warnings.  The 1st, 2nd, 4th, ... */
  if ((n & (n - 1UL)) == 0UL) {
    syslog(LOG_ERR, "WAL: ring full; refused a record of %ld bytes (%ld KiB owed to records not yet written, %ld KiB to promotions, %ld KiB usable before the checkpointed head at lap %u offset %ld, %ld KiB kept as headroom%s); %ld refused so far",
           size, owed >> 10, DebtBytesSum >> 10, usable >> 10, CheckpointedHeadLap, CheckpointedHeadRingOffset, HeadroomBytes >> 10,
           room == TRoom::KeepHeadroom ? "" : ", not for this record", n);
  }
  throw TWalRingFull("the WAL ring is full up to its checkpointed head; refused a record of " + std::to_string(size) +
                     " bytes, and nothing was appended; retry once a root flush and checkpoint have made room");
}

void TWal::ChargeLocked(size_t group_bytes) {
  ReservedGroupBytes.insert(group_bytes);
  ReservedGroupBytesSum += group_bytes;
}

void TWal::OweLocked(const std::vector<TPromotionDebt> &debts) {
  for (const auto &debt : debts) {
    if (debt.Levels) {
      DebtGroupBytes.insert(debt.GroupBytes);
      DebtBytesSum += debt.GroupBytes * debt.Levels;
    }
  }
}

void TWal::RepayLocked(const std::vector<TPromotionDebt> &debts) {
  for (const auto &debt : debts) {
    if (!debt.Levels) {
      continue;
    }
    auto iter = DebtGroupBytes.find(debt.GroupBytes);
    const size_t bytes = debt.GroupBytes * debt.Levels;
    assert(iter != DebtGroupBytes.end() && bytes <= DebtBytesSum);
    if (iter == DebtGroupBytes.end() || bytes > DebtBytesSum) {
      syslog(LOG_ERR, "WAL: repaid %ld bytes of promotion room that were not owed (%ld owed)", bytes, DebtBytesSum);
    }
    if (iter != DebtGroupBytes.end()) {
      DebtGroupBytes.erase(iter);
    }
    DebtBytesSum -= std::min(bytes, DebtBytesSum);
  }
}

void TWal::ChangePromotionDebt(const TPromotionDebtChange &debt) {
  /* lock */ {
    std::lock_guard<std::mutex> lock(Mutex);
    OweLocked(debt.Owed);
    RepayLocked(debt.Repaid);
  }
  SpaceCv.notify_all();
}

size_t TWal::GetPromotionDebtBytes() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return DebtBytesSum;
}

void TWal::UnchargeLocked(size_t group_bytes) {
  auto iter = ReservedGroupBytes.find(group_bytes);
  assert(iter != ReservedGroupBytes.end());
  if (iter != ReservedGroupBytes.end()) {
    ReservedGroupBytes.erase(iter);
    ReservedGroupBytesSum -= group_bytes;
  }
}

void TWal::QueueRecordLocked(size_t encoded) {
  /* The leader's rule: a group takes records while they fit, and a first record however big. */
  if (!PendingGroupPayloads.empty() && PendingGroupPayloads.back() + encoded <= MaxGroupPayload) {
    size_t &payload = PendingGroupPayloads.back();
    const size_t was = AlignToWal(sizeof(TGroupHeader) + payload);
    payload += encoded;
    const size_t now = AlignToWal(sizeof(TGroupHeader) + payload);
    if (now != was) {
      PendingGroupBytes.erase(PendingGroupBytes.find(was));
      PendingGroupBytes.insert(now);
      PendingGroupBytesSum += now - was;
    }
    return;
  }
  PendingGroupPayloads.push_back(encoded);
  const size_t bytes = AlignToWal(sizeof(TGroupHeader) + encoded);
  PendingGroupBytes.insert(bytes);
  PendingGroupBytesSum += bytes;
}

void TWal::DequeueGroupLocked(size_t payload) {
  assert(!PendingGroupPayloads.empty() && PendingGroupPayloads.front() == payload);
  if (PendingGroupPayloads.empty()) {
    return;
  }
  if (PendingGroupPayloads.front() != payload) {
    syslog(LOG_ERR, "WAL: the leader built a group of %ld payload bytes where the accounting expected %ld", payload, PendingGroupPayloads.front());
  }
  const size_t bytes = AlignToWal(sizeof(TGroupHeader) + PendingGroupPayloads.front());
  PendingGroupPayloads.pop_front();
  PendingGroupBytes.erase(PendingGroupBytes.find(bytes));
  PendingGroupBytesSum -= bytes;
}

void TWal::CheckAppendableLocked(size_t size) const {
  if (Failed) {
    throw TWalIoError("WAL is in failed state");
  }
  if (Stopping) {
    throw TWalIoError("WAL is stopping");
  }
  /* A group bigger than the ring could never be written: the leader would wait for room forever. */
  if (GetRecordGroupBytes(size) > Config.CapacityBytes) {
    throw TWalIoError("WAL record of " + std::to_string(size) + " bytes is bigger than the ring");
  }
}

TWalReservation TWal::Reserve(size_t size, TRoom room, std::chrono::milliseconds wait, size_t credit, std::vector<TPromotionDebt> owes) {
  std::unique_lock<std::mutex> lock(Mutex);
  CheckAppendableLocked(size);
  const size_t group_bytes = GetRecordGroupBytes(size);
  const size_t new_debt = SumDebtBytes(owes);
  const size_t new_debt_group = MaxDebtGroupBytes(owes);
  /* Its promotion needs about as much again, from the headroom alone if client writes have filled
     the rest; half of it leaves room for what the promotion adds (its pop, its seal). */
  if (room == TRoom::KeepHeadroom && group_bytes > HeadroomBytes / 2UL) {
    throw TWalRecordTooLarge("write too large: its WAL record of " + std::to_string(size) + " bytes is more than half the " +
                             std::to_string(HeadroomBytes >> 10) + " KiB the WAL keeps for promotions (an eighth of --wal_mb), "
                             "so it could be accepted and never promoted; retrying won't help, split it into smaller batches "
                             "or raise --wal_mb");
  }
  const auto admits = [&] { return AdmitsLocked(group_bytes, room, credit, new_debt, new_debt_group); };
  if (!admits()) {
    if (wait.count() > 0) {
      SpaceCv.wait_for(lock, wait, [this, &admits] { return Failed || Stopping || admits(); });
      CheckAppendableLocked(size);
    }
    if (!admits()) {
      ThrowRingFullLocked(size, room);
    }
  }
  ChargeLocked(group_bytes);
  OweLocked(owes);
  TWalReservation reservation;
  reservation.Wal = this;
  reservation.Size = size;
  reservation.GroupBytes = group_bytes;
  reservation.Owes = std::move(owes);
  return reservation;
}

std::vector<TWalReservation> TWal::ReserveGroup(const std::vector<size_t> &sizes, const std::vector<TPromotionDebt> &spend) {
  std::vector<TWalReservation> reservations;
  reservations.reserve(sizes.size());
  std::unique_lock<std::mutex> lock(Mutex);
  size_t group_bytes = 0UL, biggest = 0UL, biggest_size = 0UL;
  for (const size_t size : sizes) {
    CheckAppendableLocked(size);
    const size_t bytes = GetRecordGroupBytes(size);
    group_bytes += bytes;
    if (bytes > biggest) {
      biggest = bytes;
      biggest_size = size;
    }
  }
  /* A seal-only group may follow each record (the WalAlignment in AdmitsLocked is the one for a
     single record); count the others too. */
  const size_t seals = sizes.empty() ? 0UL : (sizes.size() - 1UL) * WalAlignment;
  if (!AdmitsLocked(group_bytes + seals, TRoom::All, SumDebtBytes(spend), 0UL, 0UL, biggest)) {
    /* Not ThrowRingFullLocked: the caller tries again at its next pass, and says so itself. */
    throw TWalRingFull("the WAL ring has no room yet for " + std::to_string(sizes.size()) + " records of up to " +
                       std::to_string(biggest_size) + " bytes; nothing was appended");
  }
  RepayLocked(spend);
  for (const size_t size : sizes) {
    const size_t bytes = GetRecordGroupBytes(size);
    ChargeLocked(bytes);
    TWalReservation reservation;
    reservation.Wal = this;
    reservation.Size = size;
    reservation.GroupBytes = bytes;
    reservations.push_back(std::move(reservation));
  }
  return reservations;
}

void TWal::ReleaseReservation(TWalReservation &reservation) {
  /* lock */ {
    std::lock_guard<std::mutex> lock(Mutex);
    UnchargeLocked(reservation.GroupBytes);
    RepayLocked(reservation.Owes);
    reservation.Owes.clear();
  }
  SpaceCv.notify_all();
}

size_t TWal::GetOwedBytes() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return OwedBytesLocked();
}

uint64_t TWal::Append(TWalRecordType type, const void *data, size_t size, uint8_t flags, const TPromotionDebtChange &debt) {
  std::unique_lock<std::mutex> lock(Mutex);
  CheckAppendableLocked(size);
  const size_t group_bytes = GetRecordGroupBytes(size);
  if (!AdmitsLocked(group_bytes, TRoom::All, SumDebtBytes(debt.Repaid), SumDebtBytes(debt.Owed), MaxDebtGroupBytes(debt.Owed))) {
    ThrowRingFullLocked(size, TRoom::All);
  }
  OweLocked(debt.Owed);
  RepayLocked(debt.Repaid);
  if (!debt.Repaid.empty()) {
    SpaceCv.notify_all();
  }
  return AppendLocked(type, data, size, flags);
}

uint64_t TWal::Append(TWalReservation &reservation, TWalRecordType type, const void *data, size_t size, uint8_t flags, const TPromotionDebtChange &debt) {
  assert(reservation.Wal == this);
  if (reservation.Wal != this) {
    throw std::logic_error("TWal::Append: the reservation is not this WAL's");
  }
  std::unique_lock<std::mutex> lock(Mutex);
  CheckAppendableLocked(size);
  /* The record takes over the reservation's room: it costs no more than the charge, so long as it
     is no bigger than what was reserved. */
  UnchargeLocked(reservation.GroupBytes);
  /* What the record leaves owed takes over from what the reservation held: the same, unless a push
     was refused (and owes nothing) or the record came out bigger. */
  RepayLocked(reservation.Owes);
  reservation.Owes.clear();
  OweLocked(debt.Owed);
  RepayLocked(debt.Repaid);
  if (GetRecordGroupBytes(size) > reservation.GroupBytes && !AdmitsLocked(GetRecordGroupBytes(size), TRoom::All)) {
    /* It has to go in all the same, since the caller can no longer back out, but the ring may then
       not have room for it with the head where it is. */
    syslog(LOG_ERR, "WAL: a record of %ld bytes was reserved room for %ld bytes; appending it beyond what the ring can promise",
           size, reservation.Size);
  }
  reservation.Wal = nullptr;
  SpaceCv.notify_all();
  return AppendLocked(type, data, size, flags);
}

uint64_t TWal::AppendLocked(TWalRecordType type, const void *data, size_t size, uint8_t flags) {
  const uint64_t lsn = ++NextLsn;
  TPendingRecord rec;
  rec.Lsn = lsn;
  rec.Type = type;
  rec.Flags = flags;

  rec.Encoded.resize(sizeof(TRecordHeader) + size);
  TRecordHeader header;
  header.Length = static_cast<uint32_t>(rec.Encoded.size());
  header.Type = static_cast<uint8_t>(type);
  header.Flags = flags;
  header.Reserved = 0;
  header.Lsn = lsn;
  header.BodyCrc32c = ComputeCrc32c(data, size);

  memcpy(rec.Encoded.data(), &header, sizeof(TRecordHeader));
  if (size > 0) {
    memcpy(rec.Encoded.data() + sizeof(TRecordHeader), data, size);
  }

  QueueRecordLocked(rec.Encoded.size());
  PendingRecords.push_back(std::move(rec));
  LeaderCv.notify_one();
  return lsn;
}

uint64_t TWal::AppendAndWait(TWalRecordType type, const void *data, size_t size, uint8_t flags) {
  const uint64_t lsn = Append(type, data, size, flags);
  WaitForDurable(lsn);
  return lsn;
}

void TWal::WaitForDurable(uint64_t lsn) {
  std::unique_lock<std::mutex> lock(Mutex);
  DurableCv.wait(lock, [this, lsn] {
    return Failed || DurableLsn >= lsn;
  });
  if (Failed && DurableLsn < lsn) {
    throw TWalIoError("WAL I/O or fsync failed before record became durable");
  }
}

bool TWal::WaitForDurable(uint64_t lsn, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(Mutex);
  const bool reached = DurableCv.wait_for(lock, timeout, [this, lsn] {
    return Failed || DurableLsn >= lsn;
  });
  if (Failed && DurableLsn < lsn) {
    throw TWalIoError("WAL I/O or fsync failed before record became durable");
  }
  return reached && (DurableLsn >= lsn);
}

void TWal::Flush() {
  uint64_t target_lsn = 0UL;
  {
    std::lock_guard<std::mutex> lock(Mutex);
    target_lsn = NextLsn;
  }
  if (target_lsn > 0) {
    LeaderCv.notify_one();
    WaitForDurable(target_lsn);
  }
}

bool TWal::IsFailed() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return Failed;
}

uint64_t TWal::GetLastAppendedLsn() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return NextLsn;
}

uint64_t TWal::GetDurableLsn() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return DurableLsn;
}

uint64_t TWal::GetHeadLsn() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return HeadLsn;
}

uint32_t TWal::GetLap() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return CurrentLap;
}

uint64_t TWal::GetGroupNum() const {
  std::lock_guard<std::mutex> lock(Mutex);
  return NextGroupNum;
}

void TWal::SetAutoSealOnQuietForTest(bool on) {
  /* lock */ {
    std::lock_guard<std::mutex> lock(Mutex);
    AutoSealOnQuiet = on;
  }
  LeaderCv.notify_all();
}

void TWal::SetBeforeSyncForTest(std::function<void ()> hook) {
  std::lock_guard<std::mutex> lock(Mutex);
  BeforeSyncForTest = std::move(hook);
}

void TWal::LeaderMain() {
  std::unique_lock<std::mutex> lock(Mutex);
  while (!Stopping || !PendingRecords.empty() || (HighestSyncedLsn > HighestSealedLsn && InFlightGroups.empty())) {
    while (!Stopping &&
           !(!PendingRecords.empty() && InFlightGroups.size() < Config.MaxInFlightGroups) &&
           !(AutoSealOnQuiet && HighestSyncedLsn > HighestSealedLsn && InFlightGroups.empty())) {
      if (AutoSealOnQuiet && HighestSyncedLsn > HighestSealedLsn) {
        LeaderCv.wait_for(lock, Config.QuietInterval);
      } else {
        LeaderCv.wait(lock);
      }
    }
    if (Failed) {
      break;
    }
    if (Stopping && PendingRecords.empty() && HighestSyncedLsn <= HighestSealedLsn && InFlightGroups.empty()) {
      break;
    }

    /* Build next group */
    const bool have_records = !PendingRecords.empty() && InFlightGroups.size() < Config.MaxInFlightGroups;
    /* Stopping, seal what is synced even with auto-seal off: the loop above waits for it. */
    const bool need_seal = !have_records && (AutoSealOnQuiet || Stopping) && HighestSyncedLsn > HighestSealedLsn && InFlightGroups.size() < Config.MaxInFlightGroups;

    if (!have_records && !need_seal) {
      /* Only when stopping with every in-flight slot taken: wait for the syncer to free one.  Going
         round again at once would hold Mutex, which the syncer needs to free it, for good. */
      LeaderCv.wait(lock);
      continue;
    }

    std::vector<TPendingRecord> batch;
    size_t payload_bytes = 0UL;
    if (have_records) {
      while (!PendingRecords.empty() && (payload_bytes == 0 || payload_bytes + PendingRecords.front().Encoded.size() <= MaxGroupPayload)) {
        payload_bytes += PendingRecords.front().Encoded.size();
        batch.push_back(std::move(PendingRecords.front()));
        PendingRecords.pop_front();
      }
    }

    const size_t unpadded_size = sizeof(TGroupHeader) + payload_bytes;
    const size_t group_size = AlignToWal(unpadded_size);

    /* Allocate ring position, but never over the head the newest synced checkpoint names: recovery
       scans from there, so everything from it to the write position is still needed (§5.6).  When
       the ring is full we wait for a checkpoint to move the head on.  The wait comes before the
       group takes its number, so a checkpoint taken meanwhile names this group's number and
       position as the head when nothing older is left. */
    const auto fits = [this, group_size] {
      uint32_t lap = CurrentLap;
      size_t offset = CurrentRingOffset;
      if (offset + group_size > Config.CapacityBytes) {
        ++lap;
        offset = 0UL;
      }
      if (lap == CheckpointedHeadLap) {
        return true;
      }
      return lap == CheckpointedHeadLap + 1U && offset + group_size <= CheckpointedHeadRingOffset;
    };
    if (!fits()) {
      /* The ring-space accounting promises every record it takes a position with the head where it
         is (#891), so this wait means the accounting is wrong: a flush may be waiting on one of
         these records, and then nothing moves the head. */
      static std::atomic<size_t> ring_full_waits(0UL);
      const size_t n = ++ring_full_waits;
      if ((n & (n - 1UL)) == 0UL) {
        syslog(LOG_ERR, "WAL: ring full up to the checkpointed head (lap %u offset %ld) for a group the accounting had room for; waiting for a checkpoint (%ld times so far)",
               CheckpointedHeadLap, CheckpointedHeadRingOffset, n);
      }
      LeaderCv.wait(lock, [this, &fits] { return Stopping || Failed || fits(); });
      if (!fits()) {
        /* Stopping or failed with the ring full: these records can't be written, so they never
           become durable.  Fail so anyone waiting on them hears about it. */
        Failed = true;
        DurableCv.notify_all();
        SyncerCv.notify_all();
        break;
      }
    }
    const uint64_t group_num = ++NextGroupNum;
    const uint64_t first_lsn = batch.empty() ? 0UL : batch.front().Lsn;
    const uint64_t sealed_through = HighestSyncedLsn;
    const uint64_t last_lsn = batch.empty() ? sealed_through : batch.back().Lsn;
    const bool is_seal_only = batch.empty();

    if (CurrentRingOffset + group_size > Config.CapacityBytes) {
      ++CurrentLap;
      CurrentRingOffset = 0UL;
    }
    const size_t group_offset = CurrentRingOffset;
    CurrentRingOffset += group_size;
    /* The records have their room now (#891). */
    if (!is_seal_only) {
      DequeueGroupLocked(payload_bytes);
    }
    Unsealed = !is_seal_only;
    SpaceCv.notify_all();

    TGroupHeader header;
    memcpy(header.Magic, "ORLYWAL1", 8);
    header.FormatVersion = WalFormatVersion;
    header.Flags = is_seal_only ? WalFlags::SealOnly : 0U;
    header.StoreId = Config.StoreId;
    header.Lap = CurrentLap;
    header.GroupNum = group_num;
    header.FirstLsn = first_lsn;
    header.RecordCount = static_cast<uint32_t>(batch.size());
    header.PayloadLen = static_cast<uint32_t>(payload_bytes);
    header.SealedThrough = sealed_through;
    header.Chain = PreviousGroupChecksum;

    /* Assemble buffer */
    auto aligned_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), group_size);
    char *buf_ptr = aligned_buf.get();

    char *payload_dest = buf_ptr + sizeof(TGroupHeader);
    for (const auto &rec : batch) {
      memcpy(payload_dest, rec.Encoded.data(), rec.Encoded.size());
      payload_dest += rec.Encoded.size();
    }

    header.PayloadChecksum = (payload_bytes > 0) ? ComputeChecksum64(buf_ptr + sizeof(TGroupHeader), payload_bytes) : 0UL;
    header.HeaderChecksum = ComputeChecksum64(&header, GroupHeaderChecksumCoveredBytes);
    memcpy(buf_ptr, &header, sizeof(TGroupHeader));

    PreviousGroupChecksum = header.HeaderChecksum;

    TGroupSummary summary;
    summary.GroupNum = group_num;
    summary.FirstLsn = first_lsn;
    summary.LastLsn = last_lsn;
    summary.Lap = CurrentLap;
    summary.Offset = group_offset;
    summary.HeaderChecksum = header.HeaderChecksum;
    summary.Chain = header.Chain;
    ActiveGroups.push_back(summary);

    /* The syncer may only sync a group once its write has completed, so it joins InFlightGroups
       after the write. */
    TInFlightGroup in_flight;
    in_flight.GroupNum = group_num;
    in_flight.FirstLsn = first_lsn;
    in_flight.LastLsn = last_lsn;
    in_flight.SealedThrough = sealed_through;
    in_flight.Lap = CurrentLap;
    in_flight.Offset = group_offset;
    in_flight.Size = group_size;
    in_flight.IsSealOnly = is_seal_only;

    lock.unlock();

    const bool write_ok = SyncDeviceWrite(Device, HERE, buf_ptr, Config.BaseOffset + group_offset, group_size);

    lock.lock();
    if (!write_ok) {
      Failed = true;
      DurableCv.notify_all();
      SyncerCv.notify_all();
      break;
    }

    InFlightGroups.push_back(in_flight);
    SyncerCv.notify_one();

    if (Config.EarlyAck) {
      if (!is_seal_only) {
        HighestSyncedLsn = std::max(HighestSyncedLsn, last_lsn);
      }
      HighestSealedLsn = std::max(HighestSealedLsn, last_lsn);
      DurableLsn = HighestSealedLsn;
      DurableCv.notify_all();
    }
  }
  /* Every path out of the loop holds Mutex. */
  LeaderStopped = true;
  SyncerCv.notify_all();
  lock.unlock();
  if (DiskUtil::TDiskController::TEvent::LocalEventPool) {
    delete DiskUtil::TDiskController::TEvent::LocalEventPool;
    DiskUtil::TDiskController::TEvent::LocalEventPool = nullptr;
  }
}

void TWal::SyncerMain() {
  std::unique_lock<std::mutex> lock(Mutex);
  /* Run until the leader has stopped, not merely been told to: stopping with records queued, it
     still writes groups, and with every in-flight slot taken it waits for this thread to free one
     (#963).  Leaving on Stopping with InFlightGroups empty stranded it. */
  while (!LeaderStopped || !InFlightGroups.empty()) {
    while (!LeaderStopped && InFlightGroups.empty() && !Failed) {
      SyncerCv.wait(lock);
    }
    if (Failed) {
      break;
    }
    if (InFlightGroups.empty() && LeaderStopped) {
      break;
    }

    /* Every group in the list has been written, so one sync covers them all. */
    const size_t num_synced = InFlightGroups.size();

    const std::function<void ()> before_sync = BeforeSyncForTest;
    lock.unlock();

    if (before_sync) {
      before_sync();
    }
    bool sync_threw = false;
    if (!Config.NoSync) {
      try {
        Device->Sync();
      } catch (const std::exception &/*ex*/) {
        sync_threw = true;
      }
    }

    lock.lock();

    if (sync_threw) {
      Failed = true;
      DurableCv.notify_all();
      LeaderCv.notify_all();
      break;
    }

    for (size_t i = 0; i < num_synced; ++i) {
      const TInFlightGroup &group = InFlightGroups.front();
      if (!group.IsSealOnly) {
        HighestSyncedLsn = std::max(HighestSyncedLsn, group.LastLsn);
      }
      HighestSealedLsn = std::max(HighestSealedLsn, group.SealedThrough);
      InFlightGroups.pop_front();
    }
    DurableLsn = HighestSealedLsn;

    DurableCv.notify_all();
    LeaderCv.notify_all();
  }
}

void TWal::Checkpoint(uint64_t head_lsn, uint64_t global_flushed_seq, const void *payload, size_t payload_len) {
  std::lock_guard<std::mutex> checkpoint_lock(CheckpointMutex);
  std::unique_lock<std::mutex> lock(Mutex);
  if (Failed) {
    throw TWalIoError("WAL is in failed state");
  }

  /* The head is the oldest record recovery still needs; it never moves back, nor past the next LSN. */
  HeadLsn = std::min(std::max(HeadLsn, head_lsn), NextLsn + 1UL);
  while (!ActiveGroups.empty() && ActiveGroups.front().LastLsn < HeadLsn) {
    ActiveGroups.pop_front();
  }
  if (!ActiveGroups.empty()) {
    HeadGroupNum = ActiveGroups.front().GroupNum;
    HeadLap = ActiveGroups.front().Lap;
    HeadRingOffset = ActiveGroups.front().Offset;
    HeadChain = ActiveGroups.front().Chain;
  } else {
    /* Every written group is retired: the head is the next group, wherever the leader puts it. */
    HeadGroupNum = NextGroupNum + 1;
    HeadLap = CurrentLap;
    HeadRingOffset = CurrentRingOffset;
    HeadChain = PreviousGroupChecksum;
  }

  const uint64_t cur_head_lsn = HeadLsn;
  const uint64_t cur_head_group = HeadGroupNum;
  const uint32_t cur_head_lap = HeadLap;
  const uint64_t cur_head_offset = HeadRingOffset;
  const uint64_t cur_head_chain = HeadChain;
  const uint64_t cur_durable_lsn = DurableLsn;
  lock.unlock();

  /* Alternate slots.  The first checkpoint of this instance goes opposite the newest on disk. */
  if (!LastCheckpointNum) {
    auto newest_cp = ReadNewestCheckpoint(Device, Config.CheckpointSlot0Offset, Config.CheckpointSlot1Offset, Config.StoreId, Config.CheckpointSlotBytes);
    LastCheckpointNum = newest_cp ? newest_cp->CheckpointNum : 0UL;
  }
  const uint64_t next_cp_num = *LastCheckpointNum + 1UL;
  const Util::TOffset target_slot = (next_cp_num % 2 == 1) ? Config.CheckpointSlot0Offset : Config.CheckpointSlot1Offset;

  const size_t total_bytes = AlignToWal(sizeof(TCheckpointHeader) + payload_len);
  if (Config.CheckpointSlotBytes && total_bytes > Config.CheckpointSlotBytes) {
    throw std::length_error("checkpoint payload of " + std::to_string(payload_len) + " bytes does not fit a checkpoint slot of " + std::to_string(Config.CheckpointSlotBytes) + " bytes");
  }
  auto buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), total_bytes);

  TCheckpointHeader header;
  memcpy(header.Magic, "ORLYCHK1", 8);
  header.FormatVersion = WalFormatVersion;
  header.Reserved = 0;
  header.StoreId = Config.StoreId;
  header.CheckpointNum = next_cp_num;
  header.HeadLsn = cur_head_lsn;
  header.DurableLsn = cur_durable_lsn;
  header.GlobalFlushedSeq = global_flushed_seq;
  header.HeadGroupNum = cur_head_group;
  header.HeadLap = cur_head_lap;
  header.Reserved2 = 0;
  header.HeadRingOffset = cur_head_offset;
  header.HeadChain = cur_head_chain;
  header.PayloadLen = static_cast<uint32_t>(payload_len);
  header.Reserved3 = 0;

  if (payload && payload_len > 0) {
    memcpy(buf.get() + sizeof(TCheckpointHeader), payload, payload_len);
    header.PayloadChecksum = ComputeChecksum64(payload, payload_len);
  } else {
    header.PayloadChecksum = 0UL;
  }
  header.HeaderChecksum = ComputeChecksum64(&header, CheckpointHeaderChecksumCoveredBytes);
  memcpy(buf.get(), &header, sizeof(TCheckpointHeader));

  bool ok = SyncDeviceWrite(Device, HERE, buf.get(), target_slot, total_bytes);
  if (ok && !Config.NoSync) {
    try {
      Device->Sync();
    } catch (const std::exception &/*ex*/) {
      ok = false;
    }
  }
  lock.lock();
  if (!ok) {
    Failed = true;
    DurableCv.notify_all();
    LeaderCv.notify_all();
    throw TWalIoError("Failed to write checkpoint slot");
  }
  LastCheckpointNum = next_cp_num;
  /* Recovery now scans from the new head, so the leader may reuse the ring up to it. */
  CheckpointedHeadLap = cur_head_lap;
  CheckpointedHeadRingOffset = cur_head_offset;
  LeaderCv.notify_all();
  SpaceCv.notify_all();
}

size_t TWal::GetRingBytesBehind(uint64_t lsn) const {
  std::lock_guard<std::mutex> lock(Mutex);
  for (const auto &group : ActiveGroups) {
    if (group.FirstLsn > 0UL && group.FirstLsn <= lsn && lsn <= group.LastLsn) {
      return static_cast<size_t>(CurrentLap - group.Lap) * Config.CapacityBytes + CurrentRingOffset - group.Offset;
    }
  }
  return 0UL;
}

std::vector<uint64_t> TWal::CopyForward(const std::vector<std::pair<TWalRecordType, std::vector<char>>> &records) {
  std::vector<uint64_t> new_lsns;
  new_lsns.reserve(records.size());
  for (const auto &entry : records) {
    new_lsns.push_back(Append(entry.first, entry.second.data(), entry.second.size(), WalRecordFlags::Copied));
  }
  Flush();
  return new_lsns;
}

std::optional<TCheckpoint> TWal::ReadNewestCheckpoint(Util::TDevice *device, Util::TOffset slot0, Util::TOffset slot1, const Base::TUuid &store_id, size_t slot_bytes) {
  auto read_slot = [device, &store_id, slot_bytes](Util::TOffset offset) -> std::optional<TCheckpoint> {
    auto buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
    const bool read_ok = SyncDeviceRead(device, HERE, buf.get(), offset, WalAlignment);
    if (!read_ok) {
      return std::nullopt;
    }
    const TCheckpointHeader *hdr = reinterpret_cast<const TCheckpointHeader *>(buf.get());
    if (memcmp(hdr->Magic, "ORLYCHK1", 8) != 0 || hdr->FormatVersion != WalFormatVersion || hdr->StoreId != store_id) {
      return std::nullopt;
    }
    if (hdr->HeaderChecksum != ComputeChecksum64(hdr, CheckpointHeaderChecksumCoveredBytes)) {
      return std::nullopt;
    }
    if (hdr->PayloadLen > 0) {
      const size_t total_size = AlignToWal(sizeof(TCheckpointHeader) + hdr->PayloadLen);
      if (slot_bytes && total_size > slot_bytes) {
        return std::nullopt;
      }
      auto full_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), total_size);
      const bool full_read_ok = SyncDeviceRead(device, HERE, full_buf.get(), offset, total_size);
      if (!full_read_ok) {
        return std::nullopt;
      }
      if (hdr->PayloadChecksum != ComputeChecksum64(full_buf.get() + sizeof(TCheckpointHeader), hdr->PayloadLen)) {
        return std::nullopt;
      }
      TCheckpoint cp;
      cp.CheckpointNum = hdr->CheckpointNum;
      cp.HeadLsn = hdr->HeadLsn;
      cp.DurableLsn = hdr->DurableLsn;
      cp.GlobalFlushedSeq = hdr->GlobalFlushedSeq;
      cp.HeadGroupNum = hdr->HeadGroupNum;
      cp.HeadLap = hdr->HeadLap;
      cp.HeadRingOffset = hdr->HeadRingOffset;
      cp.HeadChain = hdr->HeadChain;
      cp.Payload.assign(full_buf.get() + sizeof(TCheckpointHeader), full_buf.get() + sizeof(TCheckpointHeader) + hdr->PayloadLen);
      return cp;
    }
    TCheckpoint cp;
    cp.CheckpointNum = hdr->CheckpointNum;
    cp.HeadLsn = hdr->HeadLsn;
    cp.DurableLsn = hdr->DurableLsn;
    cp.GlobalFlushedSeq = hdr->GlobalFlushedSeq;
    cp.HeadGroupNum = hdr->HeadGroupNum;
    cp.HeadLap = hdr->HeadLap;
    cp.HeadRingOffset = hdr->HeadRingOffset;
    cp.HeadChain = hdr->HeadChain;
    return cp;
  };

  auto cp0 = read_slot(slot0);
  auto cp1 = read_slot(slot1);

  if (cp0 && cp1) {
    return (cp0->CheckpointNum >= cp1->CheckpointNum) ? cp0 : cp1;
  }
  return cp0 ? cp0 : cp1;
}

TScanResult TWal::Scan(Util::TDevice *device, const TConfig &config, uint64_t start_group_num, uint64_t start_lsn, uint32_t start_lap, uint64_t start_ring_offset, uint64_t start_chain) {
  TScanResult result;
  result.Status = TScanStatus::Clean;
  result.LastValidLap = start_lap;
  result.NextRingOffset = start_ring_offset;
  result.LastGroupChecksum = start_chain;
  result.StartGroupNum = start_group_num;

  uint64_t expected_group = start_group_num;
  uint32_t expected_lap = start_lap;
  uint64_t expected_chain = start_chain;
  size_t ring_offset = start_ring_offset;

  while (true) {
    /* If at end of capacity, wrap */
    if (ring_offset >= config.CapacityBytes) {
      ring_offset = 0UL;
      ++expected_lap;
    }

    auto page_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
    const bool read_ok = SyncDeviceRead(device, HERE, page_buf.get(), config.BaseOffset + ring_offset, WalAlignment);

    if (!read_ok) {
      result.Status = TScanStatus::TornTail;
      result.StoppedAtGroupNum = expected_group;
      result.ProblemDescription = "Device read error at offset " + std::to_string(ring_offset);
      break;
    }

    const TGroupHeader *hdr = reinterpret_cast<const TGroupHeader *>(page_buf.get());

    /* Check magic and whether header belongs to current or future epoch */
    if (memcmp(hdr->Magic, "ORLYWAL1", 8) != 0 ||
        (hdr->Lap < expected_lap) ||
        (hdr->Lap == expected_lap && hdr->GroupNum < expected_group)) {
      /* Not a current group header. Check if the ring wrapped to offset 0 */
      if (ring_offset > 0) {
        auto wrap_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
        const bool wrap_read_ok = SyncDeviceRead(device, HERE, wrap_buf.get(), config.BaseOffset, WalAlignment);
        if (wrap_read_ok) {
          const TGroupHeader *wrap_hdr = reinterpret_cast<const TGroupHeader *>(wrap_buf.get());
          if (memcmp(wrap_hdr->Magic, "ORLYWAL1", 8) == 0 &&
              wrap_hdr->GroupNum == expected_group &&
              wrap_hdr->Lap == expected_lap + 1 &&
              wrap_hdr->Chain == expected_chain &&
              wrap_hdr->HeaderChecksum == ComputeChecksum64(wrap_hdr, GroupHeaderChecksumCoveredBytes)) {
            /* Ring wrapped cleanly! */
            ring_offset = 0UL;
            ++expected_lap;
            continue;
          }
        }
      }

      /* Clean end of log / unwritten tail */
      if (expected_group == start_group_num) {
        result.Status = (start_group_num == 1) ? TScanStatus::Empty : TScanStatus::Clean;
      } else {
        result.Status = TScanStatus::Clean;
      }
      break;
    }

    /* Validate header fields */
    const bool valid_header =
        (hdr->FormatVersion == WalFormatVersion) &&
        (hdr->StoreId == config.StoreId) &&
        (hdr->Lap == expected_lap) &&
        (hdr->GroupNum == expected_group) &&
        (hdr->Chain == expected_chain) &&
        (hdr->HeaderChecksum == ComputeChecksum64(hdr, GroupHeaderChecksumCoveredBytes));

    if (!valid_header) {
      /* Group k is invalid! Perform Classification (§5.3 & §6) */
      result.StoppedAtGroupNum = expected_group;

      const uint64_t expected_lsn = (result.LastValidLsn > 0) ? (result.LastValidLsn + 1) : start_lsn;

      /* Read ahead through remainder of ring to check if any later valid group sealed this LSN */
      bool sealed_by_later = false;
      uint64_t later_sealed_through = 0UL;

      for (size_t check_offset = 0UL; check_offset < config.CapacityBytes; check_offset += WalAlignment) {
        if (check_offset == ring_offset) {
          continue;
        }
        auto check_page = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
        const bool check_ok = SyncDeviceRead(device, HERE, check_page.get(), config.BaseOffset + check_offset, WalAlignment);
        if (!check_ok) {
          continue;
        }
        const TGroupHeader *later_hdr = reinterpret_cast<const TGroupHeader *>(check_page.get());
        if (memcmp(later_hdr->Magic, "ORLYWAL1", 8) == 0 &&
            later_hdr->FormatVersion == WalFormatVersion &&
            later_hdr->StoreId == config.StoreId &&
            later_hdr->HeaderChecksum == ComputeChecksum64(later_hdr, GroupHeaderChecksumCoveredBytes)) {
          if (later_hdr->GroupNum > expected_group &&
              later_hdr->Lap >= expected_lap &&
              later_hdr->Lap <= expected_lap + 1 &&
              later_hdr->SealedThrough >= expected_lsn) {
            sealed_by_later = true;
            later_sealed_through = later_hdr->SealedThrough;
            break;
          }
        }
      }

      if (sealed_by_later) {
        result.Status = TScanStatus::DamagedAcknowledged;
        result.DamagedLsnStart = expected_lsn;
        result.DamagedLsnEnd = later_sealed_through;
        result.ProblemDescription = "Group " + std::to_string(expected_group) + " was acknowledged and sealed through LSN " +
                                    std::to_string(later_sealed_through) + " but is damaged on disk";
      } else {
        result.Status = TScanStatus::TornTail;
        result.ProblemDescription = "Unsealed tail at group " + std::to_string(expected_group);
      }
      break;
    }

    /* Valid header! Now read entire group if larger than 4 KiB */
    const size_t group_size = AlignToWal(sizeof(TGroupHeader) + hdr->PayloadLen);
    auto full_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), group_size);
    const bool full_read_ok = SyncDeviceRead(device, HERE, full_buf.get(), config.BaseOffset + ring_offset, group_size);

    if (!full_read_ok) {
      result.Status = TScanStatus::TornTail;
      result.StoppedAtGroupNum = expected_group;
      break;
    }

    /* Validate payload checksum */
    if (hdr->PayloadLen > 0) {
      const uint64_t payload_chk = ComputeChecksum64(full_buf.get() + sizeof(TGroupHeader), hdr->PayloadLen);
      if (payload_chk != hdr->PayloadChecksum) {
        /* Corrupt payload in a group whose header is valid! Check if sealed */
        const uint64_t expected_lsn = (result.LastValidLsn > 0) ? (result.LastValidLsn + 1) : start_lsn;
        bool sealed_by_later = false;
        uint64_t later_sealed_through = 0UL;

        for (size_t check_offset = 0UL; check_offset < config.CapacityBytes; check_offset += WalAlignment) {
          if (check_offset == ring_offset) {
            continue;
          }
          auto check_page = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
          const bool check_ok = SyncDeviceRead(device, HERE, check_page.get(), config.BaseOffset + check_offset, WalAlignment);
          if (check_ok) {
            const TGroupHeader *later_hdr = reinterpret_cast<const TGroupHeader *>(check_page.get());
            if (memcmp(later_hdr->Magic, "ORLYWAL1", 8) == 0 &&
                later_hdr->FormatVersion == WalFormatVersion &&
                later_hdr->StoreId == config.StoreId &&
                later_hdr->HeaderChecksum == ComputeChecksum64(later_hdr, GroupHeaderChecksumCoveredBytes)) {
              if (later_hdr->GroupNum > expected_group &&
                  later_hdr->Lap >= expected_lap &&
                  later_hdr->Lap <= expected_lap + 1 &&
                  later_hdr->SealedThrough >= expected_lsn) {
                sealed_by_later = true;
                later_sealed_through = later_hdr->SealedThrough;
                break;
              }
            }
          }
        }

        if (sealed_by_later) {
          result.Status = TScanStatus::DamagedAcknowledged;
          result.DamagedLsnStart = expected_lsn;
          result.DamagedLsnEnd = later_sealed_through;
          result.ProblemDescription = "Group " + std::to_string(expected_group) + " had valid header but damaged payload; sealed through " +
                                      std::to_string(later_sealed_through);
        } else {
          result.Status = TScanStatus::TornTail;
          result.ProblemDescription = "Unsealed torn payload at group " + std::to_string(expected_group);
        }
        break;
      }
    }

    /* Iterate through records */
    bool records_valid = true;
    size_t payload_offset = 0UL;
    const char *payload_data = full_buf.get() + sizeof(TGroupHeader);

    for (uint32_t r = 0; r < hdr->RecordCount; ++r) {
      if (payload_offset + sizeof(TRecordHeader) > hdr->PayloadLen) {
        records_valid = false;
        break;
      }
      const TRecordHeader *rec_hdr = reinterpret_cast<const TRecordHeader *>(payload_data + payload_offset);
      if (rec_hdr->Length < sizeof(TRecordHeader) || payload_offset + rec_hdr->Length > hdr->PayloadLen) {
        records_valid = false;
        break;
      }
      const size_t body_len = rec_hdr->Length - sizeof(TRecordHeader);
      const char *body_data = payload_data + payload_offset + sizeof(TRecordHeader);
      if (ComputeCrc32c(body_data, body_len) != rec_hdr->BodyCrc32c) {
        records_valid = false;
        break;
      }

      TScannedRecord scanned;
      scanned.Lsn = rec_hdr->Lsn;
      scanned.Type = static_cast<TWalRecordType>(rec_hdr->Type);
      scanned.Flags = rec_hdr->Flags;
      scanned.Body.assign(body_data, body_data + body_len);
      result.Records.push_back(std::move(scanned));

      payload_offset += rec_hdr->Length;
    }

    if (!records_valid) {
      result.Status = TScanStatus::TornTail;
      result.StoppedAtGroupNum = expected_group;
      break;
    }

    /* Group successfully verified! */
    result.LastValidGroupNum = hdr->GroupNum;
    result.LastValidLap = hdr->Lap;
    const uint64_t last_lsn = (hdr->RecordCount > 0) ? (hdr->FirstLsn + hdr->RecordCount - 1UL) : hdr->FirstLsn;
    if (hdr->RecordCount > 0) {
      result.LastValidLsn = last_lsn;
    }

    result.GroupLocations.push_back({hdr->GroupNum, hdr->Lap, ring_offset, hdr->Chain, hdr->FirstLsn, last_lsn});

    expected_chain = hdr->HeaderChecksum;
    expected_group = hdr->GroupNum + 1UL;
    ring_offset += group_size;
  }
  if (result.LastValidGroupNum > 0) {
    if (ring_offset >= config.CapacityBytes) {
      result.NextRingOffset = 0UL;
      result.LastValidLap = expected_lap + 1;
    } else {
      result.NextRingOffset = ring_offset;
      result.LastValidLap = expected_lap;
    }
    result.LastGroupChecksum = expected_chain;
  }

  return result;
}

    }  // Disk

  }  // Indy

}  // Orly

