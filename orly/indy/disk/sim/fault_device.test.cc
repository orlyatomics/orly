/* <orly/indy/disk/sim/fault_device.test.cc>

   Unit tests for Sim::TFaultDevice and Sim::TFaultPlan storage fault modes (#755 Stage 0).

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

#include <orly/indy/disk/sim/fault_device.h>

#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include <base/test/kit.h>

namespace DiskUtil = Orly::Indy::Disk::Util;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Sim;

static constexpr size_t TestBlocks = 256UL; /* 256 * 512 = 128 KiB capacity, 192 KiB total */

FIXTURE(BasicWriteReadSync) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'X');
  std::vector<char> read_buf(DiskUtil::PhysicalSectorSize, 0);

  TDiskResult res = Error;
  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Success);

  /* Before sync: Live has data, Durable does not */
  auto live = device.GetLiveImage();
  auto durable = device.GetDurableImage();
  EXPECT_EQ(live[DiskUtil::PhysicalBlockSize], 'X');
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 0);

  /* After sync: Durable has data */
  device.Sync();
  durable = device.GetDurableImage();
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 'X');

  /* Read back */
  res = Error;
  device.Read(HERE, DiskUtil::FullSector, 0, read_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
              Low, false,
              [&res](TDiskResult result, const char */*err_str*/) {
                res = result;
              });
  EXPECT_EQ(res, Success);
  EXPECT_EQ(read_buf[0], 'X');
}

FIXTURE(FailWriteReadSync) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> buf(DiskUtil::PhysicalSectorSize, 'A');
  TDiskResult res = Success;

  /* Fail write */
  plan.FailNth(1, TFaultPlan::Write, TOnAbortOnError::Report);
  plan.Arm();
  device.Write(HERE, DiskUtil::FullSector, 0, buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Error);
  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Kind, TFaultPlan::Write);
  plan.Disarm();

  /* Fail read */
  TFaultPlan read_plan;
  TFaultDevice read_device(&read_plan, TestBlocks);
  read_plan.FailNth(1, TFaultPlan::Read, TOnAbortOnError::Report);
  read_plan.Arm();
  const char *err = "";
  read_device.Read(HERE, DiskUtil::FullSector, 0, buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
                   Low, false,
                   [&res, &err](TDiskResult result, const char *err_str) {
                     res = result;
                     err = err_str;
                   });
  EXPECT_EQ(res, Error);
  EXPECT_EQ(std::string(err), "Disk Error");
  read_plan.Disarm();

  /* Fail sync */
  TFaultPlan sync_plan;
  TFaultDevice sync_device(&sync_plan, TestBlocks);
  sync_plan.FailNth(1, TFaultPlan::Sync, TOnAbortOnError::Report);
  sync_plan.Arm();
  bool threw = false;
  try {
    sync_device.Sync();
  } catch (const std::system_error &ex) {
    threw = true;
    EXPECT_EQ(ex.code().value(), EIO);
  }
  EXPECT_TRUE(threw);
}

FIXTURE(LostWrite) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'L');
  TDiskResult res = Error;

  plan.LostWriteNth(1);
  plan.Arm();

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  /* LostWrite reports success to caller, but data is dropped */
  EXPECT_EQ(res, Success);
  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Action, "LostWrite");

  auto live = device.GetLiveImage();
  EXPECT_EQ(live[DiskUtil::PhysicalBlockSize], 0);

  device.Sync();
  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 0);
}

FIXTURE(WriteErrAfter) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'W');
  TDiskResult res = Success;

  plan.WriteErrAfterNth(1, TOnAbortOnError::Report);
  plan.Arm();

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  /* WriteErrAfter reports error to caller, but data reaches media */
  EXPECT_EQ(res, Error);
  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Action, "WriteErrAfter");

  auto live = device.GetLiveImage();
  EXPECT_EQ(live[DiskUtil::PhysicalBlockSize], 'W');

  device.Sync();
  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 'W');
}

FIXTURE(SyncErrAfter) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'S');
  TDiskResult res = Error;

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Success);

  plan.SyncErrAfterNth(1);
  plan.Arm();

  bool threw = false;
  try {
    device.Sync();
  } catch (const std::system_error &ex) {
    threw = true;
    EXPECT_EQ(ex.code().value(), EIO);
  }
  EXPECT_TRUE(threw);
  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Action, "SyncErrAfter");

  /* Dirty sectors were flushed before error was thrown */
  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 'S');
}

FIXTURE(MisdirectedWriteOffset) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  const DiskUtil::TOffset requested_offset = DiskUtil::PhysicalBlockSize;
  const DiskUtil::TOffset target_offset = DiskUtil::PhysicalBlockSize * 2UL;

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'M');
  TDiskResult res = Error;

  plan.MisdirectNth(1, target_offset);
  plan.Arm();

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), requested_offset, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Success);
  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Action, "Misdirect");

  auto live = device.GetLiveImage();
  EXPECT_EQ(live[requested_offset], 0);
  EXPECT_EQ(live[target_offset], 'M');

  device.Sync();
  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[requested_offset], 0);
  EXPECT_EQ(durable[target_offset], 'M');
}

FIXTURE(MisdirectedWriteDelta) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  const DiskUtil::TOffset requested_offset = DiskUtil::PhysicalBlockSize;
  const int64_t delta = static_cast<int64_t>(DiskUtil::PhysicalSectorSize);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'D');
  TDiskResult res = Error;

  plan.MisdirectNthDelta(1, delta);
  plan.Arm();

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), requested_offset, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Success);
  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Action, "Misdirect");

  auto live = device.GetLiveImage();
  EXPECT_EQ(live[requested_offset], 0);
  EXPECT_EQ(live[requested_offset + delta], 'D');

  device.Sync();
  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[requested_offset + delta], 'D');
}

FIXTURE(BitFlipDirect) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'B');
  TDiskResult res = Error;

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Success);
  device.Sync();

  /* Flip bit 0 of first byte in Durable image */
  device.FlipDurableBit(DiskUtil::PhysicalBlockSize, 0);
  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], static_cast<char>('B' ^ 1));

  /* Flip bit 1 of first byte in Live image */
  device.FlipLiveBit(DiskUtil::PhysicalBlockSize, 1);
  auto live = device.GetLiveImage();
  EXPECT_EQ(live[DiskUtil::PhysicalBlockSize], static_cast<char>('B' ^ 2));
}

FIXTURE(BitFlipAtSync) {
  TFaultPlan plan;
  TFaultDevice device(&plan, TestBlocks);

  std::vector<char> write_buf(DiskUtil::PhysicalSectorSize, 'F');
  TDiskResult res = Error;

  device.Write(HERE, DiskUtil::FullSector, 0, write_buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
               Low, false, 0,
               [&res](TDiskResult result, const char */*err_str*/) {
                 res = result;
               });
  EXPECT_EQ(res, Success);

  plan.BitFlipAtSync(1, DiskUtil::PhysicalBlockSize, 3);
  plan.Arm();
  device.Sync();

  EXPECT_TRUE(plan.GetInjected().has_value());
  EXPECT_EQ(plan.GetInjected()->Action, "BitFlip");

  auto durable = device.GetDurableImage();
  EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], static_cast<char>('F' ^ (1 << 3)));
}

FIXTURE(PowerLossCleanAndTorn) {
  /* Clean power loss (tear = 0.0): dirty sectors not copied */
  {
    TFaultPlan plan;
    TFaultDevice device(&plan, TestBlocks);

    std::vector<char> buf(DiskUtil::PhysicalSectorSize, 'P');
    device.Write(HERE, DiskUtil::FullSector, 0, buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
                 Low, false, 0,
                 [](TDiskResult, const char *) {});

    bool power_lost = false;
    plan.PowerLossAtSync(1, 0.0, 42UL, [&] { power_lost = true; });
    plan.Arm();
    device.Sync();

    EXPECT_TRUE(power_lost);
    EXPECT_TRUE(plan.IsPoweredOff());
    auto durable = device.GetDurableImage();
    EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 0);
  }

  /* Full persist at power loss (tear = 1.0): all dirty sectors reach media */
  {
    TFaultPlan plan;
    TFaultDevice device(&plan, TestBlocks);

    std::vector<char> buf(DiskUtil::PhysicalSectorSize, 'T');
    device.Write(HERE, DiskUtil::FullSector, 0, buf.data(), DiskUtil::PhysicalBlockSize, DiskUtil::PhysicalSectorSize,
                 Low, false, 0,
                 [](TDiskResult, const char *) {});

    bool power_lost = false;
    plan.PowerLossAtSync(1, 1.0, 42UL, [&] { power_lost = true; });
    plan.Arm();
    device.Sync();

    EXPECT_TRUE(power_lost);
    EXPECT_TRUE(plan.IsPoweredOff());
    auto durable = device.GetDurableImage();
    EXPECT_EQ(durable[DiskUtil::PhysicalBlockSize], 'T');
  }
}
