/* <orly/var/mutation.h>

   The single source of truth for which mutators (`TMutator::Add`,
   `Or`, `Union`, ...) are commutative-and-associative -- i.e. safe
   to defer and re-fold. `IsDeferSafeCommutative` gates both
   `TMutation::Augment` (compose-time, PR #48) and the session-layer
   emit path (#49 phase 2). `Sub` / `Div` / `Mod` / `Exp` deliberately
   return false here.

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

#include <string>
#include <utility>

#include <orly/rt/runtime_error.h>
#include <orly/var/impl.h>
#include <orly/shared_enum.h>

namespace Orly {

  namespace Var {

      template <typename TVal>
      using TPtr = std::shared_ptr<TVal>;

      /* True iff this mutator is both commutative and associative, so that
         a sequence of `x OP a; x OP b; x OP c` on the same key can be
         safely re-folded as `x OP (a OP b OP c)` without knowing the
         current value of x at any intermediate point.

         This is the single source of truth for the defer-safe set: both
         TMutation::Augment (compose-time, in this PR's predecessor #48)
         and the session-layer emit path (#49 phase 2) gate on this. Sub,
         Div, Mod and Exp deliberately return false here -- they compose
         only via different operations than the mutator itself, which
         TMutation::Augment also rejects. */
      inline bool IsDeferSafeCommutative(TMutator mutator) {
        switch (mutator) {
          case TMutator::Add:
          case TMutator::Mult:
          case TMutator::And:
          case TMutator::Or:
          case TMutator::Xor:
          case TMutator::Union:
          case TMutator::Intersection:
          case TMutator::SymmetricDiff:
          /* min / max are commutative, associative AND idempotent (a
             semilattice) -- the cleanest possible merge ops (#213). */
          case TMutator::Min:
          case TMutator::Max:
            return true;
          case TMutator::Assign:
          case TMutator::Sub:
          case TMutator::Div:
          case TMutator::Mod:
          case TMutator::Exp:
            return false;
        }
        return false;
      }

      /* True iff a first-write `*<[k]>::(T) OP= v` to an ABSENT key should
         auto-initialise (upsert) by seeding the value DIRECTLY from the
         RHS, rather than throwing "Cannot de-reference Key ... which does
         not exist".

         This is the absent-key half of the commutative-upsert work
         (#151/#152, extended in #213). It is a subset of
         IsDeferSafeCommutative. Seeding from the RHS is correct precisely
         because folding any commutative merge op over a SINGLE element
         yields that element:

           Add          : 0 + r            == r
           Or           : false | r        == r
           Xor           : false ^ r        == r
           Union         : {} U r           == r
           SymmetricDiff : {} symdiff r     == r
           Min           : min(r)           == r      (#213)
           Max           : max(r)           == r      (#213)
           Intersection  : intersection(r)  == r      (#213 -- identity is
                            the universal set, NOT representable as a value,
                            but the singleton fold is still just r)
           Mult          : 1 * r            == r      (#213 PR2 -- identity
                            1, also not the default 0, but again the
                            singleton fold is just r)

         So the seed is taken as `var = Rhs` in TMutation::Apply
         (mutation.cc) -- NOT by default-constructing the operand -- and the
         matching LHS read is emitted as the non-throwing ReadOrIdentity
         (code_gen/builder.cc). The read's own value is discarded: the
         deferred-commutative path (server/session.cc) emits {mutator, RHS}
         and the read/disk fold seeds from the RHS, so two concurrent
         first-writers compose without a lost update.

         Excluded for now -- And's singleton fold is ALSO r, so it could
         ride this path, but it is deferred (no current demand; identity
         all-ones). A plain read of an absent key (outside a commutative
         mutation) still throws for every mutator. */
      inline bool IsAbsentKeySeedRhs(TMutator mutator) {
        switch (mutator) {
          case TMutator::Add:
          case TMutator::Or:
          case TMutator::Xor:
          case TMutator::Union:
          case TMutator::SymmetricDiff:
          case TMutator::Min:
          case TMutator::Max:
          case TMutator::Intersection:
          case TMutator::Mult:
            return true;
          case TMutator::And:
          case TMutator::Assign:
          case TMutator::Sub:
          case TMutator::Div:
          case TMutator::Mod:
          case TMutator::Exp:
            return false;
        }
        return false;
      }

      //NOTE: There is a class hierarchy in <orly/code_gen/mutation.h> which is almost identical to this one
      /* A database change */
      class TChange {
        public:

        /* Apply the change to a given TVar. The TVar __must__ be of the correct type. The var will not be updated in
           place, rather a new TVar will be created and they will be swapped. */
        virtual void Apply(Var::TVar &var) const = 0;

        /* Augment the change with a new partial change. This throws at a complete mutation (We can't do a partial
           mutation of a complete mutation), and throws if the partial change has already been made. */
        virtual void Augment(const TPtr<const TChange> &change) = 0;

        /* Returns true iff this change is a key deletion. Useful in flux for seeing if a key is tombstoned.

           NOTE: We could use a visitor here, but this is a very common case, and we already have a vtable. */
        virtual bool IsDelete() const = 0;

        /* Returns true if this is a complete mutation of the key in the database. This means either an assignment at
           the top level, a new, or a delete. */
        virtual bool IsFinal() const = 0;

        protected:
        TChange() {}
      };

      template <typename TFinal, typename TIndex>
      class TPartialChange : public TChange {
        public:

        typedef std::unordered_map<TIndex, TPtr<TChange>> TChanges;

        void Augment(const TPtr<const TChange> &change) final {
          const TFinal *that = dynamic_cast<const TFinal*>(change.get());
          if(!that) {
            //TODO(#383): Better diagnostic information so people can tell why this happened from their code.
            throw Rt::TSystemError(HERE, "Conflicting partial updates to the same key. Same key updated as different tyeps.");
          }

          for(const auto &it: that->Changes) {
            if(Changes.count(it.first)) {
              //TODO(#383): Better diagnostic information so people can tell why this happened from their code.
              throw Orly::Rt::TSystemError(HERE, "Tried to change the same portion of a value twice in one update.");
            }

            Changes.insert(it);
          }
        }

        /* Get all the partial changes */
        const TChanges &GetChanges() const {
          return Changes;
        };

        bool IsDelete() const final {
          return false;
        }

        virtual bool IsFinal() const {
          return false;
        }

        protected:
        TPartialChange(TChanges &&changes) : Changes(std::move(changes)) {}

        TChanges Changes;
      }; // TPartialChange

      class TObjChange : public TPartialChange<TObjChange, std::string> {
        public:

        static TPtr<TObjChange> New(std::string key, const TPtr<TChange> &change);

        void Apply(Var::TVar &var) const;

        private:
        TObjChange(TChanges &&changes);
      }; // TObjChange

      class TDictChange : public TPartialChange<TDictChange, Var::TVar> {
        public:

        static TPtr<TDictChange> New(const Var::TVar &key, const TPtr<TChange> &change);

        void Apply(Var::TVar &var) const;

        private:
        TDictChange(TChanges &&changes);

      }; // TDictChange

      class TAddrChange : public TPartialChange<TAddrChange, uint32_t>  {
        public:

        static TPtr<TAddrChange> New(uint32_t key, const TPtr<TChange> &change);

        void Apply(Var::TVar &var) const;

        private:
        TAddrChange(TChanges &&changes);
      }; // TAddrChange

      class TListChange : public TPartialChange<TListChange, uint64_t> {
        public:

        static TPtr<TListChange> New(uint64_t key, const TPtr<TChange> &change);

        void Apply(Var::TVar &var) const;

        private:
        TListChange(TChanges &&changes);
      }; // TListChange

      class TMutation : public TChange {
        public:

        static TPtr<TMutation> New(TMutator mutator, const Var::TVar &rhs);

        void Apply(Var::TVar &var) const final;
        void Augment(const TPtr<const TChange> &change) final;
        bool IsDelete() const final;
        bool IsFinal() const;

        /* The mutator + right-hand side that this mutation will apply.
           Exposed so the session layer (#49 phase 2) can detect defer-safe
           commutative mutations and emit them with their mutator preserved
           instead of resolving them to a value at write time. */
        TMutator GetMutator() const { return Mutator; }
        const Var::TVar &GetRhs() const { return Rhs; }

        private:
        TMutation(TMutator mutator, const Var::TVar &rhs);

        TMutator Mutator;
        Var::TVar Rhs;
      }; // TMutation

      class TDelete : public TChange {
        public:

        static TPtr<TDelete> New();

        void Apply(Var::TVar &var) const;
        void Augment(const TPtr<const TChange> &change) final;
        bool IsDelete() const final;
        bool IsFinal() const;

        private:
        TDelete();
      }; // TDelete

      class TNew : public TChange {
        public:

        static TPtr<TNew> New(const Var::TVar &val);

        void Apply(Var::TVar &var) const;
        void Augment(const TPtr<const TChange> &change) final;
        bool IsDelete() const final;
        bool IsFinal() const;

        private:
        TNew(const Var::TVar &val);
        Var::TVar Val;
      }; // TNew

  } // Var

} // Orly