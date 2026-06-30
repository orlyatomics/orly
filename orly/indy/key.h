/* <orly/indy/key.h>

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

#include <orly/atom/kit2.h>
#include <orly/sabot/get_hash.h>
#include <orly/sabot/order_states.h>
#include <orly/sabot/state_dumper.h>

namespace Orly {

  namespace Indy {

    class TKey {
      public:

      explicit inline TKey(Atom::TCore::TArena *arena = nullptr);

      explicit inline TKey(const Atom::TCore &core, Atom::TCore::TArena *arena = nullptr);

      inline TKey(const TKey &that);

      inline TKey(TKey &&that);

      template <typename TVal>
      inline TKey(const TVal &val, Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc);

      /* Copy from the given key into my arena */
      inline TKey(Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc, const TKey &that);

      /* Construct from sabot. */
      inline TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *state);

      /* Concat tuples */
      inline TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *lhs, const Sabot::State::TAny *rhs);

      inline TKey &operator=(TKey &&that);

      inline TKey &operator=(const TKey &that);

      inline bool operator==(const TKey &that) const;

      inline bool operator!=(const TKey &that) const;

      inline bool operator<(const TKey &that) const;

      inline bool operator<=(const TKey &that) const;

      inline bool operator>(const TKey &that) const;

      inline bool operator>=(const TKey &that) const;

      inline Atom::TComparison Compare(const TKey &that) const;

      inline void Dump(std::ostream &strm) const;

      inline Atom::TCore::TArena *GetArena() const;

      inline const Atom::TCore &GetCore() const;

      inline Atom::TCore &GetCore();

      inline Sabot::State::TAny *GetState(void *state_alloc) const;

      inline size_t GetHash() const;

      static inline bool EqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      static inline bool TupleEqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      static inline bool NeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      static inline bool TupleNeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      static inline Atom::TComparison Compare(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      private:

      Atom::TCore::TArena *Arena;

      Atom::TCore Core;

      mutable bool HashIsCached;
      mutable size_t CachedHash;

    };  // TKey

    class TIndexKey {
      public:

      TIndexKey() {}

      TIndexKey(const Base::TUuid &index_id, const TKey &key)
          : IndexId(index_id),
            Key(key) {}

      inline const Base::TUuid &GetIndexId() const {
        return IndexId;
      }

      inline const TKey &GetKey() const {
        return Key;
      }

      inline TKey &GetKey() {
        return Key;
      }

      inline size_t GetHash() const {
        return IndexId.GetHash() ^ Key.GetHash();
      }

      bool operator<(const TIndexKey &that) const {
        Atom::TComparison comp = Atom::CompareOrdered(IndexId, that.IndexId);
        switch (comp) {
          case Atom::TComparison::Lt: {
            return true;
          }
          case Atom::TComparison::Eq: {
            return Key < that.Key;
          }
          case Atom::TComparison::Gt: {
            return false;
          }
          case Atom::TComparison::Ne: {
            throw std::logic_error("CompareOrdered should not return Ne");
          }
        }
        throw;
      }

      bool operator==(const TIndexKey &that) const {
        return IndexId == that.IndexId && Key == that.Key;
      }

      bool operator!=(const TIndexKey &that) const {
        return IndexId != that.IndexId || Key != that.Key;
      }

      private:

      Base::TUuid IndexId;

      TKey Key;

    };  // TIndexKey

    /* A standard stream inserter for Orly::Indy::TKey. */
    inline std::ostream &operator<<(std::ostream &strm, const TKey &that) {
      that.Dump(strm);
      return strm;
    }

  }  // Indy

}  // Orly

namespace std {

  /* A standard hasher for Orly::Indy::TKey. */
  template <>
  struct hash<Orly::Indy::TKey> {
    typedef size_t result_type;
    typedef Orly::Indy::TKey argument_type;
    size_t operator()(const Orly::Indy::TKey &that) const {
      return that.GetHash();
    }
  };

  /* A standard hasher for Orly::Indy::TIndexKey. */
  template <>
  struct hash<Orly::Indy::TIndexKey> {
    typedef size_t result_type;
    typedef Orly::Indy::TIndexKey argument_type;
    size_t operator()(const Orly::Indy::TIndexKey &that) const {
      return that.GetHash();
    }
  };

}  // std

namespace Orly {

  namespace Indy {

    /* Inline */

    inline TKey::TKey(Atom::TCore::TArena *arena)
        : Arena(arena),
          HashIsCached(false),
          CachedHash(0UL) {}

    inline TKey::TKey(const Atom::TCore &core, Atom::TCore::TArena *arena)
        : Arena(arena),
          Core(core),
          HashIsCached(false),
          CachedHash(0UL) {}

    template <>
    inline TKey::TKey(const Atom::TCore &/*that*/, Atom::TCore::TExtensibleArena */*fast_arena*/, void */*state_alloc*/) = delete;

    template <typename TVal>
    inline TKey::TKey(const TVal &val, Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc)
        : Arena(Base::AssertTrue(fast_arena)),
          Core(val, fast_arena, state_alloc),
          HashIsCached(false),
          CachedHash(0UL) {}

    inline TKey::TKey(Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc, const TKey &that)
        : Arena(Base::AssertTrue(fast_arena)),
          Core(fast_arena, state_alloc, that.Arena, that.Core),
          HashIsCached(false),
          CachedHash(0UL) {}

    inline TKey::TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *state)
        : Arena(Base::AssertTrue(fast_arena)), Core(fast_arena, state),
          HashIsCached(false),
          CachedHash(0UL) {}

    inline TKey::TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *lhs, const Sabot::State::TAny *rhs)
        : Arena(Base::AssertTrue(fast_arena)),
          Core(fast_arena, lhs, rhs),
          HashIsCached(false),
          CachedHash(0UL) {}

    inline TKey::TKey(const TKey &that) : Arena(that.Arena), Core(that.Core), HashIsCached(that.HashIsCached), CachedHash(that.CachedHash) {}

    inline TKey::TKey(TKey &&that) : Arena(that.Arena), Core(that.Core), HashIsCached(that.HashIsCached), CachedHash(that.CachedHash) {}

    inline TKey &TKey::operator=(const TKey &that) {
      Arena = that.Arena;
      Core = that.Core;
      HashIsCached = that.HashIsCached;
      CachedHash = that.CachedHash;
      return *this;
    }

    inline TKey &TKey::operator=(TKey &&that) {
      std::swap(Arena, that.Arena);
      std::swap(Core, that.Core);
      std::swap(HashIsCached, that.HashIsCached);
      std::swap(CachedHash, that.CachedHash);
      return *this;
    }

    inline bool TKey::EqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      size_t lhs_hash, rhs_hash;
      if (lhs.TryGetQuickHash(lhs_hash) && rhs.TryGetQuickHash(rhs_hash)) {
        return lhs_hash == rhs_hash && Atom::IsEq(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
      }
      return Atom::IsEq(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
    }

    inline bool TKey::TupleEqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      return lhs.ForceGetIndirectHash() == rhs.ForceGetIndirectHash() && Atom::IsEq(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
    }

    inline bool TKey::operator==(const TKey &that) const {
      return EqEq(Core, Arena, that.Core, that.Arena);
    }

    inline bool TKey::NeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      size_t lhs_hash, rhs_hash;
      return (lhs.TryGetQuickHash(lhs_hash) && rhs.TryGetQuickHash(rhs_hash) && (lhs_hash != rhs_hash)) || Atom::IsNe(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
    }

    inline bool TKey::TupleNeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      return (lhs.ForceGetIndirectHash() != rhs.ForceGetIndirectHash()) ||
              Atom::IsNe(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
    }

    inline bool TKey::operator!=(const TKey &that) const {
      return NeEq(Core, Arena, that.Core, that.Arena);
    }

    inline bool TKey::operator<(const TKey &that) const {
      return Atom::IsLt(Compare(that));
    }

    inline bool TKey::operator<=(const TKey &that) const {
      return Atom::IsLe(Compare(that));
    }

    inline bool TKey::operator>(const TKey &that) const {
      return Atom::IsGt(Compare(that));
    }

    inline bool TKey::operator>=(const TKey &that) const {
      return Atom::IsGe(Compare(that));
    }

    inline Atom::TComparison TKey::Compare(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      Atom::TComparison comp;
      if (lhs_arena && rhs_arena && lhs.TryQuickOrderComparison(lhs_arena, rhs, rhs_arena, comp)) {
        return comp;
      }
      void *lhs_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
      void *rhs_state_alloc = reinterpret_cast<uint8_t *>(lhs_state_alloc) + Sabot::State::GetMaxStateSize();
      return Sabot::OrderStates(*Sabot::State::TAny::TWrapper(lhs.NewState(lhs_arena, lhs_state_alloc)),
                                *Sabot::State::TAny::TWrapper(rhs.NewState(rhs_arena, rhs_state_alloc)));
    }

    inline Atom::TComparison TKey::Compare(const TKey &that) const {
      return Compare(Core, Arena, that.Core, that.Arena);
    }

    inline void TKey::Dump(std::ostream &strm) const {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::State::TAny::TWrapper(GetState(state_alloc))->Accept(Sabot::TStateDumper(strm));
    }

    inline Atom::TCore::TArena *TKey::GetArena() const {
      return Arena;
    }

    inline const Atom::TCore &TKey::GetCore() const {
      return Core;
    }

    inline Atom::TCore &TKey::GetCore() {
      return Core;
    }

    inline Sabot::State::TAny *TKey::GetState(void *state_alloc) const {
      return Core.NewState(Arena, state_alloc);
    }

    inline size_t TKey::GetHash() const {
      assert(Arena);
      if (!HashIsCached) {
        if (!Core.TryGetQuickHash(CachedHash)) {
          void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
          CachedHash = Sabot::GetHash(*Sabot::State::TAny::TWrapper(Core.NewState(Arena, state_alloc)));
        }
        HashIsCached = true;
      }
      return CachedHash;
    }

  }  // Indy

}  // Orly