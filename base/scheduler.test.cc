/* <base/scheduler.test.cc>

   Unit test for <base/scheduler.h>.

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

#include <base/scheduler.h>

#include <atomic>

#include <syslog.h>

#include <base/latch.h>
#include <base/event_semaphore.h>
#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace placeholders;
using namespace Base;

#if 0
static void Push(TEventSemaphore &sem) {
  sem.Push();
}

static void PushPop(TEventSemaphore &sem1, TEventSemaphore &sem2) {
  sem1.Push();
  sem2.Pop();
}

FIXTURE(Typical) {
  // Uncomment this if you want to see the log on stderr.
  //openlog("scheduler.test", LOG_PID | LOG_PERROR, LOG_UPTO(LOG_DEBUG));
  const TScheduler::TPolicy policy(1, 1, milliseconds(10));
  TScheduler scheduler;
  TEventSemaphore sem1, sem2;
  // The scheduler should refuse jobs when it has no workers and isn't allowed to start any.
  EXPECT_FALSE(scheduler.Schedule(bind(Push, ref(sem1))));
  EXPECT_FALSE(sem1.GetFd().IsReadable());
  scheduler.SetPolicy(policy);
  // The scheduler should now accept and run the job.
  EXPECT_TRUE(scheduler.Schedule(bind(Push, ref(sem1))));
  sem1.Pop();
  // This job will hang, requiring the shutdown to be forced.
  EXPECT_TRUE(scheduler.Schedule(bind(PushPop, ref(sem1), ref(sem2))));
  sem1.Pop();
  EXPECT_FALSE(scheduler.Shutdown(milliseconds(1000)));
  // This job will not hang, allowing the shutdown to be clean.
  scheduler.SetPolicy(policy);
  EXPECT_TRUE(scheduler.Schedule(bind(PushPop, ref(sem1), ref(sem2))));
  sem1.Pop();
  sem2.Push();
  EXPECT_TRUE(scheduler.Shutdown(milliseconds(100)));
}
#endif

static void LatcherMain2(TLatch<void, int> &latch) {
  latch.TransactReply(101);
}

static void LatcherMain1(TScheduler *scheduler, TLatch<void, int> &latch) {
  latch.PopRequest();
  int val;
  TLatch<void, int> sub_latch;
  if (scheduler->Schedule(bind(LatcherMain2, ref(sub_latch)))) {
    sub_latch.TransactRequest(val);
  } else {
    val = 202;
  }
  latch.PushReply(val);
}

FIXTURE(Latch) {
  int val;
  TLatch<void, int> latch;
  TScheduler scheduler(TScheduler::TPolicy(0, 2, milliseconds(100)));
  // make a request
  scheduler.Schedule(bind(LatcherMain1, &scheduler, ref(latch)));
  val = 0;
  latch.TransactRequest(val);
  EXPECT_EQ(val, 101);
  // make another request
  scheduler.Schedule(bind(LatcherMain1, &scheduler, ref(latch)));
  val = 0;
  latch.TransactRequest(val);
  EXPECT_EQ(val, 101);
}

static void LittleTicker(atomic_int &count) {
  count += 1;
}

static void LittleMain(TScheduler *scheduler, int expected, atomic_int &count) {
  for (int i = 0; i < expected; ++i) {
    scheduler->Schedule(bind(LittleTicker, ref(count)));
  }
}

FIXTURE(PermanentQuiescence) {
  const int expected = 10;
  atomic_int count;
  count = 0;
  TScheduler::TPolicy(1, 4, milliseconds(10)).RunUntilCtrlC(bind(LittleMain, _1, expected, ref(count)));
  EXPECT_EQ(count.load(), expected);
  EXPECT_TRUE(IsShuttingDown());
}

/* Job cancellation (#462): a queued-but-never-taken job can be removed so
   it neither runs late against dying state nor goes silently missing; a
   job a worker already took reports as such, telling the owner to join
   its exited-latch instead. */
FIXTURE(CancelQueuedJob) {
  const TScheduler::TPolicy policy(1, 1, milliseconds(10));
  TScheduler scheduler(policy);
  TEventSemaphore entered, release, cancelled_job_ran;
  /* Occupy the single worker so the next job must queue. */
  EXPECT_TRUE(scheduler.Schedule([&entered, &release] {
    entered.Push();
    release.Pop();
  }));
  entered.Pop();
  /* Queue and cancel: the job must report cancelled exactly once and must
     never run. */
  auto handle = scheduler.ScheduleCancelable([&cancelled_job_ran] {
    cancelled_job_ran.Push();
  });
  EXPECT_TRUE(handle != nullptr);
  EXPECT_TRUE(scheduler.Cancel(handle));
  EXPECT_FALSE(scheduler.Cancel(handle));
  release.Push();
  EXPECT_TRUE(scheduler.Shutdown(milliseconds(2000)));
  EXPECT_FALSE(cancelled_job_ran.GetFd().IsReadable());
}

FIXTURE(CancelTakenJobFails) {
  const TScheduler::TPolicy policy(1, 1, milliseconds(10));
  TScheduler scheduler(policy);
  TEventSemaphore entered, release;
  auto handle = scheduler.ScheduleCancelable([&entered, &release] {
    entered.Push();
    release.Pop();
  });
  EXPECT_TRUE(handle != nullptr);
  entered.Pop();
  /* The worker took the job; too late to cancel -- the owner must join. */
  EXPECT_FALSE(scheduler.Cancel(handle));
  release.Push();
  EXPECT_TRUE(scheduler.Shutdown(milliseconds(2000)));
}

FIXTURE(CancelRefusedAndNullHandles) {
  /* A scheduler with no workers refuses the job: null handle, and
     cancelling a null handle is a no-op that reports not-cancelled. */
  TScheduler scheduler;
  auto handle = scheduler.ScheduleCancelable([] {});
  EXPECT_TRUE(handle == nullptr);
  EXPECT_FALSE(scheduler.Cancel(handle));
}
