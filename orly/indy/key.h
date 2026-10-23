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

#include <atomic>

#include <orly/atom/kit2.h>
#include <orly/sabot/get_hash.h>
#include <orly/sabot/order_states.h>
#include <orly/sabot/state_dumper.h>

namespace Orly {

  namespace Indy {

    class TKey {
      public:

      explicit TKey(Atom::TCore::TArena *arena = nullptr);

      explicit TKey(const Atom::TCore &core, Atom::TCore::TArena *arena = nullptr);

      TKey(const TKey &that) = default;

      TKey(TKey &&that) noexcept = default;

      template <typename TVal>
      TKey(const TVal &val, Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc);

      /* Copy from the given key into my arena */
      TKey(Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc, const TKey &that);

      /* Construct from sabot. */
      TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *state);

      /* Concat tuples */
      TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *lhs, const Sabot::State::TAny *rhs);

      TKey &operator=(const TKey &that) = default;

      TKey &operator=(TKey &&that) noexcept = default;

      [[nodiscard]] bool operator==(const TKey &that) const;

      [[nodiscard]] bool operator!=(const TKey &that) const;

      [[nodiscard]] bool operator<(const TKey &that) const;

      [[nodiscard]] bool operator<=(const TKey &that) const;

      [[nodiscard]] bool operator>(const TKey &that) const;

      [[nodiscard]] bool operator>=(const TKey &that) const;

      [[nodiscard]] Atom::TComparison Compare(const TKey &that) const;

      void Dump(std::ostream &strm) const;

      [[nodiscard]] Atom::TCore::TArena *GetArena() const noexcept;

      [[nodiscard]] const Atom::TCore &GetCore() const noexcept;

      [[nodiscard]] Atom::TCore &GetCore() noexcept;

      [[nodiscard]] Sabot::State::TAny *GetState(void *state_alloc) const;

      [[nodiscard]] size_t GetHash() const;

      /* Throws std::invalid_argument if this is a tuple pattern with a bound member after a free one. The walkers stop early on
         the premise that frees trail, which the compiler enforces (orly/synth/db_keys_expr.cc: "all free expressions must
         appear to the right of fixed expressions"); a pattern built directly in C++ must keep to it too (#944). */
      void CheckFreesTrail() const;

      /* The number of this tuple's members that are not free, or 0 if it is not a tuple. It picks the hash table a data file
         searches for the key (Disk::TReadFile::TIndexFile::FindInHash). */
      [[nodiscard]] size_t GetNumDefined() const;

      [[nodiscard]] static bool EqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      [[nodiscard]] static bool TupleEqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      [[nodiscard]] static bool NeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      [[nodiscard]] static bool TupleNeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      [[nodiscard]] static Atom::TComparison Compare(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      /* The number of times an equality test above has found two cores in one ordered arena at
         different offsets with equal hashes, and so compared their values in full (#674). Without
         duplicate notes, only a hash collision gets there. */
      [[nodiscard]] static size_t GetNumSameArenaFallbacks() noexcept;

      private:

      /* For two keys whose hashes are equal: true iff. their values are. In an ordered arena a quick
         comparison goes by offset, and a release merge from before #666 could write two copies of
         one note there (#674), so when one arena's offsets differ, compare the values in full.
         Ordering is left to Compare(), which still goes by offset. */
      [[nodiscard]] static bool HaveEqualValues(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena);

      /* See accessor. */
      static inline std::atomic<size_t> NumSameArenaFallbacks{0UL};

      Atom::TCore::TArena *Arena = nullptr;

      Atom::TCore Core;

      mutable bool HashIsCached = false;
      mutable size_t CachedHash = 0UL;

    };  // TKey

    class TIndexKey {
      public:

      TIndexKey() = default;

      TIndexKey(const Base::TUuid &index_id, const TKey &key)
          : IndexId(index_id),
            Key(key) {}

      [[nodiscard]] const Base::TUuid &GetIndexId() const noexcept {
        return IndexId;
      }

      [[nodiscard]] const TKey &GetKey() const noexcept {
        return Key;
      }

      [[nodiscard]] TKey &GetKey() noexcept {
        return Key;
      }

      [[nodiscard]] size_t GetHash() const {
        return IndexId.GetHash() ^ Key.GetHash();
      }

      [[nodiscard]] bool operator<(const TIndexKey &that) const {
        if (IndexId < that.IndexId) {
          return true;
        }
        if (that.IndexId < IndexId) {
          return false;
        }
        return Key < that.Key;
      }

      [[nodiscard]] bool operator==(const TIndexKey &that) const = default;

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
    using result_type = size_t;
    using argument_type = Orly::Indy::TKey;
    size_t operator()(const Orly::Indy::TKey &that) const {
      return that.GetHash();
    }
  };

  /* A standard hasher for Orly::Indy::TIndexKey. */
  template <>
  struct hash<Orly::Indy::TIndexKey> {
    using result_type = size_t;
    using argument_type = Orly::Indy::TIndexKey;
    size_t operator()(const Orly::Indy::TIndexKey &that) const {
      return that.GetHash();
    }
  };

}  // std

namespace Orly {

  namespace Indy {

    /* Inline */

    inline TKey::TKey(Atom::TCore::TArena *arena)
        : Arena(arena) {}

    inline TKey::TKey(const Atom::TCore &core, Atom::TCore::TArena *arena)
        : Arena(arena),
          Core(core) {}

    template <>
    inline TKey::TKey(const Atom::TCore &/*that*/, Atom::TCore::TExtensibleArena */*fast_arena*/, void */*state_alloc*/) = delete;

    template <typename TVal>
    inline TKey::TKey(const TVal &val, Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc)
        : Arena(Base::AssertTrue(fast_arena)),
          Core(val, fast_arena, state_alloc) {}

    inline TKey::TKey(Atom::TCore::TExtensibleArena *fast_arena, void *state_alloc, const TKey &that)
        : Arena(Base::AssertTrue(fast_arena)),
          Core(fast_arena, state_alloc, that.Arena, that.Core) {}

    inline TKey::TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *state)
        : Arena(Base::AssertTrue(fast_arena)), Core(fast_arena, state) {}

    inline TKey::TKey(Atom::TCore::TExtensibleArena *fast_arena, const Sabot::State::TAny *lhs, const Sabot::State::TAny *rhs)
        : Arena(Base::AssertTrue(fast_arena)),
          Core(fast_arena, lhs, rhs) {}

    inline bool TKey::HaveEqualValues(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      Atom::TComparison comp;
      if (lhs_arena && rhs_arena && lhs.TryQuickOrderComparison(lhs_arena, rhs, rhs_arena, comp)) {
        /* Only cores in one ordered arena are compared by offset. Every other quick answer, and a
           quick Eq anywhere, is the values' answer. */
        if (Atom::IsEq(comp) || lhs_arena != rhs_arena || !lhs_arena->IsOrdered()) {
          return Atom::IsEq(comp);
        }
        NumSameArenaFallbacks.fetch_add(1UL, std::memory_order_relaxed);
      }
      void *lhs_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
      void *rhs_state_alloc = reinterpret_cast<uint8_t *>(lhs_state_alloc) + Sabot::State::GetMaxStateSize();
      return Atom::IsEq(Sabot::OrderStates(*Sabot::State::TAny::TWrapper(lhs.NewState(lhs_arena, lhs_state_alloc)),
                                           *Sabot::State::TAny::TWrapper(rhs.NewState(rhs_arena, rhs_state_alloc))));
    }

    inline size_t TKey::GetNumSameArenaFallbacks() noexcept {
      return NumSameArenaFallbacks.load(std::memory_order_relaxed);
    }

    inline bool TKey::EqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      size_t lhs_hash, rhs_hash;
      if (lhs.TryGetQuickHash(lhs_hash) && rhs.TryGetQuickHash(rhs_hash)) {
        return lhs_hash == rhs_hash && HaveEqualValues(lhs, lhs_arena, rhs, rhs_arena);
      }
      return Atom::IsEq(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
    }

    inline bool TKey::TupleEqEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      return lhs.ForceGetIndirectHash() == rhs.ForceGetIndirectHash() && HaveEqualValues(lhs, lhs_arena, rhs, rhs_arena);
    }

    inline bool TKey::operator==(const TKey &that) const {
      return EqEq(Core, Arena, that.Core, that.Arena);
    }

    inline bool TKey::NeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      size_t lhs_hash, rhs_hash;
      if (lhs.TryGetQuickHash(lhs_hash) && rhs.TryGetQuickHash(rhs_hash)) {
        return lhs_hash != rhs_hash || !HaveEqualValues(lhs, lhs_arena, rhs, rhs_arena);
      }
      return Atom::IsNe(TKey::Compare(lhs, lhs_arena, rhs, rhs_arena));
    }

    inline bool TKey::TupleNeEq(const Atom::TCore &lhs, Atom::TCore::TArena *lhs_arena, const Atom::TCore &rhs, Atom::TCore::TArena *rhs_arena) {
      return lhs.ForceGetIndirectHash() != rhs.ForceGetIndirectHash() || !HaveEqualValues(lhs, lhs_arena, rhs, rhs_arena);
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

    inline Atom::TCore::TArena *TKey::GetArena() const noexcept {
      return Arena;
    }

    inline const Atom::TCore &TKey::GetCore() const noexcept {
      return Core;
    }

    inline Atom::TCore &TKey::GetCore() noexcept {
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