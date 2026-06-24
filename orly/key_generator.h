/* <orly/key_generator.h>

   `TKeyCursor` -- abstract iterator over database keys. Subclasses
   produce keys lazily for the various index-walking operators
   (`keys <[...]>`, `*<[...]>::(T)`, etc.). Holds the arena that
   the keys' sabot states are allocated against.

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

#include <memory>

#include <base/likely.h>
#include <orly/atom/kit2.h>
#include <orly/atom/suprena.h>
#include <orly/context_base.h>
#include <orly/rt/generator.h>

namespace Orly {

  /* TODO */
  class TKeyCursor {
    NO_COPY(TKeyCursor);
    public:

    /* TODO */
    virtual ~TKeyCursor() {}

    /* TODO */
    virtual operator bool() const = 0;

    /* TODO */
    virtual const Indy::TKey &operator*() const = 0;

    /* TODO */
    virtual const Indy::TKey *operator->() const = 0;

    /* TODO */
    virtual TKeyCursor &operator++() = 0;

    /* TODO */
    inline Atom::TCore::TArena *GetArena() {
      return Arena;
    }

    protected:

    /* TODO */
    TKeyCursor(Atom::TCore::TExtensibleArena *arena) : Arena(arena) {}

    /* TODO */
    Atom::TCore::TExtensibleArena *Arena;

  };  // TKeyCursor

  namespace L0 {

    /* TODO */
    class TPackageContext {
      NO_COPY(TPackageContext);
      public:

      /* TODO */
      virtual ~TPackageContext() {}

      /* TODO */
      virtual TKeyCursor *NewKeyCursor(TContextBase *context, const Indy::TIndexKey &pattern) const = 0;

      protected:

      TPackageContext() {}

    };  // TPackageContext

  }  // L0

  /* TODO */
  template <typename TRet>
  class TKeyGenerator
      : public Rt::TGenerator<TRet>,
        public std::enable_shared_from_this<TKeyGenerator<TRet>> {
    NO_COPY(TKeyGenerator);
    public:

    /* TODO */
    typedef std::shared_ptr<const TKeyGenerator> TPtr;

    //TODO: CODY: This cursor needs a rewrite. For now it is roughly copied from what was generated, as that seemed to work.
    class TCursor
        : public Base::TIter<const TRet> {
      public:

      /* TODO */
      typedef const TRet TVal;

      /* TODO */
      TCursor(TKeyGenerator::TPtr &ptr)
          : Cached(false), Valid(false), Item(0),
            /*Iter(&ptr->GetContext(), ptr->GetStart()),*/
            Iter(ptr->PackageContext->NewKeyCursor(&ptr->GetContext(), ptr->GetStart())),
            Ptr(ptr) {}

      /* TODO */
      TCursor(const TKeyGenerator::TPtr &ptr)
          : Cached(false), Valid(false), Item(0),
            /* Iter(&ptr->GetContext(), ptr->GetStart()), */
            Iter(ptr->PackageContext->NewKeyCursor(&ptr->GetContext(), ptr->GetStart())),
            Ptr(ptr) {}

      /* TODO */
      TCursor(const TCursor &that) = delete;

      /* TODO */
      TCursor(TCursor &&that)
          : Cached(that.Cached),
            Valid(that.Valid),
            Item(std::move(that.Item)),
            Iter(std::move(that.Iter)),
            Ptr(std::move(that.Ptr)) {
        that.Item = 0;
      }

      /* TODO */
      virtual ~TCursor() {
        delete Item;
      }

      /* TODO */
      operator bool() const {
        Refresh();
        assert(Cached);
        return Valid;
      }

      /* TODO */
      TVal &operator*() const {
        Refresh();
        assert(Cached);
        assert(Valid);
        return *Item;
      }

      /* TODO */
      Base::TIter<TVal> &operator++() {
        ++(*Iter);
        Cached = false;
        return *this;
      }

      private:

      /* TODO */
      inline void Refresh() const {
        if (!Cached) {
          Cached = true;
          if (static_cast<bool>(*Iter)) {
            void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
            if (Item) {
              *Item = Sabot::AsNative<TRet>(*Sabot::State::TAny::TWrapper((*Iter)->GetState(state_alloc)));
            } else {
              Item = new TRet(Sabot::AsNative<TRet>(*Sabot::State::TAny::TWrapper((*Iter)->GetState(state_alloc))));
            }
            Valid = true;
          } else {
            Valid = false;
          }
        }
      }

      /* TODO */
      mutable bool Cached;

      /* TODO */
      mutable bool Valid;

      /* TODO */
      mutable TRet *Item;

      /* TODO */
      mutable std::unique_ptr<Orly::TKeyCursor> Iter;

      /* TODO */
      const TKeyGenerator::TPtr Ptr;

    }; // TCursor<TKeyGenerator<TRet>>

    /* TODO */
    virtual ~TKeyGenerator() {}

    /* TODO */
    TContextBase &GetContext() {
      return Ctx;
    }

    /* TODO */
    TContextBase &GetContext() const {
      return Ctx;
    }

    /* TODO */
    const Indy::TIndexKey &GetStart() const {
      return Start;
    }

    /* TODO */
    virtual Base::TIterHolder<const TRet> NewCursor() const {
      return MakeHolder(new TCursor(this->shared_from_this()));
    }

    /* TODO */
    TKeyGenerator(L0::TPackageContext *package_context, TContextBase &ctx,  const Sabot::State::TAny *start, const Base::TUuid &index_id)
        : PackageContext(package_context),
          Ctx(ctx),
          Start(index_id, Indy::TKey(Ctx.GetArena(), start)) {}

    private:

    /* TODO */
    L0::TPackageContext *PackageContext;

    /* TODO */
    TContextBase &Ctx;

    /* TODO */
    const Indy::TIndexKey Start;

  };  // TKeyGenerator

  namespace Var {

    template <typename TVal>
    struct TRead {

      static void ThrowInvalidKeyDeref(const Indy::TKey &key) {
        THROW_ERROR(Orly::Rt::TRuntimeError) << "Cannot de-reference Key [" << key << "] which does not exist";
      }

      static inline std::pair<bool, TVal> Do(const Indy::TKey &val, const Indy::TKey &key) {
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        if (unlikely(val.GetCore().IsVoid())) {
          ThrowInvalidKeyDeref(key);
        }
        return std::make_pair(true, Sabot::AsNative<TVal>(*Sabot::State::TAny::TWrapper(val.GetState(state_alloc))));
      }

    };  // TRead<TVal>

    template <typename TVal>
    struct TRead<Rt::TOpt<TVal>> {

      static inline std::pair<bool, Rt::TOpt<TVal>> Do(const Indy::TKey &val, const Indy::TKey &/*key*/) {
        if (!val.GetCore().IsVoid()) {
          void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
          return std::make_pair(true, Sabot::AsNative<TVal>(*Sabot::State::TAny::TWrapper(val.GetState(state_alloc))));
        } else {
          /* we have a void */
          return std::make_pair(false, Rt::TOpt<TVal>());
        }
        //syslog(LOG_ERR, "TODO: support reading optional");
        //throw std::runtime_error("TODO: support reading optional");
      }

    };  // TRead<TOpt<TVal>>
  }

  namespace Rt {

    /* TODO: This is an odd place for this. Oh well. */
    template <typename TRet, typename TAddr>
    TMutable<TAddr, TRet> Read(TContextBase &ctx, const TAddr &addr, const Base::TUuid &index_id) {

      /* TODO: Copy copy copy copy copy! */
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      const Indy::TKey key(Atom::TCore(ctx.GetArena(), Sabot::State::TAny::TWrapper(Native::State::New(addr, state_alloc))), ctx.GetArena());
      auto ret = Var::TRead<TRet>::Do(ctx[Indy::TIndexKey(index_id, key)], key);
      /*
      std::cout << "Read [] = [";
      Sabot::State::TAny::TWrapper(Native::State::New(ret.second, state_alloc))->Accept(Sabot::TStateDumper(std::cout));
      std::cout << "]" << std::endl;
      */
      if  (ret.first) {
        return TMutable<TAddr, TRet>(TOpt<TAddr>(addr), ret.second);
      } else {
        return TMutable<TAddr, TRet>(TOpt<TAddr>(), ret.second);
      }
    };

    /* Commutative-upsert read (issue #151).

       Identical to Read() above except that a key which does not yet
       exist resolves to the monoid IDENTITY (the default-constructed
       TRet -- 0 for ints, false for bools, the empty set for sets)
       rather than throwing "Cannot de-reference Key ... which does not
       exist". The returned mutable keeps its address KNOWN (we still
       have the addr the caller asked about), so the downstream effect
       still registers against the right key.

       This is only ever emitted for the LHS of an absent-key-seedable
       commutative mutation (Add, Or, Xor, Union, SymmetricDiff, Min, Max,
       Intersection -- IsAbsentKeySeedRhs, see code_gen/builder.cc). For
       those, a first-write `*<[k]>::(T) OP= v` on an absent key
       auto-initialises by seeding from the RHS at fold time, instead of
       throwing -- and
       because session.cc routes the resulting TMutation through the
       deferred-commutative path (mutator preserved, RHS emitted as-is),
       two concurrent first-writers both emit {OP, v} entries that the
       read/disk fold sums, with no destructive Assign to mask either.
       The read VALUE returned here is discarded by that deferred path;
       it only matters as the expression's own result value. */
    template <typename TRet, typename TAddr>
    TMutable<TAddr, TRet> ReadOrIdentity(TContextBase &ctx, const TAddr &addr, const Base::TUuid &index_id) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      const Indy::TKey key(Atom::TCore(ctx.GetArena(), Sabot::State::TAny::TWrapper(Native::State::New(addr, state_alloc))), ctx.GetArena());
      const Indy::TKey &val = ctx[Indy::TIndexKey(index_id, key)];
      if (val.GetCore().IsVoid()) {
        /* Absent key: auto-initialise from the monoid identity. Keep the
           address known so the mutation effect still binds to this key. */
        return TMutable<TAddr, TRet>(TOpt<TAddr>(addr), TRet());
      }
      return TMutable<TAddr, TRet>(TOpt<TAddr>(addr),
          Sabot::AsNative<TRet>(*Sabot::State::TAny::TWrapper(val.GetState(state_alloc))));
    }

    /* TODO: This is an odd place for this. Oh well. */
    template <typename TAddr>
    bool Exists(TContextBase &ctx, const TAddr &addr, const Base::TUuid &index_id) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      return ctx.Exists(Indy::TIndexKey(index_id, Indy::TKey(Atom::TCore(ctx.GetArena(), Sabot::State::TAny::TWrapper(Native::State::New(addr, state_alloc))), ctx.GetArena())));
    }

  } // Rt

}  // Orly
