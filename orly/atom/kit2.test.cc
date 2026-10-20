/* <orly/atom/kit2.test.cc>

   Unit test for <orly/atom/kit2.h>.

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

#include <orly/atom/kit2.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <orly/native/all.h>
#include <orly/native/point.h>
#include <orly/sabot/compare_types.h>
#include <orly/sabot/get_hash.h>
#include <orly/sabot/state_dumper.h>
#include <orly/sabot/type_dumper.h>
#include <orly/type/new_sabot.h>
#include <orly/type/sabot_to_type.h>
#include <orly/type/self_ref.h>
#include <orly/type/type_czar.h>
#include <orly/type/variant.h>
#include <base/test/kit.h>

using namespace std;
using namespace Orly;
using namespace Orly::Atom;

/* An extensible arena that does no interning and enforces no ordering. */
class TTestArena final
    : public TCore::TExtensibleArena {
  public:

  /* Conveniences. */
  using TNote   = TCore::TNote;
  using TOffset = TCore::TOffset;

  /* Do-little. */
  TTestArena() : TCore::TArena(false), TCore::TExtensibleArena(false) {}

  /* Destroys all notes when it goes. */
  ~TTestArena() {
    for (TNote *note: Notes) {
      delete note;
    }
  }

  /* See base class. */
  virtual TOffset Propose(TNote *proposed_note) override {
    assert(proposed_note);
    try {
      Notes.insert(proposed_note);
    } catch (...) {
      delete proposed_note;
      throw;
    }
    return reinterpret_cast<TOffset>(proposed_note);
  }

  private:

  /* See base class.  Does nothing. */
  virtual void ReleaseNote(const TNote * /*note*/, TOffset /*offset*/, void * /*data1*/, void * /*data2*/, void * /*data3*/) override {}

  /* See base class.  Looks up the requested offset in Notes. */
  virtual const TNote *TryAcquireNote(TOffset offset, void *&/*data1*/, void *&/*data2*/, void *&/*data3*/) override {
    auto note = reinterpret_cast<TNote *>(offset);
    return (Notes.find(note) != Notes.end()) ? note : nullptr;
  }

  /* See base class.  Looks up the requested offset in Notes. */
  virtual const TNote *TryAcquireNote(TOffset offset, size_t /*known_size*/, void *&/*data1*/, void *&/*data2*/, void *&/*data3*/) override {
    auto note = reinterpret_cast<TNote *>(offset);
    return (Notes.find(note) != Notes.end()) ? note : nullptr;
  }

  /* Our notes. */
  std::unordered_set<TNote *> Notes;

};  // TTestArena

template <typename TVal>
static string ToString(const TVal &val) {
  TTestArena arena;
  ostringstream strm;
  void *state_alloc_1 = alloca(Sabot::State::GetMaxStateSize());
  void *state_alloc_2 = alloca(Sabot::State::GetMaxStateSize());
  Sabot::State::TAny::TWrapper(TCore(&arena, Sabot::State::TAny::TWrapper(Native::State::New(val, state_alloc_1))).NewState(&arena, state_alloc_2))->Accept(Sabot::TStateDumper(strm));
  return strm.str();
}

FIXTURE(Empties) {
  EXPECT_EQ(ToString(Native::TFree<bool>::Free), "free(bool)");
  EXPECT_EQ(ToString(Native::TTombstone::Tombstone), "tombstone");
  /* TODO(#329): Test void -- Native::State::TVoid is private to Native::State (only
     Native::Record<T> is a friend) and, unlike TFree/TTombstone, has no Factory<TVal>
     specialization or any other production caller that ever constructs one (verified: zero
     "TVoid()" call sites in the tree outside its own declaration). Covering it here would mean
     adding new, currently-unused production wiring just to make the test possible, which is out
     of scope for a test-coverage fill-in. */
}

FIXTURE(Int8) {
  EXPECT_EQ(ToString<int8_t>(-101), "-101");
  EXPECT_EQ(ToString<int8_t>(0), "0");
  EXPECT_EQ(ToString<int8_t>(101), "101");
}

FIXTURE(Int16) {
  EXPECT_EQ(ToString<int16_t>(-101), "-101");
  EXPECT_EQ(ToString<int16_t>(0), "0");
  EXPECT_EQ(ToString<int16_t>(101), "101");
}

FIXTURE(Int32) {
  EXPECT_EQ(ToString<int32_t>(-101), "-101");
  EXPECT_EQ(ToString<int32_t>(0), "0");
  EXPECT_EQ(ToString<int32_t>(101), "101");
}

FIXTURE(Int64) {
  EXPECT_EQ(ToString<int64_t>(-101), "-101");
  EXPECT_EQ(ToString<int64_t>(0), "0");
  EXPECT_EQ(ToString<int64_t>(101), "101");
}

FIXTURE(UInt8) {
  EXPECT_EQ(ToString<uint8_t>(0), "0");
  EXPECT_EQ(ToString<uint8_t>(101), "101");
}

FIXTURE(UInt16) {
  EXPECT_EQ(ToString<uint16_t>(0), "0");
  EXPECT_EQ(ToString<uint16_t>(101), "101");
}

FIXTURE(UInt32) {
  EXPECT_EQ(ToString<uint32_t>(0), "0");
  EXPECT_EQ(ToString<uint32_t>(101), "101");
}

FIXTURE(UInt64) {
  EXPECT_EQ(ToString<uint64_t>(0), "0");
  EXPECT_EQ(ToString<uint64_t>(101), "101");
}

FIXTURE(Bool) {
  EXPECT_EQ(ToString(true), "true");
  EXPECT_EQ(ToString(false), "false");
}

FIXTURE(Char) {
  EXPECT_EQ(ToString('x'), "'x'");
}

FIXTURE(TFloat) {
  EXPECT_EQ(ToString<float>(-98.6), "-98.6");
  EXPECT_EQ(ToString<float>(0), "0");
  EXPECT_EQ(ToString<float>(98.6), "98.6");
}

FIXTURE(TDouble) {
  EXPECT_EQ(ToString<double>(-98.6), "-98.6");
  EXPECT_EQ(ToString<double>(0), "0");
  EXPECT_EQ(ToString<double>(98.6), "98.6");
}

FIXTURE(TDuration) {
  EXPECT_EQ(ToString(TCore::TStdDuration(0)), "0ns");
  EXPECT_EQ(ToString(TCore::TStdDuration(1234)), "1234ns");
}

FIXTURE(TTimePoint) {
  EXPECT_EQ(ToString(TCore::TStdTimePoint()), "1970:01:01:00:00:00utc");
}

FIXTURE(TUuid) {
  const char *str = "1b4e28ba-2fa1-11d2-883f-b9a761bde3fb";
  EXPECT_EQ(ToString(TCore::TUuid(str)), str);
}

FIXTURE(Blob) {
  uint8_t data[100];
  for (int i = 0; i < 100; ++i) {
    data[i] = 101 + i;
  }
  EXPECT_EQ(ToString(Native::TBlob(data,  3)), "{{ 101, 102, 103 }}");
  EXPECT_EQ(ToString(Native::TBlob(data, 30)), "{{ 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130 }}");
}

FIXTURE(String) {
  EXPECT_EQ(ToString<string>("Mofo the Psychic Gorilla is in your base, shooting your dudes."), "\"Mofo the Psychic Gorilla is in your base, shooting your dudes.\"");
  EXPECT_EQ(ToString<string>("shortness"), "\"shortness\"");
}

FIXTURE(Desc) {
  EXPECT_EQ(ToString(TDesc<int>(101)), "desc(101)");
}

FIXTURE(Opt) {
  EXPECT_EQ(ToString(std::optional<int>(101)), "opt(101)");
  EXPECT_EQ(ToString(std::optional<bool>()), "unknown opt(bool)");
}

FIXTURE(Set) {
  EXPECT_EQ(ToString(set<int>({ 101, 102, 103 })), "set(101, 102, 103)");
  EXPECT_EQ(ToString(set<bool>()), "empty set(bool)");
}

FIXTURE(Vector) {
  EXPECT_EQ(ToString(vector<int>({ 101, 102, 103 })), "vector(101, 102, 103)");
  EXPECT_EQ(ToString(vector<bool>()), "empty vector(bool)");
}

FIXTURE(Map) {
  EXPECT_EQ(ToString(map<int, string>({ { 101, "hello"}, { 102, "doctor"} })), "map(101: \"hello\", 102: \"doctor\")");
  EXPECT_EQ(ToString(map<double, bool>()), "empty map(double, bool)");
}

FIXTURE(Record) {
  EXPECT_EQ(ToString(TPoint(1.5, 2.5)), "record(X: 1.5, Y: 2.5)");
}

FIXTURE(Tuple) {
  EXPECT_EQ((ToString(tuple<bool, int, double>(true, 101, 98.6))), "tuple(true, 101, 98.6)");
  EXPECT_EQ(ToString(tuple<>()), "tuple()");
}

template <typename TVal>
static string ToString() {
  TTestArena arena;
  ostringstream strm;
  void *type_alloc_1 = alloca(Sabot::Type::GetMaxTypeSize());
  void *type_alloc_2 = alloca(Sabot::Type::GetMaxTypeSize());
  Sabot::Type::TAny::TWrapper(TCore(&arena, *Sabot::Type::TAny::TWrapper(Native::Type::For<TVal>::GetType(type_alloc_1))).GetType(&arena, type_alloc_2))->Accept(Sabot::TTypeDumper(strm));
  return strm.str();
}

FIXTURE(Typical) {
  EXPECT_EQ(ToString<int8_t>(), "int8");
  EXPECT_EQ(ToString<int16_t>(), "int16");
  EXPECT_EQ(ToString<int32_t>(), "int32");
  EXPECT_EQ(ToString<int64_t>(), "int64");
  EXPECT_EQ(ToString<uint8_t>(), "uint8");
  EXPECT_EQ(ToString<uint16_t>(), "uint16");
  EXPECT_EQ(ToString<uint32_t>(), "uint32");
  EXPECT_EQ(ToString<uint64_t>(), "uint64");
  EXPECT_EQ(ToString<bool>(), "bool");
  EXPECT_EQ(ToString<char>(), "char");
  EXPECT_EQ(ToString<float>(), "float");
  EXPECT_EQ(ToString<double>(), "double");
  EXPECT_EQ(ToString<TCore::TStdDuration>(), "duration");
  EXPECT_EQ(ToString<TCore::TStdTimePoint>(), "time_point");
  EXPECT_EQ(ToString<TCore::TUuid>(), "uuid");
  EXPECT_EQ(ToString<Native::TBlob>(), "blob");
  EXPECT_EQ(ToString<string>(), "str");
  EXPECT_EQ(ToString<Native::TTombstone>(), "tombstone");
  EXPECT_EQ(ToString<void>(), "void");
  EXPECT_EQ(ToString<Native::TFree<bool>>(), "free(bool)");
}

/* A recursive sum type's #96 fixed-shape record survives the on-disk atom
   codec (issue #115): the SelfRef tycon round-trips through TCore as a direct
   scalar carrying its de Bruijn depth.  This is the on-disk half of the
   new_sabot.test bridge round-trip. */
FIXTURE(RecursiveType) {
  static Type::TTypeCzar type_czar;
  /* tree is <| Leaf(int) | Branch(<{ .l: tree, .r: tree }>) |> */
  Type::TVariantElems arms;
  arms["Leaf"] = Type::TInt::Get();
  arms["Branch"] = Type::TObj::Get({ { "l", Type::TSelfRef::Get(0) }, { "r", Type::TSelfRef::Get(0) } });
  const Type::TType tree = Type::TVariant::Get(arms);

  TTestArena arena;
  void *sabot_alloc = alloca(1000);
  void *back_alloc = alloca(1000);
  Sabot::Type::TAny::TWrapper sabot(Type::NewSabot(sabot_alloc, tree));

  /* Encode the type to a core tree, then decode it back to a sabot type. */
  TCore core(&arena, *sabot);
  Sabot::Type::TAny::TWrapper back(core.GetType(&arena, back_alloc));

  /* The decoded type compares equal to the original, and reverses all the way
     back to the identical interned orly type. */
  EXPECT_TRUE(Atom::IsEq(Sabot::CompareTypes(*sabot, *back)));
  EXPECT_TRUE(Type::ToType(*back) == tree);

  ostringstream strm;
  back->Accept(Sabot::TTypeDumper(strm));
  EXPECT_TRUE(strm.str().find("self_ref(0)") != string::npos);
}

FIXTURE(PinnedLongStr) {
  const char *expected = "This is too long to store directly, so we'll store it indirectly.";
  const size_t expected_size = strlen(expected);
  TTestArena arena;
  void *state_alloc_1 = alloca(Sabot::State::GetMaxStateSize());
  void *state_alloc_2 = alloca(Sabot::State::GetMaxStateSize());
  /* tpl_core must outlive tpl_state: the SS::TTuple constructed in
     state_alloc_2 caches a pointer back to this TCore and reads from
     it later when Pin() is called. Using a temporary here (as the
     original code did) is a stack-lifetime bug that happened to work
     under older compilers but reads freed memory under gcc 13. */
  TCore tpl_core(make_tuple(expected), &arena, state_alloc_1);
  Sabot::State::TAny::TWrapper tpl_state(tpl_core.NewState(&arena, state_alloc_2));
  auto tpl = dynamic_cast<Sabot::State::TTuple *>(tpl_state.get());
  if (EXPECT_TRUE(tpl)) {
    void *state_pin_alloc_1 = alloca(Sabot::State::GetMaxStatePinSize());
    Sabot::State::TTuple::TPin::TWrapper tpl_pin(tpl->Pin(state_pin_alloc_1));
    {
      Sabot::State::TAny::TWrapper str_state(tpl_pin->NewElem(0, state_alloc_1));
      auto str = dynamic_cast<Sabot::State::TStr *>(str_state.get());
      if (EXPECT_TRUE(str)) {
        void *state_pin_alloc_2 = alloca(Sabot::State::GetMaxStatePinSize());
        EXPECT_EQ(str->GetSize(), expected_size);
        Sabot::State::TStr::TPin::TWrapper pin(str->Pin(state_pin_alloc_2));
        EXPECT_EQ(pin->GetSize(), expected_size);
        const char
            *start = pin->GetStart(),
            *limit = pin->GetLimit();
        EXPECT_LE(start, limit);
        EXPECT_FALSE(*limit);
        size_t actual_size = limit - start;
        EXPECT_EQ(actual_size, expected_size);
        EXPECT_FALSE(strcmp(start, expected));
      }
    }
  }
}

FIXTURE(ConcatTuple) {
  TTestArena arena;
  void *state_alloc_1 = alloca(Sabot::State::GetMaxStateSize());
  void *state_alloc_2 = alloca(Sabot::State::GetMaxStateSize());
  const TCore final_tuple(&arena,
                          Sabot::State::TAny::TWrapper(Native::State::New(make_tuple(1, 3.14, true), state_alloc_1)),
                          Sabot::State::TAny::TWrapper(Native::State::New(make_tuple(set<int>{7, 1}, string("Hello")), state_alloc_2)));
  ostringstream strm;
  Sabot::State::TAny::TWrapper(final_tuple.NewState(&arena, state_alloc_1))->Accept(Sabot::TStateDumper(strm));
  EXPECT_EQ(strm.str(), "tuple(1, 3.14, true, set(1, 7), \"Hello\")");
}

FIXTURE(SplitTuple) {
  TTestArena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const TCore big_tuple(make_tuple(1, 3.14, true, set<int>{7, 1}, string("Hello")), &arena, state_alloc);
  TCore lhs_core, rhs_core;
  if (EXPECT_TRUE(big_tuple.TrySplit(&arena, 3, &arena, lhs_core, &arena, rhs_core))) {
    /* check left */ {
      ostringstream strm;
      Sabot::State::TAny::TWrapper(lhs_core.NewState(&arena, state_alloc))->Accept(Sabot::TStateDumper(strm));
      EXPECT_EQ(strm.str(), "tuple(1, 3.14, true)");
    }
    /* check right */ {
      ostringstream strm;
      Sabot::State::TAny::TWrapper(rhs_core.NewState(&arena, state_alloc))->Accept(Sabot::TStateDumper(strm));
      EXPECT_EQ(strm.str(), "tuple(set(1, 7), \"Hello\")");
    }
  }
}

FIXTURE(StoredHash) {
  TTestArena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TCore big_tuple(make_tuple(1, 3.14, true, set<int>{7, 1}, string("Hello")), &arena, state_alloc);
  size_t out_hash = 0UL;
  size_t stored_hash = Sabot::GetHash(*Sabot::State::TAny::TWrapper(big_tuple.NewState(&arena, state_alloc)));
  EXPECT_FALSE(big_tuple.TryGetStoredHash(out_hash));
  EXPECT_TRUE(big_tuple.TrySetStoredHash(stored_hash));
  EXPECT_TRUE(big_tuple.TryGetStoredHash(out_hash));
  EXPECT_EQ(stored_hash, out_hash);
  TCore direct_int64(64L, &arena, state_alloc);
  EXPECT_FALSE(direct_int64.TryGetStoredHash(out_hash));
  EXPECT_FALSE(direct_int64.TrySetStoredHash(stored_hash));
}

/* Return a specific kind of state or a null pointer. */
template <typename TSomeState>
const TSomeState *TryAsState(const Sabot::State::TAny &state) {
  struct base_visitor_t : Sabot::TStateVisitor {
    mutable const TSomeState *Out;
    virtual void operator()(const Sabot::State::TFree &      ) const override {}
    virtual void operator()(const Sabot::State::TTombstone & ) const override {}
    virtual void operator()(const Sabot::State::TVoid &      ) const override {}
    virtual void operator()(const Sabot::State::TInt8 &      ) const override {}
    virtual void operator()(const Sabot::State::TInt16 &     ) const override {}
    virtual void operator()(const Sabot::State::TInt32 &     ) const override {}
    virtual void operator()(const Sabot::State::TInt64 &     ) const override {}
    virtual void operator()(const Sabot::State::TUInt8 &     ) const override {}
    virtual void operator()(const Sabot::State::TUInt16 &    ) const override {}
    virtual void operator()(const Sabot::State::TUInt32 &    ) const override {}
    virtual void operator()(const Sabot::State::TUInt64 &    ) const override {}
    virtual void operator()(const Sabot::State::TBool &      ) const override {}
    virtual void operator()(const Sabot::State::TChar &      ) const override {}
    virtual void operator()(const Sabot::State::TFloat &     ) const override {}
    virtual void operator()(const Sabot::State::TDouble &    ) const override {}
    virtual void operator()(const Sabot::State::TDuration &  ) const override {}
    virtual void operator()(const Sabot::State::TTimePoint & ) const override {}
    virtual void operator()(const Sabot::State::TUuid &      ) const override {}
    virtual void operator()(const Sabot::State::TBlob &      ) const override {}
    virtual void operator()(const Sabot::State::TStr &       ) const override {}
    virtual void operator()(const Sabot::State::TDesc &      ) const override {}
    virtual void operator()(const Sabot::State::TOpt &       ) const override {}
    virtual void operator()(const Sabot::State::TSet &       ) const override {}
    virtual void operator()(const Sabot::State::TVector &    ) const override {}
    virtual void operator()(const Sabot::State::TMap &       ) const override {}
    virtual void operator()(const Sabot::State::TRecord &    ) const override {}
    virtual void operator()(const Sabot::State::TTuple &     ) const override {}
  };
  struct visitor_t final : base_visitor_t {
    virtual void operator()(const TSomeState &that) const override {
      this->Out = &that;
    }
  };
  visitor_t visitor;
  visitor.Out = nullptr;
  state.Accept(visitor);
  return visitor.Out;
}

FIXTURE(ConfirmStrSize) {
  static const string expected =
      "For score and seven years ago our fathers brought forth on this "
      "continent an new nation conceived in liberty.";
  TTestArena arena;
  void *state_temp = alloca(Sabot::State::GetMaxStateSize());
  TCore core(expected, &arena, state_temp);
  Sabot::State::TAny::TWrapper state(core.NewState(&arena, state_temp));
  const auto *str = TryAsState<Sabot::State::TStr>(*state);
  if (EXPECT_TRUE(str)) {
    void *pin_temp = alloca(Sabot::State::GetMaxStatePinSize());
    Sabot::State::TStr::TPin::TWrapper pin(str->Pin(pin_temp));
    const char
        *start = pin->GetStart(),
        *limit = pin->GetLimit();
    EXPECT_EQ(pin->GetSize(), expected.size());
    EXPECT_EQ(static_cast<size_t>(limit - start), expected.size());
    EXPECT_EQ(*limit, '\0');
    EXPECT_EQ(strcmp(start, expected.c_str()), 0);
  }
}

/* #666: a note's Unused flag bits are not part of its value. Notes written to disk before #666
   carry garbage there, so a disk merge must not tell two copies of one value apart by them. */
FIXTURE(NoteEqualityIgnoresUnusedBits) {
  static const char text[] = "a note long enough to live in an arena";
  const auto *start = reinterpret_cast<const uint8_t *>(text);
  unique_ptr<TCore::TNote> note(TCore::TNote::New(start, start + sizeof(text), false));
  const size_t note_size = sizeof(TCore::TNote) + note->GetRawSize();
  vector<uint8_t> copy(note_size);
  memcpy(copy.data(), note.get(), note_size);
  const auto *copied = reinterpret_cast<const TCore::TNote *>(copy.data());
  EXPECT_TRUE(note->HasSameBytes(*copied));
  /* The flags share the byte after the tycon. Flip each bit of it in turn: the six that change no
     accessor are the Unused ones, and they must not affect equality. */
  size_t num_unused = 0UL;
  for (size_t bit = 0UL; bit < 8UL; ++bit) {
    copy[1] ^= static_cast<uint8_t>(1U << bit);
    if (copied->IsExemplar() == note->IsExemplar() && copied->IsUnReferenced() == note->IsUnReferenced()) {
      ++num_unused;
      EXPECT_NE(memcmp(copy.data(), note.get(), note_size), 0);
      EXPECT_TRUE(note->HasSameBytes(*copied));
    } else {
      EXPECT_FALSE(note->HasSameBytes(*copied));
    }
    copy[1] ^= static_cast<uint8_t>(1U << bit);
  }
  EXPECT_EQ(num_unused, 6UL);
  /* A real difference still counts. */
  copy.back() ^= 1U;
  EXPECT_FALSE(note->HasSameBytes(*copied));
}

/* #993: a core is packed (alignment 1) and sits at arbitrary offsets in a note, so its direct data is read by copying, never through a typed reference. The layout, the
   bytes a scalar leaves in its cell (which are what the disk stores) and the quick hash (which is persisted) must not
   change with the way the data is read.  The expected values below were taken from the build that read through
   references. */
static_assert(sizeof(TCore) == 1 + MaxDirectSize, "A core is a tycon byte and the direct storage area.");

static array<uint8_t, sizeof(TCore)> CellBytes(const TCore &core) {
  array<uint8_t, sizeof(TCore)> bytes;
  memcpy(bytes.data(), &core, sizeof(TCore));
  return bytes;
}

static string Hex(const array<uint8_t, sizeof(TCore)> &bytes) {
  string out;
  for (uint8_t byte: bytes) {
    out += "0123456789abcdef"[byte >> 4];
    out += "0123456789abcdef"[byte & 15];
    out += ' ';
  }
  return out;
}

/* The bytes of a cell: the value little-endian, then zeroes, then the tycon in the last byte. */
static array<uint8_t, sizeof(TCore)> Expect(uint8_t tycon, initializer_list<uint8_t> value) {
  array<uint8_t, sizeof(TCore)> bytes;
  bytes.fill(0);
  bytes[sizeof(TCore) - 1] = tycon;
  copy(value.begin(), value.end(), bytes.begin());
  return bytes;
}

static size_t QuickHash(const TCore &core) {
  size_t hash = 0;
  EXPECT_TRUE(core.TryGetQuickHash(hash));
  return hash;
}

/* A core holding a scalar. */
template <typename TVal>
static TCore Make(const TVal &val) {
  static TTestArena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  return TCore(val, &arena, state_alloc);
}

FIXTURE(CellBytesAndQuickHashAreUnchanged) {
  /* Tycons are on-disk values: Int8 is 48 and the integers follow in order. */
  TCore core = Make<int8_t>(-101);
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(48, {0x9b})));
  EXPECT_EQ(QuickHash(core), 0xf0f8104cae9faf85UL);
  core = Make(static_cast<int16_t>(-12345));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(49, {0xc7, 0xcf})));
  EXPECT_EQ(QuickHash(core), 0xc2491f24fcc23ec3UL);
  core = Make(static_cast<int32_t>(0x01020304));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(50, {0x04, 0x03, 0x02, 0x01})));
  EXPECT_EQ(QuickHash(core), 0x4c089d3ca9d430f5UL);
  core = Make(static_cast<int64_t>(-0x0102030405060708LL));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(51, {0xf8, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe})));
  EXPECT_EQ(QuickHash(core), 0x8fd965119b5b9ed3UL);
  core = Make(static_cast<uint8_t>(200));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(52, {0xc8})));
  EXPECT_EQ(QuickHash(core), 0xcdc196a1997d4ddfUL);
  core = Make(static_cast<uint16_t>(54321));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(53, {0x31, 0xd4})));
  EXPECT_EQ(QuickHash(core), 0x402ac515aa514a62UL);
  core = Make(static_cast<uint32_t>(0xdeadbeefU));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(54, {0xef, 0xbe, 0xad, 0xde})));
  EXPECT_EQ(QuickHash(core), 0x305c1b46e1e71b08UL);
  core = Make(static_cast<uint64_t>(0x0123456789abcdefULL));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(55, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01})));
  EXPECT_EQ(QuickHash(core), 0x8428d319057264f0UL);
  /* The rest hash through std::hash of the value; check the cell and that the hash matches the value's own. */
  core = Make(static_cast<bool>(true));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(56, {0x01})));
  EXPECT_EQ(QuickHash(core), std::hash<bool>()(true));
  core = Make(static_cast<char>('q'));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(57, {'q'})));
  EXPECT_EQ(QuickHash(core), std::hash<char>()('q'));
  core = Make(static_cast<float>(1.5f));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(58, {0x00, 0x00, 0xc0, 0x3f})));
  EXPECT_EQ(QuickHash(core), std::hash<float>()(1.5f));
  core = Make(static_cast<double>(-2.25));
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(59, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xc0})));
  EXPECT_EQ(QuickHash(core), std::hash<double>()(-2.25));
  const Sabot::TStdDuration duration(Sabot::TStdDuration::rep(0x0102030405060708LL));
  core = Make(duration);
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(60, {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01})));
  EXPECT_EQ(QuickHash(core), std::hash<Sabot::TStdDuration::rep>()(duration.count()));
  const Sabot::TStdTimePoint time_point(duration);
  core = Make(time_point);
  EXPECT_EQ(Hex(CellBytes(core)), Hex(Expect(61, {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01})));
  EXPECT_EQ(QuickHash(core), std::hash<Sabot::TStdDuration::rep>()(duration.count()));
  const Base::TUuid uuid(Base::TUuid::Random);
  core = Make(uuid);
  auto uuid_cell = CellBytes(core);
  EXPECT_EQ(uuid_cell[sizeof(TCore) - 1], 62);
  EXPECT_EQ(memcmp(uuid_cell.data(), &uuid, sizeof(Base::TUuid)), 0);
  EXPECT_EQ(QuickHash(core), std::hash<Base::TUuid>()(uuid));
}

/* Scalars order and read back through the sabot as the values they hold, wherever in memory their cell sits. */
FIXTURE(ScalarsReadBackFromACellAtAnyAlignment) {
  TTestArena arena;
  /* A core embedded at every byte offset of a buffer, so the direct data lands on every alignment. */
  alignas(16) uint8_t buf[sizeof(TCore) + 16];
  for (size_t shift = 0; shift < 16; ++shift) {
    const TCore source = Make<int64_t>(-0x0102030405060708LL);
    memcpy(buf + shift, &source, sizeof(TCore));
    const TCore *core = reinterpret_cast<const TCore *>(buf + shift);
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    ostringstream strm;
    Sabot::State::TAny::TWrapper(core->NewState(&arena, state_alloc))->Accept(Sabot::TStateDumper(strm));
    EXPECT_EQ(strm.str(), "-72623859790382856");
    size_t hash = 0;
    EXPECT_TRUE(core->TryGetQuickHash(hash));
    EXPECT_EQ(hash, 0x8fd965119b5b9ed3UL);
  }
}

FIXTURE(ScalarOrder) {
  TTestArena arena;
  auto order = [&arena](const TCore &lhs, const TCore &rhs) {
    Atom::TComparison comp = Atom::TComparison::Eq;
    EXPECT_TRUE(lhs.TryQuickOrderComparison(&arena, rhs, &arena, comp));
    return comp;
  };
  TCore lhs = Make<int16_t>(-5);
  TCore rhs = Make<int16_t>(7);
  EXPECT_TRUE(order(lhs, rhs) == Atom::TComparison::Lt);
  EXPECT_TRUE(order(rhs, lhs) == Atom::TComparison::Gt);
  EXPECT_TRUE(order(lhs, lhs) == Atom::TComparison::Eq);
  lhs = Make(1.5);
  rhs = Make(-1.5);
  EXPECT_TRUE(order(lhs, rhs) == Atom::TComparison::Gt);
  lhs = Make<uint64_t>(1ULL << 63);
  rhs = Make<uint64_t>(1);
  EXPECT_TRUE(order(lhs, rhs) == Atom::TComparison::Gt);
  TCore::TOrderedArenaCompare compare(&arena);
  EXPECT_TRUE(compare(rhs, lhs));
  EXPECT_FALSE(compare(lhs, rhs));
}
