/* <orly/indy/repo.test.cc>

   Unit test for <orly/indy/repo.h>: a root safe repo's disk merges drop superseded history
   (#592).

   Overwrites the same keys round after round, flushing each round to its own disk file, and
   bumps a `+=` counter along the way, then drives TSafeRepo::StepMergeDisk by hand. A stub
   manager (as in context_fold.test.cc) keeps the run deterministic: its merge delays are long
   enough that, past each loop's first pass, the background merges never run.

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

#include <orly/indy/repo.h>

#include <optional>
#include <set>
#include <thread>

#include <base/scheduler.h>
#include <orly/indy/context.h>
#include <orly/indy/disk/merge_data_file.h>
#include <orly/indy/disk/present_walk_file.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber_test_runner.h>
#include <orly/indy/transaction_base.h>

#include <base/test/kit.h>

/* NB: no `using namespace Orly;` -- see context_fold.test.cc (it makes `L0` ambiguous). */
using namespace std;
using namespace std::literals;
using namespace Base;
using namespace Orly::Atom;
using namespace Orly::Indy;

namespace Sabot = Orly::Sabot;
using Orly::TMutator;
using Orly::TTtl;

const Orly::Indy::TMasterContext::TProtocol Orly::Indy::TMasterContext::TProtocol::Protocol;
const Orly::Indy::TSlaveContext::TProtocol Orly::Indy::TSlaveContext::TProtocol::Protocol;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 100UL);

Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::Pool(sizeof(Disk::TDurableManager::TMapping), "Durable Mapping", 10UL);
Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::TEntry::Pool(sizeof(Disk::TDurableManager::TMapping::TEntry), "Durable Mapping Entry", 10UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TDurableLayer::Pool(std::max(sizeof(Disk::TDurableManager::TMemSlushLayer), sizeof(Disk::TDurableManager::TDiskOrderedLayer)), "Durable Layer", 10UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TMemSlushLayer::TDurableEntry::Pool(sizeof(Disk::TDurableManager::TMemSlushLayer::TDurableEntry), "Durable Entry", 10UL);

Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(max(max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation", 100UL);
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction", 100UL);

Disk::TBufBlock::TPool Disk::TBufBlock::Pool(Disk::Util::PhysicalBlockSize, 2000UL);

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 10000UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 20000UL);

const std::vector<size_t> MemMergeCoreVec{0};
const std::vector<size_t> DiskMergeCoreVec{0};

/* The per-runner file caches that reading a disk layer needs; orlyi sets them up on each
   runner (see present_walk_file.test.cc). */
class TRunnerFileCaches {
  NO_COPY(TRunnerFileCaches);
  public:

  using TLocalReadFileCache = Disk::TLocalReadFileCache<Disk::Util::LogicalPageSize,
                                                        Disk::Util::LogicalBlockSize,
                                                        Disk::Util::PhysicalBlockSize,
                                                        Disk::Util::CheckedPage, true>;

  TRunnerFileCaches() {
    assert(!TLocalReadFileCache::Cache);
    TLocalReadFileCache::Cache = new TLocalReadFileCache();
    assert(!Disk::TLocalWalkerCache::Cache);
    Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
  }

  ~TRunnerFileCaches() {
    delete TLocalReadFileCache::Cache;
    TLocalReadFileCache::Cache = nullptr;
    delete Disk::TLocalWalkerCache::Cache;
    Disk::TLocalWalkerCache::Cache = nullptr;
  }

};  // TRunnerFileCaches

/* A safe repo whose memory merge the test can step by hand. */
class TSteppedSafeRepo final
    : public TSafeRepo {
  NO_COPY(TSteppedSafeRepo);
  public:

  using TSafeRepo::TSafeRepo;

  using Orly::Indy::TRepo::StepMergeMem;

  using TSafeRepo::FoldMergedFiles;

  /* So a test can fill the mapping pool (#627). */
  using TMappingType = Orly::Indy::TRepo::TMapping;

};  // TSteppedSafeRepo

/* A latch a test can park a merge on (#614). It lives outside the repo, because the repo may be
   gone by the time the merge is let go. */
struct TMergeLatch {
  std::mutex Mutex;
  std::condition_variable Cond;
  bool Entered = false;
  bool Released = false;
};

static TMergeLatch MergeLatch;

/* A fast repo whose memory merge parks on MergeLatch before it does any work (#614). */
class TLatchedFastRepo final
    : public TFastRepo {
  NO_COPY(TLatchedFastRepo);
  public:

  using TFastRepo::TFastRepo;

  /* Queue this repo for a memory merge, as AppendUpdate would. */
  void QueueMergeMem() {
    EnqueueMergeMem();
  }

  protected:

  virtual void StepMergeMem() override {
    /* park */ {
      std::unique_lock<std::mutex> lock(MergeLatch.Mutex);
      MergeLatch.Entered = true;
      MergeLatch.Cond.notify_all();
      MergeLatch.Cond.wait(lock, [] { return MergeLatch.Released; });
    }
    TFastRepo::StepMergeMem();
  }

};  // TLatchedFastRepo

/* A minimal L1::TManager that builds stepped safe repos, and latched fast ones. Nothing here runs
   the merge loops, so the tests drive every merge. */
class TMyManager
    : public L1::TManager {
  NO_COPY(TMyManager);
  public:

  TMyManager(Disk::Util::TEngine *engine, Base::TScheduler *scheduler, bool prune_merge_history = true, size_t max_repo_cache_size = 100UL)
      : TManager(engine, 1h, 1h, true, prune_merge_history, true, 1000ms, scheduler,
                 100UL, max_repo_cache_size, 20UL, MemMergeCoreVec, DiskMergeCoreVec, true) {}

  /* The sweeps Indy::TManager runs in its destructor. They also remove the files that merges
     retired, which needs ForEachScheduler: ~TManager's own drain runs after this object is gone. */
  virtual ~TMyManager() {
    ReleaseDirtySelfPins();
    CloseAllUnreferencedObjects();
  }

  virtual TRepo *ConstructRepo(const Base::TUuid &repo_id,
                               const std::optional<TTtl> &ttl,
                               const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                               bool is_safe,
                               bool /*create*/) override {
    return is_safe ?
      static_cast<TRepo *>(new TSteppedSafeRepo(this, repo_id, *ttl, parent_repo))
    : static_cast<TRepo *>(new TLatchedFastRepo(this, repo_id, *ttl, parent_repo));
  }

  virtual void SaveRepo(Orly::Indy::L0::TManager::TRepo *) override {}
  virtual void Enqueue(Orly::Indy::TTransactionReplication *, Orly::Indy::L1::TTransaction::TReplica &&) NO_THROW override {}
  virtual Orly::Indy::TTransactionReplication* NewTransactionReplication() override { return nullptr; }
  virtual void DeleteTransactionReplication(Orly::Indy::TTransactionReplication*) NO_THROW override {}
  virtual void ForEachScheduler(const std::function<bool (Fiber::TRunner *)> &/*cb*/) const override {}
  virtual bool CanLoad(const L0::TId &/*id*/) override { return true; }
  virtual void Delete(const L0::TId &/*id*/, L0::TSem */*sem*/) override {}
  virtual void Save(const L0::TId &/*id*/, const L0::TDeadline &/*deadline*/, const std::string &/*blob*/, L0::TSem */*sem*/) override {}
  virtual bool TryLoad(const L0::TId &/*id*/, std::string &/*blob*/) override { return true; }
  virtual TRepo *ReconstructRepo(const Base::TUuid &/*repo_id*/) override { return nullptr; }
  virtual void RunReplicationQueue() override {}
  virtual void RunReplicationWork() override {}
  virtual void RunReplicateTransaction() override {}
  virtual std::mutex &GetReplicationQueueLock() NO_THROW override { return ReplicationQueueLock; }

  inline TManager::TPtr<TRepo> GetRepo(const Base::TUuid &repo_id,
                                       const std::optional<TTtl> &ttl,
                                       const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                                       bool is_safe) {
    return OpenOrCreate(repo_id, ttl, parent_repo, is_safe);
  }

  using L0::TManager::TryOpenLiveRepo;

  private:

  std::mutex ReplicationQueueLock;
};

/* #592: overwrite the same keys round after round on a root safe repo, merge its disk files,
   and check that:
   (a) every key still reads its latest value, and a `+=` counter its full sum;
   (b) a view taken before the merges still walks every update it saw then, while a fresh
       walk still finds the update behind every current value;
   (c) the merges dropped the superseded updates. */
FIXTURE(RootMergeDropsSupersededHistory) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler);

    const Base::TUuid repo_id(TUuid::Twister);
    const Base::TUuid idx_id(TUuid::Twister);
    /* Parentless: the root, the only kind of repo whose merges drop history. */
    auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true);
    auto *stepped = dynamic_cast<TSteppedSafeRepo *>(repo.Get());
    if (!EXPECT_TRUE(stepped != nullptr)) {
      std::lock_guard<std::mutex> lock(mut);
      fin = true;
      cond.notify_one();
      return;
    }

    const int64_t num_keys = 100L, num_rounds = 10L, counter = -1L, pair_a = 1000L, pair_b = 1001L;
    auto index_key = [&](int64_t key) {
      return TIndexKey(idx_id, TKey(make_tuple(key), &arena, state_alloc));
    };
    size_t num_commits = 0UL;
    auto commit = [&](const vector<tuple<int64_t, int64_t, TMutator>> &entries) {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
      for (const auto &[key, val, mut] : entries) {
        update->AddEntry(index_key(key), TKey(val, &arena, state_alloc), mut);
      }
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
      ++num_commits;
    };
    /* Every update a view's walker yields, in order. */
    auto walk = [&](const unique_ptr<Orly::Indy::TRepo::TView> &view, TSequenceNumber from) {
      vector<TSequenceNumber> seqs;
      auto walker_ptr = stepped->NewUpdateWalker(view, from);
      for (TUpdateWalker &walker = *walker_ptr; walker; ++walker) {
        seqs.push_back((*walker).SequenceNumber);
      }
      return seqs;
    };
    auto merge_disk = [&]() {
      for (int64_t i = 0L; i < num_rounds * 4L; ++i) {
        stepped->StepMergeDisk(256UL);
      }
    };

    commit({{counter, 100L, TMutator::Assign}});
    unique_ptr<Orly::Indy::TRepo::TView> pinned;
    vector<TSequenceNumber> pinned_seqs;
    for (int64_t round = 1L; round <= num_rounds; ++round) {
      for (int64_t key = 0L; key < num_keys; ++key) {
        commit({{key, round * 1000L + key, TMutator::Assign}});
      }
      for (int64_t i = 0L; i < 3L; ++i) {
        commit({{counter, 1L, TMutator::Add}});
      }
      commit({{pair_a, round, TMutator::Assign}, {pair_b, round, TMutator::Assign}});
      commit({{pair_a, round + 100L, TMutator::Assign}});
      /* this round becomes its own disk file */
      stepped->StepMergeMem();
      if (round == 2L) {
        /* a reader that starts before the merges below */
        pinned = make_unique<Orly::Indy::TRepo::TView>(stepped);
        pinned_seqs = walk(pinned, 1UL);
        EXPECT_GT(pinned_seqs.size(), 0UL);
      }
    }
    merge_disk();

    /* (a) latest values, and the full counter */ {
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      for (int64_t key = 0L; key < num_keys; ++key) {
        EXPECT_EQ(context[index_key(key)], TKey(num_rounds * 1000L + key, &arena, state_alloc));
      }
      EXPECT_EQ(context[index_key(counter)], TKey(100L + 3L * num_rounds, &arena, state_alloc));
      EXPECT_EQ(context[index_key(pair_a)], TKey(num_rounds + 100L, &arena, state_alloc));
      EXPECT_EQ(context[index_key(pair_b)], TKey(num_rounds, &arena, state_alloc));
    }

    /* (b) the pinned view still sees exactly what it saw, the merges notwithstanding */
    const auto pinned_after = walk(pinned, 1UL);
    EXPECT_EQ(pinned_after.size(), pinned_seqs.size());
    EXPECT_TRUE(pinned_after == pinned_seqs);
    pinned.reset();

    /* (b) and (c): a fresh walk has dropped superseded updates, yet still holds the last round
       whole except for its first two `+=` (folded into the third), and nothing out of order */ {
      auto view = make_unique<Orly::Indy::TRepo::TView>(stepped);
      const auto seqs = walk(view, 1UL);
      EXPECT_LT(seqs.size(), num_commits / 2UL);
      const TSequenceNumber last = seqs.empty() ? 0UL : seqs.back();
      EXPECT_EQ(last, num_commits);
      const TSequenceNumber last_round_first = num_commits - (num_keys + 5L) + 1L;
      set<TSequenceNumber> seen(seqs.begin(), seqs.end());
      for (TSequenceNumber seq = last_round_first; seq <= num_commits; ++seq) {
        const bool folded_add = (seq == last_round_first + num_keys || seq == last_round_first + num_keys + 1L);
        if (!folded_add) {
          EXPECT_EQ(seen.count(seq), 1UL);
        }
      }
      for (size_t i = 1UL; i < seqs.size(); ++i) {
        EXPECT_GT(seqs[i], seqs[i - 1]);
      }
    }

    /* (c) keep writing: another batch of rounds must not grow what a walk sees */ {
      auto view = make_unique<Orly::Indy::TRepo::TView>(stepped);
      const size_t before = walk(view, 1UL).size();
      view.reset();
      for (int64_t round = num_rounds + 1L; round <= num_rounds * 2L; ++round) {
        for (int64_t key = 0L; key < num_keys; ++key) {
          commit({{key, round * 1000L + key, TMutator::Assign}});
        }
        stepped->StepMergeMem();
      }
      merge_disk();
      view = make_unique<Orly::Indy::TRepo::TView>(stepped);
      const size_t after = walk(view, 1UL).size();
      /* without pruning this would add num_keys * num_rounds updates */
      EXPECT_LT(after, before + static_cast<size_t>(num_keys * num_rounds / 2L));
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      for (int64_t key = 0L; key < num_keys; ++key) {
        EXPECT_EQ(context[index_key(key)], TKey(num_rounds * 2L * 1000L + key, &arena, state_alloc));
      }
      EXPECT_EQ(context[index_key(counter)], TKey(100L + 3L * num_rounds, &arena, state_alloc));
    }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Shared setup for the fixtures below: a root safe repo on a stub manager, and helpers to
   commit to it, flush its memory layer to a disk file, merge its disk files and read a key. */
class TRootRepoFixture {
  NO_COPY(TRootRepoFixture);
  public:

  explicit TRootRepoFixture(bool prune_merge_history)
      : StateBuf(Sabot::State::GetMaxStateSize()),
        State(StateBuf.data()),
        Engine(&Scheduler, 256, 256, 16384, 1, 1024, 1),
        IdxId(TUuid::Twister) {
    Scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Manager = make_unique<TMyManager>(Engine.GetEngine(), &Scheduler, prune_merge_history);
    Repo = Manager->GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
    Stepped = dynamic_cast<TSteppedSafeRepo *>(Repo.Get());
    assert(Stepped);
  }

  ~TRootRepoFixture() {
    Repo.Reset();
    Manager.reset();
  }

  TIndexKey IndexKey(int64_t key) {
    return TIndexKey(IdxId, TKey(make_tuple(key), &Arena, State));
  }

  void Commit(const vector<tuple<int64_t, int64_t, TMutator>> &entries) {
    auto transaction = Manager->NewTransaction();
    auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&Arena), TKey(Base::TUuid(TUuid::Twister), &Arena, State));
    for (const auto &[key, val, mut] : entries) {
      update->AddEntry(IndexKey(key), TKey(val, &Arena, State), mut);
    }
    transaction->Push(Repo, update);
    transaction->Prepare();
    transaction->CommitAction();
    ++NumCommits;
  }

  void Flush() {
    Stepped->StepMergeMem();
  }

  /* Keep merge outputs unfolded, so reads go through a TMergeDataFile's history (#666). */
  void KeepMergesUnfolded() {
    Stepped->FoldMergedFiles = false;
  }

  void MergeDisk(size_t passes) {
    for (size_t i = 0UL; i < passes; ++i) {
      Stepped->StepMergeDisk(256UL);
    }
  }

  TKey Read(int64_t key) {
    TSuprena ctx_arena;
    TContext context(Repo, &ctx_arena);
    return TKey(&Arena, State, context[IndexKey(key)]);
  }

  TKey Int(int64_t val) {
    return TKey(val, &Arena, State);
  }

  size_t CountWalk() {
    auto view = make_unique<Orly::Indy::TRepo::TView>(Stepped);
    size_t count = 0UL;
    auto walker_ptr = Stepped->NewUpdateWalker(view, 1UL);
    for (TUpdateWalker &walker = *walker_ptr; walker; ++walker) {
      ++count;
    }
    return count;
  }

  /* How many layers the repo's current mapping holds: one per disk file, once flushed. */
  size_t CountLayers() {
    Orly::Indy::TRepo::TView view(Stepped);
    return view.GetNumEntries();
  }

  size_t NumCommits = 0UL;

  private:

  vector<uint8_t> StateBuf;

  void *State;

  TScheduler Scheduler;

  Orly::Indy::Disk::Sim::TMemEngine Engine;

  unique_ptr<TMyManager> Manager;

  L0::TManager::TPtr<L0::TManager::TRepo> Repo;

  TSteppedSafeRepo *Stepped = nullptr;

  TSuprena Arena;

  const Base::TUuid IdxId;

};  // TRootRepoFixture

/* #592 follow-up: a `+=` chain whose Assign base sits in a disk file outside the merge.
   - The base file also holds 100 other keys, which puts it a generation above the two small
     files after it. So StepMergeDisk merges only those two, leaving the base out.
   - The two small files hold two `+= 1` each. The merged chain must keep all four deltas
     for the read to fold onto the base: 10 + 4 = 14. */
FIXTURE(RootMergeFoldsChainOntoBaseOutsideMerge) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(true);
      const int64_t counter = -1L;
      root.Commit({{counter, 10L, TMutator::Assign}});
      for (int64_t key = 0L; key < 100L; ++key) {
        root.Commit({{key, key, TMutator::Assign}});
      }
      root.Flush();
      for (int64_t file = 0L; file < 2L; ++file) {
        root.Commit({{counter, 1L, TMutator::Add}});
        root.Commit({{counter, 1L, TMutator::Add}});
        root.Flush();
      }
      root.MergeDisk(4UL);
      EXPECT_EQ(root.Read(counter), root.Int(14L));
      /* and again after a third small file merges into the merged pair */
      root.Commit({{counter, 1L, TMutator::Add}});
      root.Flush();
      root.MergeDisk(4UL);
      EXPECT_EQ(root.Read(counter), root.Int(15L));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #666: read a `+=` chain straight out of an unfolded disk merge. A merge copies the notes of
   its inputs' arenas into one ordered arena, and readers compare that arena's cores by offset.
   In release builds the inputs' notes carried garbage in their unused header bits, the merge
   kept two copies of the key, and the read stopped folding at the second: 2 instead of 14. */
FIXTURE(UnfoldedMergeReadsItsHistory) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    const int64_t counter = -1L;
    const size_t fallbacks = TKey::GetNumSameArenaFallbacks();
    /* the chain of RootMergeFoldsChainOntoBaseOutsideMerge, base outside the merge */ {
      TRootRepoFixture root(true);
      root.KeepMergesUnfolded();
      root.Commit({{counter, 10L, TMutator::Assign}});
      for (int64_t key = 0L; key < 100L; ++key) {
        root.Commit({{key, key, TMutator::Assign}});
      }
      root.Flush();
      for (int64_t file = 0L; file < 2L; ++file) {
        root.Commit({{counter, 1L, TMutator::Add}});
        root.Commit({{counter, 1L, TMutator::Add}});
        root.Flush();
      }
      root.MergeDisk(4UL);
      EXPECT_EQ(root.Read(counter), root.Int(14L));
      root.Commit({{counter, 1L, TMutator::Add}});
      root.Flush();
      root.MergeDisk(4UL);
      EXPECT_EQ(root.Read(counter), root.Int(15L));
    }
    /* one file per round, base and all, merged together */ {
      TRootRepoFixture root(false);
      root.KeepMergesUnfolded();
      const int64_t num_keys = 100L, num_rounds = 10L;
      root.Commit({{counter, 100L, TMutator::Assign}});
      for (int64_t round = 1L; round <= num_rounds; ++round) {
        for (int64_t key = 0L; key < num_keys; ++key) {
          root.Commit({{key, round * 1000L + key, TMutator::Assign}});
        }
        for (int64_t i = 0L; i < 3L; ++i) {
          root.Commit({{counter, 1L, TMutator::Add}});
        }
        root.Flush();
      }
      root.MergeDisk(static_cast<size_t>(num_rounds * 4L));
      EXPECT_EQ(root.Read(counter), root.Int(100L + 3L * num_rounds));
      for (int64_t key = 0L; key < num_keys; ++key) {
        EXPECT_EQ(root.Read(key), root.Int(num_rounds * 1000L + key));
      }
    }
    /* without duplicate notes, no read needed to compare values in full (#674) */
    EXPECT_EQ(TKey::GetNumSameArenaFallbacks(), fallbacks);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #674: a release merge from before #666 could write two copies of one key's note into its output
   arena. The arena is ordered, so a reader compares two of its cores by offset, and took the copies
   for two keys. Build such a file with the merge's test knob and read a `+=` chain out of it: the
   fold stopped at the first entry that named the other copy, and the counter read 2, not 14. */
FIXTURE(DuplicateNotesReadAsOneKey) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    const int64_t counter = -1L;
    Disk::TMergeDataFile::KeepsEqualNotes = true;
    const size_t fallbacks = TKey::GetNumSameArenaFallbacks();
    /* the chain of UnfoldedMergeReadsItsHistory, every note kept */ {
      TRootRepoFixture root(true);
      root.KeepMergesUnfolded();
      root.Commit({{counter, 10L, TMutator::Assign}});
      for (int64_t key = 0L; key < 100L; ++key) {
        root.Commit({{key, key, TMutator::Assign}});
      }
      root.Flush();
      for (int64_t file = 0L; file < 2L; ++file) {
        root.Commit({{counter, 1L, TMutator::Add}});
        root.Commit({{counter, 1L, TMutator::Add}});
        root.Flush();
      }
      root.MergeDisk(4UL);
      EXPECT_EQ(root.Read(counter), root.Int(14L));
      for (int64_t key = 0L; key < 100L; ++key) {
        EXPECT_EQ(root.Read(key), root.Int(key));
      }
      /* and once more, with the merged file itself an input */
      root.Commit({{counter, 1L, TMutator::Add}});
      root.Flush();
      root.MergeDisk(4UL);
      EXPECT_EQ(root.Read(counter), root.Int(15L));
    }
    Disk::TMergeDataFile::KeepsEqualNotes = false;
    /* the reads above went through the fallback */
    EXPECT_GT(TKey::GetNumSameArenaFallbacks(), fallbacks);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #592 follow-up: --prune_merge_history=false keeps every update through the same merges that
   otherwise drop the superseded ones. */
FIXTURE(PruneMergeHistoryFlag) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    for (bool prune : {true, false}) {
      TRootRepoFixture root(prune);
      for (int64_t round = 1L; round <= 8L; ++round) {
        for (int64_t key = 0L; key < 20L; ++key) {
          root.Commit({{key, round * 100L + key, TMutator::Assign}});
        }
        root.Flush();
      }
      root.MergeDisk(16UL);
      for (int64_t key = 0L; key < 20L; ++key) {
        EXPECT_EQ(root.Read(key), root.Int(800L + key));
      }
      const size_t walked = root.CountWalk();
      if (prune) {
        EXPECT_LT(walked, root.NumCommits / 2UL);
      } else {
        EXPECT_EQ(walked, root.NumCommits);
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #614: a repo must outlive a merge that is running on it. The merge runners take a repo off the
   queue as a raw pointer, and a ttl-0 pov's repo is destroyed the moment its last pin goes, as
   happens when Tetris releases its last update. That used to free the repo under a running
   StepMergeMem (an assert in its entry loop in debug, a crash or worse in release). Here a merge
   parks inside StepMergeMem while the test drops the last pin it holds: the repo must stay live
   until the merge is done, then go. */
FIXTURE(RepoOutlivesItsRunningMerge) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler);
    const Base::TUuid repo_id(TUuid::Twister);
    /* a ttl-0 fast repo: destroyed as soon as nothing pins it */
    auto repo = manager->GetRepo(repo_id, TTtl::zero(), std::nullopt, false);
    auto *latched = dynamic_cast<TLatchedFastRepo *>(repo.Get());
    if (EXPECT_TRUE(latched != nullptr)) {
      latched->QueueMergeMem();
      /* drain the merge queue on another thread, as a merge runner would */
      std::thread merger([&manager] { manager->FlushMemMerges(); });
      /* wait for the merge to park inside StepMergeMem */ {
        std::unique_lock<std::mutex> lock(MergeLatch.Mutex);
        MergeLatch.Cond.wait(lock, [] { return MergeLatch.Entered; });
      }
      /* drop the test's pin, the last one but the merge's */
      repo.Reset();
      EXPECT_TRUE(static_cast<bool>(manager->TryOpenLiveRepo(repo_id)));
      /* let the merge finish */ {
        std::lock_guard<std::mutex> lock(MergeLatch.Mutex);
        MergeLatch.Released = true;
        MergeLatch.Cond.notify_all();
      }
      merger.join();
      /* with the merge done and nothing else pinning it, the repo is gone */
      EXPECT_FALSE(static_cast<bool>(manager->TryOpenLiveRepo(repo_id)));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #661: a repo in the cache keeps its parent open.
   (a) A child with a ttl is closed, so it goes into the cache, and is opened again by id, as a
       slave opens a replicated repo for each batch. It must still reach its parent: a read of a
       key only the parent has falls through to it (context.cc), and promotion joins the parent's
       Tetris player. Caching used to null the parent pointer and leave the optional engaged, so
       both dereferenced null.
   (b) A ttl-0 parent with a cached child stays live until the cache discards the child, and goes
       when it does: the discarded child's reference is released, not leaked. Teardown then has to
       sweep cached children before the parents they pin. */
FIXTURE(CachedRepoKeepsItsParent) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Orly::Indy::Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      /* A cache of 4 repos: every repo takes a memory layer, and this test's pool has 100. */
      TMyManager manager(engine.GetEngine(), &scheduler, true, 4UL);
      TSuprena arena;
      const Base::TUuid idx_id(TUuid::Twister);
      const Base::TUuid root_id(TUuid::Twister), child_id(TUuid::Twister), ttl0_parent_id(TUuid::Twister), ttl0_child_id(TUuid::Twister);
      auto root = manager.GetRepo(root_id, TTtl::max(), std::nullopt, true);
      /* extra */ {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(TIndexKey(idx_id, TKey(make_tuple(int64_t(7)), &arena, state)), TKey(int64_t(77), &arena, state), TMutator::Assign);
        transaction->Push(root, update);
        transaction->Prepare();
        transaction->CommitAction();
      }

      /* (a) */
      manager.GetRepo(child_id, TTtl(3600s), root, true);  // dropped at once: closed, so cached
      /* extra */ {
        auto child = manager.TryOpenLiveRepo(child_id);
        EXPECT_TRUE(static_cast<bool>(child));
        if (child) {
          const auto &parent = child->GetParentRepo();
          EXPECT_TRUE(static_cast<bool>(parent));
          const bool live = parent && static_cast<bool>(*parent);
          EXPECT_TRUE(live);
          if (live) {
            EXPECT_TRUE((*parent)->GetId() == root_id);
            TSuprena ctx_arena;
            TContext context(child, &ctx_arena);
            EXPECT_EQ(TKey(&arena, state, context[TIndexKey(idx_id, TKey(make_tuple(int64_t(7)), &arena, state))]),
                      TKey(int64_t(77), &arena, state));
          }
        }
      }

      /* (b) */ {
        auto ttl0_parent = manager.GetRepo(ttl0_parent_id, TTtl(0s), std::nullopt, true);
        manager.GetRepo(ttl0_child_id, TTtl(3600s), ttl0_parent, true);  // cached
      }
      /* Only the cached child holds the ttl-0 parent now. */
      EXPECT_TRUE(static_cast<bool>(manager.TryOpenLiveRepo(ttl0_parent_id)));
      /* Fill the cache: the two children above have the soonest deadlines, so they go first. */
      for (size_t i = 0; i < 4UL; ++i) {
        manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl(3600s), root, true);
      }
      EXPECT_FALSE(static_cast<bool>(manager.TryOpenLiveRepo(ttl0_child_id)));
      EXPECT_FALSE(static_cast<bool>(manager.TryOpenLiveRepo(ttl0_parent_id)));
      /* Teardown: the root is still held by the cached fillers. */
      root.Reset();
      EXPECT_TRUE(static_cast<bool>(manager.TryOpenLiveRepo(root_id)));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Takes every free block of a pool, through the class's own operator new, until it throws, and
   gives them all back when released. On a fiber the pools fail at once (#607), so this doesn't
   wait. */
class TPoolHog {
  NO_COPY(TPoolHog);
  public:

  TPoolHog(const function<void *()> &alloc, const function<void (void *)> &free)
      : Free(free) {
    for (;;) {
      try {
        Blocks.push_back(alloc());
      } catch (const bad_alloc &) {
        break;
      }
    }
  }

  ~TPoolHog() {
    Release();
  }

  size_t GetSize() const {
    return Blocks.size();
  }

  void Release() {
    for (void *block : Blocks) {
      Free(block);
    }
    Blocks.clear();
  }

  private:

  function<void (void *)> Free;

  vector<void *> Blocks;

};  // TPoolHog

/* #627: a disk merge that runs out of pool space must not abort the server. Each fixture below
   writes two small disk files whose merge has a `+=` to fold, takes every free block of one
   pool, and steps the disk merge. The merge must leave the repo readable and correct, and must
   go through once the pool has room again. */
class TDiskMergePoolFixture {
  NO_COPY(TDiskMergePoolFixture);
  public:

  static constexpr int64_t Counter = -1L;

  TDiskMergePoolFixture()
      : Root(true) {
    Root.Commit({{Counter, 10L, TMutator::Assign}});
    for (int64_t key = 0L; key < 10L; ++key) {
      Root.Commit({{key, key, TMutator::Assign}});
    }
    Root.Flush();
    AddFile();
  }

  /* One more disk file, holding two `+= 1` on the counter. */
  void AddFile() {
    Root.Commit({{Counter, 1L, TMutator::Add}});
    Root.Commit({{Counter, 1L, TMutator::Add}});
    Root.Flush();
    Expected += 2L;
  }

  size_t CountLayers() {
    return Root.CountLayers();
  }

  bool ReadsCorrectly() {
    bool ok = EXPECT_EQ(Root.Read(Counter), Root.Int(Expected));
    for (int64_t key = 0L; key < 10L; ++key) {
      ok = EXPECT_EQ(Root.Read(key), Root.Int(key)) && ok;
    }
    return ok;
  }

  TRootRepoFixture Root;

  int64_t Expected = 10L;

};  // TDiskMergePoolFixture

/* The fold step builds a memory layer of updates from the Update pool. */
FIXTURE(DiskMergeWithUpdatePoolFull) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TDiskMergePoolFixture fixture;
      EXPECT_EQ(fixture.CountLayers(), 2UL);
      {
        TPoolHog hog([] { return TUpdate::operator new(sizeof(TUpdate)); },
                     [](void *ptr) { TUpdate::operator delete(ptr, sizeof(TUpdate)); });
        EXPECT_GT(hog.GetSize(), 0UL);
        fixture.Root.MergeDisk(4UL);
      }
      EXPECT_TRUE(fixture.ReadsCorrectly());
      /* A fold is an optimisation: with no room for it, the merge keeps its unfolded output. */
      EXPECT_EQ(fixture.CountLayers(), 1UL);
      /* and a later merge, with room, folds as usual */
      fixture.AddFile();
      fixture.Root.MergeDisk(4UL);
      EXPECT_EQ(fixture.CountLayers(), 1UL);
      EXPECT_TRUE(fixture.ReadsCorrectly());
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* The merged file's disk layer comes from the Data Layer pool. */
FIXTURE(DiskMergeWithDataLayerPoolFull) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TDiskMergePoolFixture fixture;
      {
        TPoolHog hog([] { return TDiskLayer::operator new(sizeof(TDiskLayer)); },
                     [](void *ptr) { TDiskLayer::operator delete(ptr, sizeof(TDiskLayer)); });
        EXPECT_GT(hog.GetSize(), 0UL);
        fixture.Root.MergeDisk(4UL);
        /* handed back, not merged */
        EXPECT_EQ(fixture.CountLayers(), 2UL);
      }
      EXPECT_TRUE(fixture.ReadsCorrectly());
      fixture.Root.MergeDisk(4UL);
      EXPECT_EQ(fixture.CountLayers(), 1UL);
      EXPECT_TRUE(fixture.ReadsCorrectly());
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Publishing the merge builds a new mapping, from the mapping and mapping entry pools. By then
   the merged file is written, so the merge must undo the half-built mapping and drop the file. */
FIXTURE(DiskMergeWithMappingPoolsFull) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      using TMapping = TSteppedSafeRepo::TMappingType;
      TDiskMergePoolFixture fixture;
      for (bool entries : {false, true}) {
        {
          TPoolHog hog(entries
                           ? function<void *()>([] { return TMapping::TEntry::operator new(sizeof(TMapping::TEntry)); })
                           : function<void *()>([] { return TMapping::operator new(sizeof(TMapping)); }),
                       entries
                           ? function<void (void *)>([](void *ptr) { TMapping::TEntry::operator delete(ptr, sizeof(TMapping::TEntry)); })
                           : function<void (void *)>([](void *ptr) { TMapping::operator delete(ptr, sizeof(TMapping)); }));
          EXPECT_GT(hog.GetSize(), 0UL);
          fixture.Root.MergeDisk(4UL);
          EXPECT_EQ(fixture.CountLayers(), 2UL);
        }
        EXPECT_TRUE(fixture.ReadsCorrectly());
        fixture.Root.MergeDisk(4UL);
        EXPECT_EQ(fixture.CountLayers(), 1UL);
        EXPECT_TRUE(fixture.ReadsCorrectly());
        fixture.AddFile();
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
