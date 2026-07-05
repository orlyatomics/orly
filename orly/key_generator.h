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
#include <optional>

#include <base/likely.h>
#include <orly/atom/kit2.h>
#include <orly/atom/suprena.h>
#include <orly/context_base.h>
#include <orly/rt/generator.h>

namespace Orly {

  class TKeyCursor {
    NO_COPY(TKeyCursor);
    public:

    virtual ~TKeyCursor() {}

    virtual operator bool() const = 0;

    virtual const Indy::TKey &operator*() const = 0;

    virtual const Indy::TKey *operator->() const = 0;

    virtual TKeyCursor &operator++() = 0;

    inline Atom::TCore::TArena *GetArena() {
      return Arena;
    }

    protected:

    TKeyCursor(Atom::TCore::TExtensibleArena *arena) : Arena(arena) {}

    Atom::TCore::TExtensibleArena *Arena;

  };  // TKeyCursor

  namespace L0 {

    class TPackageContext {
      NO_COPY(TPackageContext);
      public:

      virtual ~TPackageContext() {}

      virtual TKeyCursor *NewKeyCursor(TContextBase *context, const Indy::TIndexKey &pattern) const = 0;

      protected:

      TPackageContext() {}

    };  // TPackageContext

  }  // L0

  template <typename TRet>
  class TKeyGenerator
      : public Rt::TGenerator<TRet>,
        public std::enable_shared_from_this<TKeyGenerator<TRet>> {
    NO_COPY(TKeyGenerator);
    public:

    typedef std::shared_ptr<const TKeyGenerator> TPtr;

    /* Walks the generator's underlying key cursor, decoding each key into a
       native TRet lazily -- at most once per position, on the first bool or
       deref probe after construction or an advance (#351). */
    class TCursor final
        : public Base::TIter<const TRet> {
      NO_COPY(TCursor);
      public:

      using TVal = const TRet;

      explicit TCursor(const typename TKeyGenerator::TPtr &ptr)
          : Iter(ptr->PackageContext->NewKeyCursor(&ptr->GetContext(), ptr->GetStart())),
            Ptr(ptr) {}

      TCursor(TCursor &&that) = default;

      operator bool() const override {
        Refresh();
        return Item.has_value();
      }

      TVal &operator*() const override {
        Refresh();
        assert(Item);
        return *Item;
      }

      Base::TIter<TVal> &operator++() override {
        assert(Iter);
        ++*Iter;
        Fresh = false;
        return *this;
      }

      private:

      /* Decode the key at the current position into Item, or disengage Item
         if the underlying cursor is exhausted.  Assigns through an engaged
         Item so a decode reuses its storage. */
      void Refresh() const {
        if (!Fresh) {
          Fresh = true;
          if (*Iter) {
            void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
            Item = Sabot::AsNative<TRet>(*Sabot::State::TAny::TWrapper((*Iter)->GetState(state_alloc)));
          } else {
            Item.reset();
          }
        }
      }

      /* True once Item reflects the current position. */
      mutable bool Fresh = false;

      /* The decoded key at the current position; disengaged past the end. */
      mutable std::optional<TRet> Item;

      std::unique_ptr<Orly::TKeyCursor> Iter;

      /* Keeps the generator (and with it the package context and the start
         key's arena) alive for as long as this cursor walks it. */
      const typename TKeyGenerator::TPtr Ptr;

    };  // TCursor<TKeyGenerator<TRet>>

    virtual ~TKeyGenerator() {}

    TContextBase &GetContext() {
      return Ctx;
    }

    TContextBase &GetContext() const {
      return Ctx;
    }

    const Indy::TIndexKey &GetStart() const {
      return Start;
    }

    virtual Base::TIterHolder<const TRet> NewCursor() const {
      return MakeHolder(new TCursor(this->shared_from_this()));
    }

    TKeyGenerator(L0::TPackageContext *package_context, TContextBase &ctx,  const Sabot::State::TAny *start, const Base::TUuid &index_id)
        : PackageContext(package_context),
          Ctx(ctx),
          Start(index_id, Indy::TKey(Ctx.GetArena(), start)) {}

    private:

    L0::TPackageContext *PackageContext;

    TContextBase &Ctx;

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
      }

    };  // TRead<TOpt<TVal>>
  }

  namespace Rt {

    template <typename TRet, typename TAddr>
    TMutable<TAddr, TRet> Read(TContextBase &ctx, const TAddr &addr, const Base::TUuid &index_id) {

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
       Intersection, Mult -- IsAbsentKeySeedRhs, see code_gen/builder.cc).
       For
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

    /* Commutative-upsert LHS *address* (#151/#152). The codegen emits this for
       the left side of a defer-safe commutative `*<[k]>::(T) OP= v` mutation
       and only ever consumes its `.GetAddr()` -- the value is never read (see
       code_gen/effect.cc, which emits `.GetAddr()`, never `.GetVal()`, for a
       TMutable mutable). Unlike ReadOrIdentity it therefore does NOT read the
       current value: the mutation is recorded directly on the address and an
       absent key auto-initialises from the monoid identity at fold time, so
       the read served no purpose. Skipping it makes a commutative write O(1)
       instead of paying an O(pending-writes) TContext::operator[] lookup --
       which turned a transaction of N commutative writes into O(N^2), and a
       2x cost even on single writes. It also keeps a coordination-free `+=`
       from creating a spurious read-dependency (#49). The value slot holds the
       monoid identity and is unused. */
    template <typename TRet, typename TAddr>
    TMutable<TAddr, TRet> IdentityAddr(TContextBase &/*ctx*/, const TAddr &addr, const Base::TUuid &/*index_id*/) {
      return TMutable<TAddr, TRet>(TOpt<TAddr>(addr), TRet());
    }

    template <typename TAddr>
    bool Exists(TContextBase &ctx, const TAddr &addr, const Base::TUuid &index_id) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      return ctx.Exists(Indy::TIndexKey(index_id, Indy::TKey(Atom::TCore(ctx.GetArena(), Sabot::State::TAny::TWrapper(Native::State::New(addr, state_alloc))), ctx.GetArena())));
    }

  } // Rt

}  // Orly
