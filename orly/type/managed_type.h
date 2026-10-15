/* <orly/type/managed_type.h>

   CRTP infrastructure for type interning. `TInternedType<TFinal,
   TArgs...>` holds a static `TypeByKey` map keyed by `tuple<TArgs...>`;
   `TFinal::Get(args...)` returns the existing shared instance or
   constructs one. `TSingletonType<TFinal>` is the degenerate
   no-arguments case (used for `TBool`, `TInt`, `TAny`, etc.).
   `IMPL_INTERNED_TYPE` / `IMPL_SINGLETON_TYPE` macros emit the static
   storage definitions in each leaf's `.cc` file.

   Lookups take the interner's lock shared, so the fast runners can find
   existing types in parallel (#946 follow-up: the old recursive mutex was
   ~8% futex time across three runners). Inserting and freeing take it
   exclusive. A thread already holding it exclusive (a constructor or a
   free that reaches the same family again) re-enters without locking.
   A free that cannot get the lock does not wait -- two families freeing
   into each other could deadlock -- so it is queued and done by the next
   thread that can take the lock.

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
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <base/assert_true.h>
#include <base/hash.h>
#include <orly/type/impl.h>

#define IMPL_INTERNED_TYPE(TFinal, TArgs...) \
    template <> \
    typename ::Orly::Type::TInternedType<TFinal, TArgs>::TStore *::Orly::Type::TInternedType<TFinal, TArgs>::Store = nullptr; \
    template <> \
    thread_local int ::Orly::Type::TInternedType<TFinal, TArgs>::ExclusiveDepth = 0;

#define IMPL_UNARY_TYPE(TFinal) \
    IMPL_INTERNED_TYPE(TFinal, ::Orly::Type::TType) \
    ::Orly::Type::TType TFinal::Get(const ::Orly::Type::TType &type) { \
      return ::Orly::Type::TUnaryType<TFinal>::Get(type); \
    }

#define IMPL_SINGLETON_TYPE(TFinal) \
    template <> \
    TFinal::TPtr *::Orly::Type::TSingletonType<TFinal>::Ptr = nullptr;

namespace Orly {

  namespace Type {

    class TTypeCzar;

    template <typename TFinal, typename... TArgs>
    class TInternedType : public TType::TImpl {
      private:
      typedef std::shared_ptr<const TFinal> TPtr;
      typedef std::weak_ptr<const TFinal> TWeak;

      protected:
      typedef std::tuple<TArgs...> TKey;

      public:
      typedef std::unordered_map<TKey, TWeak> TTypeByKey;

      void Accept(const TType::TVisitor &visitor) const {
        visitor(Base::AssertTrue(dynamic_cast<const TFinal*>(this)));
      }

      protected:

      template <typename... TCompatArgs>
      static TType Get(TCompatArgs &&... args) {
        assert(Store);  // If this fails, you likely don't have an instance of TTypeCzar.
        TKey key(args...);
        if (!ExclusiveDepth) {
          {
            std::shared_lock<std::shared_mutex> lock(Store->Mutex);
            auto iter = Store->TypeByKey.find(key);
            if (iter != Store->TypeByKey.end()) {
              if (TPtr strong = iter->second.lock()) {
                return strong->AsType();
              }
            }
          }
          if (Store->NumPending.load(std::memory_order_relaxed)) {
            TryFreePending();
          }
        }
        TExclusive lock;
        TWeak &weak = Store->TypeByKey[std::move(key)];
        TPtr strong = weak.lock();
        if (!strong) {
          strong = TPtr(new TFinal(std::forward<TCompatArgs>(args)...), TInternedType::DeleteImpl);
          weak = strong;
        }
        return strong->AsType();
      }

      template <typename... TCompatArgs>
      TInternedType(TCompatArgs &&...key) : Key(std::forward<TCompatArgs>(key)...) {}

      ~TInternedType() {}

      /* The shared_ptr deleter. Frees under the exclusive lock, or queues the free when the lock
         is busy (see the header comment). */
      static void DeleteImpl(TInternedType *impl) {
        assert(impl);
        if (!Store) {
          delete impl;
          return;
        }
        if (ExclusiveDepth) {
          TExclusive lock;
          Free(impl);
          return;
        }
        if (Store->Mutex.try_lock()) {
          TExclusive lock(std::adopt_lock);
          Free(impl);
          return;
        }
        std::lock_guard<std::mutex> lock(Store->PendingMutex);
        Store->Pending.push_back(impl);
        Store->NumPending.fetch_add(1, std::memory_order_relaxed);
      }

      const TKey &GetKey() const {
        return Key;
      }

      private:

      /* Interner storage */
      struct TStore {
        std::shared_mutex Mutex;
        TTypeByKey TypeByKey;
        /* Frees that found Mutex busy. */
        std::mutex PendingMutex;
        std::vector<TInternedType *> Pending;
        std::atomic<size_t> NumPending{0};
      };

      /* Holds Mutex exclusive for this thread, re-entrantly. On the outermost release it first
         does any queued frees, which can queue more (a freed key releases its element types). */
      class TExclusive {
        public:
        TExclusive() {
          if (!ExclusiveDepth) {
            Store->Mutex.lock();
          }
          ++ExclusiveDepth;
        }
        explicit TExclusive(std::adopt_lock_t) {
          assert(!ExclusiveDepth);
          ExclusiveDepth = 1;
        }
        TExclusive(const TExclusive &) = delete;
        TExclusive &operator=(const TExclusive &) = delete;
        ~TExclusive() {
          if (ExclusiveDepth == 1) {
            FreePending();
          }
          if (!--ExclusiveDepth) {
            Store->Mutex.unlock();
          }
        }
      };

      /* Call with Mutex held exclusive. Drops impl's map entry only if the entry is still impl's:
         a queued free can run after Get has interned a new impl under the same key, and erasing
         that entry would let a second impl for the key be interned beside it. */
      static void Free(TInternedType *impl) {
        auto iter = Store->TypeByKey.find(impl->Key);
        if (iter != Store->TypeByKey.end()) {
          auto self = impl->weak_from_this();
          if (!iter->second.owner_before(self) && !self.owner_before(iter->second)) {
            Store->TypeByKey.erase(iter);
          }
        }
        delete impl;
      }

      /* Call with Mutex held exclusive. */
      static void FreePending() {
        while (Store->NumPending.load(std::memory_order_relaxed)) {
          std::vector<TInternedType *> pending;
          {
            std::lock_guard<std::mutex> lock(Store->PendingMutex);
            pending.swap(Store->Pending);
            Store->NumPending.store(0, std::memory_order_relaxed);
          }
          for (TInternedType *impl : pending) {
            Free(impl);
          }
        }
      }

      static void TryFreePending() {
        if (Store->Mutex.try_lock()) {
          TExclusive lock(std::adopt_lock);
        }
      }

      static void New() {
        assert(!Store);
        Store = new TStore();
      }

      /* Queued frees still pending here are dropped, as a busy free always was before. */
      static void Delete() {
        assert(Store);
        TStore *store = std::exchange(Store, nullptr);
        delete store;
      }

      static TStore *Store;

      /* How many TExclusive this thread holds on Store->Mutex. */
      static thread_local int ExclusiveDepth;

      TKey Key;

      friend class ::Orly::Type::TTypeCzar;

    };  // TInternedType<TFinal, TArgs...>

    template <typename TFinal>
    class TSingletonType : public TType::TImpl {
      public:
      typedef std::shared_ptr<const TFinal> TPtr;

      /* The type czar owns the singleton for as long as types exist, so hand out an unowned
         reference: no reference count to bump (#798). */
      static TType Get() {
        assert(Ptr);  // If this fails, you likely don't have an instance of TTypeCzar.
        return TType::TImpl::Unowned(Ptr->get());
      }

      void Accept(const TType::TVisitor &visitor) const {
        assert(Ptr);
        visitor(Ptr->get());
      }

      protected:
      TSingletonType() {}

      ~TSingletonType() {}

      static TPtr *Ptr;

      private:

      static void New() {
        assert(!Ptr);
        Ptr = new TPtr(new TFinal());
      }

      static void Delete() {
        assert(Ptr);
        delete Ptr;
        Ptr = nullptr;
      }

      friend class ::Orly::Type::TTypeCzar;

    };  // TSingletonType<TFinal>

    template <typename TFinal>
    class TUnaryType : public TInternedType<TFinal, Type::TType> {
      public:

      const TType &GetElem() const {
        //NOTE: The this-> really shuoldn't be necessary...
        return std::get<0>(this->GetKey());
      }

      protected:

      static TType Get(const Type::TType &type) {
        return TInternedType<TFinal, Type::TType>::Get(type);
      }

      TUnaryType(const Type::TType &elem) : TInternedType<TFinal, Type::TType>(elem) {}

    };  // TUnaryType<TFinal>

  }  // Type

}  // Orly
