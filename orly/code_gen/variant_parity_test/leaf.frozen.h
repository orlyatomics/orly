/* <orly/code_gen/variant_parity_test/leaf.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated header V2i4LeafLX04Node.h for
   leaf_t, a recursive variant (keeps the AsVar() path), from the package in
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
#include <utility>
#include <orly/rt/box.h>
#include <orly/type/self_ref.h>

namespace Orly {

  namespace Rt {

    namespace Variants {

      class TVariantV2i4LeafLX04Node;

      class TVariantV2i4LeafLX04Node {
        public:
        TVariantV2i4LeafLX04Node() : Which(0) {}

        static TVariantV2i4LeafLX04Node MkLeaf(const int64_t &vv) {
          TVariantV2i4LeafLX04Node out;
          out.Which = 0;
          out.VLeaf = vv;
          return out;
        }
        template <typename TPayload>
        static TVariantV2i4LeafLX04Node MkNode(const TPayload &vv) {
          TVariantV2i4LeafLX04Node out;
          out.Which = 1;
          out.VNode = Rt::DeepBox<std::vector<Rt::TBox<TVariantV2i4LeafLX04Node>>>(vv);
          return out;
        }

        TVariantV2i4LeafLX04Node(const TVariantV2i4LeafLX04Node &that) : Which(that.Which), VLeaf(that.VLeaf), VNode(that.VNode) {}

        Var::TVar AsVar() const;

        size_t GetHash() const {
          assert(this);
          switch (Which) {
            case 0: return std::hash<size_t>()(0) ^ std::hash<int64_t>()(VLeaf);
            case 1: return std::hash<size_t>()(1) ^ std::hash<std::vector<Rt::TBox<TVariantV2i4LeafLX04Node>>>()(VNode);
          }
          assert(false);
          return 0;
        }

        bool EqEq(const TVariantV2i4LeafLX04Node &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::EqEq(VLeaf, that.VLeaf);
            case 1: return Rt::Match(VNode, that.VNode);
          }
          assert(false);
          return false;
        }

        bool Neq(const TVariantV2i4LeafLX04Node &that) const {
          assert(this);
          assert(&that);
          return !EqEq(that);
        }

        bool Match(const TVariantV2i4LeafLX04Node &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::Match(VLeaf, that.VLeaf);
            case 1: return Rt::Match(VNode, that.VNode);
          }
          assert(false);
          return false;
        }

        bool MatchLess(const TVariantV2i4LeafLX04Node &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return Which < that.Which; }
          switch (Which) {
            case 0: return Rt::MatchLess(VLeaf, that.VLeaf);
            case 1: return Rt::MatchLess(VNode, that.VNode);
          }
          assert(false);
          return false;
        }

        size_t GetWhich() const {
          assert(this);
          return Which;
        }
        int64_t GetVLeaf() const {
          assert(this);
          assert(Which == 0);
          return VLeaf;
        }
        template <typename TRet>
        TRet GetVNode() const {
          assert(this);
          assert(Which == 1);
          return Rt::DeepUnbox<TRet>(VNode);
        }

        private:
        size_t Which;
        int64_t VLeaf;
        std::vector<Rt::TBox<TVariantV2i4LeafLX04Node>> VNode;
      }; // TVariantV2i4LeafLX04Node

    } // Variants

    template <>
    struct EqEqStruct<Variants::TVariantV2i4LeafLX04Node, Variants::TVariantV2i4LeafLX04Node> {
      static bool Do(const Variants::TVariantV2i4LeafLX04Node &lhs, const Variants::TVariantV2i4LeafLX04Node &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct

    template <>
    struct NeqStruct<Variants::TVariantV2i4LeafLX04Node, Variants::TVariantV2i4LeafLX04Node> {
      static bool Do(const Variants::TVariantV2i4LeafLX04Node &lhs, const Variants::TVariantV2i4LeafLX04Node &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct

    template <>
    inline bool Match(const Variants::TVariantV2i4LeafLX04Node &lhs, const Variants::TVariantV2i4LeafLX04Node &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Variants::TVariantV2i4LeafLX04Node &lhs, const Variants::TVariantV2i4LeafLX04Node &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Variants::TVariantV2i4LeafLX04Node> {

      static TType GetType() {
        return TVariant::Get({{"Leaf", Orly::Type::TInt::Get()}, {"Node", Orly::Type::TList::Get(Orly::Type::TSelfRef::Get(0))}});
      }

    };

  } // Orly

} // Type

namespace Orly {

  namespace Rt {

    namespace Variants {

      inline Var::TVar TVariantV2i4LeafLX04Node::AsVar() const {
        assert(this);
        switch (Which) {
          case 0: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV2i4LeafLX04Node>::GetType(), "Leaf", Var::TVar(VLeaf));
          case 1: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV2i4LeafLX04Node>::GetType(), "Node", Var::TVar(Rt::DeepUnbox<std::vector<Orly::Rt::Variants::TVariantV2i4LeafLX04Node>>(VNode)));
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
    class TToNativeVisitor<Rt::Variants::TVariantV2i4LeafLX04Node> final : public TStateVisitor {
      NO_COPY(TToNativeVisitor);
      public:
      TToNativeVisitor(Rt::Variants::TVariantV2i4LeafLX04Node &out) : Out(out) {}
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
        size_t idx_Leaf = static_cast<size_t>(-1);
        size_t idx_Node = static_cast<size_t>(-1);
        for (size_t i = 0; i < elem_count; ++i) {
          Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));
          if (field_name == "$which") {
            State::TAny::TWrapper which_state(spin->NewElem(i, estate_alloc));
            which = static_cast<size_t>(AsNative<int64_t>(*which_state));
          }
          else if (field_name == "Leaf") { idx_Leaf = i; }
          else if (field_name == "Node") { idx_Node = i; }
        }
        void *opt_pin_alloc = alloca(State::GetMaxStatePinSize());
        void *payload_alloc = alloca(State::GetMaxStateSize());
        if (which == 0) {
          if (idx_Leaf == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Leaf, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV2i4LeafLX04Node::MkLeaf(AsNative<int64_t>(*payload_state));
        }
        else if (which == 1) {
          if (idx_Node == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Node, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV2i4LeafLX04Node::MkNode(AsNative<std::vector<Orly::Rt::Variants::TVariantV2i4LeafLX04Node>>(*payload_state));
        }
        else { THROW_ERROR(TInvalidConversion); }
      }
      private:
      Rt::Variants::TVariantV2i4LeafLX04Node &Out;
    }; // TToNativeVisitor

  } // Orly

} // Sabot

namespace Orly {

  namespace Native {

    template <>
    class State::Factory<Rt::Variants::TVariantV2i4LeafLX04Node> final {
      NO_CONSTRUCTION(Factory);
      public:
      static Sabot::State::TAny *New(const Rt::Variants::TVariantV2i4LeafLX04Node &val, void *state_alloc) {
        return State::Factory<Var::TVar>::New(val.AsVar(), state_alloc);
      }
    }; // State::Factory<TVariantV2i4LeafLX04Node>

    template <>
    class Type::For<Rt::Variants::TVariantV2i4LeafLX04Node> final {
      NO_CONSTRUCTION(For);
      public:
      static Sabot::Type::TAny *GetType(void *type_alloc) {
        return Orly::Type::NewSabot(type_alloc, Orly::Type::TDt<Rt::Variants::TVariantV2i4LeafLX04Node>::GetType());
      }
      static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {
        return dynamic_cast<Sabot::Type::TRecord *>(GetType(type_alloc));
      }
    }; // Type::For<TVariantV2i4LeafLX04Node>

  } // Orly

} // Native

namespace std {
  template<>
  struct hash<Orly::Rt::Variants::TVariantV2i4LeafLX04Node> {

    typedef size_t return_type;
    typedef Orly::Rt::Variants::TVariantV2i4LeafLX04Node argument_type;

    size_t operator()(const argument_type &v) const {
      return v.GetHash();
    }

  }; // hash
} // std
