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

#include <base/scheduler.h>
#include <orly/indy/context.h>
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

};  // TSteppedSafeRepo

/* A minimal L1::TManager that builds stepped safe repos. Its merge delays are an hour, so the
   background merge loops run each repo's merges at most once, right when they are first queued;
   the test drives every merge after that. */
class TMyManager
    : public L1::TManager {
  NO_COPY(TMyManager);
  public:

  TMyManager(Disk::Util::TEngine *engine, Base::TScheduler *scheduler)
      : TManager(engine, 1h, 1h, true, true, 1000ms, scheduler,
                 100UL, 100UL, 20UL, MemMergeCoreVec, DiskMergeCoreVec, true) {}

  /* The sweeps Indy::TManager runs in its destructor. They also remove the files that merges
     retired, which needs ForEachScheduler: ~TManager's own drain runs after this object is gone. */
  virtual ~TMyManager() {
    ReleaseDirtySelfPins();
    CloseAllUnreferencedObjects();
  }

  virtual TRepo *ConstructRepo(const Base::TUuid &repo_id,
                               const std::optional<TTtl> &ttl,
                               const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                               bool /*is_safe*/,
                               bool /*create*/) override {
    return new TSteppedSafeRepo(this, repo_id, *ttl, parent_repo);
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
