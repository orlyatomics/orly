/* <orly/indy/util/min_heap.test.cc>

   Unit test for <orly/indy/util/min_heap.h>.

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

#include <orly/indy/util/min_heap.h>

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

#include <orly/atom/suprena.h>
#include <orly/indy/key.h>
#include <orly/sabot/type_dumper.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Util;

class TMyObj {
  public:

  TMyObj(size_t key, size_t val) : Key(key), Val(val) {}

  bool operator <(const TMyObj &that) const {
    return Key < that.Key;
  }

  bool operator >(const TMyObj &that) const {
    return Key > that.Key;
  }

  inline size_t GetKey() const {
    return Key;
  }

  inline size_t GetVal() const {
    return Val;
  }

  //private:

  size_t Key;
  size_t Val;

};

FIXTURE(Typical) {
  const size_t max_elems = 10;
  std::vector<TMyObj *> data;
  TMinHeap<TMyObj, size_t> min_heap(max_elems);
  for(size_t i = 0; i < max_elems; ++i) {
    data.push_back(new TMyObj(i * 10, i));
    min_heap.Insert(*data.back(), i);
  }
  size_t num_seen = 0UL;
  while (min_heap) {
    size_t pos;
    const TMyObj &min_val = min_heap.Pop(pos);
    EXPECT_EQ(pos, num_seen);
    EXPECT_EQ(min_val.GetKey(), num_seen * 10);
    EXPECT_EQ(min_val.GetVal(), num_seen);
    ++num_seen;
  }
  for (auto iter : data) {
    delete iter;
  }
}


FIXTURE(RevTypical) {
  const size_t max_elems = 10;
  std::vector<TMyObj *> data;
  TMinHeap<TMyObj, size_t> min_heap(max_elems);
  for(size_t i = 0; i < max_elems; ++i) {
    data.push_back(new TMyObj((max_elems - i - 1) * 10, i));
    min_heap.Insert(*data.back(), i);
  }
  size_t num_seen = 0UL;
  while (min_heap) {
    size_t pos;
    const TMyObj &min_val = min_heap.Pop(pos);
    EXPECT_EQ(pos, max_elems - num_seen - 1);
    EXPECT_EQ(min_val.GetKey(), num_seen * 10);
    EXPECT_EQ(min_val.GetVal(), max_elems - num_seen - 1);
    ++num_seen;
  }
  for (auto iter : data) {
    delete iter;
  }
}

FIXTURE(ReplaceTop) {
  /* Each source holds a run of values; ReplaceTop advances the source on top, Pop retires it. The merge
     must match a plain sort, including when one source supplies several rows in a row. */
  const std::vector<std::vector<size_t>> sources = {{1, 2, 3, 50}, {10, 20, 30}, {5, 6, 7, 8, 9, 100}, {}};
  std::vector<TMyObj> cur;
  std::vector<size_t> next(sources.size(), 0UL);
  cur.reserve(sources.size());
  std::vector<size_t> expected;
  for (size_t i = 0; i < sources.size(); ++i) {
    cur.emplace_back(sources[i].empty() ? 0 : sources[i][0], i);
    expected.insert(expected.end(), sources[i].begin(), sources[i].end());
  }
  std::sort(expected.begin(), expected.end());
  TMinHeap<TMyObj, size_t> min_heap(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) {
    if (!sources[i].empty()) {
      min_heap.Insert(cur[i], i);
    }
  }
  std::vector<size_t> seen;
  while (min_heap) {
    size_t pos;
    seen.push_back(min_heap.Peek(pos).GetKey());
    if (++next[pos] < sources[pos].size()) {
      cur[pos].Key = sources[pos][next[pos]];
      min_heap.ReplaceTop(cur[pos]);
    } else {
      min_heap.Pop(pos);
    }
  }
  EXPECT_TRUE(seen == expected);
}

/* #1018: ReplaceTop remembers which of the root's children comes first; over many sources with runs, ties and sources running
   dry, the merge still matches a plain sort by (key, source). */
FIXTURE(ReplaceTopMatchesASortOverManySources) {
  struct TBySource {
    bool operator()(const TMyObj &lhs, size_t lhs_ref, const TMyObj &rhs, size_t rhs_ref) const {
      return lhs.Key < rhs.Key || (lhs.Key == rhs.Key && lhs_ref < rhs_ref);
    }
  };
  std::mt19937_64 rng(1018);
  for (int round = 0; round < 200; ++round) {
    const size_t num_sources = 1UL + rng() % 9UL;
    std::vector<std::vector<size_t>> sources(num_sources);
    std::vector<std::pair<size_t, size_t>> expected;
    for (size_t i = 0; i < num_sources; ++i) {
      const size_t len = rng() % 40UL;
      for (size_t j = 0; j < len; ++j) {
        /* Long runs of one source, and keys several sources share. */
        sources[i].push_back(rng() % 3UL ? rng() % 20UL : rng() % 400UL);
      }
      std::sort(sources[i].begin(), sources[i].end());
      for (size_t key : sources[i]) {
        expected.emplace_back(key, i);
      }
    }
    std::sort(expected.begin(), expected.end());
    std::vector<TMyObj> cur;
    cur.reserve(num_sources);
    std::vector<size_t> next(num_sources, 0UL);
    TMinHeap<TMyObj, size_t, TBySource> min_heap(num_sources);
    for (size_t i = 0; i < num_sources; ++i) {
      cur.emplace_back(sources[i].empty() ? 0 : sources[i][0], i);
    }
    for (size_t i = 0; i < num_sources; ++i) {
      if (!sources[i].empty()) {
        min_heap.Insert(cur[i], i);
      }
    }
    std::vector<std::pair<size_t, size_t>> seen;
    while (min_heap) {
      size_t pos;
      seen.emplace_back(min_heap.Peek(pos).GetKey(), pos);
      if (++next[pos] < sources[pos].size()) {
        cur[pos].Key = sources[pos][next[pos]];
        min_heap.ReplaceTop(cur[pos]);
      } else {
        min_heap.Pop(pos);
      }
      EXPECT_EQ(min_heap.GetSize(), static_cast<size_t>(std::count_if(next.begin(), next.end(), [&](const size_t &n) {
        return n < sources[&n - next.data()].size();
      })));
    }
    EXPECT_TRUE(seen == expected);
  }
}
