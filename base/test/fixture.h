/* <test/fixture.h>

   A fixture in a unit test program.

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

#include <base/class_traits.h>
#include <base/code_location.h>

#define FIXTURE(name) \
  static void name##_(); \
  static const ::Test::TFixture name##Fixture(HERE, #name, name##_); \
  static void name##_()

namespace Test {

  class TFixture {
    NO_COPY(TFixture);
    public:

    typedef void (*TFunc)();

    TFixture(
        const Base::TCodeLocation &code_location, const char *name, TFunc func);

    TFunc GetFunc() const {
      return Func;
    }

    const char *GetName() const {
      return Name;
    }

    const TFixture *GetNextFixture() const {
      return NextFixture;
    }

    static const TFixture *GetFirstFixture() {
      return FirstFixture;
    }

    private:

    Base::TCodeLocation CodeLocation;

    const char *Name;

    TFunc Func;

    mutable const TFixture *NextFixture;

    static const TFixture *FirstFixture, *LastFixture;
  };

}
