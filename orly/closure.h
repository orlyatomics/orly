/* <orly/closure.h>

   `TClosure` is a name-keyed map of `Atom::TCore` values used to
   bundle method-call arguments. Comes with a `TWalker` that
   optimises sequential access through the underlying ordered map
   (rewinds when you index backwards). Used by the WS protocol and
   replay machinery to pack / unpack RPC arguments.

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
#include <map>
#include <memory>
#include <string>

#include <base/thrower.h>
#include <base/io/binary_input_stream.h>
#include <base/io/binary_output_stream.h>
#include <orly/atom/kit2.h>
#include <orly/atom/suprena.h>
#include <orly/sabot/to_native.h>

namespace Orly {

  class TClosure {
    public:

    DEFINE_ERROR(TUnknownArgName, std::invalid_argument, "the closure contains no argument by the given name");

    /* Convenience. */
    using TCoreByName = std::map<std::string, Atom::TCore>;

    /* Optimizes access to a map so that sequential access (which is the norm) is very efficient. */
    class TWalker final {
      public:

      /* Caches the pointer. */
      TWalker(const TClosure *closure)
          : Closure(closure) {
        assert(closure);
        Rewind();
      }

      /* Return an iterator to the nth element in the map. */
      TCoreByName::const_iterator operator[](size_t elem_idx) const {
        assert(elem_idx < Closure->CoreByName.size());
        if (ElemIdx > elem_idx) {
          Rewind();
        }
        while (ElemIdx < elem_idx) {
          ++Iter;
          ++ElemIdx;
        }
        return Iter;
      }

      /* The closure whose map we're walking. */
      const TClosure *GetClosure() const {
        return Closure;
      }

      private:

      /* Send Iter and ElemIdx back to the beginning of the map. */
      void Rewind() const {
        Iter = Closure->CoreByName.begin();
        ElemIdx = 0;
      }

      /* See accessor. */
      const TClosure *Closure;

      /* Our current position in the map. */
      mutable TCoreByName::const_iterator Iter;

      /* Our current index into the map. */
      mutable size_t ElemIdx;

    };  // TWalker

    /* The type sabot we use when constructing our fake record object. */
    class TType final
        : public Sabot::Type::TRecord {
      public:

      /* The base for record-type pins. */
      using TPinBase = Sabot::Type::TRecord::TPin;

      /* Our final version of TPinBase. */
      class TPin final
          : public TPinBase {
        public:

        /* Initialzies the base and the walker. */
        TPin(const TType *type)
            : TPinBase(type), Walker(type->Closure) {}

        /* See TPinBase. */
        virtual TAny *NewElem(size_t elem_idx, std::string &name, void *type_alloc) const override;

        /* See TPinBase. */
        virtual TAny *NewElem(size_t elem_idx, void *&out_field_name_state, void *field_name_state_alloc, void *type_alloc) const override;

        /* See TPinBase. */
        virtual TAny *NewElem(size_t elem_idx, void *type_alloc) const override;

        private:

        /* Walks the map for us. */
        TWalker Walker;

      };  // TPin

      /* Caches the pointer. */
      TType(const TClosure *closure)
          : Closure(closure) {
        assert(closure);
      }

      /* See Sabot::Type::TNAry. */
      virtual size_t GetElemCount() const override;

      /* See Sabot::Type::TRecord. */
      virtual TPinBase *Pin(void *alloc) const override;

      private:

      /* The closure we wrap. */
      const TClosure *Closure;

    };  // TType

    /* The state sabot we use when constructing our fake record object. */
    class TState final
        : public Sabot::State::TRecord {
      public:

      /* The base for record-state pins. */
      using TPinBase = Sabot::State::TRecord::TPin;

      /* Our final version of TPinBase. */
      class TPin final
          : public TPinBase {
        public:

        /* Initialzies the base and the walker. */
        TPin(const TState *state)
            : TPinBase(state), Walker(state->Closure) {}

        private:

        /* See TPinBase. */
        virtual TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

        /* Walks the map for us. */
        TWalker Walker;

      };  // TPin

      /* Caches the pointer. */
      TState(const TClosure *closure)
          : Closure(closure) {}

      /* See Sabot::State::TRecord. */
      virtual Sabot::Type::TRecord *GetRecordType(void *type_alloc) const override;

      /* See Sabot::State::TArrayOfSingleStates. */
      virtual TPinBase *Pin(void *alloc) const override;

      private:

      /* The closure we wrap. */
      const TClosure *Closure;

    };  // TState

    TClosure() {
      Reset();
    }

    explicit TClosure(const std::string &method_name);

    template <typename... TPairs>
    TClosure(const std::string &method_name, const TPairs &... pairs)
        : TClosure(method_name) {
      ArgsAdder<TPairs...>::AddArgs(this, pairs...);
    }

    void AddArgBySabot(const std::string &name, const Sabot::State::TAny *state) {
      AddCore(name, Atom::TCore(Arena.get(), state));
    }

    template <typename TVal>
    TVal &GetArg(const std::string &name, TVal &out) const {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TAny::TWrapper state(GetCore(name)->NewState(Arena.get(), state_alloc));
      Sabot::ToNative(*state, out);
      return out;
    }

    const std::shared_ptr<Atom::TSuprena> &GetArena() const {
      return Arena;
    }

    size_t GetArgCount() const {
      return CoreByName.size();
    }

    const TCoreByName &GetCoreByName() const {
      return CoreByName;
    }

    const std::string &GetMethodName() const {
      return MethodName;
    }

    void Read(Io::TBinaryInputStream &strm);

    void Reset();

    void Write(Io::TBinaryOutputStream &strm) const;

    private:

    template <typename... TArgs>
    class ArgsAdder;

    template <typename TVal>
    bool AddArg(const std::string &name, const TVal &val) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TAny::TWrapper state(Native::State::New(val, state_alloc));
      return AddCore(name, Atom::TCore(Arena.get(), state));
    }

    bool AddCore(const std::string &name, const Atom::TCore &core);

    const Atom::TCore *GetCore(const std::string &name) const;

    const Atom::TCore *TryGetCore(const std::string &name) const;

    std::shared_ptr<Atom::TSuprena> Arena;

    std::string MethodName;

    TCoreByName CoreByName;

  };  // TClosure

  template <>
  class TClosure::ArgsAdder<> {
    public:

    static void AddArgs(TClosure *) {}

  };  // TClosure::ArgsAdder<>

  template <typename TVal, typename... TMorePairs>
  class TClosure::ArgsAdder<std::string, TVal, TMorePairs...> {
    public:

    static void AddArgs(TClosure *closure, const std::string &name, const TVal &val, const TMorePairs &... more_pairs) {
      closure->AddArg(name, val);
      ArgsAdder<TMorePairs...>::AddArgs(closure, more_pairs...);
    }

  };  // TClosure::ArgsAdder<std::string, TVal, TMorePairs...>

  /* Binary stream extractor for Orly::TClosure. */
  inline Io::TBinaryInputStream &operator>>(Io::TBinaryInputStream &strm, TClosure &that) {
    that.Read(strm);
    return strm;
  }

  /* Binary stream inserter for Orly::TClosure. */
  inline Io::TBinaryOutputStream &operator<<(Io::TBinaryOutputStream &strm, const TClosure &that) {
    that.Write(strm);
    return strm;
  }

}  // Orly