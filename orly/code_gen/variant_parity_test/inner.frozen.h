/* <orly/code_gen/variant_parity_test/inner.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated header V2i1AO01B.h for
   inner_t, a closed nested variant, from the package in
   orly/code_gen/variant_parity_test/parity.orly. Committed verbatim except
   that the <orly/rt/objects/*.h> includes point at the co-located frozen
   copies, so orly/code_gen/variant_parity.test.cc can compile it without a
   build-time orlyc artifact. To refresh after an emitter change, compile
   parity.orly with orlyc and copy the four headers back the same way. */
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
#include <orly/native/variant.h>

/* Needed payload objects */
#include <orly/code_gen/variant_emit_test/O0.frozen.h>

namespace Orly {

  namespace Rt {

    namespace Variants {

      class TVariantV2i1AO01B {
        public:
        TVariantV2i1AO01B() : Which(0) {}

        static TVariantV2i1AO01B MkA(const int64_t &vv) {
          TVariantV2i1AO01B out;
          out.Which = 0;
          out.VA = vv;
          return out;
        }
        static TVariantV2i1AO01B MkB(const Orly::Rt::Objects::TObjO0 &vv) {
          TVariantV2i1AO01B out;
          out.Which = 1;
          out.VB = vv;
          return out;
        }

        TVariantV2i1AO01B(const TVariantV2i1AO01B &that) : Which(that.Which), VA(that.VA), VB(that.VB) {}

        Var::TVar AsVar() const;

        size_t GetHash() const {
          assert(this);
          switch (Which) {
            case 0: return std::hash<size_t>()(0) ^ std::hash<int64_t>()(VA);
            case 1: return std::hash<size_t>()(1) ^ std::hash<Orly::Rt::Objects::TObjO0>()(VB);
          }
          assert(false);
          return 0;
        }

        bool EqEq(const TVariantV2i1AO01B &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::EqEq(VA, that.VA);
            case 1: return Rt::EqEq(VB, that.VB);
          }
          assert(false);
          return false;
        }

        bool Neq(const TVariantV2i1AO01B &that) const {
          assert(this);
          assert(&that);
          return !EqEq(that);
        }

        bool Match(const TVariantV2i1AO01B &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::Match(VA, that.VA);
            case 1: return Rt::Match(VB, that.VB);
          }
          assert(false);
          return false;
        }

        bool MatchLess(const TVariantV2i1AO01B &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return Which < that.Which; }
          switch (Which) {
            case 0: return Rt::MatchLess(VA, that.VA);
            case 1: return Rt::MatchLess(VB, that.VB);
          }
          assert(false);
          return false;
        }

        size_t GetWhich() const {
          assert(this);
          return Which;
        }
        int64_t GetVA() const {
          assert(this);
          assert(Which == 0);
          return VA;
        }
        Orly::Rt::Objects::TObjO0 GetVB() const {
          assert(this);
          assert(Which == 1);
          return VB;
        }

        private:
        friend class Orly::Native::VariantArms<TVariantV2i1AO01B>;
        size_t Which;
        int64_t VA;
        Orly::Rt::Objects::TObjO0 VB;
      }; // TVariantV2i1AO01B

    } // Variants

    template <>
    struct EqEqStruct<Variants::TVariantV2i1AO01B, Variants::TVariantV2i1AO01B> {
      static bool Do(const Variants::TVariantV2i1AO01B &lhs, const Variants::TVariantV2i1AO01B &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct

    template <>
    struct NeqStruct<Variants::TVariantV2i1AO01B, Variants::TVariantV2i1AO01B> {
      static bool Do(const Variants::TVariantV2i1AO01B &lhs, const Variants::TVariantV2i1AO01B &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct

    template <>
    inline bool Match(const Variants::TVariantV2i1AO01B &lhs, const Variants::TVariantV2i1AO01B &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Variants::TVariantV2i1AO01B &lhs, const Variants::TVariantV2i1AO01B &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Variants::TVariantV2i1AO01B> {

      static TType GetType() {
        return TVariant::Get({{"A", Orly::Type::TInt::Get()}, {"B", Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{})}});
      }

    };

  } // Orly

} // Type

namespace Orly {

  namespace Rt {

    namespace Variants {

      inline Var::TVar TVariantV2i1AO01B::AsVar() const {
        assert(this);
        switch (Which) {
          case 0: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV2i1AO01B>::GetType(), "A", Var::TVar(VA));
          case 1: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV2i1AO01B>::GetType(), "B", Var::TVar(VB));
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
    class TToNativeVisitor<Rt::Variants::TVariantV2i1AO01B> final : public TStateVisitor {
      NO_COPY(TToNativeVisitor);
      public:
      TToNativeVisitor(Rt::Variants::TVariantV2i1AO01B &out) : Out(out) {}
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
        size_t idx_A = static_cast<size_t>(-1);
        size_t idx_B = static_cast<size_t>(-1);
        for (size_t i = 0; i < elem_count; ++i) {
          Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));
          if (field_name == "$which") {
            State::TAny::TWrapper which_state(spin->NewElem(i, estate_alloc));
            which = static_cast<size_t>(AsNative<int64_t>(*which_state));
          }
          else if (field_name == "A") { idx_A = i; }
          else if (field_name == "B") { idx_B = i; }
        }
        void *opt_pin_alloc = alloca(State::GetMaxStatePinSize());
        void *payload_alloc = alloca(State::GetMaxStateSize());
        if (which == 0) {
          if (idx_A == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_A, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV2i1AO01B::MkA(AsNative<int64_t>(*payload_state));
        }
        else if (which == 1) {
          if (idx_B == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_B, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV2i1AO01B::MkB(AsNative<Orly::Rt::Objects::TObjO0>(*payload_state));
        }
        else { THROW_ERROR(TInvalidConversion); }
      }
      private:
      Rt::Variants::TVariantV2i1AO01B &Out;
    }; // TToNativeVisitor

  } // Orly

} // Sabot

namespace Orly {

  namespace Native {

    template <>
    class [[gnu::visibility("hidden")]] VariantArms<Rt::Variants::TVariantV2i1AO01B> final {
      NO_CONSTRUCTION(VariantArms);
      public:
      static size_t GetArmCount() {
        return 2;
      }
      static const char *GetArmName(size_t arm) {
        switch (arm) {
          case 0: return "A";
          case 1: return "B";
        }
        throw Sabot::TIdxTooBig();
      }
      static size_t GetWhich(const Rt::Variants::TVariantV2i1AO01B &val) {
        return val.Which;
      }
      static Sabot::Type::TOpt *NewArmOptType(size_t arm, void *type_alloc) {
        switch (arm) {
          case 0: return Type::For<std::optional<int64_t>>::GetOptType(type_alloc);
          case 1: return Type::For<std::optional<Orly::Rt::Objects::TObjO0>>::GetOptType(type_alloc);
        }
        throw Sabot::TIdxTooBig();
      }
      static Sabot::State::TAny *NewPayloadState(const Rt::Variants::TVariantV2i1AO01B &val, void *state_alloc) {
        switch (val.Which) {
          case 0: return State::New(val.VA, state_alloc);
          case 1: return State::New(val.VB, state_alloc);
        }
        throw Sabot::TIdxTooBig();
      }
    }; // VariantArms<TVariantV2i1AO01B>

    template <>
    class State::Factory<Rt::Variants::TVariantV2i1AO01B> final {
      NO_CONSTRUCTION(Factory);
      public:
      static Sabot::State::TAny *New(const Rt::Variants::TVariantV2i1AO01B &val, void *state_alloc) {
        return Variant<Rt::Variants::TVariantV2i1AO01B>::NewState(val, state_alloc);
      }
    }; // State::Factory<TVariantV2i1AO01B>

    template <>
    class Type::For<Rt::Variants::TVariantV2i1AO01B> final {
      NO_CONSTRUCTION(For);
      public:
      static Sabot::Type::TAny *GetType(void *type_alloc) {
        return Variant<Rt::Variants::TVariantV2i1AO01B>::NewType(type_alloc);
      }
      static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {
        return Variant<Rt::Variants::TVariantV2i1AO01B>::NewType(type_alloc);
      }
    }; // Type::For<TVariantV2i1AO01B>

  } // Orly

} // Native

namespace std {
  template<>
  struct hash<Orly::Rt::Variants::TVariantV2i1AO01B> {

    typedef size_t return_type;
    typedef Orly::Rt::Variants::TVariantV2i1AO01B argument_type;

    size_t operator()(const argument_type &v) const {
      return v.GetHash();
    }

  }; // hash
} // std
