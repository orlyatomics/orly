/* <orly/indy/disk/durable_manager.test.cc>

   Unit test for <orly/indy/disk/durable_manager.h>, pinning the durability-signal
   contract (#277): a saver's semaphore fires only once its save is actually on
   disk, and shutdown flushes (rather than drops) whatever is still in memory.

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

#include <orly/indy/disk/durable_manager.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include <base/scheduler.h>
#include <base/uuid.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/transaction_base.h>
#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;

/* The durable manager hands every save to the indy manager for replication; these tests only
   exercise the disk path, so stub the replication hooks out. */
class TReplicationStub final
    : public Orly::Indy::DurableManager::TManager {
  public:

  TReplicationStub() {}

  virtual TDurableReplication *NewDurableReplication(const Durable::TId &/*id*/, const Durable::TTtl &/*ttl*/, const std::string &/*serialized_form*/) const override {
    return nullptr;
  }

  virtual void DeleteDurableReplication(TDurableReplication */*durable_replication*/) NO_THROW override {}

  virtual void EnqueueDurable(TDurableReplication */*durable_replication*/) NO_THROW override {}

};  // TReplicationStub

/* The durable manager's object pools are process-global statics that each binary defines and
   sizes for itself (the server does the same in server.cc). */
Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::Pool(sizeof(Disk::TDurableManager::TMapping), "Durable Mapping", 1000UL);
Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::TEntry::Pool(sizeof(Disk::TDurableManager::TMapping::TEntry), "Durable Mapping Entry", 10000UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TDurableLayer::Pool(std::max(sizeof(Disk::TDurableManager::TMemSlushLayer), sizeof(Disk::TDurableManager::TDiskOrderedLayer)), "Durable Layer", 2000UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TMemSlushLayer::TDurableEntry::Pool(sizeof(Disk::TDurableManager::TMemSlushLayer::TDurableEntry), "Durable Entry", 10000UL);

Disk::TBufBlock::TPool Disk::TBufBlock::Pool(Disk::Util::PhysicalBlockSize, 2000UL);

/* Referenced by the linked engine/manager code; not exercised by these fixtures. */
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(L0::TManager::TRepo::TMapping), "Repo Mapping", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(L0::TManager::TRepo::TMapping::TEntry), "Repo Mapping Entry", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 100UL);
Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(std::max(std::max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation", 100UL);
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction", 100UL);
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 100UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 200UL);

/* Boilerplate to run 'test' on a fiber, with the frame pool manager exposed (the durable
   manager's constructor needs it to launch its writer/merger schedulers). */
static void RunOnFiber(const std::function<void (Fiber::TRunner::TRunnerCons &,
                                                 Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *)> &test) {
  const size_t stack_size = 8 * 1024UL * 1024UL;
  /* Runner ids are handed out monotonically (never recycled), so budget for the test fiber's
     runner plus writer+merger schedulers for BOTH the manager under test and the reattached
     verification manager. */
  Fiber::TRunner::TRunnerCons runner_cons(8);
  Fiber::TRunner runner(runner_cons);
  Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> frame_pool_manager(30UL, stack_size, &runner);
  if (!Fiber::TFrame::LocalFramePool) {
    Fiber::TFrame::LocalFramePool = new Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(&frame_pool_manager);
  }
  auto launch_fiber_sched = [](Fiber::TRunner *runner, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    if (!Fiber::TFrame::LocalFramePool) {
      Fiber::TFrame::LocalFramePool = new Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(frame_pool_manager);
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
          const std::function<void (Fiber::TRunner::TRunnerCons &, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *)> &test,
          Fiber::TRunner::TRunnerCons &runner_cons,
          Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager,
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

    const std::function<void (Fiber::TRunner::TRunnerCons &, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *)> &Test;
    Fiber::TRunner::TRunnerCons &RunnerCons;
    Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *FramePoolManager;
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

/* The saver's sem must not fire before the write-behind flush has actually happened, must fire
   once it has, and the save must then be readable from disk by a fresh manager. */
FIXTURE(SemFiresOnlyAfterFlush) {
  RunOnFiber([](Fiber::TRunner::TRunnerCons &runner_cons, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    TScheduler scheduler(TScheduler::TPolicy(4, 8, milliseconds(30000)));
    Sim::TMemEngine mem_engine(&scheduler, 64, 16, 64, 1, 32, 1);
    TReplicationStub rep_stub;
    const Durable::TId id(TUuid::Twister);
    const Durable::TTtl ttl(600);
    const Durable::TDeadline deadline = Durable::TDeadline::clock::now() + ttl;
    const std::string blob = "some serialized durable";
    /* manager scope */ {
      TDurableManager durable_manager(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                                      100UL /* max cache size */,
                                      milliseconds(300) /* write delay */,
                                      milliseconds(300) /* merge delay */,
                                      milliseconds(10000) /* layer cleaning interval */,
                                      20UL /* temp file consol thresh */,
                                      true /* create */);
      Durable::TSem sem;
      durable_manager.Save(id, deadline, ttl, blob, &sem);
      /* Not yet: the writer flushes on a 300ms cadence, and durability must not be signalled
         before the data is written (#277).  (Pre-#277, Save() pushed the sem synchronously,
         which makes this assertion fail.) */
      EXPECT_FALSE(sem.GetFd().IsReadable(0));
      /* Now block for it: the flush makes it fire. */
      sem.Pop();
      /* Visible through this manager, too. */
      std::string loaded;
      EXPECT_TRUE(durable_manager.TryLoad(id, loaded));
      EXPECT_EQ(loaded, blob);
    }
    /* A fresh manager attached to the same engine sees the save on disk: it was flushed, not
       just cached in the dead manager's memory layer. */
    /* reattach scope */ {
      TDurableManager reattached(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                                 100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL,
                                 false /* create: attach to existing */);
      std::string loaded;
      EXPECT_TRUE(reattached.CanLoad(id));
      EXPECT_TRUE(reattached.TryLoad(id, loaded));
      EXPECT_EQ(loaded, blob);
    }
  });
}

/* Destroying the manager while a save is still unflushed must flush it (and release the saver),
   not silently drop it: the destructor's final drain (#277). */
FIXTURE(ShutdownDrainFlushesAndReleases) {
  RunOnFiber([](Fiber::TRunner::TRunnerCons &runner_cons, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    TScheduler scheduler(TScheduler::TPolicy(4, 8, milliseconds(30000)));
    Sim::TMemEngine mem_engine(&scheduler, 64, 16, 64, 1, 32, 1);
    TReplicationStub rep_stub;
    const Durable::TId id(TUuid::Twister);
    const Durable::TTtl ttl(600);
    const Durable::TDeadline deadline = Durable::TDeadline::clock::now() + ttl;
    const std::string blob = "unflushed at shutdown";
    Durable::TSem sem;
    /* manager scope */ {
      TDurableManager durable_manager(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                                      100UL,
                                      milliseconds(60000) /* write delay: never flushes on its own */,
                                      milliseconds(60000), milliseconds(60000), 20UL, true);
      durable_manager.Save(id, deadline, ttl, blob, &sem);
      EXPECT_FALSE(sem.GetFd().IsReadable(0));
      /* Destroy with the save still in the memory layer. */
    }
    /* The drain released the saver... */
    EXPECT_TRUE(sem.GetFd().IsReadable(0));
    /* ...and the save is actually on disk. */
    TDurableManager reattached(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                               100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL, false);
    std::string loaded;
    EXPECT_TRUE(reattached.TryLoad(id, loaded));
    EXPECT_EQ(loaded, blob);
  });
}

/* A null sem is a fire-and-forget save: no signal, no crash, still flushed. */
FIXTURE(NullSemIsFireAndForget) {
  RunOnFiber([](Fiber::TRunner::TRunnerCons &runner_cons, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    TScheduler scheduler(TScheduler::TPolicy(4, 8, milliseconds(30000)));
    Sim::TMemEngine mem_engine(&scheduler, 64, 16, 64, 1, 32, 1);
    TReplicationStub rep_stub;
    const Durable::TId id(TUuid::Twister);
    const Durable::TTtl ttl(600);
    const Durable::TDeadline deadline = Durable::TDeadline::clock::now() + ttl;
    const std::string blob = "fire and forget";
    /* manager scope */ {
      TDurableManager durable_manager(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                                      100UL, milliseconds(60000), milliseconds(60000), milliseconds(60000), 20UL, true);
      durable_manager.Save(id, deadline, ttl, blob, nullptr);
    }
    TDurableManager reattached(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                               100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL, false);
    std::string loaded;
    EXPECT_TRUE(reattached.TryLoad(id, loaded));
    EXPECT_EQ(loaded, blob);
  });
}
