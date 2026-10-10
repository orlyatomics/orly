/* <orly/indy/disk/tombstone_counts.test.cc>

   Unit test for <orly/indy/disk/tombstone_counts.h>.

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

#include <orly/indy/disk/tombstone_counts.h>

#include <random>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy::Disk;

namespace {

  constexpr size_t Stride = TTombstoneCounts::Stride;

  /* A file's tombstone flags, with a scan that counts the entries it reads. */
  struct TFile {
    vector<bool> IsTombstone;
    mutable size_t Probes = 0UL;
    size_t Scan(size_t begin, size_t end) const {
      size_t count = 0UL;
      for (size_t i = begin; i < end; ++i) {
        count += IsTombstone[i];
      }
      Probes += end - begin;
      return count;
    }
    size_t Naive(size_t begin, size_t end) const {
      size_t count = 0UL;
      for (size_t i = begin; i < end; ++i) {
        count += IsTombstone[i];
      }
      return count;
    }
  };

  TFile MakeFile(size_t num_keys, double tombstone_rate, mt19937_64 &rng) {
    TFile file;
    bernoulli_distribution coin(tombstone_rate);
    for (size_t i = 0; i < num_keys; ++i) {
      file.IsTombstone.push_back(coin(rng));
    }
    return file;
  }

}  // namespace

/* Every count matches a plain count, over random stretches of files of every shape: empty,
   shorter than a block, a whole number of blocks, a ragged last block; no tombstones, a few,
   and all of them. */
FIXTURE(MatchesNaive) {
  mt19937_64 rng(749);
  for (size_t num_keys : {0UL, 1UL, 7UL, Stride - 1UL, Stride, Stride + 1UL, 3UL * Stride, 5UL * Stride + 17UL}) {
    for (double rate : {0.0, 0.001, 0.3, 1.0}) {
      const TFile file = MakeFile(num_keys, rate, rng);
      const TTombstoneCounts counts(num_keys);
      EXPECT_EQ(counts.GetNumKeys(), num_keys);
      const auto scan = [&](size_t begin, size_t end) { return file.Scan(begin, end); };
      EXPECT_EQ(counts.Count(0UL, num_keys, scan), file.Naive(0UL, num_keys));
      uniform_int_distribution<size_t> pick(0UL, num_keys);
      for (int i = 0; i < 300; ++i) {
        size_t begin = pick(rng), end = pick(rng);
        if (begin > end) {
          swap(begin, end);
        }
        EXPECT_EQ(counts.Count(begin, end, scan), file.Naive(begin, end));
      }
    }
  }
}

/* Once its blocks are read, a stretch reads at most Stride - 1 entries at each end, and none
   where the end block holds no tombstone. */
FIXTURE(WarmReadsOnlyEdges) {
  mt19937_64 rng(4096);
  const size_t num_keys = 40UL * Stride + 123UL;
  const TFile sparse = MakeFile(num_keys, 0.0005, rng);
  const TTombstoneCounts counts(num_keys);
  const auto scan = [&](size_t begin, size_t end) { return sparse.Scan(begin, end); };
  /* The first count of everything reads every entry once. */
  EXPECT_EQ(counts.Count(0UL, num_keys, scan), sparse.Naive(0UL, num_keys));
  EXPECT_EQ(sparse.Probes, num_keys);
  for (size_t block = 0; block * Stride < num_keys; ++block) {
    EXPECT_TRUE(counts.IsKnown(block));
  }
  uniform_int_distribution<size_t> pick(0UL, num_keys);
  for (int i = 0; i < 1000; ++i) {
    size_t begin = pick(rng), end = pick(rng);
    if (begin > end) {
      swap(begin, end);
    }
    sparse.Probes = 0UL;
    EXPECT_EQ(counts.Count(begin, end, scan), sparse.Naive(begin, end));
    size_t bound = 0UL;
    if (begin != end) {
      const size_t first = begin / Stride, last = (end - 1UL) / Stride;
      const auto block_has_tombstone = [&](size_t block) {
        return sparse.Naive(block * Stride, min(num_keys, (block + 1UL) * Stride)) != 0UL;
      };
      if (first == last) {
        bound = block_has_tombstone(first) ? end - begin : 0UL;
      } else {
        bound = (block_has_tombstone(first) ? Stride - 1UL : 0UL) + (block_has_tombstone(last) ? Stride - 1UL : 0UL);
      }
    }
    EXPECT_LE(sparse.Probes, bound);
  }
  /* Whole blocks need no reading at all. */
  sparse.Probes = 0UL;
  EXPECT_EQ(counts.Count(Stride, 30UL * Stride, scan), sparse.Naive(Stride, 30UL * Stride));
  EXPECT_EQ(sparse.Probes, 0UL);
}

/* A cold count reads only the blocks its stretch touches, so a short stretch of a large file
   never pays for the whole file. */
FIXTURE(ColdReadsOnlyTouchedBlocks) {
  mt19937_64 rng(1024);
  const size_t num_keys = 100UL * Stride;
  const TFile file = MakeFile(num_keys, 0.01, rng);
  const TTombstoneCounts counts(num_keys);
  const auto scan = [&](size_t begin, size_t end) { return file.Scan(begin, end); };
  const size_t begin = 50UL * Stride + 10UL, end = begin + 100UL;
  EXPECT_EQ(counts.Count(begin, end, scan), file.Naive(begin, end));
  EXPECT_LE(file.Probes, Stride + 100UL);
  EXPECT_FALSE(counts.IsKnown(49UL));
  EXPECT_TRUE(counts.IsKnown(50UL));
  EXPECT_FALSE(counts.IsKnown(51UL));
}
