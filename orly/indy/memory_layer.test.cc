/* <orly/indy/memory_layer.test.cc>

   Unit test for <orly/indy/memory_layer.h>.

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

#include <orly/indy/memory_layer.h>

#include <orly/indy/update.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <random>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <base/test/kit.h>
#include <orly/desc.h>
#include <orly/sabot/match_prefix_state.h>
#include <orly/sabot/to_native.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;


Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping");
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry");
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer");

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 4000004UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 4000004UL);

template <typename ...TArgs>
void Insert(TMemoryLayer &mem_layer, TSequenceNumber seq_num, const Base::TUuid &idx_id, int64_t val, const TArgs &...args) {
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  std::shared_ptr<TUpdate> update(TUpdate::NewUpdate(TUpdate::TOpByKey{
    { TIndexKey(idx_id, TKey(std::make_tuple(args...), &arena, state_alloc)), TKey(val, &arena, state_alloc)}
    }, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc)));
  update->SetSequenceNumber(seq_num);
  mem_layer.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));
}

FIXTURE(Typical) {
  TMemoryLayer mem_layer(nullptr);
  Base::TUuid int_idx(Base::TUuid::Twister);
  TSuprena arena;
  TSequenceNumber seq_num = 0UL;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  /* insert data */ {
    for (int64_t i = 0; i < 11; i += 2) {
      Insert(mem_layer, ++seq_num, int_idx, static_cast<int64_t>(rand()),
               i);
    }
  }
  /* basic walk */ {
    for (int64_t i = 0; i < 11; i += 2) {
      auto walker_ptr = mem_layer.NewPresentWalker(TIndexKey(int_idx, TKey(make_tuple(i), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(walker)) {
        const TPresentWalker::TItem &item = *walker;
        EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(i), &arena, state_alloc));
      }
    }
  }
  /* non-exist walk */ {
    for (int64_t i = 1; i < 11; i += 2) {
      auto walker_ptr = mem_layer.NewPresentWalker(TIndexKey(int_idx, TKey(make_tuple(i), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      EXPECT_FALSE(walker);
    }
  }
}

FIXTURE(Free) {
  TMemoryLayer mem_layer(nullptr);
  Base::TUuid int_int_idx(Base::TUuid::Twister);
  TSuprena arena;
  TSequenceNumber seq_num = 0UL;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  /* insert data */ {
      Insert(mem_layer, ++seq_num, int_int_idx, static_cast<int64_t>(rand()),   1L, 7L);
      Insert(mem_layer, ++seq_num, int_int_idx, static_cast<int64_t>(rand()),   1L, 14L);
      Insert(mem_layer, ++seq_num, int_int_idx, static_cast<int64_t>(rand()),   3L, 21L);
      Insert(mem_layer, ++seq_num, int_int_idx, static_cast<int64_t>(rand()),   3L, 28L);
      Insert(mem_layer, ++seq_num, int_int_idx, static_cast<int64_t>(rand()),   8L, 35L);
  }
  /* basic walk */ {
    /* <[1L , 7L]> */ {
      auto walker_ptr = mem_layer.NewPresentWalker(TIndexKey(int_int_idx, TKey(make_tuple(1L, 7L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(walker)) {
        const TPresentWalker::TItem &item = *walker;
        EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(1L, 7L), &arena, state_alloc));
      }
    }
    /* <[1L , 14L]> */ {
      auto walker_ptr = mem_layer.NewPresentWalker(TIndexKey(int_int_idx, TKey(make_tuple(1L, 14L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(walker)) {
        const TPresentWalker::TItem &item = *walker;
        EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(1L, 14L), &arena, state_alloc));
      }
    }
    /* <[3L , 21L]> */ {
      auto walker_ptr = mem_layer.NewPresentWalker(TIndexKey(int_int_idx, TKey(make_tuple(3L, 21L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(walker)) {
        const TPresentWalker::TItem &item = *walker;
        EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(3L, 21L), &arena, state_alloc));
      }
    }
  }
  /* free walk */ {
    /* <[1L , free(int64_t)]> */ {
      TIndexKey search_key(int_int_idx, TKey(make_tuple(1L, Native::TFree<int64_t>()), &arena, state_alloc));
      auto walker_ptr = mem_layer.NewPresentWalker(search_key);
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(walker)) {
        const TPresentWalker::TItem &item = *walker;
        EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(1L, 7L), &arena, state_alloc));
        ++walker;
        if (EXPECT_TRUE(walker)) {
          const TPresentWalker::TItem &item = *walker;
          EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(1L, 14L), &arena, state_alloc));
          ++walker;
          EXPECT_FALSE(walker);
        }
      }
    }
    /* <[3L , free(int64_t)]> */ {
      TIndexKey search_key(int_int_idx, TKey(make_tuple(3L, Native::TFree<int64_t>()), &arena, state_alloc));
      auto walker_ptr = mem_layer.NewPresentWalker(search_key);
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(walker)) {
        const TPresentWalker::TItem &item = *walker;
        EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(3L, 21L), &arena, state_alloc));
        ++walker;
        if (EXPECT_TRUE(walker)) {
          const TPresentWalker::TItem &item = *walker;
          EXPECT_EQ(TKey(item.Key, item.KeyArena), TKey(make_tuple(3L, 28L), &arena, state_alloc));
          ++walker;
          EXPECT_FALSE(walker);
        }
      }
    }
  }
}

/* Phase 1 of #49: TUpdate::TEntry carries a TMutator field. Verify that
   the field defaults to Assign for the existing TOpByKey-based API and
   round-trips when set explicitly via the new AddEntry overload. The
   deep-copy ctor (TUpdate::CopyUpdate) must also preserve it -- that's
   the path the merge engine uses. */
FIXTURE(MutatorRoundTrip) {
  Base::TUuid int_idx(Base::TUuid::Twister);
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());

  auto update = TUpdate::NewUpdate(
      TUpdate::TOpByKey{
        { TIndexKey(int_idx, TKey(std::make_tuple(int64_t(1)), &arena, state_alloc)),
          TKey(int64_t(10), &arena, state_alloc) }
      },
      TKey(&arena),
      TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));

  /* The TOpByKey path defaults every entry's mutator to Assign. */ {
    size_t seen = 0;
    for (TUpdate::TEntryCollection::TCursor csr(update->GetEntryCollection(), InvCon::TOrient::Fwd); csr; ++csr) {
      EXPECT_EQ(static_cast<int>(csr->GetMutator()), static_cast<int>(TMutator::Assign));
      ++seen;
    }
    EXPECT_EQ(seen, 1UL);
  }

  /* The new AddEntry(...,mutator) overload stores the mutator verbatim. */
  update->AddEntry(
      TIndexKey(int_idx, TKey(std::make_tuple(int64_t(2)), &arena, state_alloc)),
      TKey(int64_t(5), &arena, state_alloc),
      TMutator::Add);
  /* And so does the default-mutator overload (still Assign). */
  update->AddEntry(
      TIndexKey(int_idx, TKey(std::make_tuple(int64_t(3)), &arena, state_alloc)),
      TKey(int64_t(7), &arena, state_alloc));
  size_t assign_count = 0, add_count = 0;
  for (TUpdate::TEntryCollection::TCursor csr(update->GetEntryCollection(), InvCon::TOrient::Fwd); csr; ++csr) {
    if (csr->GetMutator() == TMutator::Add) ++add_count;
    else if (csr->GetMutator() == TMutator::Assign) ++assign_count;
  }
  EXPECT_EQ(add_count, 1UL);
  EXPECT_EQ(assign_count, 2UL);

  /* CopyUpdate (deep copy used by the merge engine) must preserve the mutator. */
  TUpdate *copy = TUpdate::CopyUpdate(update.get(), state_alloc);
  size_t copy_assign = 0, copy_add = 0;
  for (TUpdate::TEntryCollection::TCursor csr(copy->GetEntryCollection(), InvCon::TOrient::Fwd); csr; ++csr) {
    if (csr->GetMutator() == TMutator::Add) ++copy_add;
    else if (csr->GetMutator() == TMutator::Assign) ++copy_assign;
  }
  EXPECT_EQ(copy_add, 1UL);
  EXPECT_EQ(copy_assign, 2UL);
  delete copy;
}

/* #257: exact-point reads seek via the skip-list accelerator instead of
   head-scanning the EntryCollection. Verify the seeking walker
   (exact_point=true) agrees with the authoritative ordered list for present
   and absent keys across a range large enough to populate several express
   lanes, discriminates index ids, and still surfaces a key's full SeqNum run
   newest-first. */
FIXTURE(ExactPointSeek) {
  TMemoryLayer mem_layer(nullptr);
  Base::TUuid idx_a(Base::TUuid::Twister);
  Base::TUuid idx_b(Base::TUuid::Twister);
  TSuprena arena;
  TSequenceNumber seq_num = 0UL;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const int64_t N = 200;
  /* Even keys present under idx_a; key 4 also present under idx_b (same key
     value, different index id). */ {
    for (int64_t i = 0; i < N; i += 2) {
      Insert(mem_layer, ++seq_num, idx_a, i * 100, i);
    }
    Insert(mem_layer, ++seq_num, idx_b, 999, 4L);
  }
  /* Present keys: the exact seek finds each one with the right value. */ {
    for (int64_t i = 0; i < N; i += 2) {
      auto wp = mem_layer.NewPresentWalker(TIndexKey(idx_a, TKey(make_tuple(i), &arena, state_alloc)), /* exact_point */ true);
      auto &w = *wp;
      if (EXPECT_TRUE(w)) {
        EXPECT_EQ(TKey((*w).Key, (*w).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        EXPECT_EQ(TKey((*w).Op, (*w).OpArena), TKey(i * 100, &arena, state_alloc));
      }
    }
  }
  /* Absent keys (odd, and past the end): the exact seek reports nothing and
     must not fall through into a forward scan. */ {
    for (int64_t i = 1; i < N; i += 2) {
      auto wp = mem_layer.NewPresentWalker(TIndexKey(idx_a, TKey(make_tuple(i), &arena, state_alloc)), true);
      EXPECT_FALSE(*wp);
    }
    auto past = mem_layer.NewPresentWalker(TIndexKey(idx_a, TKey(make_tuple(N + 10), &arena, state_alloc)), true);
    EXPECT_FALSE(*past);
  }
  /* Index discrimination: idx_b's key 4 carries a distinct value. */ {
    auto wp = mem_layer.NewPresentWalker(TIndexKey(idx_b, TKey(make_tuple(4L), &arena, state_alloc)), true);
    auto &w = *wp;
    if (EXPECT_TRUE(w)) {
      EXPECT_EQ(TKey((*w).Op, (*w).OpArena), TKey(int64_t(999), &arena, state_alloc));
    }
  }
  /* A key with multiple versions: the seek must land on the highest-SeqNum
     entry of the run (the anchor the read-path fold starts from), exactly as
     the head-scan walker does -- the older run members are surfaced by the
     context-level fold, not the per-layer walker. */ {
    Insert(mem_layer, ++seq_num, idx_a, 4444, 4L);  // newer version of key 4
    auto wp = mem_layer.NewPresentWalker(TIndexKey(idx_a, TKey(make_tuple(4L), &arena, state_alloc)), /* exact_point */ true);
    auto &w = *wp;
    if (EXPECT_TRUE(w)) {
      EXPECT_EQ(TKey((*w).Op, (*w).OpArena), TKey(int64_t(4444), &arena, state_alloc));
    }
  }
}

/* #770: a memory layer is read while it is still being written. One writer (Tetris promoting under
   the repo's DataLock, or the merge thread building a new layer) inserts, and any number of readers
   walk the layer at the same time without a lock: the present walkers (point, exact point through
   the skip list, range) over the entry list and the update walker over the update list. A reader
   must never follow a link to an entry it can't fully see. Under ThreadSanitizer this fixture is the
   check that every link a reader follows is published with a release store and read with an
   acquire load; run normally, it checks that whatever a reader finds is whole and that an entry
   published before the reader looked is always found. */
FIXTURE(ConcurrentWalkInsert) {
  TMemoryLayer mem_layer(nullptr);
  const Base::TUuid idx(Base::TUuid::Twister);
  /* Keys arrive out of order (a permutation of 0..N-1), so most inserts land in the middle of the
     list rather than at its tail. The value written for key k is k * 10. */
  constexpr int64_t N = 1500, Stride = 7919;  // Stride is a prime that doesn't divide N
  auto key_at = [](int64_t i) { return (i * Stride) % N; };
  /* key_at(0 .. Published-1) are in the layer; release-stored by the writer after each insert. */
  std::atomic<int64_t> published(0);
  std::atomic<bool> done(false);
  std::atomic<size_t> bad(0), missing(0), walks(0);
  auto reader = [&](unsigned seed) {
    std::mt19937_64 rng(seed);
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    while (!done.load(std::memory_order_acquire)) {
      const int64_t known = published.load(std::memory_order_acquire);
      const int64_t pick = known ? key_at(static_cast<int64_t>(rng() % known)) : 0;
      const TIndexKey key(idx, TKey(make_tuple(pick), &arena, state_alloc));
      /* Exact point read: the skip list, then level 0. A key published before we looked is there. */ {
        auto wp = mem_layer.NewPresentWalker(key, /* exact_point */ true);
        if (*wp) {
          if (!(TKey((**wp).Op, (**wp).OpArena) == TKey(pick * 10, &arena, state_alloc))) {
            ++bad;
          }
        } else if (known) {
          ++missing;
        }
      }
      /* Point read from the head of the list. */ {
        auto wp = mem_layer.NewPresentWalker(key);
        if (*wp) {
          if (!(TKey((**wp).Op, (**wp).OpArena) == TKey(pick * 10, &arena, state_alloc))) {
            ++bad;
          }
        } else if (known) {
          ++missing;
        }
      }
      /* Range read over a window of keys: every entry seen is whole and in order. */ {
        const TIndexKey to(idx, TKey(make_tuple(pick + 64), &arena, state_alloc));
        int64_t last = -1;
        for (auto wp = mem_layer.NewPresentWalker(key, to); *wp; ++*wp) {
          std::tuple<int64_t> kt;
          int64_t v = -1;
          Sabot::ToNative(*Sabot::State::TAny::TWrapper((**wp).Key.NewState((**wp).KeyArena, state_alloc)), kt);
          Sabot::ToNative(*Sabot::State::TAny::TWrapper((**wp).Op.NewState((**wp).OpArena, state_alloc)), v);
          const int64_t k = std::get<0>(kt);
          if (k <= last || k < pick || k > pick + 64 || v != k * 10) {
            ++bad;
          }
          last = k;
        }
      }
      /* The update list, oldest first: sequence numbers strictly increase and each update is whole. */ {
        TSequenceNumber last = 0;
        size_t seen = 0;
        for (auto wp = mem_layer.NewUpdateWalker(0); *wp; ++*wp) {
          const auto &item = **wp;
          if (item.SequenceNumber <= last || item.EntryVec.size() != 1) {
            ++bad;
          }
          last = item.SequenceNumber;
          if (++seen == 64) {
            break;
          }
        }
      }
      ++walks;
    }
  };
  std::vector<std::thread> readers;
  for (unsigned r = 0; r < 3; ++r) {
    readers.emplace_back(reader, r + 1);
  }
  /* The writer: one thread, as in the server. */ {
    TSequenceNumber seq_num = 0UL;
    for (int64_t i = 0; i < N; ++i) {
      const int64_t k = key_at(i);
      Insert(mem_layer, ++seq_num, idx, k * 10, k);
      published.store(i + 1, std::memory_order_release);
    }
  }
  /* Let the readers see the finished layer too. */
  for (const size_t at_end = walks.load(); walks.load() < at_end + 3;) {
    std::this_thread::yield();
  }
  done.store(true, std::memory_order_release);
  for (auto &t : readers) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0UL);
  EXPECT_EQ(missing.load(), 0UL);
  EXPECT_EQ(mem_layer.GetSize(), static_cast<size_t>(N));
}

/* A pattern with a bound member after a free one is rejected at the walker (#944): the early stop in the match walks
   assumes frees trail, as the compiler guarantees (orly/synth/db_keys_expr.cc). */
FIXTURE(RejectsBoundAfterFree) {
  TMemoryLayer mem_layer(nullptr);
  Base::TUuid idx(Base::TUuid::Twister);
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Insert(mem_layer, 1UL, idx, 1L, 1L, 2L, 3L);
  auto rejected = [&](const TIndexKey &pattern) {
    try {
      mem_layer.NewPresentWalker(pattern);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  const Native::TFree<int64_t> free_int;
  EXPECT_TRUE(rejected(TIndexKey(idx, TKey(make_tuple(1L, free_int, 3L), &arena, state_alloc))));
  EXPECT_TRUE(rejected(TIndexKey(idx, TKey(make_tuple(free_int, 2L, free_int), &arena, state_alloc))));
  EXPECT_FALSE(rejected(TIndexKey(idx, TKey(make_tuple(1L, free_int, free_int), &arena, state_alloc))));
  EXPECT_FALSE(rejected(TIndexKey(idx, TKey(make_tuple(1L, 2L, 3L), &arena, state_alloc))));
}

/* The range walker (#735): it seeks to `from` and walks to `to`, which is
   either a whole key or a pattern whose free members are its rightmost ones. */
FIXTURE(RangeWalk) {
  TMemoryLayer mem_layer(nullptr);
  Base::TUuid idx_a(Base::TUuid::Twister);
  Base::TUuid idx_b(Base::TUuid::Twister);
  TSuprena arena;
  TSequenceNumber seq_num = 0UL;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  /* Groups 0, 1 and 2, each holding the even values 0 through 18; idx_b
     holds keys of group 1 too, which no walk of idx_a may surface. */ {
    for (int64_t g = 0; g < 3; ++g) {
      for (int64_t v = 0; v < 20; v += 2) {
        Insert(mem_layer, ++seq_num, idx_a, g * 100 + v, g, v);
      }
    }
    Insert(mem_layer, ++seq_num, idx_b, -1, int64_t(1), int64_t(5));
  }
  auto walk = [&](const TIndexKey &from, const TIndexKey &to) {
    std::vector<int64_t> vals;
    auto wp = mem_layer.NewPresentWalker(from, to);
    for (auto &w = *wp; w; ++w) {
      vals.push_back(Sabot::AsNative<int64_t>(*Sabot::State::TAny::TWrapper((*w).Op.NewState((*w).OpArena, state_alloc))));
    }
    return vals;
  };
  auto key = [&](const Base::TUuid &idx, int64_t g, int64_t v) {
    return TIndexKey(idx, TKey(make_tuple(g, v), &arena, state_alloc));
  };
  const TIndexKey group_1(idx_a, TKey(make_tuple(int64_t(1), Native::TFree<int64_t>()), &arena, state_alloc));
  /* From a stored key to the end of the pattern's range. */
  EXPECT_TRUE(walk(key(idx_a, 1, 14), group_1) == (std::vector<int64_t>{114, 116, 118}));
  /* From a key that is not stored: the walk starts at the next one. */
  EXPECT_TRUE(walk(key(idx_a, 1, 13), group_1) == (std::vector<int64_t>{114, 116, 118}));
  /* From past the range: nothing, and nothing of group 2. */
  EXPECT_TRUE(walk(key(idx_a, 1, 99), group_1).empty());
  /* Two whole keys: the range is inclusive at both ends and crosses groups. */
  EXPECT_TRUE(walk(key(idx_a, 0, 16), key(idx_a, 1, 2)) == (std::vector<int64_t>{16, 18, 100, 102}));
  /* Two whole keys, `from` not stored (the walker used to return nothing
     unless the first key at or after `from` was `from` itself). */
  EXPECT_TRUE(walk(key(idx_a, 0, 15), key(idx_a, 1, 0)) == (std::vector<int64_t>{16, 18, 100}));
  /* Another index's keys never appear. */
  EXPECT_TRUE(walk(key(idx_b, 1, 0), TIndexKey(idx_b, TKey(make_tuple(int64_t(1), Native::TFree<int64_t>()), &arena, state_alloc))) ==
              (std::vector<int64_t>{-1}));
}

/* A pattern walk seeks to the pattern's run through the skip list instead of matching every entry
   from the head of the layer. Keys are (string, desc int, int) over three indexes, most with several
   versions, inserted in a shuffled order; one string is too long to sit directly in a core, and the
   desc member orders backwards. For patterns bound on none, some or all of their members, the walk
   must surface exactly the entries of the pattern's index that unify with it, in level-0 order, as a
   filter of the whole layer finds them, and SeekPattern must land on the first of them -- or, for a
   run the layer does not hold, on the first entry of the next one. */
FIXTURE(PatternSeek) {
  using D = TDesc<int64_t>;
  const string long_str = "e string well past direct storage in a core";
  const vector<string> strs = { "a", "c", long_str };
  TMemoryLayer mem_layer(nullptr);
  const vector<Base::TUuid> idxs = { Base::TUuid(Base::TUuid::Twister), Base::TUuid(Base::TUuid::Twister), Base::TUuid(Base::TUuid::Twister) };
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
  void *state_alloc_2 = static_cast<uint8_t *>(state_alloc) + Sabot::State::GetMaxStateSize();
  /* Every key gets 1 to 3 versions; every op value is distinct, so it names its entry. */ {
    std::mt19937_64 rng(257);
    vector<tuple<size_t, string, int64_t, int64_t>> keys;
    for (size_t i = 0; i < idxs.size(); ++i) {
      for (const string &str : strs) {
        for (int64_t d = 0; d < 6; ++d) {
          for (int64_t v = 0; v < 8; v += 1 + static_cast<int64_t>(rng() % 2)) {
            for (uint64_t n = 1 + rng() % 3; n; --n) {
              keys.emplace_back(i, str, d, v);
            }
          }
        }
      }
    }
    std::shuffle(keys.begin(), keys.end(), rng);
    TSequenceNumber seq_num = 0UL;
    for (const auto &[i, str, d, v] : keys) {
      ++seq_num;
      Insert(mem_layer, seq_num, idxs[i], static_cast<int64_t>(seq_num), str, D(d), v);
    }
  }
  auto op_of = [&](const Atom::TCore &op, Atom::TCore::TArena *op_arena) {
    return Sabot::AsNative<int64_t>(*Sabot::State::TAny::TWrapper(op.NewState(op_arena, state_alloc)));
  };
  /* What the walk must surface: the pattern's index's entries that unify with it, in level-0 order. */
  auto filter = [&](const TIndexKey &pattern) {
    vector<int64_t> ops;
    for (TMemoryLayer::TEntryCollection::TCursor csr(mem_layer.GetEntryCollection()); csr; ++csr) {
      if (csr->GetIndexKey().GetIndexId() == pattern.GetIndexId() &&
          Sabot::MatchPrefixState(
              *Sabot::State::TAny::TWrapper(pattern.GetKey().GetCore().NewState(pattern.GetKey().GetArena(), state_alloc)),
              *Sabot::State::TAny::TWrapper(csr->GetKey().GetCore().NewState(csr->GetKey().GetArena(), state_alloc_2))) == Sabot::TMatchResult::Unifies) {
        ops.push_back(op_of(csr->GetOp(), csr->GetKey().GetArena()));
      }
    }
    return ops;
  };
  auto walk = [&](const TIndexKey &pattern) {
    vector<int64_t> ops;
    auto wp = mem_layer.NewPresentWalker(pattern);
    for (auto &w = *wp; w; ++w) {
      ops.push_back(op_of((*w).Op, (*w).OpArena));
    }
    return ops;
  };
  auto landing = [&](const TIndexKey &pattern) {
    auto csr = mem_layer.SeekPattern(pattern);
    return csr ? op_of(csr->GetOp(), csr->GetKey().GetArena()) : int64_t(-1);
  };
  const Native::TFree<string> free_str;
  const Native::TFree<D> free_desc;
  const Native::TFree<int64_t> free_int;
  size_t checked = 0;
  for (const Base::TUuid &idx : idxs) {
    vector<TIndexKey> patterns = { TIndexKey(idx, TKey(make_tuple(free_str, free_desc, free_int), &arena, state_alloc)) };
    for (const string &str : strs) {
      patterns.emplace_back(idx, TKey(make_tuple(str, free_desc, free_int), &arena, state_alloc));
      for (int64_t d = 0; d < 6; d += 2) {
        patterns.emplace_back(idx, TKey(make_tuple(str, D(d), free_int), &arena, state_alloc));
        patterns.emplace_back(idx, TKey(make_tuple(str, D(d), int64_t(4)), &arena, state_alloc));
      }
    }
    for (const TIndexKey &pattern : patterns) {
      const vector<int64_t> want = filter(pattern);
      EXPECT_TRUE(walk(pattern) == want);
      if (!want.empty()) {
        EXPECT_EQ(landing(pattern), want.front());
        ++checked;
      }
    }
    /* Runs the layer does not hold: "b" sorts between "a" and "c", desc 9 before desc 5. */
    EXPECT_TRUE(walk(TIndexKey(idx, TKey(make_tuple(string("b"), free_desc, free_int), &arena, state_alloc))).empty());
    EXPECT_EQ(landing(TIndexKey(idx, TKey(make_tuple(string("b"), free_desc, free_int), &arena, state_alloc))),
              filter(TIndexKey(idx, TKey(make_tuple(string("c"), free_desc, free_int), &arena, state_alloc))).front());
    EXPECT_TRUE(walk(TIndexKey(idx, TKey(make_tuple(string("c"), D(9), free_int), &arena, state_alloc))).empty());
    EXPECT_EQ(landing(TIndexKey(idx, TKey(make_tuple(string("c"), D(9), free_int), &arena, state_alloc))),
              filter(TIndexKey(idx, TKey(make_tuple(string("c"), D(5), free_int), &arena, state_alloc))).front());
  }
  /* Value 0 is stored under every (string, desc) pair, so every pattern above that leaves the last
     member free found something, and the landing checks did not pass vacuously. */
  EXPECT_TRUE(checked >= idxs.size() * (1 + strs.size() * 4));
}

/* SeekPattern orders keys against a pattern core to core where it can, and through Sabot otherwise; the two must always agree.
   Order every key of a layer -- (string, int, bool, desc int, double) over two indexes, strings short and too long to sit
   directly in a core -- against patterns bound on none, some or all of their members, with values the layer holds and ones
   it does not, members of a type the keys' are not, and fewer or more members than the keys. */
FIXTURE(PatternPrefixFastPathAgreesWithSabot) {
  using D = TDesc<int64_t>;
  const string long_str = "a string well past direct storage in a core, which is held in the arena";
  const vector<string> strs = { "", "b", "d", long_str };
  const vector<int64_t> ints = { -3, 0, 7 };
  TMemoryLayer mem_layer(nullptr);
  const vector<Base::TUuid> idxs = { Base::TUuid(Base::TUuid::Twister), Base::TUuid(Base::TUuid::Twister) };
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TSequenceNumber seq_num = 0UL;
  for (const Base::TUuid &idx : idxs) {
    for (const string &str : strs) {
      for (int64_t i : ints) {
        for (bool b : {false, true}) {
          Insert(mem_layer, ++seq_num, idx, int64_t(0), str, i, b, D(i), double(i) / 2.0);
        }
      }
    }
  }
  const Native::TFree<string> free_str;
  const Native::TFree<int64_t> free_int;
  const Native::TFree<bool> free_bool;
  const Native::TFree<D> free_desc;
  const Native::TFree<double> free_double;
  const vector<string> pattern_strs = { "", "a", "b", "c", long_str, long_str + "!" };
  const vector<int64_t> pattern_ints = { -4, -3, 0, 5, 7, 9 };
  vector<TIndexKey> patterns;
  for (const Base::TUuid &idx : idxs) {
    auto add = [&](const auto &tuple) {
      patterns.emplace_back(idx, TKey(tuple, &arena, state_alloc));
    };
    add(make_tuple(free_str, free_int, free_bool, free_desc, free_double));
    add(make_tuple(int64_t(1), free_int));               // a first member of another type
    add(make_tuple(int32_t(1), free_int));               // another integer type
    add(make_tuple(string("b")));                        // fewer members
    add(make_tuple(string("b"), int64_t(0), true, D(0), 0.0, int64_t(1)));  // more members
    for (const string &str : pattern_strs) {
      add(make_tuple(str, free_int, free_bool, free_desc, free_double));
      add(make_tuple(str, int32_t(0), free_bool, free_desc, free_double));
      for (int64_t i : pattern_ints) {
        add(make_tuple(str, i, free_bool, free_desc, free_double));
        for (bool b : {false, true}) {
          add(make_tuple(str, i, b, free_desc, free_double));
          add(make_tuple(str, i, b, D(i), free_double));
          add(make_tuple(str, i, b, D(i), double(i) / 2.0));
          add(make_tuple(str, i, b, D(-i), double(i)));
        }
      }
    }
  }
  size_t compared = 0UL, lt = 0UL, gt = 0UL, disagree = 0UL;
  for (const TIndexKey &pattern : patterns) {
    for (TMemoryLayer::TEntryCollection::TCursor csr(mem_layer.GetEntryCollection()); csr; ++csr) {
      const Atom::TComparison fast = TMemoryLayer::ComparePatternPrefix(csr->GetIndexKey(), pattern);
      const Atom::TComparison slow = TMemoryLayer::ComparePatternPrefixBySabot(csr->GetIndexKey(), pattern);
      if (fast != slow) {
        ++disagree;
      }
      lt += Atom::IsLt(slow) ? 1UL : 0UL;
      gt += Atom::IsGt(slow) ? 1UL : 0UL;
      ++compared;
    }
  }
  EXPECT_EQ(disagree, 0UL);
  /* Not vacuous: plenty of keys sort before and after the patterns, and some can't be told apart from them. */
  EXPECT_GT(lt, compared / 10UL);
  EXPECT_GT(gt, compared / 10UL);
  EXPECT_GT(compared - lt - gt, 100UL);
}

/* An update holding one entry per (index id, key) in keys, each with op val, at seq_num. */
static TUpdate *MakeUpdate(TSequenceNumber seq_num, const vector<pair<Base::TUuid, int64_t>> &keys, int64_t val) {
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TUpdate::TOpByKey op_by_key;
  for (const auto &key : keys) {
    op_by_key.emplace(TIndexKey(key.first, TKey(make_tuple(key.second), &arena, state_alloc)), TKey(val, &arena, state_alloc));
  }
  std::shared_ptr<TUpdate> update(TUpdate::NewUpdate(op_by_key, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc)));
  update->SetSequenceNumber(seq_num);
  return TUpdate::CopyUpdate(update.get(), state_alloc);
}

/* The memory-layer half of the read-time trace under ingest: a pattern walk used to match every
   entry from the head of the layer, so its cost grew with the layer (other indexes included).
   Time 2,000 walks of <[a, free]> over a layer of 4k to 128k entries, half of them in a second
   index that sorts ahead of the one read, for an a the layer holds and for one it does not. A walk
   that finds nothing used to run on to the end of the index. Prints a table and checks nothing
   about timing -- a shared runner is too noisy for that. */
FIXTURE(PatternSeekScaling) {
  std::mt19937_64 rng(257);
  const Base::TUuid lo(Base::TUuid::Twister), hi(Base::TUuid::Twister);
  const Base::TUuid &other = std::min(lo, hi), &read = std::max(lo, hi);
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  std::cout << "  entries   ms per 2,000 pattern walks: present   absent" << std::endl;
  for (int64_t entries = 4096; entries <= 131072; entries *= 2) {
    TMemoryLayer mem_layer(nullptr);
    vector<pair<Base::TUuid, int64_t>> keys;
    for (int64_t i = 0; i < entries / 2; ++i) {
      keys.emplace_back(other, i);
    }
    mem_layer.Insert(MakeUpdate(1, keys, 0));
    TSequenceNumber seq_num = 1UL;
    for (int64_t a = 0; a < entries / 32; ++a) {
      for (int64_t b = 0; b < 16; ++b) {
        Insert(mem_layer, ++seq_num, read, b, a * 2, b);
      }
    }
    TSuprena arena;
    std::cout << "  " << entries;
    for (int64_t absent : {0, 1}) {
      size_t found = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int walks = 0; walks < 2000; ++walks) {
        const int64_t a = static_cast<int64_t>(rng() % static_cast<uint64_t>(entries / 32)) * 2 + absent;
        auto wp = mem_layer.NewPresentWalker(TIndexKey(read, TKey(make_tuple(a, Native::TFree<int64_t>()), &arena, state_alloc)));
        for (auto &w = *wp; w; ++w) {
          ++found;
        }
      }
      std::cout << "\t    " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      EXPECT_EQ(found, absent ? 0UL : 2000UL * 16UL);
    }
    std::cout << std::endl;
  }
}

/* #754: an insert finds its place through the skip list instead of walking back from the tail.
   Insert single-entry and batched updates over two indexes, several versions of most keys, in a
   shuffled order, and check level 0 comes out exactly as an independent sort of the same entries
   (index key ascending, then sequence number descending), that the layer's size counts every
   entry, and that exact-point seeks, which descend the express lanes, find every key's newest
   version and nothing for absent keys. Run once with sequence numbers that follow the insertion
   order (the commit path) and once with sequence numbers in no particular order (the fold). */
FIXTURE(RandomOrderInsert) {
  for (bool seq_follows_insertion : { true, false }) {
    TMemoryLayer mem_layer(nullptr);
    const Base::TUuid idx_a(Base::TUuid::Twister), idx_b(Base::TUuid::Twister);
    const int64_t num_keys = 1500;
    std::mt19937_64 rng(seq_follows_insertion ? 754 : 457);
    /* Every write: (index id, key), versions 1 to 3 of each key. */
    vector<pair<Base::TUuid, int64_t>> writes;
    for (const auto &idx : { idx_a, idx_b }) {
      for (int64_t k = 0; k < num_keys; ++k) {
        for (uint64_t v = 0, versions = 1 + rng() % 3; v < versions; ++v) {
          writes.emplace_back(idx, k);
        }
      }
    }
    std::shuffle(writes.begin(), writes.end(), rng);
    /* Group them into updates: mostly single writes, some batches of up to 32 distinct keys. */
    vector<vector<pair<Base::TUuid, int64_t>>> groups;
    for (size_t i = 0; i < writes.size();) {
      vector<pair<Base::TUuid, int64_t>> group;
      const size_t want = (rng() % 4 == 0) ? 1 + rng() % 32 : 1;
      for (; i < writes.size() && group.size() < want; ++i) {
        if (std::find(group.begin(), group.end(), writes[i]) != group.end()) {
          break;
        }
        group.push_back(writes[i]);
      }
      groups.push_back(std::move(group));
    }
    vector<TSequenceNumber> seqs(groups.size());
    for (size_t i = 0; i < seqs.size(); ++i) {
      seqs[i] = i + 1;
    }
    if (!seq_follows_insertion) {
      std::shuffle(seqs.begin(), seqs.end(), rng);
    }
    vector<const TUpdate::TEntry *> expected;
    for (size_t i = 0; i < groups.size(); ++i) {
      TUpdate *update = MakeUpdate(seqs[i], groups[i], static_cast<int64_t>(seqs[i]));
      for (TUpdate::TEntryCollection::TCursor csr(update->GetEntryCollection()); csr; ++csr) {
        expected.push_back(&*csr);
      }
      mem_layer.Insert(update);
    }
    std::sort(expected.begin(), expected.end(), [](const TUpdate::TEntry *a, const TUpdate::TEntry *b) {
      if (a->GetIndexKey() < b->GetIndexKey()) {
        return true;
      }
      if (b->GetIndexKey() < a->GetIndexKey()) {
        return false;
      }
      return a->GetSequenceNumber() > b->GetSequenceNumber();
    });
    EXPECT_EQ(mem_layer.GetSize(), expected.size());
    /* Level 0 is exactly the sorted order. */ {
      size_t pos = 0, mismatches = 0;
      for (TMemoryLayer::TEntryCollection::TCursor csr(mem_layer.GetEntryCollection()); csr; ++csr, ++pos) {
        if (pos >= expected.size() || &*csr != expected[pos]) {
          ++mismatches;
        }
      }
      EXPECT_EQ(pos, expected.size());
      EXPECT_EQ(mismatches, 0UL);
    }
    /* Exact-point seeks find each key's newest version, whose op is its sequence number. */ {
      TSuprena arena;
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      size_t runs = 0, wrong = 0;
      for (size_t i = 0; i < expected.size(); ++i) {
        if (i > 0 && expected[i - 1]->GetIndexKey() == expected[i]->GetIndexKey()) {
          continue;  // not the newest of its run
        }
        ++runs;
        auto wp = mem_layer.NewPresentWalker(expected[i]->GetIndexKey(), /* exact_point */ true);
        auto &w = *wp;
        if (!w || (*w).SequenceNumber != expected[i]->GetSequenceNumber()) {
          ++wrong;
        }
      }
      EXPECT_EQ(runs, static_cast<size_t>(2 * num_keys));
      EXPECT_EQ(wrong, 0UL);
      for (const auto &idx : { idx_a, idx_b }) {
        for (int64_t k : { int64_t(-1), num_keys, num_keys + 7 }) {
          auto wp = mem_layer.NewPresentWalker(TIndexKey(idx, TKey(make_tuple(k), &arena, state_alloc)), true);
          EXPECT_FALSE(*wp);
        }
      }
    }
  }
}

/* #754: entries with the same index key and the same sequence number (two updates numbered alike)
   keep the order they were inserted in, as OrderedList's ReverseInsert placed them, both when the
   second one is out of order and when it is appended at the tail. */
FIXTURE(EqualEntriesKeepInsertionOrder) {
  TMemoryLayer mem_layer(nullptr);
  const Base::TUuid idx(Base::TUuid::Twister);
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  mem_layer.Insert(MakeUpdate(7, { { idx, 50 } }, 1));
  for (int64_t k = 100; k < 400; ++k) {
    mem_layer.Insert(MakeUpdate(8 + k, { { idx, k } }, k));
  }
  mem_layer.Insert(MakeUpdate(7, { { idx, 50 } }, 2));  // out of order: 50 is far behind the tail
  mem_layer.Insert(MakeUpdate(1000, { { idx, 500 } }, 1));
  mem_layer.Insert(MakeUpdate(1000, { { idx, 500 } }, 2));  // in order: equal to the tail
  for (int64_t k : { int64_t(50), int64_t(500) }) {
    const TKey key(make_tuple(k), &arena, state_alloc);
    vector<int64_t> ops;
    for (TMemoryLayer::TEntryCollection::TCursor csr(mem_layer.GetEntryCollection()); csr; ++csr) {
      if (csr->GetKey() == key) {
        ops.push_back(TKey(csr->GetOp(), csr->GetKey().GetArena()) == TKey(int64_t(1), &arena, state_alloc) ? 1 : 2);
      }
    }
    EXPECT_TRUE(ops == vector<int64_t>({ 1, 2 }));
  }
  EXPECT_EQ(mem_layer.GetSize(), 304UL);
}

/* #754 micro-benchmark: the time to insert one batch of 4,096 keys into a layer that already
   holds 0 to ~130k entries, keys in order and at random. Before the fix the random column grew
   with the layer (every insert walked back from the tail); now both stay flat. Prints a table
   and checks nothing about timing -- a shared runner is too noisy for that. */
FIXTURE(InsertOrderScaling) {
  const int64_t batch = 4096, batches = 32;
  std::mt19937_64 rng(754);
  const Base::TUuid idx(Base::TUuid::Twister);
  vector<vector<double>> ms(2);
  for (int mode = 0; mode < 2; ++mode) {
    TMemoryLayer mem_layer(nullptr);
    int64_t next = 0;
    for (int64_t b = 0; b < batches; ++b) {
      vector<pair<Base::TUuid, int64_t>> keys;
      for (int64_t i = 0; i < batch; ++i) {
        keys.emplace_back(idx, mode == 0 ? next++ : static_cast<int64_t>(rng() >> 4));
      }
      TUpdate *update = MakeUpdate(static_cast<TSequenceNumber>(b + 1), keys, b);
      const auto start = std::chrono::steady_clock::now();
      mem_layer.Insert(update);
      ms[mode].push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
  }
  std::cout << "  entries before batch   in order ms   random ms" << std::endl;
  for (int64_t b = 0; b < batches; b += 4) {
    std::cout << "  " << b * batch << "\t\t\t" << ms[0][b] << "\t\t" << ms[1][b] << std::endl;
  }
  std::cout << "  " << (batches - 1) * batch << "\t\t\t" << ms[0].back() << "\t\t" << ms[1].back() << std::endl;
}

/* #879: an update builds its entries in key order. Its EntryCollection is ordered by key alone,
   and each entry finds its place walking back from the tail; TOpByKey iterates by index id
   first, so an update spanning several indexes, linked in that order, walked back over most of
   its own entries, O(N^2). Print-only: time NewUpdate against linking the same entries one by
   one in TOpByKey order (what it used to do), over 1, 4 and 8 indexes. "disjoint" gives each
   index its own key range (as a write touching several key shapes does), in an order unrelated
   to the index ids; "interleaved" draws every index's keys from one range. Both builds must
   hold the same keys in the same order. */
FIXTURE(UpdateBuildScaling) {
  std::mt19937_64 rng(878);
  constexpr int64_t num_entries = 4096;
  std::cout << "  indexes  layout        one by one ms   NewUpdate ms" << std::endl;
  for (int64_t num_indexes : { 1, 4, 8 }) {
    for (bool disjoint : { true, false }) {
      if (num_indexes == 1 && !disjoint) {
        continue;
      }
      vector<Base::TUuid> idx_ids;
      vector<int64_t> prefix;
      for (int64_t i = 0; i < num_indexes; ++i) {
        idx_ids.emplace_back(Base::TUuid::Twister);
        prefix.push_back(disjoint ? i : 0);
      }
      std::ranges::shuffle(prefix, rng);
      Atom::TSuprena arena;
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      TUpdate::TOpByKey op_by_key;
      while (static_cast<int64_t>(op_by_key.size()) < num_entries) {
        const auto i = static_cast<size_t>(rng() % static_cast<uint64_t>(num_indexes));
        op_by_key.emplace(TIndexKey(idx_ids[i], TKey(make_tuple(prefix[i], static_cast<int64_t>(rng() >> 4)), &arena, state_alloc)),
                          TKey(int64_t(1), &arena, state_alloc));
      }
      const TKey meta(&arena), id(Base::TUuid(Base::TUuid::Best), &arena, state_alloc);
      auto start = std::chrono::steady_clock::now();
      auto one_by_one = TUpdate::NewUpdate({}, meta, id);
      for (const auto &[index_key, op] : op_by_key) {
        one_by_one->AddEntry(index_key, op);
      }
      const double one_by_one_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      start = std::chrono::steady_clock::now();
      auto built = TUpdate::NewUpdate(op_by_key, meta, id);
      const double built_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      TUpdate::TEntryCollection::TCursor a(one_by_one->GetEntryCollection()), b(built->GetEntryCollection());
      int64_t count = 0;
      for (; a && b; ++a, ++b, ++count) {
        EXPECT_TRUE(a->GetIndexKey() == b->GetIndexKey());
      }
      EXPECT_FALSE(a || b);
      EXPECT_EQ(count, num_entries);
      std::cout << "  " << num_indexes << "\t   " << (disjoint ? "disjoint   " : "interleaved") << "   " << one_by_one_ms << "\t   " << built_ms
                << std::endl;
    }
  }
}

/* #879: NewUpdate with entries that carry their own mutators. Each entry keeps its op and
   mutator, the collection comes out in key order, and entries with equal keys (in different
   indexes) keep the order they were given in, op_by_key's before more's, which is where linking
   them one by one would have put them. */
FIXTURE(UpdateBuildKeepsMutatorsAndTies) {
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const Base::TUuid idx_a(Base::TUuid::Twister), idx_b(Base::TUuid::Twister), idx_c(Base::TUuid::Twister);
  auto key = [&](const Base::TUuid &idx, int64_t k) { return TIndexKey(idx, TKey(make_tuple(k), &arena, state_alloc)); };
  auto op = [&](int64_t v) { return TKey(v, &arena, state_alloc); };
  TUpdate::TOpByKey op_by_key{ { key(idx_a, 5), op(50) }, { key(idx_a, 2), op(20) }, { key(idx_b, 5), op(51) }, { key(idx_b, 1), op(10) } };
  vector<TUpdate::TNewEntry> more{ { key(idx_c, 5), op(52), TMutator::Add }, { key(idx_c, 3), op(30), TMutator::Add } };
  auto update = TUpdate::NewUpdate(op_by_key, more, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));
  /* The idx_a/idx_b tie at 5 keeps op_by_key's order, which is by index id. */
  const bool a_first = idx_a < idx_b;
  const vector<std::tuple<int64_t, int64_t, TMutator>> expected{ { 1, 10, TMutator::Assign },
                                                                  { 2, 20, TMutator::Assign },
                                                                  { 3, 30, TMutator::Add },
                                                                  { 5, a_first ? 50 : 51, TMutator::Assign },
                                                                  { 5, a_first ? 51 : 50, TMutator::Assign },
                                                                  { 5, 52, TMutator::Add } };
  vector<std::tuple<int64_t, int64_t, TMutator>> actual;
  for (TUpdate::TEntryCollection::TCursor csr(update->GetEntryCollection()); csr; ++csr) {
    std::tuple<int64_t> k;
    int64_t v;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(csr->GetKey().GetState(state_alloc)), k);
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(csr->GetOp().NewState(&csr->GetSuprena(), state_alloc)), v);
    actual.emplace_back(std::get<0>(k), v, csr->GetMutator());
  }
  EXPECT_TRUE(actual == expected);
}

/* #892: copy every update of src into a fresh layer the way StepMergeMem does: shallow-copy the
   updates, then hand the entries, in src's entry order, to ImporterAppendCopies. */
static void CopyLayer(const TMemoryLayer &src, TMemoryLayer *dst, void *state_alloc) {
  std::unordered_map<const TUpdate *, TUpdate *> remap;
  for (TMemoryLayer::TUpdateCollection::TCursor csr(src.GetUpdateCollection()); csr; ++csr) {
    TUpdate *copy = TUpdate::ShallowCopy(&*csr, state_alloc);
    remap.emplace(&*csr, copy);
    dst->ImporterAppendUpdate(copy);
  }
  vector<TMemoryLayer::TCopiedEntry> copies;
  for (TMemoryLayer::TEntryCollection::TCursor csr(src.GetEntryCollection()); csr; ++csr) {
    copies.push_back({ &*csr, remap.at(csr->GetUpdate()) });
  }
  dst->ImporterAppendCopies(copies, state_alloc);
}

static bool SameEntry(const TUpdate::TEntry &a, const TUpdate::TEntry &b) {
  return a.GetIndexKey() == b.GetIndexKey() && TKey(a.GetOp(), &a.GetSuprena()) == TKey(b.GetOp(), &b.GetSuprena()) &&
         a.GetMutator() == b.GetMutator() && a.GetSequenceNumber() == b.GetSequenceNumber();
}

/* #892: the copy has the same entries as the source, in the same order, in each update and in the
   layer, with the mutators kept and equal keys across indexes in memory-layer order. */
FIXTURE(CopyKeepsEntriesOrderAndMutators) {
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const Base::TUuid idx_a(Base::TUuid::Twister), idx_b(Base::TUuid::Twister), idx_c(Base::TUuid::Twister);
  auto key = [&](const Base::TUuid &idx, int64_t k) { return TIndexKey(idx, TKey(make_tuple(k), &arena, state_alloc)); };
  auto op = [&](int64_t v) { return TKey(v, &arena, state_alloc); };
  TMemoryLayer src(nullptr);
  /* Two updates over three indexes, with keys that tie across indexes, and Add mutators. (Entries
     added with a mutator are kept off the ties: their order among equal keys is not the index order.) */
  TUpdate::TOpByKey first{ { key(idx_a, 5), op(1) }, { key(idx_a, 2), op(2) }, { key(idx_b, 5), op(3) }, { key(idx_b, 1), op(4) } };
  vector<TUpdate::TNewEntry> first_more{ { key(idx_c, 7), op(5), TMutator::Add }, { key(idx_c, 3), op(6), TMutator::Add } };
  TUpdate::TOpByKey second{ { key(idx_a, 5), op(7) }, { key(idx_c, 5), op(8) }, { key(idx_b, 2), op(9) } };
  auto u1 = TUpdate::NewUpdate(first, first_more, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));
  u1->SetSequenceNumber(1);
  src.Insert(TUpdate::CopyUpdate(u1.get(), state_alloc));
  auto u2 = TUpdate::NewUpdate(second, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));
  u2->SetSequenceNumber(2);
  src.Insert(TUpdate::CopyUpdate(u2.get(), state_alloc));
  TMemoryLayer dst_layer(nullptr);
  TMemoryLayer *const dst = &dst_layer;
  CopyLayer(src, dst, state_alloc);
  EXPECT_EQ(dst->GetSize(), src.GetSize());
  /* the layer's entry order */ {
    TMemoryLayer::TEntryCollection::TCursor a(src.GetEntryCollection()), b(dst->GetEntryCollection());
    size_t count = 0;
    for (; a && b; ++a, ++b, ++count) {
      EXPECT_TRUE(SameEntry(*a, *b));
    }
    EXPECT_FALSE(a || b);
    EXPECT_EQ(count, 9UL);
  }
  /* each update's entry order, and that its entries are its own */ {
    TMemoryLayer::TUpdateCollection::TCursor a(src.GetUpdateCollection()), b(dst->GetUpdateCollection());
    for (; a && b; ++a, ++b) {
      EXPECT_EQ(a->GetSequenceNumber(), b->GetSequenceNumber());
      TUpdate::TEntryCollection::TCursor x(a->GetEntryCollection()), y(b->GetEntryCollection());
      for (; x && y; ++x, ++y) {
        EXPECT_TRUE(SameEntry(*x, *y));
        EXPECT_TRUE(y->GetUpdate() == &*b);
      }
      EXPECT_FALSE(x || y);
    }
    EXPECT_FALSE(a || b);
  }
  /* the Add mutators survived */ {
    size_t adds = 0;
    for (TMemoryLayer::TEntryCollection::TCursor csr(dst->GetEntryCollection()); csr; ++csr) {
      adds += csr->GetMutator() == TMutator::Add ? 1 : 0;
    }
    EXPECT_EQ(adds, 2UL);
  }
}

/* #892: time copying a layer of 4,096 entries across 8 indexes, against adding the same entries
   one by one in memory-layer order (what StepMergeMem used to do). The copy must come out the
   same, and the old way is quadratic here, so even on a noisy runner the new way is well ahead. */
FIXTURE(CopyScaling) {
  std::mt19937_64 rng(892);
  constexpr int64_t num_entries = 4096, num_indexes = 8;
  vector<Base::TUuid> idx_ids;
  for (int64_t i = 0; i < num_indexes; ++i) {
    idx_ids.emplace_back(Base::TUuid::Twister);
  }
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TUpdate::TOpByKey op_by_key;
  while (static_cast<int64_t>(op_by_key.size()) < num_entries) {
    const auto i = static_cast<size_t>(rng() % static_cast<uint64_t>(num_indexes));
    op_by_key.emplace(TIndexKey(idx_ids[i], TKey(make_tuple(static_cast<int64_t>(i), static_cast<int64_t>(rng() >> 4)), &arena, state_alloc)),
                      TKey(int64_t(1), &arena, state_alloc));
  }
  TMemoryLayer src(nullptr);
  auto update = TUpdate::NewUpdate(op_by_key, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));
  update->SetSequenceNumber(1);
  src.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));
  auto start = std::chrono::steady_clock::now();
  TMemoryLayer old_way(nullptr);
  TUpdate *old_update = TUpdate::ShallowCopy(&*TMemoryLayer::TUpdateCollection::TCursor(src.GetUpdateCollection()), state_alloc);
  for (TMemoryLayer::TEntryCollection::TCursor csr(src.GetEntryCollection()); csr; ++csr) {
    old_update->AddEntry(csr->GetIndexKey(), TKey(csr->GetOp(), &csr->GetSuprena()), csr->GetMutator());
  }
  old_way.Insert(old_update);
  const double old_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  TMemoryLayer dst_layer(nullptr);
  TMemoryLayer *const dst = &dst_layer;
  start = std::chrono::steady_clock::now();
  CopyLayer(src, dst, state_alloc);
  const double new_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::cout << "  " << num_entries << " entries, " << num_indexes << " indexes: memory-layer order " << old_ms << " ms, key order " << new_ms << " ms"
            << std::endl;
  TMemoryLayer::TEntryCollection::TCursor a(old_way.GetEntryCollection()), b(dst->GetEntryCollection());
  size_t count = 0;
  for (; a && b; ++a, ++b, ++count) {
    EXPECT_TRUE(SameEntry(*a, *b));
  }
  EXPECT_FALSE(a || b);
  EXPECT_EQ(count, static_cast<size_t>(num_entries));
  EXPECT_TRUE(new_ms < old_ms);
}

#if 0
FIXTURE(Range) {
  TMemoryLayer layer(nullptr);
  int64_t num_iter = 10L;
  TSequenceNumber seq_num = 1UL;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  for (int64_t i = 0; i < num_iter; ++i) {
    TSuprena arena;
    auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TKey(make_tuple(num_iter - 1L - i), &arena, state_alloc), TKey((num_iter - 1L - i) * 10L, &arena, state_alloc)} }, TKey(), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
    TUpdate *my_update = TUpdate::CopyUpdate(update.get());
    my_update->SetSequenceNumber(seq_num++);
    layer.Insert(my_update);
  }
  TSuprena arena;
  /* check that all the updates are there */ {
    auto walker_ptr = layer.NewPresentWalker(TKey(make_tuple(0L), &arena, state_alloc), TKey(make_tuple(num_iter), &arena, state_alloc));
    int64_t count = 0L;
    for (auto &walker = *walker_ptr; walker; ++walker) {
      EXPECT_EQ((*walker).SequenceNumber, static_cast<size_t>(num_iter) - count);
      EXPECT_EQ(TKey((*walker).Key, (*walker).Arena), TKey(make_tuple(count), &arena, state_alloc));
      EXPECT_EQ(TKey((*walker).Op, (*walker).Arena), TKey(count * 10L, &arena, state_alloc));
      ++count;
    }
    EXPECT_EQ(count, num_iter);
  }
}
#endif