/* <orly/indy/fork_watch.test.cc>

   Unit test for <orly/indy/fork_watch.h> (#746).

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

#include <orly/indy/fork_watch.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <base/test/kit.h>
#include <orly/indy/update.h>
#include <orly/native/defs.h>
#include <orly/sabot/to_native.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Indy;

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 100000UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 400000UL);

namespace {

  const TUuid Index("00000000-0000-0000-0000-0000000000a1");

  /* One write of an update: key <[name]>, and a value, a delete, or `+= n`. */
  struct TWrite {
    string Name;
    enum { Put, Delete, Add } Kind;
    int64_t Val = 0;
  };

  shared_ptr<TUpdate> NewUpdate(const vector<TWrite> &writes) {
    Atom::TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TUpdate::TOpByKey op_by_key;
    for (const auto &write: writes) {
      if (write.Kind == TWrite::Put) {
        op_by_key.emplace(TIndexKey(Index, TKey(make_tuple(write.Name), &arena, state_alloc)), TKey(write.Val, &arena, state_alloc));
      } else if (write.Kind == TWrite::Delete) {
        op_by_key.emplace(TIndexKey(Index, TKey(make_tuple(write.Name), &arena, state_alloc)),
                          TKey(Native::TTombstone::Tombstone, &arena, state_alloc));
      }
    }
    auto update = TUpdate::NewUpdate(op_by_key, TKey(&arena), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
    for (const auto &write: writes) {
      if (write.Kind == TWrite::Add) {
        update->AddEntry(TIndexKey(Index, TKey(make_tuple(write.Name), &arena, state_alloc)), TKey(write.Val, &arena, state_alloc), TMutator::Add);
      }
    }
    return update;
  }

  TIndexKey Key(const string &name, Atom::TSuprena &arena) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    return TIndexKey(Index, TKey(make_tuple(name), &arena, state_alloc));
  }

  /* The keys of a conflict list, as names. */
  vector<string> Names(const vector<TForkWatch::TConflict> &conflicts) {
    vector<string> result;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    for (const auto &conflict: conflicts) {
      tuple<string> key;
      Sabot::ToNative(*Sabot::State::TAny::TWrapper(conflict.Key.GetKey().GetState(state_alloc)), key);
      result.push_back(get<0>(key) + (conflict.IsDelete ? "-" : "") + (conflict.Raced ? "!" : ""));
    }
    return result;
  }

  vector<TForkWatch::TConflict> Conflicts(const TForkWatch &watch, uint64_t after = 0UL) {
    vector<TForkWatch::TConflict> result;
    watch.ForEachConflict(after, [&result](const TForkWatch::TConflict &conflict) { result.push_back(conflict); });
    return result;
  }

  /* The POV, its parent and its grandparent; and a sibling of the POV. */
  const TUuid Pov("00000000-0000-0000-0000-000000000001"), Parent("00000000-0000-0000-0000-000000000002"),
              Grandparent("00000000-0000-0000-0000-000000000003"), Sibling("00000000-0000-0000-0000-000000000004");

}  // namespace

FIXTURE(ChangesAndConflicts) {
  TForkWatch watch(Pov, {Parent, Grandparent}, TForkWatch::TMode::Report);
  /* Direct writes into the chain, and a sibling's promotion, are changes; content moving up the
     chain is not. */
  watch.OnAppend(*NewUpdate({{"a", TWrite::Put, 1}}), TUuid());
  watch.OnAppend(*NewUpdate({{"b", TWrite::Delete}}), Sibling);
  watch.OnAppend(*NewUpdate({{"c", TWrite::Add, 1}}), TUuid());
  watch.OnAppend(*NewUpdate({{"d", TWrite::Put, 1}}), Parent);
  EXPECT_EQ(watch.GetState().ChangedKeys, 3UL);
  Atom::TSuprena arena;
  EXPECT_TRUE(watch.IsChanged(Key("a", arena)));
  EXPECT_TRUE(watch.IsChanged(Key("b", arena)));
  EXPECT_TRUE(watch.IsChanged(Key("c", arena)));
  EXPECT_FALSE(watch.IsChanged(Key("d", arena)));
  /* The POV's overwrites and deletes of changed keys conflict; its `+=` doesn't, nor does a write
     of a key the chain didn't change. */
  auto update = NewUpdate({{"a", TWrite::Add, 5}, {"b", TWrite::Put, 2}, {"c", TWrite::Delete}, {"d", TWrite::Put, 3}, {"e", TWrite::Put, 4}});
  EXPECT_TRUE(Names(watch.FindConflicts(*update)) == vector<string>({"b", "c-"}));
  /* Its arrival in the parent reports them, numbered, and its versions now stand. */
  watch.OnAppend(*update, Pov);
  EXPECT_TRUE(Names(Conflicts(watch)) == vector<string>({"b", "c-"}));
  EXPECT_EQ(Conflicts(watch).front().Number, 1UL);
  EXPECT_EQ(Conflicts(watch).back().Number, 2UL);
  EXPECT_EQ(watch.GetState().ConflictCount, 2UL);
  EXPECT_FALSE(watch.IsChanged(Key("b", arena)));
  EXPECT_FALSE(watch.IsChanged(Key("c", arena)));
  EXPECT_TRUE(watch.IsChanged(Key("a", arena)));
  watch.OnAppend(*NewUpdate({{"b", TWrite::Put, 9}}), Pov);
  EXPECT_EQ(watch.GetState().ConflictCount, 2UL);
  /* The parent changes b again: the POV's next overwrite of it conflicts again. */
  watch.OnAppend(*NewUpdate({{"b", TWrite::Put, 10}}), Sibling);
  watch.OnAppend(*NewUpdate({{"b", TWrite::Put, 11}}), Pov);
  EXPECT_TRUE(Names(Conflicts(watch, 2UL)) == vector<string>({"b"}));
  /* Report mode never blocks, and its conflicts are never raced. */
  EXPECT_FALSE(watch.GetState().Blocked);
}

FIXTURE(RefuseBlockForce) {
  TForkWatch watch(Pov, {Parent}, TForkWatch::TMode::Refuse);
  watch.OnAppend(*NewUpdate({{"a", TWrite::Put, 1}}), TUuid());
  auto clean = NewUpdate({{"b", TWrite::Put, 1}, {"a", TWrite::Add, 1}});
  auto dirty = NewUpdate({{"a", TWrite::Put, 2}});
  EXPECT_FALSE(watch.ShouldBlock(*clean, 1));
  EXPECT_FALSE(watch.GetState().Blocked);
  EXPECT_TRUE(watch.ShouldBlock(*dirty, 2));
  EXPECT_TRUE(watch.ShouldBlock(*dirty, 2));
  EXPECT_TRUE(watch.GetState().Blocked);
  vector<TForkWatch::TConflict> blocking;
  watch.ForEachBlockingKey([&blocking](const TForkWatch::TConflict &conflict) { blocking.push_back(conflict); });
  EXPECT_TRUE(Names(blocking) == vector<string>({"a"}));
  /* Forced up to 2, the update goes through, and its conflict on arrival is expected, not raced. */
  watch.Force(2);
  EXPECT_FALSE(watch.GetState().Blocked);
  EXPECT_FALSE(watch.ShouldBlock(*dirty, 2));
  watch.OnAppend(*dirty, Pov);
  EXPECT_TRUE(Names(Conflicts(watch)) == vector<string>({"a"}));
  /* A change that lands between Tetris's test and the arrival is reported as raced. */
  auto late = NewUpdate({{"c", TWrite::Put, 1}});
  EXPECT_FALSE(watch.ShouldBlock(*late, 3));
  watch.OnAppend(*NewUpdate({{"c", TWrite::Put, 7}}), Sibling);
  watch.OnAppend(*late, Pov);
  EXPECT_TRUE(Names(Conflicts(watch, 1UL)) == vector<string>({"c!"}));
  /* Past the force, a conflict blocks again. */
  watch.OnAppend(*NewUpdate({{"d", TWrite::Put, 1}}), TUuid());
  EXPECT_TRUE(watch.ShouldBlock(*NewUpdate({{"d", TWrite::Delete}}), 4));
}

FIXTURE(Refork) {
  TForkWatch watch(Pov, {Parent}, TForkWatch::TMode::Refuse);
  watch.OnAppend(*NewUpdate({{"a", TWrite::Put, 1}, {"b", TWrite::Put, 1}}), TUuid());
  auto update = NewUpdate({{"a", TWrite::Put, 2}});
  EXPECT_TRUE(watch.ShouldBlock(*update, 1));
  watch.OnAppend(*NewUpdate({{"b", TWrite::Put, 2}}), Pov);
  watch.Refork();
  /* A discard re-forks: what the chain changed before is the POV's starting point. */
  const auto state = watch.GetState();
  EXPECT_FALSE(state.Blocked);
  EXPECT_EQ(state.ChangedKeys, 0UL);
  EXPECT_EQ(state.ConflictCount, 1UL);
  EXPECT_FALSE(watch.ShouldBlock(*update, 2));
  /* The conflicts kept survive the arena the re-fork replaced.  (Raced: no test let it through.) */
  EXPECT_TRUE(Names(Conflicts(watch)) == vector<string>({"b!"}));
}

FIXTURE(Overflow) {
  TForkWatch watch(Pov, {Parent}, TForkWatch::TMode::Report, /* max_keys */ 2UL);
  watch.OnAppend(*NewUpdate({{"a", TWrite::Put, 1}, {"b", TWrite::Put, 1}}), TUuid());
  EXPECT_FALSE(watch.GetState().Overflowed);
  watch.OnAppend(*NewUpdate({{"c", TWrite::Put, 1}}), TUuid());
  EXPECT_TRUE(watch.GetState().Overflowed);
  /* It can no longer tell, so every overwrite or delete conflicts; `+=` still never does. */
  EXPECT_TRUE(Names(watch.FindConflicts(*NewUpdate({{"z", TWrite::Put, 1}, {"y", TWrite::Add, 1}}))) == vector<string>({"z"}));
}

/* Chain repos append on many threads while Tetris tests and the review reads; for TSan. */
FIXTURE(Concurrent) {
  TForkWatch watch(Pov, {Parent, Grandparent}, TForkWatch::TMode::Refuse, /* max_keys */ 5000UL);
  atomic<bool> stop(false);
  vector<thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&watch, t] {
      for (int i = 0; i < 2000; ++i) {
        const TUuid from = (i % 3 == 0) ? Sibling : (i % 3 == 1) ? Parent : TUuid();
        watch.OnAppend(*NewUpdate({{to_string(t * 10000 + i), TWrite::Put, i}}), from);
      }
    });
  }
  threads.emplace_back([&watch, &stop] {
    for (TSequenceNumber seq = 1; !stop; ++seq) {
      auto update = NewUpdate({{to_string(seq % 2000), TWrite::Put, 1}});
      if (watch.ShouldBlock(*update, seq) && seq % 7 == 0) {
        watch.Force(seq);
      }
      watch.OnAppend(*update, Pov);
    }
  });
  threads.emplace_back([&watch, &stop] {
    while (!stop) {
      watch.GetState();
      watch.ForEachConflict(0UL, [](const TForkWatch::TConflict &) {});
      watch.ForEachBlockingKey([](const TForkWatch::TConflict &) {});
    }
  });
  for (int t = 0; t < 4; ++t) {
    threads[t].join();
  }
  stop = true;
  for (size_t t = 4; t < threads.size(); ++t) {
    threads[t].join();
  }
  const auto state = watch.GetState();
  EXPECT_TRUE(state.ChangedKeys <= 5000UL);
  EXPECT_TRUE(state.ConflictCount > 0UL);
}
