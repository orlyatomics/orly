/* <base/mlock.cc>

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

#include <base/mlock.h>

#include <cerrno>
#include <sys/mman.h>

#include <base/test/is_in_test.h>
#include <base/util/error.h>

void Base::MlockRaw(const void *val, uint64_t len) {
  int res = mlock(val, len);

  if (res == 0) {
    return;
  }

  // Failure to mlock changes perf, not correctness. On modern Linux the
  // default RLIMIT_MEMLOCK is 64KB (vs effectively unlimited in 2014),
  // so any non-trivial pool allocation hits ENOMEM/EPERM. Degrade
  // silently rather than aborting -- this also covers static-init-time
  // allocations that run before ExtraInit() sets the in-test flag.
  if (errno == ENOMEM || errno == EPERM) {
    return;
  }

  if (Test::AvoidTheseWheneverPossible::IsInTest()) {
    return;
  }

  Util::IfLt0(res);
}