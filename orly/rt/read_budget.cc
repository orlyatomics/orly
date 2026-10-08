/* <orly/rt/read_budget.cc>

   Implements <orly/rt/read_budget.h>.

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

#include <orly/rt/read_budget.h>

#include <sstream>

#include <orly/indy/fiber/fiber.h>
#include <orly/server/read_too_large.h>

using namespace Orly::Rt;

namespace {

  /* The budget of code that runs off any fiber, which therefore can't change threads. */
  thread_local TReadBudget *NoFrameBudget = nullptr;

  /* Where the running code's budget lives: its fiber's frame, or this thread. */
  TReadBudget **GetSlot() {
    Orly::Indy::Fiber::TFrame *frame = Orly::Indy::Fiber::TFrame::LocalFrame;
    return frame ? &frame->ReadBudget : &NoFrameBudget;
  }

}  // namespace

void TReadBudget::CheckAhead(size_t steps, size_t bytes) const {
  /* Compared by subtraction, so that a huge estimate can't wrap around to a small one. */
  const bool over_steps = Steps > MaxSteps || steps > MaxSteps - Steps;
  const bool over_bytes = Bytes > MaxBytes || bytes > MaxBytes - Bytes;
  if (over_steps || over_bytes) {
    OnOverBudget(over_steps);
  }
}

void TReadBudget::OnOverBudget(bool steps) const {
  std::ostringstream msg;
  msg << "read too large: ";
  if (steps) {
    msg << "it took more than " << MaxSteps << " steps (--read_budget_steps)";
  } else {
    msg << "it built more than " << MaxBytes << " bytes of values while it ran (--read_budget_mb)";
  }
  msg << "; retrying won't help, read a narrower range or raise the budget";
  throw Orly::Server::TReadTooLarge(msg.str());
}

TReadBudgetScope::TReadBudgetScope(TReadBudget *budget)
    : Slot(GetSlot()), Prev(*Slot) {
  *Slot = budget;
}

TReadBudgetScope::~TReadBudgetScope() {
  *Slot = Prev;
}

TReadBudget *Orly::Rt::GetCurrentReadBudget() {
  return *GetSlot();
}
