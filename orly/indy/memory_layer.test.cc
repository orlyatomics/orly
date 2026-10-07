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

#include <atomic>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

#include <base/test/kit.h>
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