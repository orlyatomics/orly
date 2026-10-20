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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <base/io/binary_input_only_stream.h>
#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
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
      const uint64_t lsn = wal.AppendAndWait(TWalRecordType::Txn, payload.data(), payload.size());
      /* Nothing needs the record once it's durable here, so retire it: the ring can't lap a head. */
      wal.Checkpoint(lsn + 1UL, 0UL);
    }

    wal.Flush();
    EXPECT_GT(wal.GetLap(), 1U); // Must have wrapped at least once
  }
}

/* The leader never overwrites a group recovery still needs (#755), and the WAL takes no record it
   can't promise room before the checkpointed head (#891): with nothing retired, appends are refused
   once the ring is spoken for, every record taken still becomes durable, and a checkpoint that
   moves the head on lets appends in again. */
FIXTURE(RingFullRefusesUntilCheckpoint) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 512UL);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 32UL * 1024UL; // 8 groups of 4 KiB
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  const std::string payload(2000, 'X');
  TWal wal(&device, config);
  uint64_t last = 0UL;
  bool refused = false;
  for (size_t i = 0; i < 64 && !refused; ++i) {
    try {
      last = wal.Append(TWalRecordType::Txn, payload.data(), payload.size());
    } catch (const TWalRingFull &) {
      refused = true;
    }
  }
  EXPECT_TRUE(refused);
  EXPECT_GT(last, 1UL);
  EXPECT_TRUE(wal.WaitForDurable(last, std::chrono::milliseconds(5000)));
  EXPECT_EQ(wal.GetLap(), 1U);
  EXPECT_EQ(wal.GetOwedBytes(), 0UL);

  /* Nothing recovery needs was overwritten: every record is still there from LSN 1. */
  const auto scan = TWal::Scan(&device, config);
  EXPECT_EQ(scan.Status, TScanStatus::Clean);
  EXPECT_EQ(scan.Records.size(), last);
  for (size_t i = 0; i < scan.Records.size(); ++i) {
    EXPECT_EQ(scan.Records[i].Lsn, i + 1UL);
  }

  /* Still nothing retired: a record the size of the free ring is refused, and no reservation either. */
  const std::string big(config.CapacityBytes - 2UL * WalAlignment, 'Y');
  const auto append_big = [&wal, &big] { wal.Append(TWalRecordType::Txn, big.data(), big.size()); };
  const auto reserve_big = [&wal, &big] { wal.Reserve(big.size(), TWal::TRoom::All); };
  EXPECT_THROW_FUNC(TWalRingFull, append_big);
  EXPECT_THROW_FUNC(TWalRingFull, reserve_big);
  EXPECT_FALSE(wal.IsFailed());

  /* Retiring everything lets writes in again. */
  wal.Checkpoint(last + 1UL, 0UL);
  const uint64_t next = wal.AppendAndWait(TWalRecordType::Txn, payload.data(), payload.size());
  EXPECT_EQ(next, last + 1UL);
  const auto cp = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId);
  EXPECT_TRUE(cp.has_value());
  EXPECT_EQ(cp->HeadLsn, last + 1UL);
  const auto after = TWal::Scan(&device, config, cp->HeadGroupNum, cp->HeadLsn, cp->HeadLap, cp->HeadRingOffset, cp->HeadChain);
  EXPECT_EQ(after.Status, TScanStatus::Clean);
  EXPECT_EQ(after.Records.size(), 1UL);
  if (!after.Records.empty()) {
    EXPECT_EQ(after.Records.front().Lsn, next);
  }
}

/* Room reserved ahead of an append (#891): a reservation counts against the ring until it is used
   or released, a write that keeps headroom stops short of the last eighth of the ring, and a record
   appended into its reservation is never refused. */
FIXTURE(ReservationsHoldRingRoom) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 512UL);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 64UL * 1024UL; // 16 pages; 2 of them headroom
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  TWal wal(&device, config);
  const size_t page_body = WalAlignment - sizeof(TGroupHeader) - sizeof(TRecordHeader);
  EXPECT_EQ(TWal::GetRecordGroupBytes(page_body), WalAlignment);
  EXPECT_EQ(TWal::GetRecordGroupBytes(page_body + 1UL), 2UL * WalAlignment);

  /* A record keeping headroom may be at most half the headroom (here 8 KiB, so one page): its
     promotion has only the headroom to go into once client writes have filled the rest.  Refused
     as too large, not as ring full, and nothing is charged; the server's own writes aren't capped. */
  EXPECT_EQ(wal.GetHeadroomBytes(), 2UL * WalAlignment);
  const auto reserve_too_large = [&wal, page_body] { wal.Reserve(page_body + 1UL, TWal::TRoom::KeepHeadroom); };
  EXPECT_THROW_FUNC(TWalRecordTooLarge, reserve_too_large);
  EXPECT_EQ(wal.GetOwedBytes(), 0UL);
  /* extra */ {
    TWalReservation big = wal.Reserve(page_body + 1UL, TWal::TRoom::All);
    EXPECT_TRUE(static_cast<bool>(big));
  }
  EXPECT_EQ(wal.GetOwedBytes(), 0UL);

  /* Each one-page record is charged two pages (its group and a seal), so with 14 pages outside the
     headroom, 7 reservations keeping headroom fit and an 8th doesn't; with the headroom, one more. */
  std::vector<TWalReservation> held;
  for (size_t i = 0; i < 7; ++i) {
    held.push_back(wal.Reserve(page_body, TWal::TRoom::KeepHeadroom));
  }
  EXPECT_EQ(wal.GetOwedBytes(), 14UL * WalAlignment);
  const auto reserve_keeping_headroom = [&wal, page_body] { wal.Reserve(page_body, TWal::TRoom::KeepHeadroom); };
  EXPECT_THROW_FUNC(TWalRingFull, reserve_keeping_headroom);
  held.push_back(wal.Reserve(page_body, TWal::TRoom::All));
  const auto append_one = [&wal] { wal.Append(TWalRecordType::Txn, "x", 1UL); };
  EXPECT_THROW_FUNC(TWalRingFull, append_one);

  /* Giving one back makes room for one. */
  held.back().Release();
  EXPECT_FALSE(static_cast<bool>(held.back()));
  held.pop_back();
  EXPECT_EQ(wal.GetOwedBytes(), 14UL * WalAlignment);
  held.push_back(wal.Reserve(page_body, TWal::TRoom::All));

  /* Every reserved record goes in, and becomes durable with the head where it was. */
  const std::string body(page_body, 'Z');
  uint64_t last = 0UL;
  for (auto &reservation : held) {
    last = wal.Append(reservation, TWalRecordType::Txn, body.data(), body.size());
    EXPECT_FALSE(static_cast<bool>(reservation));
  }
  EXPECT_TRUE(wal.WaitForDurable(last, std::chrono::milliseconds(5000)));
  EXPECT_EQ(wal.GetOwedBytes(), 0UL);
  EXPECT_FALSE(wal.IsFailed());

  /* A reservation dropped unused gives its room back. */
  wal.Checkpoint(last + 1UL, 0UL);
  /* reservation */ {
    TWalReservation dropped = wal.Reserve(page_body, TWal::TRoom::All);
    EXPECT_EQ(wal.GetOwedBytes(), 2UL * WalAlignment);
  }
  EXPECT_EQ(wal.GetOwedBytes(), 0UL);
}

/* The wedge in #959.  An update in a safe child holds the head until a promotion carries it up, and
   the promotion needs room of its own.  So the room is kept for it from the update's own record on:
   other records are refused before they take it, the promotion may spend it, and once it has, or
   the update's record never comes, the room is free again. */
FIXTURE(PromotionRoomKeptForUnpromotedUpdates) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 512UL);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 64UL * 1024UL; // 16 pages; 2 of them headroom
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  TWal wal(&device, config);
  const std::string body(100, 'U');
  /* Its page, and a page more: what a promotion adds to the record it promotes. */
  const size_t group = TWal::GetPromotionGroupBytes(body.size());
  EXPECT_EQ(group, 2UL * WalAlignment);
  const TPromotionDebt one_level{group, 1UL};

  /* A reservation holds its pushes' debt, and gives it back if it is dropped unused. */
  /* reservation */ {
    TWalReservation dropped = wal.Reserve(body.size(), TWal::TRoom::KeepHeadroom, std::chrono::milliseconds(0), 0UL, {one_level});
    EXPECT_EQ(wal.GetPromotionDebtBytes(), group);
  }
  EXPECT_EQ(wal.GetPromotionDebtBytes(), 0UL);

  /* A client's write into a pov: its record leaves the update owed one promotion. */
  TWalReservation write = wal.Reserve(body.size(), TWal::TRoom::KeepHeadroom, std::chrono::milliseconds(0), 0UL, {one_level});
  TWal::TPromotionDebtChange owed;
  owed.Owed.push_back(one_level);
  uint64_t last = wal.Append(write, TWalRecordType::Txn, body.data(), body.size(), 0U, owed);
  EXPECT_EQ(wal.GetPromotionDebtBytes(), group);
  EXPECT_TRUE(wal.WaitForDurable(last, std::chrono::milliseconds(5000)));

  /* The server's own records then fill the ring, the head standing still, until one is refused. */
  size_t appended = 0UL;
  for (;;) {
    try {
      last = wal.Append(TWalRecordType::Txn, "x", 1UL);
    } catch (const TWalRingFull &) {
      break;
    }
    ++appended;
    EXPECT_TRUE(wal.WaitForDurable(last, std::chrono::milliseconds(5000)));
    EXPECT_LT(appended, 16UL);
  }
  EXPECT_GT(appended, 0UL);
  EXPECT_FALSE(wal.IsFailed());

  /* Any other record is refused, but the promotion, spending what was kept for it, goes in. */
  const auto reserve_without_credit = [&wal, &body] { wal.Reserve(body.size(), TWal::TRoom::All); };
  EXPECT_THROW_FUNC(TWalRingFull, reserve_without_credit);
  TWalReservation promotion = wal.Reserve(body.size(), TWal::TRoom::All, std::chrono::milliseconds(0), group);
  TWal::TPromotionDebtChange repaid;
  repaid.Repaid.push_back(one_level);
  last = wal.Append(promotion, TWalRecordType::Txn, body.data(), body.size(), 0U, repaid);
  EXPECT_EQ(wal.GetPromotionDebtBytes(), 0UL);
  EXPECT_TRUE(wal.WaitForDurable(last, std::chrono::milliseconds(5000)));
  EXPECT_FALSE(wal.IsFailed());

  /* Once the root's layer reaches a data file the head moves, and there is room again. */
  wal.Checkpoint(last + 1UL, 0UL);
  wal.Append(TWalRecordType::Txn, "x", 1UL);

  /* A debt the retention drops (its repo discarded) frees its room too. */
  wal.ChangePromotionDebt(owed);
  EXPECT_EQ(wal.GetPromotionDebtBytes(), group);
  wal.ChangePromotionDebt(repaid);
  EXPECT_EQ(wal.GetPromotionDebtBytes(), 0UL);
}

/* The cycle in #891.  A root's unflushed memory layer pins every record that pushed into it, so the
   head can't move until the layer reaches a data file, and the flush that writes it first waits for
   those records to be durable (write-ahead, #755).  So every record the WAL takes must be able to
   become durable without the head moving: one that can't strands the flush, and the flush strands
   the checkpoint that would have made room for it. */
FIXTURE(RingFullNeverStrandsAnAcceptedRecord) {
  TFaultPlan plan;
  TFaultDevice device(&plan, 512UL);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 32UL * 1024UL; // 8 groups of 4 KiB
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  const std::string payload(2000, 'X');
  TWal wal(&device, config);
  for (int round = 0; round < 8; ++round) {
    /* Writes go into the layer, and nothing is retired, until the ring refuses one. */
    uint64_t last = 0UL;
    size_t refused = 0UL;
    for (size_t i = 0; i < 64 && !refused; ++i) {
      try {
        last = wal.Append(TWalRecordType::Txn, payload.data(), payload.size());
      } catch (const TWalRingFull &) {
        /* Only a ring-full refusal: a TWalIoError (failed or stopping) must fail the test. */
        ++refused;
      }
    }
    EXPECT_EQ(refused, 1UL);
    EXPECT_FALSE(wal.IsFailed());
    /* The flush's write-ahead wait: every record the layer holds becomes durable with the head where
       it is.  Before #891 this never returned; bounded so a regression fails rather than hangs. */
    EXPECT_TRUE(wal.WaitForDurable(last, std::chrono::milliseconds(10000)));
    /* The layer is in a data file now, so nothing before the next record is needed. */
    wal.Checkpoint(last + 1UL, 0UL);
  }
  EXPECT_GT(wal.GetLap(), 1U);
}

/* After a restart the groups recovery scanned are still needed: a checkpoint must keep naming the
   oldest of them as the head, not the write position, or the next recovery skips their records (#755). */
FIXTURE(ReopenKeepsRetainedGroupsAtHead) {
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
    for (size_t i = 1; i <= 5; ++i) {
      const std::string s = "r" + std::to_string(i);
      wal.AppendAndWait(TWalRecordType::Txn, s.data(), s.size());
    }
    wal.Checkpoint(3UL, 0UL);
  }

  for (int restart = 0; restart < 2; ++restart) {
    const auto cp = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId);
    EXPECT_TRUE(cp.has_value());
    EXPECT_EQ(cp->HeadLsn, 3UL);
    const auto scan = TWal::Scan(&device, config, cp->HeadGroupNum, cp->HeadLsn, cp->HeadLap, cp->HeadRingOffset, cp->HeadChain);
    EXPECT_EQ(scan.Status, TScanStatus::Clean);
    EXPECT_FALSE(scan.Records.empty());
    if (!scan.Records.empty()) {
      EXPECT_EQ(scan.Records.front().Lsn, 3UL);
    }
    const auto head = scan.FindGroupForLsn(cp->HeadLsn);
    TWal wal(&device, config, scan.LastValidLsn + 1UL, scan.LastValidGroupNum + 1UL, scan.LastValidLap, scan.NextRingOffset, scan.LastGroupChecksum,
             cp->HeadLsn, head.GroupNum, head.Lap, head.Offset, head.Chain, scan.GroupLocations);
    const std::string s = "after" + std::to_string(restart);
    wal.AppendAndWait(TWalRecordType::Txn, s.data(), s.size());
    /* The head hasn't moved: the checkpoint must still name the group holding LSN 3. */
    wal.Checkpoint(3UL, 0UL);
  }

  const auto cp = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId);
  EXPECT_TRUE(cp.has_value());
  const auto scan = TWal::Scan(&device, config, cp->HeadGroupNum, cp->HeadLsn, cp->HeadLap, cp->HeadRingOffset, cp->HeadChain);
  EXPECT_EQ(scan.Status, TScanStatus::Clean);
  EXPECT_EQ(scan.Records.size(), 5UL); // 3, 4, 5 and one from each restart
  if (!scan.Records.empty()) {
    EXPECT_EQ(scan.Records.front().Lsn, 3UL);
    EXPECT_EQ(scan.Records.back().Lsn, 7UL);
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

/* A checkpoint slot longer than one page carries a payload that long, and a payload that doesn't fit
   the slot is refused rather than written over the next one. */
FIXTURE(CheckpointSlotLength) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlotBytes = 6UL * WalAlignment;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + config.CheckpointSlotBytes;
  config.StoreId = TestStoreId;

  const size_t max_payload = TWal::MaxCheckpointPayload(config.CheckpointSlotBytes);
  EXPECT_EQ(max_payload, config.CheckpointSlotBytes - 116UL);
  std::string big(20000, 'x');
  big[0] = 'a';
  big[19999] = 'z';
  {
    TWal wal(&device, config);
    wal.AppendAndWait(TWalRecordType::Txn, "item", 4UL);
    wal.Checkpoint(1UL, 100UL, big.data(), big.size());
    std::string too_big(max_payload + 1UL, 'y');
    auto write = [&] { wal.Checkpoint(1UL, 100UL, too_big.data(), too_big.size()); };
    EXPECT_THROW_FUNC(std::length_error, write);
  }
  auto cp = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId, config.CheckpointSlotBytes);
  EXPECT_TRUE(cp.has_value());
  EXPECT_EQ(cp->CheckpointNum, 1UL);
  EXPECT_EQ(std::string(cp->Payload.begin(), cp->Payload.end()), big);
  /* A reader that knows a shorter slot treats the longer checkpoint as unreadable. */
  auto shorter = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId, 2UL * WalAlignment);
  EXPECT_FALSE(shorter.has_value());
}

FIXTURE(ReopenAndAppendAcrossSessions) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  /* Session 1: write 5 records and checkpoint */
  {
    TWal wal(&device, config);
    for (size_t i = 1; i <= 5; ++i) {
      std::string s = "s1_" + std::to_string(i);
      wal.AppendAndWait(TWalRecordType::Txn, s.data(), s.size());
    }
    wal.Flush();
    wal.Checkpoint(1UL, 50UL);
  }

  /* Scan session 1 */
  auto scan1 = TWal::Scan(&device, config);
  EXPECT_EQ(scan1.Status, TScanStatus::Clean);
  EXPECT_EQ(scan1.Records.size(), 5UL);
  EXPECT_EQ(scan1.LastValidLsn, 5UL);
  EXPECT_GT(scan1.NextRingOffset, 0UL);
  EXPECT_NE(scan1.LastGroupChecksum, 0UL);

  /* Session 2: reopen WAL at scan1.NextRingOffset and append 5 more records */
  {
    const uint64_t next_lsn = scan1.LastValidLsn + 1UL;
    const uint64_t next_group = scan1.LastValidGroupNum + 1UL;
    TWal wal(&device, config, next_lsn, next_group, scan1.LastValidLap, scan1.NextRingOffset, scan1.LastGroupChecksum);
    for (size_t i = 6; i <= 10; ++i) {
      std::string s = "s2_" + std::to_string(i);
      wal.AppendAndWait(TWalRecordType::Txn, s.data(), s.size());
    }
    wal.Flush();
  }

  /* Scan session 2 */
  auto scan2 = TWal::Scan(&device, config);
  EXPECT_EQ(scan2.Status, TScanStatus::Clean);
  EXPECT_EQ(scan2.Records.size(), 10UL);
  EXPECT_EQ(scan2.LastValidLsn, 10UL);
  EXPECT_GT(scan2.LastValidGroupNum, scan1.LastValidGroupNum);
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(scan2.Records[i].Lsn, i + 1);
  }
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

FIXTURE(PovRecordRoundTrip) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  const Base::TUuid expected_pov_id("11111111-2222-3333-4444-555555555555");
  const Base::TUuid expected_session_id("66666666-7777-8888-9999-000000000000");
  const bool expected_has_parent = true;
  const Base::TUuid expected_parent_id("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
  const char expected_aud = 'u';
  const char expected_pol = 's';
  const int64_t expected_ttl_sec = 3600;
  const std::vector<Base::TUuid> expected_shared = {
    Base::TUuid("12121212-3434-5656-7878-909090909090"),
    Base::TUuid("abababab-cdcd-efef-abab-cdcdcdefefef")
  };

  {
    TWal wal(&device, config);

    auto recorder = std::make_shared<Io::TRecorder>();
    {
      Io::TBinaryOutputOnlyStream strm(recorder);
      strm << expected_pov_id << expected_session_id << expected_has_parent << expected_parent_id
           << expected_aud << expected_pol << expected_ttl_sec << expected_shared.size();
      for (const auto &id : expected_shared) {
        strm << id;
      }
      strm.Flush();
    }
    std::string wire;
    recorder->CopyOut(wire);

    wal.AppendAndWait(TWalRecordType::Pov, wire.data(), wire.size());
    wal.Flush();
  }

  auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), 1UL);
  EXPECT_EQ(scan_res.Records[0].Type, TWalRecordType::Pov);

  std::string str(scan_res.Records[0].Body.data(), scan_res.Records[0].Body.size());
  auto recorder = std::make_shared<Io::TRecorder>(str);
  auto player = std::make_shared<Io::TPlayer>(recorder);
  Io::TBinaryInputOnlyStream strm(player);

  Base::TUuid actual_pov_id, actual_session_id, actual_parent_id;
  bool actual_has_parent;
  char actual_aud, actual_pol;
  int64_t actual_ttl_sec;
  size_t actual_num_parents;

  strm >> actual_pov_id >> actual_session_id >> actual_has_parent >> actual_parent_id
       >> actual_aud >> actual_pol >> actual_ttl_sec >> actual_num_parents;

  EXPECT_EQ(actual_pov_id, expected_pov_id);
  EXPECT_EQ(actual_session_id, expected_session_id);
  EXPECT_EQ(actual_has_parent, expected_has_parent);
  EXPECT_EQ(actual_parent_id, expected_parent_id);
  EXPECT_EQ(actual_aud, expected_aud);
  EXPECT_EQ(actual_pol, expected_pol);
  EXPECT_EQ(actual_ttl_sec, expected_ttl_sec);
  EXPECT_EQ(actual_num_parents, expected_shared.size());

  for (size_t i = 0; i < actual_num_parents; ++i) {
    Base::TUuid id;
    strm >> id;
    EXPECT_EQ(id, expected_shared[i]);
  }
}

FIXTURE(NegativeControlNoSync) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  config.NoSync = true;

  {
    TWal wal(&device, config);
    for (size_t i = 0; i < 5; ++i) {
      std::string data = "nosync_record_" + std::to_string(i);
      const uint64_t lsn = wal.AppendAndWait(TWalRecordType::Txn, data.data(), data.size());
      EXPECT_EQ(lsn, i + 1);
    }
    EXPECT_GE(wal.GetDurableLsn(), 5UL);
  }
}

FIXTURE(NegativeControlEarlyAck) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  config.EarlyAck = true;

  {
    TWal wal(&device, config);
    for (size_t i = 0; i < 5; ++i) {
      std::string data = "earlyack_record_" + std::to_string(i);
      const uint64_t lsn = wal.AppendAndWait(TWalRecordType::Txn, data.data(), data.size());
      EXPECT_EQ(lsn, i + 1);
    }
    EXPECT_GE(wal.GetDurableLsn(), 5UL);
  }
}

FIXTURE(RingWrapAndCheckpointHeadScan) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);

  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 32UL * 1024UL;  // 8 groups of 4 KiB
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;

  /* Session 1: write 12 records with AppendAndWait.
     With AutoSealOnQuiet, each record + seal is 2 groups = 24 groups.
     Wraps the 8-group ring across 3 laps.  The ring never overwrites a group past the
     checkpointed head, so the head follows each record up to record 9 and then stays there:
     records 10-12 fill the rest of the lap behind it. */
  {
    TWal wal(&device, config);
    for (size_t i = 1; i <= 12; ++i) {
      std::string data = "wrap_rec_" + std::to_string(i);
      wal.AppendAndWait(TWalRecordType::Txn, data.data(), data.size());
      if (i <= 9) {
        wal.Checkpoint(i, i * 10UL);
      }
    }
    wal.Flush();
  }

  /* Read checkpoint from Session 1 */
  auto cp = TWal::ReadNewestCheckpoint(&device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, TestStoreId);
  EXPECT_TRUE(cp.has_value());
  EXPECT_EQ(cp->HeadLsn, 9UL);
  EXPECT_EQ(cp->HeadLap, 3U);
  EXPECT_EQ(cp->HeadGroupNum, 17UL);
  EXPECT_EQ(cp->HeadRingOffset, 0UL);
  EXPECT_NE(cp->HeadChain, 0UL);

  /* Scan starting from checkpoint head location */
  auto scan_res = TWal::Scan(&device, config, cp->HeadGroupNum, cp->HeadLsn, cp->HeadLap, cp->HeadRingOffset, cp->HeadChain);
  EXPECT_EQ(scan_res.Status, TScanStatus::Clean);
  EXPECT_EQ(scan_res.Records.size(), 4UL);  // records 9, 10, 11, 12
  EXPECT_EQ(scan_res.LastValidLsn, 12UL);
  EXPECT_EQ(scan_res.LastValidGroupNum, 24UL);
  EXPECT_EQ(scan_res.LastValidLap, 4U);
  EXPECT_EQ(scan_res.NextRingOffset, 0UL);
}



/* Runs `stop` (a WAL's destruction) and aborts the test if it hasn't returned within a minute: a
   WAL that never stops would otherwise hang the suite rather than fail it. */
static void StopWithin(const std::function<void ()> &stop) {
  std::atomic<bool> stopped(false);
  std::thread watchdog([&stopped] {
    for (size_t i = 0UL; i < 600UL && !stopped.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!stopped.load()) {
      fprintf(stderr, "the WAL did not stop within a minute\n");
      abort();
    }
  });
  stop();
  stopped.store(true);
  watchdog.join();
}

/* A WAL stopped with records still queued writes them all and stops, though every in-flight slot is
   taken when it is told to (#963).  The leader went round again at once, holding the lock the
   syncer needed to free a slot, so it never did.  Stopping as the syncer syncs is a race, so this
   tries it many times, with one slot to make it likely. */
FIXTURE(StopWithRecordsQueuedAndSlotsFull) {
  for (size_t attempt = 0UL; attempt < 50UL; ++attempt) {
    TFaultPlan plan;
    TFaultDevice device(&plan, WalTestBlocks);
    TWal::TConfig config;
    config.BaseOffset = DiskUtil::PhysicalBlockSize;
    config.CapacityBytes = 128UL * 1024UL;
    config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
    config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
    config.StoreId = TestStoreId;
    config.MaxInFlightGroups = 1UL;
    const std::string data(1000UL, 'r');
    auto wal = std::make_unique<TWal>(&device, config);
    /* Twenty, so they fit the ring even one to a group. */
    for (size_t i = 0UL; i < 20UL; ++i) {
      wal->Append(TWalRecordType::Txn, data.data(), data.size());
    }
    StopWithin([&wal] { wal.reset(); });
    const auto scan_res = TWal::Scan(&device, config);
    if (!EXPECT_EQ(scan_res.Records.size(), 20UL)) {
      break;
    }
  }
}

/* The same, made certain: the WAL is told to stop while it syncs the one group its one slot holds,
   with more records queued behind it than one group holds.  The syncer used to leave as soon as that sync emptied the
   slot, since it was stopping and nothing was in flight; the leader then wrote the next group into
   the slot and waited for good for a syncer that had gone. */
FIXTURE(StopWhileTheOnlySlotSyncs) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);
  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  config.MaxInFlightGroups = 1UL;
  std::atomic<bool> syncing(false), stop_called(false), first(true);
  auto wal = std::make_unique<TWal>(&device, config);
  wal->SetBeforeSyncForTest([&] {
    if (!first.exchange(false)) {
      return;
    }
    syncing.store(true);
    while (!stop_called.load()) {
      std::this_thread::yield();
    }
    /* Long enough for Stop() to have set Stopping. */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });
  /* Ten thousand bytes each: the nine queued fill two groups, so the leader must wait for the slot. */
  const std::string data(10000UL, 's');
  wal->Append(TWalRecordType::Txn, data.data(), data.size());
  while (!syncing.load()) {
    std::this_thread::yield();
  }
  /* The slot is taken and its sync held, so these queue. */
  for (size_t i = 1UL; i < 10UL; ++i) {
    wal->Append(TWalRecordType::Txn, data.data(), data.size());
  }
  stop_called.store(true);
  StopWithin([&wal] { wal.reset(); });
  const auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Records.size(), 10UL);
}

/* With auto-seal off (a test's), a WAL stopped with its newest group synced but not sealed seals it
   and stops, rather than going round for good waiting for a seal it would never write. */
FIXTURE(StopUnsealedWithAutoSealOff) {
  TFaultPlan plan;
  TFaultDevice device(&plan, WalTestBlocks);
  TWal::TConfig config;
  config.BaseOffset = DiskUtil::PhysicalBlockSize;
  config.CapacityBytes = 128UL * 1024UL;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
  config.StoreId = TestStoreId;
  auto wal = std::make_unique<TWal>(&device, config);
  wal->SetAutoSealOnQuietForTest(false);
  const std::string data = "unsealed";
  const uint64_t lsn = wal->Append(TWalRecordType::Txn, data.data(), data.size());
  /* Synced, never sealed: not durable. */
  EXPECT_FALSE(wal->WaitForDurable(lsn, std::chrono::milliseconds(200)));
  StopWithin([&wal] { wal.reset(); });
  const auto scan_res = TWal::Scan(&device, config);
  EXPECT_EQ(scan_res.Records.size(), 1UL);
}
