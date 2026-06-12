/* <orly/type/unroll.cc>

   Implements <orly/type/unroll.h>.

   Both walkers handle every compound the type system can nest, even
   though the v1 surface (orly/synth/type_def.cc placement validation)
   only lets a self-reference appear as an arm payload or as a field of
   an arm's payload record -- the walkers shouldn't silently miss a leaf
   if that restriction is later relaxed.

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

#include <orly/type/unroll.h>

#include <orly/type.h>

using namespace Orly;
using namespace Orly::Type;

bool Type::HasSelfRef(const TType &type) {
  class TVisitor
      : public TType::TVisitor {
    public:
    TVisitor(bool &found) : Found(found) {}
    virtual void operator()(const TSelfRef *) const { Found = true; }
    virtual void operator()(const TVariant *that) const {
      for (const auto &elem : that->GetElems()) {
        if (Found) { return; }
        elem.second.Accept(*this);
      }
    }
    virtual void operator()(const TObj *that) const {
      for (const auto &elem : that->GetElems()) {
        if (Found) { return; }
        elem.second.Accept(*this);
      }
    }
    virtual void operator()(const TAddr *that) const {
      for (const auto &elem : that->GetElems()) {
        if (Found) { return; }
        elem.second.Accept(*this);
      }
    }
    virtual void operator()(const TDict *that) const {
      that->GetKey().Accept(*this);
      if (!Found) { that->GetVal().Accept(*this); }
    }
    virtual void operator()(const TErr *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TList *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TOpt *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TSeq *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TSet *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TFunc *that) const {
      that->GetParamObject().Accept(*this);
      if (!Found) { that->GetReturnType().Accept(*this); }
    }
    virtual void operator()(const TMutable *that) const {
      that->GetAddr().Accept(*this);
      if (!Found) { that->GetVal().Accept(*this); }
    }
    virtual void operator()(const TAny *) const {}
    virtual void operator()(const TBool *) const {}
    virtual void operator()(const TId *) const {}
    virtual void operator()(const TInt *) const {}
    virtual void operator()(const TReal *) const {}
    virtual void operator()(const TStr *) const {}
    virtual void operator()(const TTimeDiff *) const {}
    virtual void operator()(const TTimePnt *) const {}
    private:
    bool &Found;
  };  // TVisitor
  bool found = false;
  type.Accept(TVisitor(found));
  return found;
}

/* The depth-tracking walk behind HasFreeSelfRef(). */
static bool HasFreeSelfRefAtDepth(const TType &type, size_t depth) {
  class TVisitor
      : public TType::TVisitor {
    public:
    TVisitor(bool &found, size_t depth) : Found(found), Depth(depth) {}
    virtual void operator()(const TSelfRef *that) const {
      if (that->GetDepth() >= Depth) {
        Found = true;
      }
    }
    virtual void operator()(const TVariant *that) const {
      for (const auto &elem : that->GetElems()) {
        if (Found) { return; }
        Found = HasFreeSelfRefAtDepth(elem.second, Depth + 1);
      }
    }
    virtual void operator()(const TObj *that) const {
      for (const auto &elem : that->GetElems()) {
        if (Found) { return; }
        elem.second.Accept(*this);
      }
    }
    virtual void operator()(const TAddr *that) const {
      for (const auto &elem : that->GetElems()) {
        if (Found) { return; }
        elem.second.Accept(*this);
      }
    }
    virtual void operator()(const TDict *that) const {
      that->GetKey().Accept(*this);
      if (!Found) { that->GetVal().Accept(*this); }
    }
    virtual void operator()(const TErr *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TList *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TOpt *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TSeq *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TSet *that) const { that->GetElem().Accept(*this); }
    virtual void operator()(const TFunc *that) const {
      that->GetParamObject().Accept(*this);
      if (!Found) { that->GetReturnType().Accept(*this); }
    }
    virtual void operator()(const TMutable *that) const {
      that->GetAddr().Accept(*this);
      if (!Found) { that->GetVal().Accept(*this); }
    }
    virtual void operator()(const TAny *) const {}
    virtual void operator()(const TBool *) const {}
    virtual void operator()(const TId *) const {}
    virtual void operator()(const TInt *) const {}
    virtual void operator()(const TReal *) const {}
    virtual void operator()(const TStr *) const {}
    virtual void operator()(const TTimeDiff *) const {}
    virtual void operator()(const TTimePnt *) const {}
    private:
    bool &Found;
    size_t Depth;
  };  // TVisitor
  bool found = false;
  type.Accept(TVisitor(found, depth));
  return found;
}

bool Type::HasFreeSelfRef(const TType &type) {
  return HasFreeSelfRefAtDepth(type, 0);
}

/* The depth-tracking rewrite behind Unroll(). */
static TType UnrollAtDepth(const TType &type, const TType &binder, size_t depth) {
  class TVisitor
      : public TType::TVisitor {
    public:
    TVisitor(TType &out, const TType &binder, size_t depth)
        : Out(out), Binder(binder), Depth(depth) {}
    virtual void operator()(const TSelfRef *that) const {
      /* Deeper references are bound by a variant nested inside the type
         being unrolled; they travel with it unchanged. References deeper
         than every enclosing binder can't be constructed by the synth
         layer, so substituting only the exact depth is exhaustive. */
      Out = that->GetDepth() == Depth ? Binder : that->AsType();
    }
    virtual void operator()(const TVariant *that) const {
      TVariantElems elems;
      for (const auto &elem : that->GetElems()) {
        elems.emplace(elem.first, UnrollAtDepth(elem.second, Binder, Depth + 1));
      }
      Out = TVariant::Get(elems);
    }
    virtual void operator()(const TObj *that) const {
      TObjElems elems;
      for (const auto &elem : that->GetElems()) {
        elems.emplace(elem.first, UnrollAtDepth(elem.second, Binder, Depth));
      }
      Out = TObj::Get(elems);
    }
    virtual void operator()(const TAddr *that) const {
      TAddrElems elems;
      for (const auto &elem : that->GetElems()) {
        elems.emplace_back(elem.first, UnrollAtDepth(elem.second, Binder, Depth));
      }
      Out = TAddr::Get(elems);
    }
    virtual void operator()(const TDict *that) const {
      Out = TDict::Get(UnrollAtDepth(that->GetKey(), Binder, Depth),
                       UnrollAtDepth(that->GetVal(), Binder, Depth));
    }
    virtual void operator()(const TErr *that) const {
      Out = TErr::Get(UnrollAtDepth(that->GetElem(), Binder, Depth));
    }
    virtual void operator()(const TList *that) const {
      Out = TList::Get(UnrollAtDepth(that->GetElem(), Binder, Depth));
    }
    virtual void operator()(const TOpt *that) const {
      Out = TOpt::Get(UnrollAtDepth(that->GetElem(), Binder, Depth));
    }
    virtual void operator()(const TSeq *that) const {
      Out = TSeq::Get(UnrollAtDepth(that->GetElem(), Binder, Depth));
    }
    virtual void operator()(const TSet *that) const {
      Out = TSet::Get(UnrollAtDepth(that->GetElem(), Binder, Depth));
    }
    virtual void operator()(const TFunc *that) const {
      Out = TFunc::Get(UnrollAtDepth(that->GetParamObject(), Binder, Depth),
                       UnrollAtDepth(that->GetReturnType(), Binder, Depth));
    }
    virtual void operator()(const TMutable *that) const {
      Out = TMutable::Get(UnrollAtDepth(that->GetAddr(), Binder, Depth),
                          UnrollAtDepth(that->GetVal(), Binder, Depth));
    }
    virtual void operator()(const TAny *that) const { Out = that->AsType(); }
    virtual void operator()(const TBool *that) const { Out = that->AsType(); }
    virtual void operator()(const TId *that) const { Out = that->AsType(); }
    virtual void operator()(const TInt *that) const { Out = that->AsType(); }
    virtual void operator()(const TReal *that) const { Out = that->AsType(); }
    virtual void operator()(const TStr *that) const { Out = that->AsType(); }
    virtual void operator()(const TTimeDiff *that) const { Out = that->AsType(); }
    virtual void operator()(const TTimePnt *that) const { Out = that->AsType(); }
    private:
    TType &Out;
    const TType &Binder;
    size_t Depth;
  };  // TVisitor
  if (!HasSelfRef(type)) {
    return type;
  }
  TType out;
  type.Accept(TVisitor(out, binder, depth));
  return out;
}

TType Type::Unroll(const TType &type, const TType &binder) {
  return UnrollAtDepth(type, binder, 0);
}
