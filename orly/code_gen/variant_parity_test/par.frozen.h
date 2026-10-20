/* <orly/code_gen/variant_parity_test/par.frozen.h>

   FROZEN SNAPSHOT of the orlyc-generated header V15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb.h for
   par_t, the 15-arm variant under test, from the package in
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
#include <orly/code_gen/variant_parity_test/leaf.frozen.h>
#include <orly/code_gen/variant_emit_test/O0.frozen.h>
#include <orly/code_gen/variant_parity_test/inner.frozen.h>
#include <orly/code_gen/variant_parity_test/rec.frozen.h>

namespace Orly {

  namespace Rt {

    namespace Variants {

      class TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb {
        public:
        TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb() : Which(0) {}

        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkBool(const bool &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 0;
          out.VBool = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkDict(const Orly::Rt::TDict<std::string, Orly::Rt::Objects::TObjO2i1ns4name> &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 1;
          out.VDict = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkEmpty(const Orly::Rt::Objects::TObjO0 &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 2;
          out.VEmpty = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkIdent(const Base::TUuid &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 3;
          out.VIdent = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkInt(const int64_t &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 4;
          out.VInt = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkNested(const Orly::Rt::Variants::TVariantV2i1AO01B &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 5;
          out.VNested = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkRec(const Orly::Rt::Objects::TObjO2i1ns4name &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 6;
          out.VRec = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkRecur(const Orly::Rt::Variants::TVariantV2i4LeafLX04Node &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 7;
          out.VRecur = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkSet(const Orly::Rt::TSet<Orly::Rt::Objects::TObjO2i1ns4name> &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 8;
          out.VSet = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkStr(const std::string &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 9;
          out.VStr = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkTD(const Base::Chrono::TTimeDiff &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 10;
          out.VTD = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkTP(const Base::Chrono::TTimePnt &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 11;
          out.VTP = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkX_u(const int64_t &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 12;
          out.VX_u = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb MkXa(const int64_t &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 13;
          out.VXa = vv;
          return out;
        }
        static TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb Mkxb(const int64_t &vv) {
          TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb out;
          out.Which = 14;
          out.Vxb = vv;
          return out;
        }

        TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb(const TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &that) : Which(that.Which), VBool(that.VBool), VDict(that.VDict), VEmpty(that.VEmpty), VIdent(that.VIdent), VInt(that.VInt), VNested(that.VNested), VRec(that.VRec), VRecur(that.VRecur), VSet(that.VSet), VStr(that.VStr), VTD(that.VTD), VTP(that.VTP), VX_u(that.VX_u), VXa(that.VXa), Vxb(that.Vxb) {}

        Var::TVar AsVar() const;

        size_t GetHash() const {
          assert(this);
          switch (Which) {
            case 0: return std::hash<size_t>()(0) ^ std::hash<bool>()(VBool);
            case 1: return std::hash<size_t>()(1) ^ std::hash<Orly::Rt::TDict<std::string, Orly::Rt::Objects::TObjO2i1ns4name>>()(VDict);
            case 2: return std::hash<size_t>()(2) ^ std::hash<Orly::Rt::Objects::TObjO0>()(VEmpty);
            case 3: return std::hash<size_t>()(3) ^ std::hash<Base::TUuid>()(VIdent);
            case 4: return std::hash<size_t>()(4) ^ std::hash<int64_t>()(VInt);
            case 5: return std::hash<size_t>()(5) ^ std::hash<Orly::Rt::Variants::TVariantV2i1AO01B>()(VNested);
            case 6: return std::hash<size_t>()(6) ^ std::hash<Orly::Rt::Objects::TObjO2i1ns4name>()(VRec);
            case 7: return std::hash<size_t>()(7) ^ std::hash<Orly::Rt::Variants::TVariantV2i4LeafLX04Node>()(VRecur);
            case 8: return std::hash<size_t>()(8) ^ std::hash<Orly::Rt::TSet<Orly::Rt::Objects::TObjO2i1ns4name>>()(VSet);
            case 9: return std::hash<size_t>()(9) ^ std::hash<std::string>()(VStr);
            case 10: return std::hash<size_t>()(10) ^ std::hash<Base::Chrono::TTimeDiff>()(VTD);
            case 11: return std::hash<size_t>()(11) ^ std::hash<Base::Chrono::TTimePnt>()(VTP);
            case 12: return std::hash<size_t>()(12) ^ std::hash<int64_t>()(VX_u);
            case 13: return std::hash<size_t>()(13) ^ std::hash<int64_t>()(VXa);
            case 14: return std::hash<size_t>()(14) ^ std::hash<int64_t>()(Vxb);
          }
          assert(false);
          return 0;
        }

        bool EqEq(const TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::EqEq(VBool, that.VBool);
            case 1: return Rt::EqEq(VDict, that.VDict);
            case 2: return Rt::EqEq(VEmpty, that.VEmpty);
            case 3: return Rt::EqEq(VIdent, that.VIdent);
            case 4: return Rt::EqEq(VInt, that.VInt);
            case 5: return Rt::EqEq(VNested, that.VNested);
            case 6: return Rt::EqEq(VRec, that.VRec);
            case 7: return Rt::EqEq(VRecur, that.VRecur);
            case 8: return Rt::EqEq(VSet, that.VSet);
            case 9: return Rt::EqEq(VStr, that.VStr);
            case 10: return Rt::EqEq(VTD, that.VTD);
            case 11: return Rt::EqEq(VTP, that.VTP);
            case 12: return Rt::EqEq(VX_u, that.VX_u);
            case 13: return Rt::EqEq(VXa, that.VXa);
            case 14: return Rt::EqEq(Vxb, that.Vxb);
          }
          assert(false);
          return false;
        }

        bool Neq(const TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &that) const {
          assert(this);
          assert(&that);
          return !EqEq(that);
        }

        bool Match(const TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return false; }
          switch (Which) {
            case 0: return Rt::Match(VBool, that.VBool);
            case 1: return Rt::Match(VDict, that.VDict);
            case 2: return Rt::Match(VEmpty, that.VEmpty);
            case 3: return Rt::Match(VIdent, that.VIdent);
            case 4: return Rt::Match(VInt, that.VInt);
            case 5: return Rt::Match(VNested, that.VNested);
            case 6: return Rt::Match(VRec, that.VRec);
            case 7: return Rt::Match(VRecur, that.VRecur);
            case 8: return Rt::Match(VSet, that.VSet);
            case 9: return Rt::Match(VStr, that.VStr);
            case 10: return Rt::Match(VTD, that.VTD);
            case 11: return Rt::Match(VTP, that.VTP);
            case 12: return Rt::Match(VX_u, that.VX_u);
            case 13: return Rt::Match(VXa, that.VXa);
            case 14: return Rt::Match(Vxb, that.Vxb);
          }
          assert(false);
          return false;
        }

        bool MatchLess(const TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &that) const {
          assert(this);
          assert(&that);
          if (Which != that.Which) { return Which < that.Which; }
          switch (Which) {
            case 0: return Rt::MatchLess(VBool, that.VBool);
            case 1: return Rt::MatchLess(VDict, that.VDict);
            case 2: return Rt::MatchLess(VEmpty, that.VEmpty);
            case 3: return Rt::MatchLess(VIdent, that.VIdent);
            case 4: return Rt::MatchLess(VInt, that.VInt);
            case 5: return Rt::MatchLess(VNested, that.VNested);
            case 6: return Rt::MatchLess(VRec, that.VRec);
            case 7: return Rt::MatchLess(VRecur, that.VRecur);
            case 8: return Rt::MatchLess(VSet, that.VSet);
            case 9: return Rt::MatchLess(VStr, that.VStr);
            case 10: return Rt::MatchLess(VTD, that.VTD);
            case 11: return Rt::MatchLess(VTP, that.VTP);
            case 12: return Rt::MatchLess(VX_u, that.VX_u);
            case 13: return Rt::MatchLess(VXa, that.VXa);
            case 14: return Rt::MatchLess(Vxb, that.Vxb);
          }
          assert(false);
          return false;
        }

        size_t GetWhich() const {
          assert(this);
          return Which;
        }
        bool GetVBool() const {
          assert(this);
          assert(Which == 0);
          return VBool;
        }
        Orly::Rt::TDict<std::string, Orly::Rt::Objects::TObjO2i1ns4name> GetVDict() const {
          assert(this);
          assert(Which == 1);
          return VDict;
        }
        Orly::Rt::Objects::TObjO0 GetVEmpty() const {
          assert(this);
          assert(Which == 2);
          return VEmpty;
        }
        Base::TUuid GetVIdent() const {
          assert(this);
          assert(Which == 3);
          return VIdent;
        }
        int64_t GetVInt() const {
          assert(this);
          assert(Which == 4);
          return VInt;
        }
        Orly::Rt::Variants::TVariantV2i1AO01B GetVNested() const {
          assert(this);
          assert(Which == 5);
          return VNested;
        }
        Orly::Rt::Objects::TObjO2i1ns4name GetVRec() const {
          assert(this);
          assert(Which == 6);
          return VRec;
        }
        Orly::Rt::Variants::TVariantV2i4LeafLX04Node GetVRecur() const {
          assert(this);
          assert(Which == 7);
          return VRecur;
        }
        Orly::Rt::TSet<Orly::Rt::Objects::TObjO2i1ns4name> GetVSet() const {
          assert(this);
          assert(Which == 8);
          return VSet;
        }
        std::string GetVStr() const {
          assert(this);
          assert(Which == 9);
          return VStr;
        }
        Base::Chrono::TTimeDiff GetVTD() const {
          assert(this);
          assert(Which == 10);
          return VTD;
        }
        Base::Chrono::TTimePnt GetVTP() const {
          assert(this);
          assert(Which == 11);
          return VTP;
        }
        int64_t GetVX_u() const {
          assert(this);
          assert(Which == 12);
          return VX_u;
        }
        int64_t GetVXa() const {
          assert(this);
          assert(Which == 13);
          return VXa;
        }
        int64_t GetVxb() const {
          assert(this);
          assert(Which == 14);
          return Vxb;
        }

        private:
        friend class Orly::Native::VariantArms<TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>;
        size_t Which;
        bool VBool;
        Orly::Rt::TDict<std::string, Orly::Rt::Objects::TObjO2i1ns4name> VDict;
        Orly::Rt::Objects::TObjO0 VEmpty;
        Base::TUuid VIdent;
        int64_t VInt;
        Orly::Rt::Variants::TVariantV2i1AO01B VNested;
        Orly::Rt::Objects::TObjO2i1ns4name VRec;
        Orly::Rt::Variants::TVariantV2i4LeafLX04Node VRecur;
        Orly::Rt::TSet<Orly::Rt::Objects::TObjO2i1ns4name> VSet;
        std::string VStr;
        Base::Chrono::TTimeDiff VTD;
        Base::Chrono::TTimePnt VTP;
        int64_t VX_u;
        int64_t VXa;
        int64_t Vxb;
      }; // TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb

    } // Variants

    template <>
    struct EqEqStruct<Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb, Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> {
      static bool Do(const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &lhs, const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &rhs) {
        return lhs.EqEq(rhs);
      }
    }; // EqEqStruct

    template <>
    struct NeqStruct<Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb, Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> {
      static bool Do(const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &lhs, const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &rhs) {
        return lhs.Neq(rhs);
      }
    }; // NeqStruct

    template <>
    inline bool Match(const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &lhs, const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &rhs) {
      return lhs.Match(rhs);
    }
    template <>
    inline bool MatchLess(const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &lhs, const Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &rhs) {
      return lhs.MatchLess(rhs);
    }

  } // Orly

} // Rt

namespace Orly {

  namespace Type {

    template <>
    struct TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> {

      static TType GetType() {
        return TVariant::Get({{"Bool", Orly::Type::TBool::Get()}, {"Dict", Orly::Type::TDict::Get(Orly::Type::TStr::Get(), Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{{std::string("n"), Orly::Type::TInt::Get()}, {std::string("name"), Orly::Type::TStr::Get()}}))}, {"Empty", Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{})}, {"Ident", Orly::Type::TId::Get()}, {"Int", Orly::Type::TInt::Get()}, {"Nested", Orly::Type::TVariant::Get(std::map<std::string, Orly::Type::TType>{{std::string("A"), Orly::Type::TInt::Get()}, {std::string("B"), Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{})}})}, {"Rec", Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{{std::string("n"), Orly::Type::TInt::Get()}, {std::string("name"), Orly::Type::TStr::Get()}})}, {"Recur", Orly::Type::TVariant::Get(std::map<std::string, Orly::Type::TType>{{std::string("Leaf"), Orly::Type::TInt::Get()}, {std::string("Node"), Orly::Type::TList::Get(Orly::Type::TSelfRef::Get(0))}})}, {"Set", Orly::Type::TSet::Get(Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{{std::string("n"), Orly::Type::TInt::Get()}, {std::string("name"), Orly::Type::TStr::Get()}}))}, {"Str", Orly::Type::TStr::Get()}, {"TD", Orly::Type::TTimeDiff::Get()}, {"TP", Orly::Type::TTimePnt::Get()}, {"X_u", Orly::Type::TInt::Get()}, {"Xa", Orly::Type::TInt::Get()}, {"xb", Orly::Type::TInt::Get()}});
      }

    };

  } // Orly

} // Type

namespace Orly {

  namespace Rt {

    namespace Variants {

      inline Var::TVar TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::AsVar() const {
        assert(this);
        switch (Which) {
          case 0: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Bool", Var::TVar(VBool));
          case 1: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Dict", Var::TVar(VDict));
          case 2: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Empty", Var::TVar(VEmpty));
          case 3: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Ident", Var::TVar(VIdent));
          case 4: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Int", Var::TVar(VInt));
          case 5: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Nested", Var::TVar(VNested));
          case 6: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Rec", Var::TVar(VRec));
          case 7: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Recur", Var::TVar(VRecur));
          case 8: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Set", Var::TVar(VSet));
          case 9: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Str", Var::TVar(VStr));
          case 10: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "TD", Var::TVar(VTD));
          case 11: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "TP", Var::TVar(VTP));
          case 12: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "X_u", Var::TVar(VX_u));
          case 13: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "Xa", Var::TVar(VXa));
          case 14: return Var::TVar::Variant(Type::TDt<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::GetType(), "xb", Var::TVar(Vxb));
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
    class TToNativeVisitor<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> final : public TStateVisitor {
      NO_COPY(TToNativeVisitor);
      public:
      TToNativeVisitor(Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &out) : Out(out) {}
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
        size_t idx_Bool = static_cast<size_t>(-1);
        size_t idx_Dict = static_cast<size_t>(-1);
        size_t idx_Empty = static_cast<size_t>(-1);
        size_t idx_Ident = static_cast<size_t>(-1);
        size_t idx_Int = static_cast<size_t>(-1);
        size_t idx_Nested = static_cast<size_t>(-1);
        size_t idx_Rec = static_cast<size_t>(-1);
        size_t idx_Recur = static_cast<size_t>(-1);
        size_t idx_Set = static_cast<size_t>(-1);
        size_t idx_Str = static_cast<size_t>(-1);
        size_t idx_TD = static_cast<size_t>(-1);
        size_t idx_TP = static_cast<size_t>(-1);
        size_t idx_X_u = static_cast<size_t>(-1);
        size_t idx_Xa = static_cast<size_t>(-1);
        size_t idx_xb = static_cast<size_t>(-1);
        for (size_t i = 0; i < elem_count; ++i) {
          Type::TAny::TWrapper(tpin->NewElem(i, field_name, etype_alloc));
          if (field_name == "$which") {
            State::TAny::TWrapper which_state(spin->NewElem(i, estate_alloc));
            which = static_cast<size_t>(AsNative<int64_t>(*which_state));
          }
          else if (field_name == "Bool") { idx_Bool = i; }
          else if (field_name == "Dict") { idx_Dict = i; }
          else if (field_name == "Empty") { idx_Empty = i; }
          else if (field_name == "Ident") { idx_Ident = i; }
          else if (field_name == "Int") { idx_Int = i; }
          else if (field_name == "Nested") { idx_Nested = i; }
          else if (field_name == "Rec") { idx_Rec = i; }
          else if (field_name == "Recur") { idx_Recur = i; }
          else if (field_name == "Set") { idx_Set = i; }
          else if (field_name == "Str") { idx_Str = i; }
          else if (field_name == "TD") { idx_TD = i; }
          else if (field_name == "TP") { idx_TP = i; }
          else if (field_name == "X_u") { idx_X_u = i; }
          else if (field_name == "Xa") { idx_Xa = i; }
          else if (field_name == "xb") { idx_xb = i; }
        }
        void *opt_pin_alloc = alloca(State::GetMaxStatePinSize());
        void *payload_alloc = alloca(State::GetMaxStateSize());
        if (which == 0) {
          if (idx_Bool == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Bool, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkBool(AsNative<bool>(*payload_state));
        }
        else if (which == 1) {
          if (idx_Dict == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Dict, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkDict(AsNative<Orly::Rt::TDict<std::string, Orly::Rt::Objects::TObjO2i1ns4name>>(*payload_state));
        }
        else if (which == 2) {
          if (idx_Empty == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Empty, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkEmpty(AsNative<Orly::Rt::Objects::TObjO0>(*payload_state));
        }
        else if (which == 3) {
          if (idx_Ident == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Ident, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkIdent(AsNative<Base::TUuid>(*payload_state));
        }
        else if (which == 4) {
          if (idx_Int == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Int, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkInt(AsNative<int64_t>(*payload_state));
        }
        else if (which == 5) {
          if (idx_Nested == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Nested, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkNested(AsNative<Orly::Rt::Variants::TVariantV2i1AO01B>(*payload_state));
        }
        else if (which == 6) {
          if (idx_Rec == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Rec, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkRec(AsNative<Orly::Rt::Objects::TObjO2i1ns4name>(*payload_state));
        }
        else if (which == 7) {
          if (idx_Recur == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Recur, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkRecur(AsNative<Orly::Rt::Variants::TVariantV2i4LeafLX04Node>(*payload_state));
        }
        else if (which == 8) {
          if (idx_Set == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Set, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkSet(AsNative<Orly::Rt::TSet<Orly::Rt::Objects::TObjO2i1ns4name>>(*payload_state));
        }
        else if (which == 9) {
          if (idx_Str == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Str, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkStr(AsNative<std::string>(*payload_state));
        }
        else if (which == 10) {
          if (idx_TD == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_TD, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkTD(AsNative<Base::Chrono::TTimeDiff>(*payload_state));
        }
        else if (which == 11) {
          if (idx_TP == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_TP, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkTP(AsNative<Base::Chrono::TTimePnt>(*payload_state));
        }
        else if (which == 12) {
          if (idx_X_u == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_X_u, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkX_u(AsNative<int64_t>(*payload_state));
        }
        else if (which == 13) {
          if (idx_Xa == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_Xa, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::MkXa(AsNative<int64_t>(*payload_state));
        }
        else if (which == 14) {
          if (idx_xb == static_cast<size_t>(-1)) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper arm_state(spin->NewElem(idx_xb, estate_alloc));
          const State::TOpt *opt = dynamic_cast<const State::TOpt *>(arm_state.get());
          if (!opt) { THROW_ERROR(TInvalidConversion); }
          State::TOpt::TPin::TWrapper opin(opt->Pin(opt_pin_alloc));
          if (opin->GetElemCount() != 1) { THROW_ERROR(TInvalidConversion); }
          State::TAny::TWrapper payload_state(opin->NewElem(0, payload_alloc));
          Out = Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb::Mkxb(AsNative<int64_t>(*payload_state));
        }
        else { THROW_ERROR(TInvalidConversion); }
      }
      private:
      Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &Out;
    }; // TToNativeVisitor

  } // Orly

} // Sabot

namespace Orly {

  namespace Native {

    template <>
    class [[gnu::visibility("hidden")]] VariantArms<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> final {
      NO_CONSTRUCTION(VariantArms);
      public:
      static size_t GetArmCount() {
        return 15;
      }
      static const char *GetArmName(size_t arm) {
        switch (arm) {
          case 0: return "Bool";
          case 1: return "Dict";
          case 2: return "Empty";
          case 3: return "Ident";
          case 4: return "Int";
          case 5: return "Nested";
          case 6: return "Rec";
          case 7: return "Recur";
          case 8: return "Set";
          case 9: return "Str";
          case 10: return "TD";
          case 11: return "TP";
          case 12: return "X_u";
          case 13: return "Xa";
          case 14: return "xb";
        }
        throw Sabot::TIdxTooBig();
      }
      static size_t GetWhich(const Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &val) {
        return val.Which;
      }
      static Sabot::Type::TOpt *NewArmOptType(size_t arm, void *type_alloc) {
        switch (arm) {
          case 0: return Type::For<std::optional<bool>>::GetOptType(type_alloc);
          case 1: return Type::For<std::optional<Orly::Rt::TDict<std::string, Orly::Rt::Objects::TObjO2i1ns4name>>>::GetOptType(type_alloc);
          case 2: return Type::For<std::optional<Orly::Rt::Objects::TObjO0>>::GetOptType(type_alloc);
          case 3: return Type::For<std::optional<Base::TUuid>>::GetOptType(type_alloc);
          case 4: return Type::For<std::optional<int64_t>>::GetOptType(type_alloc);
          case 5: return Type::For<std::optional<Orly::Rt::Variants::TVariantV2i1AO01B>>::GetOptType(type_alloc);
          case 6: return Type::For<std::optional<Orly::Rt::Objects::TObjO2i1ns4name>>::GetOptType(type_alloc);
          case 7: return Type::For<std::optional<Orly::Rt::Variants::TVariantV2i4LeafLX04Node>>::GetOptType(type_alloc);
          case 8: return Type::For<std::optional<Orly::Rt::TSet<Orly::Rt::Objects::TObjO2i1ns4name>>>::GetOptType(type_alloc);
          case 9: return Type::For<std::optional<std::string>>::GetOptType(type_alloc);
          case 10: return Type::For<std::optional<Base::Chrono::TTimeDiff>>::GetOptType(type_alloc);
          case 11: return Type::For<std::optional<Base::Chrono::TTimePnt>>::GetOptType(type_alloc);
          case 12: return Type::For<std::optional<int64_t>>::GetOptType(type_alloc);
          case 13: return Type::For<std::optional<int64_t>>::GetOptType(type_alloc);
          case 14: return Type::For<std::optional<int64_t>>::GetOptType(type_alloc);
        }
        throw Sabot::TIdxTooBig();
      }
      static Sabot::State::TAny *NewPayloadState(const Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &val, void *state_alloc) {
        switch (val.Which) {
          case 0: return State::New(val.VBool, state_alloc);
          case 1: return State::New(val.VDict, state_alloc);
          case 2: return State::New(val.VEmpty, state_alloc);
          case 3: return State::New(val.VIdent, state_alloc);
          case 4: return State::New(val.VInt, state_alloc);
          case 5: return State::New(val.VNested, state_alloc);
          case 6: return State::New(val.VRec, state_alloc);
          case 7: return State::New(val.VRecur, state_alloc);
          case 8: return State::New(val.VSet, state_alloc);
          case 9: return State::New(val.VStr, state_alloc);
          case 10: return State::New(val.VTD, state_alloc);
          case 11: return State::New(val.VTP, state_alloc);
          case 12: return State::New(val.VX_u, state_alloc);
          case 13: return State::New(val.VXa, state_alloc);
          case 14: return State::New(val.Vxb, state_alloc);
        }
        throw Sabot::TIdxTooBig();
      }
    }; // VariantArms<TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>

    template <>
    class State::Factory<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> final {
      NO_CONSTRUCTION(Factory);
      public:
      static Sabot::State::TAny *New(const Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb &val, void *state_alloc) {
        return Variant<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::NewState(val, state_alloc);
      }
    }; // State::Factory<TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>

    template <>
    class Type::For<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> final {
      NO_CONSTRUCTION(For);
      public:
      static Sabot::Type::TAny *GetType(void *type_alloc) {
        return Variant<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::NewType(type_alloc);
      }
      static Sabot::Type::TRecord *GetRecordType(void *type_alloc) {
        return Variant<Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>::NewType(type_alloc);
      }
    }; // Type::For<TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb>

  } // Orly

} // Native

namespace std {
  template<>
  struct hash<Orly::Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb> {

    typedef size_t return_type;
    typedef Orly::Rt::Variants::TVariantV15b4BoolDsO2i1ns4name4DictO05EmptyI5Identi3IntV2i1AO01B6NestedO2i1ns4name3RecV2i4LeafLX04Node5RecurSO2i1ns4name3Sets3Strd2TDt2TPi3X_ui2Xai2xb argument_type;

    size_t operator()(const argument_type &v) const {
      return v.GetHash();
    }

  }; // hash
} // std
