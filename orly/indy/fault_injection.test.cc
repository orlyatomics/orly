/* <orly/indy/fault_injection.test.cc>

   Fault-injection harness for the disk-backed background operations (#608).

   Each case builds some state on a `Sim::TFaultEngine`, makes it durable, then runs one
   operation under a `TFaultPlan`:
   - MergeMem: one TRepo::StepMergeMem of a root safe repo, which writes a data file.
   - MergeDisk: one TSafeRepo::StepMergeDisk of two Assign-only files (the fast path), then
     the repo's teardown, which removes the merge's inputs.
   - MergeDiskFold: the same over two files of `+=` deltas, which takes the fold path
     (MergeFiles phase B).
   - DurableSaveMerge: a durable save, which the writer flushes to a third file, and the
     durable merger merging the three; the layer cleaner then removes the inputs.
   - BaseImage: file-map changes that fill the append log, so the file service writes a base
     image, then two more changes after it. File service only.

   For each operation and each fault mode, N runs from 1 until the operation completes without
   reaching the fault:
   - Write, Read, Sync: the Nth such I/O fails. A failed Read/Write is reported to its caller
     (TOnAbortOnError::Report). Every one of these I/Os passes abort_on_error, so in production
     the disk controller would abort() right there; such a run's outcome is printed with an
     "unreachable:" prefix and never fails the test. A failed Sync throws, as fsync's IfLt0
     does, which production can reach. Reads are rare: these operations read what they just
     wrote, and the page cache serves it.
   - Power: power is lost at the Nth Sync. Sectors written since their device's last Sync are
     lost. PowerTorn: as Power, but each such sector reaches the media with probability 1/2.

   Each (case, mode, N) runs in a child process, so an abort() or a hang ends only that run: the
   child of a single-threaded parent builds its own scheduler and engine, and the parent kills
   it at a deadline. A power loss saves the frozen images and _exit()s at the faulted Sync, as a
   crash would; a second child opens a new engine over them. A run passes when:
   - the operation completes, or fails with an exception, and no blocks leak (the allocator
     holds exactly what the file map and system blocks account for) after the engine's other
     work drains;
   - after a power loss, the engine reopens, every acknowledged write reads back exactly, and
     no blocks leak;
   - nothing hangs or crashes. An abort() after an injected Sync error is recorded but not
     failed: the merges' catch-alls abort on any error but a full disk, by design today (see
     the PR for #608).

   Known failures are pinned in `ExpectedFailures` with their issue: such a run must still fail
   (an expected failure that passes fails the test, so a fix has to remove its entry).

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

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <base/scheduler.h>
#include <base/uuid.h>
#include <orly/indy/context.h>
#include <orly/indy/disk/durable_manager.h>
#include <orly/indy/disk/present_walk_file.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/fault_engine.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/transaction_base.h>

#include <base/test/kit.h>

/* NB: no `using namespace Orly;` -- see context_fold.test.cc (it makes `L0` ambiguous). */
using namespace std;
using namespace std::literals;
using namespace Base;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk::Sim;

namespace Sabot = Orly::Sabot;
using Orly::TMutator;
using Orly::TTtl;

const Orly::Indy::TMasterContext::TProtocol Orly::Indy::TMasterContext::TProtocol::Protocol;
const Orly::Indy::TSlaveContext::TProtocol Orly::Indy::TSlaveContext::TProtocol::Protocol;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping", 1000UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry", 1000UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 1000UL);

Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::Pool(sizeof(Disk::TDurableManager::TMapping), "Durable Mapping", 1000UL);
Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::TEntry::Pool(sizeof(Disk::TDurableManager::TMapping::TEntry), "Durable Mapping Entry", 10000UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TDurableLayer::Pool(std::max(sizeof(Disk::TDurableManager::TMemSlushLayer), sizeof(Disk::TDurableManager::TDiskOrderedLayer)), "Durable Layer", 2000UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TMemSlushLayer::TDurableEntry::Pool(sizeof(Disk::TDurableManager::TMemSlushLayer::TDurableEntry), "Durable Entry", 10000UL);

Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(max(max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation", 1000UL);
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction", 1000UL);

Disk::TBufBlock::TPool Disk::TBufBlock::Pool(Disk::Util::PhysicalBlockSize, 2000UL);

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 10000UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 20000UL);

const std::vector<size_t> MemMergeCoreVec{0};
const std::vector<size_t> DiskMergeCoreVec{0};

using TFramePoolManager = Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>;

/*** Child-side plumbing ***/

/* key=value lines appended to a file with one write() each, so what a child wrote before it
   died (abort, _exit at a power loss, a kill at the deadline) is all there. */
class TRecord {
  NO_COPY(TRecord);
  public:

  explicit TRecord(const string &path)
      : Path(path) {}

  void Put(const string &key, const string &val) const {
    const string line = key + "=" + val + "\n";
    const int fd = open(Path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      ssize_t ignored = write(fd, line.data(), line.size());
      (void)ignored;
      close(fd);
    }
  }

  /* The last value of each key. */
  static map<string, string> Read(const string &path) {
    map<string, string> out;
    ifstream in(path);
    string line;
    while (getline(in, line)) {
      const size_t eq = line.find('=');
      if (eq != string::npos) {
        out[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    return out;
  }

  private:

  const string Path;

};  // TRecord

/* Runs 'test' on a fiber, with the frame pool manager exposed (durable_manager.test.cc). */
static void RunOnFiber(const std::function<void (Fiber::TRunner::TRunnerCons &, TFramePoolManager *)> &test) {
  const size_t stack_size = 8 * 1024UL * 1024UL;
  Fiber::TRunner::TRunnerCons runner_cons(12);
  Fiber::TRunner runner(runner_cons);
  TFramePoolManager frame_pool_manager(30UL, stack_size, &runner);
  if (!Fiber::TFrame::LocalFramePool) {
    Fiber::TFrame::LocalFramePool = new TFramePoolManager::TThreadLocalPool(&frame_pool_manager);
  }
  auto launch_fiber_sched = [](Fiber::TRunner *runner, TFramePoolManager *frame_pool_manager) {
    if (!Fiber::TFrame::LocalFramePool) {
      Fiber::TFrame::LocalFramePool = new TFramePoolManager::TThreadLocalPool(frame_pool_manager);
    }
    runner->Run();
    delete Fiber::TFrame::LocalFramePool;
    Fiber::TFrame::LocalFramePool = nullptr;
  };
  std::thread t1(std::bind(launch_fiber_sched, &runner, &frame_pool_manager));

  class TTest final
      : public Fiber::TRunnable {
    NO_COPY(TTest);
    public:

    TTest(Fiber::TRunner *runner,
          const std::function<void (Fiber::TRunner::TRunnerCons &, TFramePoolManager *)> &test,
          Fiber::TRunner::TRunnerCons &runner_cons,
          TFramePoolManager *frame_pool_manager,
          std::mutex &mut,
          std::condition_variable &cond,
          bool &fin)
        : Test(test), RunnerCons(runner_cons), FramePoolManager(frame_pool_manager), Mutex(mut), Cond(cond), Finished(fin) {
      Frame = Fiber::TFrame::LocalFramePool->Alloc();
      try {
        Frame->Latch(runner, this, static_cast<Fiber::TRunnable::TFunc>(&TTest::Run));
      } catch (...) {
        Fiber::TFrame::LocalFramePool->Free(Frame);
        throw;
      }
    }

    void Run() {
      Test(RunnerCons, FramePoolManager);
      std::lock_guard<std::mutex> lock(Mutex);
      Finished = true;
      Cond.notify_one();
      Fiber::FreeMyFrame(Fiber::TFrame::LocalFramePool);
    }

    private:

    const std::function<void (Fiber::TRunner::TRunnerCons &, TFramePoolManager *)> &Test;
    Fiber::TRunner::TRunnerCons &RunnerCons;
    TFramePoolManager *FramePoolManager;
    std::mutex &Mutex;
    std::condition_variable &Cond;
    bool &Finished;
    Fiber::TFrame *Frame;

  };  // TTest

  std::mutex mut;
  std::condition_variable cond;
  bool fin = false;
  TTest test_runnable(&runner, test, runner_cons, &frame_pool_manager, mut, cond, fin);
  /* extra */ {
    std::unique_lock<std::mutex> lock(mut);
    while (!fin) {
      cond.wait(lock);
    }
  }
  runner.ShutDown();
  t1.join();
  delete Fiber::TFrame::LocalFramePool;
  Fiber::TFrame::LocalFramePool = nullptr;
}

/* A runnable on its own runner, hosted on a scheduler job, as orlyi hosts its layer cleaners.
   The runnable must have returned before this is destroyed. */
class THostedFiber final
    : public Fiber::TRunnable {
  NO_COPY(THostedFiber);
  public:

  THostedFiber(TScheduler *scheduler, Fiber::TRunner::TRunnerCons &runner_cons, TFramePoolManager *frame_pool_manager,
               Fiber::TRunnable *runnable, Fiber::TRunnable::TFunc func)
      : Scheduler(scheduler), Runner(runner_cons), Runnable(runnable), Func(func) {
    Handle = scheduler->ScheduleCancelable([this, frame_pool_manager] {
      Fiber::LaunchSlowFiberSched(&Runner, frame_pool_manager);
      Exited.Push();
    });
    Frame = Fiber::TFrame::LocalFramePool->Alloc();
    Frame->Latch(&Runner, this, static_cast<Fiber::TRunnable::TFunc>(&THostedFiber::Run));
  }

  /* The caller has already told the runnable to stop. Cancel first, as ~TFileService does
     (#631): if the host never ran, neither did the latched frame, so free it here; otherwise the
     loop is running and will run the fiber, so wait for it to finish (it frees its own frame)
     before shutting the loop down. Shutting down first could leave the frame latched and never
     run, and the frame pool's destructor then terminates on it. */
  ~THostedFiber() {
    const bool hosted = !Scheduler->Cancel(Handle);
    if (hosted) {
      RunExited.Pop();
    }
    Runner.ShutDown();
    if (hosted) {
      Exited.Pop();
    } else {
      Fiber::TFrame::LocalFramePool->Free(Frame);
    }
  }

  private:

  void Run() {
    (Runnable->*Func)();
    RunExited.Push();
    Fiber::FreeMyFrame(Fiber::TFrame::LocalFramePool);
  }

  TScheduler *Scheduler;

  Fiber::TRunner Runner;

  Fiber::TRunnable *Runnable;

  Fiber::TRunnable::TFunc Func;

  Fiber::TFrame *Frame;

  TEventSemaphore RunExited;

  TScheduler::TJobHandle Handle;

  TEventSemaphore Exited;

};  // THostedFiber

/* The per-runner file caches that reading a disk layer needs (repo.test.cc). */
class TRunnerFileCaches {
  NO_COPY(TRunnerFileCaches);
  public:

  using TLocalReadFileCache = Disk::TLocalReadFileCache<Disk::Util::LogicalPageSize,
                                                        Disk::Util::LogicalBlockSize,
                                                        Disk::Util::PhysicalBlockSize,
                                                        Disk::Util::CheckedPage, true>;

  TRunnerFileCaches() {
    TLocalReadFileCache::Cache = new TLocalReadFileCache();
    Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
  }

  ~TRunnerFileCaches() {
    delete TLocalReadFileCache::Cache;
    TLocalReadFileCache::Cache = nullptr;
    delete Disk::TLocalWalkerCache::Cache;
    Disk::TLocalWalkerCache::Cache = nullptr;
  }

};  // TRunnerFileCaches

/* A safe repo whose merges the test steps by hand. Nothing else runs them: the merge loops
   are started by the server, not by the manager. */
class TSteppedSafeRepo final
    : public TSafeRepo {
  NO_COPY(TSteppedSafeRepo);
  public:

  using TSafeRepo::TSafeRepo;

  using Orly::Indy::TRepo::StepMergeMem;

};  // TSteppedSafeRepo

/* A minimal L1::TManager (repo.test.cc) that creates stepped safe repos and reloads one from
   disk the way orlyi reloads its system repo (TSafeRepo::ReConstructFromDisk). */
class TFaultRepoManager
    : public L1::TManager {
  NO_COPY(TFaultRepoManager);
  public:

  TFaultRepoManager(Disk::Util::TEngine *engine, Base::TScheduler *scheduler)
      : TManager(engine, 1h, 1h, true, true, true, 1000ms, scheduler,
                 100UL, 100UL, 20UL, MemMergeCoreVec, DiskMergeCoreVec, true) {}

  /* The sweeps Indy::TManager runs in its destructor. They delete the layers merges retired,
     which removes their files. */
  virtual ~TFaultRepoManager() {
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

  virtual TRepo *ReconstructRepo(const Base::TUuid &repo_id) override {
    return TSafeRepo::ReConstructFromDisk(this, repo_id, L0::TDeadline::clock::now() + std::chrono::seconds(1000));
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
  virtual void RunReplicationQueue() override {}
  virtual void RunReplicationWork() override {}
  virtual void RunReplicateTransaction() override {}
  virtual std::mutex &GetReplicationQueueLock() NO_THROW override { return ReplicationQueueLock; }

  TManager::TPtr<TRepo> Create(const Base::TUuid &repo_id) {
    return OpenOrCreate(repo_id, TTtl::max(), std::nullopt, true);
  }

  TManager::TPtr<TRepo> Reopen(const Base::TUuid &repo_id) {
    return ForceOpenRepo(repo_id);
  }

  private:

  std::mutex ReplicationQueueLock;

};  // TFaultRepoManager

/* The durable manager hands every save to the indy manager for replication; stub it out
   (durable_manager.test.cc). */
class TReplicationStub final
    : public Orly::Indy::DurableManager::TManager {
  public:

  virtual Orly::Indy::TDurableReplication *NewDurableReplication(const Orly::Durable::TId &, const Orly::Durable::TTtl &, const std::string &) const override {
    return nullptr;
  }

  virtual void DeleteDurableReplication(Orly::Indy::TDurableReplication *) NO_THROW override {}

  virtual void EnqueueDurable(Orly::Indy::TDurableReplication *) NO_THROW override {}

};  // TReplicationStub

/* What a case runs with, in a child. */
struct TCaseEnv {
  TScheduler *Scheduler;
  Fiber::TRunner::TRunnerCons *RunnerCons;
  TFramePoolManager *FramePoolManager;
  TFaultPlan *Plan;
  const TRecord *Record;
  unique_ptr<TFaultEngine> Engine;
  /* Running over the image a power loss left. */
  bool Reopened = false;
  /* Lose power now: save the durable image and end the child, as a power loss at a Sync does.
     Set in the operation's child only. */
  std::function<void ()> Crash;
};

/* An operation under test. A fresh object runs in each child. */
class TCase {
  NO_COPY(TCase);
  public:

  virtual ~TCase() {}

  virtual TFaultEngine::TLayout GetLayout() const {
    return TFaultEngine::TLayout();
  }

  virtual Disk::TFileService::TFileInitCb GetFileInitCb() const {
    return nullptr;
  }

  /* Build the state the operation starts from, durably. Record what a reopen must find. */
  virtual void Setup(TCaseEnv &env) = 0;

  /* The operation; the plan counts I/O only during this. */
  virtual void Op(TCaseEnv &env) = 0;

  /* Undo Setup's objects, leaving the engine. */
  virtual void Close(TCaseEnv &env) = 0;

  /* After a reopen: "" if every acknowledged write reads back exactly, or what did not. */
  virtual string Verify(TCaseEnv &env, const map<string, string> &record) = 0;

  /* Blocks the allocator holds beyond what the file map accounts for. */
  virtual long GetLeak(TCaseEnv &env) {
    return static_cast<long>(env.Engine->GetLiveBlocks()) - static_cast<long>(env.Engine->CountReferencedBlocks());
  }

  protected:

  TCase() {}

};  // TCase

/*** Repo cases ***/

static const Base::TUuid FaultRepoId("8D7E5A2C-6B0F-4C35-9E21-0A6B3C8F1D47");
static const Base::TUuid FaultIdxId("1F3C9B6E-2D84-4A7B-B5E0-7C9D2E4A6F18");

/* A root safe repo on a fault engine: create it, or reload it. */
class TRepoFixture {
  NO_COPY(TRepoFixture);
  public:

  TRepoFixture(Disk::Util::TEngine *engine, TScheduler *scheduler, bool create)
      : StateBuf(Sabot::State::GetMaxStateSize()), State(StateBuf.data()) {
    Manager = make_unique<TFaultRepoManager>(engine, scheduler);
    Repo = create ? Manager->Create(FaultRepoId) : Manager->Reopen(FaultRepoId);
    Stepped = dynamic_cast<TSteppedSafeRepo *>(Repo.Get());
  }

  ~TRepoFixture() {
    Close();
  }

  /* Drop the repo and the manager. The manager's teardown deletes the layers merges retired,
     which removes their files. */
  void Close() {
    Repo.Reset();
    Manager.reset();
  }

  TIndexKey IndexKey(int64_t key) {
    return TIndexKey(FaultIdxId, TKey(make_tuple(key), &Arena, State));
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
  }

  string Read(int64_t key) {
    try {
      TSuprena ctx_arena;
      TContext context(Repo, &ctx_arena);
      ostringstream strm;
      strm << context[IndexKey(key)];
      return strm.str();
    } catch (const exception &ex) {
      return string("<throws: ") + ex.what() + ">";
    }
  }

  string Int(int64_t val) {
    ostringstream strm;
    strm << TKey(val, &Arena, State);
    return strm.str();
  }

  TSteppedSafeRepo *GetStepped() const {
    return Stepped;
  }

  private:

  vector<uint8_t> StateBuf;

  void *State;

  TSuprena Arena;

  unique_ptr<TFaultRepoManager> Manager;

  L0::TManager::TPtr<L0::TManager::TRepo> Repo;

  TSteppedSafeRepo *Stepped = nullptr;

};  // TRepoFixture

/* Shared by the repo cases: the keys a reopen reads, and the states it may find. */
class TRepoCase
    : public TCase {
  NO_COPY(TRepoCase);
  public:

  virtual void Close(TCaseEnv &) override {
    if (Fixture) {
      Fixture->Close();
    }
  }

  virtual string Verify(TCaseEnv &env, const map<string, string> &record) override {
    env.Record->Put("files_at_open", DescribeFiles(env));
    Fixture = make_unique<TRepoFixture>(env.Engine->GetEngine(), env.Scheduler, false);
    env.Record->Put("files_after_reload", DescribeFiles(env));
    /* This object did not run Setup, so the keys come from the states it recorded. */
    set<int64_t> keys;
    for (const string &state : GetStates(record)) {
      const string prefix = state + ".";
      for (auto pos = record.lower_bound(prefix); pos != record.end() && pos->first.compare(0, prefix.size(), prefix) == 0; ++pos) {
        keys.insert(stoll(pos->first.substr(prefix.size())));
      }
    }
    if (keys.empty()) {
      return "no keys recorded";
    }
    for (int64_t key : keys) {
      env.Record->Put("read." + to_string(key), Fixture->Read(key));
    }
    /* Every key must read as in one of the acceptable states, the same one for all keys. */
    string why;
    for (const string &state : GetStates(record)) {
      bool all = true;
      ostringstream diff;
      for (int64_t key : keys) {
        const auto pos = record.find(state + "." + to_string(key));
        const string want = (pos == record.end()) ? "?" : pos->second;
        const string got = Fixture->Read(key);
        if (got != want) {
          if (all) {
            diff << "vs " << state << ":";
          }
          all = false;
          diff << " key " << key << " read " << got << " want " << want << ";";
        }
      }
      if (all) {
        return "";
      }
      why += diff.str();
    }
    return why;
  }

  protected:

  TRepoCase() {}

  /* The repo's data files in the file map: gen[lowest-highest]. */
  static string DescribeFiles(TCaseEnv &env) {
    std::vector<Disk::TFileObj> files;
    env.Engine->GetEngine()->AppendFileGenSet(FaultRepoId, files);
    std::sort(files.begin(), files.end(), [](const Disk::TFileObj &lhs, const Disk::TFileObj &rhs) {
      return lhs.GenId < rhs.GenId;
    });
    ostringstream strm;
    for (const auto &file : files) {
      strm << file.GenId << "[" << file.LowestSeq << "-" << file.HighestSeq << "] ";
    }
    return strm.str();
  }

  /* The states a reopen may find, by record prefix. */
  virtual vector<string> GetStates(const map<string, string> &record) const = 0;

  void RecordState(TCaseEnv &env, const string &state, const map<int64_t, int64_t> &vals) {
    for (const auto &[key, val] : vals) {
      env.Record->Put(state + "." + to_string(key), Fixture->Int(val));
    }
  }

  unique_ptr<TRepoFixture> Fixture;

};  // TRepoCase

/* One StepMergeMem: a memory layer of overwrites and `+=` flushed to a second file. A reopen
   finds the first file alone, or both. */
class TMergeMemCase final
    : public TRepoCase {
  public:

  virtual void Setup(TCaseEnv &env) override {
    Fixture = make_unique<TRepoFixture>(env.Engine->GetEngine(), env.Scheduler, true);
    map<int64_t, int64_t> pre, post;
    for (int64_t key = 0L; key < 20L; ++key) {
      Fixture->Commit({{key, 100L + key, TMutator::Assign}});
      pre[key] = 100L + key;
    }
    Fixture->Commit({{Counter, 10L, TMutator::Assign}});
    pre[Counter] = 10L;
    Fixture->GetStepped()->StepMergeMem();
    post = pre;
    for (int64_t key = 0L; key < 20L; key += 2L) {
      Fixture->Commit({{key, 200L + key, TMutator::Assign}});
      post[key] = 200L + key;
    }
    for (int64_t i = 0L; i < 3L; ++i) {
      Fixture->Commit({{Counter, 1L, TMutator::Add}});
    }
    post[Counter] = 13L;
    RecordState(env, "pre", pre);
    RecordState(env, "post", post);
  }

  virtual void Op(TCaseEnv &) override {
    Fixture->GetStepped()->StepMergeMem();
  }

  private:

  virtual vector<string> GetStates(const map<string, string> &) const override {
    return {"pre", "post"};
  }

  static constexpr int64_t Counter = -1L;

};  // TMergeMemCase

/* One StepMergeDisk of two files, then the repo's teardown, which removes the inputs. A merge
   changes no value, so a reopen at any point finds the values written. */
class TMergeDiskCase final
    : public TRepoCase {
  public:

  /* Where the operation ends. */
  enum class TEnd {
    /* The repo's teardown, which removes the merge's inputs. */
    Teardown,
    /* Power is lost once the merge's output is in the file map, before its inputs are
       removed. */
    CrashBeforeRemoval,
    /* As CrashBeforeRemoval, with the output's file-map entry recording only the range of the
       updates the fold kept, as a fold output did before #618. */
    CrashBeforeRemovalOldRange
  };

  /* fold: the files hold only `+=` deltas, so MergeFiles takes the fold path. */
  explicit TMergeDiskCase(bool fold, TEnd end = TEnd::Teardown)
      : Fold(fold), End(end) {}

  virtual void Setup(TCaseEnv &env) override {
    Fixture = make_unique<TRepoFixture>(env.Engine->GetEngine(), env.Scheduler, true);
    map<int64_t, int64_t> vals;
    if (Fold) {
      /* As RootMergeFoldsChainOntoBaseOutsideMerge (repo.test.cc): Assign bases in a big file,
         then two small files of `+= 1`s, which StepMergeDisk merges on their own. Counter A's
         last delta is in the second small file and counter B's in the first, so the fold's
         output spans both files' ranges without covering the first file's lowest update. */
      Fixture->Commit({{CounterA, 10L, TMutator::Assign}, {CounterB, 10L, TMutator::Assign}});
      for (int64_t key = 0L; key < 100L; ++key) {
        Fixture->Commit({{key, key, TMutator::Assign}});
        vals[key] = key;
      }
      Fixture->GetStepped()->StepMergeMem();
      Fixture->Commit({{CounterA, 1L, TMutator::Add}});
      Fixture->Commit({{CounterB, 1L, TMutator::Add}});
      Fixture->GetStepped()->StepMergeMem();
      Fixture->Commit({{CounterA, 1L, TMutator::Add}});
      Fixture->Commit({{CounterA, 1L, TMutator::Add}});
      Fixture->GetStepped()->StepMergeMem();
      vals[CounterA] = 13L;
      vals[CounterB] = 11L;
    } else {
      for (int64_t round = 1L; round <= 2L; ++round) {
        for (int64_t key = 0L; key < 20L; ++key) {
          Fixture->Commit({{key, round * 100L + key, TMutator::Assign}});
          vals[key] = round * 100L + key;
        }
        Fixture->GetStepped()->StepMergeMem();
      }
    }
    RecordState(env, "want", vals);
  }

  virtual void Op(TCaseEnv &env) override {
    Fixture->GetStepped()->StepMergeDisk(256UL);
    if (End == TEnd::Teardown) {
      Fixture->Close();
      return;
    }
    env.Record->Put("files_before_crash", DescribeFiles(env));
    if (End == TEnd::CrashBeforeRemovalOldRange) {
      RecordOldRange(env);
      env.Record->Put("files_with_old_range", DescribeFiles(env));
    }
    env.Crash();
  }

  private:

  virtual vector<string> GetStates(const map<string, string> &) const override {
    return {"want"};
  }

  /* Rewrite the merge output's file-map entry to the range a fold output recorded before #618:
     only the updates it kept. Here those are counter B's last delta, the first input's last
     update, and counter A's, the second input's last update. The file itself is unchanged. */
  static void RecordOldRange(TCaseEnv &env) {
    Disk::Util::TEngine *engine = env.Engine->GetEngine();
    std::vector<Disk::TFileObj> files;
    engine->AppendFileGenSet(FaultRepoId, files);
    std::sort(files.begin(), files.end(), [](const Disk::TFileObj &lhs, const Disk::TFileObj &rhs) {
      return lhs.GenId < rhs.GenId;
    });
    /* The base file, the two inputs and the output. */
    if (files.size() != 4UL) {
      throw logic_error("expected 4 files before the inputs' removal, found " + to_string(files.size()));
    }
    const Disk::TFileObj &first = files[1], &second = files[2], &output = files[3];
    size_t block_id, block_offset, file_size, num_keys;
    if (!engine->FindFile(FaultRepoId, output.GenId, block_id, block_offset, file_size, num_keys)) {
      throw logic_error("merge output not in the file map");
    }
    /* removal */ {
      Disk::TCompletionTrigger trigger;
      engine->RemoveFile(FaultRepoId, output.GenId, trigger);
      trigger.Wait();
    }
    /* insertion */ {
      Disk::TCompletionTrigger trigger;
      engine->InsertFile(FaultRepoId, Disk::TFileObj::TKind::DataFile, output.GenId, block_id, block_offset, file_size, num_keys,
                         first.HighestSeq, second.HighestSeq, trigger);
      trigger.Wait();
    }
  }

  static constexpr int64_t CounterA = -1L, CounterB = -2L;

  const bool Fold;

  const TEnd End;

};  // TMergeDiskCase

/*** Durable case ***/

/* Two saves flushed to two files, then a third: the writer flushes it and the merger merges the
   three (it merges three files of a generation), and the layer cleaner removes the inputs. A
   reopen must load every save that was acknowledged (its sem fired). */
class TDurableSaveMergeCase final
    : public TCase {
  public:

  virtual void Setup(TCaseEnv &env) override {
    Open(env, true);
    for (size_t i = 0UL; i < 2UL; ++i) {
      Save(env, i);
    }
  }

  virtual void Op(TCaseEnv &env) override {
    Save(env, 2UL);
    /* Wait for the merge and the removal of its inputs: one durable file left. */
    const auto give_up = std::chrono::steady_clock::now() + 20s;
    for (;;) {
      std::vector<Disk::TFileObj> files;
      env.Engine->GetEngine()->AppendFileGenSet(Disk::TDurableManager::DurableByIdFileId, files);
      if (files.size() == 1UL) {
        break;
      }
      /* A writer or merger that hit an I/O error stops writing (#621): the merge won't come. */
      if (Manager->HasFailed()) {
        throw std::runtime_error("durable manager stopped writing after an I/O error; files=" + to_string(files.size()));
      }
      if (std::chrono::steady_clock::now() > give_up) {
        throw std::runtime_error("durable merge did not finish; files=" + to_string(files.size()));
      }
      std::this_thread::sleep_for(10ms);
      Fiber::YieldSlow();
    }
  }

  virtual void Close(TCaseEnv &) override {
    if (Manager) {
      Manager->StopLayerCleaner();
      Manager->JoinLayerCleaner();
      Cleaner.reset();
      Manager.reset();
    }
  }

  virtual string Verify(TCaseEnv &env, const map<string, string> &record) override {
    Open(env, false);
    ostringstream why;
    for (size_t i = 0UL; i < NumSaves; ++i) {
      const auto pos = record.find("acked." + to_string(i));
      if (pos == record.end()) {
        continue;
      }
      string loaded;
      if (!Manager->TryLoad(GetId(i), loaded)) {
        why << " save " << i << " (acked) not found;";
      } else if (loaded != GetBlob(i)) {
        why << " save " << i << " (acked) read [" << loaded << "];";
      }
    }
    return why.str();
  }

  private:

  static constexpr size_t NumSaves = 3UL;

  static Orly::Durable::TId GetId(size_t i) {
    static const char *ids[NumSaves] = {"5B1D7E3A-0C49-4F62-8A1B-9E2C4D6F8A01", "5B1D7E3A-0C49-4F62-8A1B-9E2C4D6F8A02",
                                        "5B1D7E3A-0C49-4F62-8A1B-9E2C4D6F8A03"};
    return Orly::Durable::TId(ids[i]);
  }

  static string GetBlob(size_t i) {
    return "durable object " + to_string(i);
  }

  void Open(TCaseEnv &env, bool create) {
    Manager = make_unique<Disk::TDurableManager>(env.Scheduler, *env.RunnerCons, env.FramePoolManager, &RepStub, env.Engine->GetEngine(),
                                                 100UL, 0ms /* write delay */, 0ms /* merge delay */, 20ms /* layer cleaning */,
                                                 20UL, create);
    Cleaner = make_unique<THostedFiber>(env.Scheduler, *env.RunnerCons, env.FramePoolManager, Manager.get(),
                                        static_cast<Fiber::TRunnable::TFunc>(&Orly::Durable::TManager::RunLayerCleaner));
  }

  void Save(TCaseEnv &env, size_t i) {
    const Orly::Durable::TTtl ttl(600);
    Orly::Durable::TSem sem;
    Manager->Save(GetId(i), Orly::Durable::TDeadline::clock::now() + ttl, ttl, GetBlob(i), &sem);
    sem.Pop();
    env.Record->Put("acked." + to_string(i), "1");
  }

  TReplicationStub RepStub;

  unique_ptr<Disk::TDurableManager> Manager;

  unique_ptr<THostedFiber> Cleaner;

};  // TDurableSaveMergeCase

/*** File service case ***/

/* File-map changes on a one-block append log (128 sectors, one per change here): the setup
   fills it, so the first change of the operation goes into a base image, and the next two into
   the log after it. The files have no blocks; the startup walk is skipped. A reopen must find
   every acknowledged change, and may find the one in flight. */
class TBaseImageCase final
    : public TCase {
  public:

  virtual TFaultEngine::TLayout GetLayout() const override {
    TFaultEngine::TLayout layout;
    layout.AppendLogBlocks = 1UL;
    return layout;
  }

  virtual Disk::TFileService::TFileInitCb GetFileInitCb() const override {
    return [](Disk::TFileObj::TKind, const Base::TUuid &, size_t, size_t, size_t, size_t) {
      return true;
    };
  }

  virtual void Setup(TCaseEnv &env) override {
    for (size_t gen = 1UL; gen <= 128UL; ++gen) {
      Insert(env, gen);
    }
  }

  virtual void Op(TCaseEnv &env) override {
    Insert(env, 129UL);
    Insert(env, 130UL);
    Remove(env, 1UL);
  }

  virtual void Close(TCaseEnv &) override {}

  virtual string Verify(TCaseEnv &env, const map<string, string> &record) override {
    std::vector<Disk::TFileObj> files;
    env.Engine->GetEngine()->AppendFileGenSet(FileId, files);
    set<size_t> found;
    for (const auto &file : files) {
      found.insert(file.GenId);
    }
    const auto inflight = record.find("inflight");
    ostringstream why;
    for (size_t gen = 1UL; gen <= 130UL; ++gen) {
      const auto pos = record.find("acked." + to_string(gen));
      /* The change in flight when the power went may or may not have landed. */
      const bool in_flight = inflight != record.end() && inflight->second == to_string(gen);
      const bool may_differ = in_flight && (pos == record.end() || pos->second != record.at("inflight_kind"));
      if (pos == record.end() && !may_differ) {
        /* never acked and not in flight: must be absent (gen > 128 not inserted yet) */
        if (found.count(gen)) {
          why << " gen " << gen << " present, never inserted;";
        }
        continue;
      }
      if (may_differ) {
        continue;
      }
      const bool want = (pos->second == "insert");
      if (found.count(gen) != (want ? 1UL : 0UL)) {
        why << " gen " << gen << (want ? " (inserted, acked) missing;" : " (removed, acked) present;");
      }
    }
    return why.str();
  }

  /* The files have no blocks, so the system block, the two base-image blocks and the append
     log are all there is. Writing a base image first gives it two spare blocks, which are free
     again after a restart (file_service.cc). */
  virtual long GetLeak(TCaseEnv &env) override {
    const long live = static_cast<long>(env.Engine->GetLiveBlocks());
    return (env.Reopened || live < 6L) ? live - 4L : live - 6L;
  }

  private:

  static inline const Base::TUuid FileId{"3E8A1C5F-7B2D-4096-A3E4-6F1B8D0C2E95"};

  void Insert(TCaseEnv &env, size_t gen) {
    env.Record->Put("inflight_kind", "insert");
    env.Record->Put("inflight", to_string(gen));
    Disk::TCompletionTrigger trigger;
    env.Engine->GetEngine()->InsertFile(FileId, Disk::TFileObj::TKind::DataFile, gen, 1000UL + gen, 0UL, 1UL, 1UL, gen, gen, trigger);
    trigger.Wait();
    env.Record->Put("acked." + to_string(gen), "insert");
  }

  void Remove(TCaseEnv &env, size_t gen) {
    env.Record->Put("inflight_kind", "remove");
    env.Record->Put("inflight", to_string(gen));
    Disk::TCompletionTrigger trigger;
    env.Engine->GetEngine()->RemoveFile(FileId, gen, trigger);
    trigger.Wait();
    env.Record->Put("acked." + to_string(gen), "remove");
  }

};  // TBaseImageCase

static unique_ptr<TCase> NewCase(const string &name) {
  if (name == "MergeMem") {
    return make_unique<TMergeMemCase>();
  }
  if (name == "MergeDisk") {
    return make_unique<TMergeDiskCase>(false);
  }
  if (name == "MergeDiskFold") {
    return make_unique<TMergeDiskCase>(true);
  }
  /* Not swept: FoldMergeCrashBeforeInputRemoval runs these once. */
  if (name == "MergeDiskFoldCrash") {
    return make_unique<TMergeDiskCase>(true, TMergeDiskCase::TEnd::CrashBeforeRemoval);
  }
  if (name == "MergeDiskFoldCrashOldRange") {
    return make_unique<TMergeDiskCase>(true, TMergeDiskCase::TEnd::CrashBeforeRemovalOldRange);
  }
  if (name == "DurableSaveMerge") {
    return make_unique<TDurableSaveMergeCase>();
  }
  if (name == "BaseImage") {
    return make_unique<TBaseImageCase>();
  }
  throw logic_error("no such case: " + name);
}

static const vector<string> CaseNames{"MergeMem", "MergeDisk", "MergeDiskFold", "DurableSaveMerge", "BaseImage"};

/*** Modes ***/

struct TMode {
  const char *Name;
  unsigned Kinds;
  bool PowerLoss;
  double Tear;
};

static const vector<TMode> Modes{
  {"Write", TFaultPlan::Write, false, 0.0},
  {"Read", TFaultPlan::Read | TFaultPlan::ReadV, false, 0.0},
  {"Sync", TFaultPlan::Sync, false, 0.0},
  {"Power", TFaultPlan::Sync, true, 0.0},
  {"PowerTorn", TFaultPlan::Sync, true, 0.5}
};

static constexpr int PowerLossExit = 42;

/* A run takes well under a second; one still going after this is hung. */
static constexpr std::chrono::seconds ChildDeadline{6};

static const TMode &FindMode(const string &name) {
  for (const auto &mode : Modes) {
    if (name == mode.Name) {
      return mode;
    }
  }
  throw logic_error("no such mode: " + name);
}

/*** The children ***/

static void InitChild(const string &log_path) {
  const int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
  }
  /* No core dump. These children abort by design, and nothing reads their cores. A dump is not
     free: GitHub's runners pipe it to systemd-coredump, and the child isn't reaped until the
     dump is written. On an x86 runner that took 1.5 s per abort idle and over 5 s under load,
     past ChildDeadline, so a deliberate abort was reported as a hang (#682). */
  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  /* Engine errors go to syslog; copy them to the log. */
  openlog("fault_injection", LOG_PERROR, LOG_USER);
  setlogmask(LOG_UPTO(LOG_DEBUG));
  if (!Disk::Util::TDiskController::TEvent::DiskEventPoolManager) {
    Disk::Util::TDiskController::TEvent::InitializeDiskEventPoolManager(64UL);
  }
}

/* Child A: set up, run the operation under the plan, check for leaks, tear down. */
[[noreturn]] static void RunOpChild(const string &case_name, const TMode &mode, size_t n, const string &dir) {
  InitChild(dir + "/op.log");
  const TRecord record(dir + "/record");
  const string image_path = dir + "/image";
  RunOnFiber([&](Fiber::TRunner::TRunnerCons &runner_cons, TFramePoolManager *frame_pool_manager) {
    TRunnerFileCaches file_caches;
    TScheduler scheduler(TScheduler::TPolicy(4, 16, 10s));
    TFaultPlan plan;
    auto test_case = NewCase(case_name);
    TCaseEnv env{&scheduler, &runner_cons, frame_pool_manager, &plan, &record, nullptr};
    env.Crash = [&]() {
      record.Put("power_loss", to_string(n));
      try {
        env.Engine->GetDurableImage().Save(image_path);
      } catch (const exception &ex) {
        record.Put("image", ex.what());
        _exit(1);
      }
      _exit(PowerLossExit);
    };
    if (mode.PowerLoss) {
      plan.PowerLossAtSync(n, mode.Tear, n * 7919UL + 13UL, env.Crash);
    } else {
      plan.FailNth(n, mode.Kinds, TOnAbortOnError::Report);
    }
    plan.SetOnInject([&record](const TFaultPlan::TInjected &injected) {
      ostringstream strm;
      strm << TFaultPlan::GetKindName(injected.Kind) << "#" << injected.N << " abort_on_error=" << injected.AbortOnError << " at " << injected.Where;
      record.Put("injected", strm.str());
    });
    record.Put("phase", "setup");
    env.Engine = make_unique<TFaultEngine>(&scheduler, runner_cons, frame_pool_manager, &plan, test_case->GetLayout(), nullptr, test_case->GetFileInitCb());
    test_case->Setup(env);
    record.Put("phase", "op");
    plan.Arm();
    try {
      test_case->Op(env);
      record.Put("op", "ok");
    } catch (const exception &ex) {
      record.Put("op", string("threw: ") + ex.what());
    }
    plan.Disarm();
    record.Put("reached", plan.GetInjected() ? "1" : "0");
    record.Put("count", to_string(plan.GetCount(mode.Kinds)));
    record.Put("phase", "close");
    test_case->Close(env);
    record.Put("phase", "leak");
    record.Put("leak", to_string(test_case->GetLeak(env)));
    record.Put("phase", "teardown");
    env.Engine.reset();
    record.Put("phase", "done");
  });
  fflush(nullptr);
  _exit(0);
}

/* Child B: open a new engine over the image a power loss left, and check it. */
[[noreturn]] static void RunReopenChild(const string &case_name, const string &dir) {
  InitChild(dir + "/reopen.log");
  const TRecord record(dir + "/reopen");
  const auto op_record = TRecord::Read(dir + "/record");
  RunOnFiber([&](Fiber::TRunner::TRunnerCons &runner_cons, TFramePoolManager *frame_pool_manager) {
    TRunnerFileCaches file_caches;
    TScheduler scheduler(TScheduler::TPolicy(4, 16, 10s));
    TFaultPlan plan;
    auto test_case = NewCase(case_name);
    TCaseEnv env{&scheduler, &runner_cons, frame_pool_manager, &plan, &record, nullptr, true};
    record.Put("phase", "open");
    try {
      const TFaultImage image = TFaultImage::Load(dir + "/image");
      env.Engine = make_unique<TFaultEngine>(&scheduler, runner_cons, frame_pool_manager, &plan, test_case->GetLayout(), &image, test_case->GetFileInitCb());
      record.Put("phase", "verify");
      record.Put("verify", test_case->Verify(env, op_record));
      record.Put("phase", "leak");
      record.Put("leak", to_string(test_case->GetLeak(env)));
      record.Put("phase", "close");
      test_case->Close(env);
    } catch (const exception &ex) {
      record.Put("threw", ex.what());
    }
    record.Put("phase", "teardown");
    env.Engine.reset();
    record.Put("phase", "done");
  });
  fflush(nullptr);
  _exit(0);
}

/*** The parent ***/

/* The State and CoreDumping lines of a live child's /proc status, so a run killed at the
   deadline says whether it was stuck, or dying slowly. */
static string DescribeProcStatus(pid_t pid) {
  ifstream strm("/proc/" + to_string(pid) + "/status");
  string line, out;
  while (getline(strm, line)) {
    if (line.rfind("State:", 0) == 0 || line.rfind("CoreDumping:", 0) == 0) {
      for (auto &c : line) {
        if (c == '\t') {
          c = ' ';
        }
      }
      out += (out.empty() ? "" : ", ") + line;
    }
  }
  return out;
}

/* Waits for a child until the deadline, then kills it. The exit status, or nullopt on a hang,
   with the child's state just before the kill in *at_kill. */
static optional<int> WaitChild(pid_t pid, std::chrono::seconds deadline, string *at_kill) {
  const auto give_up = std::chrono::steady_clock::now() + deadline;
  for (;;) {
    int status = 0;
    const pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
      return status;
    }
    if (std::chrono::steady_clock::now() > give_up) {
      *at_kill = DescribeProcStatus(pid);
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return nullopt;
    }
    std::this_thread::sleep_for(5ms);
  }
}

static string DescribeStatus(const optional<int> &status, const map<string, string> &record, const string &at_kill = "") {
  const auto phase = record.count("phase") ? record.at("phase") : "?";
  if (!status) {
    return "hang in " + phase + (at_kill.empty() ? "" : "; " + at_kill);
  }
  if (WIFSIGNALED(*status)) {
    return string(WTERMSIG(*status) == SIGABRT ? "abort" : strsignal(WTERMSIG(*status))) + " in " + phase;
  }
  return "exit " + to_string(WEXITSTATUS(*status)) + " in " + phase;
}

/* One (case, mode, N) run and what came of it. */
struct TRun {
  string Case;
  string Mode;
  size_t N = 0UL;

  /* The fault fired. */
  bool Reached = false;

  /* "ok", or a failure category: "abort", "hang", "crash", "leak", "lost", "reopen". */
  string Outcome;

  string Detail;

  /* What the faulted I/O was, from the child. */
  string Injected;

  bool IsFailure() const {
    return Outcome != "ok" && Outcome != "abort-after-io-error" && Outcome.rfind("unreachable:", 0) != 0;
  }

};  // TRun

static TRun RunOne(const string &case_name, const TMode &mode, size_t n, const string &dir) {
  TRun run{case_name, mode.Name, n};
  mkdir(dir.c_str(), 0755);
  for (const char *file : {"record", "reopen", "image", "op.log", "reopen.log"}) {
    unlink((dir + "/" + file).c_str());
  }
  const pid_t op_pid = fork();
  if (op_pid == 0) {
    RunOpChild(case_name, mode, n, dir);
  }
  const auto op_start = std::chrono::steady_clock::now();
  string op_at_kill;
  const auto op_status = WaitChild(op_pid, ChildDeadline, &op_at_kill);
  const auto op_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - op_start).count();
  const auto record = TRecord::Read(dir + "/record");
  run.Injected = record.count("injected") ? record.at("injected") : "";
  if (mode.PowerLoss) {
    if (op_status && WIFEXITED(*op_status) && WEXITSTATUS(*op_status) == PowerLossExit) {
      run.Reached = true;
      const pid_t reopen_pid = fork();
      if (reopen_pid == 0) {
        RunReopenChild(case_name, dir);
      }
      string reopen_at_kill;
      const auto reopen_status = WaitChild(reopen_pid, ChildDeadline, &reopen_at_kill);
      const auto reopen = TRecord::Read(dir + "/reopen");
      if (!reopen_status || !WIFEXITED(*reopen_status) || WEXITSTATUS(*reopen_status) != 0) {
        run.Outcome = "reopen";
        run.Detail = DescribeStatus(reopen_status, reopen, reopen_at_kill);
      } else if (reopen.count("threw")) {
        run.Outcome = "reopen";
        run.Detail = "threw: " + reopen.at("threw");
      } else if (reopen.count("verify") && !reopen.at("verify").empty()) {
        run.Outcome = "lost";
        run.Detail = reopen.at("verify");
      } else if (!reopen.count("leak") || reopen.at("leak") != "0") {
        run.Outcome = "leak";
        run.Detail = "after reopen: " + (reopen.count("leak") ? reopen.at("leak") : string("?")) + " blocks";
      } else {
        run.Outcome = "ok";
      }
      return run;
    }
    /* The power stayed on: either the operation never reached sync N, or it failed first. */
    run.Reached = false;
  } else {
    run.Reached = record.count("reached") ? record.at("reached") == "1" : !run.Injected.empty();
  }
  if (!op_status) {
    run.Outcome = "hang";
    run.Detail = DescribeStatus(op_status, record, op_at_kill);
    run.Reached = run.Reached || !run.Injected.empty();
  } else if (WIFSIGNALED(*op_status)) {
    run.Reached = run.Reached || !run.Injected.empty();
    run.Outcome = (WTERMSIG(*op_status) == SIGABRT) ? (run.Injected.empty() ? "abort" : "abort-after-io-error") : "crash";
    run.Detail = DescribeStatus(op_status, record) + ", " + to_string(op_ms) + "ms";
  } else if (WEXITSTATUS(*op_status) != 0 || !record.count("phase") || record.at("phase") != "done") {
    run.Outcome = "crash";
    run.Detail = DescribeStatus(op_status, record);
  } else if (record.count("leak") && record.at("leak") != "0") {
    run.Outcome = "leak";
    run.Detail = record.at("leak") + " blocks after op " + record.at("op");
  } else if (!run.Reached && record.count("op") && record.at("op") != "ok") {
    /* No fault fired, yet the operation failed. */
    run.Outcome = "crash";
    run.Detail = "op " + record.at("op");
  } else {
    run.Outcome = "ok";
    run.Detail = record.count("op") ? "op " + record.at("op") : "";
  }
  /* A Read or Write whose caller passed abort_on_error: in production the disk controller
     aborts right there, so whatever the error went on to do cannot happen today. */
  if (run.Outcome != "ok" && run.Injected.find("abort_on_error=1") != string::npos) {
    run.Outcome = "unreachable:" + run.Outcome;
  }
  return run;
}

/* A run that is known to fail, with the issue that tracks it. A run whose case, mode and
   outcome match is an expected failure; an entry that no run matches is an unexpected pass. */
struct TExpectedFailure {
  const char *Case;
  const char *Mode;
  const char *Outcome;
  int Issue;
};

static const vector<TExpectedFailure> ExpectedFailures{
};

static bool IsExpected(const TRun &run) {
  for (const auto &xfail : ExpectedFailures) {
    if (run.Case == xfail.Case && run.Mode == xfail.Mode && run.Outcome == xfail.Outcome) {
      return true;
    }
  }
  return false;
}

/* Runs N = 1, 2, ... for one case and mode until the operation completes without reaching the
   fault, a few runs at a time. */
static vector<TRun> Sweep(const string &case_name, const TMode &mode, const string &root, size_t max_n) {
  const size_t parallel = std::clamp<size_t>(std::thread::hardware_concurrency() / 2U, 1UL, 4UL);
  vector<TRun> runs;
  for (size_t first = 1UL; first <= max_n; first += parallel) {
    /* Each run in its own process: fork one supervisor per N, each of which forks its children
       in turn and writes its TRun back. */
    vector<pair<size_t, pid_t>> pids;
    for (size_t n = first; n < first + parallel && n <= max_n; ++n) {
      const string dir = root + "/" + case_name + "." + mode.Name + "." + to_string(n);
      const pid_t pid = fork();
      if (pid == 0) {
        const TRun run = RunOne(case_name, mode, n, dir);
        const TRecord out(dir + "/run");
        out.Put("reached", run.Reached ? "1" : "0");
        out.Put("outcome", run.Outcome);
        out.Put("detail", run.Detail);
        out.Put("injected", run.Injected);
        _exit(0);
      }
      pids.emplace_back(n, pid);
    }
    bool done = false;
    for (const auto &[n, pid] : pids) {
      int status = 0;
      waitpid(pid, &status, 0);
      const string dir = root + "/" + case_name + "." + mode.Name + "." + to_string(n);
      const auto rec = TRecord::Read(dir + "/run");
      TRun run{case_name, mode.Name, n};
      run.Reached = rec.count("reached") && rec.at("reached") == "1";
      run.Outcome = rec.count("outcome") ? rec.at("outcome") : "crash";
      run.Detail = rec.count("detail") ? rec.at("detail") : "supervisor died";
      run.Injected = rec.count("injected") ? rec.at("injected") : "";
      if (!done) {
        runs.push_back(run);
        if (run.Outcome != "ok") {
          cout << "  " << case_name << " " << mode.Name << " N=" << n << ": " << run.Outcome << " (" << run.Detail << ")"
               << (run.Injected.empty() ? "" : " fault " + run.Injected) << (IsExpected(run) ? " [expected]" : "") << endl;
        }
        if (!run.Reached) {
          done = true;
        }
      }
      if (!run.IsFailure() && rec.count("outcome")) {
        for (const char *file : {"record", "reopen", "image", "op.log", "reopen.log", "run"}) {
          unlink((dir + "/" + file).c_str());
        }
        rmdir(dir.c_str());
      }
    }
    if (done) {
      break;
    }
  }
  return runs;
}

static size_t CountThreads() {
  size_t count = 0UL;
  if (DIR *dir = opendir("/proc/self/task")) {
    while (const dirent *entry = readdir(dir)) {
      if (entry->d_name[0] != '.') {
        ++count;
      }
    }
    closedir(dir);
  }
  return count;
}

/* The device itself: a write is lost unless a Sync of its device completed before the power
   loss; it survives one that did. */
FIXTURE(FaultDevicePowerLoss) {
  TFaultPlan plan;
  std::vector<char> image;
  /* device scope */ {
    TFaultDevice device(&plan, 2048UL);
    alignas(4096) static char buf[Disk::Util::PhysicalSectorSize];
    memset(buf, 'a', sizeof(buf));
    auto write = [&](size_t sector, char fill) {
      memset(buf, fill, sizeof(buf));
      device.Write(HERE, Disk::Util::FullSector, 0, buf, Disk::Util::PhysicalBlockSize + sector * Disk::Util::PhysicalSectorSize,
                   Disk::Util::PhysicalSectorSize, Disk::RealTime, true, 0, [](Disk::TDiskResult, const char *) {});
    };
    write(0, 'a');
    device.Sync();
    write(1, 'b');
    write(0, 'c');
    plan.Arm();
    plan.PowerLossAtSync(1UL, 0.0, 1UL, nullptr);
    device.Sync();
    EXPECT_TRUE(plan.IsPoweredOff());
    image = device.GetDurableImage();
    const auto live = device.GetLiveImage();
    EXPECT_EQ(live[Disk::Util::PhysicalBlockSize], 'c');
  }
  EXPECT_EQ(image[Disk::Util::PhysicalBlockSize], 'a');
  EXPECT_EQ(image[Disk::Util::PhysicalBlockSize + Disk::Util::PhysicalSectorSize], 0);
}

/* #618: power is lost after a fold-path disk merge has put its output in the file map, before
   its inputs are removed. Counter A's `+=` chain runs from an Assign base in the first file
   through both inputs, and counter B has a delta in the first input only. A reopen must read
   both exactly as written: an input that reload kept next to the output would count its deltas
   twice. Run with the range a fold output records, and with the narrower range it recorded
   before #618, which a store written by older code can still hold. */
FIXTURE(FoldMergeCrashBeforeInputRemoval) {
  if (!EXPECT_EQ(CountThreads(), 1UL)) {
    /* fork() needs a single-threaded parent */
    return;
  }
  char root_buf[] = "/tmp/orly_fault_XXXXXX";
  if (!EXPECT_TRUE(mkdtemp(root_buf) != nullptr)) {
    return;
  }
  const string root = root_buf;
  auto get = [](const map<string, string> &record, const string &key) {
    const auto pos = record.find(key);
    return (pos == record.end()) ? string("?") : pos->second;
  };
  for (const string case_name : {"MergeDiskFoldCrash", "MergeDiskFoldCrashOldRange"}) {
    const string dir = root + "/" + case_name;
    /* The case loses power itself, so no Sync is ever the Nth. */
    const TRun run = RunOne(case_name, FindMode("Power"), numeric_limits<size_t>::max(), dir);
    const auto op = TRecord::Read(dir + "/record");
    const auto reopen = TRecord::Read(dir + "/reopen");
    cout << case_name << ": " << run.Outcome << " (" << run.Detail << ")" << endl
         << "  before the crash: " << get(op, "files_before_crash") << endl;
    if (op.count("files_with_old_range")) {
      cout << "  with the old range: " << get(op, "files_with_old_range") << endl;
    }
    cout << "  after the reload: " << get(reopen, "files_after_reload") << endl;
    EXPECT_TRUE(run.Reached);
    /* "ok" also means the reload freed the leftover inputs' blocks (#620). */
    EXPECT_EQ(run.Outcome, "ok");
    EXPECT_EQ(get(reopen, "verify"), "");
  }
  if (!getenv("ORLY_FAULT_KEEP")) {
    const string cmd = "rm -rf " + root;
    int ignored = system(cmd.c_str());
    (void)ignored;
  } else {
    cout << "runs kept in " << root << endl;
  }
}

/* The sweep. Prints one line per failing run and a table of outcomes per case and mode. */
FIXTURE(FaultInjection) {
  if (!EXPECT_EQ(CountThreads(), 1UL)) {
    /* fork() needs a single-threaded parent */
    return;
  }
  char root_buf[] = "/tmp/orly_fault_XXXXXX";
  if (!EXPECT_TRUE(mkdtemp(root_buf) != nullptr)) {
    return;
  }
  const string root = root_buf;
  const char *only_case = getenv("ORLY_FAULT_CASE");
  const char *only_mode = getenv("ORLY_FAULT_MODE");
  const size_t max_n = 400UL;
  map<pair<string, string>, vector<TRun>> all;
  for (const auto &case_name : CaseNames) {
    if (only_case && case_name != only_case) {
      continue;
    }
    for (const auto &mode : Modes) {
      if (only_mode && string(mode.Name) != only_mode) {
        continue;
      }
      const auto start = std::chrono::steady_clock::now();
      auto runs = Sweep(case_name, mode, root, max_n);
      const auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() / 1000.0;
      map<string, size_t> by_outcome;
      for (const auto &run : runs) {
        if (run.Reached) {
          ++by_outcome[run.Outcome];
        }
      }
      ostringstream line;
      line << case_name << " " << mode.Name << ": " << (runs.empty() ? 0UL : runs.size() - (runs.back().Reached ? 0UL : 1UL)) << " faults (" << secs << "s)";
      for (const auto &[outcome, count] : by_outcome) {
        line << ", " << outcome << " " << count;
      }
      if (!runs.empty() && runs.back().Reached) {
        line << ", STOPPED AT N=" << max_n;
      }
      cout << line.str() << endl;
      all[{case_name, mode.Name}] = std::move(runs);
    }
  }
  /* Unexpected failures, then expected failures that did not happen. */
  for (const auto &[key, runs] : all) {
    for (const auto &run : runs) {
      if (run.IsFailure() && !IsExpected(run)) {
        EXPECT_TRUE(false);
        cout << "UNEXPECTED: " << run.Case << " " << run.Mode << " N=" << run.N << ": " << run.Outcome << " (" << run.Detail << ")" << endl;
      }
    }
    EXPECT_TRUE(runs.empty() || !runs.back().Reached);
  }
  for (const auto &xfail : ExpectedFailures) {
    const auto pos = all.find({xfail.Case, xfail.Mode});
    if (pos == all.end()) {
      continue;
    }
    bool seen = false;
    for (const auto &run : pos->second) {
      seen = seen || (run.Outcome == xfail.Outcome);
    }
    if (!seen) {
      EXPECT_TRUE(false);
      cout << "UNEXPECTED PASS: " << xfail.Case << " " << xfail.Mode << " no longer ends in " << xfail.Outcome << "; #" << xfail.Issue
           << " may be fixed, remove its entry" << endl;
    }
  }
  /* Keep the directories of failing runs only when asked. */
  if (!getenv("ORLY_FAULT_KEEP")) {
    const string cmd = "rm -rf " + root;
    int ignored = system(cmd.c_str());
    (void)ignored;
  } else {
    cout << "runs kept in " << root << endl;
  }
}
