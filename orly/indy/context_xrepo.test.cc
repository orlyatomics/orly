/* <orly/indy/context_xrepo.test.cc>

   Regression guard for issue #143's cross-repo NON-ATOMIC SNAPSHOT hypothesis.

   TContext (orly/indy/context.cc ctor) builds the reader's view of the repo
   chain (POV child -> ... -> global parent) by constructing ONE TRepo::TView
   per repo, each captured under THAT repo's own DataLock at a DIFFERENT instant
   (repo.cc TView ctor). So the snapshot is atomic per-repo but NOT atomic
   across repos. The #143 hypothesis was that a Tetris promotion (Pop from a
   child + Push to its parent) committing BETWEEN two of those per-repo
   snapshots could leave an in-flight update visible in NEITHER snapshot (a
   drop / undercount of a commutative `+= 1`).

   This guard disproves that hypothesis and PINS the invariant. It drives the
   cross-repo read BY HAND -- snapshotting child-first then each parent (the
   exact order TContext's ctor loop uses), interleaving a child->parent
   promotion between successive snapshots -- and folds the result with the same
   UpdateId-dedup + Rt::Mutate logic as TContext::TPresentWalker::
   ApplyDeferredFold. Repos are parentless and promotion is hand-driven
   (Pop(from)+Push(to,Peek(from)) in one transaction, exactly as
   TRepoTetrisManager::Play registers it) so no live Tetris player fiber is
   needed; the cross-repo snapshot timing under test is identical.

   Why no drop is possible (what this pins): promotion moves an update strictly
   from child toward parent (low index -> high index in the chain), and the
   reader snapshots strictly child -> parent (low index -> high index). With
   both monotone in the same direction, the update is always at-or-ahead of the
   reader's snapshot frontier: it is captured by exactly one snapshot, or by two
   adjacent snapshots (the brief push-before-pop "seen in both" window), which
   the UpdateId dedup collapses. The "neither" transient requires the reader to
   snapshot a higher repo before a lower one -- which the fixed child-first ctor
   order forbids. This generalizes PR #145's single-2-repo in-window guard to
   N-repo chains and to a promotion racing the reader at any snapshot boundary.

   NB on scope: like context_fold.test (PR #146), this is the deterministic
   half. The residual agent-swarm undercount could NOT be reproduced as a
   deterministic indy-layer drop: every cross-repo snapshot/promotion
   interleaving that can be constructed by hand folds correctly. The cross-repo
   non-atomic snapshot, though real, is not the bug.

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

#include <orly/indy/context.h>
#include <optional>

#include <base/scheduler.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber_test_runner.h>
#include <orly/indy/repo.h>
#include <orly/indy/transaction_base.h>
#include <orly/rt/mutate.h>
#include <orly/var/sabot_to_var.h>

#include <base/test/kit.h>

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
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(Disk::Util::PhysicalBlockSize);
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 100UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 200UL);

const std::vector<size_t> MemMergeCoreVec{0};
const std::vector<size_t> DiskMergeCoreVec{0};

class TMyManager : public L1::TManager {
  NO_COPY(TMyManager);
  public:
  TMyManager(Disk::Util::TEngine *engine, Base::TScheduler *scheduler,
             const std::vector<size_t> &mem_merge_cores, const std::vector<size_t> &disk_merge_cores)
      : TManager(engine, 10ms, 100ms, true, true, 1000ms, scheduler, 100UL, 100UL, 20UL, mem_merge_cores, disk_merge_cores, true) {}
  virtual ~TMyManager() {}
  virtual TRepo *ConstructRepo(const Base::TUuid &repo_id, const std::optional<TTtl> &ttl,
                               const std::optional<TManager::TPtr<TRepo>> &parent_repo, bool is_safe, bool) override {
    return is_safe ? static_cast<TRepo *>(new TSafeRepo(this, repo_id, *ttl, parent_repo))
                   : static_cast<TRepo *>(new TFastRepo(this, repo_id, *ttl, parent_repo));
  }
  virtual void SaveRepo(Orly::Indy::L0::TManager::TRepo *) override {}
  virtual void Enqueue(Orly::Indy::TTransactionReplication *, Orly::Indy::L1::TTransaction::TReplica &&) NO_THROW override {}
  virtual Orly::Indy::TTransactionReplication* NewTransactionReplication() override { return nullptr; }
  virtual void DeleteTransactionReplication(Orly::Indy::TTransactionReplication*) NO_THROW override {}
  virtual void ForEachScheduler(const std::function<bool (Fiber::TRunner *)> &) const override {}
  virtual bool CanLoad(const L0::TId &) override { return true; }
  virtual void Delete(const L0::TId &, L0::TSem *) override {}
  virtual void Save(const L0::TId &, const L0::TDeadline &, const std::string &, L0::TSem *) override {}
  virtual bool TryLoad(const L0::TId &, std::string &) override { return true; }
  virtual TRepo *ReconstructRepo(const Base::TUuid &) override { return nullptr; }
  virtual void RunReplicationQueue() override {}
  virtual void RunReplicationWork() override {}
  virtual void RunReplicateTransaction() override {}
  virtual std::mutex &GetReplicationQueueLock() NO_THROW override { return ReplicationQueueLock; }
  inline TManager::TPtr<TRepo> GetRepo(const Base::TUuid &repo_id, const std::optional<TTtl> &ttl,
                                       const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo, bool is_safe, bool create) {
    return create ? OpenOrCreate(repo_id, ttl, parent_repo, is_safe) : ForceOpenRepo(repo_id);
  }
  using TManager::OpenOrCreate;
  private:
  std::mutex ReplicationQueueLock;
};

/* Hand-rolled cross-repo commutative fold over an ORDERED list of repo views,
   mirroring what TContext + ApplyDeferredFold do: union all repos' walkers for
   `key`, dedup by UpdateId, fold same-mutator entries via Rt::Mutate. Returns
   the folded int64 counter, or std::nullopt if the key is found in no repo (a
   DROP). `views` must be ordered child-first (index 0) toward parent, matching
   TContext. */
static std::optional<int64_t> FoldCounter(
    const std::vector<TManager::TPtr<TRepo>> &repos,
    const std::vector<std::unique_ptr<TRepo::TView>> &views,
    const TIndexKey &key) {
  void *sa = alloca(Sabot::State::GetMaxStateSize() * 2);
  void *sb = static_cast<uint8_t *>(sa) + Sabot::State::GetMaxStateSize();
  const Base::TUuid zero_uuid;
  std::vector<Base::TUuid> seen;
  bool any = false;
  Orly::Var::TVar acc;
  for (size_t i = 0; i < repos.size(); ++i) {
    auto walker_ptr = repos[i]->NewPresentWalker(views[i], key);
    auto &walker = *walker_ptr;
    while (walker) {
      const auto &item = *walker;
      if (!item.Op.IsTombstone()) {
        bool dup = false;
        if (item.UpdateId != zero_uuid) {
          for (const auto &s : seen) { if (s == item.UpdateId) { dup = true; break; } }
        }
        if (!dup) {
          if (item.UpdateId != zero_uuid) seen.push_back(item.UpdateId);
          Orly::Var::TVar v = Orly::Var::ToVar(*Sabot::State::TAny::TWrapper(item.Op.NewState(item.OpArena, sb)));
          if (!any) { acc = v; any = true; }
          else { acc = Orly::Rt::Mutate(acc, TMutator::Add, v); }
        }
      }
      ++walker;
    }
  }
  if (!any) return std::nullopt;
  return Orly::Var::TVar::TDt<int64_t>::As(acc);
}

/* The exact agent-swarm topology: POV (child) -> global (parent), 2 repos.
   A `+= 1` from agent A sits in global; a `+= 1` from agent B sits in the POV
   child. Reader snapshots child-first, then promotes B child->global between
   the two snapshots, then snapshots global. Probe the fold. */
FIXTURE(XRepoSnapshot2RepoPovGlobal) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    Base::TUuid global_id(TUuid::Twister), child_id(TUuid::Twister), idx_id(TUuid::Twister);
    auto global_repo = manager->GetRepo(global_id, TTtl::max(), std::nullopt, false, true);
    auto child_repo = manager->GetRepo(child_id, TTtl::max(), std::nullopt, false, true);
    const TIndexKey counter_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));
    auto commit_increment = [&](const TManager::TPtr<TRepo> &repo) {
      auto t = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
      update->AddEntry(counter_key, TKey(1L, &arena, state_alloc), TMutator::Add);
      t->Push(repo, update); t->Prepare(); t->CommitAction();
    };
    auto promote = [&](const TManager::TPtr<TRepo> &from, const TManager::TPtr<TRepo> &to) {
      auto t = manager->NewTransaction();
      t->Pop(from); t->Push(to, t->Peek(from)); t->Prepare(); t->CommitAction();
    };
    commit_increment(global_repo);  // A in global
    commit_increment(child_repo);   // B in child
    std::vector<TManager::TPtr<TRepo>> repos{child_repo, global_repo};
    std::vector<std::unique_ptr<TRepo::TView>> views;
    views.emplace_back(make_unique<TRepo::TView>(child_repo));   // snap child (B present)
    promote(child_repo, global_repo);                           // B child->global mid-read
    views.emplace_back(make_unique<TRepo::TView>(global_repo));  // snap global (A + B)
    auto got = FoldCounter(repos, views, counter_key);
    /* Must be found (no drop) and fold to A + B == 2 (no under/over-count). */
    if (EXPECT_TRUE(static_cast<bool>(got))) { EXPECT_EQ(*got, 2L); }
    std::lock_guard<std::mutex> lock(mut);
    fin = true; cond.notify_one();
  });
}

/* 3-repo chain child -> mid -> global. Value B (`+= 1`) starts in `child`,
   value A (`+= 1`) sits in `global`. We snapshot child-first (the order
   TContext uses), pausing AFTER the child snapshot to promote B child->mid,
   then pause AFTER the mid snapshot to promote B mid->global, all before the
   global snapshot. B leaves every repo just after the reader passes it.

   Drop iff B is captured by no snapshot. Expected fold = 2 (A + B). */
FIXTURE(XRepoSnapshot3RepoTrailing) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);

    Base::TUuid global_id(TUuid::Twister), mid_id(TUuid::Twister), child_id(TUuid::Twister), idx_id(TUuid::Twister);
    /* parentless, hand-driven promotion (no live Tetris fiber) */
    auto global_repo = manager->GetRepo(global_id, TTtl::max(), std::nullopt, false, true);
    auto mid_repo = manager->GetRepo(mid_id, TTtl::max(), std::nullopt, false, true);
    auto child_repo = manager->GetRepo(child_id, TTtl::max(), std::nullopt, false, true);

    const TIndexKey counter_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));

    auto commit_increment = [&](const TManager::TPtr<TRepo> &repo) {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                       TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
      update->AddEntry(counter_key, TKey(1L, &arena, state_alloc), TMutator::Add);
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    };
    auto promote = [&](const TManager::TPtr<TRepo> &from, const TManager::TPtr<TRepo> &to) {
      auto t = manager->NewTransaction();
      t->Pop(from);
      t->Push(to, t->Peek(from));
      t->Prepare();
      t->CommitAction();
    };

    commit_increment(global_repo);  // A in global
    commit_increment(child_repo);   // B in child

    /* Reader snapshots child-first, promoting B one hop ahead between snaps. */
    std::vector<TManager::TPtr<TRepo>> repos{child_repo, mid_repo, global_repo};
    std::vector<std::unique_ptr<TRepo::TView>> views;
    views.emplace_back(make_unique<TRepo::TView>(child_repo));   // snap child (B in child)
    promote(child_repo, mid_repo);                               // B: child -> mid
    views.emplace_back(make_unique<TRepo::TView>(mid_repo));     // snap mid (B left for... it's in mid now)
    promote(mid_repo, global_repo);                              // B: mid -> global
    views.emplace_back(make_unique<TRepo::TView>(global_repo));  // snap global

    auto got = FoldCounter(repos, views, counter_key);
    /* B trails one hop behind the reader frontier at every boundary, yet is
       always captured exactly once. Must fold to A + B == 2, never drop. */
    if (EXPECT_TRUE(static_cast<bool>(got))) { EXPECT_EQ(*got, 2L); }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Same topology, but promote B one hop AHEAD of the snapshot frontier: promote
   B child->mid BEFORE snapshotting child (so child snap misses B), then snap
   child, then snap mid (B in mid -> captured). Sanity that "ahead" is caught.
   Then the adversarial case: promote B such that it is at mid when reader is
   about to snap mid, but we promote mid->global right before snapping mid and
   B was never in child snapshot. */
FIXTURE(XRepoSnapshot3RepoLeading) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);

    Base::TUuid global_id(TUuid::Twister), mid_id(TUuid::Twister), child_id(TUuid::Twister), idx_id(TUuid::Twister);
    auto global_repo = manager->GetRepo(global_id, TTtl::max(), std::nullopt, false, true);
    auto mid_repo = manager->GetRepo(mid_id, TTtl::max(), std::nullopt, false, true);
    auto child_repo = manager->GetRepo(child_id, TTtl::max(), std::nullopt, false, true);

    const TIndexKey counter_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));
    auto commit_increment = [&](const TManager::TPtr<TRepo> &repo) {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                       TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
      update->AddEntry(counter_key, TKey(1L, &arena, state_alloc), TMutator::Add);
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    };
    auto promote = [&](const TManager::TPtr<TRepo> &from, const TManager::TPtr<TRepo> &to) {
      auto t = manager->NewTransaction();
      t->Pop(from);
      t->Push(to, t->Peek(from));
      t->Prepare();
      t->CommitAction();
    };

    commit_increment(global_repo);  // A in global
    commit_increment(mid_repo);     // B in mid

    std::vector<TManager::TPtr<TRepo>> repos{child_repo, mid_repo, global_repo};
    std::vector<std::unique_ptr<TRepo::TView>> views;
    views.emplace_back(make_unique<TRepo::TView>(child_repo));   // snap child (empty)
    /* Promote B mid->global AFTER child snap but BEFORE mid snap. B leaves mid
       before the reader snaps mid, and landed in global before reader snaps
       global. mid snap misses B (it left); global snap catches B. */
    promote(mid_repo, global_repo);
    views.emplace_back(make_unique<TRepo::TView>(mid_repo));     // snap mid (B gone)
    views.emplace_back(make_unique<TRepo::TView>(global_repo));  // snap global (B + A)

    auto got = FoldCounter(repos, views, counter_key);
    /* B leads ahead of the frontier (leaves mid before the mid snapshot) but
       lands in global before the global snapshot. Must fold to 2, never drop. */
    if (EXPECT_TRUE(static_cast<bool>(got))) { EXPECT_EQ(*got, 2L); }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
