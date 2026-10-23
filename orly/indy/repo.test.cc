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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <random>
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
#include <orly/sabot/to_native.h>
#include <orly/server/read_too_large.h>
#include <orly/server/version_unavailable.h>

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
/* RangeCountDiskBenchmark (opt-in) writes its rows in updates of up to CountBenchChunk entries.
   Nothing here frees a flushed memory layer, so the pool holds every row, plus a chunk of slack
   for the merges. */
static const size_t CountBenchRows = [] {
  const char *rows = std::getenv("ORLY_COUNT_BENCH_ROWS");
  return rows ? std::stoull(rows) : 0UL;
}();
static const size_t CountBenchChunk = std::min<size_t>(CountBenchRows, 250000UL);
/* RangeCountsByRankMatchTheWalk writes every key of its group once, and about as many again. */
static constexpr size_t RankCountKeys = 6000UL;
/* ReadersSeeWholeUpdatesWhileAppending appends AppendStressUpdates updates of AppendStressEntries entries each. */
static constexpr size_t AppendStressUpdates = 24UL;
static constexpr size_t AppendStressEntries = 2000UL;
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 20000UL + (2UL * RankCountKeys) + CountBenchRows + CountBenchChunk + (AppendStressUpdates * AppendStressEntries));

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

  using L0::TManager::TRepo::GetTimeOfNextMergeMem;

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

  /* Run one memory merge, as the merge runner would. */
  void StepMem() {
    StepMergeMem();
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
/* #745 stage 3: a retention record store in memory, standing in for the durable manager. While Hold is set, saves
   wait in Held, off the disk, until Release. With Ships off, the node is a slave. */
struct TRetentionStoreSim {
  std::mutex Mutex;
  std::map<Base::TUuid, std::string> Records;
  std::vector<std::tuple<Base::TUuid, std::string, Base::TEventSemaphore *>> Held;
  bool Hold = false;
  bool Ships = true;
  size_t Saves = 0UL;

  void Release() {
    std::lock_guard<std::mutex> lock(Mutex);
    for (auto &[id, blob, sem] : Held) {
      Records[id] = blob;
      if (sem) {
        sem->Push();
      }
    }
    Held.clear();
    Hold = false;
  }
};

class TMyManager
    : public L1::TManager {
  NO_COPY(TMyManager);
  public:

  TMyManager(Disk::Util::TEngine *engine, Base::TScheduler *scheduler, bool prune_merge_history = true, size_t max_repo_cache_size = 100UL)
      : TManager(engine, 1h, 1h, true, prune_merge_history, true, 1000ms, scheduler,
                 100UL, max_repo_cache_size, 20UL, MemMergeCoreVec, DiskMergeCoreVec, true) {}

  using L0::TManager::DiscardExpiredObjs;
  using L0::TManager::IsLiveRepo;
  using L0::TManager::IsMergeDiskQueued;

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
  virtual TRepo *ReconstructRepo(const Base::TUuid &repo_id) override {
    /* With a record store, a restart (TRootRepoFixture::Restart) reloads the repo from the engine's files. */
    return RetentionSim ? TSafeRepo::ReConstructFromDisk(this, repo_id, L0::TDeadline::clock::now() + std::chrono::seconds(1000)) : nullptr;
  }
  virtual void RunReplicationQueue() override {}
  virtual void RunReplicationWork() override {}
  virtual void RunReplicateTransaction() override {}
  virtual std::mutex &GetReplicationQueueLock() NO_THROW override { return ReplicationQueueLock; }

  virtual bool HasRetentionStore() const noexcept override {
    return RetentionSim != nullptr;
  }

  virtual bool TryLoadRetentionRecord(const Base::TUuid &id, std::string &blob) override {
    if (!RetentionSim) {
      return false;
    }
    std::lock_guard<std::mutex> lock(RetentionSim->Mutex);
    const auto pos = RetentionSim->Records.find(id);
    if (pos == RetentionSim->Records.end()) {
      return false;
    }
    blob = pos->second;
    return true;
  }

  virtual bool TrySaveRetentionRecord(const Base::TUuid &id, const std::string &blob, Base::TEventSemaphore *sem, bool /*replicate*/) override {
    if (!RetentionSim) {
      return false;
    }
    std::lock_guard<std::mutex> lock(RetentionSim->Mutex);
    ++RetentionSim->Saves;
    if (RetentionSim->Hold) {
      RetentionSim->Held.emplace_back(id, blob, sem);
    } else {
      RetentionSim->Records[id] = blob;
      if (sem) {
        sem->Push();
      }
    }
    return true;
  }

  virtual bool ShipsRetention() const noexcept override {
    return !RetentionSim || RetentionSim->Ships;
  }

  TRetentionStoreSim *RetentionSim = nullptr;

  inline TManager::TPtr<TRepo> GetRepo(const Base::TUuid &repo_id,
                                       const std::optional<TTtl> &ttl,
                                       const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                                       bool is_safe) {
    return OpenOrCreate(repo_id, ttl, parent_repo, is_safe);
  }

  using L0::TManager::TryOpenLiveRepo;

  TManager::TPtr<TRepo> Reopen(const Base::TUuid &repo_id) {
    return ForceOpenRepo(repo_id);
  }

  private:

  std::mutex ReplicationQueueLock;
};

/* Opt-in measurements of counts over keys that live in disk files (#749); normal test runs do
   not build the data set. Writes ORLY_COUNT_BENCH_ROWS keys <[1, i]> in chunks, flushes each
   chunk to its own file, merges the files, then times key-only counts. Run 0 of each kind is
   the first count after the files were written. It sits here, ahead of the fixtures that
   exhaust the shared pools on purpose. */
FIXTURE(RangeCountDiskBenchmark) {
  if (!CountBenchRows) {
    return;
  }
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TSuprena arena;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    /* Room for the files and one merge's output. */
    const size_t num_mb = std::max<size_t>(256UL, CountBenchRows * 600UL / 1000000UL + 256UL);
    Disk::Sim::TMemEngine engine(&scheduler, num_mb, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      auto repo = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      auto *stepped = dynamic_cast<TSteppedSafeRepo *>(repo.Get());
      const Base::TUuid index_id(TUuid::Twister);
      const auto key = [&](int64_t n) { return TIndexKey(index_id, TKey(make_tuple(1L, n), &arena, state)); };
      const auto load_begin = std::chrono::steady_clock::now();
      for (size_t base = 0; base < CountBenchRows; base += CountBenchChunk) {
        TSuprena update_arena;
        /* The update lands in the repo when the transaction goes away. */ {
          auto transaction = manager.NewTransaction();
          auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&update_arena), TKey(Base::TUuid(TUuid::Twister), &update_arena, state));
          for (size_t i = base; i < std::min(CountBenchRows, base + CountBenchChunk); ++i) {
            update->AddEntry(TIndexKey(index_id, TKey(make_tuple(1L, static_cast<int64_t>(i)), &update_arena, state)),
                             TKey(static_cast<int64_t>(i), &update_arena, state));
          }
          transaction->Push(repo, update);
          transaction->Prepare();
          transaction->CommitAction();
        }
        stepped->StepMergeMem();
      }
      const auto flush_end = std::chrono::steady_clock::now();
      /* count */ {
        size_t d = 0UL, m = 0UL;
        Orly::Indy::TRepo::TView view(stepped);
        view.CountLayers(d, m);
        std::printf("RangeCountDiskBenchmark rows=%zu after_flush disk_layers=%zu mem_layers=%zu\n", CountBenchRows, d, m);
      }
      for (int i = 0; i < 64; ++i) {
        stepped->StepMergeDisk(256UL);
      }
      const auto merge_end = std::chrono::steady_clock::now();
      size_t disk_layers = 0UL, mem_layers = 0UL;
      /* count */ {
        Orly::Indy::TRepo::TView view(stepped);
        view.CountLayers(disk_layers, mem_layers);
      }
      std::printf("RangeCountDiskBenchmark rows=%zu disk_layers=%zu mem_layers=%zu flush_ms=%ld merge_ms=%ld\n", CountBenchRows, disk_layers, mem_layers,
                  static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(flush_end - load_begin).count()),
                  static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(merge_end - flush_end).count()));
      const TIndexKey all(index_id, TKey(make_tuple(1L, Orly::Native::TFree<int64_t>()), &arena, state));
      const auto rows = static_cast<int64_t>(CountBenchRows);
      const int64_t small = std::min<int64_t>(rows, 100L);
      const auto time_count = [&](const char *label, int run, int64_t expected, const std::function<int64_t (TContext &)> &count) {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        const auto begin = std::chrono::steady_clock::now();
        const int64_t got = count(context);
        const auto end = std::chrono::steady_clock::now();
        EXPECT_EQ(got, expected);
        std::printf("RangeCountDiskBenchmark rows=%zu run=%d %s_us=%ld\n", CountBenchRows, run, label,
                    static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()));
      };
      for (int run = 0; run < 7; ++run) {
        time_count("pattern_all", run, rows, [&](TContext &context) { return context.CountKeys(all); });
      }
      for (int run = 0; run < 7; ++run) {
        time_count("range_half", run, rows / 2, [&](TContext &context) { return context.CountKeys(key(rows / 4), key(rows / 4 + rows / 2 - 1)); });
      }
      for (int run = 0; run < 7; ++run) {
        time_count("range_100", run, small, [&](TContext &context) { return context.CountKeys(key(rows / 2 - small / 2), key(rows / 2 - small / 2 + small - 1)); });
      }
      for (int run = 0; run < 7; ++run) {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        const auto point = key(rows - 1);
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 10000; ++i) {
          EXPECT_TRUE(context.Exists(point));
        }
        const auto end = std::chrono::steady_clock::now();
        std::printf("RangeCountDiskBenchmark rows=%zu run=%d point_reads_us=%ld\n", CountBenchRows, run,
                    static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()));
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* AppendUpdate links an update's entries into the memory layer outside DataLock and publishes the
   sequence number afterwards, so a reader must see exactly the updates up to the HighestSeqNum it
   snapshotted: all of each, none of a later one. One thread appends big updates of unique keys.
   With readers, other threads snapshot and walk the whole key range meanwhile (memory layers only:
   a plain thread cannot wait on the disk, so nothing is merged). Without, this fiber thread seals
   and merges the memory layer (StepMergeMem) meanwhile, since the seal must not land between an
   append's link and its publish; the whole key range is then checked once the appender is done. */
static void AppendStress(bool with_readers, bool bump_numbers = false) {
  Fiber::TFiberTestRunner runner([with_readers, bump_numbers](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    const TScheduler::TPolicy scheduler_policy(10, 10, 10ms);
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Orly::Indy::Disk::Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    auto manager = make_unique<TMyManager>(mem_engine.GetEngine(), &scheduler);
    const Base::TUuid repo_id(TUuid::Twister);
    const Base::TUuid idx_id(TUuid::Twister);
    auto repo = manager->GetRepo(repo_id, TTtl::max(), std::nullopt, true);
    auto *stepped = dynamic_cast<TSteppedSafeRepo *>(repo.Get());
    if (!EXPECT_TRUE(stepped != nullptr)) {
      std::lock_guard<std::mutex> lock(mut);
      fin = true;
      cond.notify_one();
      return;
    }
    std::atomic<bool> appending(true);
    std::atomic<size_t> num_walks(0UL), num_bad(0UL), num_partial_seen(0UL);
    std::atomic<size_t> first_bad_high(0UL), first_bad_count(0UL);
    std::thread appender([&] {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      for (size_t n = 0UL; n < AppendStressUpdates; ++n) {
        TSuprena arena;
        auto transaction = manager->NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state_alloc));
        for (size_t j = 0UL; j < AppendStressEntries; ++j) {
          const auto key = static_cast<int64_t>(n * AppendStressEntries + j);
          update->AddEntry(TIndexKey(idx_id, TKey(make_tuple(key), &arena, state_alloc)), TKey(key, &arena, state_alloc), TMutator::Assign);
        }
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      }
      appending = false;
    });
    vector<std::thread> readers;
    for (size_t r = 0UL; with_readers && r < 4UL; ++r) {
      readers.emplace_back([&] {
        TRunnerFileCaches reader_caches;
        TSuprena arena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        const TIndexKey from(idx_id, TKey(make_tuple(static_cast<int64_t>(0)), &arena, state_alloc));
        const TIndexKey to(idx_id, TKey(make_tuple(std::numeric_limits<int64_t>::max()), &arena, state_alloc));
        const auto walk = [&] {
          auto view = make_unique<Orly::Indy::TRepo::TView>(stepped);
          const TSequenceNumber high = view->GetUpper().value_or(0UL);
          size_t count = 0UL;
          TSequenceNumber max_seq = 0UL;
          auto walker_ptr = stepped->NewPresentWalker(view, from, to);
          for (TPresentWalker &walker = *walker_ptr; walker; ++walker) {
            ++count;
            max_seq = std::max(max_seq, (*walker).SequenceNumber);
          }
          /* Updates 1..high, each whole; nothing newer than the snapshot, and not part of one. */
          if (count != high * AppendStressEntries || max_seq > high) {
            if (num_bad++ == 0UL) {
              first_bad_high = high;
              first_bad_count = count;
            }
          }
          if (count % AppendStressEntries) {
            ++num_partial_seen;
          }
          ++num_walks;
        };
        while (appending) {
          walk();
        }
        walk();
      });
    }
    while (appending) {
      if (bump_numbers) {
        /* The importer's numbering moves must wait for an append in flight, not land inside it. */
        stepped->UseSequenceNumbers(1UL);
      } else if (!with_readers) {
        stepped->StepMergeMem();
      }
      std::this_thread::sleep_for(1ms);
    }
    appender.join();
    for (auto &reader : readers) {
      reader.join();
    }
    std::printf("AppendStress readers=%d walks=%zu bad=%zu partial=%zu first_bad_high=%zu first_bad_count=%zu\n",
                with_readers,
                num_walks.load(), num_bad.load(), num_partial_seen.load(), first_bad_high.load(), first_bad_count.load());
    EXPECT_EQ(num_bad.load(), 0UL);
    EXPECT_TRUE(!with_readers || num_walks.load() > 100UL);
    /* At the end every entry is there. */ {
      auto view = make_unique<Orly::Indy::TRepo::TView>(stepped);
      EXPECT_TRUE(bump_numbers || *view->GetUpper() == AppendStressUpdates);
      TSuprena arena;
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      const TIndexKey from(idx_id, TKey(make_tuple(static_cast<int64_t>(0)), &arena, state_alloc));
      const TIndexKey to(idx_id, TKey(make_tuple(std::numeric_limits<int64_t>::max()), &arena, state_alloc));
      size_t count = 0UL;
      auto walker_ptr = stepped->NewPresentWalker(view, from, to);
      for (TPresentWalker &walker = *walker_ptr; walker; ++walker) {
        ++count;
      }
      EXPECT_EQ(count, AppendStressUpdates * AppendStressEntries);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(ReadersSeeWholeUpdatesWhileAppending) {
  AppendStress(true);
}

FIXTURE(SealDoesNotSplitAnAppend) {
  AppendStress(false);
}

FIXTURE(NumberingMovesDoNotSplitAnAppend) {
  AppendStress(false, true);
}

/* #749: a count passes over by rank each stretch of keys only one disk file holds. Build a root
   with several disk files (a base file, overwrites, tombstones, `+=` entries, and one merge) plus
   memory, and a paused child with its own file and memory, then check over random ranges and
   patterns, from the child, from the root, and from a context pinned before the later writes,
   that the count by rank equals the walk and the cursor, and that a read budget refuses it in
   the same place with the same rows charged. */
FIXTURE(RangeCountsByRankMatchTheWalk) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TSuprena arena;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      const Base::TUuid index_id(TUuid::Twister);
      auto root = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      auto child = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), root, true);
      /* Keep the child's updates unpromoted; this fixture has no Tetris service. */ {
        auto transaction = manager.NewTransaction();
        transaction->Pause(child);
        transaction->Prepare();
        transaction->CommitAction();
      }
      /* Keys <[g, n]>: group 1 holds most of them, groups 0 and 2 a few on either side. */
      constexpr int64_t Keys = static_cast<int64_t>(RankCountKeys);
      const auto key = [&](int64_t g, int64_t n) { return TIndexKey(index_id, TKey(make_tuple(g, n), &arena, state)); };
      const auto group = [&](int64_t g) { return TIndexKey(index_id, TKey(make_tuple(g, Orly::Native::TFree<int64_t>()), &arena, state)); };
      std::mt19937_64 rng(749);
      enum class TOp { Assign, Tombstone, Add };
      /* One update of the given ops on distinct keys of group 1. */
      const auto commit = [&](const L0::TManager::TPtr<TRepo> &repo, const vector<pair<int64_t, TOp>> &ops) {
        TSuprena update_arena;
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&update_arena), TKey(Base::TUuid(TUuid::Twister), &update_arena, state));
        for (const auto &[n, op] : ops) {
          const TIndexKey index_key(index_id, TKey(make_tuple(1L, n), &update_arena, state));
          switch (op) {
            case TOp::Assign: update->AddEntry(index_key, TKey(n, &update_arena, state)); break;
            case TOp::Tombstone: update->AddEntry(index_key, TKey(Orly::Native::TTombstone::Tombstone, &update_arena, state)); break;
            case TOp::Add: update->AddEntry(index_key, TKey(1L, &update_arena, state), TMutator::Add); break;
          }
        }
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      const auto random_ops = [&](size_t count) {
        std::set<int64_t> picked;
        std::uniform_int_distribution<int64_t> pick(0L, Keys - 1L);
        while (picked.size() < count) {
          picked.insert(pick(rng));
        }
        vector<pair<int64_t, TOp>> ops;
        std::uniform_int_distribution<int> kind(0, 9);
        for (int64_t n : picked) {
          const int k = kind(rng);
          ops.emplace_back(n, k < 5 ? TOp::Assign : k < 8 ? TOp::Tombstone : TOp::Add);
        }
        return ops;
      };
      const auto flush = [&](const L0::TManager::TPtr<TRepo> &repo) {
        dynamic_cast<TSteppedSafeRepo *>(repo.Get())->StepMergeMem();
      };
      /* The base file: every key of group 1, but every seventh a tombstone, and a few keys of groups 0 and 2. */ {
        vector<pair<int64_t, TOp>> ops;
        for (int64_t n = 0; n < Keys; ++n) {
          ops.emplace_back(n, n % 7 == 3 ? TOp::Tombstone : TOp::Assign);
        }
        commit(root, ops);
        TSuprena update_arena;
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&update_arena), TKey(Base::TUuid(TUuid::Twister), &update_arena, state));
        for (int64_t g : {0L, 2L}) {
          for (int64_t n = 0; n < 50; ++n) {
            update->AddEntry(TIndexKey(index_id, TKey(make_tuple(g, n * 100L), &update_arena, state)), TKey(n, &update_arena, state));
          }
        }
        transaction->Push(root, update);
        transaction->Prepare();
        transaction->CommitAction();
        flush(root);
      }
      for (int round = 0; round < 2; ++round) {
        commit(root, random_ops(300));
        flush(root);
      }
      /* A context pinned here keeps these files, and must not count by rank a file written after it. */
      TSuprena pinned_arena;
      TContext pinned(child, &pinned_arena);
      for (int round = 0; round < 2; ++round) {
        commit(root, random_ops(300));
        flush(root);
      }
      for (int i = 0; i < 8; ++i) {
        dynamic_cast<TSteppedSafeRepo *>(root.Get())->StepMergeDisk(256UL);
      }
      commit(root, random_ops(200));
      commit(child, random_ops(300));
      flush(child);
      commit(child, random_ops(200));
      /* One query: a pattern, or a range from `from` to `to`. */
      struct TQuery {
        TIndexKey From;
        std::optional<TIndexKey> To;
      };
      std::uniform_int_distribution<int64_t> point(-5L, Keys + 5L);
      std::uniform_int_distribution<int> shape(0, 9);
      const auto random_query = [&]() -> TQuery {
        const int s = shape(rng);
        if (s < 5) {
          int64_t a = point(rng), b = point(rng);
          if (a > b) {
            std::swap(a, b);
          }
          return {key(1L, a), key(1L, b)};
        }
        switch (s) {
          case 5: return {group(1L), std::nullopt};
          case 6: return {key(1L, point(rng)), std::nullopt};
          case 7: return {key(0L, point(rng) % 5000L), key(2L, point(rng) % 5000L)};
          case 8: return {group(3L), std::nullopt};
          default: return {key(1L, point(rng)), group(1L)};
        }
      };
      const auto count = [&](TContext &context, const TQuery &query) {
        return query.To ? context.CountKeys(query.From, *query.To) : context.CountKeys(query.From);
      };
      const auto count_cursor = [&](TContext &context, const TQuery &query) {
        unique_ptr<TContext::TKeyCursor> csr(query.To ? new TContext::TKeyCursor(&context, query.From, *query.To) : new TContext::TKeyCursor(&context, query.From));
        int64_t n = 0;
        for (; *csr; ++*csr) {
          ++n;
        }
        return n;
      };
      size_t ranked = 0UL;
      const auto check = [&](TContext &context, const TQuery &query) {
        context.SetRankCounts(false);
        const int64_t walked = count(context, query);
        context.SetRankCounts(true);
        const size_t before = context.GetRowsCountedByRank();
        const int64_t by_rank = count(context, query);
        ranked += context.GetRowsCountedByRank() - before;
        EXPECT_EQ(by_rank, walked);
        EXPECT_EQ(by_rank, count_cursor(context, query));
        return walked;
      };
      for (int i = 0; i < 300; ++i) {
        const TQuery query = random_query();
        TSuprena child_arena, root_arena;
        TContext child_context(child, &child_arena), root_context(root, &root_arena);
        check(child_context, query);
        check(root_context, query);
        check(pinned, query);
      }
      /* Every key of group 1 sits in some file most of whose stretches no other layer touches. */
      EXPECT_GT(ranked, 10000UL);
      /* Each count gives up at the same row either way. */
      for (int i = 0; i < 100; ++i) {
        const TQuery query = random_query();
        for (const auto *repo : {&child, &root}) {
          size_t expected;
          /* expected */ {
            TSuprena ctx_arena;
            TContext context(*repo, &ctx_arena);
            context.SetRankCounts(false);
            expected = static_cast<size_t>(count(context, query));
          }
          const size_t max_rows = std::uniform_int_distribution<size_t>(1UL, expected + 2UL)(rng);
          const auto attempt = [&](bool by_rank) {
            TSuprena ctx_arena;
            TContext context(*repo, &ctx_arena);
            context.SetRankCounts(by_rank);
            context.SetReadBudget(max_rows, 0, nullptr, 0);
            int64_t got = -1;
            try {
              got = count(context, query);
            } catch (const Orly::Server::TReadTooLarge &) {}
            return make_pair(got, context.GetRowsWalked());
          };
          const auto walked = attempt(false), by_rank = attempt(true);
          EXPECT_EQ(by_rank.first, walked.first);
          EXPECT_EQ(by_rank.second, walked.second);
          EXPECT_EQ(walked.first < 0, max_rows < expected);
        }
      }
      /* A context whose views were released refuses to count either way. */ {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        context.ReleaseViews();
        auto count_after_release = [&]() { (void) context.CountKeys(group(1L)); };
        EXPECT_THROW_FUNC(logic_error, count_after_release);
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #1018: a range walk over several disk files and the memtable yields each key once per entry, keys ascending and a key's entries
   newest first, with each key's newest entry the last one written to it; and a context's walk over it yields the keys alive in
   the range, with their values. The keys lead with elements every layer shares (a group, and a string, long in odd groups so it
   is stored apart from the key), which is what the merge's comparisons have to look past. */
FIXTURE(RangeWalkMergesLayersInKeyOrder) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TSuprena arena;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      const Base::TUuid index_id(TUuid::Twister);
      auto repo = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      auto *stepped = dynamic_cast<TSteppedSafeRepo *>(repo.Get());
      constexpr int64_t Groups = 3L, Keys = 300L;
      const auto label = [](int64_t g) { return g % 2L ? string(40, 'q') : string("vi"); };
      const auto key = [&](TSuprena &key_arena, int64_t g, int64_t n) {
        return TIndexKey(index_id, TKey(make_tuple(g, label(g), n), &key_arena, state));
      };
      const auto group = [&](TSuprena &key_arena, int64_t g) {
        return TIndexKey(index_id, TKey(make_tuple(g, label(g), Orly::Native::TFree<int64_t>()), &key_arena, state));
      };
      enum class TOp { Assign, Tombstone, Add };
      /* The ops on each key, oldest first. */
      std::map<pair<int64_t, int64_t>, vector<pair<TOp, int64_t>>> model;
      std::mt19937_64 rng(1018);
      const auto commit = [&](size_t count) {
        std::map<pair<int64_t, int64_t>, pair<TOp, int64_t>> ops;
        std::uniform_int_distribution<int64_t> pick_g(0L, Groups - 1L), pick_n(0L, Keys - 1L), pick_val(-1000L, 1000L);
        std::uniform_int_distribution<int> kind(0, 9);
        while (ops.size() < count) {
          const int k = kind(rng);
          ops[{pick_g(rng), pick_n(rng)}] = {k < 6 ? TOp::Assign : k < 8 ? TOp::Tombstone : TOp::Add, pick_val(rng)};
        }
        TSuprena update_arena;
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&update_arena), TKey(Base::TUuid(TUuid::Twister), &update_arena, state));
        for (const auto &[gn, op] : ops) {
          const TIndexKey index_key = key(update_arena, gn.first, gn.second);
          switch (op.first) {
            case TOp::Assign: update->AddEntry(index_key, TKey(op.second, &update_arena, state)); break;
            case TOp::Tombstone: update->AddEntry(index_key, TKey(Orly::Native::TTombstone::Tombstone, &update_arena, state)); break;
            case TOp::Add: update->AddEntry(index_key, TKey(op.second, &update_arena, state), TMutator::Add); break;
          }
          model[gn].push_back(op);
        }
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      for (int round = 0; round < 6; ++round) {
        commit(round == 0 ? 600UL : 150UL);
        stepped->StepMergeMem();
        if (round == 2) {
          stepped->StepMergeDisk(256UL);
        }
      }
      commit(150UL);
      /* The value a context reads for a key, if the key is alive. */
      const auto alive = [](const vector<pair<TOp, int64_t>> &ops) -> std::optional<int64_t> {
        std::optional<int64_t> val;
        for (const auto &[op, n] : ops) {
          switch (op) {
            case TOp::Assign: val = n; break;
            case TOp::Tombstone: val.reset(); break;
            case TOp::Add: val = val.value_or(0L) + n; break;
          }
        }
        return val;
      };
      std::uniform_int_distribution<int64_t> pick_g(0L, Groups - 1L), pick_n(-3L, Keys + 3L);
      std::uniform_int_distribution<int> shape(0, 3);
      size_t num_rows = 0UL, num_keys = 0UL;
      for (int i = 0; i < 200; ++i) {
        TSuprena query_arena;
        int64_t from_g = pick_g(rng), to_g = pick_g(rng), from_n = pick_n(rng), to_n = pick_n(rng);
        if (make_pair(from_g, from_n) > make_pair(to_g, to_n)) {
          std::swap(from_g, to_g);
          std::swap(from_n, to_n);
        }
        /* Up to a key, or through the whole of a group. */
        const bool to_group = shape(rng) == 0;
        const TIndexKey from = key(query_arena, from_g, from_n);
        const TIndexKey to = to_group ? group(query_arena, to_g) : key(query_arena, to_g, to_n);
        const auto in_range = [&](const pair<int64_t, int64_t> &gn) {
          return gn >= make_pair(from_g, from_n) && (to_group ? gn.first <= to_g : gn <= make_pair(to_g, to_n));
        };
        vector<pair<int64_t, int64_t>> expected_keys, expected_alive;
        for (const auto &[gn, ops] : model) {
          if (in_range(gn)) {
            expected_keys.push_back(gn);
            if (alive(ops)) {
              expected_alive.push_back(gn);
            }
          }
        }
        /* The repo's walk. */ {
          auto view = make_unique<Orly::Indy::TRepo::TView>(stepped);
          auto walker_ptr = stepped->NewPresentWalker(view, from, to);
          vector<pair<int64_t, int64_t>> walked;
          std::optional<TSequenceNumber> last_seq;
          for (TPresentWalker &walker = *walker_ptr; walker; ++walker) {
            const TPresentWalker::TItem &item = *walker;
            ++num_rows;
            const auto [g, s, n] = Sabot::AsNative<tuple<int64_t, string, int64_t>>(*Sabot::State::TAny::TWrapper(item.Key.NewState(item.KeyArena, state)));
            EXPECT_EQ(s, label(g));
            if (!walked.empty() && walked.back() == make_pair(g, n)) {
              /* An older entry of the same key. */
              EXPECT_TRUE(last_seq && item.SequenceNumber < *last_seq);
              last_seq = item.SequenceNumber;
              continue;
            }
            EXPECT_TRUE(walked.empty() || walked.back() < make_pair(g, n));
            walked.emplace_back(g, n);
            last_seq = item.SequenceNumber;
            /* A key's newest entry is the last op written to it. */
            const auto found = model.find({g, n});
            if (!EXPECT_TRUE(found != model.end())) {
              continue;
            }
            /* A disk merge may fold a `+=` onto what it adds to, so only an assignment or a delete is checked as written. */
            const auto &[op, val] = found->second.back();
            if (op == TOp::Assign) {
              EXPECT_TRUE(item.Mutator == TMutator::Assign);
              EXPECT_TRUE(TKey(item.Op, item.OpArena) == TKey(val, &query_arena, state));
            } else if (op == TOp::Tombstone) {
              EXPECT_TRUE(item.Op.IsTombstone());
            }
          }
          /* Every key the walk yields is in the range, in order, and so is every key whose last op was not a delete; a merge
             may have dropped a deleted one. */
          EXPECT_TRUE(std::includes(expected_keys.begin(), expected_keys.end(), walked.begin(), walked.end()));
          for (const auto &gn : expected_keys) {
            if (model[gn].back().first != TOp::Tombstone) {
              EXPECT_TRUE(std::binary_search(walked.begin(), walked.end(), gn));
            }
          }
        }
        /* A context's walk. */ {
          TSuprena ctx_arena;
          TContext context(repo, &ctx_arena);
          vector<pair<int64_t, int64_t>> walked;
          for (TContext::TKeyCursor csr(&context, from, to); csr; ++csr) {
            const auto [g, s, n] = Sabot::AsNative<tuple<int64_t, string, int64_t>>(*Sabot::State::TAny::TWrapper(csr->GetCore().NewState(csr->GetArena(), state)));
            walked.emplace_back(g, n);
            num_keys += 1UL;
            const auto val = alive(model[{g, n}]);
            if (EXPECT_TRUE(val.has_value())) {
              EXPECT_TRUE(TKey(csr.GetVal().Op, csr.GetVal().OpArena) == TKey(*val, &query_arena, state));
            }
          }
          EXPECT_TRUE(walked == expected_alive);
        }
      }
      /* The ranges reached into every layer. */
      EXPECT_GT(num_rows, 20000UL);
      EXPECT_GT(num_keys, 10000UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

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

  /* With 'retention_sim', retention is persisted there (#745 stage 3), and Restart reloads the repo. */
  explicit TRootRepoFixture(bool prune_merge_history, TRetentionStoreSim *retention_sim = nullptr)
      : StateBuf(Sabot::State::GetMaxStateSize()),
        State(StateBuf.data()),
        Engine(&Scheduler, 256, 256, 16384, 1, 1024, 1),
        PruneMergeHistory(prune_merge_history),
        RetentionSim(retention_sim),
        IdxId(TUuid::Twister) {
    Scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    NewManager();
    Repo = Manager->GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
    Stepped = dynamic_cast<TSteppedSafeRepo *>(Repo.Get());
    assert(Stepped);
    Safe = Stepped;
  }

  /* Closes the repo and its manager, then reloads the repo from the files, as a restart would. Flush first: what
     is still in memory is gone. Afterwards only what works on a TSafeRepo (no Flush) is available. */
  void Restart() {
    assert(RetentionSim);
    const Base::TUuid repo_id = Repo->GetId();
    Repo.Reset();
    Stepped = nullptr;
    Safe = nullptr;
    Manager.reset();
    NewManager();
    Repo = Manager->Reopen(repo_id);
    Safe = dynamic_cast<TSafeRepo *>(Repo.Get());
    assert(Safe);
  }

  TSafeRepo *GetSafe() const {
    return Safe;
  }

  Orly::Indy::Disk::Util::TEngine *GetEngine() {
    return Engine.GetEngine();
  }

  bool IsMergeDiskQueued() {
    return Manager->IsMergeDiskQueued(Repo.Get());
  }

  ~TRootRepoFixture() {
    Repo.Reset();
    Manager.reset();
    /* The test's manager tours no runners, so nothing cleared the walkers this runner pooled for the repo's files,
       and they pin pages of Engine. Free them while Engine is alive: a later trim would release the pins into a
       cache that is gone (#967). Two trims free every pooled walker, since none is taken in between. */
    if (auto *cache = Disk::TLocalWalkerCache::Cache.Get()) {
      cache->Trim();
      cache->Trim();
    }
  }

  TIndexKey IndexKey(int64_t key) {
    return TIndexKey(IdxId, TKey(make_tuple(key), &Arena, State));
  }

  /* Commits the entries as one update and returns its sequence number. */
  TSequenceNumber Commit(const vector<tuple<int64_t, int64_t, TMutator>> &entries) {
    return CommitWith([&](TUpdate *update) {
      for (const auto &[key, val, mut] : entries) {
        update->AddEntry(IndexKey(key), TKey(val, &Arena, State), mut);
      }
    });
  }

  /* Commits a delete of `key` and returns its sequence number. */
  TSequenceNumber CommitDelete(int64_t key) {
    return CommitWith([&](TUpdate *update) {
      update->AddEntry(IndexKey(key), TKey(Orly::Native::TTombstone::Tombstone, &Arena, State));
    });
  }

  /* Reads `key` as of version `at` (#745): its value, or unset if it isn't there. */
  std::optional<TKey> ReadAt(int64_t key, TSequenceNumber at) {
    TSuprena ctx_arena;
    TContext context(Repo, &ctx_arena, at);
    if (!context.Exists(IndexKey(key))) {
      return std::nullopt;
    }
    return TKey(&Arena, State, context[IndexKey(key)]);
  }

  /* The keys in [lo, hi] as of version `at`, by a range walk (#745). */
  vector<TKey> KeysAt(int64_t lo, int64_t hi, TSequenceNumber at) {
    TSuprena ctx_arena;
    TContext context(Repo, &ctx_arena, at);
    vector<TKey> keys;
    for (TContext::TKeyCursor csr(&context, IndexKey(lo), IndexKey(hi)); csr; ++csr) {
      keys.emplace_back(&Arena, State, *csr);
    }
    return keys;
  }

  /* The keys in [lo, hi] whose value differs between versions `first` and `second` (#889), by the changed-key
     cursor; `pattern` walks the one key `lo` as a pattern instead. */
  vector<TKey> ChangedKeys(int64_t lo, int64_t hi, TSequenceNumber first, TSequenceNumber second, bool pattern = false) {
    TSuprena ctx_arena;
    TContext context(Repo, &ctx_arena);
    vector<TKey> keys;
    const TIndexKey from = IndexKey(lo), to = IndexKey(hi);
    TContext::TChangedKeyCursor csr(&context, from, pattern ? nullptr : &to, first, second);
    for (; csr; ++csr) {
      keys.emplace_back(&Arena, State, *csr);
    }
    return keys;
  }

  /* The key as a range walk yields it. */
  TKey KeyOf(int64_t key) {
    return TKey(make_tuple(key), &Arena, State);
  }

  TSequenceNumber GetHistoryHorizon() const {
    return Safe->GetHistoryHorizon();
  }

  /* The disk and memory layers of the repo's current view. */
  pair<size_t, size_t> CountLayersByKind() {
    Orly::Indy::TRepo::TView view(Safe);
    size_t disk = 0UL, mem = 0UL;
    view.CountLayers(disk, mem);
    return {disk, mem};
  }

  void Flush() {
    Stepped->StepMergeMem();
  }

  /* Keep merge outputs unfolded, so reads go through a TMergeDataFile's history (#666). */
  /* #745 stage 2: the retention window, and how long a read at a version leases it. */
  void SetRetention(size_t versions, size_t seconds, std::chrono::steady_clock::duration lease_ttl) {
    Manager->SetRetention(versions, seconds);
    Safe->SetVersionLeaseTtl(lease_ttl);
  }

  /* Tails the oldest disk file. */
  void Tail() {
    static_cast<Orly::Indy::TRepo *>(Stepped)->StepTail(256UL);
  }

  void KeepMergesUnfolded() {
    Stepped->FoldMergedFiles = false;
  }

  void MergeDisk(size_t passes) {
    for (size_t i = 0UL; i < passes; ++i) {
      Safe->StepMergeDisk(256UL);
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

  Disk::Util::TVolumeManager *GetVolMan() {
    return Engine.GetEngine()->GetVolMan();
  }

  private:

  template <typename TFill>
  TSequenceNumber CommitWith(const TFill &fill) {
    std::optional<TSequenceNumber> seq;
    /* The transaction commits, and reports the sequence number, as it goes out of scope. */ {
      auto transaction = Manager->NewTransaction();
      transaction->ReportCommitSequenceNumber(&seq);
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&Arena), TKey(Base::TUuid(TUuid::Twister), &Arena, State));
      fill(update.get());
      transaction->Push(Repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    ++NumCommits;
    assert(seq);
    return *seq;
  }

  vector<uint8_t> StateBuf;

  void *State;

  TScheduler Scheduler;

  Orly::Indy::Disk::Sim::TMemEngine Engine;

  unique_ptr<TMyManager> Manager;

  L0::TManager::TPtr<L0::TManager::TRepo> Repo;

  TSteppedSafeRepo *Stepped = nullptr;

  /* The repo, before and after a Restart. */
  TSafeRepo *Safe = nullptr;

  const bool PruneMergeHistory;

  TRetentionStoreSim *RetentionSim;

  void NewManager() {
    Manager = make_unique<TMyManager>(Engine.GetEngine(), &Scheduler, PruneMergeHistory);
    if (RetentionSim) {
      Manager->RetentionSim = RetentionSim;
      Manager->SetRetentionPersisted(true);
    }
  }

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

/* #988: a merged file past the fold cap stays unfolded, and its `+=` chain still reads in full. */
FIXTURE(DiskMergePastTheFoldCapStaysUnfolded) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    const int64_t counter = -1L;
    const size_t old_cap = TRepo::GetMaxFoldInputBytes();
    for (bool capped : {false, true}) {
      TRepo::SetMaxFoldInputBytes(capped ? 1UL : std::numeric_limits<size_t>::max());
      const size_t too_large = TRepo::GetNumFoldsTooLarge();
      {
        TRootRepoFixture root(true);
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
        for (int64_t key = 0L; key < 100L; ++key) {
          EXPECT_EQ(root.Read(key), root.Int(key));
        }
      }
      EXPECT_EQ(TRepo::GetNumFoldsTooLarge() > too_large, capped);
    }
    TRepo::SetMaxFoldInputBytes(old_cap);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* A merge whose runs are no longer than the minimum fold run keeps its output unfolded, and its `+=` chain still reads
   in full. Each round here adds a run of ten to the counter, and each merge takes in the last one's output, so with a
   minimum of 16 the merges alternate: a run of ten or so stays unfolded, the next one past 16 folds. At 0, the default,
   every merge folds. */
FIXTURE(DiskMergeWithShortRunsStaysUnfolded) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    const int64_t counter = -1L;
    const size_t old_min = TRepo::GetMinFoldRun();
    for (size_t min_run : {0UL, 16UL}) {
      TRepo::SetMinFoldRun(min_run);
      const size_t short_runs = TRepo::GetNumFoldsShortRuns();
      {
        TRootRepoFixture root(false);
        root.Commit({{counter, 0L, TMutator::Assign}});
        for (int64_t key = 0L; key < 100L; ++key) {
          root.Commit({{key, key, TMutator::Assign}});
        }
        root.Flush();
        int64_t expected = 0L;
        for (int64_t round = 0L; round < 12L; ++round) {
          for (int64_t i = 0L; i < 10L; ++i) {
            root.Commit({{counter, 1L, TMutator::Add}});
            ++expected;
          }
          root.Flush();
          root.MergeDisk(4UL);
          EXPECT_EQ(root.Read(counter), root.Int(expected));
        }
        for (int64_t key = 0L; key < 100L; ++key) {
          EXPECT_EQ(root.Read(key), root.Int(key));
        }
      }
      EXPECT_EQ(TRepo::GetNumFoldsShortRuns() > short_runs, min_run > 0UL);
    }
    TRepo::SetMinFoldRun(old_min);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #988: the fold cap must not let a `+=` run grow without bound. Every merge here is past the cap, and each one takes
   in the last one's output, so without the run bound the counter's run would only grow, to 300 entries. Once it passes
   MaxUnfoldedRun the merge folds anyway, and the run starts again from one. */
FIXTURE(DiskMergePastTheFoldCapBoundsARun) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    const int64_t counter = -1L;
    const size_t old_cap = TRepo::GetMaxFoldInputBytes();
    TRepo::SetMaxFoldInputBytes(1UL);
    const size_t too_large = TRepo::GetNumFoldsTooLarge(), long_runs = TRepo::GetNumFoldsForLongRuns();
    {
      TRootRepoFixture root(false);
      root.Commit({{counter, 0L, TMutator::Assign}});
      for (int64_t key = 0L; key < 100L; ++key) {
        root.Commit({{key, key, TMutator::Assign}});
      }
      root.Flush();
      int64_t expected = 0L;
      for (int64_t round = 0L; round < 30L; ++round) {
        for (int64_t i = 0L; i < 10L; ++i) {
          root.Commit({{counter, 1L, TMutator::Add}});
          ++expected;
        }
        root.Flush();
        root.MergeDisk(4UL);
        EXPECT_EQ(root.Read(counter), root.Int(expected));
      }
      for (int64_t key = 0L; key < 100L; ++key) {
        EXPECT_EQ(root.Read(key), root.Int(key));
      }
    }
    EXPECT_GT(TRepo::GetNumFoldsTooLarge(), too_large);
    EXPECT_GT(TRepo::GetNumFoldsForLongRuns(), long_runs);
    EXPECT_LE(TRepo::GetLongestRunKeptUnfolded(), TRepo::MaxUnfoldedRun);
    cerr << "fold cap: " << (TRepo::GetNumFoldsTooLarge() - too_large) << " kept unfolded, "
         << (TRepo::GetNumFoldsForLongRuns() - long_runs) << " folded for a long run, longest kept "
         << TRepo::GetLongestRunKeptUnfolded() << endl;
    TRepo::SetMaxFoldInputBytes(old_cap);
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
   otherwise drop the superseded ones.

   A merge drops an input's history, not its current versions: those it keeps whole, as history of
   the output. So a superseded version goes at the second merge it is in. Since #968 the eight files
   below go in one merge, which keeps all eight rounds; a ninth file and one more merge drop all but
   the newest two. */
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
      EXPECT_EQ(root.CountLayers(), 1UL);
      EXPECT_EQ(root.CountWalk(), root.NumCommits);
      for (int64_t key = 0L; key < 20L; ++key) {
        root.Commit({{key, 900L + key, TMutator::Assign}});
      }
      root.Flush();
      root.MergeDisk(16UL);
      EXPECT_EQ(root.CountLayers(), 1UL);
      for (int64_t key = 0L; key < 20L; ++key) {
        EXPECT_EQ(root.Read(key), root.Int(900L + key));
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

/* #692: a disk merge whose pair can't fit in the free space must not keep the repo from merging
   a pair that can. The disk merge takes the leftmost eligible pair, oldest first. Here that is two
   big files, and the space left above the data floor is half of what they hold; two small files
   behind them would fit easily. The merge used to retry the big pair forever, writing until it hit
   the floor each time, and the failed claim kept write admission at the big pair's size. */
FIXTURE(DiskMergeSkipsAPairThatCannotFit) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(true);
      auto *vol_man = root.GetVolMan();
      /* Writes keys [first, first + count) as one file; returns the space it took. */
      auto write_file = [&](int64_t first, int64_t count) {
        vector<tuple<int64_t, int64_t, TMutator>> entries;
        for (int64_t key = first; key < first + count; ++key) {
          entries.emplace_back(key, key, TMutator::Assign);
        }
        root.Commit(entries);
        const size_t before = vol_man->GetSpace().GetAvailable();
        root.Flush();
        return before - vol_man->GetSpace().GetAvailable();
      };
      /* Two big files and two small ones. (The test's entry pool holds 20000.) */
      size_t big = write_file(0L, 4500L);
      big += write_file(4500L, 4500L);
      size_t small = write_file(20000L, 1L);
      small += write_file(20001L, 1L);
      EXPECT_EQ(root.CountLayers(), 4UL);
      EXPECT_GT(big / 2UL, small);
      /* Room above the floor for half the big pair. */
      const size_t room = big / 2UL;
      vol_man->SetDataFloor(vol_man->GetSpace().GetAvailable() - room);
      root.MergeDisk(3UL);
      /* the small pair merged; the big pair is still two files */
      EXPECT_EQ(root.CountLayers(), 3UL);
      /* and admission isn't holding writes off for a merge that was skipped */
      EXPECT_LT(vol_man->GetRecentPeakClaims(), room);
      for (int64_t key : {0L, 4499L, 4500L, 8999L, 20000L, 20001L}) {
        EXPECT_EQ(root.Read(key), root.Int(key));
      }
      /* With room, the big pair merges too. */
      vol_man->SetDataFloor(0UL);
      root.MergeDisk(3UL);
      EXPECT_EQ(root.CountLayers(), 2UL);
      for (int64_t key : {0L, 4499L, 4500L, 8999L, 20000L, 20001L}) {
        EXPECT_EQ(root.Read(key), root.Int(key));
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #968: a disk merge takes a run of files, not just a pair. Ten one-key files sit in one gen set. The
   first pass merges the oldest MaxDiskMergeInputs of them into one file, which is still in that set,
   so the second merges it with the last two. Pairwise, that took nine passes. A big file after the
   small ones is in a higher set, so the run takes it too; a small file after the big one stops the
   run, since its set is lower. */
FIXTURE(DiskMergeTakesARunOfFiles) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(true);
      static_assert(TSafeRepo::MaxDiskMergeInputs == 8UL);
      for (int64_t key = 0L; key < 10L; ++key) {
        root.Commit({{key, key, TMutator::Assign}});
        root.Flush();
      }
      EXPECT_EQ(root.CountLayers(), 10UL);
      root.MergeDisk(1UL);
      EXPECT_EQ(root.CountLayers(), 3UL);
      root.MergeDisk(1UL);
      EXPECT_EQ(root.CountLayers(), 1UL);
      for (int64_t key = 0L; key < 10L; ++key) {
        EXPECT_EQ(root.Read(key), root.Int(key));
      }
      /* Two one-key files, a thousand-key file, then a one-key file. */
      root.Commit({{100L, 100L, TMutator::Assign}});
      root.Flush();
      root.Commit({{101L, 101L, TMutator::Assign}});
      root.Flush();
      vector<tuple<int64_t, int64_t, TMutator>> entries;
      for (int64_t key = 1000L; key < 2000L; ++key) {
        entries.emplace_back(key, key, TMutator::Assign);
      }
      root.Commit(entries);
      root.Flush();
      root.Commit({{102L, 102L, TMutator::Assign}});
      root.Flush();
      EXPECT_EQ(root.CountLayers(), 5UL);
      /* The oldest file (ten keys) and the two small ones after it are in one set, and the big one is
         in a higher set, so the run is the first four; the last small file is in a lower set. */
      root.MergeDisk(1UL);
      EXPECT_EQ(root.CountLayers(), 2UL);
      for (int64_t key : {0L, 9L, 100L, 101L, 102L, 1000L, 1999L}) {
        EXPECT_EQ(root.Read(key), root.Int(key));
      }
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

/* A cached repo goes at its deadline, not only when the cache is full.  An expired pov's repo
   used to sit in the cache, holding a Data Layer and a Repo Mapping, until the cache filled; the
   cache is twice the size of those pools, so they ran out first and every new pov was refused.
   Its WAL Pov record stayed pinned too, since the repo was still live. */
FIXTURE(CachedRepoGoesAtItsDeadline) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Orly::Indy::Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler, true, 16UL);
      const Base::TUuid root_id(TUuid::Twister), short_id(TUuid::Twister), long_id(TUuid::Twister), ttl0_parent_id(TUuid::Twister);
      auto root = manager.GetRepo(root_id, TTtl::max(), std::nullopt, true);
      const auto start = L0::TDeadline::clock::now();
      manager.GetRepo(short_id, TTtl(60s), root, true);  // dropped at once: closed, so cached
      manager.GetRepo(long_id, TTtl(3600s), root, true);
      /* A ttl-0 parent held open only by its cached child. */ {
        auto ttl0_parent = manager.GetRepo(ttl0_parent_id, TTtl(0s), std::nullopt, true);
        manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl(60s), ttl0_parent, true);
      }
      EXPECT_TRUE(manager.IsLiveRepo(short_id));
      EXPECT_TRUE(manager.IsLiveRepo(ttl0_parent_id));
      /* Nothing has expired yet. */
      manager.DiscardExpiredObjs(start);
      EXPECT_TRUE(manager.IsLiveRepo(short_id));
      EXPECT_TRUE(manager.IsLiveRepo(long_id));
      EXPECT_TRUE(manager.IsLiveRepo(ttl0_parent_id));
      /* Past the short deadlines but not the long one. */
      manager.DiscardExpiredObjs(start + 120s);
      EXPECT_FALSE(manager.IsLiveRepo(short_id));
      EXPECT_TRUE(manager.IsLiveRepo(long_id));
      /* The discarded child let go of its parent, which then went too. */
      EXPECT_FALSE(manager.IsLiveRepo(ttl0_parent_id));
      /* The cached repos still hold the root. */
      root.Reset();
      EXPECT_TRUE(manager.IsLiveRepo(root_id));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #914: an expired pov's repo is flagged DiscardWhenClosed(), so it skips the cache when its
   last ptr goes, whichever holder lets go last, and lets go of its parent. */
FIXTURE(DiscardWhenClosedSkipsTheCache) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Orly::Indy::Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler, true, 16UL);
      const Base::TUuid parent_id(TUuid::Twister), flagged_id(TUuid::Twister), plain_id(TUuid::Twister);
      /* A ttl-0 parent, held open only by its children. */ {
        auto parent = manager.GetRepo(parent_id, TTtl(0s), std::nullopt, true);
        auto flagged = manager.GetRepo(flagged_id, TTtl(600s), parent, true);
        auto plain = manager.GetRepo(plain_id, TTtl(600s), parent, true);
        /* A second holder, as a Tetris player holds a safe child's repo. */
        auto other = flagged;
        flagged->DiscardWhenClosed();
        flagged.Reset();
        EXPECT_TRUE(manager.IsLiveRepo(flagged_id));
        other.Reset();
        EXPECT_FALSE(manager.IsLiveRepo(flagged_id));
        /* An unflagged repo is still cached. */
        plain.Reset();
        EXPECT_TRUE(manager.IsLiveRepo(plain_id));
      }
      /* The cached child still holds the parent. */
      EXPECT_TRUE(manager.IsLiveRepo(parent_id));
      manager.DiscardExpiredObjs(L0::TDeadline::clock::now() + 1200s);
      EXPECT_FALSE(manager.IsLiveRepo(plain_id));
      EXPECT_FALSE(manager.IsLiveRepo(parent_id));
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

  /* Gives back 'n' of the blocks it took, so the pool has exactly that much room. */
  void Give(size_t n) {
    for (; n && !Blocks.empty(); --n) {
      Free(Blocks.back());
      Blocks.pop_back();
    }
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

/* The fold step used to build a memory layer of updates from the Update pool. Since #745 §9.6 it
   streams the merged file and takes nothing from the pool, so a full pool no longer stops it. */
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

/* #858: a fold too big for the room left in the Update pool used to take every free block before
   it failed, and whatever else wanted one meanwhile (a new POV's System repo write, after a
   restart) got a bare bad_alloc. The fold then claimed its blocks up front; since #745 §9.6 it
   takes none at all. Here the pool has room for a few updates: the merge must not miss on it. */
FIXTURE(DiskMergeFoldClaimsBeforeAllocating) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TDiskMergePoolFixture fixture;
      EXPECT_EQ(fixture.CountLayers(), 2UL);
      {
        TPoolHog hog([] { return TUpdate::operator new(sizeof(TUpdate)); },
                     [](void *ptr) { TUpdate::operator delete(ptr, sizeof(TUpdate)); });
        EXPECT_GT(hog.GetSize(), 4UL);
        hog.Give(4UL);
        const size_t misses = TUpdate::GetUpdatePool().GetNumMisses();
        fixture.Root.MergeDisk(4UL);
        EXPECT_EQ(TUpdate::GetUpdatePool().GetNumMisses(), misses);
        EXPECT_EQ(TUpdate::GetUpdatePool().GetNumBlocksClaimed(), 0UL);
      }
      EXPECT_TRUE(fixture.ReadsCorrectly());
      EXPECT_EQ(fixture.CountLayers(), 1UL);
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

/* Holds every page buffer of a cache, through pages no file owns, until released, as the readers of a
   busy server do. With none free, a read that misses throws TOutOfCachePages (#932). */
template <typename TCacheType>
class TCachePageHog {
  NO_COPY(TCachePageHog);
  public:

  explicit TCachePageHog(TCacheType *cache)
      : Cache(cache) {
    constexpr size_t bogus_page_id_base = 200000000UL;
    try {
      for (size_t i = 0UL; i < 1000000UL; ++i) {
        typename TCacheType::TSlot *data_slot;
        const size_t page_id = bogus_page_id_base + i;
        Held.emplace_back(Cache->Get(page_id, data_slot), page_id);
      }
    } catch (const Orly::Indy::Disk::Util::TOutOfCachePages &) {
      Full = true;
    }
  }

  ~TCachePageHog() {
    Release();
  }

  bool IsFull() const {
    return Full;
  }

  void Give(size_t n) {
    while (n > 0UL && !Held.empty()) {
      Cache->Release(Held.back().first, Held.back().second);
      Held.pop_back();
      --n;
    }
  }

  void Release() {
    for (const auto &held : Held) {
      Cache->Release(held.first, held.second);
    }
    Held.clear();
  }

  private:

  TCacheType *Cache;

  bool Full = false;

  vector<pair<typename TCacheType::TSlot *, size_t>> Held;

};  // TCachePageHog

/* #932: a disk merge that finds every cache page referenced must not abort the server. A read in the merge throws
   TOutOfCachePages. The merge must free what it wrote and let go of the pages and the layers it took, so the input
   layers stay readable and mergeable, and it must go through once pages are free again. */
FIXTURE(DiskMergeWithCachePagesPinned) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TDiskMergePoolFixture fixture;
      EXPECT_EQ(fixture.CountLayers(), 2UL);
      auto *const engine = fixture.Root.GetEngine();
      auto *const vol_man = engine->GetVolMan();
      const size_t available = vol_man->GetSpace().GetAvailable();
      {
        TCachePageHog page_hog(engine->GetPageCache());
        TCachePageHog block_hog(engine->GetBlockCache());
        EXPECT_TRUE(page_hog.IsFull());
        EXPECT_TRUE(block_hog.IsFull());
        /* AsyncMultiGet that throws midway must release the pages it already grabbed, leaving them
           in the LRU rather than leaked with RefCount > 0. */
        page_hog.Give(1UL);
        {
          Disk::TCompletionTrigger trigger;
          bool threw = false;
          try {
            engine->GetPageCache()->AsyncMultiGet(HERE, Disk::Low, engine->GetPageCache(),
                Disk::Util::PageCheckedBlock, 0U, 300000000UL, 2UL, true, trigger);
          } catch (const Disk::Util::TOutOfCachePages &) {
            threw = true;
          }
          EXPECT_TRUE(threw);
          Disk::Util::TPageCache::TSlot *data_slot = nullptr;
          auto *const main_slot = engine->GetPageCache()->Get(300000002UL, data_slot);
          EXPECT_TRUE(main_slot != nullptr);
          engine->GetPageCache()->Release(main_slot, 300000002UL);
          /* A stream with only 1 free page in the cache must still step across sequential pages
             without PrefetchNextPage / AsyncMultiGet eating the last page or ~TStream releasing a
             mismatched (MainSlot, LoadedPageId) pair. */
          struct TMultiPageFile final : Disk::TInFile {
            virtual size_t GetFileLength() const override { return 8UL * Disk::Util::LogicalPageSize; }
            virtual size_t GetStartingBlock() const override { return 0UL; }
            virtual void ReadMeta(size_t, size_t &) const override { throw; }
            virtual size_t FindPageIdOfByte(size_t offset) const override {
              return 1024UL + (offset / Disk::Util::LogicalPageSize);
            }
          } multi_page_file;
          {
            Disk::TStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize,
                Disk::Util::PhysicalBlockSize, Disk::Util::FullPage, 0UL>
                stream(HERE, 0U, Disk::RealTime, &multi_page_file, engine->GetPageCache(), 0UL);
            stream.GoTo(Disk::Util::LogicalPageSize);
            stream.GoTo(2UL * Disk::Util::LogicalPageSize);
          }
          /* And a locally cached stream that hits TOutOfCachePages on page 1 must not claim page 1
             is loaded or wedge the cache. */
          {
            Disk::TStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize,
                Disk::Util::PhysicalBlockSize, Disk::Util::FullPage, 4UL>
                stream(HERE, 0U, Disk::RealTime, &multi_page_file, engine->GetPageCache(), 0UL);
            bool page1_threw = false;
            try {
              stream.GoTo(Disk::Util::LogicalPageSize);
            } catch (const Disk::Util::TOutOfCachePages &) {
              page1_threw = true;
            }
            EXPECT_TRUE(page1_threw);
            stream.GoTo(0UL);
          }
          /* Re-pin that single page while the hog holds the rest. */
          auto *const pinned_slot = engine->GetPageCache()->Get(300000002UL, data_slot);
          bool full_again = false;
          try {
            engine->GetPageCache()->Get(300000003UL, data_slot);
          } catch (const Disk::Util::TOutOfCachePages &) {
            full_again = true;
          }
          EXPECT_TRUE(full_again);
          /* Each pass takes the pair, fails, and hands it back. */
          fixture.Root.MergeDisk(4UL);
          EXPECT_EQ(fixture.CountLayers(), 2UL);
          /* The failed merges kept none of the blocks they wrote. */
          EXPECT_EQ(vol_man->GetSpace().GetAvailable(), available);
          engine->GetPageCache()->Release(pinned_slot, 300000002UL);
        }
      }
      EXPECT_TRUE(fixture.ReadsCorrectly());
      fixture.Root.MergeDisk(4UL);
      EXPECT_EQ(fixture.CountLayers(), 1UL);
      EXPECT_TRUE(fixture.ReadsCorrectly());
      /* and a later merge, with pages free, folds as usual */
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

/* #691: a memory merge that has written its file but finds no pool room to publish it must not
   hold the merge runner while it waits. Every repo's memory merge runs on that one runner, and
   another repo's merge can be exactly what frees the room: merging a repo's memory layers
   replaces its mapping with a smaller one.

   Repo A (the root) has three disk files and a sealed memory layer to flush. Repo B (a fast
   repo) has ten memory layers waiting to merge. The mapping entry pool is left with one free
   entry: too few for A's new mapping (four entries), enough for B's (one), and B's merge frees
   ten. On the runner, A's merge comes first, then B's, then A's again. A's first merge used to
   sleep until the pool had room, which nothing on the runner could give it; a watchdog gives the
   pool back after 5 s so that the test ends. */
FIXTURE(MemMergePublishWaitsOffTheRunner) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    using TMapping = TSteppedSafeRepo::TMappingType;
    TRunnerFileCaches file_caches;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Orly::Indy::Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    /* B's merges must not park (see TLatchedFastRepo). */ {
      std::lock_guard<std::mutex> lock(MergeLatch.Mutex);
      MergeLatch.Released = true;
    }
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      TSuprena arena;
      const Base::TUuid idx_id(TUuid::Twister);
      auto a = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      auto b = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false);
      auto *stepped_a = dynamic_cast<TSteppedSafeRepo *>(a.Get());
      auto *latched_b = dynamic_cast<TLatchedFastRepo *>(b.Get());
      if (!EXPECT_TRUE(stepped_a && latched_b)) {
        std::lock_guard<std::mutex> lock(mut);
        fin = true;
        cond.notify_one();
        return;
      }
      auto commit = [&](auto &repo, int64_t key) {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(TIndexKey(idx_id, TKey(make_tuple(key), &arena, state)), TKey(key * 10L, &arena, state), TMutator::Assign);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      auto read = [&](auto &repo, int64_t key) {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        return TKey(&arena, state, context[TIndexKey(idx_id, TKey(make_tuple(key), &arena, state))]);
      };
      auto count_layers = [](Orly::Indy::TRepo *repo) {
        Orly::Indy::TRepo::TView view(repo);
        return view.GetNumEntries();
      };
      /* A: three disk files. */
      for (int64_t key = 0L; key < 3L; ++key) {
        commit(a, key);
        stepped_a->StepMergeMem();
      }
      EXPECT_EQ(count_layers(stepped_a), 3UL);
      /* B: ten memory layers, each sealed by a merge that then gives up. */
      bool fail_b = true;
      std::mutex hog_mutex;
      std::optional<TPoolHog> hog;
      TSteppedSafeRepo::OnMergeMemSealedForTest = [&](Orly::Indy::TRepo *repo) {
        if (repo == latched_b && fail_b) {
          throw std::bad_alloc();
        }
        std::lock_guard<std::mutex> lock(hog_mutex);
        if (repo == stepped_a && !hog) {
          hog.emplace([] { return TMapping::TEntry::operator new(sizeof(TMapping::TEntry)); },
                      [](void *ptr) { TMapping::TEntry::operator delete(ptr, sizeof(TMapping::TEntry)); });
          hog->Give(1UL);
        }
      };
      for (int64_t key = 100L; key < 110L; ++key) {
        commit(b, key);
        latched_b->StepMem();
      }
      fail_b = false;
      EXPECT_EQ(count_layers(latched_b), 10UL);
      /* A's next merge seals one more layer, writes it, and then can't publish. */
      commit(a, 3L);
      std::mutex watch_mutex;
      std::condition_variable watch_cond;
      bool runner_done = false, watchdog_fired = false;
      std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(watch_mutex);
        if (!watch_cond.wait_for(lock, 5s, [&] { return runner_done; })) {
          watchdog_fired = true;
          std::lock_guard<std::mutex> hog_lock(hog_mutex);
          if (hog) {
            hog->Release();
          }
        }
      });
      /* the runner: A, then B, then A again */
      stepped_a->StepMergeMem();
      latched_b->StepMem();
      stepped_a->StepMergeMem();
      /* runner done */ {
        std::lock_guard<std::mutex> lock(watch_mutex);
        runner_done = true;
        watch_cond.notify_all();
      }
      watchdog.join();
      EXPECT_FALSE(watchdog_fired);
      TSteppedSafeRepo::OnMergeMemSealedForTest = nullptr;
      /* hog released */ {
        std::lock_guard<std::mutex> lock(hog_mutex);
        hog.reset();
      }
      EXPECT_EQ(count_layers(latched_b), 1UL);
      EXPECT_EQ(count_layers(stepped_a), 4UL);
      for (int64_t key = 0L; key < 4L; ++key) {
        EXPECT_EQ(read(a, key), TKey(key * 10L, &arena, state));
      }
      for (int64_t key = 100L; key < 110L; ++key) {
        EXPECT_EQ(read(b, key), TKey(key * 10L, &arena, state));
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #713: a view drops the last reference to a layer while a merge marks it taken. Decr() read the
   plain bool MarkedTaken on every release, and the merge wrote it under a lock the view doesn't
   take, so ThreadSanitizer reported a race on every orlyi run. The answer only matters to the
   release that takes the count to zero, and that one can't be concurrent with the mark: the
   merge's mapping holds a reference until it is released under MappingLock, after the mark. So
   the layer must still be queued for removal, and every update it held freed by teardown.

   A fast root repo keeps its merged layer in memory, so each round merges two layers: the one the
   previous rounds merged into, and the round's own, which a view pinned before the merge sealed
   it. The view goes away while the merge runs, after a delay that varies from round to round so
   that some rounds land between the mark and the publish. */
FIXTURE(ViewDropsLastRefToTakenLayer) {
  const size_t updates_before = TUpdate::GetUpdatePool().GetNumBlocksUsed();
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Orly::Indy::Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    /* The fast repo's merges must not park (see TLatchedFastRepo). */ {
      std::lock_guard<std::mutex> lock(MergeLatch.Mutex);
      MergeLatch.Released = true;
    }
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      TSuprena arena;
      const Base::TUuid idx_id(TUuid::Twister);
      auto repo = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false);
      auto *latched = dynamic_cast<TLatchedFastRepo *>(repo.Get());
      if (!EXPECT_TRUE(latched != nullptr)) {
        std::lock_guard<std::mutex> lock(mut);
        fin = true;
        cond.notify_one();
        return;
      }
      auto commit = [&](int64_t key) {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(TIndexKey(idx_id, TKey(make_tuple(key), &arena, state)), TKey(key * 10L, &arena, state), TMutator::Assign);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      commit(0L);
      latched->StepMem();
      std::mutex sealed_mutex;
      std::condition_variable sealed_cond;
      bool sealed = false;
      Orly::Indy::TRepo::OnMergeMemSealedForTest = [&](Orly::Indy::TRepo *sealed_repo) {
        if (sealed_repo == latched) {
          std::lock_guard<std::mutex> lock(sealed_mutex);
          sealed = true;
          sealed_cond.notify_all();
        }
      };
      /* Nothing runs the layer cleaner here, so every merged layer waits in the removal queue
         until teardown: two per round, out of a Data Layer pool of 100. */
      const int64_t num_rounds = 20L;
      for (int64_t round = 1L; round <= num_rounds; ++round) {
        commit(round);
        auto view = make_unique<Orly::Indy::TRepo::TView>(latched);
        sealed = false;
        std::thread merger([latched] { latched->StepMem(); });
        /* wait for the merge to seal the layer the view pins */ {
          std::unique_lock<std::mutex> lock(sealed_mutex);
          sealed_cond.wait(lock, [&] { return sealed; });
        }
        for (int64_t spin = 0L; spin < (round % 5L) * 4000L; ++spin) {
          asm volatile("" ::: "memory");
        }
        view.reset();
        merger.join();
      }
      Orly::Indy::TRepo::OnMergeMemSealedForTest = nullptr;
      /* every round merged into one layer */ {
        Orly::Indy::TRepo::TView view(latched);
        EXPECT_EQ(view.GetNumEntries(), 1UL);
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
  /* A taken layer whose last release didn't queue it would keep its updates past teardown. */
  EXPECT_EQ(TUpdate::GetUpdatePool().GetNumBlocksUsed(), updates_before);
}

/* #722: a writer waiting out back-pressure used to keep its TContext's views, and with them the
   mapping and memtable of its POV and of every ancestor as of the call: whatever a merge had
   replaced since could not be freed until the wait ended. TContext::ReleaseViews drops them once
   the statement has read and committed.

   A safe child under a fast root (merged in memory, by hand; a child's writes would need a Tetris
   player). A context on the child reads two keys, which live in the root; a writer then
   overwrites one and the root's memory is merged, which replaces the root mapping the context
   pinned. Checks that:
   (a) until it releases, the context still reads its own snapshot, not the newer write;
   (b) releasing drops exactly one reference from the pinned mapping of the child and of its
       ancestor;
   (c) values read before the release stay valid after it, and after the merge;
   (d) any read after the release throws instead of seeing an empty repo, and so does opening a
       key cursor;
   (e) a context with a key cursor open refuses to release, and keeps reading. */
FIXTURE(ContextReleasesViewsBeforeWait) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Orly::Indy::Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    /* The root's merges must not park (see TLatchedFastRepo). */ {
      std::lock_guard<std::mutex> lock(MergeLatch.Mutex);
      MergeLatch.Released = true;
    }
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      TSuprena arena;
      const Base::TUuid idx_id(TUuid::Twister);
      auto root = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, false);
      auto child = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), root, true);
      auto *latched = dynamic_cast<TLatchedFastRepo *>(root.Get());
      if (!EXPECT_TRUE(latched != nullptr)) {
        std::lock_guard<std::mutex> lock(mut);
        fin = true;
        cond.notify_one();
        return;
      }
      const auto index_key = [&](int64_t key) {
        return TIndexKey(idx_id, TKey(make_tuple(key), &arena, state));
      };
      const auto commit = [&](const L0::TManager::TPtr<Orly::Indy::TRepo> &repo, int64_t key, int64_t val) {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(index_key(key), TKey(val, &arena, state), TMutator::Assign);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      commit(root, 1L, 10L);
      commit(root, 2L, 20L);
      /* (a), (b), (c), (d) */ {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        const TKey root_val = context[index_key(1L)];
        const TKey child_val = context[index_key(2L)];
        EXPECT_EQ(TKey(&arena, state, root_val), TKey(10L, &arena, state));
        EXPECT_EQ(TKey(&arena, state, child_val), TKey(20L, &arena, state));
        /* Probes taken after the context, so they pin the same mappings (nothing else merges). */
        Orly::Indy::TRepo::TView root_probe(root), child_probe(child);
        const size_t root_refs = root_probe.GetMapping()->GetRefCount();
        const size_t child_refs = child_probe.GetMapping()->GetRefCount();
        /* Another writer, then a merge that replaces the root's mapping. */
        commit(root, 2L, 21L);
        latched->QueueMergeMem();
        latched->StepMem();
        /* (a) */
        EXPECT_EQ(TKey(&arena, state, context[index_key(2L)]), TKey(20L, &arena, state));
        EXPECT_EQ(root_probe.GetMapping()->GetRefCount(), root_refs);
        /* (b) */
        context.ReleaseViews();
        EXPECT_TRUE(context.HasReleasedViews());
        EXPECT_EQ(root_probe.GetMapping()->GetRefCount(), root_refs - 1UL);
        EXPECT_EQ(child_probe.GetMapping()->GetRefCount(), child_refs - 1UL);
        /* (c) */
        EXPECT_EQ(TKey(&arena, state, root_val), TKey(10L, &arena, state));
        EXPECT_EQ(TKey(&arena, state, child_val), TKey(20L, &arena, state));
        /* (d) */
        bool threw = false;
        try {
          (void) context[index_key(1L)];
        } catch (const logic_error &) {
          threw = true;
        }
        EXPECT_TRUE(threw);
        threw = false;
        try {
          (void) context.Exists(index_key(2L));
        } catch (const logic_error &) {
          threw = true;
        }
        EXPECT_TRUE(threw);
        threw = false;
        try {
          TContext::TKeyCursor csr(&context, index_key(2L));
        } catch (const logic_error &) {
          threw = true;
        }
        EXPECT_TRUE(threw);
        /* Releasing twice is harmless. */
        context.ReleaseViews();
      }
      /* A fresh context sees the newer write. */ {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        EXPECT_EQ(TKey(&arena, state, context[index_key(2L)]), TKey(21L, &arena, state));
      }
      /* (e) */ {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        bool threw = false;
        {
          TContext::TKeyCursor csr(&context, index_key(2L));
          EXPECT_TRUE(static_cast<bool>(csr));
          try {
            context.ReleaseViews();
          } catch (const logic_error &) {
            threw = true;
          }
          EXPECT_FALSE(context.HasReleasedViews());
        }
        EXPECT_TRUE(threw);
        EXPECT_EQ(TKey(&arena, state, context[index_key(1L)]), TKey(10L, &arena, state));
        context.ReleaseViews();
        EXPECT_TRUE(context.HasReleasedViews());
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(RangeCountsPreserveVisibilityAndSnapshots) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    class TContextWithoutCounts final : public Orly::TContextBase {
      public:
      explicit TContextWithoutCounts(Orly::Indy::TContext &context) : TContextBase(context.GetArena()), Context(context) {}
      TKey operator[](const TIndexKey &key) override { return Context[key]; }
      bool Exists(const TIndexKey &key) override { return Context.Exists(key); }
      private:
      Orly::Indy::TContext &Context;
    };
    class TCursorPackageContext final : public Orly::Package::TContext {
      public:
      TCursorPackageContext(Orly::Indy::TContext &context, TSuprena &arena, TScheduler &scheduler)
          : Orly::Package::TContext({}, Base::TUuid(TUuid::Twister), &arena, &scheduler, {}, {}), Context(context) {}
      Orly::TContextBase &GetFlux() override { return Context; }
      Orly::TKeyCursor *NewKeyCursor(Orly::TContextBase *, const TIndexKey &pattern) const override {
        return new Orly::Indy::TContext::TKeyCursor(&Context, pattern);
      }
      Orly::TKeyCursor *NewKeyCursor(Orly::TContextBase *, const TIndexKey &from, const TIndexKey &to) const override {
        return new Orly::Indy::TContext::TKeyCursor(&Context, from, to);
      }
      private:
      Orly::Indy::TContext &Context;
    };
    TRunnerFileCaches file_caches;
    TSuprena arena;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      const Base::TUuid index_id(TUuid::Twister);
      auto root = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      auto child = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), root, true);
      /* Keep the child's updates unpromoted; this fixture has no Tetris service. */
      {
        auto transaction = manager.NewTransaction();
        transaction->Pause(child);
        transaction->Prepare();
        transaction->CommitAction();
      }
      const auto key = [&](int64_t n) { return TIndexKey(index_id, TKey(make_tuple(n), &arena, state)); };
      const TIndexKey all(index_id, TKey(make_tuple(Orly::Native::TFree<int64_t>()), &arena, state));
      const auto commit = [&](const L0::TManager::TPtr<TRepo> &repo, int64_t n, const TKey &value, TMutator mutator = TMutator::Assign) {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(key(n), value, mutator);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      const auto flush = [&](const L0::TManager::TPtr<TRepo> &repo) {
        dynamic_cast<TSteppedSafeRepo *>(repo.Get())->StepMergeMem();
      };
      for (int64_t n = 1; n <= 3; ++n) {
        commit(root, n, TKey(n, &arena, state));
      }
      flush(root);
      commit(root, 2, TKey(10L, &arena, state), TMutator::Add);
      commit(root, 2, TKey(20L, &arena, state), TMutator::Add);
      flush(root);
      TSuprena pinned_arena;
      TContext pinned(child, &pinned_arena);
      EXPECT_EQ(pinned.CountKeys(all), 3L);
      EXPECT_EQ(pinned.GetFoldDedupProbes(), 0UL);
      commit(root, 1, TKey(Orly::Native::TTombstone::Tombstone, &arena, state));
      commit(root, 4, TKey(7L, &arena, state), TMutator::Add);
      {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        EXPECT_EQ(context.CountKeys(all), 3L);
        EXPECT_EQ(context.CountKeys(key(1)), 0L);
        EXPECT_EQ(context.CountKeys(key(4)), 1L);
        EXPECT_EQ(context.CountKeys(key(2), key(3)), 2L);
        EXPECT_EQ(context.CountKeys(key(2), key(2)), 1L);
        EXPECT_EQ(context.CountKeys(key(9)), 0L);
        EXPECT_EQ(context.CountKeys(key(9), key(10)), 0L);
        const TIndexKey other_index(Base::TUuid(TUuid::Twister), key(3).GetKey());
        auto mismatched_indexes = [&]() { (void) context.CountKeys(key(2), other_index); };
        EXPECT_THROW_FUNC(invalid_argument, mismatched_indexes);
        EXPECT_EQ(context.GetFoldDedupProbes(), 0UL);
        EXPECT_EQ(context[key(2)], TKey(32L, &arena, state));
        context.ReleaseViews();
        auto count_after_release = [&]() { (void) context.CountKeys(all); };
        EXPECT_THROW_FUNC(logic_error, count_after_release);
      }
      flush(root);
      EXPECT_EQ(pinned.CountKeys(all), 3L);
      EXPECT_EQ(pinned.CountKeys(key(1)), 1L);
      EXPECT_EQ(pinned.CountKeys(key(4)), 0L);
      /* A child shadows a parent's key, and a deferred upsert adds one key. */
      /* Give the tombstone newer sequence numbers than the parent's entries. */
      for (int i = 0; i < 10; ++i) {
        commit(child, 3, TKey(Orly::Native::TTombstone::Tombstone, &arena, state));
      }
      commit(child, 2, TKey(1L, &arena, state), TMutator::Add);
      commit(child, 5, TKey(9L, &arena, state), TMutator::Add);
      {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        TIndyContext package_context({}, Base::TUuid(TUuid::Twister), context, &ctx_arena, &scheduler, {}, {});
        const auto pattern = make_tuple(Orly::Native::TFree<int64_t>());
        EXPECT_EQ(package_context.CountKeys(context, index_id, pattern), 3L);
        EXPECT_EQ(context.GetFoldDedupProbes(), 0UL);
        TContextWithoutCounts legacy_context(context);
        TCursorPackageContext legacy_package_context(context, ctx_arena, scheduler);
        EXPECT_EQ(legacy_package_context.CountKeys(legacy_context, index_id, pattern), 3L);
        EXPECT_GT(context.GetFoldDedupProbes(), 0UL);
      }
      flush(child);
      {
        TSuprena ctx_arena;
        TContext context(child, &ctx_arena);
        EXPECT_EQ(context.CountKeys(all), 3L);
        EXPECT_EQ(context.CountKeys(key(3)), 0L);
        EXPECT_EQ(context.CountKeys(key(2), key(5)), 3L);
        EXPECT_EQ(context.GetFoldDedupProbes(), 0UL);
        EXPECT_EQ(context[key(2)], TKey(33L, &arena, state));
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(RangeCountsRespectReadBudgetsWithoutFoldingValues) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    TSuprena arena;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      auto repo = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      const Base::TUuid index_id(TUuid::Twister);
      const TIndexKey all(index_id, TKey(make_tuple(Orly::Native::TFree<int64_t>()), &arena, state));
      const TIndexKey absent(index_id, TKey(make_tuple(99L), &arena, state));
      const string value(32768, 'x');
      for (int pass = 0; pass < 2; ++pass) {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        for (int64_t n = 1; n <= 3; ++n) {
          update->AddEntry(TIndexKey(index_id, TKey(make_tuple(n), &arena, state)), TKey(value, &arena, state), TMutator::Add);
        }
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
        dynamic_cast<TSteppedSafeRepo *>(repo.Get())->StepMergeMem();
      }
      {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        const auto bytes_before = ctx_arena.GetByteSize();
        context.SetReadBudget(3, 1, &ctx_arena, 0);
        EXPECT_EQ(context.CountKeys(all), 3L);
        EXPECT_EQ(context.GetRowsWalked(), 3UL);
        EXPECT_EQ(context.GetFoldDedupProbes(), 0UL);
        EXPECT_EQ(ctx_arena.GetByteSize(), bytes_before);
        EXPECT_EQ(context.CountKeys(absent), 0L);
        auto over_rows = [&]() { (void) context.CountKeys(all); };
        EXPECT_THROW_FUNC(Orly::Server::TReadTooLarge, over_rows);
        EXPECT_EQ(context.GetRowsWalked(), 4UL);
        context.ClearReadBudget();
        EXPECT_EQ(context.CountKeys(all), 3L);
        const TIndexKey first(index_id, TKey(make_tuple(1L), &arena, state));
        EXPECT_EQ(context[first], TKey(value + value, &arena, state));
        EXPECT_GT(ctx_arena.GetByteSize(), bytes_before);
      }
      {
        TSuprena ctx_arena;
        TContext context(repo, &ctx_arena);
        TKey allocated(string(1024, 'y'), &ctx_arena, state);
        context.SetReadBudget(0, 1, &ctx_arena, 0);
        auto over_bytes = [&]() { (void) context.CountKeys(absent); };
        EXPECT_THROW_FUNC(Orly::Server::TReadTooLarge, over_bytes);
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745: reads at a version, checked against a model of every write. Each scenario commits a random run of
   assigns, deletes and `+=` over a few keys, one hot key written by every commit so its history runs deep, flushing a
   disk file per round and merging the files twice. After each round it reads every version the repo will answer,
   by point and by range, and compares it with the model; a version below the history horizon, or past the newest,
   must be refused. With history kept (no pruning, merges left unfolded) the horizon must stay 0, so every version
   back to the first is read out of merged disk files. */
namespace {

  struct TReadAtScenario {
    bool Prune;
    bool Unfolded;
    uint32_t Seed;
    /* #745 stage 2: --retention_versions, and whether each merge round also tails the oldest file. */
    size_t RetainVersions = 0UL;
    bool Tail = false;
  };

  struct TReadAtStats {
    /* (version, key) reads whose value differs from the key's newest: each read history out of a disk file. */
    size_t PastValues = 0UL;
    size_t Mismatches = 0UL;
    size_t Refusals = 0UL;
    size_t MaxDiskLayers = 0UL;
    size_t DiskLayersAfterMerges = 0UL;
    TSequenceNumber Horizon = 0UL;
    /* Checks at which the horizon cut into the retention window. */
    size_t WindowCuts = 0UL;
    /* (first, second) pairs whose changed keys were compared with the model (#889). */
    size_t ChangedChecks = 0UL;
  };

  /* Compares the changed-key cursor with the model for pairs of versions in [oldest, newest]. */
  template <typename TExpectedAt>
  size_t CheckChangedKeys(TRootRepoFixture &root, const TExpectedAt &expected_at, TSequenceNumber oldest,
                          TSequenceNumber newest, int64_t hot_key, int64_t num_keys, size_t &mismatches) {
    size_t checks = 0UL;
    const auto check_pair = [&](TSequenceNumber first, TSequenceNumber second) {
      ++checks;
      const auto &before = expected_at(first);
      const auto &after = expected_at(second);
      vector<TKey> expected;
      for (int64_t key = 0L; key < num_keys; ++key) {
        const auto lhs = before.find(key), rhs = after.find(key);
        if ((lhs == before.end()) != (rhs == after.end()) || (lhs != before.end() && lhs->second != rhs->second)) {
          expected.push_back(root.KeyOf(key));
        }
      }
      if (root.ChangedKeys(0L, num_keys - 1L, first, second) != expected && mismatches++ < 10UL) {
        cerr << "changed between " << first << " and " << second << " differs from the model" << endl;
      }
      const bool hot_changed = !expected.empty() && expected.front() == root.KeyOf(hot_key);
      if (root.ChangedKeys(hot_key, hot_key, first, second, true).size() != (hot_changed ? 1UL : 0UL) && mismatches++ < 10UL) {
        cerr << "changed between " << first << " and " << second << " differs from the model for key " << hot_key << endl;
      }
    };
    /* Negative controls: a version below the horizon or past the newest is refused, whichever side it is on. */
    const auto refuses = [&](TSequenceNumber first, TSequenceNumber second) {
      try {
        root.ChangedKeys(0L, num_keys - 1L, first, second);
      } catch (const Orly::Server::TVersionUnavailable &) {
        return true;
      }
      return false;
    };
    if (oldest) {
      for (const bool refused : {refuses(oldest - 1UL, newest), refuses(newest, oldest - 1UL)}) {
        if (!refused) {
          ++mismatches;
          cerr << "changed between below the horizon " << oldest << " was not refused" << endl;
        }
      }
    }
    for (const bool refused : {refuses(oldest, newest + 1UL), refuses(newest + 1UL, oldest)}) {
      if (!refused) {
        ++mismatches;
        cerr << "changed between past the newest version " << newest << " was not refused" << endl;
      }
    }
    for (TSequenceNumber first = oldest; first <= newest; first += 3UL) {
      check_pair(first, newest);
      check_pair(newest, first);
      check_pair(first, first);
      if (first + 1UL <= newest) {
        check_pair(first, first + 1UL);
      }
    }
    return checks;
  }

  TReadAtStats RunReadAtScenario(const TReadAtScenario &scenario) {
    constexpr int64_t num_keys = 8L, hot_key = 0L;
    constexpr size_t num_rounds = 6UL, commits_per_round = 12UL;
    TReadAtStats stats;
    TRootRepoFixture root(scenario.Prune);
    if (scenario.Unfolded) {
      root.KeepMergesUnfolded();
    }
    if (scenario.RetainVersions) {
      /* Leases expire at once, so the reads below don't hold history past the window. */
      root.SetRetention(scenario.RetainVersions, 0UL, chrono::steady_clock::duration::zero());
    }
    mt19937 rng(scenario.Seed);
    /* The model: the state after each commit, by the commit's sequence number. */
    map<int64_t, int64_t> state;
    map<TSequenceNumber, map<int64_t, int64_t>> history;
    const auto expected_at = [&](TSequenceNumber at) -> const map<int64_t, int64_t> & {
      static const map<int64_t, int64_t> empty;
      auto iter = history.upper_bound(at);
      return iter == history.begin() ? empty : prev(iter)->second;
    };
    const auto write = [&](int64_t key) {
      const uint32_t roll = rng() % 10U;
      const auto found = state.find(key);
      if (found != state.end() && roll < 2U) {
        state.erase(found);
        return root.CommitDelete(key);
      }
      if (found != state.end() && roll < 6U) {
        const int64_t delta = static_cast<int64_t>(rng() % 9U) + 1L;
        found->second += delta;
        return root.Commit({{key, delta, TMutator::Add}});
      }
      const int64_t val = static_cast<int64_t>(rng() % 1000U);
      state[key] = val;
      return root.Commit({{key, val, TMutator::Assign}});
    };
    const auto check = [&]() {
      stats.Horizon = root.GetHistoryHorizon();
      const TSequenceNumber newest = history.empty() ? 0UL : history.rbegin()->first;
      const auto &now = expected_at(newest);
      if (scenario.RetainVersions && newest >= scenario.RetainVersions && stats.Horizon > newest - scenario.RetainVersions + 1UL) {
        ++stats.WindowCuts;
      }
      for (TSequenceNumber at = stats.Horizon; at <= newest; ++at) {
        const auto &expected = expected_at(at);
        vector<TKey> expected_keys;
        for (int64_t key = 0L; key < num_keys; ++key) {
          const auto found = expected.find(key);
          const auto actual = root.ReadAt(key, at);
          bool ok;
          if (found == expected.end()) {
            ok = !actual;
          } else {
            expected_keys.push_back(root.KeyOf(key));
            ok = actual && *actual == root.Int(found->second);
            const auto cur = now.find(key);
            if (ok && (cur == now.end() || cur->second != found->second)) {
              ++stats.PastValues;
            }
          }
          if (!ok && stats.Mismatches++ < 10UL) {
            cerr << "seed " << scenario.Seed << ": key " << key << " at " << at << " expected "
                 << (found == expected.end() ? string("nothing") : to_string(found->second)) << ", got "
                 ; if (actual) { cerr << *actual; } else { cerr << "nothing"; } cerr << " (prune " << scenario.Prune << ", unfolded " << scenario.Unfolded << ")" << endl;
          }
        }
        if (root.KeysAt(0L, num_keys - 1L, at) != expected_keys && stats.Mismatches++ < 10UL) {
          cerr << "seed " << scenario.Seed << ": range at " << at << " differs" << endl;
        }
      }
      /* #889: the keys that differ between two versions, against the model, for a spread of pairs (either order, an
         empty pair, the whole window) and, for one key, as a pattern. */
      stats.ChangedChecks += CheckChangedKeys(root, expected_at, stats.Horizon, newest, hot_key, num_keys, stats.Mismatches);
      /* The negative controls: below the horizon, and past the newest. */
      const auto refused = [&](TSequenceNumber at) {
        try {
          root.ReadAt(hot_key, at);
        } catch (const Orly::Server::TVersionUnavailable &ex) {
          return ex.GetRequested() == at && ex.GetOldest() == stats.Horizon && ex.GetNewest() == newest;
        }
        return false;
      };
      if (stats.Horizon) {
        stats.Refusals += refused(stats.Horizon - 1UL) ? 1UL : 0UL;
        stats.Refusals += refused(0UL) ? 1UL : 0UL;
      }
      stats.Refusals += refused(newest + 1UL) ? 1UL : 0UL;
    };
    size_t expected_refusals = 0UL;
    for (size_t round = 0UL; round < num_rounds; ++round) {
      for (size_t i = 0UL; i < commits_per_round; ++i) {
        history[write(hot_key)] = state;
        const int64_t key = 1L + static_cast<int64_t>(rng() % static_cast<uint32_t>(num_keys - 1L));
        history[write(key)] = state;
      }
      root.Flush();
      const auto [disk, mem] = root.CountLayersByKind();
      EXPECT_EQ(mem, 0UL);
      stats.MaxDiskLayers = max(stats.MaxDiskLayers, disk);
      if (round == 2UL || round == num_rounds - 1UL) {
        root.MergeDisk(8UL);
        if (scenario.Tail) {
          root.Tail();
        }
      }
      check();
      expected_refusals += stats.Horizon ? 3UL : 1UL;
    }
    stats.DiskLayersAfterMerges = root.CountLayersByKind().first;
    /* And with the newest writes still in memory, over the merged files. */
    for (size_t i = 0UL; i < 4UL; ++i) {
      history[write(hot_key)] = state;
    }
    check();
    expected_refusals += stats.Horizon ? 3UL : 1UL;
    EXPECT_EQ(stats.Refusals, expected_refusals);
    return stats;
  }

}  // namespace

FIXTURE(ReadAtVersionMatchesModel) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    for (uint32_t seed : {1U, 2U, 3U}) {
      /* History kept: no merge drops any, so every version reads, out of merged files. */ {
        const auto stats = RunReadAtScenario({.Prune = false, .Unfolded = true, .Seed = seed});
        EXPECT_EQ(stats.Mismatches, 0UL);
        EXPECT_GT(stats.ChangedChecks, 0UL);
        EXPECT_EQ(stats.Horizon, 0UL);
        EXPECT_GT(stats.PastValues, 0UL);
        EXPECT_GE(stats.MaxDiskLayers, 3UL);
        EXPECT_LT(stats.DiskLayersAfterMerges, stats.MaxDiskLayers);
      }
      /* Folded merges: the fold drops the `+=` history it folds, so the horizon rises past it. */ {
        const auto stats = RunReadAtScenario({.Prune = false, .Unfolded = false, .Seed = seed});
        EXPECT_EQ(stats.Mismatches, 0UL);
        EXPECT_GT(stats.Horizon, 0UL);
        EXPECT_LT(stats.DiskLayersAfterMerges, stats.MaxDiskLayers);
      }
      /* Pruning merges, unfolded: each key keeps only its newest entry. */ {
        const auto stats = RunReadAtScenario({.Prune = true, .Unfolded = true, .Seed = seed});
        EXPECT_EQ(stats.Mismatches, 0UL);
        EXPECT_GT(stats.Horizon, 0UL);
        EXPECT_LT(stats.DiskLayersAfterMerges, stats.MaxDiskLayers);
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745 stage 2: with a retention window, merges, folds and tails keep every version in it. The window (20 versions)
   is shorter than the run (~150), so the horizon still rises, but never past the window's start; every version from
   the horizon up reads as the model says, `+=` keys included, out of folded files. */
FIXTURE(ReadAtVersionWithRetentionWindow) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    for (uint32_t seed : {1U, 2U, 3U}) {
      for (bool prune : {true, false}) {
        for (bool unfolded : {false, true}) {
          for (bool tail : {false, true}) {
            const auto stats = RunReadAtScenario({.Prune = prune, .Unfolded = unfolded, .Seed = seed, .RetainVersions = 20UL, .Tail = tail});
            EXPECT_EQ(stats.Mismatches, 0UL);
            EXPECT_EQ(stats.WindowCuts, 0UL);
            EXPECT_GT(stats.PastValues, 0UL);
            EXPECT_LT(stats.DiskLayersAfterMerges, stats.MaxDiskLayers);
            if (prune || !unfolded) {
              EXPECT_GT(stats.Horizon, 0UL);
            }
          }
        }
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745 stage 2: a read at a version leases it. Once the window has moved past the version, merges still keep it
   while the lease lasts, so the read can go on; with the lease gone, the next merge drops it. */
FIXTURE(ReadAtVersionLeaseHoldsHistory) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(true);
      root.SetRetention(2UL, 0UL, chrono::hours(1));
      const TSequenceNumber leased = root.Commit({{1L, 10L, TMutator::Assign}});
      root.Commit({{2L, 5L, TMutator::Assign}});
      root.Flush();
      EXPECT_TRUE(root.ReadAt(1L, leased) == root.Int(10L));
      for (int64_t i = 0L; i < 8L; ++i) {
        root.Commit({{1L, 11L + i, TMutator::Assign}});
        root.Commit({{2L, 1L, TMutator::Add}});
        root.Flush();
      }
      root.MergeDisk(8UL);
      root.Tail();
      EXPECT_LE(root.GetHistoryHorizon(), leased);
      EXPECT_TRUE(root.ReadAt(1L, leased) == root.Int(10L));
      EXPECT_FALSE(root.ReadAt(2L, leased));
      /* Without the lease, the window alone decides. */
      root.SetRetention(2UL, 0UL, chrono::steady_clock::duration::zero());
      root.ReadAt(1L, leased);
      root.Commit({{1L, 100L, TMutator::Assign}});
      root.Flush();
      root.MergeDisk(8UL);
      root.Tail();
      EXPECT_GT(root.GetHistoryHorizon(), leased);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #889: `hold version` keeps a version's history through merges like a lease, until it is released or lapses; with no
   retention at all it is refused, as nothing could be kept, and so is a version outside what can be read. */
FIXTURE(HoldVersionKeepsHistory) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(true);
      /* Negative control: no window and no save points. */
      const TSequenceNumber held = root.Commit({{1L, 10L, TMutator::Assign}});
      root.Flush();
      { auto thrower = [&] { (void) root.GetSafe()->HoldVersion(held, chrono::seconds(60)); }; EXPECT_THROW_FUNC(std::runtime_error, thrower); }
      root.SetRetention(2UL, 0UL, chrono::steady_clock::duration::zero());
      { auto thrower = [&] { (void) root.GetSafe()->HoldVersion(held + 1000UL, chrono::seconds(60)); }; EXPECT_THROW_FUNC(Orly::Server::TVersionUnavailable, thrower); }
      const auto hold = root.GetSafe()->HoldVersion(held, chrono::seconds(600));
      for (int64_t i = 0L; i < 8L; ++i) {
        root.Commit({{1L, 11L + i, TMutator::Assign}});
        root.Flush();
      }
      root.MergeDisk(8UL);
      root.Tail();
      EXPECT_LE(root.GetHistoryHorizon(), held);
      EXPECT_TRUE(root.ReadAt(1L, held) == root.Int(10L));
      EXPECT_TRUE(root.GetSafe()->ReleaseHold(hold));
      EXPECT_FALSE(root.GetSafe()->ReleaseHold(hold));
      root.Commit({{1L, 100L, TMutator::Assign}});
      root.Flush();
      root.MergeDisk(8UL);
      root.Tail();
      EXPECT_GT(root.GetHistoryHorizon(), held);
      /* The version is gone, so a hold of it is refused, and leaves nothing behind. */
      { auto thrower = [&] { (void) root.GetSafe()->HoldVersion(held, chrono::seconds(60)); }; EXPECT_THROW_FUNC(Orly::Server::TVersionUnavailable, thrower); }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #889: `at time` resolves through the (time, version) samples of the seconds window: to the newest sample at or before
   the time, refusing a time before the first sample and one in the future, and refusing outright with no seconds window. */
FIXTURE(ResolveTimeThroughSamples) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(true);
      root.Commit({{1L, 1L, TMutator::Assign}});
      root.Flush();
      { auto thrower = [&] { (void) root.GetSafe()->ResolveTime(chrono::system_clock::now()); }; EXPECT_THROW_FUNC(std::runtime_error, thrower); }
      /* A window of at most 1024 seconds samples at most a second apart. */
      root.SetRetention(0UL, 1000UL, chrono::steady_clock::duration::zero());
      const auto start = chrono::system_clock::now();
      const TSequenceNumber first = root.Commit({{1L, 10L, TMutator::Assign}});
      root.Flush();
      root.MergeDisk(8UL);
      root.Tail();
      /* The merge took the first sample. A read of "now" finds it, and not the writes since. */
      const TSequenceNumber second = root.Commit({{1L, 11L, TMutator::Assign}});
      root.Flush();
      EXPECT_EQ(root.GetSafe()->ResolveTime(chrono::system_clock::now()), first);
      /* Before the first sample, and in the future: refused. */
      { auto thrower = [&] { (void) root.GetSafe()->ResolveTime(start - chrono::hours(1)); }; EXPECT_THROW_FUNC(Orly::Server::TTimeUnavailable, thrower); }
      { auto thrower = [&] { (void) root.GetSafe()->ResolveTime(chrono::system_clock::now() + chrono::hours(1)); }; EXPECT_THROW_FUNC(Orly::Server::TTimeUnavailable, thrower); }
      /* Far in the past: the clock's minimum (what a year-0000 time parses to) and the year 1700.  Subtracting
         these from now overflows, which used to resolve them to the newest version. */
      for (const auto when : { chrono::system_clock::time_point::min(),
                               chrono::system_clock::time_point(chrono::seconds(-7'479'000'000L)),
                               chrono::system_clock::time_point(chrono::seconds(-8'000'000'000L)) }) {
        auto thrower = [&] { (void) root.GetSafe()->ResolveTime(when); };
        EXPECT_THROW_FUNC(Orly::Server::TTimeUnavailable, thrower);
      }
      /* A second sample, a spacing later, takes over. */
      this_thread::sleep_for(chrono::milliseconds(1100));
      /* The first call takes the sample, so a time just before it still resolves to the old one. */
      EXPECT_EQ(root.GetSafe()->ResolveTime(chrono::system_clock::now()), first);
      const auto between = chrono::system_clock::now();
      EXPECT_EQ(root.GetSafe()->ResolveTime(between), second);
      this_thread::sleep_for(chrono::milliseconds(10));
      root.Commit({{1L, 12L, TMutator::Assign}});
      root.Flush();
      /* The earlier time still resolves to what it did. */
      EXPECT_EQ(root.GetSafe()->ResolveTime(start + chrono::milliseconds(500)), first);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745 stage 3: with persisted retention, a restart keeps the history horizon and the save points. Without, the
   reload raises the horizon to the newest version the files hold (stage 2). After the restart the window is off, and
   the record and its save point go on holding history through merges. */
FIXTURE(PersistedRetentionSurvivesRestart) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    for (bool persisted : {false, true}) {
      TRetentionStoreSim sim;
      TRootRepoFixture root(true, &sim);
      root.SetRetention(3UL, 0UL, chrono::steady_clock::duration::zero());
      for (int64_t i = 0L; i < 6L; ++i) {
        root.Commit({{1L, 10L + i, TMutator::Assign}});
        root.Flush();
      }
      root.MergeDisk(8UL);
      const TSequenceNumber at = root.Commit({{1L, 50L, TMutator::Assign}});
      root.Flush();
      root.GetSafe()->SetSavePoint("sp", at);
      for (int64_t i = 0L; i < 6L; ++i) {
        root.Commit({{1L, 51L + i, TMutator::Assign}});
        root.Flush();
      }
      root.MergeDisk(8UL);
      const TSequenceNumber newest = root.Commit({{1L, 99L, TMutator::Assign}});
      root.Flush();
      EXPECT_LE(root.GetHistoryHorizon(), at);
      EXPECT_TRUE(root.ReadAt(1L, at) == root.Int(50L));
      if (!persisted) {
        /* As if they had never been kept: a store without records (stage 2). */
        std::lock_guard<std::mutex> lock(sim.Mutex);
        sim.Records.clear();
      }
      root.Restart();
      if (persisted) {
        EXPECT_LE(root.GetHistoryHorizon(), at);
        EXPECT_TRUE(root.ReadAt(1L, at) == root.Int(50L));
        const auto save_points = root.GetSafe()->GetSavePoints();
        EXPECT_EQ(save_points.size(), 1UL);
        EXPECT_TRUE(save_points.count("sp") && save_points.at("sp") == at);
        /* The window is off now; the record still holds. */
        root.MergeDisk(8UL);
        EXPECT_LE(root.GetHistoryHorizon(), at);
        EXPECT_TRUE(root.ReadAt(1L, at) == root.Int(50L));
        /* With the save point gone and no window, merges may drop history again. */
        EXPECT_TRUE(root.GetSafe()->DeleteSavePoint("sp"));
      } else {
        EXPECT_EQ(root.GetHistoryHorizon(), newest);
        EXPECT_TRUE(root.GetSafe()->GetSavePoints().empty());
      }
      EXPECT_TRUE(root.ReadAt(1L, newest) == root.Int(99L));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* A repo that is only read after a restart still merges the files it came back with: reopening it queues a disk
   merge, as every other mapping install does, when there are three or more and a pair is mergeable. */
FIXTURE(ReopenedRepoQueuesItsDiskMerge) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    for (size_t files : {2UL, 6UL}) {
      TRetentionStoreSim sim;
      TRootRepoFixture root(true, &sim);
      for (size_t i = 0UL; i < files; ++i) {
        root.Commit({{static_cast<int64_t>(i), 10L, TMutator::Assign}});
        root.Flush();
      }
      EXPECT_EQ(root.CountLayersByKind().first, files);
      root.Restart();
      EXPECT_EQ(root.CountLayersByKind().first, files);
      EXPECT_EQ(root.IsMergeDiskQueued(), files >= 3UL);
      if (files >= 3UL) {
        root.MergeDisk(1UL);
        EXPECT_LT(root.CountLayersByKind().first, files);
      }
      EXPECT_TRUE(root.Read(static_cast<int64_t>(files) - 1L) == root.Int(10L));
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745 stage 3: a merge never drops a version the record on disk doesn't let it: until a save of the record is on
   disk, merges keep everything older; once it is, the window decides again. */
FIXTURE(PersistedRetentionWaitsForTheRecord) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRetentionStoreSim sim;
      sim.Hold = true;
      TRootRepoFixture root(true, &sim);
      root.SetRetention(1UL, 0UL, chrono::steady_clock::duration::zero());
      const TSequenceNumber first = root.Commit({{1L, 10L, TMutator::Assign}});
      root.Flush();
      for (int64_t i = 0L; i < 6L; ++i) {
        root.Commit({{1L, 11L + i, TMutator::Assign}});
        root.Flush();
      }
      root.MergeDisk(8UL);
      root.Tail();
      EXPECT_LE(root.GetHistoryHorizon(), first);
      EXPECT_TRUE(root.ReadAt(1L, first) == root.Int(10L));
      EXPECT_GE(sim.Saves, 1UL);
      sim.Release();
      for (int64_t i = 0L; i < 3L; ++i) {
        root.Commit({{1L, 20L + i, TMutator::Assign}});
        root.Flush();
        root.MergeDisk(8UL);
        root.Tail();
      }
      EXPECT_GT(root.GetHistoryHorizon(), first);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745 stage 3: a slave never drops a version at or above the horizon its master ships, whatever its own window
   says, so a failover to it keeps every version the master promised. With none shipped yet, it keeps everything. */
FIXTURE(PersistedRetentionSlaveHoldsToShippedHorizon) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRetentionStoreSim sim;
      sim.Ships = false;
      TRootRepoFixture root(true, &sim);
      root.SetRetention(1UL, 0UL, chrono::steady_clock::duration::zero());
      const TSequenceNumber first = root.Commit({{1L, 10L, TMutator::Assign}});
      root.Flush();
      for (int64_t i = 0L; i < 4L; ++i) {
        root.Commit({{1L, 11L + i, TMutator::Assign}});
        root.Flush();
      }
      root.MergeDisk(8UL);
      root.Tail();
      /* Nothing shipped: the cap is the empty record's horizon, 0. */
      EXPECT_LE(root.GetHistoryHorizon(), first);
      EXPECT_TRUE(root.ReadAt(1L, first) == root.Int(10L));
      const TSequenceNumber shipped = root.Commit({{1L, 30L, TMutator::Assign}});
      root.Flush();
      /* acquire the sim's lock */ {
        Orly::Indy::TRepo::TRetentionRecord rec;
        rec.Horizon = shipped;
        std::lock_guard<std::mutex> lock(sim.Mutex);
        sim.Records[Orly::Indy::TRepo::GetRetentionRecordId(root.GetSafe()->GetId(), true)] = rec.Encode();
      }
      for (int64_t i = 0L; i < 4L; ++i) {
        root.Commit({{1L, 31L + i, TMutator::Assign}});
        root.Flush();
        root.MergeDisk(8UL);
        root.Tail();
      }
      EXPECT_GT(root.GetHistoryHorizon(), first);
      EXPECT_LE(root.GetHistoryHorizon(), shipped);
      EXPECT_TRUE(root.ReadAt(1L, shipped) == root.Int(30L));
      /* A slave takes its save points from the master, and sets none. */
      bool refused = false;
      try {
        root.GetSafe()->SetSavePoint("sp", shipped);
      } catch (const std::runtime_error &) {
        refused = true;
      }
      EXPECT_TRUE(refused);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #745 stage 3: a record's encoding round-trips, and a truncated or foreign one is refused. */
FIXTURE(RetentionRecordEncoding) {
  Orly::Indy::TRepo::TRetentionRecord rec;
  rec.Horizon = 42UL;
  rec.Versions = 1000UL;
  rec.Seconds = 60UL;
  rec.Samples.emplace_back(chrono::system_clock::now(), 41UL);
  rec.SavePoints["a"] = 42UL;
  rec.SavePoints["bb"] = 50UL;
  const string blob = rec.Encode();
  Orly::Indy::TRepo::TRetentionRecord out;
  EXPECT_TRUE(Orly::Indy::TRepo::TRetentionRecord::TryDecode(blob, out));
  EXPECT_EQ(out.Horizon, 42UL);
  EXPECT_EQ(out.Versions, 1000UL);
  EXPECT_EQ(out.Seconds, 60UL);
  EXPECT_EQ(out.Samples.size(), 1UL);
  EXPECT_TRUE(out.SavePoints == rec.SavePoints);
  for (size_t len = 0UL; len < blob.size(); ++len) {
    EXPECT_FALSE(Orly::Indy::TRepo::TRetentionRecord::TryDecode(blob.substr(0UL, len), out));
  }
  EXPECT_FALSE(Orly::Indy::TRepo::TRetentionRecord::TryDecode(blob + "x", out));
  string foreign = blob;
  foreign[0] ^= 1;
  EXPECT_FALSE(Orly::Indy::TRepo::TRetentionRecord::TryDecode(foreign, out));
  const Base::TUuid id(TUuid::Twister);
  EXPECT_FALSE(Orly::Indy::TRepo::GetRetentionRecordId(id, false) == Orly::Indy::TRepo::GetRetentionRecordId(id, true));
  EXPECT_FALSE(Orly::Indy::TRepo::GetRetentionRecordId(id, false) == id);
}

/* #745: a key whose whole history is newer than the version is not there, in a file that holds older keys. */
FIXTURE(ReadAtVersionBeforeAKeyExists) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    {
      TRootRepoFixture root(false);
      root.KeepMergesUnfolded();
      const TSequenceNumber first = root.Commit({{1L, 10L, TMutator::Assign}});
      const TSequenceNumber second = root.Commit({{2L, 20L, TMutator::Assign}});
      root.Commit({{2L, 21L, TMutator::Assign}});
      root.Flush();
      EXPECT_FALSE(root.ReadAt(2L, first));
      EXPECT_TRUE(root.ReadAt(1L, first) == root.Int(10L));
      EXPECT_TRUE(root.ReadAt(2L, second) == root.Int(20L));
      EXPECT_EQ(root.KeysAt(0L, 9L, first).size(), 1UL);
      EXPECT_EQ(root.KeysAt(0L, 9L, second).size(), 2UL);
      EXPECT_EQ(root.KeysAt(0L, 9L, 0UL).size(), 0UL);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* An idle child POV dirtied after a root repo must not jump ahead of the root repo in the merge
   queue by carrying its older construction/last-merge timestamp; and when both a child POV and a
   root repo are due, RunMergeMem steps the root repo first. */
FIXTURE(MergeMemQueuePrioritizesDueRootReposAndClampsIdleTimestamps) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRunnerFileCaches file_caches;
    vector<uint8_t> state_buf(Sabot::State::GetMaxStateSize());
    void *const state = state_buf.data();
    TScheduler scheduler;
    scheduler.SetPolicy(TScheduler::TPolicy(10, 10, 10ms));
    Disk::Sim::TMemEngine engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    {
      TMyManager manager(engine.GetEngine(), &scheduler);
      TSuprena arena;
      const Base::TUuid idx_id(TUuid::Twister);
      auto root = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), std::nullopt, true);
      auto child = manager.GetRepo(Base::TUuid(TUuid::Twister), TTtl::max(), root, true);
      std::this_thread::sleep_for(5ms);
      auto commit = [&](const L0::TManager::TPtr<TRepo> &repo, int64_t key) {
        auto transaction = manager.NewTransaction();
        auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(Base::TUuid(TUuid::Twister), &arena, state));
        update->AddEntry(TIndexKey(idx_id, TKey(make_tuple(key), &arena, state)), TKey(key, &arena, state), TMutator::Assign);
        transaction->Push(repo, update);
        transaction->Prepare();
        transaction->CommitAction();
      };
      commit(root, 1L);
      std::this_thread::sleep_for(2ms);
      commit(child, 2L);
      /* Child was dirtied after root, so its clamped due time is >= root's due time. */
      auto *stepped_root = dynamic_cast<TSteppedSafeRepo *>(root.Get());
      auto *stepped_child = dynamic_cast<TSteppedSafeRepo *>(child.Get());
      if (EXPECT_TRUE(stepped_root && stepped_child)) {
        EXPECT_TRUE(stepped_child->GetTimeOfNextMergeMem() >= stepped_root->GetTimeOfNextMergeMem());
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* The retention window is capped, so that no --retention_seconds value overflows the conversion to nanoseconds (#1007). */
FIXTURE(RetentionWindowCap) {
  EXPECT_EQ(RetentionWindow(0UL), chrono::seconds(0));
  EXPECT_EQ(RetentionWindow(3600UL), chrono::seconds(3600));
  EXPECT_EQ(RetentionWindow(MaxRetentionSeconds), chrono::seconds(MaxRetentionSeconds));
  EXPECT_EQ(RetentionWindow(MaxRetentionSeconds + 1UL), chrono::seconds(MaxRetentionSeconds));
  EXPECT_EQ(RetentionWindow(numeric_limits<size_t>::max()), chrono::seconds(MaxRetentionSeconds));
  /* Even the largest window fits in nanoseconds. */
  EXPECT_TRUE(chrono::duration_cast<chrono::nanoseconds>(RetentionWindow(numeric_limits<size_t>::max())).count() > 0);
}
