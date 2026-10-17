/* <orly/indy/transaction_base.test.cc>

   Unit test for <orly/indy/transaction_base.h>.

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

#include <orly/indy/transaction_base.h>
#include <dirent.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include <base/scheduler.h>
#include <orly/indy/disk/sim/fault_device.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/disk/wal.h>
#include <orly/indy/fiber/fiber_test_runner.h>
#include <orly/indy/repo.h>
#include <orly/indy/wal_retention.h>
#include <orly/server/tetris_manager.h>

#include <base/test/kit.h>

using namespace std;
using namespace std::literals;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 100UL);

Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(max(max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation", 100UL);
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction", 100UL);

/* Blocks for the #665 fixtures' safe root, which writes its memory layers to disk files. */
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(Disk::Util::PhysicalBlockSize, 256UL);

/* Sized for Issue636PauseMidRound and the #657 fixtures, whose parents keep every update they
   are promoted: this harness latches no merge runner, so nothing ever leaves a memory layer. */
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 20000UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 40000UL);

const std::vector<size_t> MemMergeCoreVec{0};
const std::vector<size_t> DiskMergeCoreVec{0};

class TMyManager
    : public L1::TManager {
  NO_COPY(TMyManager);
  public:

  TMyManager(Disk::Util::TEngine *engine,
             Base::TScheduler *scheduler,
             const std::vector<size_t> &mem_merge_cores,
             const std::vector<size_t> &disk_merge_cores)
      : TManager(engine,
                 10ms,
                 100ms,
                 true,
                 true,
                 true,
                 1000ms,
                 scheduler,
                 100UL,
                 100UL,
                 20UL,
                 mem_merge_cores,
                 disk_merge_cores,
                 true) {}

  virtual ~TMyManager() {}

  virtual TRepo *ConstructRepo(const Base::TUuid &repo_id,
                                     const std::optional<TTtl> &ttl,
                                     const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                                     bool is_safe,
                                     bool /*create*/) override {
    return is_safe ?
      static_cast<TRepo *>(new TSafeRepo(this, repo_id, *ttl, parent_repo))
    : static_cast<TRepo *>(new TFastRepo(this, repo_id, *ttl, parent_repo));
  }

  virtual void SaveRepo(Orly::Indy::L0::TManager::TRepo *) override {}

  virtual void Enqueue(Orly::Indy::TTransactionReplication *, Orly::Indy::L1::TTransaction::TReplica &&) NO_THROW override {}

  virtual Orly::Indy::TTransactionReplication* NewTransactionReplication() override {
    return nullptr;
  }

  virtual void DeleteTransactionReplication(Orly::Indy::TTransactionReplication*) NO_THROW override {}

  virtual void ForEachScheduler(const std::function<bool (Fiber::TRunner *)> &/*cb*/) const override {}

  virtual bool CanLoad(const L0::TId &/*id*/) override {
    return true;
  }

  virtual void Delete(const L0::TId &/*id*/, L0::TSem */*sem*/) override {}

  virtual void Save(const L0::TId &/*id*/, const L0::TDeadline &/*deadline*/, const std::string &/*blob*/, L0::TSem */*sem*/) override {}

  virtual bool TryLoad(const L0::TId &/*id*/, std::string &/*blob*/) override {
    return true;
  }

  virtual TRepo *ReconstructRepo(const Base::TUuid &/*repo_id*/) override {
    return nullptr;
  }

  virtual void RunReplicationQueue() override {}

  virtual void RunReplicationWork() override {}

  virtual void RunReplicateTransaction() override {}

  virtual std::mutex &GetReplicationQueueLock() NO_THROW override {
    return ReplicationQueueLock;
  }

  inline TManager::TPtr<Indy::TRepo> GetRepo(const Base::TUuid &repo_id,
                                                 const std::optional<TTtl> &ttl,
                                                 const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                                                 bool is_safe,
                                                 bool create) {
    return create ? OpenOrCreate(repo_id, ttl, parent_repo, is_safe) : ForceOpenRepo(repo_id);
  }

  /* The teardown sweeps Indy::TManager runs in its destructor, exposed so a
     fixture can drive the manager-discards-a-dirty-repo path (#521). */
  void TearDownRepos() {
    ReleaseDirtySelfPins();
    CloseAllUnreferencedObjects();
  }

  using TManager::OpenOrCreate;
  using L0::TManager::TryOpenLiveRepo;
  using L1::TManager::SetWal;

  private:

  std::mutex ReplicationQueueLock;

};

FIXTURE(Typical) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler,
                                                 256 /* fast disk space: 256MB */,
                                                 64 /* slow disk space: 64MB */,
                                                 128 /* page cache slots: 8MB */,
                                                 1 /* num page lru */,
                                                 64 /* block cache slots: 4MB */,
                                                 1 /* num block lru */);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid repo_1_id(TUuid::Twister);
    Base::TUuid idx_id(TUuid::Twister);
    auto repo_1 = manager->GetRepo(repo_1_id, TTtl::max(), std::nullopt, false, true);
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* don't commit Push */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(repo_1, update);
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* check that nothing is there */ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* commit Push */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(repo_1, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* check that our update is there */ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(static_cast<bool>(walker))) {
        EXPECT_EQ((*walker).SequenceNumber, 1UL);
        ++walker;
      }
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* don't commit Pop */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Pop(repo_1);
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* check that our update is there */ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(static_cast<bool>(walker))) {
        EXPECT_EQ((*walker).SequenceNumber, 1UL);
        ++walker;
      }
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* commit Pop */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Pop(repo_1);
      transaction->Prepare();
      transaction->CommitAction();
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* check that nothing is there */ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* don't commit Pause */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Pause(repo_1);
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* commit Pause */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Pause(repo_1);
      transaction->Prepare();
      transaction->CommitAction();
    }
    EXPECT_EQ(repo_1->GetStatus(), Paused);
    /* don't commit UnPause */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->UnPause(repo_1);
    }
    EXPECT_EQ(repo_1->GetStatus(), Paused);
    /* commit UnPause */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->UnPause(repo_1);
      transaction->Prepare();
      transaction->CommitAction();
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* don't commit Fail */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Fail(repo_1);
    }
    EXPECT_EQ(repo_1->GetStatus(), Normal);
    /* commit Fail */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Fail(repo_1);
      transaction->Prepare();
      transaction->CommitAction();
    }
    EXPECT_EQ(repo_1->GetStatus(), Failed);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(Promoter) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler,
                                                 256 /* fast disk space: 256MB */,
                                                 64 /* slow disk space: 64MB */,
                                                 128 /* page cache slots: 8MB */,
                                                 1 /* num page lru */,
                                                 64 /* block cache slots: 4MB */,
                                                 1 /* num block lru */);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid repo_1_id(TUuid::Twister);
    Base::TUuid repo_2_id(TUuid::Twister);
    Base::TUuid idx_id(TUuid::Twister);
    auto repo_1 = manager->GetRepo(repo_1_id, TTtl::max(), std::nullopt, false, true);
    auto repo_2 = manager->GetRepo(repo_2_id, TTtl::max(), std::nullopt, false, true);
    /* Push to 1*/ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(repo_1, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* check that repo 1 has the update */ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(static_cast<bool>(walker))) {
        EXPECT_EQ((*walker).SequenceNumber, 1UL);
        ++walker;
      }
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    /* pop from 1, push to 2 */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Pop(repo_1);
      transaction->Push(repo_2, transaction->Peek(repo_1));
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* check that repo 2 has the update */ {
      auto view = make_unique<TRepo::TView>(repo_2);
      auto walker_ptr = repo_2->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(static_cast<bool>(walker))) {
        EXPECT_EQ((*walker).SequenceNumber, 1UL);
        ++walker;
      }
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    /* check that nothing is in repo 1*/ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Issue #143: a child repo's promotion to its parent (Tetris push-to-parent
   + pop-from-child) must never present a "neither" transient to a reader that
   spans child + parent (which is what a shared-POV read does, since the
   shared POV's repo is a child of the global repo). At every instant during
   commit the in-flight update must be visible in at least one of the two
   repos -- otherwise a concurrent read drops it (the agent-swarm symptom).

   We make the window deterministic with the test-only commit hook that fires
   between the pusher-apply pass and the popper-apply pass. From inside the
   window we present-walk the child snapshot then the parent snapshot (the
   same child-first union TContext performs) and assert the key is present in
   at least one of them -- never dropped from both. */
FIXTURE(Issue143PromotionWindow) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler,
                                                 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid global_id(TUuid::Twister);
    Base::TUuid child_id(TUuid::Twister);
    Base::TUuid idx_id(TUuid::Twister);

    /* global repo (the promotion target / parent) and a child repo. In
       production the child's repo is constructed with ParentRepo = global so
       it auto-registers with the Tetris manager; here we drive the
       child->parent promotion by hand (as TRepoTetrisManager::Play does) and
       perform the cross-repo read by hand too, so we leave the child
       parent-less to avoid pulling in a live Tetris player fiber. The
       cross-repo read invariant under test is identical: a reader that unions
       the child snapshot with the global snapshot must never see "neither". */
    auto global_repo = manager->GetRepo(global_id, TTtl::max(), std::nullopt, false, true);
    auto child_repo = manager->GetRepo(child_id, TTtl::max(), std::nullopt, false, true);

    const TIndexKey from_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));
    const TIndexKey to_key(idx_id, TKey(make_tuple(10L), &arena, state_alloc));

    /* A reader that spans child -> parent finds the key iff a present-walk on
       either repo's snapshot is valid. This mirrors TContext, which snapshots
       the child view first, then each parent view (context.cc ctor), and
       unions them. A "drop" is: found in NEITHER. */
    auto found_in_child_or_parent = [&]() -> bool {
      /* child snapshot first (matches TContext ctor order) */
      auto child_view = make_unique<TRepo::TView>(child_repo);
      auto child_walker_ptr = child_repo->NewPresentWalker(child_view, from_key, to_key);
      bool in_child = static_cast<bool>(*child_walker_ptr);
      /* parent snapshot second */
      auto parent_view = make_unique<TRepo::TView>(global_repo);
      auto parent_walker_ptr = global_repo->NewPresentWalker(parent_view, from_key, to_key);
      bool in_parent = static_cast<bool>(*parent_walker_ptr);
      return in_child || in_parent;
    };

    /* Push an update into the child. */
    /* commit push to child */ {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { from_key, TKey(7L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(child_repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    EXPECT_TRUE(found_in_child_or_parent());

    /* Install the test-only hook: fires inside the commit, between the
       push-to-parent apply and the pop-from-child apply. */
    bool window_saw_value = false;
    L1::TTransaction::OnCommitBetweenPushAndPopForTest = [&]() {
      window_saw_value = found_in_child_or_parent();
    };

    /* Promote: pop the lowest from the child and push it to the parent, all
       in one transaction (this is what TRepoTetrisManager::Play registers). */
    /* commit promotion */ {
      auto transaction = manager->NewTransaction();
      transaction->Pop(child_repo);
      transaction->Push(global_repo, transaction->Peek(child_repo));
      transaction->Prepare();
      transaction->CommitAction();
    }  // <-- ~TTransaction here applies push (parent), fires hook, then pop (child)

    L1::TTransaction::OnCommitBetweenPushAndPopForTest = nullptr;

    /* The whole point: mid-promotion the value must be observable in at least
       one repo -- never dropped from both. */
    EXPECT_TRUE(window_saw_value);

    /* And after promotion the value lives in the parent and is still found. */
    EXPECT_TRUE(found_in_child_or_parent());

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Issue #172: a Pop/Fail that lands on an already-attached popper with a stale
   ensure_or_discard -- the repo's sequence start has advanced past the requested
   point -- must be *discarded* (return false, leave the popper's state untouched),
   not crash with the old placeholder runtime_error ("check behavior"). We attach a popper
   in each
   reachable state (Pop / Fail / Peek) and then issue the mismatching call. The repo
   holds one committed update (sequence start 1), so passing ensure/follow = 0 never
   matches and exercises the discard branch with the start-aware assert satisfied
   (start 1 >= 0). Before the fix each of these threw; here they must return false. */
FIXTURE(Issue172StaleDiscard) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid repo_id(TUuid::Twister);
    Base::TUuid idx_id(TUuid::Twister);
    auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, false, true);

    /* commit one update so the repo has a sequence start of 1 */ {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }

    const std::optional<TSequenceNumber> stale(0);  // start is 1, so 0 never matches -> discard

    /* Pop reaching an existing Pop-state popper (was: throw at transaction_base.cc Pop/Pop) */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction->Pop(repo));          // attach popper in Pop
      EXPECT_FALSE(transaction->Pop(repo, stale));   // stale -> discard, must not throw
    }
    /* Pop reaching an existing Fail-state popper (was: throw at Pop/Fail) */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction->Fail(repo));         // attach popper in Fail
      EXPECT_FALSE(transaction->Pop(repo, stale));
    }
    /* Fail reaching an existing Peek-state popper (was: throw at Fail/Peek) */ {
      auto transaction = manager->NewTransaction();
      transaction->Peek(repo);                      // attach popper in Peek
      EXPECT_FALSE(transaction->Fail(repo, stale));
    }
    /* Fail reaching an existing Pop-state popper (was: throw at Fail/Pop) */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction->Pop(repo));          // attach popper in Pop
      EXPECT_FALSE(transaction->Fail(repo, stale));
    }

    /* None of the discarded ops were committed, so the repo is untouched: still
       Normal, with the original update still present. */
    EXPECT_EQ(repo->GetStatus(), Normal);
    {
      auto view = make_unique<TRepo::TView>(repo);
      auto walker_ptr = repo->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      EXPECT_TRUE(static_cast<bool>(walker));
    }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #521: a zero-ttl fast repo discarded with an update still in its current
   memory layer must not trip the ~TRepo lifecycle assert.  This is the pov
   shape from orly/server: a ttl=0 fast private pov commits a write whose
   lifecycle never finishes (in production, parked on a replication stall),
   the owner disconnects, and the repo -- kept alive only by its MakeDirty()
   self-pin -- is finally discarded by the manager's teardown sweeps.  That
   discard is sanctioned (expiry of an unsafe pov drops unmerged data by
   contract); before the fix, ReleaseDirtySelfPins() aborted the whole
   process on assert(CurMemoryLayer->IsEmpty()). */
FIXTURE(Issue521SanctionedDiscard) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid repo_id(TUuid::Twister);
    Base::TUuid idx_id(TUuid::Twister);
    /* Commit one update to a zero-ttl fast repo, then drop every external
       ptr.  The update is never released and no merge runner is latched in
       this harness, so it stays in CurMemoryLayer and the MakeDirty()
       self-pin is all that keeps the repo open. */ {
      auto repo = manager->GetRepo(repo_id, TTtl(0), std::nullopt, false, true);
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* The sweep drops the self-pin; the zero-ttl repo closes and the manager
       destroys it with the unmerged update still aboard -- a sanctioned
       discard that must complete without tripping ~TRepo's asserts. */
    manager->TearDownRepos();
    EXPECT_FALSE(static_cast<bool>(manager->TryOpenLiveRepo(repo_id)));
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(DiskPromoter) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(4, 4, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);


    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler,
                                                 256 /* fast disk space: 256MB */,
                                                 64 /* slow disk space: 64MB */,
                                                 128 /* page cache slots: 8MB */,
                                                 1 /* num page lru */,
                                                 64 /* block cache slots: 4MB */,
                                                 1 /* num block lru */);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid repo_1_id(TUuid::Twister);
    Base::TUuid repo_2_id(TUuid::Twister);
    Base::TUuid idx_id(TUuid::Twister);
    auto repo_1 = manager->GetRepo(repo_1_id, TTtl::max(), std::nullopt, true, true);
    auto repo_2 = manager->GetRepo(repo_2_id, TTtl::max(), std::nullopt, true, true);
    /* Push to 1*/ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
      transaction->Push(repo_1, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* check that repo 1 has the update */ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(static_cast<bool>(walker))) {
        EXPECT_EQ((*walker).SequenceNumber, 1UL);
        ++walker;
      }
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    /* pop from 1, push to 2 */ {
      auto transaction = manager->NewTransaction();
      EXPECT_TRUE(transaction);
      transaction->Pop(repo_1);
      transaction->Push(repo_2, transaction->Peek(repo_1));
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* check that repo 2 has the update */ {
      auto view = make_unique<TRepo::TView>(repo_2);
      auto walker_ptr = repo_2->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      if (EXPECT_TRUE(static_cast<bool>(walker))) {
        EXPECT_EQ((*walker).SequenceNumber, 1UL);
        ++walker;
      }
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    /* check that nothing is in repo 1*/ {
      auto view = make_unique<TRepo::TView>(repo_1);
      auto walker_ptr = repo_1->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(10L), &arena, state_alloc)));
      auto &walker = *walker_ptr;
      EXPECT_FALSE(static_cast<bool>(walker));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
/* A Tetris manager whose players promote the way TRepoTetrisManager's do -- each round, for each
   child with a backlog: Peek its lowest update, Push it to the parent and Pop it from the child,
   all in one transaction -- minus the session metadata and package lookups the real player needs
   to test assertions.  That is enough to drive a real TRepo's Join/Part/ChangeStatus, which is
   what pause and unpause exercise. */
class TPromotingTetrisManager final
    : public Orly::Server::TTetrisManager {
  NO_COPY(TPromotingTetrisManager);
  public:

  TPromotingTetrisManager(TScheduler *scheduler,
                          Fiber::TRunner::TRunnerCons &runner_cons,
                          Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager,
                          TMyManager *repo_manager)
      : Orly::Server::TTetrisManager(scheduler, runner_cons, frame_pool_manager, [](Fiber::TRunner *) {}, true),
        RepoManager(repo_manager) {}

  virtual ~TPromotingTetrisManager() {
    StopAllPlayers();
  }

  /* Updates promoted from a child to its parent, across all players. */
  std::atomic<size_t> PromotionCount{0UL};

  /* If set, a player calls this between peeking a child and committing its promotion. */
  std::function<void ()> OnPeeked;

  private:

  class TPlayer final
      : public Orly::Server::TTetrisManager::TPlayer {
    NO_COPY(TPlayer);
    public:

    TPlayer(TPromotingTetrisManager *manager, const TUuid &parent_id, const TUuid &child_id, bool is_paused, bool is_master)
        : Orly::Server::TTetrisManager::TPlayer(manager), Manager(manager),
          Parent(manager->RepoManager->GetRepo(parent_id, std::nullopt, std::nullopt, false, false)) {
      OnJoin(child_id);
      Start(is_paused, is_master);
    }

    private:

    virtual void OnJoin(const TUuid &child_id) override {
      std::lock_guard<std::mutex> lock(Mutex);
      Children.emplace(child_id, Manager->RepoManager->GetRepo(child_id, std::nullopt, std::nullopt, false, false));
    }

    virtual void OnPart(const TUuid &child_id) override {
      std::lock_guard<std::mutex> lock(Mutex);
      Children.erase(child_id);
    }

    virtual void OnPause() override {}

    virtual void OnUnpause() override {}

    virtual void Play() override {
      std::vector<L0::TManager::TPtr<Indy::TRepo>> children;
      /* extra */ {
        std::lock_guard<std::mutex> lock(Mutex);
        for (const auto &item: Children) {
          children.push_back(item.second);
        }
      }
      for (const auto &child: children) {
        if (!child->GetMemBacklogDepth()) {
          continue;
        }
        auto transaction = Manager->RepoManager->NewTransaction();
        auto update = transaction->Peek(child);
        if (!update || child->GetStatus() != Normal) {
          continue;
        }
        if (Manager->OnPeeked) {
          Manager->OnPeeked();
        }
        transaction->Push(Parent, update);
        transaction->Pop(child);
        transaction->Prepare();
        transaction->CommitAction();
        transaction.reset();
        ++(Manager->PromotionCount);
      }
    }

    TPromotingTetrisManager *Manager;

    L0::TManager::TPtr<Indy::TRepo> Parent;

    std::mutex Mutex;

    std::unordered_map<TUuid, L0::TManager::TPtr<Indy::TRepo>> Children;

  };  // TPromotingTetrisManager::TPlayer

  virtual Orly::Server::TTetrisManager::TPlayer *NewPlayer(const TUuid &parent_id, const TUuid &child_id, bool is_paused, bool is_master) override {
    return new TPlayer(this, parent_id, child_id, is_paused, is_master);
  }

  TMyManager *RepoManager;

};  // TPromotingTetrisManager

/* Commit one single-key update to `repo`. */
static void PushOne(TMyManager *manager, const L0::TManager::TPtr<Indy::TRepo> &repo, const TUuid &idx_id, int64_t key) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto transaction = manager->NewTransaction();
  auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(key), &arena, state_alloc)), TKey(key * 10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
  transaction->Push(repo, update);
  transaction->Prepare();
  transaction->CommitAction();
}

/* Commit a pause (or an unpause) of `repo`. */
static void SetPaused(TMyManager *manager, const L0::TManager::TPtr<Indy::TRepo> &repo, bool paused) {
  auto transaction = manager->NewTransaction();
  if (paused) {
    transaction->Pause(repo);
  } else {
    transaction->UnPause(repo);
  }
  transaction->Prepare();
  transaction->CommitAction();
}

/* Wait up to `timeout` for `pred`, polling.  True iff it came true. */
static bool WaitFor(const std::function<bool ()> &pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

/* #635: writes made while a pov is paused must reach its parent once it's unpaused, without any
   further write.  ChangeStatus(Normal) rejoined the parent's Tetris only when the repo had NO
   pending updates -- inverted -- and AppendUpdate never joins a paused repo, so nothing woke
   Tetris for them until the next write. */
FIXTURE(Issue635UnpausePromotesPausedWrites) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      const size_t write_count = 3UL;
      int64_t key = 0;
      for (size_t cycle = 0; cycle < 3UL; ++cycle) {
        auto child = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, false, true);
        const size_t promoted_before = tetris.PromotionCount;
        SetPaused(manager.get(), child, true);
        for (size_t i = 0; i < write_count; ++i) {
          PushOne(manager.get(), child, idx_id, ++key);
        }
        /* Paused: nothing moves. */
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(child->GetMemBacklogDepth(), write_count);
        SetPaused(manager.get(), child, false);
        /* Promotion lands in milliseconds once Tetris knows about it; allow 10 s. */
        bool promoted = WaitFor([&] {
          return !child->GetMemBacklogDepth() && tetris.PromotionCount == promoted_before + write_count;
        }, 10s);
        EXPECT_TRUE(promoted);
        if (!promoted) {
          /* The pre-fix workaround: one more write joins the child and promotes everything, so the
             fixture can tear down with no repo still holding unpromoted updates. */
          PushOne(manager.get(), child, idx_id, ++key);
          WaitFor([&] { return !child->GetMemBacklogDepth(); }, 10s);
        }
        EXPECT_EQ(child->GetMemBacklogDepth(), 0UL);
      }
      EXPECT_EQ(parent->GetMemBacklogDepth(), static_cast<size_t>(key));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #769: the process-wide unpromoted count follows every child repo's backlog: up by one per
   committed update, down by one per promotion, and back where it was once Tetris has promoted
   everything. A graceful stop logs it as the writes it will lose, so it must be exact. The
   child is paused while it fills, so the count holds still. */
FIXTURE(Issue769UnpromotedCounts) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      const size_t unpromoted_before = Indy::TRepo::GetUnpromotedUpdates();
      const size_t entries_before = Indy::TRepo::GetUnpromotedEntries();
      const size_t promoted_before = Indy::TRepo::GetPromotedUpdates();
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      auto child = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, false, true);
      SetPaused(manager.get(), child, true);
      int64_t key = 0;
      for (size_t i = 0; i < 4UL; ++i) {
        PushOne(manager.get(), child, idx_id, ++key);
      }
      /* The root's own updates aren't counted: nothing promotes them. */
      PushOne(manager.get(), parent, idx_id, ++key);
      EXPECT_EQ(Indy::TRepo::GetUnpromotedUpdates(), unpromoted_before + 4UL);
      EXPECT_EQ(Indy::TRepo::GetUnpromotedEntries(), entries_before + 4UL);
      EXPECT_EQ(Indy::TRepo::GetPromotedUpdates(), promoted_before);
      SetPaused(manager.get(), child, false);
      EXPECT_TRUE(WaitFor([&] { return !child->GetMemBacklogDepth(); }, 10s));
      EXPECT_EQ(Indy::TRepo::GetUnpromotedUpdates(), unpromoted_before);
      EXPECT_EQ(Indy::TRepo::GetUnpromotedEntries(), entries_before);
      EXPECT_EQ(Indy::TRepo::GetPromotedUpdates(), promoted_before + 4UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #721: a writer's room in a POV's backlog counts the backlog plus the room other writers hold,
   in updates and in entries, so writers arriving together can't pass the cap; a write bigger than
   the entry cap gets room only once the backlog is empty, and smaller writes wait behind it; room
   is given back when its reservation goes. The child is paused so its backlog holds still. */
FIXTURE(Issue721BacklogReservation) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      auto child = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, false, true);
      SetPaused(manager.get(), child, true);
      int64_t key = 0;
      for (size_t i = 0; i < 3UL; ++i) {
        PushOne(manager.get(), child, idx_id, ++key);
      }
      EXPECT_EQ(child->GetMemBacklogDepth(), 3UL);
      EXPECT_EQ(child->GetMemBacklogEntries(), 3UL);
      /* Caps of 5 updates and 6 entries. */
      constexpr size_t cap = 5UL, entry_cap = 6UL;
      /* extra */ {
        Indy::TRepo::TBacklogReservation first(&*child), second(&*child), third(&*child), fourth(&*child);
        EXPECT_TRUE(first.TryReserve(cap, entry_cap, 2UL));
        /* 3 in the backlog + 2 held + 2 more would pass 6 entries. */
        EXPECT_FALSE(second.TryReserve(cap, entry_cap, 2UL));
        EXPECT_FALSE(second.IsReserved());
        EXPECT_TRUE(third.TryReserve(cap, entry_cap, 1UL));
        /* 3 in the backlog + 2 held would pass 5 updates, however few entries. */
        EXPECT_FALSE(fourth.TryReserve(cap, entry_cap, 0UL));
      }
      /* extra */ {
        /* The room above was given back. */
        Indy::TRepo::TBacklogReservation room(&*child);
        EXPECT_TRUE(room.TryReserve(cap, entry_cap, 3UL));
      }
      /* extra */ {
        Indy::TRepo::TBacklogReservation big(&*child);
        /* Bigger than the cap: it waits for an empty backlog... */
        EXPECT_FALSE(big.TryReserve(cap, entry_cap, 7UL));
        /* extra */ {
          /* ...and while it does, smaller writes wait behind it. */
          Indy::TRepo::TBacklogReservation small(&*child);
          EXPECT_FALSE(small.TryReserve(cap, entry_cap, 1UL));
        }
      }
      /* extra */ {
        /* Once it has gone, they don't. */
        Indy::TRepo::TBacklogReservation small(&*child);
        EXPECT_TRUE(small.TryReserve(cap, entry_cap, 1UL));
      }
      /* extra */ {
        /* An empty backlog takes a write bigger than the cap. */
        auto empty = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, false, true);
        Indy::TRepo::TBacklogReservation big(&*empty), small(&*empty);
        EXPECT_TRUE(big.TryReserve(cap, entry_cap, 7UL));
        EXPECT_FALSE(small.TryReserve(cap, entry_cap, 1UL));
      }
      SetPaused(manager.get(), child, false);
      EXPECT_TRUE(WaitFor([&] { return !child->GetMemBacklogDepth(); }, 10s));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #636: pausing a pov at any point of a promotion round must be safe.  A round peeks a child,
   then pushes its update to the parent and pops it from the child in one commit.  A pause that
   committed in between left the round popping a paused repo (PopLowest asserts Status == Normal)
   and parting it from the player a second time.

   The player here holds each round open for a moment after its peek, and the test pauses the
   child over and over while rounds are in flight, many of them inside that window.  Whatever the
   timing: once pause has returned nothing more of the child's reaches the parent, no update is
   lost or promoted twice, and after unpause the child drains. */
FIXTURE(Issue636PauseMidRound) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      std::atomic<bool> in_window(false);
      tetris.OnPeeked = [&in_window] {
        in_window = true;
        std::this_thread::sleep_for(200us);
        in_window = false;
      };
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      auto child = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, false, true);
      const size_t cycle_count = 300UL;
      size_t paused_in_window = 0UL, late_promotions = 0UL, bad_totals = 0UL, stuck = 0UL;
      int64_t key = 0;
      for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        for (size_t i = 0; i < 3UL; ++i) {
          PushOne(manager.get(), child, idx_id, ++key);
        }
        /* Vary where the pause lands: mostly inside a round's window, sometimes anywhere. */
        if (cycle % 4UL) {
          WaitFor([&] { return in_window.load() || !child->GetMemBacklogDepth(); }, 1s);
        }
        if (in_window) {
          ++paused_in_window;
        }
        SetPaused(manager.get(), child, true);
        const size_t parent_at_pause = parent->GetMemBacklogDepth();
        std::this_thread::sleep_for(2ms);
        if (parent->GetMemBacklogDepth() != parent_at_pause) {
          ++late_promotions;
        }
        if (parent->GetMemBacklogDepth() + child->GetMemBacklogDepth() != static_cast<size_t>(key)) {
          ++bad_totals;
        }
        SetPaused(manager.get(), child, false);
        if (!WaitFor([&] {
          return !child->GetMemBacklogDepth() && tetris.PromotionCount == static_cast<size_t>(key);
        }, 10s)) {
          ++stuck;
          break;
        }
      }
      std::cout << "Issue636PauseMidRound: " << cycle_count << " pauses, " << paused_in_window
                << " inside a round's peek-to-commit window; " << late_promotions << " promoted after pause returned, "
                << bad_totals << " with updates lost or duplicated, " << stuck << " stuck" << std::endl;
      /* The test only proves something if many pauses really did land mid-round.  Typically
         about half do; a loaded machine (CI) lands fewer, so ask for a tenth. */
      EXPECT_GT(paused_in_window, cycle_count / 10UL);
      EXPECT_EQ(late_promotions, 0UL);
      EXPECT_EQ(bad_totals, 0UL);
      EXPECT_EQ(stuck, 0UL);
      EXPECT_EQ(parent->GetMemBacklogDepth(), static_cast<size_t>(key));
      EXPECT_EQ(tetris.PromotionCount, static_cast<size_t>(key));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* The Tetris manager of the fixture now running, for a debugger attached when TStallWatchdog
   aborts: its lock and its players' pause state say who is parked waiting for whom. */
static const Orly::Server::TTetrisManager *StalledTetrisManager = nullptr;

/* Aborts the test, with every thread's wait channel printed, if Tick() isn't called for `limit`.
   The #657 fixtures deadlock rather than fail, and a test that hangs eats the CI job's whole
   budget without saying which test or where. */
class TStallWatchdog final {
  NO_COPY(TStallWatchdog);
  public:

  TStallWatchdog(const char *name, std::chrono::seconds limit)
      : Name(name), Limit(limit), Stopping(false), Ticks(0UL), Thread([this] { Run(); }) {}

  ~TStallWatchdog() {
    Stopping = true;
    Thread.join();
  }

  void Tick() {
    ++Ticks;
  }

  private:

  void Run() {
    size_t last = Ticks;
    auto since = std::chrono::steady_clock::now();
    while (!Stopping) {
      std::this_thread::sleep_for(100ms);
      const size_t now_ticks = Ticks;
      const auto now = std::chrono::steady_clock::now();
      if (now_ticks != last) {
        last = now_ticks;
        since = now;
      } else if (now - since > Limit) {
        std::cout << Name << ": no progress for " << Limit.count() << "s after " << now_ticks
                  << " ticks; deadlocked (#657).  Threads:" << std::endl;
        if (DIR *dir = opendir("/proc/self/task")) {
          while (const dirent *ent = readdir(dir)) {
            const std::string tid = ent->d_name;
            if (tid == "." || tid == "..") {
              continue;
            }
            std::string wchan;
            std::ifstream strm("/proc/self/task/" + tid + "/wchan");
            std::getline(strm, wchan);
            std::cout << "  tid " << tid << " wchan=" << (wchan.empty() ? "-" : wchan) << std::endl;
          }
          closedir(dir);
        }
        std::cout.flush();
        abort();
      }
    }
  }

  const char *Name;

  const std::chrono::seconds Limit;

  std::atomic<bool> Stopping;

  std::atomic<size_t> Ticks;

  std::thread Thread;

};  // TStallWatchdog

/* #657: a commit holds the replication queue lock (a std::mutex) for its whole apply, and the
   apply can wait for the Tetris manager's lock: PopLowest parts a drained child.  If that lock is
   held by something that is itself waiting for the player -- PausePlayer, which parks until the
   player acknowledges the pause -- neither moves, and every later commit in the server queues
   behind the replication queue lock.  A server reaches this through BeginImport, which pauses the
   global pov's player, while that player is promoting a burst of writes.

   Each cycle writes one update to each of several children, so the player's round is a run of
   commits that each drain a child and part it, then pauses and unpauses the parent's player
   while that round is in flight. */
FIXTURE(Issue657PausePlayerMidCommit) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      /* Before the manager, so it also covers the manager's teardown (StopAllPlayers). */
      TStallWatchdog watchdog("Issue657PausePlayerMidCommit", 60s);
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      StalledTetrisManager = &tetris;
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      std::vector<L0::TManager::TPtr<Indy::TRepo>> children;
      for (size_t i = 0; i < 8UL; ++i) {
        children.push_back(manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, false, true));
      }
      const size_t cycle_count = 300UL;
      size_t stuck = 0UL;
      int64_t key = 0;
      for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        for (const auto &child: children) {
          PushOne(manager.get(), child, idx_id, ++key);
        }
        tetris.PausePlayer(parent->GetId());
        watchdog.Tick();
        tetris.UnpausePlayer(parent->GetId());
        if (!WaitFor([&] { return tetris.PromotionCount == static_cast<size_t>(key); }, 10s)) {
          ++stuck;
          break;
        }
        watchdog.Tick();
      }
      std::cout << "Issue657PausePlayerMidCommit: " << cycle_count << " cycles, " << stuck << " stuck" << std::endl;
      EXPECT_EQ(stuck, 0UL);
      EXPECT_EQ(tetris.PromotionCount, static_cast<size_t>(key));
      EXPECT_EQ(parent->GetMemBacklogDepth(), static_cast<size_t>(key));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #657, the shape the issue describes: two players share the Tetris runner, and a fiber on
   another runner takes the manager's lock briefly and often (IsPlayerPaused stands in for any
   short holder).  Player A's commit holds the replication queue lock and parks on the manager's
   lock in PopLowest -> Part; player B, on the same runner, reaches its own commit and blocks the
   runner's thread on the replication queue lock.  When the brief holder lets go, the lock passes
   to A, which can't run: its runner is blocked behind A's own commit. */
FIXTURE(Issue657TwoPlayersBriefHolder) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      /* Before the manager, so it also covers the manager's teardown (StopAllPlayers). */
      TStallWatchdog watchdog("Issue657TwoPlayersBriefHolder", 60s);
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      StalledTetrisManager = &tetris;
      const TUuid idx_id(TUuid::Twister);
      std::vector<L0::TManager::TPtr<Indy::TRepo>> parents, children;
      for (size_t p = 0; p < 2UL; ++p) {
        parents.push_back(manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true));
        for (size_t i = 0; i < 4UL; ++i) {
          children.push_back(manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parents.back(), false, true));
        }
      }
      const size_t cycle_count = 400UL;
      size_t stuck = 0UL, polls = 0UL;
      int64_t key = 0;
      for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        for (const auto &child: children) {
          PushOne(manager.get(), child, idx_id, ++key);
        }
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (tetris.PromotionCount != static_cast<size_t>(key)) {
          if (std::chrono::steady_clock::now() >= deadline) {
            ++stuck;
            break;
          }
          tetris.IsPlayerPaused(parents[polls % 2UL]->GetId());
          ++polls;
        }
        if (stuck) {
          break;
        }
        watchdog.Tick();
      }
      std::cout << "Issue657TwoPlayersBriefHolder: " << cycle_count << " cycles, " << polls << " lock polls, "
                << stuck << " stuck" << std::endl;
      EXPECT_EQ(stuck, 0UL);
      EXPECT_EQ(tetris.PromotionCount, static_cast<size_t>(key));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #657: a write that joins a paused parent with no player constructs the player paused, and the
   constructor used to park, inside the writer's commit (replication queue lock and the child's
   DataLock held), until the new player's first round acknowledged the pause.  That round runs on
   the Tetris runner, which another player's commit can be holding OS-blocked on the replication
   queue lock. */
FIXTURE(Issue657PausedJoinMidCommit) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      /* Before the manager, so it also covers the manager's teardown (StopAllPlayers). */
      TStallWatchdog watchdog("Issue657PausedJoinMidCommit", 60s);
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      StalledTetrisManager = &tetris;
      const TUuid idx_id(TUuid::Twister);
      auto paused_parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      auto paused_child = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), paused_parent, false, true);
      auto busy_parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false, true);
      std::vector<L0::TManager::TPtr<Indy::TRepo>> busy_children;
      for (size_t i = 0; i < 8UL; ++i) {
        busy_children.push_back(manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), busy_parent, false, true));
      }
      const size_t cycle_count = 300UL;
      size_t stuck = 0UL;
      int64_t key = 0;
      for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        /* The paused parent has no player now (its only child drained last cycle), so the write
           below constructs one, paused. */
        tetris.PausePlayer(paused_parent->GetId());
        for (const auto &child: busy_children) {
          PushOne(manager.get(), child, idx_id, ++key);
        }
        PushOne(manager.get(), paused_child, idx_id, ++key);
        watchdog.Tick();
        tetris.UnpausePlayer(paused_parent->GetId());
        if (!WaitFor([&] { return tetris.PromotionCount == static_cast<size_t>(key); }, 10s)) {
          ++stuck;
          break;
        }
        watchdog.Tick();
      }
      std::cout << "Issue657PausedJoinMidCommit: " << cycle_count << " cycles, " << stuck << " stuck" << std::endl;
      EXPECT_EQ(stuck, 0UL);
      EXPECT_EQ(tetris.PromotionCount, static_cast<size_t>(key));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #665 harness.  A Tetris manager whose players never promote and pin nothing: the fixture does
   each promotion by hand, so it decides when a pop commits, and the only pins on the child are
   the ones the fixture and the repo itself hold.  (A real player's TChild pins the child until it
   parts, which only shifts the moment the child can be discarded; the bug is the same.) */
class TIdleTetrisManager final
    : public Orly::Server::TTetrisManager {
  NO_COPY(TIdleTetrisManager);
  public:

  TIdleTetrisManager(TScheduler *scheduler,
                     Fiber::TRunner::TRunnerCons &runner_cons,
                     Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager)
      : Orly::Server::TTetrisManager(scheduler, runner_cons, frame_pool_manager, [](Fiber::TRunner *) {}, true) {}

  virtual ~TIdleTetrisManager() {
    StopAllPlayers();
  }

  private:

  class TPlayer final
      : public Orly::Server::TTetrisManager::TPlayer {
    NO_COPY(TPlayer);
    public:

    TPlayer(TIdleTetrisManager *manager, bool is_paused, bool is_master)
        : Orly::Server::TTetrisManager::TPlayer(manager) {
      Start(is_paused, is_master);
    }

    private:

    virtual void OnJoin(const TUuid &) override {}

    virtual void OnPart(const TUuid &) override {}

    virtual void OnPause() override {}

    virtual void OnUnpause() override {}

    virtual void Play() override {
      std::this_thread::sleep_for(1ms);
    }

  };  // TIdleTetrisManager::TPlayer

  virtual Orly::Server::TTetrisManager::TPlayer *NewPlayer(const TUuid &, const TUuid &, bool is_paused, bool is_master) override {
    return new TPlayer(this, is_paused, is_master);
  }

};  // TIdleTetrisManager

/* Repos whose memory merge a fixture can step by hand. */
class TSteppedSafeRepo final
    : public TSafeRepo {
  NO_COPY(TSteppedSafeRepo);
  public:
  using TSafeRepo::TSafeRepo;
  using Orly::Indy::TRepo::StepMergeMem;
};  // TSteppedSafeRepo

class TSteppedFastRepo final
    : public TFastRepo {
  NO_COPY(TSteppedFastRepo);
  public:
  using TFastRepo::TFastRepo;
  using Orly::Indy::TRepo::StepMergeMem;
};  // TSteppedFastRepo

/* Builds stepped repos, and opens a missing repo the way orlyi does (Indy::TManager::
   ReconstructRepo): a fresh, empty safe repo under the asked-for id.  That stand-in is what a pop
   completion used to land on once the popped repo was gone (#665). */
class T665Manager final
    : public TMyManager {
  NO_COPY(T665Manager);
  public:

  using TMyManager::TMyManager;

  virtual TRepo *ConstructRepo(const Base::TUuid &repo_id,
                               const std::optional<TTtl> &ttl,
                               const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                               bool is_safe,
                               bool /*create*/) override {
    return is_safe ?
      static_cast<TRepo *>(new TSteppedSafeRepo(this, repo_id, *ttl, parent_repo))
    : static_cast<TRepo *>(new TSteppedFastRepo(this, repo_id, *ttl, parent_repo));
  }

  virtual TRepo *ReconstructRepo(const Base::TUuid &repo_id) override {
    return TSafeRepo::ReConstructFromDisk(this, repo_id, L0::TDeadline::clock::now() + std::chrono::seconds(1000));
  }

  /* The teardown sweep that drops every repo's self-pin: a sanctioned discard (#521). */
  void DropDirtySelfPins() {
    ReleaseDirtySelfPins();
  }

};  // T665Manager

/* Promote the child's oldest update to the parent, as a Tetris round does: peek it, push it to the
   parent and pop it from the child, in one transaction.  With a safe parent the pop's completion,
   which releases the update in the child, waits until the parent writes the update to disk: the
   parent's next memory merge. */
static void PromoteOne(T665Manager *manager, const L0::TManager::TPtr<Indy::TRepo> &child, const L0::TManager::TPtr<Indy::TRepo> &parent) {
  auto transaction = manager->NewTransaction();
  auto update = transaction->Peek(child);
  if (!EXPECT_TRUE(static_cast<bool>(update))) {
    return;
  }
  transaction->Push(parent, update);
  transaction->Pop(child);
  transaction->Prepare();
  transaction->CommitAction();
}

static void StepMergeMem(const L0::TManager::TPtr<Indy::TRepo> &repo) {
  if (auto *safe = dynamic_cast<TSteppedSafeRepo *>(repo.Get())) {
    safe->StepMergeMem();
  } else if (auto *fast = dynamic_cast<TSteppedFastRepo *>(repo.Get())) {
    fast->StepMergeMem();
  } else {
    EXPECT_TRUE(false);
  }
}

/* #665: a ttl-0 pov's repo must not be discarded while a pop of one of its updates is still
   waiting for its completion, and the completion must land on that repo.

   The child writes, Tetris pops the write into the safe root, and the pop's completion waits for
   the root's next memory merge to put it on disk.  Meanwhile the child's own memory merge runs,
   and a second write lands while it does.  The merge sealed the current memory layer before it
   began; the second write goes to a fresh one.  The merge then drops the first write (released),
   finds no data in the mapping, and used to drop the repo's self-pin, with the second write
   unreleased in the current layer.  Once Tetris popped that write and the pov's owner let go,
   nothing pinned the repo, and the manager discarded it as an expired pov ("dropping unmerged
   updates", #521).  When the root's merge then completed the pop, the completion's ForceOpenRepo
   built an empty repo under the old id and released update 2 in it: `seq_num < NextUpdate` in
   debug, a resurrected empty pov in release. */
FIXTURE(Issue665PopCompletionOutlivesMergeRace) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<T665Manager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      const TUuid child_id(TUuid::Twister);
      auto root = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      auto child = manager->GetRepo(child_id, TTtl(0), root, false, true);
      /* Write 1, promote it, and complete the pop: update 1 is released in the child. */
      PushOne(manager.get(), child, idx_id, 1L);
      PromoteOne(manager.get(), child, root);
      StepMergeMem(root);
      EXPECT_EQ(child->GetReleasedUpTo(), 1UL);
      /* The child's merge; write 2 lands after it has sealed the layer holding update 1. */
      bool wrote_mid_merge = false;
      Orly::Indy::TRepo::OnMergeMemSealedForTest = [&](Orly::Indy::TRepo *repo) {
        if (!wrote_mid_merge && repo->GetId() == child_id) {
          wrote_mid_merge = true;
          PushOne(manager.get(), child, idx_id, 2L);
        }
      };
      StepMergeMem(child);
      Orly::Indy::TRepo::OnMergeMemSealedForTest = nullptr;
      EXPECT_TRUE(wrote_mid_merge);
      /* Promote update 2.  Its pop waits on the root's merge. */
      PromoteOne(manager.get(), child, root);
      EXPECT_EQ(child->GetReleasedUpTo(), 1UL);
      /* The pov's owner lets go.  Update 2 is unreleased, so the repo must stay. */
      child.Reset();
      EXPECT_TRUE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      /* The root's merge completes the pop: the child releases update 2, and with nothing left
         to release and nobody holding it, the ttl-0 pov goes. */
      StepMergeMem(root);
      EXPECT_FALSE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      /* Both writes reached the root. */
      EXPECT_EQ(root->GetNextSequenceNumber(), 3UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #665: a pop's completion for a repo the manager has already discarded is a no-op.  It must not
   open the repo, which for a missing id constructs an empty one: `seq_num < NextUpdate` in
   ReleaseUpdate in debug, and in release an empty repo resurrected under the expired pov's id.
   The discard here is the manager's sweep of self-pins, a sanctioned discard (#521), taken while
   the pop's completion waits on the root's merge. */
FIXTURE(Issue665PopCompletionAfterDiscardIsNoOp) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<T665Manager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      const TUuid child_id(TUuid::Twister);
      auto root = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      auto child = manager->GetRepo(child_id, TTtl(0), root, false, true);
      PushOne(manager.get(), child, idx_id, 1L);
      PushOne(manager.get(), child, idx_id, 2L);
      PromoteOne(manager.get(), child, root);
      PromoteOne(manager.get(), child, root);
      /* Both pops wait on the root's merge.  The owner lets go, and the sweep discards the pov. */
      child.Reset();
      EXPECT_TRUE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      manager->DropDirtySelfPins();
      EXPECT_FALSE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      /* The completions find nothing to release, and must leave it that way. */
      StepMergeMem(root);
      EXPECT_FALSE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      EXPECT_EQ(root->GetNextSequenceNumber(), 3UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* #665 with #661: a pov with a ttl is cached, not destroyed, when its pop completes and nothing
   holds it, and since #661 a cached repo keeps its parent.  The completion's TryOpenLiveRepo
   reopens such a repo exactly as it was, so a later write rejoins Tetris through that parent and a
   later completion releases in the same repo. */
FIXTURE(Issue665PopCompletionOnCachedRepo) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<T665Manager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      const TUuid child_id(TUuid::Twister);
      auto root = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      /* extra */ {
        auto child = manager->GetRepo(child_id, TTtl(600s), root, false, true);
        PushOne(manager.get(), child, idx_id, 1L);
        PromoteOne(manager.get(), child, root);
      }
      /* Only the self-pin holds the child now.  Completing the pop releases update 1 and the pin,
         and the child closes into the cache. */
      StepMergeMem(root);
      if (EXPECT_TRUE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)))) {
        auto child = manager->GetRepo(child_id, std::nullopt, std::nullopt, false, false);
        EXPECT_EQ(child->GetReleasedUpTo(), 1UL);
        const auto &parent = child->GetParentRepo();
        EXPECT_TRUE(parent && parent->Get() == root.Get());
        PushOne(manager.get(), child, idx_id, 2L);
        PromoteOne(manager.get(), child, root);
        StepMergeMem(root);
        EXPECT_EQ(child->GetReleasedUpTo(), 2UL);
      }
      EXPECT_TRUE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      EXPECT_EQ(root->GetNextSequenceNumber(), 3UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* A transaction a master replicated must not be refused for want of WAL room (#891): the slave
   applies it on a fiber, and a refusal there left the batch half-applied and the replica behind
   for good.  With WaitForWalRoom, Prepare on a fiber waits out a full ring until a checkpoint (on
   another thread, as the server's checkpointer is) makes room, then commits; it refuses only once
   its give_up says so. */
FIXTURE(Issue891ReplicatedWriteWaitsForWalRoom) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    using namespace Orly::Indy::Disk;
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(4, 4, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    /* Declared before the manager, so it outlives it. */
    Sim::TFaultPlan plan;
    Sim::TFaultDevice device(&plan, 512UL);
    TWal::TConfig config;
    config.BaseOffset = Orly::Indy::Disk::Util::PhysicalBlockSize;
    config.CapacityBytes = 32UL * 1024UL;
    config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
    config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + WalAlignment;
    config.StoreId = TUuid("12345678-1234-1234-1234-123456789abc");
    TWal wal(&device, config);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    /* Fill the ring until not even a one-byte record fits.  Admission is conservative while records
       are pending (it allows for the biggest group the leader might build from them), so once they
       are durable a little room can come back; top up until a pass after they are durable takes
       nothing. */
    uint64_t last = 0UL;
    for (bool took = true; took;) {
      took = false;
      for (size_t size : {2000UL, 200UL, 1UL}) {
        const std::string payload(size, 'X');
        for (;;) {
          try {
            last = wal.Append(TWalRecordType::Txn, payload.data(), payload.size());
            took = true;
          } catch (const TWalRingFull &) {
            break;
          }
        }
      }
      EXPECT_TRUE(wal.WaitForDurable(last, 5000ms));
    }
    const auto reserve_one = [&wal] { wal.Reserve(1UL, TWal::TRoom::All); };
    EXPECT_THROW_FUNC(TWalRingFull, reserve_one);
    const TUuid idx_id(TUuid::Twister);
    auto repo = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
    const auto new_update = [&] {
      return TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(1L), &arena, state_alloc)), TKey(10L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc));
    };
    /* Stopping: refused, with nothing applied. */ {
      auto transaction = manager->NewTransaction();
      transaction->Push(repo, new_update());
      transaction->WaitForWalRoom([] { return true; });
      const auto prepare = [&transaction] { transaction->Prepare(); };
      EXPECT_THROW_FUNC(TWalRingFull, prepare);
    }
    EXPECT_EQ(repo->GetNextSequenceNumber(), 1UL);
    /* Not stopping: waits for the checkpoint, then commits. */
    std::atomic<bool> checkpointed(false);
    std::thread checkpointer([&wal, &checkpointed, last] {
      std::this_thread::sleep_for(200ms);
      checkpointed = true;
      wal.Checkpoint(last + 1UL, 0UL);
    });
    /* extra */ {
      auto transaction = manager->NewTransaction();
      transaction->Push(repo, new_update());
      transaction->WaitForWalRoom([] { return false; });
      bool prepared = false;
      try {
        transaction->Prepare();
        prepared = true;
      } catch (const std::exception &ex) {
        std::cerr << "Prepare threw: " << ex.what() << std::endl;
      }
      EXPECT_TRUE(prepared);
      EXPECT_TRUE(checkpointed.load());
      transaction->CommitAction();
    }
    checkpointer.join();
    EXPECT_EQ(repo->GetNextSequenceNumber(), 2UL);
    EXPECT_EQ(wal.GetLastAppendedLsn(), last + 1UL);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #963: a paused safe repo's unpromoted updates are copied forward in the WAL (RepoCopy records),
   so the head can pass the records that pushed them.  The fixtures below run a TWal of
   Issue963RingBytes on a fault device, with a safe root and paused safe children of it. */
static constexpr size_t Issue963RingBytes = 1024UL * 1024UL;

static Disk::TWal::TConfig MakeIssue963WalConfig() {
  Disk::TWal::TConfig config;
  config.BaseOffset = Disk::Util::PhysicalBlockSize;
  config.CapacityBytes = Issue963RingBytes;
  config.CheckpointSlot0Offset = config.BaseOffset + config.CapacityBytes;
  config.CheckpointSlot1Offset = config.CheckpointSlot0Offset + Disk::WalAlignment;
  config.StoreId = TUuid("12345678-1234-1234-1234-123456789abc");
  return config;
}

/* Sectors for a fault device holding that ring and its checkpoint slots. */
static constexpr size_t Issue963DeviceSectors = (Disk::Util::PhysicalBlockSize + Issue963RingBytes + 2UL * Disk::WalAlignment) / Disk::Util::PhysicalSectorSize;

/* Append filler records until `lsn` is more than half the ring behind, as the checkpointer's
   copiers count it.  With `wait`, until the filler is durable too.  Returns the last filler LSN. */
static uint64_t AdvancePast(Disk::TWal &wal, uint64_t lsn, bool wait = true) {
  const std::string payload(2000UL, 'X');
  uint64_t last = 0UL;
  for (size_t round = 0UL; wal.GetRingBytesBehind(lsn) <= wal.GetCapacityBytes() / 2UL + Disk::WalAlignment; ++round) {
    if (!EXPECT_LT(round, 10000UL)) {
      break;
    }
    for (size_t i = 0UL; i < 8UL; ++i) {
      try {
        last = wal.Append(Disk::TWalRecordType::Txn, payload.data(), payload.size());
      } catch (const Disk::TWalRingFull &) {
        break;
      }
    }
    if (wait) {
      EXPECT_TRUE(wal.WaitForDurable(last, 5000ms));
    } else {
      std::this_thread::sleep_for(2ms);
    }
  }
  return last;
}

/* What recovery would replay from the newest checkpoint: the records, and the copies it would
   rebuild repos from (TWalRetention::PlanReplay, as server.cc's Step 7 calls it). */
struct TIssue963Replay {
  Disk::TScanResult Scan;
  uint64_t StartLsn = 1UL;
  TWalRetention::TReplayPlan Plan;
};

static TIssue963Replay ScanForReplay(Disk::Util::TDevice *device, const Disk::TWal::TConfig &config) {
  TIssue963Replay out;
  uint64_t start_group = 1UL, start_ring_offset = 0UL, start_chain = 0UL;
  uint32_t start_lap = 1U;
  auto cp = Disk::TWal::ReadNewestCheckpoint(device, config.CheckpointSlot0Offset, config.CheckpointSlot1Offset, config.StoreId, config.CheckpointSlotBytes);
  if (cp) {
    out.StartLsn = cp->HeadLsn;
    if (cp->HeadGroupNum > 0UL) {
      start_group = cp->HeadGroupNum;
      start_lap = cp->HeadLap;
      start_ring_offset = cp->HeadRingOffset;
      start_chain = cp->HeadChain;
    }
  }
  out.Scan = Disk::TWal::Scan(device, config, start_group, out.StartLsn, start_lap, start_ring_offset, start_chain);
  std::vector<TWalRetention::TReplayRecord> records;
  for (const auto &rec : out.Scan.Records) {
    if (rec.Lsn >= out.StartLsn) {
      records.push_back(TWalRetention::TReplayRecord{rec.Lsn, rec.Type, rec.Body.data(), rec.Body.size()});
    }
  }
  out.Plan = TWalRetention::PlanReplay(records);
  return out;
}

/* Which of the keys [1, 10] `repo` shows. */
static std::vector<int64_t> VisibleKeys(const L0::TManager::TPtr<Indy::TRepo> &repo, const TUuid &idx_id) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  std::vector<int64_t> keys;
  for (int64_t key = 1; key <= 10; ++key) {
    auto view = make_unique<Indy::TRepo::TView>(repo);
    auto walker_ptr = repo->NewPresentWalker(view, TIndexKey(idx_id, TKey(make_tuple(key), &arena, state_alloc)), TIndexKey(idx_id, TKey(make_tuple(key + 1L), &arena, state_alloc)));
    if (*walker_ptr) {
      keys.push_back(key);
    }
  }
  return keys;
}

/* Pause, copy forward two laps of the ring, unpause, promote.  Each lap the copy is the only
   record of the repo's updates left in the ring, its room is spent and owed again, and recovery
   would rebuild the repo from it; the store is marked as holding copies once, before the first. */
FIXTURE(Issue963PausedRepoCopiedForwardTwoLaps) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    Disk::Sim::TFaultPlan plan;
    Disk::Sim::TFaultDevice device(&plan, Issue963DeviceSectors);
    const auto config = MakeIssue963WalConfig();
    Disk::TWal wal(&device, config);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    TWalRetention *retention = manager->GetWalRetention();
    size_t gate_runs = 0UL;
    retention->SetIncompatGate([&gate_runs] { ++gate_runs; });
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      const TUuid child_id(TUuid::Twister);
      auto child = manager->GetRepo(child_id, TTtl::max(), parent, true, true);
      SetPaused(manager.get(), child, true);
      for (int64_t key = 1; key <= 4; ++key) {
        PushOne(manager.get(), child, idx_id, key);
      }
      const uint64_t originals = wal.GetLastAppendedLsn();
      /* Each update holds a group for its promotion and one for its copy, and the WAL is owed both. */
      const size_t footprint = retention->GetPausedFootprintBytes();
      EXPECT_GT(footprint, 0UL);
      EXPECT_EQ(footprint % 8UL, 0UL);
      EXPECT_EQ(wal.GetPromotionDebtBytes(), footprint);
      const uint64_t head0 = retention->ComputeHead();
      EXPECT_LE(head0, originals);
      uint64_t prev_head = head0;
      for (size_t lap = 1UL; lap <= 2UL; ++lap) {
        AdvancePast(wal, prev_head);
        retention->Checkpoint(0UL, true);
        const uint64_t head = retention->ComputeHead();
        /* Its pins (the updates' and the pause's) moved to the copy, so nothing older is needed. */
        EXPECT_GT(head, prev_head);
        EXPECT_GT(head, originals);
        /* The copy spent its room, and the checkpoint that passed what it replaced owed it again. */
        EXPECT_EQ(wal.GetPromotionDebtBytes(), footprint);
        EXPECT_EQ(retention->GetPausedFootprintBytes(), footprint);
        EXPECT_EQ(gate_runs, 1UL);
        const auto replay = ScanForReplay(&device, config);
        EXPECT_EQ(replay.StartLsn, head);
        if (EXPECT_EQ(replay.Plan.Copies.size(), 1UL)) {
          const auto &[first_lsn, parts] = *replay.Plan.Copies.begin();
          EXPECT_EQ(first_lsn, head);
          size_t wires = 0UL;
          for (const auto &part : parts) {
            EXPECT_TRUE(part.RepoId == child_id);
            EXPECT_EQ(part.Start, 1UL);
            EXPECT_EQ(part.Limit, 4UL);
            EXPECT_EQ(part.Next, 5UL);
            EXPECT_TRUE(part.Paused);
            wires += part.Wires.size();
          }
          /* One transaction for each update, and the pause. */
          EXPECT_EQ(wires, 5UL);
        }
        EXPECT_EQ(replay.Plan.Skip.size(), 1UL);
        EXPECT_EQ(replay.Plan.Skip.count(child_id), 1UL);
        prev_head = head;
      }
      EXPECT_EQ(child->GetStatus(), Paused);
      EXPECT_TRUE(VisibleKeys(parent, idx_id).empty());
      SetPaused(manager.get(), child, false);
      EXPECT_TRUE(WaitFor([&] { return !child->GetMemBacklogDepth(); }, 10s));
      EXPECT_TRUE(VisibleKeys(parent, idx_id) == std::vector<int64_t>({1, 2, 3, 4}));
      /* Promoted: nothing paused, nothing owed. */
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 0UL);
      EXPECT_EQ(wal.GetPromotionDebtBytes(), 0UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* A write that would take the room paused repos hold past their share of the ring is refused, as
   retriable and with nothing applied; and so is a pause that would take it there all at once.  An
   unpause gives the copy room back. */
FIXTURE(Issue963PausedRoomCap) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    Disk::Sim::TFaultPlan plan;
    Disk::Sim::TFaultDevice device(&plan, Issue963DeviceSectors);
    Disk::TWal wal(&device, MakeIssue963WalConfig());
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    TWalRetention *retention = manager->GetWalRetention();
    EXPECT_EQ(retention->GetPausedCapBytes(), Issue963RingBytes / 2UL);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      auto child = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, true, true);
      SetPaused(manager.get(), child, true);
      PushOne(manager.get(), child, idx_id, 1);
      const size_t one = retention->GetPausedFootprintBytes();
      EXPECT_GT(one, 0UL);
      retention->SetPausedCapBytesForTest(2UL * one);
      PushOne(manager.get(), child, idx_id, 2);
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 2UL * one);
      const uint64_t last = wal.GetLastAppendedLsn();
      /* extra */ {
        auto transaction = manager->NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ { TIndexKey(idx_id, TKey(make_tuple(3L), &arena, state_alloc)), TKey(30L, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
        transaction->Push(child, update);
        const auto prepare = [&transaction] { transaction->Prepare(); };
        EXPECT_THROW_FUNC(Disk::TWalPausedFull, prepare);
      }
      EXPECT_EQ(child->GetNextSequenceNumber(), 3UL);
      EXPECT_EQ(wal.GetLastAppendedLsn(), last);
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 2UL * one);
      /* Writes anywhere else are not touched by it. */
      PushOne(manager.get(), parent, idx_id, 100);
      auto other = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, true, true);
      PushOne(manager.get(), other, idx_id, 10);
      PushOne(manager.get(), other, idx_id, 11);
      /* Pausing it would add its two updates' room at once: refused, and it stays unpaused. */
      /* extra */ {
        auto transaction = manager->NewTransaction();
        transaction->Pause(other);
        const auto prepare = [&transaction] { transaction->Prepare(); };
        EXPECT_THROW_FUNC(Disk::TWalPausedFull, prepare);
      }
      EXPECT_EQ(other->GetStatus(), Normal);
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 2UL * one);
      /* Unpaused, it owes only its promotions: a group each. */
      const size_t debt = wal.GetPromotionDebtBytes();
      EXPECT_EQ(debt, 2UL * one + one);
      retention->SetPausedCapBytesForTest(4UL * one);
      SetPaused(manager.get(), other, true);
      EXPECT_EQ(other->GetStatus(), Paused);
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 4UL * one);
      EXPECT_EQ(wal.GetPromotionDebtBytes(), 4UL * one);
      /* Unpausing is never refused, and gives the copy room back. */
      retention->SetPausedCapBytesForTest(0UL);
      SetPaused(manager.get(), child, false);
      EXPECT_EQ(child->GetStatus(), Normal);
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 2UL * one);
      EXPECT_EQ(wal.GetPromotionDebtBytes(), 3UL * one);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* A paused repo that goes (thrown away unpromoted) takes its pins with it, and the room they were owed, for
   their promotions and their copy, is repaid (#962 review). */
FIXTURE(Issue963DropDeadPausedRepoRepaysItsRoom) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    Disk::Sim::TFaultPlan plan;
    Disk::Sim::TFaultDevice device(&plan, Issue963DeviceSectors);
    Disk::TWal wal(&device, MakeIssue963WalConfig());
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    TWalRetention *retention = manager->GetWalRetention();
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      const TUuid child_id(TUuid::Twister);
      /* extra */ {
        auto child = manager->GetRepo(child_id, TTtl(0), parent, true, true);
        SetPaused(manager.get(), child, true);
        PushOne(manager.get(), child, idx_id, 1);
        PushOne(manager.get(), child, idx_id, 2);
        EXPECT_GT(wal.GetPromotionDebtBytes(), 0UL);
        EXPECT_EQ(wal.GetPromotionDebtBytes(), retention->GetPausedFootprintBytes());
        EXPECT_LE(retention->ComputeHead(), wal.GetLastAppendedLsn());
        /* Thrown away with its writes unpromoted (#683): the zero-ttl pov goes when we let go. */
        child->ReleaseDirtyPin();
      }
      EXPECT_FALSE(static_cast<bool>(manager->TryOpenLiveRepo(child_id)));
      /* ComputeHead drops the dead pins: the pause's too, so the head is past everything. */
      EXPECT_EQ(retention->ComputeHead(), wal.GetLastAppendedLsn() + 1UL);
      EXPECT_EQ(wal.GetPromotionDebtBytes(), 0UL);
      EXPECT_EQ(retention->GetPausedFootprintBytes(), 0UL);
      /* And the checkpointer finds nothing to copy. */
      const uint64_t last = wal.GetLastAppendedLsn();
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(wal.GetLastAppendedLsn(), last);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* The ring full when a copy is due (#962 review).  A pause record's copy has no room kept for it,
   so it waits for room, the head stays at the pause, and the next checkpoint with room writes it.
   A paused repo's copy spends the room kept for it, so it is written all the same.  Nothing throws. */
FIXTURE(Issue963CopiesWithTheRingFull) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    Disk::Sim::TFaultPlan plan;
    Disk::Sim::TFaultDevice device(&plan, Issue963DeviceSectors);
    const auto config = MakeIssue963WalConfig();
    Disk::TWal wal(&device, config);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    TWalRetention *retention = manager->GetWalRetention();
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* Take every byte of the ring that isn't kept for promotions and copies. */
    const auto fill = [&wal] {
      std::vector<Disk::TWalReservation> held;
      for (size_t size : {16000UL, 2000UL, 200UL, 1UL}) {
        for (;;) {
          try {
            held.push_back(wal.Reserve(size, Disk::TWal::TRoom::All));
          } catch (const Disk::TWalRingFull &) {
            break;
          }
        }
      }
      const auto reserve_one = [&wal] { wal.Reserve(1UL, Disk::TWal::TRoom::All); };
      EXPECT_THROW_FUNC(Disk::TWalRingFull, reserve_one);
      return held;
    };
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      auto empty = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, true, true);
      SetPaused(manager.get(), empty, true);
      const uint64_t empty_pause = wal.GetLastAppendedLsn();
      EXPECT_EQ(retention->ComputeHead(), empty_pause);
      AdvancePast(wal, empty_pause);
      /* extra */ {
        auto held = fill();
        const uint64_t before = wal.GetLastAppendedLsn();
        retention->Checkpoint(0UL, true);
        EXPECT_EQ(wal.GetLastAppendedLsn(), before);
        EXPECT_EQ(retention->ComputeHead(), empty_pause);
      }
      /* Room again. */
      const uint64_t before = wal.GetLastAppendedLsn();
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(wal.GetLastAppendedLsn(), before + 1UL);
      EXPECT_EQ(retention->ComputeHead(), before + 1UL);
      /* A paused repo with writes. */
      const TUuid full_id(TUuid::Twister);
      auto full = manager->GetRepo(full_id, TTtl::max(), parent, true, true);
      SetPaused(manager.get(), full, true);
      PushOne(manager.get(), full, idx_id, 1);
      PushOne(manager.get(), full, idx_id, 2);
      const uint64_t full_last = wal.GetLastAppendedLsn();
      AdvancePast(wal, full_last);
      /* extra */ {
        auto held = fill();
        const size_t debt = wal.GetPromotionDebtBytes();
        const uint64_t before_full = wal.GetLastAppendedLsn();
        retention->Checkpoint(0UL, true);
        /* Copied, so recovery would rebuild it from the copy; the head is past its originals
           unless the older pause's copy is still waiting for room (the repo's copy may free
           some of what it spent, and the pause's copy may take it). */
        EXPECT_GT(wal.GetLastAppendedLsn(), before_full);
        const uint64_t head = retention->ComputeHead();
        EXPECT_TRUE(head == before + 1UL || head > full_last);
        const auto replay = ScanForReplay(&device, config);
        EXPECT_EQ(replay.Plan.Skip.count(full_id), 1UL);
        for (const auto &[lsn, parts] : replay.Plan.Copies) {
          if (parts.front().RepoId == full_id) {
            EXPECT_GT(lsn, before_full);
            EXPECT_EQ(parts.front().Limit, 2UL);
          }
        }
        /* What the copy spent, the checkpoint (whose head passed the records it replaced) owed again. */
        EXPECT_EQ(wal.GetPromotionDebtBytes(), debt);
      }
      EXPECT_EQ(full->GetStatus(), Paused);
      EXPECT_EQ(empty->GetStatus(), Paused);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* The ring refused a record while a paused repo holds the head, but less than half the ring
   behind (M1 harness): the next checkpoint copies it anyway, as waiting for half the ring would
   wait on writes the ring refuses.  Once copied, more refusals don't copy it again, as the copy
   would give back nothing. */
FIXTURE(Issue963CopiesUnderRingPressure) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    Disk::Sim::TFaultPlan plan;
    Disk::Sim::TFaultDevice device(&plan, Issue963DeviceSectors);
    const auto config = MakeIssue963WalConfig();
    Disk::TWal wal(&device, config);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    TWalRetention *retention = manager->GetWalRetention();
    retention->SetIncompatGate([] {});
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TPromotingTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager, manager.get());
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      const TUuid child_id(TUuid::Twister);
      auto child = manager->GetRepo(child_id, TTtl::max(), parent, true, true);
      SetPaused(manager.get(), child, true);
      for (int64_t key = 1; key <= 4; ++key) {
        PushOne(manager.get(), child, idx_id, key);
      }
      const uint64_t originals = wal.GetLastAppendedLsn();
      const uint64_t head0 = retention->ComputeHead();
      const size_t ring = wal.GetCapacityBytes();
      const size_t target = std::max(3UL * ring / 8UL, ring / 8UL + 2UL * retention->GetPausedFootprintBytes()) + Disk::WalAlignment;
      EXPECT_LT(target, ring / 2UL);
      /* Filler, durable, to between the pressure threshold and half the ring (the distance counts
         what is written, so wait for each few). */
      const std::string payload(2000UL, 'X');
      uint64_t last = 0UL;
      for (size_t round = 0UL; wal.GetRingBytesBehind(head0) <= target; ++round) {
        if (!EXPECT_LT(round, 10000UL)) {
          break;
        }
        last = wal.Append(Disk::TWalRecordType::Txn, payload.data(), payload.size());
        EXPECT_TRUE(wal.WaitForDurable(last, 5000ms));
      }
      EXPECT_LE(wal.GetRingBytesBehind(head0), ring / 2UL);
      /* No refusal yet: less than half the ring behind, so not copied. */
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(retention->ComputeHead(), head0);
      EXPECT_EQ(wal.GetLastAppendedLsn(), last);
      /* A record the ring has no room for. */
      const size_t refusals = wal.GetRingFullRefusals();
      bool refused = false;
      try {
        auto reservation = wal.Reserve(ring * 3UL / 4UL, Disk::TWal::TRoom::All);
      } catch (const Disk::TWalRingFull &) {
        refused = true;
      }
      EXPECT_TRUE(refused);
      EXPECT_EQ(wal.GetRingFullRefusals(), refusals + 1UL);
      retention->Checkpoint(0UL, true);
      const uint64_t copied_head = retention->ComputeHead();
      EXPECT_GT(copied_head, originals);
      EXPECT_GT(wal.GetLastAppendedLsn(), last);
      const auto replay = ScanForReplay(&device, config);
      EXPECT_EQ(replay.Plan.Copies.size(), 1UL);
      EXPECT_EQ(replay.Plan.Skip.count(child_id), 1UL);
      /* Refused again, but the copy is the head now and would give back nothing. */
      const uint64_t after_copy = wal.GetLastAppendedLsn();
      try {
        auto reservation = wal.Reserve(ring - Disk::WalAlignment * 4UL, Disk::TWal::TRoom::All);
      } catch (const Disk::TWalRingFull &) {
      }
      EXPECT_EQ(wal.GetRingFullRefusals(), refusals + 2UL);
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(wal.GetLastAppendedLsn(), after_copy);
      EXPECT_EQ(retention->ComputeHead(), copied_head);
      EXPECT_EQ(child->GetStatus(), Paused);
      SetPaused(manager.get(), child, false);
      EXPECT_TRUE(WaitFor([&] { return !child->GetMemBacklogDepth(); }, 10s));
      EXPECT_TRUE(VisibleKeys(parent, idx_id) == std::vector<int64_t>({1, 2, 3, 4}));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* A copy not durable within the wait (#962 review): its pins stay where they are, the next
   checkpoint doesn't write it again, and the one after it is durable moves them. */
FIXTURE(Issue963CopyNotDurableInTime) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    Disk::Sim::TFaultPlan plan;
    Disk::Sim::TFaultDevice device(&plan, Issue963DeviceSectors);
    Disk::TWal wal(&device, MakeIssue963WalConfig());
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetWal(&wal);
    TWalRetention *retention = manager->GetWalRetention();
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(10UL, 8UL * 1024UL * 1024UL, Fiber::TRunner::LocalRunner.Get());
    /* extra */ {
      TIdleTetrisManager tetris(&scheduler, runner_cons, &frame_pool_manager);
      manager->SetTetrisManager(&tetris);
      const TUuid idx_id(TUuid::Twister);
      auto parent = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true, true);
      auto full = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, true, true);
      SetPaused(manager.get(), full, true);
      PushOne(manager.get(), full, idx_id, 1);
      PushOne(manager.get(), full, idx_id, 2);
      auto empty = manager->GetRepo(TUuid(TUuid::Twister), TTtl::max(), parent, true, true);
      SetPaused(manager.get(), empty, true);
      const uint64_t empty_pause = wal.GetLastAppendedLsn();
      const uint64_t head0 = retention->ComputeHead();
      EXPECT_LT(head0, empty_pause);
      /* The writes are durable; from here the newest group isn't until another follows. */
      wal.SetAutoSealOnQuietForTest(false);
      AdvancePast(wal, empty_pause, false);
      const uint64_t before = wal.GetLastAppendedLsn();
      /* The repo's copy times out, then the pause's (whose group makes the repo's durable). */
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(wal.GetLastAppendedLsn(), before + 2UL);
      EXPECT_EQ(retention->ComputeHead(), head0);
      /* No copy written again; the repo's, durable now, has its pins. */
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(wal.GetLastAppendedLsn(), before + 2UL);
      EXPECT_EQ(retention->ComputeHead(), empty_pause);
      /* A record after it makes the pause's copy durable too. */
      const std::string payload(10UL, 'X');
      wal.Append(Disk::TWalRecordType::Txn, payload.data(), payload.size());
      EXPECT_TRUE(wal.WaitForDurable(before + 2UL, 5000ms));
      retention->Checkpoint(0UL, true);
      EXPECT_EQ(wal.GetLastAppendedLsn(), before + 3UL);
      EXPECT_EQ(retention->ComputeHead(), before + 1UL);
      wal.SetAutoSealOnQuietForTest(true);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 1UL /* a runner for the Tetris manager */);
}

/* A RepoCopy body round-trips, and one cut short or with a part out of range is corrupt. */
FIXTURE(Issue963RepoCopyPartEncoding) {
  TWalRepoCopyPart part;
  part.RepoId = TUuid(TUuid::Twister);
  part.SetId = TUuid(TUuid::Twister);
  part.Part = 1U;
  part.Count = 3U;
  part.Start = 7UL;
  part.Limit = 9UL;
  part.Next = 10UL;
  part.Paused = false;
  part.Wires = {std::string("abc"), std::string(), std::string(3000UL, '\0')};
  const std::string body = part.Encode();
  const auto back = TWalRepoCopyPart::Decode(body.data(), body.size());
  EXPECT_TRUE(back.RepoId == part.RepoId);
  EXPECT_TRUE(back.SetId == part.SetId);
  EXPECT_EQ(back.Part, 1U);
  EXPECT_EQ(back.Count, 3U);
  EXPECT_EQ(back.Start, 7UL);
  EXPECT_EQ(back.Limit, 9UL);
  EXPECT_EQ(back.Next, 10UL);
  EXPECT_FALSE(back.Paused);
  EXPECT_TRUE(back.Wires == part.Wires);
  for (size_t cut : {0UL, 1UL, body.size() / 2UL, body.size() - 1UL}) {
    const auto decode = [&body, cut] { TWalRepoCopyPart::Decode(body.data(), cut); };
    EXPECT_THROW_FUNC(Disk::TWalCorruptError, decode);
  }
  /* extra */ {
    const std::string longer = body + "x";
    const auto decode = [&longer] { TWalRepoCopyPart::Decode(longer.data(), longer.size()); };
    EXPECT_THROW_FUNC(Disk::TWalCorruptError, decode);
  }
  /* extra */ {
    auto bad = part;
    bad.Part = 3U;
    const std::string bad_body = bad.Encode();
    const auto decode = [&bad_body] { TWalRepoCopyPart::Decode(bad_body.data(), bad_body.size()); };
    EXPECT_THROW_FUNC(Disk::TWalCorruptError, decode);
  }
}

/* Recovery's choice of copies, for a crash at each step of a copy: before it is durable, between
   its parts, after the pins moved to it (the originals gone from the ring), and with an older copy
   still there. */
FIXTURE(Issue963PlanReplayAtEachCrashPoint) {
  const TUuid repo_id(TUuid::Twister), other_id(TUuid::Twister);
  const auto make_set = [](const TUuid &repo, uint32_t count, TSequenceNumber start, TSequenceNumber limit) {
    std::vector<std::string> bodies;
    const TUuid set_id(TUuid::Twister);
    for (uint32_t i = 0U; i < count; ++i) {
      TWalRepoCopyPart part;
      part.RepoId = repo;
      part.SetId = set_id;
      part.Part = i;
      part.Count = count;
      part.Start = start;
      part.Limit = limit;
      part.Next = limit + 1UL;
      part.Wires = {"w" + std::to_string(i)};
      bodies.push_back(part.Encode());
    }
    return bodies;
  };
  const std::string txn = "txn";
  using TRecords = std::vector<TWalRetention::TReplayRecord>;
  const auto txn_at = [&txn](uint64_t lsn) {
    return TWalRetention::TReplayRecord{lsn, Disk::TWalRecordType::Txn, txn.data(), txn.size()};
  };
  const auto copy_at = [](uint64_t lsn, const std::string &body) {
    return TWalRetention::TReplayRecord{lsn, Disk::TWalRecordType::RepoCopy, body.data(), body.size()};
  };
  const auto older = make_set(repo_id, 2U, 1UL, 4UL);
  const auto newer = make_set(repo_id, 2U, 1UL, 6UL);
  /* Before the copy is durable: none of it survives, and the originals rebuild the repo. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{txn_at(1UL), txn_at(2UL), txn_at(3UL)});
    EXPECT_TRUE(plan.Copies.empty());
    EXPECT_TRUE(plan.Skip.empty());
  }
  /* Between its parts: an incomplete set counts for nothing. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{txn_at(1UL), txn_at(2UL), copy_at(3UL, newer[0])});
    EXPECT_TRUE(plan.Copies.empty());
    EXPECT_TRUE(plan.Skip.empty());
  }
  /* Complete, the originals still there: it rebuilds the repo. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{txn_at(1UL), txn_at(2UL), copy_at(3UL, newer[0]), txn_at(4UL), copy_at(5UL, newer[1])});
    if (EXPECT_EQ(plan.Copies.size(), 1UL)) {
      EXPECT_EQ(plan.Copies.begin()->first, 3UL);
      EXPECT_EQ(plan.Copies.begin()->second.size(), 2UL);
      EXPECT_EQ(plan.Copies.begin()->second[1].Part, 1U);
      EXPECT_EQ(plan.Copies.begin()->second[1].Limit, 6UL);
    }
    EXPECT_EQ(plan.Skip.count(repo_id), 1UL);
  }
  /* After the pins moved: only the copy is left. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{copy_at(10UL, newer[0]), copy_at(11UL, newer[1]), txn_at(12UL)});
    if (EXPECT_EQ(plan.Copies.size(), 1UL)) {
      EXPECT_EQ(plan.Copies.begin()->first, 10UL);
    }
  }
  /* An older complete copy and a newer one cut short: the older. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{copy_at(10UL, older[0]), copy_at(11UL, older[1]), txn_at(12UL), copy_at(13UL, newer[0])});
    if (EXPECT_EQ(plan.Copies.size(), 1UL)) {
      EXPECT_EQ(plan.Copies.begin()->first, 10UL);
      EXPECT_EQ(plan.Copies.begin()->second[0].Limit, 4UL);
    }
  }
  /* Both complete: the newer. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{copy_at(10UL, older[0]), copy_at(11UL, older[1]), copy_at(13UL, newer[0]), copy_at(14UL, newer[1])});
    if (EXPECT_EQ(plan.Copies.size(), 1UL)) {
      EXPECT_EQ(plan.Copies.begin()->first, 13UL);
    }
    EXPECT_EQ(plan.Skip.size(), 1UL);
  }
  /* A set whose parts disagree counts for nothing; another repo's set is its own. */ {
    const auto mismatched = make_set(repo_id, 2U, 1UL, 4UL);
    auto second = TWalRepoCopyPart::Decode(mismatched[1].data(), mismatched[1].size());
    second.Start = 2UL;
    const std::string bad_second = second.Encode();
    const auto other = make_set(other_id, 1U, 3UL, 3UL);
    const auto plan = TWalRetention::PlanReplay(TRecords{copy_at(10UL, mismatched[0]), copy_at(11UL, bad_second), copy_at(12UL, other[0])});
    if (EXPECT_EQ(plan.Copies.size(), 1UL)) {
      EXPECT_EQ(plan.Copies.begin()->first, 12UL);
    }
    EXPECT_EQ(plan.Skip.count(repo_id), 0UL);
    EXPECT_EQ(plan.Skip.count(other_id), 1UL);
  }
  /* A part seen twice: corrupt, so the set counts for nothing. */ {
    const auto plan = TWalRetention::PlanReplay(TRecords{copy_at(10UL, older[0]), copy_at(11UL, older[0]), copy_at(12UL, older[1])});
    EXPECT_TRUE(plan.Copies.empty());
  }
  /* A RepoCopy record that isn't one. */ {
    const std::string junk = "not a copy";
    const auto plan_junk = [&] { TWalRetention::PlanReplay(TRecords{copy_at(10UL, junk)}); };
    EXPECT_THROW_FUNC(Disk::TWalCorruptError, plan_junk);
  }
}
