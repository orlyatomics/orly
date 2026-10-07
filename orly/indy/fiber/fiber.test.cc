/* <orly/indy/fiber/fiber.test.cc>

   Unit test for <orly/indy/fiber/fiber.h>.

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

#include <orly/indy/fiber/fiber.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <thread>
#include <vector>

#include <unistd.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy::Fiber;

class TTestClass {
  NO_COPY(TTestClass);
  public:

  TTestClass(int64_t v1, double v2, std::atomic<int64_t> &v3)
      : V1(v1), V2(v2), V3(v3) {}

  int64_t V1;
  double V2;
  std::atomic<int64_t> &V3;
};

static int64_t V1Init = 2;
static double V2Init = 3.0;
static std::atomic<int64_t> V3Init(42);
static std::atomic<int64_t> ExpectedV3(42);
static auto MyLocal =
    MakeFiberLocal<TTestClass>(V1Init, V2Init, std::ref(V3Init));

FIXTURE(Typical) {
  TRunner::TRunnerCons runner_cons(1);
  TRunner runner(runner_cons);
  auto launch_fiber_sched = [&]() {
    runner.Run();
  };
  thread t1(launch_fiber_sched);
  sleep(2);
  runner.ShutDown();
  t1.join();
}

FIXTURE(FiberLocal) {
  class TTest : public TRunnable {
    NO_COPY(TTest);
    public:
    TTest(TRunner *runner, const std::function<void ()> &completion_cb) : CompletionCb(completion_cb) {
      Frame = TFrame::LocalFramePool->Alloc();
      try {
        Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TTest::Run));
      } catch (...) {
        TFrame::LocalFramePool->Free(Frame);
        throw;
      }
    }
    ~TTest() {
      TFrame::LocalFramePool->Free(Frame);
    }
    void Run() {
      EXPECT_EQ(MyLocal->V1, V1Init);
      EXPECT_EQ(MyLocal->V2, V2Init);
      EXPECT_EQ(MyLocal->V3, V3Init);
      MyLocal->V1 *= 2;
      MyLocal->V2 *= 2;
      MyLocal->V3 = MyLocal->V3 * 2L;
      ExpectedV3 = ExpectedV3 * 2L;
      EXPECT_EQ(MyLocal->V1, V1Init * 2);
      EXPECT_EQ(MyLocal->V2, V2Init * 2);
      EXPECT_EQ(MyLocal->V3, V3Init);
      EXPECT_EQ(MyLocal->V3, ExpectedV3);
      CompletionCb();
    }
    private:
    TFrame *Frame;
    const std::function<void ()> CompletionCb;
  };
  const size_t num_frames = 2UL;
  const size_t stack_size = 1 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(1);
  TRunner runner(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> frame_pool_manager(num_frames, stack_size, &runner);
  TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(&frame_pool_manager);
  try {
    auto launch_fiber_sched = [&]() {
      runner.Run();
    };
    thread t1(launch_fiber_sched);
    sleep(1);
    std::mutex mut;
    std::condition_variable cond;
    size_t finished_count = 0UL;
    auto completion_cb = [&]() {
      std::lock_guard<std::mutex> lock(mut);
      ++finished_count;
      cond.notify_one();
    };
    TTest runnable1(&runner, completion_cb);
    TTest runnable2(&runner, completion_cb);
    std::unique_lock<std::mutex> lock(mut);
    while (finished_count < 2UL) {
      cond.wait(lock);
    }
    runner.ShutDown();
    t1.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
}

class TCoIterRunnable
    : public TRunnable {
  NO_COPY(TCoIterRunnable);
  public:

  TCoIterRunnable(size_t /*id*/, size_t num_iter, TRunner *runner, const std::function<void ()> &start_cb, const std::function<void ()> &completion_cb)
      : /*Id(id),*/ NumIter(num_iter), StartCb(start_cb), CompletionCb(completion_cb) {
    Frame = TFrame::LocalFramePool->Alloc();
    try {
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TCoIterRunnable::Compute));
    } catch (...) {
      TFrame::LocalFramePool->Free(Frame);
      throw;
    }
  }

  ~TCoIterRunnable() {
    TFrame::LocalFramePool->Free(Frame);
  }

  void Compute() {
    StartCb();
    for (size_t i = 0; i < NumIter; ++i) {
      //std::cout << "TMyRunnable [" << Id << "] Compute [" << i << "]" << std::endl;
      Yield();
    }
    CompletionCb();
  }

  TFrame *GetFrame() const {
    return Frame;
  }

  private:

  //size_t Id;

  size_t NumIter;

  TFrame *Frame;

  const std::function<void ()> StartCb;

  const std::function<void ()> CompletionCb;

};

FIXTURE(CoIterate) {
  const size_t num_iter = 10000000UL;
  const size_t num_frames = 4UL;
  const size_t stack_size = 8 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(1);
  TRunner runner(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> frame_pool_manager(num_frames, stack_size, &runner);
  TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(&frame_pool_manager);
  try {
    auto launch_fiber_sched = [&]() {
      runner.Run();
    };
    thread t1(launch_fiber_sched);
    sleep(1);
    std::mutex mut;
    std::condition_variable cond;
    size_t finished_count = 0UL;
    auto completion_cb = [&]() {
      std::lock_guard<std::mutex> lock(mut);
      ++finished_count;
      cond.notify_one();
    };
    TCoIterRunnable runnable1(1UL, num_iter, &runner, [&](){
      Wait();
    }, completion_cb);
    TCoIterRunnable runnable2(2UL, num_iter, &runner, [&](){
      TRunner::Schedule(runnable1.GetFrame());
    }, completion_cb);
    std::unique_lock<std::mutex> lock(mut);
    while (finished_count < 2UL) {
      cond.wait(lock);
    }
    runner.ShutDown();
    t1.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
}

class TSpawnRunnable
    : public TRunnable {
  NO_COPY(TSpawnRunnable);
  public:

  TSpawnRunnable(TRunner *runner)
      : Finished(0UL), ExpectedSum(0UL), ActualSum(0UL) {
    Frame = TFrame::LocalFramePool->Alloc();
    try {
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TSpawnRunnable::Spawn));
    } catch (...) {
      TFrame::LocalFramePool->Free(Frame);
      throw;
    }
  }

  ~TSpawnRunnable() {
    TFrame::LocalFramePool->Free(Frame);
  }

  void Spawn() {
    for (size_t i = 0; i < 5; ++i) {
      ExpectedSum += i;
      SubProcVec.emplace_back(new TSubProc(this, i));
    }
    EXPECT_NE(ActualSum, ExpectedSum);
    Wait();
    EXPECT_EQ(ActualSum, ExpectedSum);
    for (auto sub_proc : SubProcVec) {
      delete sub_proc;
    }
  }

  TFrame *GetFrame() const {
    return Frame;
  }

  private:

  void CompleteSubProc() {
    ++Finished;
    if (Finished == SubProcVec.size()) {
      TRunner::Schedule(Frame);
    }
  }

  class TSubProc
      : public TRunnable {
    NO_COPY(TSubProc);
    public:

    TSubProc(TSpawnRunnable *spawner, size_t my_val)
        : Spawner(spawner), MyVal(my_val) {
      Frame = TFrame::LocalFramePool->Alloc();
      try {
        Frame->Latch(this, static_cast<TRunnable::TFunc>(&TSubProc::Compute));
      } catch (...) {
        TFrame::LocalFramePool->Free(Frame);
        throw;
      }
    }

    virtual ~TSubProc() {
      TFrame::LocalFramePool->Free(Frame);
    }

    TFrame *GetFrame() const {
      return Frame;
    }

    void Compute() {
      Spawner->ActualSum += MyVal;
      Spawner->CompleteSubProc();
    }

    private:

    TSpawnRunnable *Spawner;

    TFrame *Frame;

    size_t MyVal;

  };

  std::vector<TSubProc *> SubProcVec;

  size_t Finished;

  TFrame *Frame;

  size_t ExpectedSum;

  size_t ActualSum;

};

FIXTURE(SpawnAndSync) {
  const size_t num_frames = 11UL;
  const size_t stack_size = 8 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(1UL);
  TRunner runner(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> frame_pool_manager(num_frames, stack_size, &runner);
  TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(&frame_pool_manager);
  try {
    auto launch_fiber_sched = [&]() {
      TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(&frame_pool_manager);
      try {
        runner.Run();
      } catch (...) {
        delete TFrame::LocalFramePool;
        TFrame::LocalFramePool = nullptr;
        throw;
      }
      delete TFrame::LocalFramePool;
      TFrame::LocalFramePool = nullptr;
    };
    thread t1(launch_fiber_sched);
    sleep(1);
    TSpawnRunnable runnable(&runner);
    sleep(2);
    runner.ShutDown();
    t1.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
}

class TLeakRunnable
    : public TRunnable {
  NO_COPY(TLeakRunnable);
  public:

  TLeakRunnable(TRunner *runner, bool &caught_exception) : CaughtException(caught_exception) {
    assert(!caught_exception);
    Frame = TFrame::LocalFramePool->Alloc();
    try {
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TLeakRunnable::Leak));
    } catch (...) {
      TFrame::LocalFramePool->Free(Frame);
      throw;
    }
  }

  ~TLeakRunnable() {
    try {
      TFrame::LocalFramePool->Free(Frame);
    } catch (const std::logic_error &) {
      CaughtException = true;
    }
  }

  void Leak() {
    Wait();
  }

  TFrame *GetFrame() const {
    return Frame;
  }

  private:

  TFrame *Frame;

  bool &CaughtException;

};

FIXTURE(CatchLeakedFrame) {
  const size_t num_frames = 1UL;
  const size_t stack_size = 8 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(1);
  TRunner runner(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> *frame_pool_manager = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>(num_frames, stack_size, &runner);
  TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(frame_pool_manager);
  bool caught_expected_exception = false;
  try {
    auto launch_fiber_sched = [&]() {
      runner.Run();
    };
    thread t1(launch_fiber_sched);
    sleep(1);
    TLeakRunnable runnable(&runner, caught_expected_exception);
    sleep(2);
    runner.ShutDown();
    t1.join();
  } catch (const std::logic_error &ex) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  delete frame_pool_manager;
  EXPECT_TRUE(caught_expected_exception);
}

class TSimpleRunnable
    : public TRunnable {
  NO_COPY(TSimpleRunnable);
  public:

  TSimpleRunnable(TRunner *runner, TRunner *other_runner, bool &did_run) : OtherRunner(other_runner), DidRun(did_run) {
    Frame = TFrame::LocalFramePool->Alloc();
    try {
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TSimpleRunnable::RunMe));
    } catch (...) {
      TFrame::LocalFramePool->Free(Frame);
      throw;
    }
  }

  ~TSimpleRunnable() {
    TFrame::LocalFramePool->Free(Frame);
  }

  void RunMe() {
    TRunner *start_runner = TRunner::LocalRunner;
    TRunner *middle_runner = nullptr;
    /* RAII scope */ {
      TSwitchToRunner switcher(OtherRunner);
      middle_runner = TRunner::LocalRunner;
    }
    TRunner *end_runner = TRunner::LocalRunner;
    EXPECT_EQ(start_runner, end_runner);
    EXPECT_NE(start_runner, middle_runner);
    DidRun = true;
  }

  private:

  TFrame *Frame;

  TRunner *OtherRunner;

  bool &DidRun;

};

FIXTURE(SwitchRunnerToRunnerRAII) {
  const size_t num_frames = 2UL;
  const size_t stack_size = 8 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(2UL);
  bool did_run = false;
  TRunner runner_1(runner_cons);
  TRunner runner_2(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> *frame_pool_manager = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>(num_frames, stack_size, &runner_1);
  try {
    TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(frame_pool_manager);
    auto launch_fiber_sched = [&](TRunner *runner) {
      runner->Run();
    };
    thread t1(std::bind(launch_fiber_sched, &runner_1));
    thread t2(std::bind(launch_fiber_sched, &runner_2));
    TSimpleRunnable runnable(&runner_1, &runner_2, did_run);
    sleep(2);
    runner_1.ShutDown();
    runner_2.ShutDown();
    t1.join();
    t2.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  delete frame_pool_manager;
  EXPECT_TRUE(did_run);
}

class TActorRunnable
    : public TRunnable {
  NO_COPY(TActorRunnable);
  public:

  TActorRunnable(TRunner *runner,
                   TSingleSem &my_push_sem,
                   TSingleSem &my_pop_sem,
                   std::atomic<size_t> &pos_counter,
                   std::atomic<size_t> &accuracy_counter,
                   std::mutex &mut,
                   std::condition_variable &cond,
                   size_t &finish_count)
    : MyPushSem(my_push_sem),
      MyPopSem(my_pop_sem),
      PosCounter(pos_counter),
      AccuracyCounter(accuracy_counter),
      Mut(mut),
      Cond(cond),
      FinishCount(finish_count) {
    Frame = TFrame::LocalFramePool->Alloc();
    try {
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TActorRunnable::RunMe));
    } catch (...) {
      TFrame::LocalFramePool->Free(Frame);
      throw;
    }
  }

  ~TActorRunnable() {
    TFrame::LocalFramePool->Free(Frame);
  }

  void RunMe() {
    MyPopSem.Pop();
    // 2
    ++PosCounter;
    AccuracyCounter += (PosCounter * 11UL);
    MyPushSem.Push();
    MyPopSem.Pop();
    // 4
    ++PosCounter;
    AccuracyCounter += (PosCounter * 13UL);
    MyPushSem.Push();
    std::lock_guard<std::mutex> lock(Mut);
    ++FinishCount;
    Cond.notify_one();
  }

  private:

  TFrame *Frame;

  TSingleSem &MyPushSem;
  TSingleSem &MyPopSem;

  std::atomic<size_t> &PosCounter;
  std::atomic<size_t> &AccuracyCounter;

  std::mutex &Mut;
  std::condition_variable &Cond;
  size_t &FinishCount;

};

class TTriggerRunnable
    : public TRunnable {
  NO_COPY(TTriggerRunnable);
  public:

  TTriggerRunnable(TRunner *runner,
                   TSingleSem &my_push_sem,
                   TSingleSem &my_pop_sem,
                   std::atomic<size_t> &pos_counter,
                   std::atomic<size_t> &accuracy_counter,
                   std::mutex &mut,
                   std::condition_variable &cond,
                   size_t &finish_count)
    : MyPushSem(my_push_sem),
      MyPopSem(my_pop_sem),
      PosCounter(pos_counter),
      AccuracyCounter(accuracy_counter),
      Mut(mut),
      Cond(cond),
      FinishCount(finish_count) {
    Frame = TFrame::LocalFramePool->Alloc();
    try {
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TTriggerRunnable::RunMe));
    } catch (...) {
      TFrame::LocalFramePool->Free(Frame);
      throw;
    }
  }

  ~TTriggerRunnable() {
    TFrame::LocalFramePool->Free(Frame);
  }

  void RunMe() {
    // 1
    ++PosCounter;
    AccuracyCounter += (PosCounter * 3UL);
    MyPushSem.Push();
    MyPopSem.Pop();
    // 3
    ++PosCounter;
    AccuracyCounter += (PosCounter * 5UL);
    MyPushSem.Push();
    MyPopSem.Pop();
    // 5
    ++PosCounter;
    AccuracyCounter += (PosCounter * 7UL);
    std::lock_guard<std::mutex> lock(Mut);
    ++FinishCount;
    Cond.notify_one();
  }

  private:

  TFrame *Frame;

  TSingleSem &MyPushSem;
  TSingleSem &MyPopSem;

  std::atomic<size_t> &PosCounter;
  std::atomic<size_t> &AccuracyCounter;

  std::mutex &Mut;
  std::condition_variable &Cond;
  size_t &FinishCount;

};

FIXTURE(Sem) {
  const size_t num_frames = 2UL;
  const size_t stack_size = 8 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(2UL);
  TRunner runner_1(runner_cons);
  TRunner runner_2(runner_cons);
  std::atomic<size_t> pos_counter(0UL);
  std::atomic<size_t> accuracy_counter(0UL);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> *frame_pool_manager = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>(num_frames, stack_size, &runner_1);
  try {
    TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(frame_pool_manager);
    auto launch_fiber_sched = [&](TRunner *runner) {
      runner->Run();
    };
    thread t1(std::bind(launch_fiber_sched, &runner_1));
    thread t2(std::bind(launch_fiber_sched, &runner_2));
    TSingleSem fiber_sem_1, fiber_sem_2;
    std::mutex mut;
    std::condition_variable cond;
    size_t finish_count = 0UL;
    TActorRunnable actor_runnable(&runner_1, fiber_sem_1, fiber_sem_2, pos_counter, accuracy_counter, mut, cond, finish_count);
    TTriggerRunnable trigger_runnable(&runner_2, fiber_sem_2, fiber_sem_1, pos_counter, accuracy_counter, mut, cond, finish_count);
    /* wait for them to finish using condition variable since we're not frame ourselves... */ {
      std::unique_lock<std::mutex> lock(mut);
      while (finish_count != 2) {
        cond.wait(lock);
      }
    }
    runner_1.ShutDown();
    runner_2.ShutDown();
    t1.join();
    t2.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  delete frame_pool_manager;
  EXPECT_EQ(pos_counter, 5UL);
  EXPECT_EQ(accuracy_counter, 127UL);
}

/* A cross-runner handoff must survive the destruction of the runner that
   pushed it (#463): the handoff slots are owned by the TRunnerCons, not by
   either runner.  Staged deterministically: runner A dispatches a fiber
   whose SwitchTo(B) publishes the deferred move into the (B, A) handoff
   slot while B's thread has not started yet; A is then shut down, joined,
   and destroyed before B ever polls.  Before the fix the handoff lived in
   A's own queue array and died with A -- B would find nothing (the lost
   fiber is the hang presentation of the teardown race) or, in the live
   server, read A's freed memory (the SIGSEGV presentation). */
FIXTURE(HandoffOutlivesPushingRunner) {
  class TMover
      : public TRunnable {
    NO_COPY(TMover);
    public:

    TMover(TRunner *start_runner, TRunner *move_to_runner, std::atomic<bool> &done)
        : MoveToRunner(move_to_runner), Done(done) {
      Frame = TFrame::LocalFramePool->Alloc();
      try {
        Frame->Latch(start_runner, this, static_cast<TRunnable::TFunc>(&TMover::Run));
      } catch (...) {
        TFrame::LocalFramePool->Free(Frame);
        throw;
      }
    }

    ~TMover() {
      TFrame::LocalFramePool->Free(Frame);
    }

    void Run() {
      SwitchTo(MoveToRunner);
      Done.store(true);
    }

    private:

    TFrame *Frame;

    TRunner *MoveToRunner;

    std::atomic<bool> &Done;

  };
  const size_t num_frames = 1UL;
  const size_t stack_size = 1 * 1024 * 1024;
  TRunner::TRunnerCons runner_cons(2);
  auto runner_a = std::make_unique<TRunner>(runner_cons);
  TRunner runner_b(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> frame_pool_manager(num_frames, stack_size, &runner_b);
  TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(&frame_pool_manager);
  try {
    std::atomic<bool> done(false);
    TMover mover(runner_a.get(), &runner_b, done);
    /* Run A alone: it dispatches the fiber, which parks toward B; A's loop
       publishes the deferred move into the cons-owned slot before it can
       observe the shutdown flag. */
    thread thread_a([&]() { runner_a->Run(); });
    sleep(1);
    runner_a->ShutDown();
    thread_a.join();
    /* The pusher dies before the target ever polls. */
    runner_a.reset();
    EXPECT_FALSE(done.load());
    /* B must drain the handoff from the cons-owned slot and finish the
       fiber, even though the runner that pushed it is gone. */
    thread thread_b([&]() { runner_b.Run(); });
    while (!done.load()) {
      std::this_thread::yield();
    }
    runner_b.ShutDown();
    thread_b.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
}

/* An idle runner parks until a frame is handed to it (#764); it used to poll
   with 10 us sleeps, so every hop onto an idle runner paid a timer tick
   (~60 us on bare Linux, ~3 ms in a Docker VM on macOS). A fiber on runner 1
   idles for a while, so runner 2 has parked, then hops to runner 2 and back.
   Prints the hop latencies. A hop whose wakeup was lost would wait out the
   park's 100 ms safety net, so the median must be far below that. */
FIXTURE(HopOntoParkedRunner) {
  class THopper
      : public TRunnable {
    NO_COPY(THopper);
    public:

    THopper(TRunner *home, TRunner *away, size_t hops, std::chrono::microseconds idle,
            std::vector<double> &to_parked, std::vector<double> &back, std::atomic<bool> &done)
        : Home(home), Away(away), Hops(hops), Idle(idle), ToParked(to_parked), Back(back), Done(done) {
      Frame = TFrame::LocalFramePool->Alloc();
      try {
        Frame->Latch(home, this, static_cast<TRunnable::TFunc>(&THopper::Run));
      } catch (...) {
        TFrame::LocalFramePool->Free(Frame);
        throw;
      }
    }

    ~THopper() {
      TFrame::LocalFramePool->Free(Frame);
    }

    void Run() {
      using namespace std::chrono;
      for (size_t i = 0; i < Hops; ++i) {
        /* Block this runner's thread, so the other runner has nothing to do
           for long enough to park. */
        std::this_thread::sleep_for(Idle);
        const auto t0 = steady_clock::now();
        SwitchTo(Away);
        const auto t1 = steady_clock::now();
        SwitchTo(Home);
        const auto t2 = steady_clock::now();
        ToParked.push_back(duration<double, std::micro>(t1 - t0).count());
        Back.push_back(duration<double, std::micro>(t2 - t1).count());
      }
      Done.store(true);
    }

    private:

    TFrame *Frame;

    TRunner *Home, *Away;

    const size_t Hops;

    const std::chrono::microseconds Idle;

    std::vector<double> &ToParked, &Back;

    std::atomic<bool> &Done;

  };
  const size_t hops = 200UL;
  TRunner::TRunnerCons runner_cons(2UL);
  TRunner runner_1(runner_cons);
  TRunner runner_2(runner_cons);
  TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *> frame_pool_manager(1UL, 1024UL * 1024UL, &runner_1);
  TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(&frame_pool_manager);
  std::vector<double> to_parked, back;
  to_parked.reserve(hops);
  back.reserve(hops);
  try {
    std::atomic<bool> done(false);
    thread t1([&] { runner_1.Run(); });
    thread t2([&] { runner_2.Run(); });
    THopper hopper(&runner_1, &runner_2, hops, std::chrono::microseconds(2000), to_parked, back, done);
    while (!done.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runner_1.ShutDown();
    runner_2.ShutDown();
    t1.join();
    t2.join();
  } catch (...) {
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    throw;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  auto pct = [](std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
  };
  if (EXPECT_EQ(to_parked.size(), hops)) {
    std::cout << "hop onto a parked runner: p50 " << pct(to_parked, 0.5) << " us, p99 " << pct(to_parked, 0.99) << " us" << std::endl
              << "hop back onto a spinning runner: p50 " << pct(back, 0.5) << " us, p99 " << pct(back, 0.99) << " us" << std::endl;
    EXPECT_LT(pct(to_parked, 0.5), 50000.0);
  }
}

/* ShutDown() wakes a parked runner (#764): Run() returns well inside the
   park's 100 ms safety net, rather than when the park next times out. */
FIXTURE(ShutDownWakesParkedRunner) {
  using namespace std::chrono;
  constexpr size_t trials = 3UL;
  TRunner::TRunnerCons runner_cons(trials);
  double worst_ms = 0;
  for (size_t trial = 0; trial < trials; ++trial) {
    TRunner runner(runner_cons);
    thread t([&] { runner.Run(); });
    /* Long enough to finish the spin and park. */
    std::this_thread::sleep_for(milliseconds(50));
    const auto start = steady_clock::now();
    runner.ShutDown();
    t.join();
    worst_ms = std::max(worst_ms, duration<double, std::milli>(steady_clock::now() - start).count());
  }
  std::cout << "slowest stop of a parked runner: " << worst_ms << " ms" << std::endl;
  EXPECT_LT(worst_ms, 60.0);
}
