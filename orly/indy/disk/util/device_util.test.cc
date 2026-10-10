/* <orly/indy/disk/util/device_util.test.cc>

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

#include <orly/indy/disk/util/device_util.h>

#include <base/test/kit.h>
#include <base/tmp_file.h>

using namespace Orly::Indy::Disk::Util;

FIXTURE(GetPathToDeviceInfo) {
  EXPECT_EQ(TDeviceUtil::GetPathToDeviceInfo("sda5"), "/sys/block/sda/");
  EXPECT_EQ(TDeviceUtil::GetPathToDeviceInfo("sr0"), "/sys/block/sr0/");
  EXPECT_EQ(TDeviceUtil::GetPathToDeviceInfo("loop0"), "/sys/block/loop0/");
  EXPECT_EQ(TDeviceUtil::GetPathToDeviceInfo("dm-2"), "/sys/block/dm-2/");

}

FIXTURE(GetPathToPartitionInfo) {
  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("sda5"), "/sys/block/sda/sda5/");
  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("sr0"), "/sys/block/sr0/");
  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("loop0"), "/sys/block/loop0/");
  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("dm-2"), "/sys/block/dm-2/");

  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("sdb6"), "/sys/block/sdb/sdb6/");
  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("sde0"), "/sys/block/sde/sde0/");
  EXPECT_EQ(TDeviceUtil::GetPathToPartitionInfo("sdaz19"), "/sys/block/sdaz/sdaz19/");
}

FIXTURE(SuperBlockWalFields) {
  Base::TTmpFile tmp_file;
  EXPECT_EQ(ftruncate(tmp_file.GetFd(), TDeviceUtil::BlockSize * 2), 0);
  TDeviceUtil::TOrlyDevice dev_write;
  dev_write.VolumeId.Id = 42UL;
  strncpy(dev_write.VolumeId.InstanceName, "test_inst", MaxInstanceNameSize);
  dev_write.VolumeDeviceNumber = 1UL;
  dev_write.NumDevicesInVolume = 1UL;
  dev_write.LogicalExtentStart = 0UL;
  dev_write.LogicalExtentSize = 1000UL;
  dev_write.VolumeStrategy = 0UL;
  dev_write.VolumeSpeed = 0UL;
  dev_write.ReplicationFactor = 1UL;
  dev_write.StripeSizeKB = 64UL;
  dev_write.LogicalBlockSize = 4096UL;
  dev_write.PhysicalBlockSize = 65536UL;
  dev_write.NumLogicalBlockExposed = 1000UL;
  dev_write.MinDiscardBlocks = 1UL;
  dev_write.FormatVersion = 1UL;
  dev_write.WalStartBlock = 10UL;
  dev_write.WalNumBlocks = 4096UL;
  dev_write.WalCheckpoint0Block = 4106UL;
  dev_write.WalCheckpoint1Block = 4107UL;

  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);

  TDeviceUtil::TOrlyDevice dev_read;
  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 1UL);
  EXPECT_EQ(dev_read.WalStartBlock, 10UL);
  EXPECT_EQ(dev_read.WalNumBlocks, 4096UL);
  EXPECT_EQ(dev_read.WalCheckpoint0Block, 4106UL);
  EXPECT_EQ(dev_read.WalCheckpoint1Block, 4107UL);
  EXPECT_EQ(dev_read.VolumeId.Id, 42UL);
  EXPECT_EQ(std::string(dev_read.VolumeId.InstanceName), "test_inst");
}