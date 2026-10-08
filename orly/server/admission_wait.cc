/* <orly/server/admission_wait.cc>

   Implements <orly/server/admission_wait.h>.

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

#include <orly/server/admission_wait.h>

#include <algorithm>

using namespace std;
using namespace chrono;
using namespace Orly::Indy;
using namespace Orly::Server;

mutex TAdmissionWait::RegistryMutex;

TAdmissionWait *TAdmissionWait::Registered = nullptr;

TAdmissionWait::TAdmissionWait(milliseconds bound)
    : Bound(bound) {
  /* lock */ {
    lock_guard<mutex> lock(RegistryMutex);
    Registered = this;
  }
  Indy::Util::TPool::SetRoomCallback(&TAdmissionWait::OnRoom);
  Thread = thread([this] { Run(); });
}

TAdmissionWait::~TAdmissionWait() {
  Close();
}

TAdmissionWait::TResult TAdmissionWait::Admit(TUpdate::TWriteAdmission &admission, size_t num_entries) {
  TResult result;
  /* Nobody waiting: try at once, without the queue's lock. A writer that slips in here just as
     the first waiter queues gets ahead of it once, which can't starve it. */
  if (!Waiting.load(memory_order_relaxed) && admission.TryAcquire(num_entries)) {
    result.Admitted = true;
    return result;
  }
  /* Only a fiber can wait: the wake reschedules its frame. */
  if (!Fiber::TFrame::LocalFrame) {
    result.Admitted = admission.TryAcquire(num_entries);
    return result;
  }
  const auto start = steady_clock::now();
  TWaiter waiter;
  waiter.Admission = &admission;
  waiter.NumEntries = num_entries;
  waiter.Deadline = start + Bound;
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    if (Closed) {
      result.Admitted = admission.TryAcquire(num_entries);
      return result;
    }
    /* First come, first served: try again only if nobody is ahead of us. */
    if (Queue.empty() && admission.TryAcquire(num_entries)) {
      result.Admitted = true;
      return result;
    }
    Queue.push_back(&waiter);
    Waiting = Queue.size();
    ++Waited;
  }
  /* The thread arms the watch for whoever is at the head, and serves the queue. */
  /* lock */ {
    lock_guard<mutex> lock(RegistryMutex);
    KickLocked(this);
  }
  waiter.Sem.Pop();
  const int outcome = waiter.Outcome.load(memory_order_acquire);
  result.Waited = true;
  result.Admitted = outcome == Granted;
  result.WaitTime = duration_cast<microseconds>(steady_clock::now() - start);
  const size_t us = static_cast<size_t>(result.WaitTime.count());
  for (size_t longest = LongestWaitUs.load(); us > longest && !LongestWaitUs.compare_exchange_weak(longest, us);) {}
  return result;
}

void TAdmissionWait::Close() {
  /* lock */ {
    lock_guard<mutex> lock(Mutex);
    if (!Closed) {
      Closed = true;
      ClosedOut += Queue.size();
      for (TWaiter *waiter: Queue) {
        Finish(*waiter, Shut);
      }
      Queue.clear();
      Waiting = 0UL;
      TUpdate::TWriteAdmission::DisarmRoomWakes();
    }
  }
  /* lock */ {
    lock_guard<mutex> lock(RegistryMutex);
    Stopping = true;
    Cv.notify_one();
    if (Registered == this) {
      Registered = nullptr;
    }
  }
  if (Thread.joinable()) {
    Thread.join();
  }
}

void TAdmissionWait::Run() {
  bool waiting = false;
  steady_clock::time_point next_deadline;
  for (;;) {
    /* lock */ {
      unique_lock<mutex> lock(RegistryMutex);
      const auto woken = [this] { return Kicked || Stopping; };
      if (waiting) {
        Cv.wait_until(lock, next_deadline, woken);
      } else {
        Cv.wait(lock, woken);
      }
      if (Stopping) {
        return;
      }
      Kicked = false;
    }
    /* lock */ {
      lock_guard<mutex> lock(Mutex);
      waiting = ServeLocked(next_deadline);
    }
  }
}

bool TAdmissionWait::ServeLocked(steady_clock::time_point &next_deadline) {
  const auto now = steady_clock::now();
  while (!Queue.empty()) {
    TWaiter &head = *Queue.front();
    if (TryAdmitLocked(head)) {
      Queue.pop_front();
      Waiting = Queue.size();
      Finish(head, Granted);
    } else if (now >= head.Deadline) {
      Queue.pop_front();
      Waiting = Queue.size();
      ++TimedOut;
      Finish(head, Expired);
    } else {
      break;
    }
  }
  Waiting = Queue.size();
  if (Queue.empty()) {
    TUpdate::TWriteAdmission::DisarmRoomWakes();
    return false;
  }
  next_deadline = Queue.front()->Deadline;
  return true;
}

bool TAdmissionWait::TryAdmitLocked(TWaiter &waiter) {
  /* Each round either admits, finds the watch already armed where this refusal needs it (so
     the try after arming has been made), or arms it and tries again. The level only moves when
     the pool's hysteresis or the other pool changes which line refused, so a few rounds settle
     it; past that, the deadline still bounds the wait. */
  for (int round = 0; round < 8; ++round) {
    if (waiter.Admission->TryAcquire(waiter.NumEntries)) {
      return true;
    }
    if (!waiter.Admission->ArmRoomWake()) {
      return false;
    }
  }
  return false;
}

void TAdmissionWait::Finish(TWaiter &waiter, TOutcome outcome) {
  waiter.Outcome.store(outcome, memory_order_release);
  waiter.Sem.Push();
}

void TAdmissionWait::OnRoom() {
  lock_guard<mutex> lock(RegistryMutex);
  if (Registered) {
    KickLocked(Registered);
  }
}

void TAdmissionWait::KickLocked(TAdmissionWait *self) {
  self->Kicked = true;
  self->Cv.notify_one();
}
