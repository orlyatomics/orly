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
