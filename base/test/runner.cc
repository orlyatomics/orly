/* <test/runner.cc>

   Implements <test/runner.h>.

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

#include <base/test/runner.h>

#include <cassert>
#include <exception>

#include <base/demangle.h>
#include <base/test/fixture.h>

using namespace std;
using namespace Base;
using namespace Test;

TRunner::TExpect::~TExpect() {}

void TRunner::TExpect::PreDtor() {
  GetRunner()->OnExpectDtor(this);
}

TRunner::TRunner(TApp *app, const TFixture *fixture)
    : TApp::TRunner(app), Fixture(AssertTrue(fixture)), Pass(true) {
  assert(!Runner);
  TApp::TLogger() << "begin " << fixture->GetName();
  Runner = this;
}

TRunner::~TRunner() {
  assert(Runner == this);
  PreDtor();
  const bool pass = Pass.load(std::memory_order_relaxed);
  TApp::TLogger(!pass)
      << "end " << Fixture->GetName()
      << "; " << (pass ? "pass" : "fail");
  Runner = 0;
}

TRunner::operator bool() const {
  return Pass.load(std::memory_order_relaxed);
}

void TRunner::Run() {
  /* Single-threaded: this runs before the fixture spawns any worker threads. */
  Pass.store(true, std::memory_order_relaxed);
  try {
    (*Fixture->GetFunc())();
  } catch (const exception &ex) {
    TApp::TLogger(true) << "exception"
                        << "(" << Demangle(typeid(ex)).get() << ")"
                        << ": " << ex.what();
    Pass = false;
  } catch (...) {
    TApp::TLogger(true) << "unknown exception";
    Pass = false;
  }
}

void TRunner::OnExpectDtor(const TExpect *expect) {
  assert(expect);
  /* Fold this expectation's result in. The flag only ever moves true -> false,
     so a failed expectation stores false and a passing one leaves it alone --
     an idempotent, order-independent update that is safe to run concurrently
     from the worker threads of a multi-threaded fixture (issue #184). */
  if (!bool(*expect)) {
    Pass.store(false, std::memory_order_relaxed);
  }
}

TRunner *TRunner::Runner = 0;

void TApp::TRunner::Run(TApp *app, const TFixture *fixture) {
  Test::TRunner(app, fixture).Run();
}
