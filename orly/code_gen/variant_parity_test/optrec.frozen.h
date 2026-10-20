/* <orly/code_gen/variant_parity_test/optrec.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated header O1Pi1a.h for
   the record payload <{.a: int?}>, from the package in
   orly/code_gen/variant_parity_test/parity.orly. Committed verbatim except
   that the <orly/rt/objects/*.h> includes point at the co-located frozen
   copies, so orly/code_gen/variant_parity.test.cc can compile it without a
   build-time orlyc artifact. To refresh after an emitter change, compile
   parity.orly with orlyc and copy the headers back the same way. */

#pragma once

#include <cassert>

#include <base/hash.h>
#include <orly/rt/tuple.h>
#include <orly/rt/containers.h>
#include <orly/rt/obj.h>
#include <orly/type/obj.h>
#include <orly/var/impl.h>
#include <orly/var/obj.h>

namespace Orly {

  namespace Rt {

    namespace Objects {

      class TObjO1Pi1a : public TObj {
        public:
        TObjO1Pi1a() {}

        TObjO1Pi1a(const TDynamicMembers &m) : Va(Sabot::AsNative<Orly::Rt::TOpt<int64_t>>(*Sabot::State::TAny::TWrapper(m.at("a").GetState(alloca(Sabot::State::GetMaxStateSize()))))) {}
        TObjO1Pi1a(const Orly::Rt::TOpt<int64_t> &vva) : Va(vva) {}
        TObjO1Pi1a(const TObjO1Pi1a &that) : Va(that.Va) {}

        Var::TVar AsVar() const final {
          assert(this);
          return Var::TVar::Obj(std::unordered_map<std::string, Var::TVar>{{"a", Var::TVar(Va)}});
        }

        size_t GetHash() const {
          assert(this);
          size_t hash = 0;
          hash = Base::CombineHashes(hash, std::hash<Orly::Rt::TOpt<int64_t>>()(Va));
          return hash;
        }

        TOpt<bool> EqEq(const TObjO1Pi1a &that) const {
          assert(this);
          assert(&that);
          return Rt::And(Rt::EqEq(Va, that.Va), true);
        }


        bool Match(const TObjO1Pi1a &that) const {
          assert(this);
          assert(&that);
          return Rt::Match(Va, that.Va);
        }
        bool MatchLess(const TObjO1Pi1a &that) const {
          assert(this);
          assert(&that);
          return (Rt::MatchLess(Va, that.Va));
        }

        TOpt<bool> Neq(const TObjO1Pi1a &that) const {
          assert(this);
          assert(&that);
          return Rt::Or(Rt::Neq(Va, that.Va), false);
        }

        Orly::Rt::TOpt<int64_t> GetVa() const {
          assert(this);
          return Va;
        }

        private:
        typedef Orly::Rt::TOpt<int64_t> TVTVa;
        Orly::Rt::TOpt<int64_t> Va;
        friend class Orly::Sabot::TToNativeVisitor<TObjO1Pi1a>;
      }; // TObjO1Pi1a

    } // Objects

    template <>
    struct EqEqStruct<Objects::TObjO1Pi1a, Objects::TObjO1Pi1a> {
      static TOpt<bool> Do(const Objects::TObjO1Pi1a &lhs, const Objects::TObjO1Pi1a &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct<Objects::TObjO1Pi1a, Objects::TObjO1Pi1a>

    template <>
    struct NeqStruct<Objects::TObjO1Pi1a, Objects::TObjO1Pi1a> {
      static TOpt<bool> Do(const Objects::TObjO1Pi1a &lhs, const Objects::TObjO1Pi1a &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct<Objects::TObjO1Pi1a, Objects::TObjO1Pi1a>

    template <>
    inline bool Match(const Objects::TObjO1Pi1a &lhs, const Objects::TObjO1Pi1a &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Objects::TObjO1Pi1a &lhs, const Objects::TObjO1Pi1a &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Objects::TObjO1Pi1a> {

      static TType GetType() {
        return TObj::Get({{"a", Orly::Type::TOpt::Get(Orly::Type::TInt::Get())}});
      }

    };

  } // Orly

} // Type

namespace std {
  template<>
  struct hash<Orly::Rt::Objects::TObjO1Pi1a> {

    typedef size_t return_type;
    typedef Orly::Rt::Objects::TObjO1Pi1a argument_type;

    size_t operator()(const argument_type &obj) const {
      return  obj.GetHash();
    }

  }; // hash<Orly::Rt::Objects::TObjO1Pi1a>
} // std
RECORD_ELEM_WITH_FIELD_NAME(Orly::Rt::Objects::TObjO1Pi1a, Orly::Rt::Objects::TObjO1Pi1a::TVTVa, Va, a);
