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
