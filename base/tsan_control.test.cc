/* <base/tsan_control.test.cc>

   The ThreadSanitizer CI job's negative control. With
   ORLY_TSAN_RACY_CONTROL set, `Racy` starts two threads that increment a
   plain int with no synchronisation, a data race TSan must report. The job
   runs it first and fails unless TSan does, so an instrumentation or
   suppression change that silences TSan can't turn the gate green by
   accident. `Ordered` does the same work through std::atomic and must stay
   clean. Without the variable `Racy` does nothing, so ordinary test runs never
   execute the race.

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

#include <atomic>
#include <cstdlib>
#include <thread>

#include <base/test/kit.h>

namespace {

  constexpr int Iterations = 100000;

  /* Out of line so the compiler can't fold the loop into one add. */
  [[gnu::noinline]] void Bump(int &n) {
    for (int i = 0; i < Iterations; ++i) {
      n = n + 1;
    }
  }

}  // namespace

FIXTURE(Racy) {
  if (!std::getenv("ORLY_TSAN_RACY_CONTROL")) {
    return;
  }
  int n = 0;
  std::thread a(Bump, std::ref(n));
  std::thread b(Bump, std::ref(n));
  a.join();
  b.join();
  EXPECT_GT(n, 0);
}

FIXTURE(Ordered) {
  std::atomic<int> n{0};
  const auto bump = [&n] {
    for (int i = 0; i < Iterations; ++i) {
      n.fetch_add(1, std::memory_order_relaxed);
    }
  };
  std::thread a(bump);
  std::thread b(bump);
  a.join();
  b.join();
  EXPECT_EQ(n.load(), 2 * Iterations);
}
