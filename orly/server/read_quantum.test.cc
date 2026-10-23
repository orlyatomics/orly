/* <orly/server/read_quantum.test.cc>

   Unit test for <orly/server/read_quantum.h>.

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

#include <orly/server/read_quantum.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include <base/class_traits.h>
#include <base/test/kit.h>

using namespace std;
using namespace std::chrono;
using namespace Orly::Rt;
using namespace Orly::Server;

namespace {

  /* A runner and clock the test moves by hand. */
  class TFakePacer final : public TReadQuantum<TFakePacer> {
    NO_COPY(TFakePacer);
    public:

    explicit TFakePacer(microseconds quantum, microseconds slice = {})
        : TReadQuantum(quantum, slice) {}

    /* Pace as a call that has walked this many rows. */
    void Rows(size_t rows) {
      Pace(rows * TReadBudget::RowWork);
    }

    /* Something else ran on the runner: the call was switched out and back in. */
    void SwitchedOut() {
      ++Switches;
    }

    TClock::time_point Clock{};

    uint64_t Switches = 1UL;

    bool Ready = false;

    /* Is a fiber ready at normal priority? Implies Ready. */
    bool High = false;

    /* How many of the yields went to the low-priority queue. */
    size_t LowYields = 0UL;

    /* What Weigh() was told, in order. */
    vector<size_t> Weighed;

    private:

    TClock::time_point Now() const {
      return Clock;
    }

    TPlace Where() const {
      return {this, Switches};
    }

    bool OthersReady() const {
      return Ready || High;
    }

    bool HighReady() const {
      return High;
    }

    void GiveWayLow() {
      ++LowYields;
      SwitchedOut();
    }

    /* As YieldSlow() does, the runner switches to something else and back. */
    void GiveWay() {
      SwitchedOut();
    }

    void Weigh(size_t more) {
      Weighed.push_back(more);
    }

    friend class TReadQuantum<TFakePacer>;

  };  // TFakePacer

}  // namespace

/* A call doesn't yield before it has run for the quantum, even with others ready. */
FIXTURE(NoYieldBeforeQuantum) {
  TFakePacer pacer(500us);
  pacer.Ready = true;
  pacer.Rows(8);
  for (size_t i = 0; i < 49; ++i) {
    pacer.Clock += 10us;
    pacer.Rows(8 * (i + 2));
  }
  /* 490 us into the slice. */
  EXPECT_EQ(pacer.GetYields(), 0UL);
}

/* Once it has, it yields to a ready fiber, and its next slice starts when it is back. */
FIXTURE(YieldAfterQuantumWhenOthersReady) {
  TFakePacer pacer(500us);
  pacer.Ready = true;
  pacer.Rows(8);
  pacer.Clock += 499us;
  pacer.Rows(16);
  EXPECT_EQ(pacer.GetYields(), 0UL);
  pacer.Clock += 1us;
  pacer.Rows(24);
  EXPECT_EQ(pacer.GetYields(), 1UL);
  /* Back from the yield: a new slice, so no second yield until another quantum passes. */
  pacer.Clock += 100us;
  pacer.Rows(32);
  pacer.Clock += 499us;
  pacer.Rows(40);
  EXPECT_EQ(pacer.GetYields(), 1UL);
  pacer.Clock += 1us;
  pacer.Rows(48);
  EXPECT_EQ(pacer.GetYields(), 2UL);
}

/* With nothing else ready on the runner, it runs on past the quantum, and yields as soon as
   something is. */
FIXTURE(NoYieldWhenNothingReady) {
  TFakePacer pacer(500us);
  pacer.Rows(8);
  for (size_t i = 0; i < 100; ++i) {
    pacer.Clock += 100us;
    pacer.Rows(8 * (i + 2));
  }
  EXPECT_EQ(pacer.GetYields(), 0UL);
  pacer.Ready = true;
  pacer.Rows(8 * 102);
  EXPECT_EQ(pacer.GetYields(), 1UL);
}

/* Time switched out, waiting for disk say, isn't time on the runner: the slice starts again at
   the first look after the call is back. */
FIXTURE(SliceRestartsAfterExternalSwitch) {
  TFakePacer pacer(500us);
  pacer.Ready = true;
  pacer.Rows(8);
  pacer.Clock += 400us;
  pacer.Rows(16);
  /* Parked for a page for 2 ms. */
  pacer.SwitchedOut();
  pacer.Clock += 2000us;
  pacer.Rows(24);
  EXPECT_EQ(pacer.GetYields(), 0UL);
  pacer.Clock += 400us;
  pacer.Rows(32);
  EXPECT_EQ(pacer.GetYields(), 0UL);
  pacer.Clock += 100us;
  pacer.Rows(40);
  EXPECT_EQ(pacer.GetYields(), 1UL);
}

/* Moving to another runner starts a new slice too. */
FIXTURE(SliceRestartsOnAnotherRunner) {
  class TMover final : public TReadQuantum<TMover> {
    NO_COPY(TMover);
    public:
    TMover() : TReadQuantum(500us) {}
    TClock::time_point Clock{};
    int Runner = 0;
    size_t Paces = 0UL;
    void Look() {
      Pace(++Paces * 8 * TReadBudget::RowWork);
    }
    private:
    TClock::time_point Now() const { return Clock; }
    TPlace Where() const { return {&Runners[Runner], 7UL}; }
    bool OthersReady() const { return true; }
    void GiveWay() { ++Runner; }
    void Weigh(size_t) {}
    int Runners[4] = {};
    friend class TReadQuantum<TMover>;
  };
  TMover mover;
  mover.Look();
  mover.Clock += 400us;
  mover.Look();
  ++mover.Runner;
  mover.Clock += 400us;
  mover.Look();
  EXPECT_EQ(mover.GetYields(), 0UL);
  mover.Clock += 500us;
  mover.Look();
  EXPECT_EQ(mover.GetYields(), 1UL);
}

/* A quantum of 0 never yields. */
FIXTURE(ZeroNeverYields) {
  TFakePacer pacer(0us);
  pacer.Ready = true;
  for (size_t i = 0; i < 100; ++i) {
    pacer.Clock += 1ms;
    pacer.Rows(8 * (i + 1));
  }
  EXPECT_EQ(pacer.GetYields(), 0UL);
}

/* The call weighs one small statement per 32 rows, the first as soon as it has walked them and
   the rest at most once per WeighEvery, catching up all at once; a quantum of 0 still weighs. */
FIXTURE(Weighs) {
  TFakePacer pacer(0us);
  pacer.Rows(8);
  pacer.Rows(16);
  EXPECT_TRUE(pacer.Weighed.empty());
  pacer.Rows(32);
  EXPECT_TRUE(pacer.Weighed == vector<size_t>{1UL});
  /* 64 and 96 rows, too soon after the first. */
  pacer.Clock += 50us;
  pacer.Rows(64);
  pacer.Clock += 49us;
  pacer.Rows(96);
  EXPECT_EQ(pacer.Weighed.size(), 1UL);
  pacer.Clock += 1us;
  pacer.Rows(100);
  EXPECT_TRUE(pacer.Weighed == (vector<size_t>{1UL, 2UL}));
  /* Nothing new to weigh, however long it has been. */
  pacer.Clock += 1ms;
  pacer.Rows(127);
  EXPECT_EQ(pacer.Weighed.size(), 2UL);
  pacer.Rows(128);
  EXPECT_TRUE(pacer.Weighed == (vector<size_t>{1UL, 2UL, 1UL}));
}

/* Paced through a read budget, as TContext paces a call (#1015): every N rows' worth of work. */
FIXTURE(PacedByBudget) {
  TFakePacer pacer(500us);
  pacer.Ready = true;
  TReadBudget budget(0, 0);
  budget.SetPacer(&pacer, 8UL * TReadBudget::RowWork);
  for (size_t row = 0; row < 8 * 100; ++row) {
    if (row % 8 == 0) {
      pacer.Clock += 10us;
    }
    budget.Tick(TReadBudget::RowWork);
  }
  /* 100 looks 10 us apart: slices start at the first look and after each yield. */
  EXPECT_EQ(pacer.GetYields(), 1UL);
}

/* With a slice, a call drops to low priority once it has run that long in all, not counting time
   switched out, and from then on yields at every look that finds a fiber ready at normal
   priority (#1015). */
FIXTURE(DropsToLowAfterSlice) {
  TFakePacer pacer(1000us, 200us);
  pacer.Rows(8);
  pacer.Clock += 150us;
  pacer.Rows(16);
  EXPECT_FALSE(pacer.IsLow());
  /* Switched out for a long while (a disk wait): that isn't run time. */
  pacer.SwitchedOut();
  pacer.Clock += 5000us;
  pacer.Rows(24);
  pacer.Clock += 40us;
  pacer.Rows(32);
  EXPECT_FALSE(pacer.IsLow());
  pacer.Clock += 10us;
  pacer.Rows(40);
  EXPECT_TRUE(pacer.IsLow());
  /* Nothing ready: it runs on. */
  EXPECT_EQ(pacer.GetYields(), 0UL);
  /* A fiber ready at normal priority: it yields at the next look, without waiting a quantum. */
  pacer.High = true;
  pacer.Clock += 1us;
  pacer.Rows(48);
  EXPECT_EQ(pacer.LowYields, 1UL);
  pacer.High = false;
  /* Only other low-priority calls ready: they take turns of a quantum. */
  pacer.Ready = true;
  pacer.Rows(56);
  pacer.Clock += 999us;
  pacer.Rows(64);
  EXPECT_EQ(pacer.LowYields, 1UL);
  pacer.Clock += 1us;
  pacer.Rows(72);
  EXPECT_EQ(pacer.LowYields, 2UL);
  EXPECT_EQ(pacer.GetYields(), 2UL);
}

/* A call shorter than the slice stays at normal priority, and a slice of 0 never drops. */
FIXTURE(ShortCallStaysHigh) {
  TFakePacer pacer(1000us, 200us);
  pacer.High = true;
  pacer.Rows(8);
  pacer.Clock += 199us;
  pacer.Rows(16);
  EXPECT_FALSE(pacer.IsLow());
  EXPECT_EQ(pacer.GetYields(), 0UL);
  TFakePacer single(1000us);
  single.High = true;
  single.Rows(8);
  single.Clock += 5000us;
  single.Rows(16);
  EXPECT_FALSE(single.IsLow());
  EXPECT_EQ(single.LowYields, 0UL);
  EXPECT_EQ(single.GetYields(), 1UL);
}
