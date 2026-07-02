/* <orly/code_gen/type.cc>

   Implements <orly/code_gen/type.h>.

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

#include <orly/code_gen/type.h>

#include <base/not_implemented.h>
#include <base/split.h>
#include <orly/type/group_ref.h>
#include <orly/type/rec_group.h>
#include <orly/type/variant.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Type;

template <size_t N>
using TTypeArray = array<TType, N>;

class TCodeGenVisitor : public TType::TVisitor {
  public:

  /* `equation_mode` is set while emitting a group's member equations for
     the MakeRecGroup reconstruction (see EmitGroupMember): there, a group
     reference is a placeholder (empty group id + member index) rather than a
     nested reconstruction, and a member variant is emitted as its own arms
     rather than re-reconstructed. */
  TCodeGenVisitor(ostream &strm, bool equation_mode = false)
      : Strm(strm), EquationMode(equation_mode) {}
  virtual ~TCodeGenVisitor() {}

  private:
  virtual void operator()(const TAddr *that) const {
    Strm
      << "Orly::Type::TAddr::Get(Orly::Type::TAddr::TElems{"
      << Base::Join(that->GetElems(),
                    ", ",
                    [this](ostream &strm, const pair<TAddrDir, TType> &elem) {
                      strm << '{';
                      WriteCppType(strm, elem.first) << ", ";
                      elem.second.Accept(*this);
                      strm << '}';
                    })
      << "})";
  }

  virtual void operator()(const TAny *) const {NOT_IMPLEMENTED();}
  virtual void operator()(const TBool *) const {
    Write("TBool");
  }
  virtual void operator()(const TDict *that) const {
    Write("TDict", that->GetKey(), that->GetVal());
  }
  virtual void operator()(const TErr *that) const {
    Write("TErr", that->GetElem());
  }
  virtual void operator()(const TFunc *that) const {
    Write("TFunc", that->GetParamObject(), that->GetReturnType());
  }
  virtual void operator()(const TId *) const {
    Write("TId");
  }
  virtual void operator()(const TInt *) const {
    Write("TInt");
  }
  virtual void operator()(const TList *that) const {
    Write("TList", {that->GetElem()});
  }
  virtual void operator()(const TMutable *that) const {
    Write("TMutable", that->GetAddr(), that->GetVal());
  }
  virtual void operator()(const TObj *that) const {
  const TObj::TElems &elem_map = that->GetElems();
    Strm
      << "Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType>{"
      << Base::Join(elem_map,
                    ", ",
                    [this](ostream &strm, const TObj::TElems::value_type &that) {
                      strm << "{std::string(\"" << that.first << "\"), ";
                      that.second.Accept(*this);
                      strm << '}';
                    })
      << "})";
  }
  virtual void operator()(const TVariant *that) const {
    /* A member of a mutually-recursive group (#116) is reconstructed at
       runtime via MakeRecGroup, not as a plain variant -- otherwise its
       group-ref arms would have no interned identity. In equation mode we
       are *inside* such a reconstruction, so emit the member's arms plainly. */
    if (!EquationMode) {
      std::vector<TType> members;
      size_t index;
      if (TryGetGroupMembers(that->AsType(), members, index)) {
        EmitGroupMember(members, index);
        return;
      }
    }
    const TVariant::TElems &elem_map = that->GetElems();
    Strm
      << "Orly::Type::TVariant::Get(std::map<std::string, Orly::Type::TType>{"
      << Base::Join(elem_map,
                    ", ",
                    [this](ostream &strm, const TVariant::TElems::value_type &that) {
                      strm << "{std::string(\"" << that.first << "\"), ";
                      that.second.Accept(*this);
                      strm << '}';
                    })
      << "})";
  }
  /* Reconstructs the self-reference leaf inside a generated TDt<...>
     specialization (the only place a self-ref's type code is emitted). */
  virtual void operator()(const TSelfRef *that) const {
    Strm << "Orly::Type::TSelfRef::Get(" << that->GetDepth() << ')';
  }
  /* A reference into a mutually-recursive group (#116). In equation mode it
     is a placeholder; otherwise it reconstructs and indexes the group. */
  virtual void operator()(const TGroupRef *that) const {
    if (EquationMode) {
      Strm << "Orly::Type::TGroupRef::Get({}, " << that->GetIndex() << ')';
      return;
    }
    std::vector<TType> members;
    size_t index;
    bool ok = TryGetGroupMembers(ResolveGroupRef(that), members, index);
    assert(ok);
    (void)ok;
    EmitGroupMember(members, index);
  }
  virtual void operator()(const TOpt *that) const {
    Write("TOpt", that->GetElem());
  }
  virtual void operator()(const TReal *) const {
    Write("TReal");
  }
  virtual void operator()(const TSeq *that) const {
    Write("TSeq", that->GetElem());
  }
  virtual void operator()(const TSet *that) const {
    Write("TSet", that->GetElem());
  }
  virtual void operator()(const TStr *) const {
    Write("TStr");
  }
  virtual void operator()(const TTimeDiff *) const {
    Write("TTimeDiff");
  }
  virtual void operator()(const TTimePnt *) const {
    Write("TTimePnt");
  }

  void Write(const char *name) const {
    Write<0>(name, TTypeArray<0>());
  }
  void Write(const char *name, const TType &type) const {
    Write<1>(name, {{type}});
  }

  void Write(const char *name, const TType &t1, const TType &t2) const {
    Write<2>(name, {{t1, t2}});
  }

  template <size_t N>
  void Write(const char *name, const TTypeArray<N> &elems) const {
    Strm
      << "Orly::Type::" << name << "::Get("
      << Base::Join(elems,
                    ", ",
                    [this](ostream &, const TType &type) {
                      type.Accept(*this);
                    })
      << ')';
  }

  /* Emit an expression that rebuilds the whole mutually-recursive group via
     Orly::Type::MakeRecGroup and yields member `index`. The members are
     emitted as equations (arms with placeholder group refs), so the result
     is the canonical interned member type at runtime. */
  void EmitGroupMember(const std::vector<TType> &members, size_t index) const {
    Strm << "([]() -> Orly::Type::TType { auto __g = Orly::Type::MakeRecGroup("
         << "std::vector<Orly::Type::TVariantElems>{";
    for (size_t k = 0; k < members.size(); ++k) {
      if (k) { Strm << ", "; }
      const auto &arms = members[k].As<TVariant>()->GetElems();
      Strm << "Orly::Type::TVariantElems{"
           << Base::Join(arms,
                         ", ",
                         [](ostream &strm, const TVariant::TElems::value_type &arm) {
                           strm << "{std::string(\"" << arm.first << "\"), ";
                           arm.second.Accept(TCodeGenVisitor(strm, true));
                           strm << '}';
                         })
           << '}';
    }
    Strm << "}); return __g[" << index << "]; }())";
  }

  ostream &Strm;
  bool EquationMode;
};

void Orly::CodeGen::GenCode(ostream &strm, const TType &type) {
  type.Accept(TCodeGenVisitor(strm));
}
