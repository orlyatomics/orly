/* <test/runner.h>

   Runs a fixture in unit test program.

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

#pragma once

#include <atomic>

#include <base/assert_true.h>
#include <base/class_traits.h>
#include <base/test/app.h>

namespace Test {

  class TFixture;

  class TRunner : public TApp::TRunner {
    NO_COPY(TRunner);
    public:

    class TExpect {
      NO_COPY(TExpect);
      public:

      virtual ~TExpect();

      virtual operator bool() const = 0;

      protected:

      TExpect() {}

      void PreDtor();
    };

    TRunner(TApp *app, const TFixture *fixture);

    virtual ~TRunner();

    virtual operator bool() const;

    void Run();

    static TRunner *GetRunner() {
      return Base::AssertTrue(Runner);
    }

    private:

    void OnExpectDtor(const TExpect *expect);

    const TFixture *Fixture;

    /* The fixture's running pass/fail state. A multi-threaded fixture fires
       `EXPECT_*` from worker threads, so every `TExpect` destructor folds its
       result into this flag (see OnExpectDtor) from whatever thread it runs
       on. The flag only ever moves true -> false, but the concurrent
       read-modify-writes are still a data race on a plain bool (flagged by
       ThreadSanitizer, issue #184). Making it atomic turns the fold into an
       idempotent relaxed store and de-noises every multi-threaded test. */
    std::atomic<bool> Pass;

    static TRunner *Runner;
  };

}
