/* <orly/client/program/translate_expr.h>

   Bridge between client-program CST expressions and orly's sabot
   type/state types. Templated `TUnaryExpr` / `TBinaryExpr` /
   `TRecordExpr` / `TTupleExpr` (plus the per-leaf `State::TBool`,
   `TInt`, `TReal`, `TStr`, ... family) adapt CST nodes to
   `Sabot::Type` / `Sabot::State` so values typed at the client side
   can flow into the same comparison and serialisation machinery
   the server uses.

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

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include <orly/atom/kit2.h>
#include <orly/client/program/program.program.cst.h>
#include <orly/sabot/state.h>
#include <orly/sabot/type.h>

namespace Orly {

  namespace Client {

    namespace Program {

      /* Convert a Package Name expression into a vector of strings and a version number */
      void TranslatePackage(std::vector<std::string> &package_name, uint64_t &version_number, const TPackageName *expr);

      /* Convert a Name List expression into a vector of string. */
      void TranslatePathName(std::vector<std::string> &path_name, const TNameList *expr);

      Sabot::Type::TAny *NewTypeSabot(const TType *type, void *alloc);

      Sabot::Type::TAny *NewTypeSabot(const TExpr *expr, void *alloc);

      Sabot::State::TAny *NewStateSabot(const TExpr *expr, void *alloc);

      Atom::TCore *TranslateExpr(Atom::TCore::TExtensibleArena *arena, void *core_alloc, const TExpr *expr);

      namespace Type {

        template <typename TBase>
        class TUnaryExpr final
            : public TBase {
          public:

          using TBasePin = typename TBase::TPin;

          class TPin final
              : public TBasePin {
            public:

            TPin(const TUnaryExpr *unary)
                : UnaryExpr(unary) {
              assert(unary);
            }

            virtual Sabot::Type::TAny *NewElem(void *type_alloc) const override {
              return NewTypeSabot(UnaryExpr->Expr, type_alloc);
            }

            private:

            const TUnaryExpr *UnaryExpr;

          };  // TUnaryExpr<TCst, TBase>::TPin

          TUnaryExpr(const TExpr *expr)
              : Expr(expr) {
            assert(expr);
          }

          /* Pin the array into memory. */
          virtual TBasePin *Pin(void *pin_alloc) const override {
            return new (pin_alloc) TPin(this);
          }

          private:

          const TExpr *Expr;

        };  // Type::TUnaryExpr<TBase>

        template <typename TBase>
        class TUnaryType final
            : public TBase {
          public:

          using TBasePin = typename TBase::TPin;

          class TPin final
              : public TBasePin {
            public:

            TPin(const TUnaryType *unary)
                : UnaryType(unary) {
              assert(unary);
            }

            virtual Sabot::Type::TAny *NewElem(void *type_alloc) const override {
              return NewTypeSabot(UnaryType->Type, type_alloc);
            }

            private:

            const TUnaryType *UnaryType;

          };  // TUnaryType<TCst, TBase>::TPin

          TUnaryType(const TType *type)
              : Type(type) {
            assert(type);
          }

          /* Pin the array into memory. */
          virtual TBasePin *Pin(void *pin_alloc) const override {
            return new (pin_alloc) TPin(this);
          }

          private:

          const TType *Type;

        };  // Type::TUnaryType<TBase>

        template <typename TBase>
        class TBinaryExpr final
            : public TBase {
          public:

          using TBasePin = typename TBase::TPin;

          class TPin final
              : public TBasePin {
            public:

            TPin(const TBinaryExpr *binary)
                : BinaryExpr(binary) {
              assert(binary);
            }

            virtual Sabot::Type::TAny *NewLhs(void *type_alloc) const override {
              return NewTypeSabot(BinaryExpr->Lhs, type_alloc);
            }

            virtual Sabot::Type::TAny *NewRhs(void *type_alloc) const override {
              return NewTypeSabot(BinaryExpr->Rhs, type_alloc);
            }

            private:

            const TBinaryExpr *BinaryExpr;

          };  // TBinaryExpr<TCst, TBase>::TPin

          TBinaryExpr(const TExpr *lhs, const TExpr *rhs)
              : Lhs(lhs), Rhs(rhs) {
            assert(lhs);
            assert(rhs);
          }

          /* Pin the array into memory. */
          virtual TBasePin *Pin(void *pin_alloc) const override {
            return new (pin_alloc) TPin(this);
          }

          private:

          const TExpr *Lhs, *Rhs;

        };  // Type::TBinaryExpr<TBase>

        template <typename TBase>
        class TBinaryType final
            : public TBase {
          public:

          using TBasePin = typename TBase::TPin;

          class TPin final
              : public TBasePin {
            public:

            TPin(const TBinaryType *binary)
                : BinaryType(binary) {
              assert(binary);
            }

            virtual Sabot::Type::TAny *NewLhs(void *type_alloc) const override {
              return NewTypeSabot(BinaryType->Lhs, type_alloc);
            }

            virtual Sabot::Type::TAny *NewRhs(void *type_alloc) const override {
              return NewTypeSabot(BinaryType->Rhs, type_alloc);
            }

            private:

            const TBinaryType *BinaryType;

          };  // TBinaryType<TCst, TBase>::TPin

          TBinaryType(const TType *lhs, const TType *rhs)
              : Lhs(lhs), Rhs(rhs) {
            assert(lhs);
            assert(rhs);
          }

          /* Pin the array into memory. */
          virtual TBasePin *Pin(void *pin_alloc) const override {
            return new (pin_alloc) TPin(this);
          }

          private:

          const TType *Lhs, *Rhs;

        };  // Type::TBinaryType<TBase>

        class TRecordType final
            : public Sabot::Type::TRecord {
          public:

          using TPinBase = Sabot::Type::TRecord::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TRecordType *record_type);

            virtual Sabot::Type::TAny *NewElem(size_t elem_idx, std::string &name, void *type_alloc) const override;

            virtual Sabot::Type::TAny *NewElem(
                size_t elem_idx, void *&out_field_name_sabot_state, void *field_name_state_alloc, void *type_alloc) const override;

            virtual Sabot::Type::TAny *NewElem(size_t elem_idx, void *type_alloc) const override;

            private:

            const TRecordType *RecordType;

          };  // TRecordType::TPin

          TRecordType(const TObjType *type);

          virtual size_t GetElemCount() const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TObjTypeMember *> Members;

        };  // Type::TRecordType

        class TRecordExpr final
            : public Sabot::Type::TRecord {
          public:

          using TPinBase = Sabot::Type::TRecord::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TRecordExpr *record_expr);

            virtual Sabot::Type::TAny *NewElem(size_t elem_idx, std::string &name, void *type_alloc) const override;

            virtual Sabot::Type::TAny *NewElem(
                size_t elem_idx, void *&out_field_name_sabot_state, void *field_name_state_alloc, void *type_alloc) const override;

            virtual Sabot::Type::TAny *NewElem(size_t elem_idx, void *type_alloc) const override;

            private:

            const TRecordExpr *RecordExpr;

          };  // TRecordExpr::TPin

          TRecordExpr(const TObjExpr *expr);

          virtual size_t GetElemCount() const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TObjMember *> Members;

        };  // Type::TRecordExpr

        class TTupleType final
            : public Sabot::Type::TTuple {
          public:

          using TPinBase = Sabot::Type::TTuple::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TTupleType *tuple_type);

            virtual Sabot::Type::TAny *NewElem(size_t elem_idx, void *type_alloc) const override;

            private:

            const TTupleType *TupleType;

          };  // TTupleType::TPin

          TTupleType(const TAddrType *type);

          virtual size_t GetElemCount() const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TAddrTypeMember *> Members;

        };  // Type::TTupleType

        class TTupleExpr final
            : public Sabot::Type::TTuple {
          public:

          using TPinBase = Sabot::Type::TTuple::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TTupleExpr *tuple_expr);

            virtual Sabot::Type::TAny *NewElem(size_t elem_idx, void *type_alloc) const override;

            private:

            const TTupleExpr *TupleExpr;

          };  // TTupleExpr::TPin

          TTupleExpr(const TAddrExpr *expr);

          virtual size_t GetElemCount() const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TAddrMember *> Members;

        };  // Type::TTupleExpr

      }  // Type

      namespace State {

        class TBool final
            : public Sabot::State::TBool {
          public:

          TBool(const TTrueExpr *);

          TBool(const TFalseExpr *);

          virtual const bool &Get() const override;

          virtual Sabot::Type::TBool *GetBoolType(void *type_alloc) const override;

          private:

          bool Val;

        };  // State::TBool

        class TInt final
            : public Sabot::State::TInt64 {
          public:

          TInt(const TIntExpr *expr);

          virtual const int64_t &Get() const override;

          virtual Sabot::Type::TInt64 *GetInt64Type(void *type_alloc) const override;

          private:

          int64_t Val;

        };  // State::TInt

        class TReal final
            : public Sabot::State::TDouble {
          public:

          TReal(const TRealExpr *expr);

          virtual const double &Get() const override;

          virtual Sabot::Type::TDouble *GetDoubleType(void *type_alloc) const override;

          private:

          double Val;

        };  // State::TReal

        class TId final
            : public Sabot::State::TUuid {
          public:

          TId(const TIdExpr *expr);

          virtual const Base::TUuid &Get() const override;

          virtual Sabot::Type::TUuid *GetUuidType(void *type_alloc) const override;

          private:

          Base::TUuid Val;

        };  // State::TId

        class TTimePnt final
            : public Sabot::State::TTimePoint {
          public:

          TTimePnt(const TTimePntExpr *expr);

          virtual const Sabot::TStdTimePoint &Get() const override;

          virtual Sabot::Type::TTimePoint *GetTimePointType(void *type_alloc) const override;

          private:

          Sabot::TStdTimePoint Val;

        };  // State::TTimePnt

        class TTimeDiff final
            : public Sabot::State::TDuration {
          public:

          TTimeDiff(const TTimeDiffExpr *expr);

          virtual const Sabot::TStdDuration &Get() const override;

          virtual Sabot::Type::TDuration *GetDurationType(void *type_alloc) const override;

          private:

          Sabot::TStdDuration Val;

        };  // State::TTimeDiff

        class TStr final
            : public Sabot::State::TStr {
          public:

          using TPinBase = Sabot::State::TStr::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TStr *str);

          };  // TStr::TPin

          TStr(const TSingleQuotedStrExpr *expr);

          TStr(const TDoubleQuotedStrExpr *expr);

          TStr(const TSingleQuotedRawStrExpr *expr);

          TStr(const TDoubleQuotedRawStrExpr *expr);

          virtual size_t GetSize() const override;

          virtual Sabot::Type::TStr *GetStrType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::string Val;

        };  // State::TStr

        class TOpt final
            : public Sabot::State::TOpt {
          public:

          using TPinBase = Sabot::State::TOpt::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TOpt *opt);

            private:

            virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

            const TExpr *Expr;

          };  // TOpt::TPin

          TOpt(const TOptExpr *expr);

          TOpt(const TUnknownExpr *expr);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TOpt *GetOptType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          const TExpr *Expr;

          const TType *Type;

        };  // State::TOpt

        class TDesc final
            : public Sabot::State::TDesc {
          public:

          using TPinBase = Sabot::State::TDesc::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TDesc *set);

            private:

            virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

            const TDesc *Desc;

          };  // TDesc::TPin

          TDesc(const TExpr *expr);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TDesc *GetDescType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          const TExpr *Expr;

        };  // State::TDesc

        class TSet final
            : public Sabot::State::TSet {
          public:

          using TPinBase = Sabot::State::TSet::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TSet *set);

            private:

            virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

            const TSet *Set;

          };  // TSet::TPin

          TSet(const TSetExpr *expr);

          TSet(const TSetType *type);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TSet *GetSetType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TExpr *> Members;

          const TType *Type;

        };  // State::TSet

        class TList final
            : public Sabot::State::TVector {
          public:

          using TPinBase = Sabot::State::TVector::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TList *list);

            private:

            virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

            const TList *List;

          };  // TList::TPin

          TList(const TListExpr *expr);

          TList(const TListType *type);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TVector *GetVectorType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TExpr *> Members;

          const TType *Type;

        };  // State::TList

        class TDict final
            : public Sabot::State::TMap {
          public:

          using TPinBase = Sabot::State::TMap::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TDict *list);

            private:

            virtual Sabot::State::TAny *NewLhsInRange(size_t elem_idx, void *state_alloc) const override;

            virtual Sabot::State::TAny *NewRhsInRange(size_t elem_idx, void *state_alloc) const override;

            const TDict *Dict;

          };  // TDict::TPin

          TDict(const TDictExpr *expr);

          TDict(const TDictType *type);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TMap *GetMapType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          std::vector<const TDictMember *> Members;

          const TType *LhsType, *RhsType;

        };  // State::TDict

        class TObj final
            : public Sabot::State::TRecord {
          public:

          using TPinBase = Sabot::State::TRecord::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TObj *obj);

            private:

            virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

            const TObj *Obj;

          };  // TObj::TPin

          TObj(const TObjExpr *expr);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TRecord *GetRecordType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          const TObjExpr *Expr;

          std::vector<const TObjMember *> Members;

        };  // State::TObj

        class TAddr final
            : public Sabot::State::TTuple {
          public:

          using TPinBase = Sabot::State::TTuple::TPin;

          class TPin final
              : public TPinBase {
            public:

            TPin(const TAddr *addr);

            private:

            virtual Sabot::State::TAny *NewElemInRange(size_t elem_idx, void *state_alloc) const override;

            const TAddr *Addr;

          };  // TAddr::TPin

          TAddr(const TAddrExpr *expr);

          virtual size_t GetElemCount() const override;

          virtual Sabot::Type::TTuple *GetTupleType(void *type_alloc) const override;

          virtual TPinBase *Pin(void *alloc) const override;

          private:

          const TAddrExpr *Expr;

          std::vector<const TAddrMember *> Members;

        };  // State::TAddr

      }  // State

    }  // Program

  }  // Client

}  // Orly
