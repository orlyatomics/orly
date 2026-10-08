/* <orly/durable/test_manager.h>

   `Durable::TTestManager` -- an in-memory `Durable::TManager` used
   by tests. No real disk I/O; the durable layer's `CanLoad` /
   `CleanDisk` / `Delete` operations all run against an in-memory
   `BlobById` map.

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

#include <functional>
#include <mutex>
#include <unordered_map>

#include <orly/durable/kit.h>

namespace Orly {

  namespace Durable {

    class TTestManager final
        : public TManager {
      public:

      TTestManager(size_t max_cache_size)
          : TManager(max_cache_size) {}

      /* If set, CanLoad() and TryLoad() call this after their look, outside every lock, as a slow
         disk read would end: a test can hold a load there (#804). */
      std::function<void (const TId &)> AfterLoad;

      private:

      /* TManager calls this and TryLoad() without its mutex (#804), so the map has a lock of its own. */
      virtual bool CanLoad(const TId &id) override {
        bool found;
        /* extra */ {
          std::lock_guard<std::mutex> lock(BlobMutex);
          found = BlobById.find(id) != BlobById.end();
        }
        if (AfterLoad) {
          AfterLoad(id);
        }
        return found;
      }

      virtual void RunLayerCleaner() override {}

      virtual void CleanDisk(const TDeadline &now, TSem *sem) override {
        assert(sem);
        std::lock_guard<std::mutex> lock(BlobMutex);
        std::unordered_map<TId, std::pair<TDeadline, std::string>> temp;
        for (const auto &item: BlobById) {
          if (item.second.first > now) {
            temp.insert(item);
          }
        }
        swap(BlobById, temp);
        sem->Push();
      }

      virtual void Delete(const TId &id, TSem *sem) override {
        assert(sem);
        std::lock_guard<std::mutex> lock(BlobMutex);
        auto erased_count = BlobById.erase(id);
        assert(erased_count == 1);
        sem->Push();
      }

      virtual void Save(const TId &id, const TDeadline &deadline, const TTtl &/*ttl*/, const std::string &blob, TSem *sem) override {
        /* extra */ {
          std::lock_guard<std::mutex> lock(BlobMutex);
          BlobById[id] = std::make_pair(deadline, blob);
        }
        /* The in-memory map IS this manager's disk, so the save is "durable" the moment it's
           inserted; a null sem is a fire-and-forget save (see Save()'s contract in kit.h). */
        if (sem) {
          sem->Push();
        }
      }

      virtual bool TryLoad(const TId &id, std::string &blob) override {
        bool success;
        /* extra */ {
          std::lock_guard<std::mutex> lock(BlobMutex);
          auto iter = BlobById.find(id);
          success = (iter != BlobById.end());
          if (success) {
            blob = iter->second.second;
          }
        }
        if (AfterLoad) {
          AfterLoad(id);
        }
        return success;
      }

      private:

      /* Covers BlobById. */
      std::mutex BlobMutex;

      std::unordered_map<TId, std::pair<TDeadline, std::string>> BlobById;

    };  //TTestManager

  }  // Durable

}  // Orly