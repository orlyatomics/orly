/* <orly/sabot/match_prefix_type.cc>

   Implements <orly/sabot/match_prefix_type.h>.

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

#include <orly/sabot/match_prefix_type.h>

#include <string>

using namespace std;
using namespace Orly;
using namespace Orly::Sabot;

namespace {

  /* One record enclosing the current position on one side of the match, kept
     so a recursive leaf there can be resolved to the variant it refers back
     to.  Each node lives in the frame of the record visit that pushed it. */
  struct TEnclosing {
    const Type::TRecord *Record;
    const TEnclosing *Next;
  };

  /* True iff this record is the stored form of a variant: it carries the
     `$which` sentinel field, which no user record can name. */
  bool IsVariantRecord(const Type::TRecord &record) {
    void *pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize());
    Type::TRecord::TPin::TWrapper pin(record.Pin(pin_alloc));
    void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
    for (size_t elem_idx = 0; elem_idx < record.GetElemCount(); ++elem_idx) {
      string name;
      Type::TAny::TWrapper elem(pin->NewElem(elem_idx, name, type_alloc));
      if (name == "$which") {
        return true;
      }
    }
    return false;
  }

  /* The variant a recursive leaf of the given de Bruijn depth refers back to
     (0 is the nearest enclosing variant), or null if there are not that many
     enclosing variants. */
  const TEnclosing *FindBinder(const TEnclosing *enclosing, uint64_t depth) {
    for (; enclosing; enclosing = enclosing->Next) {
      if (IsVariantRecord(*enclosing->Record)) {
        if (!depth) {
          return enclosing;
        }
        --depth;
      }
    }
    return nullptr;
  }

}  // namespace

class TMatchPrefixTypeVisitor final
    : public TTypeDoubleVisitor {
  public:

  TMatchPrefixTypeVisitor(TMatchResult &out, const TEnclosing *lhs_enclosing = nullptr, const TEnclosing *rhs_enclosing = nullptr)
      : Result(out), LhsEnclosing(lhs_enclosing), RhsEnclosing(rhs_enclosing) {
  }

  /* Overrides. */
  virtual void operator()(const Type::TInt8 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TInt8 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TInt16 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TInt16 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TInt32 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TInt32 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TInt64 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TInt64 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TUInt8 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TUInt8 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TUInt16 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TUInt16 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TUInt32 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TUInt32 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TUInt64 &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TUInt64 &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TBool &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TBool &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TChar &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TChar &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TFloat &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TFloat &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TDouble &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TDouble &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TDuration &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TDuration &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TTimePoint &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TTimePoint &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TUuid &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TUuid &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TBlob &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TBlob &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TStr &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TStr &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TTombstone &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TTombstone &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TVoid &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TVoid &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TDesc &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TDesc &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TFree &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TFree &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TOpt &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TOpt &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TSet &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TSet &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TVector &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TVector &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TMap &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TMap &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TRecord &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TRecord &lhs, const Type::TTuple &rhs    ) const override;

  virtual void operator()(const Type::TTuple &lhs, const Type::TInt8 &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TInt16 &rhs    ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TInt32 &rhs    ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TInt64 &rhs    ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TUInt8 &rhs    ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TUInt16 &rhs   ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TUInt32 &rhs   ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TUInt64 &rhs   ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TBool &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TChar &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TFloat &rhs    ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TDouble &rhs   ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TDuration &rhs ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TTimePoint &rhs) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TUuid &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TBlob &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TStr &rhs      ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TTombstone &rhs) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TVoid &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TDesc &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TFree &rhs     ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TOpt &rhs      ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TSet &rhs      ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TVector &rhs   ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TMap &rhs      ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TRecord &rhs   ) const override;
  virtual void operator()(const Type::TTuple &lhs, const Type::TTuple &rhs    ) const override;

  /* The recursive back-reference leaf (issue #115), which AcceptDouble routes
     here instead of through the N*N matrix. */
  virtual void OnSelfRef(const Type::TAny &lhs, const Type::TAny &rhs) const override;

  private:

  inline void OnUnary(const Type::TUnary &lhs, const Type::TUnary &rhs) const;

  inline void OnBinary(const Type::TBinary &lhs, const Type::TBinary &rhs) const;

  inline void DidNotMatch() const;

  inline void DidNotUnify() const;

  inline void OnFree(const Type::TFree &lhs, const Type::TAny &rhs) const;

  TMatchResult &Result;

  /* The records enclosing the current position on each side, innermost
     first.  See OnSelfRef. */
  const TEnclosing *LhsEnclosing;
  const TEnclosing *RhsEnclosing;

};  // TMatchPrefixTypeVisitor

inline void TMatchPrefixTypeVisitor::OnUnary(const Type::TUnary &lhs, const Type::TUnary &rhs) const {
  void *lhs_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize() * 2);
  void *rhs_pin_alloc = reinterpret_cast<uint8_t *>(lhs_pin_alloc) + Sabot::Type::GetMaxTypePinSize();
  Type::TUnary::TPin::TWrapper
    lhs_pin(lhs.Pin(lhs_pin_alloc)),
    rhs_pin(rhs.Pin(rhs_pin_alloc));
  void *lhs_type_alloc = alloca(Sabot::Type::GetMaxTypeSize() * 2);
  void *rhs_type_alloc = reinterpret_cast<uint8_t *>(lhs_type_alloc) + Sabot::Type::GetMaxTypeSize();
  AcceptDouble(*Type::TAny::TWrapper(lhs_pin->NewElem(lhs_type_alloc)),
               *Type::TAny::TWrapper(rhs_pin->NewElem(rhs_type_alloc)),
               *this);
}

inline void TMatchPrefixTypeVisitor::OnBinary(const Type::TBinary &lhs, const Type::TBinary &rhs) const {
  void *lhs_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize() * 2);
  void *rhs_pin_alloc = reinterpret_cast<uint8_t *>(lhs_pin_alloc) + Sabot::Type::GetMaxTypePinSize();
  Type::TBinary::TPin::TWrapper
    lhs_pin(lhs.Pin(lhs_pin_alloc)),
    rhs_pin(rhs.Pin(rhs_pin_alloc));
  void *lhs_type_alloc = alloca(Sabot::Type::GetMaxTypeSize() * 2);
  void *rhs_type_alloc = reinterpret_cast<uint8_t *>(lhs_type_alloc) + Sabot::Type::GetMaxTypeSize();
  AcceptDouble(*Type::TAny::TWrapper(lhs_pin->NewLhs(lhs_type_alloc)),
               *Type::TAny::TWrapper(rhs_pin->NewLhs(rhs_type_alloc)),
               *this);
  if (!IsNoMatch(Result)) {
    AcceptDouble(*Type::TAny::TWrapper(lhs_pin->NewRhs(lhs_type_alloc)),
                 *Type::TAny::TWrapper(rhs_pin->NewRhs(rhs_type_alloc)),
                 *this);
  }
}

inline void TMatchPrefixTypeVisitor::DidNotMatch() const {
  Result = TMatchResult::NoMatch;
}

inline void TMatchPrefixTypeVisitor::DidNotUnify() const {
  switch (Result) {
    case TMatchResult::NoMatch: {
      break;
    }
    case TMatchResult::PrefixMatch: {
      break;
    }
    case TMatchResult::Unifies: {
      Result = TMatchResult::PrefixMatch;
      break;
    }
  }
}

/* A recursive leaf (issue #115) in a key's type.  Two leaves at corresponding
   positions match iff they refer back to the same binder (same de Bruijn
   depth), as in CompareTypes.

   A leaf can also meet a concrete type, because a stored key's type is
   rebuilt from its value: a taken recursive arm comes back with the payload
   variant spelled out, while the declared type (a `free::(T)` slot, say)
   keeps the leaf there.  The two describe the same type, so the leaf is
   unrolled to the variant it refers to and matching continues against that.
   This terminates: each unrolling is paid for by structure on the other side,
   and a concrete type is finite.

   A free pattern slot unifies with whatever its element type matches; a free
   on the key side is as unreachable here as it is in the matrix. */
void TMatchPrefixTypeVisitor::OnSelfRef(const Type::TAny &lhs, const Type::TAny &rhs) const {
  const bool lhs_is_ref = lhs.IsRecursiveRef();
  const bool rhs_is_ref = rhs.IsRecursiveRef();
  if (lhs_is_ref && rhs_is_ref) {
    if (static_cast<const Type::TSelfRef &>(lhs).GetDepth() != static_cast<const Type::TSelfRef &>(rhs).GetDepth()) {
      DidNotMatch();
    }
    return;
  }
  if (const auto *free = dynamic_cast<const Type::TFree *>(&lhs)) {
    OnFree(*free, rhs);
    return;
  }
  if (dynamic_cast<const Type::TFree *>(&rhs)) {
    throw TUnreachable();
  }
  if (lhs_is_ref) {
    const TEnclosing *binder = FindBinder(LhsEnclosing, static_cast<const Type::TSelfRef &>(lhs).GetDepth());
    if (!binder) {
      DidNotMatch();
      return;
    }
    AcceptDouble(*binder->Record, rhs, TMatchPrefixTypeVisitor(Result, binder->Next, RhsEnclosing));
  } else {
    const TEnclosing *binder = FindBinder(RhsEnclosing, static_cast<const Type::TSelfRef &>(rhs).GetDepth());
    if (!binder) {
      DidNotMatch();
      return;
    }
    AcceptDouble(lhs, *binder->Record, TMatchPrefixTypeVisitor(Result, LhsEnclosing, binder->Next));
  }
}

inline void TMatchPrefixTypeVisitor::OnFree(const Type::TFree &lhs, const Type::TAny &rhs) const {
  TMatchResult cur = TMatchResult::Unifies;
  void *lhs_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize());
  Type::TUnary::TPin::TWrapper lhs_pin(lhs.Pin(lhs_pin_alloc));
  void *lhs_type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  AcceptDouble(*Type::TAny::TWrapper(lhs_pin->NewElem(lhs_type_alloc)), rhs, TMatchPrefixTypeVisitor(cur, LhsEnclosing, RhsEnclosing));
  switch (cur) {
    case TMatchResult::NoMatch: {
      DidNotUnify();
      break;
    }
    case TMatchResult::PrefixMatch: {
      throw std::runtime_error("We do not currently support Free<Free<T>>");
      break;
    }
    case TMatchResult::Unifies: {
      break;
    }
  }
}

void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt8 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt16 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt32 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TInt64 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt8 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt16 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt32 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUInt64 &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TBool &/*rhs*/     ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBool &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TChar &/*rhs*/     ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TChar &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFloat &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDouble &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDuration &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTimePoint &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TUuid &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TBlob &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TStr &/*rhs*/      ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TStr &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TTombstone &/*rhs*/) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTombstone &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { /* Matches */  }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVoid &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &lhs,     const Type::TDesc &rhs         ) const { OnUnary(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TDesc &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TFree &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TInt8 &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TInt16 &rhs    ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TInt32 &rhs    ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TInt64 &rhs    ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TUInt8 &rhs    ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TUInt16 &rhs   ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TUInt32 &rhs   ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TUInt64 &rhs   ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TBool &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TChar &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TFloat &rhs    ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TDouble &rhs   ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TDuration &rhs ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TTimePoint &rhs) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TUuid &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TBlob &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TStr &rhs      ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TTombstone &rhs) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TVoid &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TDesc &rhs     ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TOpt &rhs      ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TSet &rhs      ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TVector &rhs   ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TMap &rhs      ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TRecord &rhs   ) const { OnFree(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TFree &lhs, const Type::TTuple &rhs    ) const { OnFree(lhs, rhs); }


void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &lhs,     const Type::TOpt &rhs          ) const { OnUnary(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TOpt &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &lhs,     const Type::TSet &rhs          ) const { OnUnary(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TSet &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &lhs,     const Type::TVector &rhs       ) const { OnUnary(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TVector &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &lhs,     const Type::TMap &rhs          ) const { OnBinary(lhs, rhs); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TMap &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &lhs,     const Type::TRecord &rhs       ) const {
  size_t lhs_count = lhs.GetElemCount();
  size_t rhs_count = rhs.GetElemCount();
  if (lhs_count != rhs_count) {
    DidNotMatch();
  } else {
    void *lhs_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize() * 2);
    void *rhs_pin_alloc = reinterpret_cast<uint8_t *>(lhs_pin_alloc) + Sabot::Type::GetMaxTypePinSize();
    Type::TRecord::TPin::TWrapper
      lhs_pin(lhs.Pin(lhs_pin_alloc)),
      rhs_pin(rhs.Pin(rhs_pin_alloc));
    void *lhs_type_alloc = alloca(Sabot::Type::GetMaxTypeSize() * 2);
    void *rhs_type_alloc = reinterpret_cast<uint8_t *>(lhs_type_alloc) + Sabot::Type::GetMaxTypeSize();
    void *lhs_type_alloc_2 = alloca(Sabot::Type::GetMaxTypeSize() * 2);
    void *rhs_type_alloc_2 = reinterpret_cast<uint8_t *>(lhs_type_alloc_2) + Sabot::Type::GetMaxTypeSize();
    Sabot::State::TAny
      *lhs_field_name_state,
      *rhs_field_name_state;
    /* The fields see these records as enclosing them (see OnSelfRef). */
    const TEnclosing lhs_enclosing { &lhs, LhsEnclosing }, rhs_enclosing { &rhs, RhsEnclosing };
    const TMatchPrefixTypeVisitor field_visitor(Result, &lhs_enclosing, &rhs_enclosing);
    void *lhs_field_name_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
    void *rhs_field_name_state_alloc = reinterpret_cast<uint8_t *>(lhs_field_name_state_alloc) + Sabot::State::GetMaxStateSize();
    for (size_t elem_idx = 0; elem_idx < lhs_count && elem_idx < rhs_count; ++elem_idx) {
      const Type::TAny::TWrapper lhs_elem(lhs_pin->NewElem(elem_idx, reinterpret_cast<void *&>(lhs_field_name_state), lhs_field_name_state_alloc, lhs_type_alloc));
      const Type::TAny::TWrapper rhs_elem(rhs_pin->NewElem(elem_idx, reinterpret_cast<void *&>(rhs_field_name_state), rhs_field_name_state_alloc, rhs_type_alloc));
      Sabot::State::TAny::TWrapper
      lhs_field_name(lhs_field_name_state),
      rhs_field_name(rhs_field_name_state);
      Atom::TComparison str_comp = CompareStates(*lhs_field_name, *rhs_field_name, lhs_type_alloc_2, rhs_type_alloc_2);
      if (Atom::IsEq(str_comp)) {  // Names match
        AcceptDouble(*lhs_elem, *rhs_elem, field_visitor);
        if (IsNoMatch(Result)) {
          return;
        }
      } else {  // Names don't match
        DidNotMatch();
        return;
      }
    }
  }
}
void TMatchPrefixTypeVisitor::operator()(const Type::TRecord &/*lhs*/, const Type::TTuple &/*rhs*/    ) const { DidNotMatch(); }


void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TFree &/*rhs*/     ) const { throw TUnreachable(); }

void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TInt8 &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TInt16 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TInt32 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TInt64 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TUInt8 &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TUInt16 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TUInt32 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TUInt64 &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TBool &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TChar &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TFloat &/*rhs*/    ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TDouble &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TDuration &/*rhs*/ ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TTimePoint &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TUuid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TBlob &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TStr &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TTombstone &/*rhs*/) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TVoid &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TDesc &/*rhs*/     ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TOpt &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TSet &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TVector &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TMap &/*rhs*/      ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &/*lhs*/, const Type::TRecord &/*rhs*/   ) const { DidNotMatch(); }
void TMatchPrefixTypeVisitor::operator()(const Type::TTuple &lhs,     const Type::TTuple &rhs        ) const {
  size_t lhs_count = lhs.GetElemCount();
  size_t rhs_count = rhs.GetElemCount();
  if (lhs_count != rhs_count) {
    DidNotMatch();
  } else {
    void *lhs_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize() * 2);
    void *rhs_pin_alloc = reinterpret_cast<uint8_t *>(lhs_pin_alloc) + Sabot::Type::GetMaxTypePinSize();
    Type::TTuple::TPin::TWrapper
      lhs_pin(lhs.Pin(lhs_pin_alloc)),
      rhs_pin(rhs.Pin(rhs_pin_alloc));
    void *lhs_type_alloc = alloca(Sabot::Type::GetMaxTypeSize() * 2);
    void *rhs_type_alloc = reinterpret_cast<uint8_t *>(lhs_type_alloc) + Sabot::Type::GetMaxTypeSize();
    for (size_t elem_idx = 0; elem_idx < lhs_count && elem_idx < rhs_count; ++elem_idx) {
      AcceptDouble(*Type::TAny::TWrapper(lhs_pin->NewElem(elem_idx, lhs_type_alloc)),
                   *Type::TAny::TWrapper(rhs_pin->NewElem(elem_idx, rhs_type_alloc)),
                   *this);
      if (IsNoMatch(Result)) {
        break;
      }
    }
  }
}

TMatchResult Orly::Sabot::MatchPrefixType(const Type::TAny &lhs_any, const Type::TAny &rhs_any) {
  TMatchResult result = TMatchResult::Unifies;
  TMatchPrefixTypeVisitor visitor(result);
  //const Type::TTuple *lhs_ptr = dynamic_cast<const Type::TTuple *>(&lhs_any);
  //const Type::TTuple *rhs_ptr = dynamic_cast<const Type::TTuple *>(&rhs_any);
  assert(dynamic_cast<const Type::TTuple *>(&lhs_any));  /* lhs must be a tuple. */
  assert(dynamic_cast<const Type::TTuple *>(&rhs_any));  /* rhs must be a tuple. */
  const Type::TTuple &lhs = *static_cast<const Type::TTuple *>(&lhs_any);
  const Type::TTuple &rhs = *static_cast<const Type::TTuple *>(&rhs_any);
  size_t lhs_count = lhs.GetElemCount();
  size_t rhs_count = rhs.GetElemCount();
  void *lhs_pin_alloc = alloca(Sabot::Type::GetMaxTypePinSize() * 2);
  void *rhs_pin_alloc = reinterpret_cast<uint8_t *>(lhs_pin_alloc) + Sabot::Type::GetMaxTypePinSize();
  Type::TTuple::TPin::TWrapper
    lhs_pin(lhs.Pin(lhs_pin_alloc)),
    rhs_pin(rhs.Pin(rhs_pin_alloc));
  void *lhs_type_alloc = alloca(Sabot::Type::GetMaxTypeSize() * 2);
  void *rhs_type_alloc = reinterpret_cast<uint8_t *>(lhs_type_alloc) + Sabot::Type::GetMaxTypeSize();
  size_t elem_idx = 0;
  for (; elem_idx < lhs_count && elem_idx < rhs_count; ++elem_idx) {
    AcceptDouble(*Type::TAny::TWrapper(lhs_pin->NewElem(elem_idx, lhs_type_alloc)),
                 *Type::TAny::TWrapper(rhs_pin->NewElem(elem_idx, rhs_type_alloc)),
                 visitor);
    switch (result) {
      case TMatchResult::NoMatch: {
        return result;
      }
      case TMatchResult::PrefixMatch: {
        /* we didn't unify on one of the free types, so we're not going to be able to unify anymore.
           If we're in the unification phase we're never going to get a NoMatch either */
        return result;
      }
      case TMatchResult::Unifies: {
        break;
      }
    }
  }
  if (IsUnifies(result)) {
    if (lhs_count == rhs_count) {
      /* do nothing */
    } else if (elem_idx < lhs_count) { /* lhs is bigger */
      result = TMatchResult::PrefixMatch;
      for (; elem_idx < lhs_count; ++elem_idx) {
        Type::TAny::TWrapper elem_wrapper(lhs_pin->NewElem(elem_idx, lhs_type_alloc));
        const Type::TFree *free_type = dynamic_cast<const Type::TFree *>(elem_wrapper.get());
        if (free_type == nullptr) {
          result = TMatchResult::NoMatch;
        }
      }
    } else { /* rhs is bigger */
      assert(elem_idx < rhs_count);
      result = TMatchResult::PrefixMatch;
    }
  }
  return result;
}