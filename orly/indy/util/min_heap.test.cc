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

/* Orders by value, then by ref, so equal values come out in ref order. */
struct TByValThenRef {
  bool operator()(const TMyObj &lhs, size_t lhs_ref, const TMyObj &rhs, size_t rhs_ref) const {
    return lhs.Key != rhs.Key ? lhs.Key < rhs.Key : lhs_ref < rhs_ref;
  }
};

FIXTURE(MatchesSort) {
  /* Random inserts, pops and replace-tops over a small key range, so ties are common; every pop must
     give the smallest (key, ref) left, as a sorted list of the same pairs does. */
  const size_t max_elems = 37;
  std::vector<TMyObj> vals;
  vals.reserve(max_elems);
  for (size_t i = 0; i < max_elems; ++i) {
    vals.emplace_back(0, i);
  }
  uint64_t seed = 0x9e3779b97f4a7c15ULL;
  auto rand = [&seed](size_t n) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return static_cast<size_t>(seed % n);
  };
  TMinHeap<TMyObj, size_t, TByValThenRef> min_heap(max_elems);
  std::vector<std::pair<size_t, size_t>> model;
  std::vector<size_t> free_refs;
  for (size_t i = 0; i < max_elems; ++i) {
    free_refs.push_back(i);
  }
  bool ok = true;
  for (int step = 0; step < 20000 && ok; ++step) {
    const size_t op = rand(3);
    if (op == 0 && !free_refs.empty()) {
      const size_t ref = free_refs.back();
      free_refs.pop_back();
      vals[ref].Key = rand(8);
      min_heap.Insert(vals[ref], ref);
      model.emplace_back(vals[ref].Key, ref);
    } else if (model.empty()) {
      continue;
    } else {
      std::sort(model.begin(), model.end());
      size_t ref;
      const TMyObj &top = min_heap.Peek(ref);
      ok = top.Key == model.front().first && ref == model.front().second;
      if (op == 1) {
        min_heap.Pop(ref);
        model.erase(model.begin());
        free_refs.push_back(ref);
      } else {
        vals[ref].Key = rand(8);
        min_heap.ReplaceTop(vals[ref]);
        model.front().first = vals[ref].Key;
      }
    }
  }
  EXPECT_TRUE(ok);
  std::sort(model.begin(), model.end());
  for (const auto &[key, ref]: model) {
    size_t pos;
    const TMyObj &min_val = min_heap.Pop(pos);
    EXPECT_EQ(min_val.Key, key);
    EXPECT_EQ(pos, ref);
  }
  EXPECT_FALSE(min_heap);
}
