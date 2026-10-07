/* <base/parker.test.cc>

   Unit test for <base/parker.h>.

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

#include <base/parker.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace std::chrono;
using namespace Base;

/* A wake that lands before Park() must not be lost: Park() returns at once. */
FIXTURE(WakeBeforePark) {
  TParker parker;
  parker.PrepareToPark();
  parker.Wake();
  const auto start = steady_clock::now();
  parker.Park(seconds(10));
  EXPECT_TRUE(steady_clock::now() - start < seconds(5));
}

/* With nobody waking it, Park() comes back after its timeout. */
FIXTURE(Timeout) {
  TParker parker;
  parker.PrepareToPark();
  const auto start = steady_clock::now();
  parker.Park(milliseconds(20));
  EXPECT_TRUE(steady_clock::now() - start >= milliseconds(15));
}

/* The protocol from parker.h, under contention: several producers push
   counts onto one word and wake the consumer, which takes everything and
   parks whenever the word is empty. Every push must be taken, and with a
   long timeout a lost wakeup shows up as a stall (the test's wall time). */
FIXTURE(NoLostWakeups) {
  constexpr size_t producers = 4, pushes_each = 20000;
  TParker parker;
  std::atomic<size_t> pending(0);
  std::atomic<size_t> slow_parks(0);
  size_t taken = 0;
  std::thread consumer([&] {
    while (taken < producers * pushes_each) {
      const size_t got = pending.exchange(0, std::memory_order_acquire);
      if (got) {
        taken += got;
        continue;
      }
      parker.PrepareToPark();
      if (pending.load(std::memory_order_seq_cst)) {
        parker.CancelPark();
        continue;
      }
      const auto start = steady_clock::now();
      parker.Park(seconds(1));
      if (steady_clock::now() - start >= milliseconds(900)) {
        ++slow_parks;
      }
    }
  });
  std::vector<std::thread> threads;
  for (size_t p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      for (size_t i = 0; i < pushes_each; ++i) {
        pending.fetch_add(1, std::memory_order_seq_cst);
        parker.Wake();
        if ((i + p) % 64 == 0) {
          /* Let the consumer catch up and park now and then. */
          std::this_thread::sleep_for(microseconds(50));
        }
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  consumer.join();
  EXPECT_EQ(taken, producers * pushes_each);
  EXPECT_EQ(slow_parks.load(), 0UL);
}
