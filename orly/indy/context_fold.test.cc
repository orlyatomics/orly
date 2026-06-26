/* <orly/indy/context_fold.test.cc>

   Regression guard for the read-time commutative fold in
   <orly/indy/context.h> (TContext::TPresentWalker::ApplyDeferredFold) -- the
   #49/#60 lineage that issue #143 (the agent-swarm "got 1 want 2" undercount)
   points at.

   N independent transactions each `+= 1` the SAME key (each carrying a
   distinct per-transaction UpdateId, exactly as session.cc mints via
   TUuid::Twister). Reading the key through TContext folds that commutative run
   via ApplyDeferredFold; the result must equal N. The UpdateId-dedup added in
   PR #60 must NOT collapse these distinct increments. This pins the fold +
   dedup logic so a future change can't silently re-introduce the undercount.

   NB on scope: this is the deterministic, single-repo half of the fold path.
   The agent-swarm undercount itself is intermittent and could only be
   reproduced as a concurrency race (a read fiber observing partially-applied
   memory-layer state mid-write), which the engine offers no deterministic hook
   for; see the #143 PR notes. Every single-threaded interleaving of the fold +
   cross-repo dedup that was constructed by hand (including a read driven from
   inside the promotion push/pop window via the PR #145 commit hook) folds
   correctly, so the dedup/fold logic is not itself wrong.

   Deliberately light (a stub manager + in-memory repo, no compiler / durable
   manager / honcho), so it is deterministic and fast -- unlike
   context.test.broken.cc which stands up the full stack.

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

#include <base/test/kit.h>

/* NB: we intentionally do NOT `using namespace Orly;` here. context.h pulls in
   <orly/key_generator.h>, which declares a second `Orly::L0` namespace; with
   both `Orly` and `Orly::Indy` in scope, the bare `L0` we use everywhere below
   becomes ambiguous. Keeping only `Orly::Indy` in scope resolves `L0` -> the
   indy one. */
using namespace std;
using namespace std::literals;
using namespace Base;
using namespace Orly::Atom;
using namespace Orly::Indy;

/* Pull the handful of names that live directly in Orly:: (rather than
   Orly::Indy::) -- we deliberately do NOT `using namespace Orly` (see above). */
namespace Sabot = Orly::Sabot;
using Orly::TMutator;
using Orly::TTtl;

const Orly::Indy::TMasterContext::TProtocol Orly::Indy::TMasterContext::TProtocol::Protocol;
const Orly::Indy::TSlaveContext::TProtocol Orly::Indy::TSlaveContext::TProtocol::Protocol;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 100UL);

/* context.h transitively links the durable manager; these pool statics must
   be defined by exactly one TU in the test binary. */
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

/* A minimal L1::TManager that keeps everything in memory -- enough to create
   parent/child repos, run transactions, and walk via TContext. (Mirrors the
   stub in transaction_base.test.cc.) */
class TMyManager
    : public L1::TManager {
  NO_COPY(TMyManager);
  public:

  TMyManager(Disk::Util::TEngine *engine,
             Base::TScheduler *scheduler,
             const std::vector<size_t> &mem_merge_cores,
             const std::vector<size_t> &disk_merge_cores)
      : TManager(engine, 10ms, 100ms, true, true, 1000ms, scheduler,
                 100UL, 100UL, 20UL, mem_merge_cores, disk_merge_cores, true) {}

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
                                             bool is_safe,
                                             bool create) {
    return create ? OpenOrCreate(repo_id, ttl, parent_repo, is_safe) : ForceOpenRepo(repo_id);
  }

  using TManager::OpenOrCreate;

  private:

  std::mutex ReplicationQueueLock;
};

/* See file header: N distinct `+= 1` to one key must fold (via TContext) to
   exactly N -- the UpdateId dedup must not skip legitimately-distinct
   increments. */
FIXTURE(Issue143FoldUndercount) {
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
    /* Parentless safe repo: no Tetris player needed, fully deterministic. */
    auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true, true);

    const TIndexKey counter_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));

    /* Commit a single `+= 1` (TMutator::Add), each with a distinct
       per-transaction UpdateId (TUuid::Twister, as session.cc does). */
    auto commit_increment = [&]() {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                       TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
      update->AddEntry(counter_key, TKey(1L, &arena, state_alloc), TMutator::Add);
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    };

    /* Assert the folded counter (read via TContext) equals `want`. */
    auto expect_counter = [&](int64_t want) {
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      EXPECT_EQ(context[counter_key], TKey(want, &arena, state_alloc));
    };

    /* N independent agents each += 1 to the same key. */
    const int64_t kNumAgents = 5L;
    for (int64_t i = 0; i < kNumAgents; ++i) {
      commit_increment();
      expect_counter(i + 1L);
    }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Issue #151 commutative-upsert read.

   Exercises the statement-layer half of #151 directly at the engine: the
   ReadOrIdentity<TRet> helper (orly/key_generator.h) that codegen now
   emits for the LHS of a defer-safe, identity-default commutative
   mutation (Add/Or/Xor/Union/SymmetricDiff). Where the old Read<TRet>
   threw "Cannot de-reference Key ... which does not exist" on an absent
   key, ReadOrIdentity must instead resolve to the monoid identity (0 for
   ints) WITH the address kept known, so the downstream mutation effect
   still binds to the right key.

   This is what lets a bare first-write `*<[k]>::(int) += 1` on a fresh
   key reach session.cc's deferred-commutative path and emit {Add,1}
   (commutative) rather than throwing -- and, combined with the
   Issue143CreateRaceSemantics guard below (which already pins
   Add1_only_absentkey->1, Add1_then_Add1->2, Add1_then_Assign1->1),
   closes the create-race without a destructive Assign. */
FIXTURE(Issue151CommutativeUpsertRead) {
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
    auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true, true);

    const TIndexKey counter_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));

    /* (a) Absent key: ReadOrIdentity must NOT throw; it resolves to the
       identity (0) and keeps the address known (so an effect could bind). */
    {
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      auto m = Orly::Rt::ReadOrIdentity<int64_t>(context, make_tuple(1L), idx_id);
      EXPECT_EQ(m.GetVal(), 0L);
      EXPECT_TRUE(m.GetOptAddr().IsKnown());
    }

    /* Commit a single `+= 1` (Add) against the still-absent key -- exactly
       the deferred-commutative entry session.cc emits, with NO Assign
       base preceding it. */
    {
      auto transaction = manager->NewTransaction();
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                       TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
      update->AddEntry(counter_key, TKey(1L, &arena, state_alloc), TMutator::Add);
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }

    /* (b) After the commutative create, ReadOrIdentity reads the folded
       value (1), again with no throw. */
    {
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      auto m = Orly::Rt::ReadOrIdentity<int64_t>(context, make_tuple(1L), idx_id);
      EXPECT_EQ(m.GetVal(), 1L);
      EXPECT_TRUE(m.GetOptAddr().IsKnown());
      /* The plain operator[] read agrees (the #150 fold path). */
      EXPECT_EQ(context[counter_key], TKey(1L, &arena, state_alloc));
    }

    /* (c) IdentityAddr -- what the codegen actually emits for the
       commutative-upsert LHS. It hands back the address WITHOUT reading the
       current value (so it never throws and is O(1), regardless of how many
       writes are pending in the transaction); the absent-key seed happens at
       fold time, and callers consume only .GetAddr(). The address must be
       known whether or not the key already exists. */
    {
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      auto m = Orly::Rt::IdentityAddr<int64_t>(context, make_tuple(1L), idx_id);
      EXPECT_TRUE(m.GetOptAddr().IsKnown());
    }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Issue #143 create-race characterisation.

   The agent-swarm `add_mention` is a check-then-act: it emits `+= 1`
   (TMutator::Add) when the key `is known`, else `new <- 1` -- and `new`
   lowers to a TMutator::Assign entry (session.cc routes TNew through the
   op_by_key/Assign path, not the deferred-commutative path). Under
   concurrency the two branches can BOTH fire for one key when a writer's
   read snapshot predates a concurrent create: one session sees the key
   known and emits Add(1); another sees it absent and emits Assign(1).
   The SeqNum is assigned at AppendUpdate (commit) time
   (transaction_base.cc ~637), independent of when each session read, so
   the Assign can land at a HIGHER SeqNum than the Add.

   This fixture commits each ordering by hand (commit order == SeqNum
   order; the newest entry is the fold anchor) and pins the resulting
   read. The key finding for #143:

     * `Add(1)` against an absent key folds to 1 -- the commutative
       mutator already treats "missing" as the monoid identity (0). So
       the `new <-` create branch is not needed for correctness; an
       unconditional `+= 1` works on a fresh key.

     * A newer Assign MASKS older Add runs (Add(1) then Assign(1) reads
       as 1, not 2). This is the agent-swarm "got 1 want 2" undercount.
       It is NOT a fold bug: Assign is destructive/final by definition,
       so `x += 1; x <- 1` MUST read as 1. The fold is behaving
       correctly; the defect is that the racy check-then-act emits a
       destructive Assign for what is logically an initialise. The
       engine-clean idiom is unconditional `+= 1` (no `is known` / `new`
       branch); a deeper fix (a distinct create/upsert mutator that does
       not mask commutative history) is a separate, larger indy change.

   We therefore pin the SEMANTICALLY CORRECT values (Assign is
   destructive), so this guard fails if a future change "fixes" #143 by
   making Assign non-destructive -- which would corrupt legitimate
   assign-after-increment. */
FIXTURE(Issue143CreateRaceSemantics) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Base::TUuid idx_id(TUuid::Twister);
    const TIndexKey counter_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));

    /* Each scenario gets a FRESH repo so prior entries don't leak. The
       order of `entries` is the COMMIT order: earliest first => lowest
       SeqNum. At read time the heap delivers highest SeqNum (newest)
       first, so the LAST entry committed is the fold anchor. */
    auto run_scenario = [&](const char *name,
                            const std::vector<std::pair<TMutator, int64_t>> &entries,
                            int64_t want) {
      Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
      auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
      Base::TUuid repo_id(TUuid::Twister);
      auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true, true);
      for (const auto &e : entries) {
        auto transaction = manager->NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                         TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
        update->AddEntry(counter_key, TKey(e.second, &arena, state_alloc), e.first);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      }
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      EXPECT_EQ(context[counter_key], TKey(want, &arena, state_alloc));
    };

    /* Commit order is oldest->newest (newest = anchor). `want` is the
       SEMANTICALLY CORRECT value under destructive-Assign semantics. */

    /* `+= 1` on a totally absent key already folds to 1 (identity 0):
       the create branch is unnecessary for a fresh counter. */
    run_scenario("Add1_only_absentkey",     {{TMutator::Add,1}},                        1);
    /* Two blind creates of the same value: latest Assign wins -> 1.
       (A genuine user-level lost update; both writers said "set to 1".) */
    run_scenario("Assign1_then_Assign1",     {{TMutator::Assign,1},{TMutator::Assign,1}},1);
    /* Add anchored above an Assign base: folds base + acc -> 2. */
    run_scenario("Assign1_then_Add1",        {{TMutator::Assign,1},{TMutator::Add,1}},   2);
    /* THE #143 ORDERING: Add then a newer Assign. Assign is destructive
       and masks the older increment -> 1. Correct for assignment; the
       agent-swarm undercount is this case, caused by `new <-` emitting a
       destructive Assign rather than an initialise. */
    run_scenario("Add1_then_Assign1",        {{TMutator::Add,1},{TMutator::Assign,1}},   1);
    /* Two distinct deferred increments fold -> 2 (the happy path the
       demo wants; achieved when both writers take the `+=` branch). */
    run_scenario("Add1_then_Add1",           {{TMutator::Add,1},{TMutator::Add,1}},      2);
    /* Assign base + two increments -> 3. */
    run_scenario("Assign1_Add1_Add1",        {{TMutator::Assign,1},{TMutator::Add,1},{TMutator::Add,1}}, 3);
    /* Newer Assign masks two older increments -> 1 (destructive). */
    run_scenario("Add1_Add1_then_Assign1",   {{TMutator::Add,1},{TMutator::Add,1},{TMutator::Assign,1}}, 1);

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Issue #213: the min (`<?=`) / max (`>?=`) merge mutators fold through the
   exact same deferred-commutative read path as `+=` (ApplyDeferredFold).
   min/max are commutative, associative AND idempotent, so a run of same-key
   entries collapses to a single value regardless of order, and a first
   entry on an absent key seeds from its own RHS (the singleton fold ==
   the element). This mirrors Issue143CreateRaceSemantics but pins the
   min/max results, exercising the real indy memory/disk fold rather than
   just the var-layer TMutation::Apply unit. */
FIXTURE(MinMaxDeferredFold) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Base::TUuid idx_id(TUuid::Twister);
    const TIndexKey gauge_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));

    auto run_scenario = [&](const char *name,
                            const std::vector<std::pair<TMutator, int64_t>> &entries,
                            int64_t want) {
      Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
      auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
      Base::TUuid repo_id(TUuid::Twister);
      auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true, true);
      for (const auto &e : entries) {
        auto transaction = manager->NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                         TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
        update->AddEntry(gauge_key, TKey(e.second, &arena, state_alloc), e.first);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      }
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      EXPECT_EQ(context[gauge_key], TKey(want, &arena, state_alloc));
    };

    /* Single min/max on a totally absent key seeds from the RHS. */
    run_scenario("Min7_only_absentkey",   {{TMutator::Min,7}},                          7);
    run_scenario("Max3_only_absentkey",   {{TMutator::Max,3}},                          3);
    /* Two deferred mins/maxes fold; idempotent + commutative => the bound
       wins regardless of commit order. */
    run_scenario("Min7_then_Min3",        {{TMutator::Min,7},{TMutator::Min,3}},        3);
    run_scenario("Min3_then_Min7",        {{TMutator::Min,3},{TMutator::Min,7}},        3);
    run_scenario("Max3_then_Max7",        {{TMutator::Max,3},{TMutator::Max,7}},        7);
    run_scenario("Max7_then_Max3",        {{TMutator::Max,7},{TMutator::Max,3}},        7);
    /* Three-deep folds. */
    run_scenario("Min_5_2_9",             {{TMutator::Min,5},{TMutator::Min,2},{TMutator::Min,9}}, 2);
    run_scenario("Max_5_9_2",             {{TMutator::Max,5},{TMutator::Max,9},{TMutator::Max,2}}, 9);
    /* Assign base then a min/max above it folds against the base. */
    run_scenario("Assign10_then_Min3",    {{TMutator::Assign,10},{TMutator::Min,3}},    3);
    run_scenario("Assign10_then_Min30",   {{TMutator::Assign,10},{TMutator::Min,30}},   10);
    run_scenario("Assign2_then_Max9",     {{TMutator::Assign,2},{TMutator::Max,9}},     9);
    /* A newer destructive Assign masks older min/max history (same
       destructive semantics pinned in Issue143CreateRaceSemantics). */
    run_scenario("Min3_then_Assign10",    {{TMutator::Min,3},{TMutator::Assign,10}},    10);

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* Issue #213 PR2: mult (`*=`) absent-key upsert through the real indy
   fold. Mult was already defer-safe for existing keys; the only change is
   that a first entry on an absent key now seeds from its RHS (1 * r == r)
   instead of throwing. Identical fold mechanism to Add, exercised here for
   the mult op specifically. */
FIXTURE(MultDeferredFold) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Base::TUuid idx_id(TUuid::Twister);
    const TIndexKey prod_key(idx_id, TKey(make_tuple(1L), &arena, state_alloc));

    auto run_scenario = [&](const char *name,
                            const std::vector<std::pair<TMutator, int64_t>> &entries,
                            int64_t want) {
      Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 64, 128, 1, 64, 1);
      auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler, MemMergeCoreVec, DiskMergeCoreVec);
      Base::TUuid repo_id(TUuid::Twister);
      auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true, true);
      for (const auto &e : entries) {
        auto transaction = manager->NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena),
                                         TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
        update->AddEntry(prod_key, TKey(e.second, &arena, state_alloc), e.first);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      }
      TSuprena ctx_arena;
      TContext context(repo, &ctx_arena);
      EXPECT_EQ(context[prod_key], TKey(want, &arena, state_alloc));
    };

    /* Single `*= 6` on a totally absent key seeds from the RHS (== 1*6). */
    run_scenario("Mult6_only_absentkey",   {{TMutator::Mult,6}},                          6);
    /* Two deferred mults fold: 6 * 3 = 18. */
    run_scenario("Mult6_then_Mult3",       {{TMutator::Mult,6},{TMutator::Mult,3}},        18);
    /* Three-deep: 2 * 3 * 5 = 30 regardless of commit order. */
    run_scenario("Mult_2_3_5",             {{TMutator::Mult,2},{TMutator::Mult,3},{TMutator::Mult,5}}, 30);
    /* Assign base then a mult folds against the base: 4 * 5 = 20. */
    run_scenario("Assign4_then_Mult5",     {{TMutator::Assign,4},{TMutator::Mult,5}},       20);
    /* A newer destructive Assign masks older mult history. */
    run_scenario("Mult6_then_Assign10",    {{TMutator::Mult,6},{TMutator::Assign,10}},      10);

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
