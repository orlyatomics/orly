/* <orly/indy/disk/wal.test.cc>

   Unit tests and fault-injection verification for TWal (#755 Stage 1).

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

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <base/test/kit.h>
#include <orly/indy/disk/sim/fault_device.h>

namespace DiskUtil = Orly::Indy::Disk::Util;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Sim;

static constexpr size_t WalTestBlocks = 512UL; // 512 * 512 = 256 KiB capacity
static const Base::TUuid TestStoreId("12345678-1234-1234-1234-123456789abc");

FIXTURE(Crc32cStandardVector) {
  /* Standard Castagnoli test vector for "123456789" */
  const char *test_str = "123456789";
  const uint32_t crc = ComputeCrc32c(test_str, 9);
  EXPECT_EQ(crc, 0xE3069283U);
}

FIXTURE(BasicAppendWaitAndScan) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize; // 64 KiB
  config.CapacityBytes = 128UL * 1024UL; // 128 KiB
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  config.QuietInterval = std::chrono::microseconds(100);

  {
    TWal wal(&device, config);

    for (size_t i = 0; i < 10; ++i) {
      std::string data = "record_" + std::to_string(i);
      const uint64_t lsn = wal.AppendAndWait(TWalRecordType::Txn, data.data(), data.size());
      EXPECT_EQ(lsn, i + 1);
    }

    wal.Flush();
    EXPECT_GE(wal.GetDurableLsn(), 10UL);
  }

  /* Scan after close */
  const auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), 10UL);
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(scan_res.Records[i].Lsn, i + 1);
    EXPECT_EQ(scan_res.Records[i].Type, TWalRecordType::Txn);
    std::string expected_body = "record_" + std::to_string(i);
    std::string actual_body(scan_res.Records[i].Body.begin(), scan_res.Records[i].Body.end());
    EXPECT_EQ(actual_body, expected_body);
  }
}

FIXTURE(SealOnQuiet) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  config.QuietInterval = std::chrono::microseconds(200);

  {
    TWal wal(&device, config);

    /* Write single record */
    std::string data = "lonely_write";
    const uint64_t lsn = wal.Append(TWalRecordType::Txn, data.data(), data.size());
    EXPECT_EQ(lsn, 1UL);

    /* Must become durable via quiet seal without needing subsequent writes */
    wal.WaitForDurable(1UL);
    EXPECT_GE(wal.GetDurableLsn(), 1UL);
  }

  const auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), 1UL);
}

FIXTURE(MultipleWritersGroupCommit) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 4096UL); // 2 MiB

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 1024UL * 1024UL; // 1 MiB
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  config.MaxInFlightGroups = 2UL;

  const size_t num_threads = 4;
  const size_t records_per_thread = 25;
  const size_t total_records = num_threads * records_per_thread;

  {
    TWal wal(&device, config);

    std::vector<std::thread> writers;
    writers.reserve(num_threads);

    for (size_t t = 0; t < num_threads; ++t) {
      writers.emplace_back([&wal, t, records_per_thread] {
        for (size_t i = 0; i < records_per_thread; ++i) {
          std::string body = "t" + std::to_string(t) + "_r" + std::to_string(i);
          wal.AppendAndWait(TWalRecordType::Txn, body.data(), body.size());
        }
      });
    }

    for (auto &t : writers) {
      t.join();
    }

    wal.Flush();
    EXPECT_EQ(wal.GetDurableLsn(), total_records);
  }

  const auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), total_records);
  EXPECT_EQ(scan_res.LastValidLsn, total_records);
}

FIXTURE(RingWrapAcrossLaps) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 512UL); // 256 KiB

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 32UL * 1024UL; // 32 KiB small ring (8 pages)
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  const size_t num_records = 40; // ~40 * 4KB groups will wrap multiple laps
  {
    TWal wal(&device, config);

    for (size_t i = 0; i < num_records; ++i) {
      std::string payload(2000, 'X'); // Large enough to make 4 KiB group
      wal.AppendAndWait(TWalRecordType::Txn, payload.data(), payload.size());
    }

    wal.Flush();
    EXPECT_GT(wal.GetLap(), 1U); // Must have wrapped at least once
  }
}

FIXTURE(CheckpointAndHeadAdvance) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  {
    TWal wal(&device, config);

    for (size_t i = 0; i < 5; ++i) {
      std::string body = "item" + std::to_string(i);
      wal.AppendAndWait(TWalRecordType::Txn, body.data(), body.size());
    }

    /* Write checkpoint 1 */
    std::string cp_payload1 = "checkpoint_metadata_1";
    wal.Checkpoint(2UL, 100UL, cp_payload1.data(), cp_payload1.size());
    EXPECT_EQ(wal.GetHeadLsn(), 2UL);

    /* Write checkpoint 2 */
    std::string cp_payload2 = "checkpoint_metadata_2";
    wal.Checkpoint(4UL, 200UL, cp_payload2.data(), cp_payload2.size());
    EXPECT_EQ(wal.GetHeadLsn(), 4UL);
  }

  /* Read newest checkpoint */
  auto cp = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId);
  EXPECT_TRUE(cp.has_value());
  EXPECT_EQ(cp->CheckpointNum, 2UL);
  EXPECT_EQ(cp->HeadLsn, 4UL);
  EXPECT_EQ(cp->GlobalFlushedSeq, 200UL);
  std::string payload_str(cp->Payload.begin(), cp->Payload.end());
  EXPECT_EQ(payload_str, "checkpoint_metadata_2");
}

FIXTURE(CopyForward) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  {
    TWal wal(&device, config);

    wal.AppendAndWait(TWalRecordType::Txn, "first", 5);
    wal.AppendAndWait(TWalRecordType::Txn, "second", 6);

    std::vector<std::pair<TWalRecordType, std::vector<char>>> to_copy;
    to_copy.emplace_back(TWalRecordType::Txn, std::vector<char>{'c', 'o', 'p', 'y', '1'});
    to_copy.emplace_back(TWalRecordType::Txn, std::vector<char>{'c', 'o', 'p', 'y', '2'});

    auto new_lsns = wal.CopyForward(to_copy);
    EXPECT_EQ(new_lsns.size(), 2UL);
    EXPECT_EQ(new_lsns[0], 3UL);
    EXPECT_EQ(new_lsns[1], 4UL);
  }

  const auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), 4UL);
  EXPECT_TRUE(scan_res.Records[2].Flags & WalRecordFlags::Copied);
  EXPECT_TRUE(scan_res.Records[3].Flags & WalRecordFlags::Copied);
}

FIXTURE(FaultLostWriteTornTail) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  {
    TWal wal(&device, config);

    /* Write 3 valid records */
    for (size_t i = 0; i < 3; ++i) {
      std::string s = "rec" + std::to_string(i);
      wal.AppendAndWait(TWalRecordType::Txn, s.data(), s.size());
    }
  }

  /* Scan detects clean log with 3 records */
  auto scan1 = TWal::Scan(&device, config);
  EXPECT_EQ(scan1.Status, TScanStatus::Clean);
  EXPECT_EQ(scan1.Records.size(), 3UL);

  /* Simulate a crash with an unsealed, torn tail group: valid magic and group header, but corrupted header checksum */
  const size_t next_group_offset = config.BaseOffset + WalAlignment * scan1.LastValidGroupNum;
  std::vector<char> torn_buf(WalAlignment, 0);
  TGroupHeader *torn_hdr = reinterpret_cast<TGroupHeader *>(torn_buf.data());
  memcpy(torn_hdr->Magic, "ORLYWAL1", 8);
  torn_hdr->FormatVersion = WalFormatVersion;
  torn_hdr->StoreId = TestStoreId;
  torn_hdr->GroupNum = scan1.LastValidGroupNum + 1;
  torn_hdr->Lap = 1;
  torn_hdr->FirstLsn = 4;
  torn_hdr->RecordCount = 1;
  torn_hdr->PayloadLen = 10;
  torn_hdr->HeaderChecksum = 0xdeadbeefULL; // Bad checksum, unsealed torn write!
  device.Write(HERE, DiskUtil::FullPage, 0, torn_buf.data(), next_group_offset, WalAlignment,
               Low, false, next_group_offset, [](TDiskResult, const char *) {});

  auto scan2 = TWal::Scan(&device, config);
  EXPECT_EQ(scan2.Status, TScanStatus::TornTail);
  EXPECT_EQ(scan2.Records.size(), 3UL);
}

FIXTURE(FaultDamagedAcknowledgedSealedGroup) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  {
    TWal wal(&device, config);

    /* Write group 1 */
    wal.AppendAndWait(TWalRecordType::Txn, "group1_data", 11);
    /* Write group 2 which seals group 1 */
    wal.AppendAndWait(TWalRecordType::Txn, "group2_data", 11);
    wal.Flush();
  }

  /* Verify scan clean before corruption */
  auto scan_clean = TWal::Scan(&device, config);
  EXPECT_EQ(scan_clean.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_clean.Records.size(), 2UL);

  /* Corrupt payload of group 1 in Live and Durable images */
  device.FlipLiveBit(config.BaseOffset + sizeof(TGroupHeader) + 2, 0);
  device.FlipDurableBit(config.BaseOffset + sizeof(TGroupHeader) + 2, 0);

  /* Scan must classify this as DamagedAcknowledged corruption */
  auto scan_damaged = TWal::Scan(&device, config);
  EXPECT_EQ(scan_damaged.Status, TScanStatus::DamagedAcknowledged);
  EXPECT_EQ(scan_damaged.DamagedLsnStart, 1UL);
}

FIXTURE(FaultIoErrorFsyncgate) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  TWal wal(&device, config);

  /* Inject sync error after next sync */
  plan.SyncErrAfterNth(1);
  plan.Arm();

  bool threw = false;
  try {
    wal.AppendAndWait(TWalRecordType::Txn, "bad_sync", 8);
  } catch (const TWalIoError &ex) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  EXPECT_TRUE(wal.IsFailed());
}

FIXTURE(StandaloneBenchmarkReplayRate) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 2048UL); // 1 MiB

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 512UL * 1024UL; // 512 KiB
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  const size_t record_count = 200;
  std::string data(128, 'B'); // 128 byte records

  {
    TWal wal(&device, config);
    for (size_t i = 0; i < record_count; ++i) {
      wal.Append(TWalRecordType::Txn, data.data(), data.size());
    }
    wal.Flush();
  }

  const auto t0 = std::chrono::steady_clock::now();
  const auto scan_res = TWal::Scan(&device, config);
  const auto t1 = std::chrono::steady_clock::now();

  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), record_count);

  const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  const double replay_records_sec = (elapsed_us > 0) ? (record_count * 1000000.0 / elapsed_us) : 0.0;
  std::cout << "Replay scan benchmark: " << record_count << " records in "
            << elapsed_us << " us (" << static_cast<size_t>(replay_records_sec) << " rec/s)" << std::endl;
}
