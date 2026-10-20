/* <orly/indy/disk/util/device_util.h>

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

#pragma once

#include <fcntl.h>
#include <string.h>
#include <syslog.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include <base/class_traits.h>
#include <base/dir_iter.h>
#include <base/fd.h>
#include <base/murmur.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        static constexpr size_t MaxInstanceNameSize = 24UL;

        struct TVolumeId {
          size_t Id;
          char InstanceName[MaxInstanceNameSize];
          inline bool operator==(const TVolumeId &that) const {
            return Id == that.Id && memcmp(InstanceName, that.InstanceName, MaxInstanceNameSize) == 0;
          }
          inline size_t GetHash() const {
            return Id;
          }
          inline std::string GetInstanceName() const {
            return std::string(InstanceName);
          }
        };

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly

namespace std {

  /* A standard hasher for Orly::Indy::Disk::Util::TVolumeId. */
  template <>
  struct hash<Orly::Indy::Disk::Util::TVolumeId> {
    typedef size_t result_type;
    typedef Orly::Indy::Disk::Util::TVolumeId argument_type;
    size_t operator()(const argument_type &that) const {
      return that.GetHash();
    }
  };

}  // std

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        class TDeviceUtil {
          NO_CONSTRUCTION(TDeviceUtil);
          public:

          static constexpr uint64_t OrlyFSMagicNumber = 15684509348693311000UL;
          static constexpr uint64_t BlockSize = 4096 * 16;
          static constexpr uint64_t CorruptionCheckSize = sizeof(uint64_t);
          static_assert(sizeof(TVolumeId) % sizeof(uint64_t) == 0, "TVolumeId size must be a multiple of 8 bytes");
          static constexpr uint64_t NumDataElem = (BlockSize - CorruptionCheckSize) / sizeof(uint64_t);
          static constexpr uint64_t MagicNumberPos = 0UL;
          static constexpr uint64_t VolumeIdPos = 1UL;
          static constexpr uint64_t VolumeDeviceNumberPos = VolumeIdPos + sizeof(TVolumeId) / sizeof(uint64_t);
          static constexpr uint64_t NumDevicesInVolumePos = VolumeDeviceNumberPos + 1UL;
          static constexpr uint64_t LogicalExtentStartPos = NumDevicesInVolumePos + 1UL;
          static constexpr uint64_t LogicalExtentSizePos = LogicalExtentStartPos + 1UL;
          static constexpr uint64_t VolumeStrategyPos = LogicalExtentSizePos + 1UL;
          static constexpr uint64_t VolumeSpeedPos = VolumeStrategyPos + 1UL;
          static constexpr uint64_t ReplicationFactorPos = VolumeSpeedPos + 1UL;
          static constexpr uint64_t StripeSizeKBPos = ReplicationFactorPos + 1UL;
          static constexpr uint64_t LogicalBlockSizePos = StripeSizeKBPos + 1UL;
          static constexpr uint64_t PhysicalBlockSizePos = LogicalBlockSizePos + 1UL;
          static constexpr uint64_t NumLogicalBlockExposedPos = PhysicalBlockSizePos + 1UL;
          static constexpr uint64_t MinDiscardBlocksPos = NumLogicalBlockExposedPos + 1UL;
          static constexpr uint64_t FormatVersionPos = MinDiscardBlocksPos + 1UL;
          static constexpr uint64_t WalStartBlockPos = FormatVersionPos + 1UL;
          static constexpr uint64_t WalNumBlocksPos = WalStartBlockPos + 1UL;
          static constexpr uint64_t WalCheckpoint0BlockPos = WalNumBlocksPos + 1UL;
          static constexpr uint64_t WalCheckpoint1BlockPos = WalCheckpoint0BlockPos + 1UL;
          /* The length of each checkpoint slot in blocks.  Zero (a store written before the field
             existed) means one block. */
          static constexpr uint64_t WalCheckpointBlocksPos = WalCheckpoint1BlockPos + 1UL;
          /* From format version 3: a bit for each feature the store has used that a binary must
             understand to open it (TDiskEngine::TIncompatFeature).  Zero before version 3. */
          static constexpr uint64_t IncompatFeaturesPos = WalCheckpointBlocksPos + 1UL;

          struct TOrlyDevice {
            TVolumeId VolumeId;
            uint64_t VolumeDeviceNumber;
            uint64_t NumDevicesInVolume;
            uint64_t LogicalExtentStart;
            uint64_t LogicalExtentSize;
            uint64_t VolumeStrategy;
            uint64_t VolumeSpeed;
            uint64_t ReplicationFactor;
            uint64_t StripeSizeKB;
            uint64_t LogicalBlockSize;
            uint64_t PhysicalBlockSize;
            uint64_t NumLogicalBlockExposed;
            uint64_t MinDiscardBlocks;
            uint64_t FormatVersion = 0UL;
            uint64_t WalStartBlock = 0UL;
            uint64_t WalNumBlocks = 0UL;
            uint64_t WalCheckpoint0Block = 0UL;
            uint64_t WalCheckpoint1Block = 0UL;
            uint64_t WalCheckpointBlocks = 0UL;
            uint64_t IncompatFeatures = 0UL;
          };

          static bool ProbeDevice(const char *path, TOrlyDevice &out_device);

          static void ModifyDevice(const char *path, TOrlyDevice &new_device_info);

          static void ZeroSuperBlock(const char *path);

          static bool ForEachDevice(const std::function<bool (const char *)> &cb);

          static bool IsHardDrive(const std::string &dev_path);

          static std::string GetPathToPartitionInfo(const std::string &dev_path);

          static std::string GetPathToDeviceInfo(const std::string &dev_path);

          static size_t GetValFromDeviceInfo(const std::string &dev_name, const std::string &path_to_field);

          static size_t GetDiscardGranularity(const std::string &dev_name) {
            return GetValFromDeviceInfo(dev_name, "queue/discard_granularity");
          }

          static size_t GetDiscardMaxBytes(const std::string &dev_name) {
            return GetValFromDeviceInfo(dev_name, "queue/discard_max_bytes");
          }

          static size_t GetRqAffinity(const std::string &dev_name) {
            return GetValFromDeviceInfo(dev_name, "queue/rq_affinity");
          }

          static size_t GetMaxSegments(const std::string &dev_name) {
            return GetValFromDeviceInfo(dev_name, "queue/max_segments");
          }

          static size_t GetMaxSectorsKb(const std::string &dev_name) {
            return GetValFromDeviceInfo(dev_name, "queue/max_sectors_kb");
          }

        };  // TDeviceUtil

        /* Which store formats a binary opens (#963).  Version 1 adds the WAL (#755) and version 2
           the retention records (#745).  Version 3 changes nothing on disk by itself: it says the
           superblock's IncompatFeatures word is valid, and each bit in it is a feature the store
           has used that a binary must understand to open it.  A binary refuses a version it
           doesn't know, and a version 3 store with a bit it doesn't know, so a feature a store
           has used can never be ignored by a binary that predates it: every binary before version
           3 refuses version 3, whatever the bits.  A store only goes to version 3 when it first
           uses such a feature; until then a binary from before it still opens the store. */
        struct TStoreFormat {
          NO_CONSTRUCTION(TStoreFormat);

          static constexpr uint64_t MaxVersion = 3UL;

          /* The retention records of version 2 (#745). */
          static constexpr uint64_t RetentionFeature = 1UL;

          /* WAL RepoCopy records (#963): a paused safe repo's unpromoted updates copied forward.
             A binary that skipped them would lose those updates once the head passed the
             records that pushed them. */
          static constexpr uint64_t WalRepoCopyFeature = 2UL;

          static constexpr uint64_t KnownFeatures = RetentionFeature | WalRepoCopyFeature;

          /* The features a store at this version and with this word has used. */
          static constexpr uint64_t GetFeatures(uint64_t version, uint64_t features) noexcept {
            return version >= 3UL ? features : (version == 2UL ? RetentionFeature : 0UL);
          }

          /* Empty if this binary opens such a store, else why not. */
          static std::string CheckSupported(uint64_t version, uint64_t features) {
            if (version > MaxVersion) {
              return "unsupported store format version " + std::to_string(version) + ": this binary supports format versions up to " + std::to_string(MaxVersion);
            }
            if (version >= 3UL && (features & ~KnownFeatures)) {
              return "unsupported store features 0x" + ToHex(features & ~KnownFeatures) + " (format version " + std::to_string(version) + "): this binary is too old for this store";
            }
            return {};
          }

          /* The version and word a store has once it has used `feature` too; a store already at
             version 3 keeps it. */
          static constexpr std::pair<uint64_t, uint64_t> WithFeature(uint64_t version, uint64_t features, uint64_t feature) noexcept {
            return {std::max<uint64_t>(version, 3UL), GetFeatures(version, features) | feature};
          }

          private:

          static std::string ToHex(uint64_t val) {
            static constexpr char Digits[] = "0123456789abcdef";
            std::string out;
            do {
              out.insert(out.begin(), Digits[val & 0xF]);
              val >>= 4;
            } while (val);
            return out;
          }

        };  // TStoreFormat

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly
