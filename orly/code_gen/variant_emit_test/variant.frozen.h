/* <orly/code_gen/variant_emit_test/variant.frozen.h>

   FROZEN SNAPSHOT of the GenVariantHeader output for the variant
   { Integer(int) | Deleted } (mangled V2O07Deletedi7Integer), committed
   verbatim (only the O0 payload #include is redirected to the co-located
   O0.frozen.h) so the emitted code can be compiled, linked and exercised
   by orly/code_gen/variant_emit.test.cc without depending on a build-time
   orlyc artifact. Regenerate from GenVariantHeader if the emitter changes
   -- the live emitter is also run by orly/code_gen/variant.test.cc. */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include <orly/rt/tuple.h>
#include <orly/rt/containers.h>
#include <orly/rt/obj.h>
#include <orly/type/variant.h>
#include <orly/var/impl.h>
#include <orly/var/obj.h>
#include <orly/var/new_sabot.h>
#include <orly/sabot/to_native.h>
#include <orly/native/type.h>
#include <orly/type/new_sabot.h>

/* Needed payload objects */
#include <orly/code_gen/variant_emit_test/O0.frozen.h>

namespace Orly {

  namespace Rt {

    namespace Variants {

      class TVariantV2O07Deletedi7Integer {
        public:
        TVariantV2O07Deletedi7Integer() : Which(0) {}

        static TVariantV2O07Deletedi7Integer MkDeleted(const Orly::Rt::Objects::TObjO0 &vv) {
          TVariantV2O07Deletedi7Integer out;
          out.Which = 0;
          out.VDeleted = vv;
          return out;
        }
        static TVariantV2O07Deletedi7Integer MkInteger(const int64_t &vv) {
          TVariantV2O07Deletedi7Integer out;
          out.Which = 1;
          out.VInteger = vv;
          return out;
        }

        TVariantV2O07Deletedi7Integer(const TVariantV2O07Deletedi7Integer &that) : Which(that.Which), VDeleted(that.VDeleted), VInteger(that.VInteger) {}

        Var::TVar AsVar() const;

        size_t GetHash() const {
          assert(this);
          switch (Which) {
            case 0: return std::hash<size_t>()(0) ^ std::hash<Orly::Rt::Objects::TObjO0>()(VDeleted);
            case 1: return std::hash<size_t>()(1) ^ std::hash<int64_t>()(VInteger);
          }
          assert(false);
          return 0;
        }

        bool EqEq(const TVariantV2O07Deletedi7Integer &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::EqEq(VDeleted, that.VDeleted);
            case 1: return Rt::EqEq(VInteger, that.VInteger);
          }
          assert(false);
          return false;
        }

        bool Neq(const TVariantV2O07Deletedi7Integer &that) const {
          assert(this);
          assert(&that);
          return !EqEq(that);
        }

        bool Match(const TVariantV2O07Deletedi7Integer &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::Match(VDeleted, that.VDeleted);
            case 1: return Rt::Match(VInteger, that.VInteger);
          }
          assert(false);
          return false;
        }

        bool MatchLess(const TVariantV2O07Deletedi7Integer &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return Which < that.Which; }
          switch (Which) {
            case 0: return Rt::MatchLess(VDeleted, that.VDeleted);
            case 1: return Rt::MatchLess(VInteger, that.VInteger);
          }
          assert(false);
          return false;
        }

        size_t GetWhich() const {
          assert(this);
          return Which;
        }
        Orly::Rt::Objects::TObjO0 GetVDeleted() const {
          assert(this);
          assert(Which == 0);
          return VDeleted;
        }
        int64_t GetVInteger() const {
          assert(this);
          assert(Which == 1);
          return VInteger;
        }

        private:
        size_t Which;
        Orly::Rt::Objects::TObjO0 VDeleted;
        int64_t VInteger;
      }; // TVariantV2O07Deletedi7Integer

    } // Variants

    template <>
    struct EqEqStruct<Variants::TVariantV2O07Deletedi7Integer, Variants::TVariantV2O07Deletedi7Integer> {
      static bool Do(const Variants::TVariantV2O07Deletedi7Integer &lhs, const Variants::TVariantV2O07Deletedi7Integer &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct

    template <>
    struct NeqStruct<Variants::TVariantV2O07Deletedi7Integer, Variants::TVariantV2O07Deletedi7Integer> {
      static bool Do(const Variants::TVariantV2O07Deletedi7Integer &lhs, const Variants::TVariantV2O07Deletedi7Integer &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct

    template <>
    inline bool Match(const Variants::TVariantV2O07Deletedi7Integer &lhs, const Variants::TVariantV2O07Deletedi7Integer &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Variants::TVariantV2O07Deletedi7Integer &lhs, const Variants::TVariantV2O07Deletedi7Integer &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Variants::TVariantV2O07Deletedi7Integer> {

      static TType GetType() {
        return TVariant::Get({{"Deleted", Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{})}, {"Integer", Orly::Type::TInt::Get()}});
      }

    };

  } // Orly

} // Type

namespace Orly {

  namespace Rt {

    namespace Variants {

      inline Var::TVar TVariantV2O07Deletedi7Integer::AsVar() const {
        assert(this);
        switch (Which) {
          case 0: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV2O07Deletedi7Integer>::GetType(), "Deleted", Var::TVar(VDeleted));
          case 1: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV2O07Deletedi7Integer>::GetType(), "Integer", Var::TVar(VInteger));
        }
        assert(false);
        throw Rt::TSystemError(HERE, "variant has no active arm");
      }

    } // Orly

  } // Rt

} // Variants

namespace Orly {

  namespace Sabot {

    template <>
    class TToNativeVisitor<Rt::Variants::TVariantV2O07Deletedi7Integer> final : public TStateVisitor {
      NO_COPY(TToNativeVisitor);
      public:
      TToNativeVisitor(Rt::Variants::TVariantV2O07Deletedi7Integer &out) : Out(out) {}
      virtual void operator()(const State::TFree &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TTombstone &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TVoid &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TInt8 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TInt16 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TInt32 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TInt64 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TUInt8 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TUInt16 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TUInt32 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TUInt64 &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TBool &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TChar &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TFloat &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TDouble &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TDuration &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TTimePoint &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TUuid &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TBlob &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TStr &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TDesc &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TOpt &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TSet &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TVector &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TMap &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TTuple &) const override { THROW_ERROR(TInvalidConversion); }
      virtual void operator()(const State::TRecord &state) const override {
        void *type_alloc = alloca(Type::GetMaxTypeSize());
        Type::TRecord::TWrapper rtype(state.GetRecordType(type_alloc));
        void *type_pin_alloc = alloca(Type::GetMaxTypePinSize());
        Type::TRecord::TPin::TWrapper tpin(rtype->Pin(type_pin_alloc));
        void *state_pin_alloc = alloca(State::GetMaxStatePinSize());
        State::TRecord::TPin::TWrapper spin(state.Pin(state_pin_alloc));
        const size_t elem_count = tpin->GetElemCount();
        void *etype_alloc = alloca(Type::GetMaxTypeSize());
        void *estate_alloc = alloca(State::GetMaxStateSize());
        std::string field_name;
        size_t which = static_cast<size_t>(-1);
        size_t idx_Deleted = static_cast<size_t>(-1);
        size_t idx_Integer = static_cast<size_t>(-1);
        for (size_t i = 0; i < elem_count; ++i) {
          Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));
          if (field_name == "$which") {
            State::TAny::TWrapper which_state(spin->NewElem(i, estate_alloc));
            which = static_cast<size_t>(AsNative<int64_t>(*which_state));
          }
          else if (field_name == "Deleted") { idx_Deleted = i; }
          else if (field_name == "Integer") { idx_Integer = i; }
        }
        void *opt_pin_alloc = alloca(State::GetMaxStatePinSize());
        void *payload_alloc = alloca(State::GetMaxStateSize());
        if (which == 0) {
          if (idx_Deleted == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Deleted, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV2O07Deletedi7Integer::MkDeleted(AsNative<Orly::Rt::Objects::TObjO0>(*payload_state));
        }
        else if (which == 1) {
          if (idx_Integer == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Integer, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV2O07Deletedi7Integer::MkInteger(AsNative<int64_t>(*payload_state));
        }
        else { THROW_ERROR(TInvalidConversion); }
      }
      private:
      Rt::Variants::TVariantV2O07Deletedi7Integer &Out;
    }; // TToNativeVisitor

  } // Orly

} // Sabot

namespace Orly {

  namespace Native {

    template <>
    class State::Factory<Rt::Variants::TVariantV2O07Deletedi7Integer> final {
      NO_CONSTRUCTION(Factory);
      public:
      static Sabot::State::TAny *New(const Rt::Variants::TVariantV2O07Deletedi7Integer &val, void *state_alloc) {
        return State::Factory<Var::TVar>::New(val.AsVar(), state_alloc);
      }
    }; // State::Factory<TVariantV2O07Deletedi7Integer>

    template <>
    class Type::For<Rt::Variants::TVariantV2O07Deletedi7Integer> final {
      NO_CONSTRUCTION(For);
      public:
      static Sabot::Type::TAny *GetType(void *type_alloc) {
        return Orly::Type::NewSabot(type_alloc, Orly::Type::TDt<Rt::Variants::TVariantV2O07Deletedi7Integer>::GetType());
      }
      static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {
        return dynamic_cast<Sabot::Type::TRecord *>(GetType(type_alloc));
      }
    }; // Type::For<TVariantV2O07Deletedi7Integer>

  } // Orly

} // Native

namespace std {
  template<>
  struct hash<Orly::Rt::Variants::TVariantV2O07Deletedi7Integer> {

    typedef size_t return_type;
    typedef Orly::Rt::Variants::TVariantV2O07Deletedi7Integer argument_type;

    size_t operator()(const argument_type &v) const {
      return v.GetHash();
    }

  }; // hash
} // std
