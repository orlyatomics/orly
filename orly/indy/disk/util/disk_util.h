/* <orly/indy/disk/util/disk_util.h>

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

#include <optional>

#include <base/class_traits.h>
#include <orly/indy/disk/util/device_util.h>
#include <orly/indy/disk/util/volume_manager.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      namespace Util {

        class TDiskUtil {
          NO_COPY(TDiskUtil);
          public:

          TDiskUtil(Base::TScheduler *scheduler,
                    TDiskController *controller,
                    const std::optional<std::string> &instance_filter,
                    bool do_fsync,
                    const TCacheCb &cache_cb,
                    bool do_corruption_check = true);

          void List(std::stringstream &ss) const;

          void CreateVolume(const std::string &instance_name,
                            size_t num_devices,
                            const std::set<std::string> &device_set,
                            const TVolume::TDesc::TKind kind,
                            const size_t replication_factor,
                            const size_t stripe_size_in_kb,
                            const TVolume::TDesc::TStorageSpeed storage_speed,
                            bool do_fsync);

          TVolumeManager *GetVolumeManager(const std::string &instance_name) const;

          inline const std::unordered_set<std::unique_ptr<TDevice>> &GetPersistentDeviceSet() const {
            return PersistentDeviceSet;
          }

          private:

          Base::TScheduler *Scheduler;

          TDiskController *Controller;

          std::unordered_map<std::string, std::unique_ptr<TVolumeManager>> VolumeManagerByInstance;

          std::unordered_map<TVolumeId, std::unique_ptr<TVolume>> VolumeById;
          std::unordered_set<std::unique_ptr<TDevice>> PersistentDeviceSet;

          std::unordered_set<std::string> AllDeviceSet;

          std::unordered_map<std::string, TDeviceUtil::TOrlyDevice> OrlyDeviceMap;

          const TCacheCb &CacheCb;

        };  // TDiskUtil

      }  // Util

    }  // Disk

  }  // Indy

}  // Orly