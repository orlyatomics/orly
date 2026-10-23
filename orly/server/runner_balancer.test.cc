/* <orly/server/runner_balancer.test.cc>

   Unit test for <orly/server/runner_balancer.h>.

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

#include <orly/server/runner_balancer.h>

#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Server;

using TLease = TRunnerBalancer::TLease;

/* With nothing in flight, the assignment is the plain round-robin it was before #908. */
FIXTURE(IdleIsRoundRobin) {
  TRunnerBalancer balancer(5UL);
  for (size_t i = 0UL; i < 12UL; ++i) {
    auto lease = balancer.Acquire();
    EXPECT_EQ(lease.GetIndex(), i % 5UL);
  }
  for (size_t i = 0UL; i < 5UL; ++i) {
    EXPECT_EQ(balancer.GetLoad(i), 0UL);
  }
}

/* #908: a runner held by a long statement gets nothing more while another runner is idle. */
FIXTURE(BusyRunnerIsAvoided) {
  TRunnerBalancer balancer(5UL);
  TLease batch = balancer.Acquire();
  const size_t busy = batch.GetIndex();
  for (size_t i = 0UL; i < 100UL; ++i) {
    auto read = balancer.Acquire();
    EXPECT_NE(read.GetIndex(), busy);
  }
  EXPECT_EQ(balancer.GetLoad(busy), 1UL);
  batch.Release();
  EXPECT_EQ(balancer.GetLoad(busy), 0UL);
  /* Free again, it takes its turn. */
  bool seen = false;
  for (size_t i = 0UL; i < 5UL; ++i) {
    seen |= balancer.Acquire().GetIndex() == busy;
  }
  EXPECT_TRUE(seen);
}

/* Statements that pile up are spread evenly, and new ones go to the shortest queue. */
FIXTURE(ShortestQueueWins) {
  TRunnerBalancer balancer(4UL);
  vector<TLease> held;
  for (size_t i = 0UL; i < 12UL; ++i) {
    held.push_back(balancer.Acquire());
  }
  for (size_t i = 0UL; i < 4UL; ++i) {
    EXPECT_EQ(balancer.GetLoad(i), 3UL);
  }
  /* Free two on runner 2 and one on runner 1: the next two go to 2, then one to 1 or 2. */
  size_t freed_on_2 = 0UL, freed_on_1 = 0UL;
  for (auto &lease: held) {
    if (lease.GetIndex() == 2UL && freed_on_2 < 2UL) {
      lease.Release();
      ++freed_on_2;
    } else if (lease.GetIndex() == 1UL && !freed_on_1) {
      lease.Release();
      ++freed_on_1;
    }
  }
  EXPECT_EQ(balancer.GetLoad(2UL), 1UL);
  EXPECT_EQ(balancer.GetLoad(1UL), 2UL);
  auto a = balancer.Acquire();
  EXPECT_EQ(a.GetIndex(), 2UL);
  auto b = balancer.Acquire();
  const size_t b_index = b.GetIndex();
  EXPECT_TRUE(b_index == 1UL || b_index == 2UL);
  auto c = balancer.Acquire();
  EXPECT_TRUE(c.GetIndex() == 1UL || c.GetIndex() == 2UL);
  EXPECT_NE(c.GetIndex(), b_index);
  EXPECT_EQ(balancer.GetLoad(1UL), 3UL);
  EXPECT_EQ(balancer.GetLoad(2UL), 3UL);
}

/* A lease releases its load exactly once, wherever it ends up. */
FIXTURE(LeaseMoves) {
  TRunnerBalancer balancer(2UL);
  TLease empty;
  EXPECT_FALSE(empty);
  empty.Release();
  TLease first = balancer.Acquire();
  EXPECT_TRUE(first);
  const size_t index = first.GetIndex();
  TLease moved(std::move(first));
  EXPECT_FALSE(first);
  EXPECT_TRUE(moved);
  EXPECT_EQ(moved.GetIndex(), index);
  EXPECT_EQ(balancer.GetLoad(index), 1UL);
  /* Move-assigning over a live lease releases the one it held. */
  TLease other = balancer.Acquire();
  const size_t other_index = other.GetIndex();
  EXPECT_NE(other_index, index);
  other = std::move(moved);
  EXPECT_EQ(balancer.GetLoad(other_index), 0UL);
  EXPECT_EQ(balancer.GetLoad(index), 1UL);
  other.Release();
  other.Release();
  EXPECT_EQ(balancer.GetLoad(index), 0UL);
  {
    TLease scoped = balancer.Acquire();
    EXPECT_EQ(balancer.GetLoad(scoped.GetIndex()), 1UL);
  }
  EXPECT_EQ(balancer.GetLoad(0UL), 0UL);
  EXPECT_EQ(balancer.GetLoad(1UL), 0UL);
}

/* #908 with many sessions: every runner has a small statement or two in flight, and the one
   holding a big batch, weighted by its calls, still gets nothing until the others catch up. */
FIXTURE(WeightedBatchIsAvoided) {
  TRunnerBalancer balancer(4UL);
  TLease batch = balancer.Acquire(100UL);
  const size_t busy = batch.GetIndex();
  EXPECT_EQ(balancer.GetLoad(busy), 100UL);
  vector<TLease> reads;
  for (size_t i = 0UL; i < 3UL * 100UL; ++i) {
    reads.push_back(balancer.Acquire());
    EXPECT_NE(reads.back().GetIndex(), busy);
  }
  /* Now all four are at 100: the next goes wherever rotation says. */
  for (size_t i = 0UL; i < 4UL; ++i) {
    EXPECT_EQ(balancer.GetLoad(i), 100UL);
  }
  reads.clear();
  batch.Release();
  for (size_t i = 0UL; i < 4UL; ++i) {
    EXPECT_EQ(balancer.GetLoad(i), 0UL);
  }
  /* A weight of 0 still counts as one statement. */
  auto zero = balancer.Acquire(0UL);
  EXPECT_EQ(balancer.GetLoad(zero.GetIndex()), 1UL);
}

/* A statement already on a runner charges it the rest of its weight once it knows it, as a
   websocket batch does from RunBatch(); the charge goes when the lease does. */
FIXTURE(ChargeInPlace) {
  TRunnerBalancer balancer(3UL);
  TLease statement = balancer.Acquire();
  const size_t index = statement.GetIndex();
  {
    TLease charge = balancer.Charge(index, 49UL);
    EXPECT_TRUE(charge);
    EXPECT_EQ(charge.GetIndex(), index);
    EXPECT_EQ(balancer.GetLoad(index), 50UL);
    for (size_t i = 0UL; i < 20UL; ++i) {
      EXPECT_NE(balancer.Acquire().GetIndex(), index);
    }
    /* A weighted lease moves with its weight. */
    TLease moved(std::move(charge));
    EXPECT_EQ(balancer.GetLoad(index), 50UL);
  }
  EXPECT_EQ(balancer.GetLoad(index), 1UL);
  TLease nothing = balancer.Charge(index, 0UL);
  EXPECT_FALSE(nothing);
  EXPECT_EQ(balancer.GetLoad(index), 1UL);
}

FIXTURE(LeaseGrows) {
  TRunnerBalancer balancer(2UL);
  TLease scan = balancer.Acquire();
  const size_t index = scan.GetIndex();
  /* A read that turns out long weighs more as it goes (#1015)... */
  scan.Add(10UL);
  EXPECT_EQ(balancer.GetLoad(index), 11UL);
  for (size_t i = 0UL; i < 5UL; ++i) {
    EXPECT_NE(balancer.Acquire().GetIndex(), index);
  }
  /* ...and gives all of it back at once. */
  scan.Release();
  EXPECT_EQ(balancer.GetLoad(index), 0UL);
  EXPECT_EQ(balancer.Acquire().GetIndex(), index);
}

FIXTURE(Sizes) {
  EXPECT_THROW(std::invalid_argument, [] { TRunnerBalancer balancer(0UL); (void)balancer; });
  TRunnerBalancer one(1UL);
  auto a = one.Acquire();
  auto b = one.Acquire();
  EXPECT_EQ(a.GetIndex(), 0UL);
  EXPECT_EQ(b.GetIndex(), 0UL);
  EXPECT_EQ(one.GetLoad(0UL), 2UL);
}

/* Many threads at once, as the websocket I/O threads and the fast runners assign: every lease
   lands on a real runner and every load returns to zero. (Which runner each one gets is racy by
   design, so this checks consistency, not choice.) */
FIXTURE(Concurrent) {
  TRunnerBalancer balancer(5UL);
  TLease batch = balancer.Acquire();
  const size_t busy = batch.GetIndex();
  vector<thread> threads;
  vector<size_t> bad(4UL, 0UL);
  for (size_t t = 0UL; t < 4UL; ++t) {
    threads.emplace_back([&balancer, &bad, t] {
      for (size_t i = 0UL; i < 100000UL; ++i) {
        auto lease = balancer.Acquire();
        bad[t] += lease.GetIndex() >= balancer.GetSize() || !balancer.GetLoad(lease.GetIndex());
      }
    });
  }
  for (auto &thread: threads) {
    thread.join();
  }
  for (size_t t = 0UL; t < 4UL; ++t) {
    EXPECT_EQ(bad[t], 0UL);
  }
  EXPECT_EQ(balancer.GetLoad(busy), 1UL);
  batch.Release();
  for (size_t i = 0UL; i < 5UL; ++i) {
    EXPECT_EQ(balancer.GetLoad(i), 0UL);
  }
}

/* AcquireBelow() takes the least-loaded runner only while its load is under the cap. */
FIXTURE(AcquireBelowRespectsTheCap) {
  TRunnerBalancer balancer(2UL);
  TLease a = balancer.AcquireBelow(1UL);
  TLease b = balancer.AcquireBelow(1UL);
  EXPECT_TRUE(a && b);
  EXPECT_NE(a.GetIndex(), b.GetIndex());
  EXPECT_FALSE(balancer.AcquireBelow(1UL));
  EXPECT_EQ(balancer.GetLoad(0UL) + balancer.GetLoad(1UL), 2UL);
  TLease c = balancer.AcquireBelow(2UL, 3UL);
  EXPECT_TRUE(c);
  EXPECT_EQ(balancer.GetLoad(c.GetIndex()), 4UL);
  c.Release();
  const size_t freed = b.GetIndex();
  b.Release();
  /* The one runner back under the cap is the one it picks. */
  TLease d = balancer.AcquireBelow(1UL);
  EXPECT_TRUE(d);
  EXPECT_EQ(d.GetIndex(), freed);
}
