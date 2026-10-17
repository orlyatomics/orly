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
  dev_write.WalCheckpointBlocks = 16UL;

  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);

  TDeviceUtil::TOrlyDevice dev_read;
  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 1UL);
  EXPECT_EQ(dev_read.WalStartBlock, 10UL);
  EXPECT_EQ(dev_read.WalNumBlocks, 4096UL);
  EXPECT_EQ(dev_read.WalCheckpoint0Block, 4106UL);
  EXPECT_EQ(dev_read.WalCheckpoint1Block, 4107UL);
  EXPECT_EQ(dev_read.WalCheckpointBlocks, 16UL);
  EXPECT_EQ(dev_read.VolumeId.Id, 42UL);
  EXPECT_EQ(std::string(dev_read.VolumeId.InstanceName), "test_inst");
}

FIXTURE(FormatVersionUpgradeAndDowngrade) {
  Base::TTmpFile tmp_file;
  EXPECT_EQ(ftruncate(tmp_file.GetFd(), TDeviceUtil::BlockSize * 2), 0);

  /* Start as format version 0 */
  TDeviceUtil::TOrlyDevice dev_write;
  dev_write.VolumeId.Id = 99UL;
  strncpy(dev_write.VolumeId.InstanceName, "v0_inst", MaxInstanceNameSize);
  dev_write.FormatVersion = 0UL;
  dev_write.WalStartBlock = 0UL;
  dev_write.WalNumBlocks = 0UL;

  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);

  TDeviceUtil::TOrlyDevice dev_read;
  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 0UL);
  EXPECT_EQ(dev_read.WalNumBlocks, 0UL);

  /* Upgrade to format version 1 */
  dev_write.FormatVersion = 1UL;
  dev_write.WalStartBlock = 100UL;
  dev_write.WalNumBlocks = 2048UL;
  dev_write.WalCheckpoint0Block = 2148UL;
  dev_write.WalCheckpoint1Block = 2149UL;
  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);

  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 1UL);
  EXPECT_EQ(dev_read.WalStartBlock, 100UL);
  EXPECT_EQ(dev_read.WalNumBlocks, 2048UL);

  /* Downgrade back to format version 0 */
  dev_write.FormatVersion = 0UL;
  dev_write.WalStartBlock = 0UL;
  dev_write.WalNumBlocks = 0UL;
  dev_write.WalCheckpoint0Block = 0UL;
  dev_write.WalCheckpoint1Block = 0UL;
  dev_write.WalCheckpointBlocks = 0UL;
  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);

  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 0UL);
  EXPECT_EQ(dev_read.WalNumBlocks, 0UL);
}

FIXTURE(FormatVersionFuture) {
  Base::TTmpFile tmp_file;
  EXPECT_EQ(ftruncate(tmp_file.GetFd(), TDeviceUtil::BlockSize * 2), 0);

  TDeviceUtil::TOrlyDevice dev_write;
  dev_write.FormatVersion = 2UL; // Future format version
  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);

  TDeviceUtil::TOrlyDevice dev_read;
  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 2UL);
}
/* #963: the superblock carries the features a store has used, and a binary refuses a store that
   used one it doesn't know. */
FIXTURE(IncompatFeaturesRoundTrip) {
  Base::TTmpFile tmp_file;
  EXPECT_EQ(ftruncate(tmp_file.GetFd(), TDeviceUtil::BlockSize * 2), 0);
  TDeviceUtil::TOrlyDevice dev_write;
  dev_write.FormatVersion = 2UL;
  dev_write.WalCheckpointBlocks = 256UL;
  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);
  TDeviceUtil::TOrlyDevice dev_read;
  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.IncompatFeatures, 0UL);
  const auto [version, features] = TStoreFormat::WithFeature(dev_read.FormatVersion, dev_read.IncompatFeatures, TStoreFormat::WalRepoCopyFeature);
  dev_write.FormatVersion = version;
  dev_write.IncompatFeatures = features;
  TDeviceUtil::ModifyDevice(tmp_file.GetName(), dev_write);
  EXPECT_TRUE(TDeviceUtil::ProbeDevice(tmp_file.GetName(), dev_read));
  EXPECT_EQ(dev_read.FormatVersion, 3UL);
  /* A version 2 store kept retention records, and keeps saying so. */
  EXPECT_EQ(dev_read.IncompatFeatures, TStoreFormat::RetentionFeature | TStoreFormat::WalRepoCopyFeature);
  /* The word doesn't disturb the field before it. */
  EXPECT_EQ(dev_read.WalCheckpointBlocks, 256UL);
}

FIXTURE(StoreFormatFeatures) {
  /* What a store has used, by version. */
  EXPECT_EQ(TStoreFormat::GetFeatures(0UL, 0UL), 0UL);
  EXPECT_EQ(TStoreFormat::GetFeatures(1UL, 0UL), 0UL);
  EXPECT_EQ(TStoreFormat::GetFeatures(2UL, 0UL), TStoreFormat::RetentionFeature);
  /* Before version 3 the word is not read, whatever is there. */
  EXPECT_EQ(TStoreFormat::GetFeatures(2UL, 0xffUL), TStoreFormat::RetentionFeature);
  EXPECT_EQ(TStoreFormat::GetFeatures(3UL, TStoreFormat::WalRepoCopyFeature), TStoreFormat::WalRepoCopyFeature);
  /* Taking a version 0 or 1 store to a feature keeps what it didn't have. */
  EXPECT_TRUE(TStoreFormat::WithFeature(1UL, 0UL, TStoreFormat::WalRepoCopyFeature) == std::make_pair(3UL, TStoreFormat::WalRepoCopyFeature));
  /* A version 3 store keeps its version and its other bits. */
  EXPECT_TRUE(TStoreFormat::WithFeature(3UL, TStoreFormat::WalRepoCopyFeature, TStoreFormat::RetentionFeature) ==
              std::make_pair(3UL, TStoreFormat::RetentionFeature | TStoreFormat::WalRepoCopyFeature));
}

FIXTURE(StoreFormatDowngradeRefused) {
  /* This binary opens every version up to 3 with the features it knows. */
  for (uint64_t version = 0UL; version <= 2UL; ++version) {
    EXPECT_TRUE(TStoreFormat::CheckSupported(version, 0UL).empty());
  }
  EXPECT_TRUE(TStoreFormat::CheckSupported(3UL, TStoreFormat::KnownFeatures).empty());
  /* A feature it doesn't know: a newer binary's store. */
  const std::string unknown = TStoreFormat::CheckSupported(3UL, TStoreFormat::WalRepoCopyFeature | 4UL);
  EXPECT_FALSE(unknown.empty());
  EXPECT_NE(unknown.find("0x4"), std::string::npos);
  EXPECT_FALSE(TStoreFormat::CheckSupported(4UL, 0UL).empty());
  /* A binary from before #963 opens versions up to 2 only (TDiskEngine::MaxFormatVersion was 2),
     so it refuses every store that has written a RepoCopy record: the first one takes it to 3. */
  constexpr uint64_t OldMaxVersion = 2UL;
  for (uint64_t version = 0UL; version <= 3UL; ++version) {
    EXPECT_GT(TStoreFormat::WithFeature(version, 0UL, TStoreFormat::WalRepoCopyFeature).first, OldMaxVersion);
  }
}
