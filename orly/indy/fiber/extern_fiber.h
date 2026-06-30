/* <orly/indy/fiber/extern_fiber.h>

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

#include <base/class_traits.h>

namespace Orly {

  namespace Indy {

    namespace Fiber {

      /* Forward Declarations. */
      class TSync;
      class TRunner;

    }  // Fiber

    namespace ExternFiber {

      #ifndef NDEBUG
      constexpr size_t SyncImplSize = 32UL;
      #else
      constexpr size_t SyncImplSize = 24UL;
      #endif

      /* This should only be used within the same scheduler. It is not safe to use between schedulers. */
      class TSync {
        NO_COPY(TSync);
        public:

        TSync(size_t waiting_for = 0UL);

        ~TSync();

        void Sync(bool come_back_right_away = true);

        void Complete();

        void WaitForMore(size_t num);

        private:

        inline Fiber::TSync *GetImpl() {
          return reinterpret_cast<Fiber::TSync *>(&SyncSpace[0]);
        }

        char SyncSpace[SyncImplSize];

      };  // TSync

      void SchedTaskLocally(const std::function<void ()> &func);

    }  // Fiber

  }  // Indy

}  // Orly
