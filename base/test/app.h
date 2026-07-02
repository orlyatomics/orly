/* <test/app.h>

   The application framework for unit test executables.

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

#include <cassert>
#include <cstddef>
#include <iostream>

#include <base/assert_true.h>
#include <base/class_traits.h>
#include <base/cmd.h>
#include <base/log.h>

namespace Test {

  class TFixture;

  class TApp final {
    NO_COPY(TApp);
    public:

    class TCmd : public Base::TLog::TCmd {
      NO_COPY(TCmd);
      public:

      TCmd(int argc, char **argv) : PrintTiming(false), VerboseMember(false) {
        Parse(argc, argv, TMeta());
      }

      private:
      class TMeta : public Base::TLog::TCmd::TMeta {
        public:
        TMeta() : Base::TLog::TCmd::TMeta("Orly Unit Test") {
          Param(&TCmd::VerboseMember, "verbose", Optional, "verbose\0v\0", "Show the results of unit tests, regardless of whether they pass or fail");
          Param(&TCmd::PrintTiming, "print_timing", Optional, "timing\0t\0", "Print execution time for each fixture");
        }
      };

      public:
      bool PrintTiming;
      bool VerboseMember;
    };

    class TLogger {
      NO_COPY(TLogger);
      public:

      TLogger(bool is_critical = false) {
        Enabled = is_critical || TApp::IsVerbose();
      }

      ~TLogger() {
        if (Enabled) {
          std::cout << std::endl;
        }
      }

      template <typename TVal>
      const TLogger &Write(const TVal &val) const {
        if (Enabled) {
          std::cout << val;
        }
        return *this;
      }

      private:

      bool Enabled;
    };

    class TRunner {
      NO_COPY(TRunner);
      public:

      virtual ~TRunner();

      virtual operator bool() const = 0;

      static void Run(TApp *app, const TFixture *fixture);

      protected:

      TRunner(TApp *app) : App(app) {}

      void PreDtor();

      TApp *App;
    };

    TApp(const TCmd &cmd)
        : Cmd(cmd), PassCount(0), FailCount(0) {
      assert(!App);
      App = this;
    }

    ~TApp() {
      assert(App == this);
      App = nullptr;
    }

    const TCmd &GetCmd() const {
      return Cmd;
    }

    void OnRunnerDtor(const TRunner *runner);

    int Run();

    static const TApp *GetApp() {
      assert(App);
      return App;
    }

    static bool IsVerbose() {
      return Verbose;
    }

    private:


    static bool Verbose;
    const TCmd &Cmd;

    /* The number of fixtures which passed and failed. */
    size_t PassCount, FailCount;

    /* See accessor. */
    static TApp *App;

  };

  /* A stream inserter for Test::TApp::TLogger targets. */
  template <typename TVal>
  const Test::TApp::TLogger &operator<<(
      const Test::TApp::TLogger &logger, const TVal &val) {
    return logger.Write(val);
  }
}