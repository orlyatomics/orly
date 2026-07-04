/* <orly/var/jsonify.cc>

   Implements <orly/var/jsonify.h>.

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

#include <orly/var/jsonify.h>

#include <cmath>
#include <sstream>

#include <base/json.h>
#include <base/not_implemented.h>
#include <orly/type/orlyify.h>
#include <orly/var.h>

using namespace std;
using namespace Orly;
using namespace Orly::Var;

void Orly::Var::Jsonify(ostream &strm, const TVar &var) {
  class TJsonifyVisitor : public TVar::TVisitor {
    NO_COPY(TJsonifyVisitor);
    public:
    TJsonifyVisitor(ostream &strm) : Strm(strm) {}
    private:
    virtual void operator()(const TAddr *that) const {
      Strm << '[';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        iter.second.Accept(*this);
        ++num_args;
      }
      Strm << ']';
    }
    virtual void operator()(const TBool *that) const {
      Strm << boolalpha << that->GetVal();
    }
    virtual void operator()(const TDict *that) const {
      Strm << '{';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }

        // We're actually encoding a JSON Object here, since there is no support
        // for dictionaries.  So, if the key is not a string we render its JSON
        // form and emit that as a (properly escaped) string -- bare quotes
        // around a composite key that itself contains strings would produce
        // nested unescaped quotes, i.e. invalid JSON (#276).
        if(iter.first.GetType() != Type::TStr::Get()) {
          ostringstream key_strm;
          iter.first.Accept(TJsonifyVisitor(key_strm));
          Base::TJson::WriteString(Strm, key_strm.str());
        }
        else {
          iter.first.Accept(*this);
        }
        Strm << ':';
        iter.second.Accept(*this);
        ++num_args;
      }
      Strm << '}';
    }
    virtual void operator()(const TErr *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TFree *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TId *that) const {
      Strm << '"' << that->GetVal() << '"';
    }
    virtual void operator()(const TInt *that) const {
      Strm << that->GetVal();
    }
    virtual void operator()(const TList *that) const {
      Strm << '[';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        iter.Accept(*this);
        ++num_args;
      }
      Strm << ']';
    }
    virtual void operator()(const TMutable *that) const {
      that->GetVal().Accept(*this);
    }
    virtual void operator()(const TObj *that) const {
      Strm << '{';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        Strm << '"' << iter.first << "\":";
        iter.second.Accept(*this);
        ++num_args;
      }
      Strm << '}';
    }
    virtual void operator()(const TOpt *that) const {
      if (that->GetVal().IsKnown()) {
        that->GetVal().GetVal().Accept(*this);
      } else {
        Strm << "null";
      }
    }
    virtual void operator()(const TReal *that) const {
      /* JSON has no NaN / Infinity literals; the conventional lenient
         mapping is null (#276). */
      if (isfinite(that->GetVal())) {
        Strm << showpoint << that->GetVal();
      } else {
        Strm << "null";
      }
    }
    virtual void operator()(const TSet *that) const {
      Strm << '[';
      size_t num_args = 0;
      for (auto iter : that->GetVal()) {
        if (num_args > 0) {
          Strm << ',';
        }
        iter.Accept(*this);
        ++num_args;
      }
      Strm << ']';
    }
    virtual void operator()(const TStr *that) const {
      Base::TJson::WriteString(Strm, that->GetVal());
    }
    virtual void operator()(const TTimeDiff *that) const {
      Strm << that->GetVal().count();
    }
    virtual void operator()(const TTimePnt *that) const {
      Strm << Base::Chrono::TimeDiffCast(that->GetVal().time_since_epoch()).count();
    }
    virtual void operator()(const TVariant *that) const {
      /* A variant value serializes as the single-key object
         {"<tag>": <payload>}, byte-consistent with the single-key-record
         storage reuse described in issue #95. */
      Strm << "{\"" << that->GetTag() << "\":";
      that->GetVal().Accept(*this);
      Strm << '}';
    }
    ostream &Strm;
  };
  var.Accept(TJsonifyVisitor(strm));
}

Base::TJson Orly::Var::ToJson(const TVar &var) {
  using TJson = Base::TJson;
  class TToJsonVisitor : public TVar::TVisitor {
    NO_COPY(TToJsonVisitor);
    public:
    TToJsonVisitor(TJson &out) : Out(out) {}
    private:
    virtual void operator()(const TAddr *that) const {
      TJson::TArray arr;
      for (auto iter : that->GetVal()) {
        arr.emplace_back(ToJson(iter.second));
      }
      Out = TJson(move(arr));
    }
    virtual void operator()(const TBool *that) const {
      Out = TJson(that->GetVal());
    }
    virtual void operator()(const TDict *that) const {
      TJson::TObject obj;
      for (auto iter : that->GetVal()) {
        /* Same key convention as Jsonify (which the wire format inherited
           through the old parse round-trip): a string key is used verbatim;
           any other key type appears as the compact JSON text of the key
           (#276, #377). */
        if (iter.first.GetType() != Type::TStr::Get()) {
          ostringstream key_strm;
          Jsonify(key_strm, iter.first);
          obj[key_strm.str()] = ToJson(iter.second);
        } else {
          obj[ToJson(iter.first).GetString()] = ToJson(iter.second);
        }
      }
      Out = TJson(move(obj));
    }
    virtual void operator()(const TErr *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TFree *) const {NOT_IMPLEMENTED();}
    virtual void operator()(const TId *that) const {
      ostringstream strm;
      strm << that->GetVal();
      Out = TJson(strm.str());
    }
    virtual void operator()(const TInt *that) const {
      Out = TJson(that->GetVal());
    }
    virtual void operator()(const TList *that) const {
      TJson::TArray arr;
      for (auto iter : that->GetVal()) {
        arr.emplace_back(ToJson(iter));
      }
      Out = TJson(move(arr));
    }
    virtual void operator()(const TMutable *that) const {
      Out = ToJson(that->GetVal());
    }
    virtual void operator()(const TObj *that) const {
      TJson::TObject obj;
      for (auto iter : that->GetVal()) {
        obj[iter.first] = ToJson(iter.second);
      }
      Out = TJson(move(obj));
    }
    virtual void operator()(const TOpt *that) const {
      if (that->GetVal().IsKnown()) {
        Out = ToJson(that->GetVal().GetVal());
      } else {
        Out = TJson();
      }
    }
    virtual void operator()(const TReal *that) const {
      /* JSON has no NaN / Infinity literals; the conventional lenient
         mapping is null (#276). */
      if (isfinite(that->GetVal())) {
        Out = TJson(that->GetVal());
      } else {
        Out = TJson();
      }
    }
    virtual void operator()(const TSet *that) const {
      TJson::TArray arr;
      for (auto iter : that->GetVal()) {
        arr.emplace_back(ToJson(iter));
      }
      Out = TJson(move(arr));
    }
    virtual void operator()(const TStr *that) const {
      Out = TJson(that->GetVal());
    }
    virtual void operator()(const TTimeDiff *that) const {
      Out = TJson(that->GetVal().count());
    }
    virtual void operator()(const TTimePnt *that) const {
      Out = TJson(Base::Chrono::TimeDiffCast(that->GetVal().time_since_epoch()).count());
    }
    virtual void operator()(const TVariant *that) const {
      /* A variant value serializes as the single-key object
         {"<tag>": <payload>}, byte-consistent with the single-key-record
         storage reuse described in issue #95. */
      TJson::TObject obj;
      obj[that->GetTag()] = ToJson(that->GetVal());
      Out = TJson(move(obj));
    }
    TJson &Out;
  };
  TJson result;
  var.Accept(TToJsonVisitor(result));
  return result;
}