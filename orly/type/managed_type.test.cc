/* <orly/type/managed_type.test.cc>

   Unit test for <orly/type/managed_type.h>: the interner's hit and miss paths,
   growth of the map while a type constructs, and a timed hit-path loop.

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

#include <orly/type.h>

#include <orly/type/type_czar.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly;
using namespace Orly::Type;

/* N distinct object types, held alive by the caller. */
static vector<TType> MakeObjs(size_t n) {
  vector<TType> objs;
  objs.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    objs.push_back(TObj::Get(map<string, TType>{{"f" + to_string(i), TInt::Get()}}));
  }
  return objs;
}

FIXTURE(HitReturnsSameImpl) {
  TTypeCzar type_czar;
  TType a = TList::Get(TInt::Get());
  TType b = TList::Get(TInt::Get());
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(TList::Get(TBool::Get()) == a);
}

/* Interning many distinct keys grows (and so rehashes) the map; every earlier entry must still be
   found, and must still be the same impl. Nested types intern their element types inside the
   outer Get, so this also exercises growth while a Get is in flight. */
FIXTURE(GrowthKeepsEntries) {
  TTypeCzar type_czar;
  const size_t n = 5000;
  vector<TType> objs = MakeObjs(n);
  vector<TType> lists;
  lists.reserve(n);
  for (const TType &o : objs) {
    lists.push_back(TList::Get(o));
  }
  for (size_t i = 0; i < n; ++i) {
    EXPECT_TRUE(TList::Get(objs[i]) == lists[i]);
  }
  /* Deep nesting: each level's constructor reaches the next family member. */
  TType deep = TInt::Get();
  for (int i = 0; i < 200; ++i) {
    deep = TList::Get(TOpt::Get(deep));
  }
  TType deep2 = TInt::Get();
  for (int i = 0; i < 200; ++i) {
    deep2 = TList::Get(TOpt::Get(deep2));
  }
  EXPECT_TRUE(deep == deep2);
}

/* A freed type leaves the map; interning the same key again makes a fresh, working one. */
FIXTURE(FreeThenReintern) {
  TTypeCzar type_czar;
  for (int round = 0; round < 3; ++round) {
    vector<TType> objs = MakeObjs(1000);
    {
      vector<TType> lists;
      for (const TType &o : objs) {
        lists.push_back(TList::Get(o));
      }
    }
    for (const TType &o : objs) {
      EXPECT_TRUE(TList::Get(o) == TList::Get(o));
    }
  }
}

/* Timed hit path: not an assertion, only printed. ORLY_TYPE_BENCH_RUNS (default 1) repeats it and
   ORLY_TYPE_BENCH_N (default 1000) sets how many distinct keys the lookups cycle through. */
FIXTURE(TimedHitPath) {
  TTypeCzar type_czar;
  const char *n_env = getenv("ORLY_TYPE_BENCH_N");
  const size_t n = n_env ? static_cast<size_t>(atoll(n_env)) : 1000;
  const size_t passes = 2000000 / n;
  vector<TType> objs = MakeObjs(n);
  vector<TType> lists;
  for (const TType &o : objs) {
    lists.push_back(TList::Get(o));
  }
  /* Visit the keys in a shuffled order: the impls are allocated in sequence, so their pointer-derived
     hashes are too, and walking them in allocation order would flatter any table that keeps
     neighbouring hashes in neighbouring slots. */
  vector<size_t> order(n);
  iota(order.begin(), order.end(), 0);
  shuffle(order.begin(), order.end(), mt19937_64(1));
  const char *env = getenv("ORLY_TYPE_BENCH_RUNS");
  int runs = env ? atoi(env) : 1;
  for (int run = 0; run < runs; ++run) {
    auto start = chrono::steady_clock::now();
    size_t sum = 0;
    for (size_t p = 0; p < passes; ++p) {
      for (size_t i = 0; i < n; ++i) {
        sum += TList::Get(objs[order[i]]).GetHash();
      }
    }
    auto ns = chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now() - start).count();
    printf("type-intern hit: %.1f ns/lookup (%zu lookups, sum %zu)\n",
           static_cast<double>(ns) / static_cast<double>(n * passes), n * passes, sum & 1);
  }
}
