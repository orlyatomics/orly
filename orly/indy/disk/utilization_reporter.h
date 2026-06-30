/* <orly/indy/disk/utilization_reporter.h>

   Abstract base for I/O utilization reporting. `Push(source, kind,
   bytes, priority)` is called from the disk read/write paths;
   `Report(ss)` emits a summary into a stringstream. Concrete impl is
   `TIndyUtilReporter` (see `indy_util_reporter.h`); the base lives
   separately so tests and non-orly callers can plug in their own
   accounting.

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
#include <orly/indy/disk/priority.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      class TUtilizationReporter {
        NO_COPY(TUtilizationReporter);
        public:

        enum TKind {
          SyncRead,
          AsyncRead,
          Write
        };

        virtual void Push(uint8_t source, TKind kind, size_t num_bytes, DiskPriority priority) = 0;

        virtual void Report(std::stringstream &ss) = 0;

        protected:

        TUtilizationReporter() {}

        virtual ~TUtilizationReporter() {}

      };  // TUtilizationReporter

    }  // Disk

  }  // Indy

}  // Orly