/* <orly/indy/disk/util/orly_dm.cc>

   The 'main' of the Orly disk manager.

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

#include <iomanip>
#include <iostream>
#include <optional>

#include <base/booster.h>
#include <base/cmd.h>
#include <base/log.h>
#include <orly/indy/disk/util/disk_util.h>
#include <orly/indy/disk/wal.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly::Indy::Fiber;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;

/* Command-line arguments. */
class TCmd final
    : public Base::TLog::TCmd {
  public:

  /* Construct with defaults. */
  TCmd()
      : ZeroSuperBlock(false),
        List(false),
        DowngradeFormat(false),
        CreateVolume(false),
        InstanceName(""),
        VolumeKind("stripe"),
        DeviceSpeed(""),
        NumDevicesInVolume(0UL),
        ReplicationFactor(1UL),
        StripeSizeKB(512){}

  /* Construct from argc/argv. */
  TCmd(int argc, char *argv[])
      : TCmd() {
    Parse(argc, argv, TMeta());
  }

  /* If true, clear the superblock of the provided devices */
  bool ZeroSuperBlock;

  /* If true, then list all the devices. */
  bool List;

  /* If true, downgrade a format version 1 volume to format version 0. */
  bool DowngradeFormat;

  /* If true, then create a new volume. */
  bool CreateVolume;
  std::string InstanceName;
  std::string VolumeKind;
  std::string DeviceSpeed;
  size_t NumDevicesInVolume;
  size_t ReplicationFactor;
  size_t StripeSizeKB;

  /* The device set. */
  std::set<std::string> DeviceSet;

  private:

  /* Our meta-type. */
  class TMeta final
      : public Base::TLog::TCmd::TMeta {
    public:

    /* Registers our fields. */
    TMeta()
        : Base::TLog::TCmd::TMeta("Orly disk utility") {
      Param(
          &TCmd::ZeroSuperBlock, "zero-super-block", Optional, "zero-super-block\0",
          "Zero out the super block of the provded devices."
      );
      Param(
          &TCmd::List, "list", Optional, "list\0l\0",
          "List all the devices on this system."
      );
      Param(
          &TCmd::DowngradeFormat, "downgrade-format", Optional, "downgrade-format\0downgrade_format\0",
          "Downgrade format version 1 volume to format version 0 (refuses if WAL holds uncheckpointed records)."
      );
      Param(
          &TCmd::CreateVolume, "create-volume", Optional, "create-volume\0cb\0",
          "Create a new volume."
      );
      Param(
          &TCmd::InstanceName, "instance-name", Optional, "instance-name\0iname\0",
          "The instance name used for the newly created volume."
      );
      /* Disabling Chained for now
      Param(
          &TCmd::VolumeKind, "strategy", Optional, "strategy\0strat\0",
          "(stripe | chain). The strategy used when creating a new volume."
      );
      */
      Param(
          &TCmd::DeviceSpeed, "device-speed", Optional, "device-speed\0speed\0",
          "(fast | slow). The speed of the devices used in creating a new volume."
      );
      Param(
          &TCmd::NumDevicesInVolume, "num-devices", Optional, "num-devices\0nd\0",
          "The number of device names expected when creating a new volume."
      );
      /* Disabling Replication factor for now
      Param(
          &TCmd::ReplicationFactor, "replication-factor", Optional, "replication-factor\0rf\0",
          "The replication factor used when creating a new volume."
      );
      */
      Param(
          &TCmd::StripeSizeKB, "stripe-size", Optional, "stripe-size\0",
          "The stripe size (in KB) used when creating a new striped volume."
      );
      Param(
          &TCmd::DeviceSet, "dev", Optional, /*"dev\0",*/
          "The list of devices on which to act."
      );
    }

  };  // TCmd::TMeta

};  // TCmd

int main(int argc, char *argv[]) {
  ::TCmd cmd(argc, argv);
  Base::TLog log(cmd);
  /* Devices are named as in /proc/partitions ("loop0"); tolerate the
     natural "/dev/loop0" spelling instead of crashing on a doubled path
     ("/sys/block//dev/loop0/...", #435). */
  {
    std::set<std::string> normalized;
    for (const auto &device : cmd.DeviceSet) {
      static const std::string dev_prefix = "/dev/";
      normalized.insert(
          device.compare(0, dev_prefix.size(), dev_prefix) == 0
              ? device.substr(dev_prefix.size()) : device);
    }
    cmd.DeviceSet = std::move(normalized);
  }
  Base::TScheduler scheduler(Base::TScheduler::TPolicy(1, 64, milliseconds(10)));
  if(cmd.ZeroSuperBlock) {
    const auto &device_set = cmd.DeviceSet;
    for (const auto &device : device_set) {
      string device_path = "/dev/" + device;
      TDeviceUtil::ZeroSuperBlock(device_path.c_str());
    }
    return EXIT_SUCCESS;
  }
  TCacheCb cache_cb = [](TCacheInstr /*cache_instr*/, const TOffset /*logical_start_offset*/, void */*buf*/, size_t /*count*/) {};
  TDiskController controller;
  TDiskUtil disk_util(&scheduler, &controller, std::optional<std::string>(), false /*fsync*/, cache_cb, true);
  if (cmd.List) {
    const char *str_pattern = "|\%12s |%7s |%15s |%5s |%5s |%21s |%20s |%7s |\n";
    const char *num_pattern = "|\%12s |%7s |%15s |%5ld |%5ld |%21ld |%20ld |%7s |\n";
    const char *break_line = "|-------------|--------|----------------|------|------|----------------------|---------------------|--------|\n";
    printf("%s", break_line);
    printf(str_pattern,
           "Device",
           "OrlyFS",
           "Volume Id",
           "Pos",
           "Tot",
           "Logical Extent Start",
           "Logical Exten Size",
           "Kind");
    printf("%s", break_line);
    std::vector<std::tuple<bool, std::string, TDeviceUtil::TOrlyDevice>> device_vec;
    TDeviceUtil::ForEachDevice([&str_pattern, &num_pattern, &device_vec](const char *path) {
      TDeviceUtil::TOrlyDevice device_info;
      string path_to_device = "/dev/";
      path_to_device += path;
      bool ret = TDeviceUtil::ProbeDevice(path_to_device.c_str(), device_info);
      std::stringstream dev_name;
      dev_name << "/dev/" << path;
      device_vec.emplace_back(ret, dev_name.str(), device_info);
      return true;
    });
    /* sort the device by (whether they have a OrlyFS), (Instance Name), (Volume ID), (Position in Volume)*/
    std::sort(device_vec.begin(), device_vec.end(), [](const std::tuple<bool, std::string, TDeviceUtil::TOrlyDevice> &lhs,
                                                       const std::tuple<bool, std::string, TDeviceUtil::TOrlyDevice> &rhs) {
      bool lhs_ret = std::get<0>(lhs);
      bool rhs_ret = std::get<0>(rhs);
      const std::string &lhs_dev_name = std::get<1>(lhs);
      const std::string &rhs_dev_name = std::get<1>(rhs);
      const TDeviceUtil::TOrlyDevice &lhs_device_info = std::get<2>(lhs);
      const TDeviceUtil::TOrlyDevice &rhs_device_info = std::get<2>(rhs);
      if (lhs_ret && ! rhs_ret) {
        return true;
      } else if (!lhs_ret && rhs_ret) {
        return false;
      } else if (lhs_ret && rhs_ret) {
        int vol_comp = strcmp(lhs_device_info.VolumeId.InstanceName, rhs_device_info.VolumeId.InstanceName);
        if (vol_comp == 0) {
          if (lhs_device_info.VolumeId.Id == rhs_device_info.VolumeId.Id) {
            assert(lhs_device_info.VolumeDeviceNumber != rhs_device_info.VolumeDeviceNumber);
            return lhs_device_info.VolumeDeviceNumber < rhs_device_info.VolumeDeviceNumber;
          }
          return lhs_device_info.VolumeId.Id < rhs_device_info.VolumeId.Id;
        }
        return vol_comp < 0;
      } else {
        /* sort by device name if neither is orly fs */
        return lhs_dev_name < rhs_dev_name;
      }
    });
    std::optional<TDeviceUtil::TOrlyDevice> prev_dev_info;
    for (const auto &dev : device_vec) {
      bool ret = std::get<0>(dev);
      const std::string &dev_name = std::get<1>(dev);
      const TDeviceUtil::TOrlyDevice &device_info = std::get<2>(dev);
      if ((!ret && prev_dev_info)
          || (prev_dev_info && ((strcmp(prev_dev_info->VolumeId.InstanceName, device_info.VolumeId.InstanceName) != 0)
             || (strcmp(prev_dev_info->VolumeId.InstanceName, device_info.VolumeId.InstanceName) == 0 && prev_dev_info->VolumeId.Id != device_info.VolumeId.Id)))) {
        /* print out a break line if we're on a different volume */
        printf("%s", break_line);
      }
      if (ret) {
        std::stringstream iname;
        iname << string(device_info.VolumeId.InstanceName) << ", " << device_info.VolumeId.Id;
        printf(num_pattern,
               dev_name.c_str(),
               "YES",
               iname.str().c_str(),
               device_info.VolumeDeviceNumber,
               device_info.NumDevicesInVolume,
               device_info.LogicalExtentStart,
               device_info.LogicalExtentSize,
               device_info.VolumeStrategy == 1UL ? "Stripe" : "Chain");
      } else {
        printf(str_pattern, dev_name.c_str(), "NO", "", "", "", "", "", "");
      }
      if (ret) {
        prev_dev_info = device_info;
      } else {
        prev_dev_info.reset();
      }
    }
    printf("%s", break_line);
    return EXIT_SUCCESS;
  } else if (cmd.CreateVolume) {
    TVolume::TDesc::TKind strategy;
    TVolume::TDesc::TStorageSpeed speed;
    if (strncmp(cmd.VolumeKind.c_str(), "stripe", 5) == 0) {
      strategy = TVolume::TDesc::Striped;
    } else if (strncmp(cmd.VolumeKind.c_str(), "chain", 4) == 0) {
      strategy = TVolume::TDesc::Chained;
    } else {
      throw std::runtime_error("strategy must be (stripe | chain)");
    }
    if (strncmp(cmd.DeviceSpeed.c_str(), "fast", 4) == 0) {
      speed = TVolume::TDesc::TStorageSpeed::Fast;
    } else if (strncmp(cmd.DeviceSpeed.c_str(), "slow", 4) == 0) {
      speed = TVolume::TDesc::TStorageSpeed::Slow;
    } else {
      throw std::runtime_error("device speed must be (fast | slow)");
    }
    disk_util.CreateVolume(cmd.InstanceName, cmd.NumDevicesInVolume, cmd.DeviceSet, strategy, cmd.ReplicationFactor, cmd.StripeSizeKB, speed, false /* fsync */);
  } else if (cmd.DowngradeFormat) {
    if (cmd.DeviceSet.empty()) {
      cerr << "--downgrade-format requires at least one device specified with --dev" << endl;
      return EXIT_FAILURE;
    }
    for (const auto &device : cmd.DeviceSet) {
      string path_to_device = "/dev/" + device;
      TDeviceUtil::TOrlyDevice dev_info;
      if (!TDeviceUtil::ProbeDevice(path_to_device.c_str(), dev_info)) {
        cerr << "Could not probe device " << path_to_device << endl;
        return EXIT_FAILURE;
      }
      if (dev_info.FormatVersion == 0UL) {
        cout << path_to_device << " is already format version 0." << endl;
        continue;
      }
      if (dev_info.FormatVersion > 1UL) {
        cerr << "Unsupported format version " << dev_info.FormatVersion << " on " << path_to_device << endl;
        return EXIT_FAILURE;
      }
      auto dev = std::make_unique<TPersistentDevice>(&controller, path_to_device.c_str(), device.c_str(),
                                                     dev_info.LogicalBlockSize, dev_info.PhysicalBlockSize,
                                                     dev_info.NumLogicalBlockExposed, false, false);
      TWal::TConfig wal_config;
      wal_config.BaseOffset = dev_info.WalStartBlock * dev_info.PhysicalBlockSize;
      wal_config.CapacityBytes = dev_info.WalNumBlocks * dev_info.PhysicalBlockSize;
      wal_config.CheckpointSlot0Offset = dev_info.WalCheckpoint0Block * dev_info.PhysicalBlockSize;
      wal_config.CheckpointSlot1Offset = dev_info.WalCheckpoint1Block * dev_info.PhysicalBlockSize;
      uuid_t raw_inst;
      memset(raw_inst, 0, sizeof(raw_inst));
      memcpy(raw_inst, dev_info.VolumeId.InstanceName, std::min(sizeof(raw_inst), strlen(dev_info.VolumeId.InstanceName)));
      wal_config.StoreId = Base::TUuid(raw_inst);

      uint64_t start_lsn = 1UL;
      uint64_t start_group = 1UL;
      uint32_t start_lap = 1U;
      auto cp_opt = TWal::ReadNewestCheckpoint(dev.get(), wal_config.CheckpointSlot0Offset, wal_config.CheckpointSlot1Offset, wal_config.StoreId);
      if (cp_opt) {
        start_lsn = cp_opt->HeadLsn;
        start_group = cp_opt->CheckpointNum + 1UL;
      }
      auto scan_res = TWal::Scan(dev.get(), wal_config, start_group, start_lsn, start_lap);
      if (scan_res.Status != TScanStatus::Empty && !scan_res.Records.empty()) {
        cerr << "Refusing to downgrade " << path_to_device << ": WAL contains " << scan_res.Records.size()
             << " uncheckpointed records; start the server and shut down cleanly or drain POVs first." << endl;
        return EXIT_FAILURE;
      }
      dev_info.FormatVersion = 0UL;
      dev_info.WalStartBlock = 0UL;
      dev_info.WalNumBlocks = 0UL;
      dev_info.WalCheckpoint0Block = 0UL;
      dev_info.WalCheckpoint1Block = 0UL;
      TDeviceUtil::ModifyDevice(path_to_device.c_str(), dev_info);
      cout << "Successfully downgraded " << path_to_device << " to format version 0." << endl;
    }
    return EXIT_SUCCESS;
  }
}