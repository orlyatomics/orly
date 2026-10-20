/* <orly/code_gen/variant_parity_test/rec.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated header O2i1ns4name.h for
   rec_t, the record payload <{.name: str, .n: int}>, from the package in
   orly/code_gen/variant_parity_test/parity.orly. Committed verbatim except
   that the <orly/rt/objects/*.h> includes point at the co-located frozen
   copies, so orly/code_gen/variant_parity.test.cc can compile it without a
   build-time orlyc artifact. To refresh after an emitter change, compile
   parity.orly with orlyc and copy the four headers back the same way. */
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

      class TObjO2i1ns4name : public TObj {
        public:
        TObjO2i1ns4name() {}

        TObjO2i1ns4name(const TDynamicMembers &m) : Vn(Sabot::AsNative<int64_t>(*Sabot::State::TAny::TWrapper(m.at("n").GetState(alloca(Sabot::State::GetMaxStateSize()))))), Vname(Sabot::AsNative<std::string>(*Sabot::State::TAny::TWrapper(m.at("name").GetState(alloca(Sabot::State::GetMaxStateSize()))))) {}
        TObjO2i1ns4name(const int64_t &vvn, const std::string &vvname) : Vn(vvn), Vname(vvname) {}
        TObjO2i1ns4name(const TObjO2i1ns4name &that) : Vn(that.Vn), Vname(that.Vname) {}

        Var::TVar AsVar() const final {
          assert(this);
          return Var::TVar::Obj(std::unordered_map<std::string, Var::TVar>{{"n", Var::TVar(Vn)}, {"name", Var::TVar(Vname)}});
        }

        size_t GetHash() const {
          assert(this);
          size_t hash = 0;
          hash = Base::CombineHashes(hash, std::hash<int64_t>()(Vn));
          hash = Base::CombineHashes(hash, std::hash<std::string>()(Vname));
          return hash;
        }

        bool EqEq(const TObjO2i1ns4name &that) const {
          assert(this);
          assert(&that);
          return Rt::And(Rt::EqEq(Vn, that.Vn), Rt::And(Rt::EqEq(Vname, that.Vname), true));
        }


        bool Match(const TObjO2i1ns4name &that) const {
          assert(this);
          assert(&that);
          return Rt::Match(Vn, that.Vn) && Rt::Match(Vname, that.Vname);
        }
        bool MatchLess(const TObjO2i1ns4name &that) const {
          assert(this);
          assert(&that);
          return (Rt::MatchLess(Vn, that.Vn) || (Rt::Match(Vn, that.Vn) && (Rt::MatchLess(Vname, that.Vname))));
        }

        bool Neq(const TObjO2i1ns4name &that) const {
          assert(this);
          assert(&that);
          return Rt::Or(Rt::Neq(Vn, that.Vn), Rt::Or(Rt::Neq(Vname, that.Vname), false));
        }

        int64_t GetVn() const {
          assert(this);
          return Vn;
        }
        std::string GetVname() const {
          assert(this);
          return Vname;
        }

        private:
        typedef int64_t TVTVn;
        int64_t Vn;
        typedef std::string TVTVname;
        std::string Vname;
        friend class Orly::Sabot::TToNativeVisitor<TObjO2i1ns4name>;
      }; // TObjO2i1ns4name

    } // Objects

    template <>
    struct EqEqStruct<Objects::TObjO2i1ns4name, Objects::TObjO2i1ns4name> {
      static bool Do(const Objects::TObjO2i1ns4name &lhs, const Objects::TObjO2i1ns4name &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct<Objects::TObjO2i1ns4name, Objects::TObjO2i1ns4name>

    template <>
    struct NeqStruct<Objects::TObjO2i1ns4name, Objects::TObjO2i1ns4name> {
      static bool Do(const Objects::TObjO2i1ns4name &lhs, const Objects::TObjO2i1ns4name &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct<Objects::TObjO2i1ns4name, Objects::TObjO2i1ns4name>

    template <>
    inline bool Match(const Objects::TObjO2i1ns4name &lhs, const Objects::TObjO2i1ns4name &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Objects::TObjO2i1ns4name &lhs, const Objects::TObjO2i1ns4name &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Objects::TObjO2i1ns4name> {

      static TType GetType() {
        return TObj::Get({{"n", Orly::Type::TInt::Get()}, {"name", Orly::Type::TStr::Get()}});
      }

    };

  } // Orly

} // Type

namespace std {
  template<>
  struct hash<Orly::Rt::Objects::TObjO2i1ns4name> {

    typedef size_t return_type;
    typedef Orly::Rt::Objects::TObjO2i1ns4name argument_type;

    size_t operator()(const argument_type &obj) const {
      return  obj.GetHash();
    }

  }; // hash<Orly::Rt::Objects::TObjO2i1ns4name>
} // std
RECORD_ELEM_WITH_FIELD_NAME(Orly::Rt::Objects::TObjO2i1ns4name, Orly::Rt::Objects::TObjO2i1ns4name::TVTVn, Vn, n);
RECORD_ELEM_WITH_FIELD_NAME(Orly::Rt::Objects::TObjO2i1ns4name, Orly::Rt::Objects::TObjO2i1ns4name::TVTVname, Vname, name);
