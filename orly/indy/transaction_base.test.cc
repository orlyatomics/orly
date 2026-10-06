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
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber_test_runner.h>
#include <orly/indy/repo.h>
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
        if (!WaitFor([&] { return !child->GetMemBacklogDepth(); }, 10s)) {
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
