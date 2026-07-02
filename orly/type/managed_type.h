/* <orly/type/managed_type.h>

   CRTP infrastructure for type interning. `TInternedType<TFinal,
   TArgs...>` holds a static `TypeByKey` map keyed by `tuple<TArgs...>`;
   `TFinal::Get(args...)` returns the existing shared instance or
   constructs one. `TSingletonType<TFinal>` is the degenerate
   no-arguments case (used for `TBool`, `TInt`, `TAny`, etc.).
   `IMPL_INTERNED_TYPE` / `IMPL_SINGLETON_TYPE` macros emit the static
   storage definitions in each leaf's `.cc` file.

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
#include <mutex>
#include <unordered_map>

#include <base/assert_true.h>
#include <base/hash.h>
#include <orly/type/impl.h>

#define IMPL_INTERNED_TYPE(TFinal, TArgs...) \
    template <> \
    std::recursive_mutex *::Orly::Type::TInternedType<TFinal, TArgs>::Mutex = nullptr; \
    template <> \
    typename ::Orly::Type::TInternedType<TFinal, TArgs>::TTypeByKey *::Orly::Type::TInternedType<TFinal, TArgs>::TypeByKey = nullptr;

#define IMPL_UNARY_TYPE(TFinal) \
    IMPL_INTERNED_TYPE(TFinal, ::Orly::Type::TType)

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
        assert(Mutex);  // If this fails, you likely don't have an instance of TTypeCzar.
        assert(TypeByKey);  // If this fails, you likely don't have an instance of TTypeCzar.
        std::lock_guard<std::recursive_mutex> lock(*Mutex);
        TWeak &weak = (*TypeByKey)[TKey(args...)];
        TPtr strong = weak.lock();
        if (!strong) {
          strong = TPtr(new TFinal(std::forward<TCompatArgs>(args)...), TInternedType::DeleteImpl);
          weak = strong;
        }
        return strong->AsType();
      }

      template <typename... TCompatArgs>
      TInternedType(TCompatArgs &&...key) : Key(std::forward<TCompatArgs>(key)...) {}

      ~TInternedType() {
        if (TypeByKey) {
          TypeByKey->erase(Key);
        }
      }

      static void DeleteImpl(TInternedType *impl) {
        assert(impl);
        if (Mutex) {
          if (Mutex->try_lock()) {
            if (impl->CanDeleteImpl()) {
              delete impl;
            }
            Mutex->unlock();
          }
        } else {
          delete impl;
        }
      }

      bool CanDeleteImpl() const {

        /* quick fix to get indy running :s */
        auto res = TypeByKey->find(Key);
        return res != TypeByKey->end() ? res->second.expired() : true;

        #if 0
        TWeak &result = TypeByKey->find(Key)->second;
        TPtr ptr = result.lock();
        return ptr == 0;
        #endif
      }

      const TKey &GetKey() const {
        return Key;
      }

      private:

      static void New() {
        assert(!Mutex);
        assert(!TypeByKey);
        Mutex = new std::recursive_mutex();
        TypeByKey = new TTypeByKey();
      }

      static void Delete() {
        assert(TypeByKey);
        assert(Mutex);
        delete TypeByKey;
        delete Mutex;
        TypeByKey = nullptr;
        Mutex = nullptr;
      }

      /* Interner storage */
      static std::recursive_mutex *Mutex;

      static TTypeByKey *TypeByKey;

      TKey Key;

      friend class ::Orly::Type::TTypeCzar;

    };  // TInternedType<TFinal, TArgs...>

    template <typename TFinal>
    class TSingletonType : public TType::TImpl {
      public:
      typedef std::shared_ptr<const TFinal> TPtr;

      static TType Get() {
        assert(Ptr);  // If this fails, you likely don't have an instance of TTypeCzar.
        return (*Ptr)->AsType();
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

      static TType Get(const Type::TType &type) {
        return TInternedType<TFinal, Type::TType>::Get(type);
      }

      const TType &GetElem() const {
        //NOTE: The this-> really shuoldn't be necessary...
        return std::get<0>(this->GetKey());
      }

      protected:

      TUnaryType(const Type::TType &elem) : TInternedType<TFinal, Type::TType>(elem) {}

    };  // TUnaryType<TFinal>

  }  // Type

}  // Orly
