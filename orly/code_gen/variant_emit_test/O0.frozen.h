/* <orly/code_gen/variant_emit_test/O0.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated empty-object record header (O0.h,
   the unit/empty record `<{}>`). Committed verbatim so the emitted-variant
   validation test does not depend on a build-time-generated artifact under
   orly/rt/objects/ (which `make clean` removes). If orly/code_gen/obj.cc's
   emitter changes, regenerate this from a real O0.h. See
   orly/code_gen/variant_emit.test.cc. */

#pragma once

#include <cassert>

#include <orly/rt/tuple.h>
#include <orly/rt/containers.h>
#include <orly/rt/obj.h>
#include <orly/type/obj.h>
#include <orly/var/impl.h>
#include <orly/var/obj.h>

namespace Orly {

  namespace Rt {

    namespace Objects {

      class TObjO0 : public TObj {
        public:
        TObjO0(const TDynamicMembers &m) {}
        TObjO0() {}
        TObjO0(const TObjO0 &that) {}

        Var::TVar AsVar() const final {
          assert(this);
          return Var::TVar::Obj(std::unordered_map<std::string, Var::TVar>{});
        }

        size_t GetHash() const {
          assert(this);
          return  0;
        }

        bool EqEq(const TObjO0 &that) const {
          assert(this);
          assert(&that);
          return true;
        }


        bool Match(const TObjO0 &that) const {
          assert(this);
          assert(&that);
          return true;
        }
        bool MatchLess(const TObjO0 &that) const {
          assert(this);
          assert(&that);
          return false;
        }

        bool Neq(const TObjO0 &that) const {
          assert(this);
          assert(&that);
          return false;
        }


        private:
        friend class Orly::Sabot::TToNativeVisitor<TObjO0>;
      }; // TObjO0

    } // Objects

    template <>
    struct EqEqStruct<Objects::TObjO0, Objects::TObjO0> {
      static bool Do(const Objects::TObjO0 &lhs, const Objects::TObjO0 &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct<Objects::TObjO0, Objects::TObjO0>

    template <>
    struct NeqStruct<Objects::TObjO0, Objects::TObjO0> {
      static bool Do(const Objects::TObjO0 &lhs, const Objects::TObjO0 &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct<Objects::TObjO0, Objects::TObjO0>

    template <>
    inline bool Match(const Objects::TObjO0 &lhs, const Objects::TObjO0 &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Objects::TObjO0 &lhs, const Objects::TObjO0 &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Objects::TObjO0> {

      static TType GetType() {
        return TObj::Get({});
      }

    };

  } // Orly

} // Type

namespace std {
  template<>
  struct hash<Orly::Rt::Objects::TObjO0> {

    typedef size_t return_type;
    typedef Orly::Rt::Objects::TObjO0 argument_type;

    size_t operator()(const argument_type &obj) const {
      return  obj.GetHash();
    }

  }; // hash<Orly::Rt::Objects::TObjO0>
} // std
