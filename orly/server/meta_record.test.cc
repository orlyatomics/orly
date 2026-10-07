/* <orly/server/meta_record.test.cc>

   Unit test for <orly/server/meta_record.h>: a batch's recorded calls read back as the calls
   that ran (#751), through the same serialization Tetris reads them from.

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

#include <orly/server/meta_record.h>

#include <stdexcept>

#include <orly/atom/suprena.h>
#include <orly/indy/key.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Server;

using TEntry = TMetaRecord::TEntry;
using TCall = TEntry::TCall;

Type::TTypeCzar TypeCzar;

static const TEntry::TPackageFqName Pkg{"batch_promotion"};

/* An entry as RunBatch writes it, round-tripped through the update metadata Tetris parses. */
static TEntry Reread(const TEntry::TPackageFqName &pkg, const string &method, TEntry::TArgByName &&args) {
  TUuid update_id(TUuid::Twister);
  TMetaRecord written(update_id, TEntry(
      TUuid(TUuid::Twister), nullopt, pkg, method, std::move(args), TEntry::TExpectedPredicateResults{1, 0},
      Chrono::Now(), 7));
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Indy::TKey key(written, &arena, state_alloc);
  TMetaRecord read;
  Sabot::ToNative(*Sabot::State::TAny::TWrapper(key.GetCore().NewState(&arena, state_alloc)), read);
  return read.GetEntry(update_id);
}

static bool SameArgs(const TEntry::TArgByName &lhs, const TEntry::TArgByName &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto l = lhs.begin(), r = rhs.begin(); l != lhs.end(); ++l, ++r) {
    if (l->first != r->first || l->second != r->second) {
      return false;
    }
  }
  return true;
}

static bool SameCalls(const vector<TCall> &lhs, const vector<TCall> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].PackageFqName != rhs[i].PackageFqName || lhs[i].MethodName != rhs[i].MethodName ||
        !SameArgs(lhs[i].ArgByName, rhs[i].ArgByName)) {
      return false;
    }
  }
  return true;
}

/* An unbatched call records its args by their own names: one call. */
FIXTURE(SingleCall) {
  TEntry::TArgByName args{{"k", Var::TVar(string("a"))}, {"x", Var::TVar(int64_t(9))}};
  auto entry = Reread(Pkg, "put_ci", TEntry::TArgByName(args));
  vector<TCall> want{TCall{Pkg, "put_ci", args}};
  EXPECT_TRUE(SameCalls(entry.GetCalls(), want));
}

/* A same-method batch, including a batch of one: each call gets its own args back. Before #751,
   Tetris replayed this entry as one call whose args were named "0.k", "1.k", ... */
FIXTURE(SameMethodBatch) {
  for (size_t size: {1, 2, 8}) {
    vector<TCall> calls;
    for (size_t i = 0; i < size; ++i) {
      calls.push_back(TCall{Pkg, "put_cond", {{"k", Var::TVar("k" + to_string(i))}, {"v", Var::TVar(string("v"))}}});
    }
    auto args = TEntry::EncodeBatch(calls);
    EXPECT_TRUE(args.find("0.$method") == args.end());
    auto entry = Reread(Pkg, "put_cond", std::move(args));
    EXPECT_TRUE(SameCalls(entry.GetCalls(), calls));
  }
}

/* A mixed batch (#255): each call's own package (a multi-part name too) and method. */
FIXTURE(MixedBatch) {
  TEntry::TPackageFqName other{"some", "where", "else"};
  vector<TCall> calls{
    TCall{Pkg, "guard", {{"k", Var::TVar(string("g"))}}},
    TCall{other, "put_ci", {{"k", Var::TVar(string("c"))}, {"x", Var::TVar(int64_t(3))}}},
    TCall{Pkg, "tick", {}},
    TCall{Pkg, "guard", {{"k", Var::TVar(string("h"))}}},
  };
  auto entry = Reread(Pkg, "guard", TEntry::EncodeBatch(calls));
  EXPECT_TRUE(SameCalls(entry.GetCalls(), calls));
}

/* A batch of calls without args still records how many calls there were. */
FIXTURE(ArglessBatch) {
  vector<TCall> calls{TCall{Pkg, "tick", {}}, TCall{Pkg, "tick", {}}, TCall{Pkg, "tick", {}}};
  auto entry = Reread(Pkg, "tick", TEntry::EncodeBatch(calls));
  EXPECT_EQ(entry.GetCalls().size(), 3UL);
  EXPECT_TRUE(SameCalls(entry.GetCalls(), calls));
}

/* A batch recorded by v0.2.0, without "$calls", is read by its index prefixes. */
FIXTURE(V020Batch) {
  TEntry::TArgByName args{
    {"0.k", Var::TVar(string("a"))}, {"0.x", Var::TVar(int64_t(1))},
    {"1.k", Var::TVar(string("b"))}, {"1.x", Var::TVar(int64_t(9))},
  };
  auto entry = Reread(Pkg, "put_ci", std::move(args));
  vector<TCall> want{
    TCall{Pkg, "put_ci", {{"k", Var::TVar(string("a"))}, {"x", Var::TVar(int64_t(1))}}},
    TCall{Pkg, "put_ci", {{"k", Var::TVar(string("b"))}, {"x", Var::TVar(int64_t(9))}}},
  };
  EXPECT_TRUE(SameCalls(entry.GetCalls(), want));
}

/* A malformed batch record throws instead of replaying the wrong calls. */
FIXTURE(Malformed) {
  auto past_count = Reread(Pkg, "put_ci", TEntry::TArgByName{
      {"$calls", Var::TVar(int64_t(1))}, {"0.k", Var::TVar(string("a"))}, {"3.k", Var::TVar(string("b"))}});
  auto read_past_count = [&] { past_count.GetCalls(); };
  EXPECT_THROW_FUNC(runtime_error, read_past_count);
  auto unindexed = Reread(Pkg, "put_ci", TEntry::TArgByName{
      {"$calls", Var::TVar(int64_t(1))}, {"0.k", Var::TVar(string("a"))}, {"k", Var::TVar(string("b"))}});
  auto read_unindexed = [&] { unindexed.GetCalls(); };
  EXPECT_THROW_FUNC(runtime_error, read_unindexed);
  auto bad_count = Reread(Pkg, "put_ci", TEntry::TArgByName{{"$calls", Var::TVar(string("two"))}});
  auto read_bad_count = [&] { bad_count.GetCalls(); };
  EXPECT_THROW_FUNC(runtime_error, read_bad_count);
}
