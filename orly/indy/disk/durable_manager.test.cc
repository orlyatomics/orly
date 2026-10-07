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
#include <vector>

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

/* The saver's sem must fire only once the save is on disk (#277), and the save must then be
   readable from disk by a fresh manager. */
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
      sem.Pop();
      /* #277: by the time the sem fires, the save must already be on disk.  The writer registers
         the durable file with the engine, and waits for that to land, before it releases the
         savers (TSortedByIdFile's constructor, then ReleaseSavers), so this holds on every
         interleaving and cannot fail on correct code.  Pre-#277, Save() pushed the sem
         synchronously: Pop() then returns at once, while the writer has barely started writing
         the file, so the set is all but certainly still empty -- a regression is caught on
         essentially every run, and correct code is never failed.

         This used to be asserted the other way round: "the sem has not fired just after Save()",
         on the premise that the writer waits out its write delay first.  It never did --
         Util::SleepUntil never slept (#576), and the writer now has no delay at all -- so it
         flushes the moment the save arrives and that assertion was a race against it.  That race is the #551 flake, and why widening
         the delay to 2s did not stop it. */
      std::vector<TFileObj> durable_files;
      mem_engine.GetEngine()->AppendFileGenSet(TDurableManager::DurableByIdFileId, durable_files);
      EXPECT_FALSE(durable_files.empty());
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

/* Saves made after a reopen must win over the copies already on disk (#609). Loads and merges
   both keep an id's entry with the highest sequence number, so a manager that numbered its saves
   from 1 again after a restart lost every new save to an older one: TryLoad returned the old copy
   and the next merge discarded the new one for good. */
FIXTURE(SeqNumSurvivesReopen) {
  RunOnFiber([](Fiber::TRunner::TRunnerCons &runner_cons, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    TScheduler scheduler(TScheduler::TPolicy(4, 8, milliseconds(30000)));
    Sim::TMemEngine mem_engine(&scheduler, 64, 16, 64, 1, 32, 1);
    TReplicationStub rep_stub;
    const Durable::TId id(TUuid::Twister);
    const Durable::TTtl ttl(600);
    const Durable::TDeadline deadline = Durable::TDeadline::clock::now() + ttl;
    auto save = [&](TDurableManager &manager, const std::string &blob) {
      Durable::TSem sem;
      manager.Save(id, deadline, ttl, blob, &sem);
      sem.Pop();  // on disk, in a file of its own
    };
    auto num_files = [&] {
      std::vector<TFileObj> files;
      mem_engine.GetEngine()->AppendFileGenSet(TDurableManager::DurableByIdFileId, files);
      return files.size();
    };
    /* Before the restart: two saves, so the copy on disk has a sequence number above 1. */ {
      TDurableManager first(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                            100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL, true);
      save(first, "before restart 1");
      save(first, "before restart 2");
    }
    EXPECT_EQ(num_files(), 2UL);
    /* After the restart: one more save, which must be the one a load returns. */ {
      TDurableManager second(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                             100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL, false);
      save(second, "after restart");
      std::string loaded;
      EXPECT_TRUE(second.TryLoad(id, loaded));
      EXPECT_EQ(loaded, "after restart");
      /* That makes three one-entry files, which the merger folds into a fourth. The merge keeps
         the entry with the highest sequence number, so it must keep the new save, not drop it.
         (The three inputs stay in the file map: only the layer cleaner, which this test doesn't
         run, removes them. The mapping that TryLoad reads holds just the merged file.) */
      const auto give_up = steady_clock::now() + seconds(30);
      while (num_files() < 4UL && steady_clock::now() < give_up) {
        std::this_thread::sleep_for(milliseconds(10));
      }
      EXPECT_EQ(num_files(), 4UL);
      EXPECT_TRUE(second.TryLoad(id, loaded));
      EXPECT_EQ(loaded, "after restart");
    }
    /* And it is still the new save after another restart. */ {
      TDurableManager third(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                            100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL, false);
      std::string loaded;
      EXPECT_TRUE(third.TryLoad(id, loaded));
      EXPECT_EQ(loaded, "after restart");
      /* Its own saves must still be numbered above everything on disk. */
      save(third, "after second restart");
      EXPECT_TRUE(third.TryLoad(id, loaded));
      EXPECT_EQ(loaded, "after second restart");
    }
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

namespace Orly {

  namespace Indy {

    namespace Disk {

      /* A friend of TDurableManager (see its header), for the test below. */
      class TDurableManagerTestAccess {
        public:

        using TView = TDurableManager::TMapping::TView;

        /* Does the mapping this view holds list this layer? (A mapping's entries don't change
           once it is published.) */
        static bool Lists(const TView &view, const TDurableManager::TMemSlushLayer *layer) {
          for (TDurableManager::TMapping::TEntryCollection::TCursor csr(view.GetMapping()->GetEntryCollection()); csr; ++csr) {
            if (csr->GetLayer() == layer) {
              return true;
            }
          }
          return false;
        }

        /* How many disk layers does the mapping this view holds list? */
        static size_t CountDiskLayers(const TView &view) {
          size_t count = 0UL;
          for (TDurableManager::TMapping::TEntryCollection::TCursor csr(view.GetMapping()->GetEntryCollection()); csr; ++csr) {
            if (csr->GetLayer()->GetKind() == TDurableManager::TDurableLayer::DiskOrdered) {
              ++count;
            }
          }
          return count;
        }

        /* Is this layer waiting in the removal queue? Nothing runs the layer cleaner in these
           tests, so a layer queued there stays until the manager goes. */
        static bool IsQueuedForRemoval(TDurableManager &manager, const TDurableManager::TMemSlushLayer *layer) {
          std::lock_guard<std::mutex> removal_lock(manager.RemovalLock);
          for (TDurableManager::TRemovalCollection::TCursor csr(&manager.RemovalCollection); csr; ++csr) {
            if (&*csr == layer) {
              return true;
            }
          }
          return false;
        }

      };  // TDurableManagerTestAccess

    }  // Disk

  }  // Indy

}  // Orly

/* #727: a view drops its reference to a memory layer while the writer marks that layer for
   delete. TView's destructor releases its memory layer before it takes MappingLock, and Decr()
   read the plain bool MarkedForDelete on every release, while the writer set it under
   MappingLock, so ThreadSanitizer reported a race on orlyi runs. The answer only matters to the
   release that takes the count to zero, and that one can't be concurrent with the mark: the
   mapping the writer replaces still lists the layer, and its entry's reference goes only under
   MappingLock, after the mark. So the layer must still be queued for removal, and only when its
   last reference goes.

   Each round: `held` pins the current memory layer before a save lands in it. The writer moves
   that layer into a new mapping and writes it to disk; the hook holds it there while `pin` takes
   that mapping, so the writer's mark can't drop the layer's last mapping reference itself. Then
   the writer marks the layer and publishes a mapping without it, and `held` goes after the mark,
   on a thread that hasn't synchronized with the writer since the hook: before the fix, that read
   is the race. */
FIXTURE(ViewDropsRefToLayerBeingMarked) {
  using TAccess = Disk::TDurableManagerTestAccess;
  RunOnFiber([](Fiber::TRunner::TRunnerCons &runner_cons, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    TScheduler scheduler(TScheduler::TPolicy(4, 8, milliseconds(30000)));
    Sim::TMemEngine mem_engine(&scheduler, 64, 16, 64, 1, 32, 1);
    TReplicationStub rep_stub;
    const Durable::TTtl ttl(600);
    const Durable::TDeadline deadline = Durable::TDeadline::clock::now() + ttl;
    TDurableManager durable_manager(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                                    100UL, milliseconds(300), milliseconds(300), milliseconds(10000), 20UL, true);
    std::mutex hook_mutex;
    std::condition_variable hook_cond;
    bool written = false, resume = false;
    TDurableManager::OnMemLayerWrittenForTest = [&](TDurableManager *manager) {
      if (manager == &durable_manager) {
        std::unique_lock<std::mutex> lock(hook_mutex);
        written = true;
        hook_cond.notify_all();
        hook_cond.wait(lock, [&] { return resume; });
      }
    };
    for (size_t round = 0UL; round < 20UL; ++round) {
      auto held = make_unique<TAccess::TView>(&durable_manager);
      const auto *layer = held->GetCurLayer();
      /* hook */ {
        std::lock_guard<std::mutex> lock(hook_mutex);
        written = false;
        resume = false;
      }
      Durable::TSem sem;
      durable_manager.Save(Durable::TId(TUuid::Twister), deadline, ttl, "round " + to_string(round), &sem);
      unique_ptr<TAccess::TView> pin;
      /* the writer is parked in the hook: its new mapping, which lists the layer, is the newest */ {
        std::unique_lock<std::mutex> lock(hook_mutex);
        hook_cond.wait(lock, [&] { return written; });
        pin = make_unique<TAccess::TView>(&durable_manager);
        resume = true;
        hook_cond.notify_all();
      }
      EXPECT_TRUE(TAccess::Lists(*pin, layer));
      sem.Pop();
      /* Let the writer mark the layer. Waiting on anything the writer does after the mark would
         order the mark before the release below, and hide the race. */
      std::this_thread::sleep_for(milliseconds(20));
      held.reset();
      /* `pin`'s mapping still holds a reference, so the layer can't be queued yet... */
      EXPECT_FALSE(TAccess::IsQueuedForRemoval(durable_manager, layer));
      pin.reset();
      /* ...and is once the last one goes. The merger's views pin mappings too, so its release may
         come a moment later, on its thread. */
      const auto give_up = steady_clock::now() + seconds(10);
      while (!TAccess::IsQueuedForRemoval(durable_manager, layer) && steady_clock::now() < give_up) {
        std::this_thread::sleep_for(milliseconds(1));
      }
      EXPECT_TRUE(TAccess::IsQueuedForRemoval(durable_manager, layer));
    }
    TDurableManager::OnMemLayerWrittenForTest = nullptr;
  });
}

/* #782: the writer woke the merger before publishing the mapping that lists the file it had just
   written. A merger that lapped in between found a generation one file short of the three it
   merges and waited, and nothing woke it again: three files that should have merged never did
   (the fault-injection durable case hung). The hook holds the writer in that window on every
   flush. Now the wake comes after the mapping, so the three files merge into one. */
FIXTURE(MergerSeesTheFileItWasWokenFor) {
  using TAccess = Disk::TDurableManagerTestAccess;
  RunOnFiber([](Fiber::TRunner::TRunnerCons &runner_cons, Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *> *frame_pool_manager) {
    TScheduler scheduler(TScheduler::TPolicy(4, 8, milliseconds(30000)));
    Sim::TMemEngine mem_engine(&scheduler, 64, 16, 64, 1, 32, 1);
    TReplicationStub rep_stub;
    const Durable::TTtl ttl(600);
    const Durable::TDeadline deadline = Durable::TDeadline::clock::now() + ttl;
    TDurableManager durable_manager(&scheduler, runner_cons, frame_pool_manager, &rep_stub, mem_engine.GetEngine(),
                                    100UL, milliseconds(0), milliseconds(0), milliseconds(10000), 20UL, true);
    TDurableManager::OnMemLayerWrittenForTest = [&](TDurableManager *manager) {
      if (manager == &durable_manager) {
        /* Long enough for a woken merger to lap. */
        std::this_thread::sleep_for(milliseconds(50));
      }
    };
    for (size_t i = 0UL; i < 3UL; ++i) {
      Durable::TSem sem;
      durable_manager.Save(Durable::TId(TUuid::Twister), deadline, ttl, "save " + to_string(i), &sem);
      sem.Pop();
    }
    const auto give_up = steady_clock::now() + seconds(10);
    size_t disk_layers = 0UL;
    for (;;) {
      /* scope the view */ {
        TAccess::TView view(&durable_manager);
        disk_layers = TAccess::CountDiskLayers(view);
      }
      if (disk_layers == 1UL || steady_clock::now() > give_up) {
        break;
      }
      std::this_thread::sleep_for(milliseconds(10));
      Fiber::YieldSlow();
    }
    EXPECT_EQ(disk_layers, 1UL);
    TDurableManager::OnMemLayerWrittenForTest = nullptr;
  });
}
