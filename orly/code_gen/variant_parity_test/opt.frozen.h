/* <orly/code_gen/variant_parity_test/opt.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated header V5Ps1Ei1FPi1GO1Pi1a1HLPi1L.h for
   opt_t, a variant whose arms have optional payloads (a bare optional, a record with an optional field and a list of optionals), from the package in
   orly/code_gen/variant_parity_test/parity.orly. Committed verbatim except
   that the <orly/rt/objects/*.h> includes point at the co-located frozen
   copies, so orly/code_gen/variant_parity.test.cc can compile it without a
   build-time orlyc artifact. To refresh after an emitter change, compile
   parity.orly with orlyc and copy the headers back the same way. */

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
#include <orly/code_gen/variant_parity_test/optrec.frozen.h>

namespace Orly {

  namespace Rt {

    namespace Variants {

      class TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L {
        public:
        TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L() : Which(0) {}

        static TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L MkE(const Orly::Rt::TOpt<std::string> &vv) {
          TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L out;
          out.Which = 0;
          out.VE = vv;
          return out;
        }
        static TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L MkF(const int64_t &vv) {
          TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L out;
          out.Which = 1;
          out.VF = vv;
          return out;
        }
        static TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L MkG(const Orly::Rt::TOpt<int64_t> &vv) {
          TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L out;
          out.Which = 2;
          out.VG = vv;
          return out;
        }
        static TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L MkH(const Orly::Rt::Objects::TObjO1Pi1a &vv) {
          TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L out;
          out.Which = 3;
          out.VH = vv;
          return out;
        }
        static TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L MkL(const std::vector<Orly::Rt::TOpt<int64_t>> &vv) {
          TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L out;
          out.Which = 4;
          out.VL = vv;
          return out;
        }

        TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L(const TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &that) : Which(that.Which), VE(that.VE), VF(that.VF), VG(that.VG), VH(that.VH), VL(that.VL) {}

        Var::TVar AsVar() const;

        size_t GetHash() const {
          assert(this);
          switch (Which) {
            case 0: return std::hash<size_t>()(0) ^ std::hash<Orly::Rt::TOpt<std::string>>()(VE);
            case 1: return std::hash<size_t>()(1) ^ std::hash<int64_t>()(VF);
            case 2: return std::hash<size_t>()(2) ^ std::hash<Orly::Rt::TOpt<int64_t>>()(VG);
            case 3: return std::hash<size_t>()(3) ^ std::hash<Orly::Rt::Objects::TObjO1Pi1a>()(VH);
            case 4: return std::hash<size_t>()(4) ^ std::hash<std::vector<Orly::Rt::TOpt<int64_t>>>()(VL);
          }
          assert(false);
          return 0;
        }

        bool EqEq(const TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::Match(VE, that.VE);
            case 1: return Rt::EqEq(VF, that.VF);
            case 2: return Rt::Match(VG, that.VG);
            case 3: return Rt::Match(VH, that.VH);
            case 4: return Rt::Match(VL, that.VL);
          }
          assert(false);
          return false;
        }

        bool Neq(const TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &that) const {
          assert(this);
          assert(&that);
          return !EqEq(that);
        }

        bool Match(const TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::Match(VE, that.VE);
            case 1: return Rt::Match(VF, that.VF);
            case 2: return Rt::Match(VG, that.VG);
            case 3: return Rt::Match(VH, that.VH);
            case 4: return Rt::Match(VL, that.VL);
          }
          assert(false);
          return false;
        }

        bool MatchLess(const TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return Which < that.Which; }
          switch (Which) {
            case 0: return Rt::MatchLess(VE, that.VE);
            case 1: return Rt::MatchLess(VF, that.VF);
            case 2: return Rt::MatchLess(VG, that.VG);
            case 3: return Rt::MatchLess(VH, that.VH);
            case 4: return Rt::MatchLess(VL, that.VL);
          }
          assert(false);
          return false;
        }

        size_t GetWhich() const {
          assert(this);
          return Which;
        }
        Orly::Rt::TOpt<std::string> GetVE() const {
          assert(this);
          assert(Which == 0);
          return VE;
        }
        int64_t GetVF() const {
          assert(this);
          assert(Which == 1);
          return VF;
        }
        Orly::Rt::TOpt<int64_t> GetVG() const {
          assert(this);
          assert(Which == 2);
          return VG;
        }
        Orly::Rt::Objects::TObjO1Pi1a GetVH() const {
          assert(this);
          assert(Which == 3);
          return VH;
        }
        std::vector<Orly::Rt::TOpt<int64_t>> GetVL() const {
          assert(this);
          assert(Which == 4);
          return VL;
        }

        private:
        size_t Which;
        Orly::Rt::TOpt<std::string> VE;
        int64_t VF;
        Orly::Rt::TOpt<int64_t> VG;
        Orly::Rt::Objects::TObjO1Pi1a VH;
        std::vector<Orly::Rt::TOpt<int64_t>> VL;
      }; // TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L

    } // Variants

    template <>
    struct EqEqStruct<Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L, Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> {
      static bool Do(const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &lhs, const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct

    template <>
    struct NeqStruct<Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L, Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> {
      static bool Do(const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &lhs, const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct

    template <>
    inline bool Match(const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &lhs, const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &lhs, const Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> {

      static TType GetType() {
        return TVariant::Get({{"E", Orly::Type::TOpt::Get(Orly::Type::TStr::Get())}, {"F", Orly::Type::TInt::Get()}, {"G", Orly::Type::TOpt::Get(Orly::Type::TInt::Get())}, {"H", Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{{std::string("a"), Orly::Type::TOpt::Get(Orly::Type::TInt::Get())}})}, {"L", Orly::Type::TList::Get(Orly::Type::TOpt::Get(Orly::Type::TInt::Get()))}});
      }

    };

  } // Orly

} // Type

namespace Orly {

  namespace Rt {

    namespace Variants {

      inline Var::TVar TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L::AsVar() const {
        assert(this);
        switch (Which) {
          case 0: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>::GetType(), "E", Var::TVar(VE));
          case 1: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>::GetType(), "F", Var::TVar(VF));
          case 2: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>::GetType(), "G", Var::TVar(VG));
          case 3: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>::GetType(), "H", Var::TVar(VH));
          case 4: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>::GetType(), "L", Var::TVar(VL));
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
    class TToNativeVisitor<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> final : public TStateVisitor {
      NO_COPY(TToNativeVisitor);
      public:
      TToNativeVisitor(Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &out) : Out(out) {}
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
        size_t idx_E = static_cast<size_t>(-1);
        size_t idx_F = static_cast<size_t>(-1);
        size_t idx_G = static_cast<size_t>(-1);
        size_t idx_H = static_cast<size_t>(-1);
        size_t idx_L = static_cast<size_t>(-1);
        for (size_t i = 0; i < elem_count; ++i) {
          Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));
          if (field_name == "$which") {
            State::TAny::TWrapper which_state(spin->NewElem(i, estate_alloc));
            which = static_cast<size_t>(AsNative<int64_t>(*which_state));
          }
          else if (field_name == "E") { idx_E = i; }
          else if (field_name == "F") { idx_F = i; }
          else if (field_name == "G") { idx_G = i; }
          else if (field_name == "H") { idx_H = i; }
          else if (field_name == "L") { idx_L = i; }
        }
        void *opt_pin_alloc = alloca(State::GetMaxStatePinSize());
        void *payload_alloc = alloca(State::GetMaxStateSize());
        if (which == 0) {
          if (idx_E == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_E, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L::MkE(AsNative<Orly::Rt::TOpt<std::string>>(*payload_state));
        }
        else if (which == 1) {
          if (idx_F == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_F, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L::MkF(AsNative<int64_t>(*payload_state));
        }
        else if (which == 2) {
          if (idx_G == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_G, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L::MkG(AsNative<Orly::Rt::TOpt<int64_t>>(*payload_state));
        }
        else if (which == 3) {
          if (idx_H == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_H, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L::MkH(AsNative<Orly::Rt::Objects::TObjO1Pi1a>(*payload_state));
        }
        else if (which == 4) {
          if (idx_L == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_L, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L::MkL(AsNative<std::vector<Orly::Rt::TOpt<int64_t>>>(*payload_state));
        }
        else { THROW_ERROR(TInvalidConversion); }
      }
      private:
      Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &Out;
    }; // TToNativeVisitor

  } // Orly

} // Sabot

namespace Orly {

  namespace Native {

    template <>
    class State::Factory<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> final {
      NO_CONSTRUCTION(Factory);
      public:
      static Sabot::State::TAny *New(const Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L &val, void *state_alloc) {
        return State::Factory<Var::TVar>::New(val.AsVar(), state_alloc);
      }
    }; // State::Factory<TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>

    template <>
    class Type::For<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> final {
      NO_CONSTRUCTION(For);
      public:
      static Sabot::Type::TAny *GetType(void *type_alloc) {
        return Orly::Type::NewSabot(type_alloc, Orly::Type::TDt<Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>::GetType());
      }
      static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {
        return dynamic_cast<Sabot::Type::TRecord *>(GetType(type_alloc));
      }
    }; // Type::For<TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L>

  } // Orly

} // Native

namespace std {
  template<>
  struct hash<Orly::Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L> {

    typedef size_t return_type;
    typedef Orly::Rt::Variants::TVariantV5Ps1Ei1FPi1GO1Pi1a1HLPi1L argument_type;

    size_t operator()(const argument_type &v) const {
      return v.GetHash();
    }

  }; // hash
} // std
