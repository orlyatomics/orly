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
#include <iostream>

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

TWal::TWal(Util::TDevice *device, const TConfig &config, uint64_t start_lsn, uint64_t start_group_num, uint32_t start_lap)
    : Device(device),
      Config(config),
      NextLsn(start_lsn > 0 ? start_lsn - 1 : 0),
      NextGroupNum(start_group_num > 0 ? start_group_num - 1 : 0),
      CurrentLap(start_lap),
      CurrentRingOffset(0UL),
      PreviousGroupChecksum(0UL),
      HighestSyncedLsn(NextLsn),
      HighestSealedLsn(0UL),
      DurableLsn(0UL),
      HeadLsn(start_lsn),
      Stopping(false),
      Failed(false) {
  assert(Device != nullptr);
  assert(Config.CapacityBytes >= WalAlignment);
  assert(Config.CapacityBytes % WalAlignment == 0);
  assert(Config.BaseOffset % WalAlignment == 0);

  LeaderThread = std::thread(&TWal::LeaderMain, this);
  SyncerThread = std::thread(&TWal::SyncerMain, this);
}

TWal::~TWal() {
  Stop();
}

void TWal::Stop() {
  {
    std::lock_guard<std::mutex> lock(Mutex);
    if (Stopping) {
      return;
    }
    Stopping = true;
  }
  LeaderCv.notify_all();
  if (LeaderThread.joinable()) {
    LeaderThread.join();
  }
  SyncerCv.notify_all();
  if (SyncerThread.joinable()) {
    SyncerThread.join();
  }
}

uint64_t TWal::Append(TWalRecordType type, const void *data, size_t size, uint8_t flags) {
  std::unique_lock<std::mutex> lock(Mutex);
  if (Failed) {
    throw TWalIoError("WAL is in failed state");
  }
  if (Stopping) {
    throw TWalIoError("WAL is stopping");
  }

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

void TWal::LeaderMain() {
  std::unique_lock<std::mutex> lock(Mutex);
  while (!Stopping || !PendingRecords.empty() || (HighestSyncedLsn > HighestSealedLsn && InFlightGroups.empty())) {
    while (!Stopping &&
           !(!PendingRecords.empty() && InFlightGroups.size() < Config.MaxInFlightGroups) &&
           !(Config.AutoSealOnQuiet && HighestSyncedLsn > HighestSealedLsn && InFlightGroups.empty())) {
      if (Config.AutoSealOnQuiet && HighestSyncedLsn > HighestSealedLsn) {
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
    const bool need_seal = !have_records && Config.AutoSealOnQuiet && HighestSyncedLsn > HighestSealedLsn && InFlightGroups.size() < Config.MaxInFlightGroups;

    if (!have_records && !need_seal) {
      continue;
    }

    std::vector<TPendingRecord> batch;
    size_t payload_bytes = 0UL;
    if (have_records) {
      const size_t max_group_payload = 64UL * 1024UL; // 64 KiB batch limit
      while (!PendingRecords.empty() && (payload_bytes == 0 || payload_bytes + PendingRecords.front().Encoded.size() <= max_group_payload)) {
        payload_bytes += PendingRecords.front().Encoded.size();
        batch.push_back(std::move(PendingRecords.front()));
        PendingRecords.pop_front();
      }
    }

    const uint64_t group_num = ++NextGroupNum;
    const uint64_t first_lsn = batch.empty() ? 0UL : batch.front().Lsn;
    const uint64_t last_lsn = batch.empty() ? 0UL : batch.back().Lsn;
    const uint64_t sealed_through = HighestSyncedLsn;
    const bool is_seal_only = batch.empty();

    const size_t unpadded_size = sizeof(TGroupHeader) + payload_bytes;
    const size_t group_size = AlignToWal(unpadded_size);

    /* Allocate ring position */
    if (CurrentRingOffset + group_size > Config.CapacityBytes) {
      ++CurrentLap;
      CurrentRingOffset = 0UL;
    }
    const size_t group_offset = CurrentRingOffset;
    CurrentRingOffset += group_size;

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

    /* Write to device while holding no lock or release lock during I/O */
    TInFlightGroup in_flight;
    in_flight.GroupNum = group_num;
    in_flight.FirstLsn = first_lsn;
    in_flight.LastLsn = last_lsn;
    in_flight.SealedThrough = sealed_through;
    in_flight.Lap = CurrentLap;
    in_flight.Offset = group_offset;
    in_flight.Size = group_size;
    in_flight.IsSealOnly = is_seal_only;
    InFlightGroups.push_back(in_flight);

    lock.unlock();

    bool write_ok = false;
    Device->Write(HERE, DiskUtil::FullPage, 0, buf_ptr, Config.BaseOffset + group_offset, group_size,
                  DiskPriority::RealTime, false, Config.BaseOffset + group_offset,
                  [&write_ok](TDiskResult res, const char */*err*/) {
                    write_ok = (res == Success);
                  });

    lock.lock();
    if (!write_ok) {
      Failed = true;
      DurableCv.notify_all();
      SyncerCv.notify_all();
      break;
    }

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
}

void TWal::SyncerMain() {
  std::unique_lock<std::mutex> lock(Mutex);
  while (!Stopping || !InFlightGroups.empty()) {
    while (!Stopping && InFlightGroups.empty() && !Failed) {
      SyncerCv.wait(lock);
    }
    if (Failed) {
      break;
    }
    if (InFlightGroups.empty() && Stopping) {
      break;
    }

    TInFlightGroup group = InFlightGroups.front();

    lock.unlock();

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

    InFlightGroups.pop_front();

    if (!group.IsSealOnly) {
      HighestSyncedLsn = std::max(HighestSyncedLsn, group.LastLsn);
    }
    HighestSealedLsn = std::max(HighestSealedLsn, group.SealedThrough);
    DurableLsn = HighestSealedLsn;

    DurableCv.notify_all();
    LeaderCv.notify_all();
  }
}

void TWal::Checkpoint(uint64_t head_lsn, uint64_t global_flushed_seq, const void *payload, size_t payload_len) {
  std::unique_lock<std::mutex> lock(Mutex);
  if (Failed) {
    throw TWalIoError("WAL is in failed state");
  }

  /* Identify newest valid checkpoint on disk to determine inactive slot */
  lock.unlock();
  auto newest_cp = ReadNewestCheckpoint(Device, Config.CheckpointSlot0Offset, Config.CheckpointSlot1Offset, Config.StoreId);
  lock.lock();

  uint64_t next_cp_num = 1UL;
  Util::TOffset target_slot = Config.CheckpointSlot0Offset;
  if (newest_cp.has_value()) {
    next_cp_num = newest_cp->CheckpointNum + 1UL;
    /* If newest came from slot 0, use slot 1; otherwise use slot 0 */
    target_slot = (newest_cp->CheckpointNum % 2 == 1) ? Config.CheckpointSlot1Offset : Config.CheckpointSlot0Offset;
  }

  const size_t total_bytes = AlignToWal(sizeof(TCheckpointHeader) + payload_len);
  auto buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), total_bytes);

  TCheckpointHeader header;
  memcpy(header.Magic, "ORLYCHK1", 8);
  header.FormatVersion = WalFormatVersion;
  header.Reserved = 0;
  header.StoreId = Config.StoreId;
  header.CheckpointNum = next_cp_num;
  header.HeadLsn = head_lsn;
  header.DurableLsn = DurableLsn;
  header.GlobalFlushedSeq = global_flushed_seq;
  header.PayloadLen = static_cast<uint32_t>(payload_len);
  header.Reserved2 = 0;

  if (payload && payload_len > 0) {
    memcpy(buf.get() + sizeof(TCheckpointHeader), payload, payload_len);
    header.PayloadChecksum = ComputeChecksum64(payload, payload_len);
  } else {
    header.PayloadChecksum = 0UL;
  }
  header.HeaderChecksum = ComputeChecksum64(&header, CheckpointHeaderChecksumCoveredBytes);
  memcpy(buf.get(), &header, sizeof(TCheckpointHeader));

  lock.unlock();

  bool write_ok = false;
  Device->Write(HERE, DiskUtil::FullPage, 0, buf.get(), target_slot, total_bytes,
                DiskPriority::RealTime, false, target_slot,
                [&write_ok](TDiskResult res, const char */*err*/) {
                  write_ok = (res == Success);
                });
  if (!write_ok) {
    lock.lock();
    Failed = true;
    throw TWalIoError("Failed to write checkpoint slot");
  }

  if (!Config.NoSync) {
    Device->Sync();
  }

  lock.lock();
  HeadLsn = head_lsn;
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

std::optional<TCheckpoint> TWal::ReadNewestCheckpoint(Util::TDevice *device, Util::TOffset slot0, Util::TOffset slot1, const Base::TUuid &store_id) {
  auto read_slot = [device, &store_id](Util::TOffset offset) -> std::optional<TCheckpoint> {
    auto buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
    bool read_ok = false;
    device->Read(HERE, DiskUtil::FullPage, 0, buf.get(), offset, WalAlignment,
                 DiskPriority::RealTime, false,
                 [&read_ok](TDiskResult res, const char */*err*/) {
                   read_ok = (res == Success);
                 });
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
      auto full_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), total_size);
      bool full_read_ok = false;
      device->Read(HERE, DiskUtil::FullPage, 0, full_buf.get(), offset, total_size,
                   DiskPriority::RealTime, false,
                   [&full_read_ok](TDiskResult res, const char */*err*/) {
                     full_read_ok = (res == Success);
                   });
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
      cp.Payload.assign(full_buf.get() + sizeof(TCheckpointHeader), full_buf.get() + sizeof(TCheckpointHeader) + hdr->PayloadLen);
      return cp;
    }
    TCheckpoint cp;
    cp.CheckpointNum = hdr->CheckpointNum;
    cp.HeadLsn = hdr->HeadLsn;
    cp.DurableLsn = hdr->DurableLsn;
    cp.GlobalFlushedSeq = hdr->GlobalFlushedSeq;
    return cp;
  };

  auto cp0 = read_slot(slot0);
  auto cp1 = read_slot(slot1);

  if (cp0 && cp1) {
    return (cp0->CheckpointNum >= cp1->CheckpointNum) ? cp0 : cp1;
  }
  return cp0 ? cp0 : cp1;
}

TScanResult TWal::Scan(Util::TDevice *device, const TConfig &config, uint64_t start_group_num, uint64_t start_lsn, uint32_t start_lap, uint64_t start_ring_offset) {
  TScanResult result;
  result.Status = TScanStatus::Clean;
  result.LastValidLap = start_lap;

  uint64_t expected_group = start_group_num;
  uint32_t expected_lap = start_lap;
  uint64_t expected_chain = 0UL;
  size_t ring_offset = start_ring_offset;

  while (true) {
    /* If at end of capacity, wrap */
    if (ring_offset >= config.CapacityBytes) {
      ring_offset = 0UL;
      ++expected_lap;
    }

    auto page_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
    bool read_ok = false;
    device->Read(HERE, DiskUtil::FullPage, 0, page_buf.get(), config.BaseOffset + ring_offset, WalAlignment,
                 DiskPriority::RealTime, false,
                 [&read_ok](TDiskResult res, const char */*err*/) {
                   read_ok = (res == Success);
                 });

    if (!read_ok) {
      result.Status = TScanStatus::TornTail;
      result.StoppedAtGroupNum = expected_group;
      result.ProblemDescription = "Device read error at offset " + std::to_string(ring_offset);
      break;
    }

    const TGroupHeader *hdr = reinterpret_cast<const TGroupHeader *>(page_buf.get());

    /* Check magic */
    if (memcmp(hdr->Magic, "ORLYWAL1", 8) != 0) {
      /* Not a group header. Check if the ring wrapped to offset 0 */
      if (ring_offset > 0) {
        auto wrap_buf = Base::MemAlignedAllocZeroInitialized<char>(getpagesize(), WalAlignment);
        bool wrap_read_ok = false;
        device->Read(HERE, DiskUtil::FullPage, 0, wrap_buf.get(), config.BaseOffset, WalAlignment,
                     DiskPriority::RealTime, false,
                     [&wrap_read_ok](TDiskResult res, const char */*err*/) {
                       wrap_read_ok = (res == Success);
                     });
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
        result.Status = TScanStatus::Empty;
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
        bool check_ok = false;
        device->Read(HERE, DiskUtil::FullPage, 0, check_page.get(), config.BaseOffset + check_offset, WalAlignment,
                     DiskPriority::RealTime, false,
                     [&check_ok](TDiskResult res, const char */*err*/) {
                       check_ok = (res == Success);
                     });
        if (!check_ok) {
          continue;
        }
        const TGroupHeader *later_hdr = reinterpret_cast<const TGroupHeader *>(check_page.get());
        if (memcmp(later_hdr->Magic, "ORLYWAL1", 8) == 0 &&
            later_hdr->FormatVersion == WalFormatVersion &&
            later_hdr->StoreId == config.StoreId &&
            later_hdr->HeaderChecksum == ComputeChecksum64(later_hdr, GroupHeaderChecksumCoveredBytes)) {
          if (later_hdr->GroupNum > expected_group && later_hdr->SealedThrough >= expected_lsn) {
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
    bool full_read_ok = false;
    device->Read(HERE, DiskUtil::FullPage, 0, full_buf.get(), config.BaseOffset + ring_offset, group_size,
                 DiskPriority::RealTime, false,
                 [&full_read_ok](TDiskResult res, const char */*err*/) {
                   full_read_ok = (res == Success);
                 });

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
          bool check_ok = false;
          device->Read(HERE, DiskUtil::FullPage, 0, check_page.get(), config.BaseOffset + check_offset, WalAlignment,
                       DiskPriority::RealTime, false,
                       [&check_ok](TDiskResult res, const char */*err*/) {
                         check_ok = (res == Success);
                       });
          if (check_ok) {
            const TGroupHeader *later_hdr = reinterpret_cast<const TGroupHeader *>(check_page.get());
            if (memcmp(later_hdr->Magic, "ORLYWAL1", 8) == 0 &&
                later_hdr->FormatVersion == WalFormatVersion &&
                later_hdr->StoreId == config.StoreId &&
                later_hdr->HeaderChecksum == ComputeChecksum64(later_hdr, GroupHeaderChecksumCoveredBytes)) {
              if (later_hdr->GroupNum > expected_group && later_hdr->SealedThrough >= expected_lsn) {
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
    if (hdr->RecordCount > 0) {
      result.LastValidLsn = hdr->FirstLsn + hdr->RecordCount - 1UL;
    }

    expected_chain = hdr->HeaderChecksum;
    expected_group = hdr->GroupNum + 1UL;
    ring_offset += group_size;
  }

  return result;
}

    }  // Disk

  }  // Indy

}  // Orly

