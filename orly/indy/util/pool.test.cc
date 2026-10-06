/* <orly/indy/util/pool.test.cc>

   Unit test for <orly/indy/util/pool.h>.

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

/* Before pool.h: see pool.cc. */
#include <orly/indy/fiber/fiber.h>

#include <orly/indy/util/pool.h>

#include <new>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy::Util;

/* Writers are admitted until the blocks in use plus those promised would leave fewer than the
   reserve free (#607). */
FIXTURE(AdmitStopsAtReserve) {
  TPool pool(sizeof(void *), "test", 100UL);
  pool.SetReserve(25UL);
  EXPECT_EQ(pool.GetReserve(), 25UL);
  EXPECT_TRUE(pool.TryAdmit(50UL));
  EXPECT_TRUE(pool.TryAdmit(25UL));
  EXPECT_EQ(pool.GetNumBlocksAdmitted(), 75UL);
  EXPECT_FALSE(pool.IsRefusing());
  EXPECT_FALSE(pool.TryAdmit(1UL));
  EXPECT_TRUE(pool.IsRefusing());
  /* A refusal promises nothing. */
  EXPECT_EQ(pool.GetNumBlocksAdmitted(), 75UL);
  pool.ReleaseAdmitted(75UL);
  EXPECT_EQ(pool.GetNumBlocksAdmitted(), 0UL);
}

/* Blocks in use count against the limit, and allocations themselves are never restricted, so
   the merges can use the reserve. */
FIXTURE(AdmitCountsBlocksInUse) {
  TPool pool(sizeof(void *), "test", 100UL);
  pool.SetReserve(25UL);
  vector<void *> blocks;
  for (size_t i = 0; i < 70UL; ++i) {
    blocks.push_back(pool.Alloc(sizeof(void *)));
  }
  EXPECT_TRUE(pool.TryAdmit(5UL));
  EXPECT_FALSE(pool.TryAdmit(1UL));
  for (size_t i = 0; i < 30UL; ++i) {
    blocks.push_back(pool.TryAlloc(sizeof(void *)));
    EXPECT_TRUE(blocks.back() != nullptr);
  }
  EXPECT_EQ(pool.GetNumBlocksUsed(), 100UL);
  for (void *block : blocks) {
    pool.Free(block);
  }
  pool.ReleaseAdmitted(5UL);
}

/* A refusal reports what it compared (#719): the blocks in use, those promised to writers, those
   claimed for copies, the blocks asked for, and what writers may use. */
FIXTURE(RefusalReportsCounts) {
  TPool pool(sizeof(void *), "test", 100UL);
  pool.SetReserve(25UL);
  vector<void *> blocks;
  for (size_t i = 0; i < 40UL; ++i) {
    blocks.push_back(pool.Alloc(sizeof(void *)));
  }
  EXPECT_TRUE(pool.TryAdmit(10UL));
  EXPECT_TRUE(pool.TryClaim(20UL));
  TPool::TRefusal refusal;
  EXPECT_FALSE(pool.TryAdmit(6UL, &refusal));
  EXPECT_EQ(refusal.Used, 40UL);
  EXPECT_EQ(refusal.Admitted, 10UL);
  EXPECT_EQ(refusal.Claimed, 20UL);
  EXPECT_EQ(refusal.Asked, 6UL);
  EXPECT_EQ(refusal.Limit, 75UL);
  /* Once refusing, the limit includes the hysteresis. */
  EXPECT_FALSE(pool.TryAdmit(6UL, &refusal));
  EXPECT_EQ(refusal.Limit, 69UL);
  /* An admission leaves it alone. */
  pool.ReleaseClaim(20UL);
  refusal = TPool::TRefusal();
  EXPECT_TRUE(pool.TryAdmit(6UL, &refusal));
  EXPECT_EQ(refusal.Used, 0UL);
  pool.ReleaseAdmitted(16UL);
  for (void *block : blocks) {
    pool.Free(block);
  }
}

/* Once refusing, a writer is admitted again only when a further reserve / 4 is free. */
FIXTURE(Hysteresis) {
  TPool pool(sizeof(void *), "test", 100UL);
  pool.SetReserve(20UL);
  vector<void *> blocks;
  for (size_t i = 0; i < 80UL; ++i) {
    blocks.push_back(pool.Alloc(sizeof(void *)));
  }
  EXPECT_FALSE(pool.TryAdmit(1UL));
  /* 79 in use + 1 = 80, the limit when accepting, but refusing needs 5 more free. */
  pool.Free(blocks.back());
  blocks.pop_back();
  EXPECT_FALSE(pool.TryAdmit(1UL));
  while (blocks.size() > 74UL) {
    pool.Free(blocks.back());
    blocks.pop_back();
  }
  EXPECT_TRUE(pool.TryAdmit(1UL));
  EXPECT_FALSE(pool.IsRefusing());
  pool.ReleaseAdmitted(1UL);
  for (void *block : blocks) {
    pool.Free(block);
  }
}

/* A reserve of 0 refuses nothing but still counts what writers hold. */
FIXTURE(ZeroReserveNeverRefuses) {
  TPool pool(sizeof(void *), "test", 10UL);
  EXPECT_EQ(pool.GetReserve(), 0UL);
  EXPECT_TRUE(pool.TryAdmit(10UL));
  EXPECT_TRUE(pool.TryAdmit(10UL));
  EXPECT_EQ(pool.GetNumBlocksAdmitted(), 20UL);
  EXPECT_FALSE(pool.IsRefusing());
  pool.ReleaseAdmitted(20UL);
}

/* Off a fiber, an empty pool still waits (up to 2 s) and then throws; the miss is counted. */
FIXTURE(AllocThrowsWhenEmpty) {
  TPool pool(sizeof(void *), "test", 2UL);
  void *a = pool.Alloc(sizeof(void *));
  void *b = pool.Alloc(sizeof(void *));
  EXPECT_EQ(pool.GetNumMisses(), 0UL);
  bool threw = false;
  try {
    pool.Alloc(sizeof(void *));
  } catch (const bad_alloc &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  EXPECT_EQ(pool.GetNumMisses(), 1UL);
  pool.Free(a);
  pool.Free(b);
}

/* A copy's claim is granted whole or not at all, against what is in use, promised and already
   claimed, so two copies never each hold part of what they need (#607). */
FIXTURE(ClaimIsAllOrNothing) {
  TPool pool(sizeof(void *), "test", 100UL);
  vector<void *> blocks;
  for (size_t i = 0; i < 60UL; ++i) {
    blocks.push_back(pool.Alloc(sizeof(void *)));
  }
  EXPECT_FALSE(pool.TryClaim(50UL));
  EXPECT_EQ(pool.GetNumBlocksClaimed(), 0UL);
  EXPECT_TRUE(pool.TryClaim(40UL));
  EXPECT_FALSE(pool.TryClaim(1UL));
  EXPECT_TRUE(pool.TryClaim(0UL));
  pool.ReleaseClaim(40UL);
  EXPECT_TRUE(pool.TryClaim(40UL));
  pool.ReleaseClaim(40UL);
  EXPECT_EQ(pool.GetNumBlocksClaimed(), 0UL);
  for (void *block : blocks) {
    pool.Free(block);
  }
}

/* Writers count claims, so a write admitted while a copy holds its claim can't take the blocks
   the copy is about to allocate. */
FIXTURE(WritersCountClaims) {
  TPool pool(sizeof(void *), "test", 100UL);
  pool.SetReserve(25UL);
  EXPECT_TRUE(pool.TryClaim(50UL));
  EXPECT_TRUE(pool.TryAdmit(25UL));
  EXPECT_FALSE(pool.TryAdmit(1UL));
  pool.ReleaseClaim(50UL);
  pool.ReleaseAdmitted(25UL);
}
