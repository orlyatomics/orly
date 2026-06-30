/* <base/peekable.h>

   Template for changing a non-peekable producer into a peekable iterator.

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
#include <functional>

#include <base/class_traits.h>
#include <base/no_default_case.h>
#include <base/thrower.h>

namespace Base {

  DEFINE_ERROR(TPeekablePastEndError, std::runtime_error, "past end")

  template <typename TVal>
  class TPeekable {
    NO_COPY(TPeekable);
    public:

    typedef std::function<bool (TVal &)> TProducingFunc;

    TPeekable(const TProducingFunc &producing_func)
        : ProducingFunc(producing_func), State(Unknown) {}

    operator bool() const {
      return TryRefresh();
    }

    TVal &operator*() const {
      Refresh();
      return Val;
    }

    void operator++() {
      Refresh();
      State = Unknown;
    }

    private:

    enum TState { Unknown, Ready, Done };

    void Refresh() const {
      if (!TryRefresh()) {
        throw TPeekablePastEndError(HERE);
      }
    }

    bool TryRefresh() const {
      bool is_ready;
      switch (State) {
        case Unknown: {
          is_ready = ProducingFunc(Val);
          State = is_ready ? Ready : Done;
          break;
        }
        case Ready: {
          is_ready = true;
          break;
        }
        case Done: {
          is_ready = false;
          break;
        }
        NO_DEFAULT_CASE;
      }
      return is_ready;
    }

    TProducingFunc ProducingFunc;

    mutable TState State;

    mutable TVal Val;

  };  // TPeekable<TVal>

}  // Base
