/* <orly/indy/disk/key_filter.test.cc>

   Unit test for <orly/indy/disk/key_filter.h>.

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

#include <orly/indy/disk/key_filter.h>

#include <cstdint>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy::Disk;

FIXTURE(EmptyHoldsNothing) {
  const TKeyFilter filter{span<const uint64_t>()};
  EXPECT_EQ(filter.GetNumBytes(), 0UL);
  for (const uint64_t hash : {uint64_t{0}, uint64_t{1}, uint64_t{42}, numeric_limits<uint64_t>::max()}) {
    EXPECT_FALSE(filter.MayContain(hash));
  }
}

/* Every hash it was made from, including the extremes and runs of small integers (a small integer key's hash may be close
   to itself), is reported. */
FIXTURE(NoFalseNegatives) {
  mt19937_64 rng(1);
  for (const size_t num : {1UL, 2UL, 7UL, 100UL, 1000UL, 100000UL}) {
    vector<uint64_t> hashes{0UL, numeric_limits<uint64_t>::max()};
    for (size_t i = 0; i < num; ++i) {
      hashes.push_back(i % 2 ? rng() : i);
    }
    const TKeyFilter filter(hashes);
    size_t missed = 0UL;
    for (const uint64_t hash : hashes) {
      missed += !filter.MayContain(hash);
    }
    EXPECT_EQ(missed, 0UL);
  }
}

/* About 1.5 bytes an entry, and hashes it wasn't made from come back true rarely: under 2% here, where the design is
   about 0.5%. Sequential hashes check the mixing. */
FIXTURE(FalsePositiveRate) {
  constexpr size_t num = 50000UL;
  vector<uint64_t> hashes;
  unordered_set<uint64_t> held;
  for (size_t i = 0; i < num; ++i) {
    hashes.push_back(i * 2UL);
    held.insert(i * 2UL);
  }
  const TKeyFilter filter(hashes);
  EXPECT_LE(filter.GetNumBytes(), (num * TKeyFilter::BitsPerEntry) / 8UL + 32UL);
  mt19937_64 rng(2);
  size_t probes = 0UL, false_positives = 0UL;
  for (size_t i = 0; i < 200000UL; ++i) {
    const uint64_t hash = i % 2 ? rng() : (i * 2UL + 1UL);
    if (held.count(hash)) {
      continue;
    }
    ++probes;
    false_positives += filter.MayContain(hash);
  }
  EXPECT_LT(false_positives * 50UL, probes);
}
