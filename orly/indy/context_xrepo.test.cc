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
#include <orly/server/tetris_manager.h>
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
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 500UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 500UL);

const std::vector<size_t> MemMergeCoreVec{0};
const std::vector<size_t> DiskMergeCoreVec{0};

class TMyManager : public L1::TManager {
  NO_COPY(TMyManager);
  public:
  TMyManager(Disk::Util::TEngine *engine, Base::TScheduler *scheduler,
             const std::vector<size_t> &mem_merge_cores, const std::vector<size_t> &disk_merge_cores)
      : TManager(engine, 10ms, 100ms, true, true, true, 1000ms, scheduler, 100UL, 100UL, 20UL, mem_merge_cores, disk_merge_cores, true) {}
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

/* A Tetris manager whose players never play, so a test can write to a child repo (a child's first
   write joins its parent's player) and promote by hand. It is not the master, so each player
   waits for permission to work until its last child parts, and then exits. */
class TInertTetrisManager final
    : public Orly::Server::TTetrisManager {
  public:
  TInertTetrisManager(TScheduler *scheduler, Fiber::TRunner::TRunnerCons &runner_cons,
                      Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager)
      : TTetrisManager(scheduler, runner_cons, frame_pool_manager, [](Fiber::TRunner *) {}, /* is_master */ false) {}
  virtual ~TInertTetrisManager() {
    StopAllPlayers();
  }
  virtual TPlayer *NewPlayer(const TUuid &, const TUuid &, bool is_paused, bool is_master) override {
    return new TInertPlayer(this, is_paused, is_master);
  }
  private:
  class TInertPlayer final
      : public TPlayer {
    public:
    TInertPlayer(TTetrisManager *tetris_manager, bool is_paused, bool is_master)
        : TPlayer(tetris_manager) {
      Start(is_paused, is_master);
    }
    virtual void OnJoin(const TUuid &) override {}
    virtual void OnPart(const TUuid &) override {}
    virtual void OnPause() override {}
    virtual void OnUnpause() override {}
    virtual void Play() override {}
  };
};

/* #791: a POV reads its own writes over its ancestors' entries for the same key, deletes
   included, before Tetris promotes them.

   Sequence numbers are per repo (sequence_number.h), so an ancestor's entry for a key can carry
   a higher number than the child's newer delete or overwrite of it. The context's merge across
   the repo chain must therefore prefer the repo nearest the POV for a key, not the higher
   number. Here `root` holds keys (4, 0..5) and an int counter under (9, *), padded so its
   numbers run well past the child's; `pov` (child of root) and `reader` (child of pov) read
   through the chain while pov's writes are unpromoted, then after they are promoted. */
FIXTURE(Issue791ChildDeleteMasksAncestor) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    TSuprena arena;
    void *state = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> tetris_frames(10UL, 1024UL * 1024UL, nullptr);
    TInertTetrisManager tetris(&scheduler, runner_cons, &tetris_frames);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
    manager->SetTetrisManager(&tetris);
    for (bool is_safe : {false, true}) {
      const Base::TUuid idx_id(TUuid::Twister);
      auto root = manager->GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, is_safe, true);
      auto pov = manager->GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), root, is_safe, true);
      auto reader = manager->GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), pov, is_safe, true);
      const auto key = [&](int64_t g, int64_t e) {
        return TIndexKey(idx_id, TKey(make_tuple(g, e), &arena, state));
      };
      const auto commit = [&](const TManager::TPtr<TRepo> &repo, const TIndexKey &k, const TKey &op, TMutator mutator) {
        auto t = manager->NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(k, op, mutator);
        t->Push(repo, update); t->Prepare(); t->CommitAction();
      };
      const auto put = [&](const TManager::TPtr<TRepo> &repo, int64_t g, int64_t e, int64_t v) {
        commit(repo, key(g, e), TKey(v, &arena, state), TMutator::Assign);
      };
      const auto add = [&](const TManager::TPtr<TRepo> &repo, int64_t g, int64_t e, int64_t v) {
        commit(repo, key(g, e), TKey(v, &arena, state), TMutator::Add);
      };
      const auto remove = [&](const TManager::TPtr<TRepo> &repo, int64_t g, int64_t e) {
        commit(repo, key(g, e), TKey(Orly::Native::TTombstone::Tombstone, &arena, state), TMutator::Assign);
      };
      /* Move every unpromoted update from `from` to `to`, as Tetris would. */
      const auto promote_all = [&](const TManager::TPtr<TRepo> &from, const TManager::TPtr<TRepo> &to) {
        for (;;) {
          {
            TRepo::TView view(from);
            if (!view.GetLower()) {
              break;
            }
          }
          auto t = manager->NewTransaction();
          t->Pop(from); t->Push(to, t->Peek(from)); t->Prepare(); t->CommitAction();
        }
      };
      /* The value at (g, e) through `repo`, or nullopt if the read finds nothing; checks that
         Exists agrees. */
      const auto read = [&](const TManager::TPtr<TRepo> &repo, int64_t g, int64_t e) -> std::string {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        const TKey got = context[key(g, e)];
        const bool exists = context.Exists(key(g, e));
        if (!got.GetArena()) {
          EXPECT_FALSE(exists);
          return "absent";
        }
        EXPECT_TRUE(exists);
        void *sa = alloca(Sabot::State::GetMaxStateSize());
        return std::to_string(Orly::Var::TVar::TDt<int64_t>::As(Orly::Var::ToVar(*Sabot::State::TAny::TWrapper(got.GetCore().NewState(got.GetArena(), sa)))));
      };
      /* The e's a key cursor yields, for the pattern (g, free) or the range [(g, lo), (g, hi)]. */
      const auto walk = [&](TContext::TKeyCursor &csr) {
        std::string out = "[";
        for (; csr; ++csr) {
          void *sa = alloca(Sabot::State::GetMaxStateSize());
          const auto v = Orly::Var::TVar::TDt<std::tuple<int64_t, int64_t>>::As(
              Orly::Var::ToVar(*Sabot::State::TAny::TWrapper((*csr).GetCore().NewState((*csr).GetArena(), sa))));
          out += (out.size() > 1 ? "," : "") + std::to_string(std::get<1>(v));
        }
        return out + "]";
      };
      const auto keys = [&](const TManager::TPtr<TRepo> &repo, int64_t g) {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        TContext::TKeyCursor csr(&context, TIndexKey(idx_id, TKey(make_tuple(g, Orly::Native::TFree<int64_t>()), &arena, state)));
        return walk(csr);
      };
      const auto range = [&](const TManager::TPtr<TRepo> &repo, int64_t g, int64_t lo, int64_t hi) {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        TContext::TKeyCursor csr(&context, key(g, lo), key(g, hi));
        return walk(csr);
      };

      /* The ancestor's data, numbered well past anything the child will reach. */
      for (int64_t e = 0; e < 6; ++e) {
        put(root, 4, e, e * 10);
      }
      for (int64_t i = 0; i < 20; ++i) {
        put(root, 8, i, i);
      }
      put(root, 9, 0, 10);  // counter: 10 + 5 = 15
      add(root, 9, 0, 5);
      put(root, 9, 1, 10);  // counter the pov will delete, then add to
      add(root, 9, 1, 5);
      add(root, 9, 2, 7);   // commutative-only history the pov will overwrite
      add(root, 9, 2, 7);
      EXPECT_EQ(keys(pov, 4), "[0,1,2,3,4,5]");

      /* The pov's writes, unpromoted: delete an ancestor-held key, overwrite another, delete and
         re-insert a third, insert and delete one of its own, and add to / delete / overwrite the
         ancestor's counters. */
      remove(pov, 4, 4);
      put(pov, 4, 3, 333);
      remove(pov, 4, 1);
      put(pov, 4, 1, 111);
      put(pov, 4, 100, 0);
      put(pov, 4, 50, 5);
      remove(pov, 4, 50);
      add(pov, 9, 0, 1);
      remove(pov, 9, 1);
      add(pov, 9, 1, 2);
      put(pov, 9, 2, 100);

      for (const auto &through : {pov, reader}) {
        EXPECT_EQ(read(through, 4, 4), "absent");
        EXPECT_EQ(read(through, 4, 3), "333");
        EXPECT_EQ(read(through, 4, 1), "111");
        EXPECT_EQ(read(through, 4, 5), "50");
        EXPECT_EQ(read(through, 4, 50), "absent");
        EXPECT_EQ(keys(through, 4), "[0,1,2,3,5,100]");
        /* The range starts at a key the pov holds: on a master before #793, a memory layer's
           range walk whose first key in range lies past `from` yields nothing at all. */
        EXPECT_EQ(range(through, 4, 3, 5), "[3,5]");
        EXPECT_EQ(range(through, 4, 4, 4), "[]");
        EXPECT_EQ(read(through, 9, 0), "16");
        EXPECT_EQ(read(through, 9, 1), "2");
        EXPECT_EQ(read(through, 9, 2), "100");
      }
      /* The ancestor alone is unchanged. */
      EXPECT_EQ(read(root, 4, 4), "40");
      EXPECT_EQ(read(root, 4, 3), "30");
      EXPECT_EQ(keys(root, 4), "[0,1,2,3,4,5]");
      EXPECT_EQ(range(root, 4, 3, 5), "[3,4,5]");
      EXPECT_EQ(read(root, 9, 0), "15");
      EXPECT_EQ(read(root, 9, 1), "15");
      EXPECT_EQ(read(root, 9, 2), "14");

      /* Once promoted, every level agrees. */
      promote_all(pov, root);
      for (const auto &through : {root, pov, reader}) {
        EXPECT_EQ(read(through, 4, 4), "absent");
        EXPECT_EQ(read(through, 4, 3), "333");
        EXPECT_EQ(read(through, 4, 1), "111");
        EXPECT_EQ(keys(through, 4), "[0,1,2,3,5,100]");
        EXPECT_EQ(range(through, 4, 3, 5), "[3,5]");
        EXPECT_EQ(read(through, 9, 0), "16");
        EXPECT_EQ(read(through, 9, 1), "2");
        EXPECT_EQ(read(through, 9, 2), "100");
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, /* extra_runners, for Tetris */ 1);
}
