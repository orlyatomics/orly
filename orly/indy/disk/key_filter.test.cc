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

#include <alloca.h>

#include <random>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <orly/atom/suprena.h>
#include <orly/native/all.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;

/* Every hash a filter is made from passes, and about 1% of the rest do, whatever the size. */
FIXTURE(NoFalseNegativesFewFalsePositives) {
  mt19937_64 gen(0x5eedULL);
  for (size_t num_keys : {1UL, 7UL, 100UL, 5000UL, 200000UL}) {
    vector<uint64_t> hashes(num_keys);
    unordered_set<uint64_t> held;
    for (auto &hash : hashes) {
      hash = gen();
      held.insert(hash);
    }
    const TKeyFilter filter(hashes);
    EXPECT_GT(filter.GetNumBytes(), 0UL);
    EXPECT_TRUE(filter.GetNumBytes() * 8UL >= num_keys * TKeyFilter::BitsPerKey);
    size_t false_negatives = 0UL;
    for (uint64_t hash : hashes) {
      if (!filter.MayContain(hash)) {
        ++false_negatives;
      }
    }
    EXPECT_EQ(false_negatives, 0UL);
    if (num_keys >= 5000UL) {
      const size_t num_probes = 1000000UL;
      size_t false_positives = 0UL, absent = 0UL;
      for (size_t i = 0UL; i < num_probes; ++i) {
        const uint64_t hash = gen();
        if (held.count(hash)) {
          continue;
        }
        ++absent;
        if (filter.MayContain(hash)) {
          ++false_positives;
        }
      }
      /* About 1% expected; 2.5% leaves room for a blocked filter's spread. */
      EXPECT_LT(false_positives * 1000UL, absent * 25UL);
    }
  }
}

/* Small and sequential hashes: a hash index's slots come from the low bits, so these mustn't
   crowd a few blocks. */
FIXTURE(SequentialHashes) {
  vector<uint64_t> hashes;
  for (uint64_t i = 0UL; i < 100000UL; ++i) {
    hashes.push_back(i * 2UL);
  }
  const TKeyFilter filter(hashes);
  for (uint64_t hash : hashes) {
    EXPECT_TRUE(filter.MayContain(hash));
  }
  size_t false_positives = 0UL;
  for (uint64_t i = 0UL; i < 100000UL; ++i) {
    if (filter.MayContain(i * 2UL + 1UL)) {
      ++false_positives;
    }
  }
  EXPECT_LT(false_positives, 2500UL);
}

/* An empty filter holds nothing. */
FIXTURE(EmptyFilter) {
  const TKeyFilter none;
  EXPECT_FALSE(none.MayContain(0UL));
  EXPECT_FALSE(none.MayContain(12345UL));
  EXPECT_EQ(none.GetNumBytes(), 0UL);
  const TKeyFilter empty((vector<uint64_t>()));
  EXPECT_FALSE(empty.MayContain(0UL));
}

/* A file's filter: what it may rule out, and what it must let through. */
FIXTURE(FileKeyFilter) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const TUuid pair_idx(TUuid::Twister), single_idx(TUuid::Twister), missing_idx(TUuid::Twister), unfiltered_idx(TUuid::Twister);
  const size_t files_before = TFileKeyFilter::GetTotalFiles();
  const size_t bytes_before = TFileKeyFilter::GetTotalBytes();
  {
    vector<TFileKeyFilter::TIndex> indexes;
    /* (a, b) for a in [0, 100), b in [0, 10) */ {
      TKeyFilterBuilder builder;
      for (int64_t a = 0L; a < 100L; ++a) {
        for (int64_t b = 0L; b < 10L; ++b) {
          builder.Push(TKey(make_tuple(a, b), &arena, state_alloc).GetHash());
        }
      }
      indexes.push_back(builder.Make(pair_idx, 2UL));
      EXPECT_TRUE(indexes.back().HasFilter);
    }
    /* (a) for a in [0, 1000) */ {
      TKeyFilterBuilder builder;
      for (int64_t a = 0L; a < 1000L; ++a) {
        builder.Push(TKey(make_tuple(a), &arena, state_alloc).GetHash());
      }
      indexes.push_back(builder.Make(single_idx, 1UL));
    }
    /* An index with no filter. */
    indexes.push_back(TFileKeyFilter::TIndex{unfiltered_idx, 1UL, false, TKeyFilter()});
    const TFileKeyFilter filter(std::move(indexes));
    EXPECT_GT(filter.GetNumBytes(), 0UL);
    EXPECT_EQ(TFileKeyFilter::GetTotalFiles(), files_before + 1UL);
    EXPECT_EQ(TFileKeyFilter::GetTotalBytes(), bytes_before + filter.GetNumBytes());
    auto may_contain = [&](const TUuid &idx, const TKey &key) {
      return filter.MayContain(TFileKeyFilter::TProbe(TIndexKey(idx, key)));
    };
    /* Every key held passes. */
    for (int64_t a = 0L; a < 100L; ++a) {
      for (int64_t b = 0L; b < 10L; ++b) {
        EXPECT_TRUE(may_contain(pair_idx, TKey(make_tuple(a, b), &arena, state_alloc)));
      }
    }
    for (int64_t a = 0L; a < 1000L; ++a) {
      EXPECT_TRUE(may_contain(single_idx, TKey(make_tuple(a), &arena, state_alloc)));
    }
    /* Most keys not held are ruled out. */
    size_t passed = 0UL;
    for (int64_t a = 1000L; a < 11000L; ++a) {
      if (may_contain(single_idx, TKey(make_tuple(a), &arena, state_alloc))) {
        ++passed;
      }
      if (may_contain(pair_idx, TKey(make_tuple(a, a), &arena, state_alloc))) {
        ++passed;
      }
    }
    EXPECT_LT(passed, 500UL);
    /* An index the file doesn't have holds nothing. */
    EXPECT_FALSE(may_contain(missing_idx, TKey(make_tuple(1L), &arena, state_alloc)));
    EXPECT_FALSE(may_contain(missing_idx, TKey(make_tuple(1L, Native::TFree<int64_t>()), &arena, state_alloc)));
    /* An index without a filter may hold anything. */
    EXPECT_TRUE(may_contain(unfiltered_idx, TKey(make_tuple(123456L), &arena, state_alloc)));
    /* A key with a free member is looked up by its prefix, so it passes. */
    EXPECT_TRUE(may_contain(pair_idx, TKey(make_tuple(5000L, Native::TFree<int64_t>()), &arena, state_alloc)));
    /* So does a key whose shape isn't the index's. */
    EXPECT_TRUE(may_contain(pair_idx, TKey(make_tuple(5000L), &arena, state_alloc)));
    EXPECT_TRUE(may_contain(single_idx, TKey(make_tuple(5000L, 5000L), &arena, state_alloc)));
    /* And a key that isn't a tuple. */
    EXPECT_TRUE(may_contain(single_idx, TKey(5000L, &arena, state_alloc)));
  }
  /* The filter's bytes go with it. */
  EXPECT_EQ(TFileKeyFilter::GetTotalFiles(), files_before);
  EXPECT_EQ(TFileKeyFilter::GetTotalBytes(), bytes_before);
}

/* A builder starts afresh for each index. */
FIXTURE(BuilderStartsAfresh) {
  TKeyFilterBuilder builder;
  for (uint64_t i = 0UL; i < 1000UL; ++i) {
    builder.Push(i);
  }
  const auto first = builder.Make(TUuid(TUuid::Twister), 1UL);
  EXPECT_TRUE(first.HasFilter);
  EXPECT_TRUE(first.Filter.MayContain(999UL));
  builder.Push(7UL);
  const auto second = builder.Make(TUuid(TUuid::Twister), 1UL);
  EXPECT_TRUE(second.HasFilter);
  EXPECT_TRUE(second.Filter.MayContain(7UL));
  EXPECT_LT(second.Filter.GetNumBytes(), first.Filter.GetNumBytes());
  /* An index with no keys gets an empty filter, which holds nothing. */
  const auto empty = builder.Make(TUuid(TUuid::Twister), 1UL);
  EXPECT_TRUE(empty.HasFilter);
  EXPECT_FALSE(empty.Filter.MayContain(7UL));
}
