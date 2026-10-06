/* <orly/indy/key.test.cc>

   Unit test for <orly/indy/key.h>.

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

#include <orly/indy/key.h>

#include <cstring>
#include <tuple>
#include <vector>

#include <orly/atom/suprena.h>
#include <orly/native/all.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;

FIXTURE(CoreFromKey) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TKey op(10UL, &arena, state_alloc);
  TSuprena new_arena;
  TCore op_copy(&new_arena, Sabot::State::TAny::TWrapper(op.GetCore().NewState(op.GetArena(), state_alloc)).get());
}

FIXTURE(KeyFromKey) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TKey key_1(10UL, &arena, state_alloc);
  TSuprena new_arena;
  TKey key_2(&new_arena, state_alloc, key_1);
}
/* An ordered arena laid out by hand, as a disk file's arena is: one note after another. */
class TOrderedTestArena final
    : public TCore::TArena {
  NO_COPY(TOrderedTestArena);
  public:

  TOrderedTestArena()
      : TArena(true) {}

  /* Append a copy of the note, returning its offset. */
  TCore::TOffset Append(const TCore::TNote *note) {
    const size_t note_size = sizeof(TCore::TNote) + note->GetRawSize();
    const TCore::TOffset offset = Bytes.size() * sizeof(uint64_t);
    Bytes.resize(Bytes.size() + (note_size + sizeof(uint64_t) - 1UL) / sizeof(uint64_t));
    memcpy(reinterpret_cast<uint8_t *>(Bytes.data()) + offset, note, note_size);
    return offset;
  }

  private:

  virtual void ReleaseNote(const TCore::TNote *, TCore::TOffset, void *, void *, void *) override {}

  virtual const TCore::TNote *TryAcquireNote(TCore::TOffset offset, void *&, void *&, void *&) override {
    return offset < Bytes.size() * sizeof(uint64_t) ? reinterpret_cast<const TCore::TNote *>(reinterpret_cast<const uint8_t *>(Bytes.data()) + offset) : nullptr;
  }

  virtual const TCore::TNote *TryAcquireNote(TCore::TOffset offset, size_t, void *&data1, void *&data2, void *&data3) override {
    return TryAcquireNote(offset, data1, data2, data3);
  }

  vector<uint64_t> Bytes;

};  // TOrderedTestArena

/* #674: a release merge from before #666 could write two copies of one note into an ordered
   arena. Two keys in such an arena at different offsets are equal if their values are, and the
   stored hashes alone must not decide it. */
FIXTURE(DuplicateNoteInOrderedArena) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  void *pin_alloc = alloca(sizeof(TCore::TArena::TFinalPin));
  TSuprena source;
  TOrderedTestArena arena;
  /* Copy a key's note into the ordered arena, and return a core that names the copy and stores
     the given hash, as a disk file's key does. */
  auto copy_key = [&](int64_t val, size_t hash) {
    const TCore core(make_tuple(val), &source, state_alloc);
    TCore::TArena::TFinalPin::TWrapper pin(source.Pin(*core.TryGetOffset(), pin_alloc));
    TCore copy(arena.Append(pin->GetNote()), pin->GetNote());
    EXPECT_TRUE(copy.TrySetStoredHash(hash));
    return copy;
  };
  auto hash_of = [&](int64_t val) {
    return TKey(make_tuple(val), &source, state_alloc).GetHash();
  };
  const TCore one = copy_key(1L, hash_of(1L)),
              one_again = copy_key(1L, hash_of(1L)),
              two = copy_key(2L, hash_of(2L)),
              /* a different value whose stored hash matches one's */
              two_colliding = copy_key(2L, hash_of(1L));
  size_t fallbacks = TKey::GetNumSameArenaFallbacks();
  /* the copies are one key */
  EXPECT_FALSE(TKey::TupleNeEq(one, &arena, one_again, &arena));
  EXPECT_TRUE(TKey::TupleEqEq(one, &arena, one_again, &arena));
  EXPECT_TRUE(TKey(one, &arena) == TKey(one_again, &arena));
  EXPECT_FALSE(TKey(one, &arena) != TKey(one_again, &arena));
  EXPECT_EQ(TKey::GetNumSameArenaFallbacks(), fallbacks + 4UL);
  fallbacks = TKey::GetNumSameArenaFallbacks();
  /* equal hashes are not equal values */
  EXPECT_TRUE(TKey::TupleNeEq(one, &arena, two_colliding, &arena));
  EXPECT_FALSE(TKey::TupleEqEq(one, &arena, two_colliding, &arena));
  EXPECT_FALSE(TKey(one, &arena) == TKey(two_colliding, &arena));
  EXPECT_TRUE(TKey(one, &arena) != TKey(two_colliding, &arena));
  EXPECT_EQ(TKey::GetNumSameArenaFallbacks(), fallbacks + 4UL);
  fallbacks = TKey::GetNumSameArenaFallbacks();
  /* and the common cases take no fallback */
  EXPECT_FALSE(TKey::TupleNeEq(one, &arena, one, &arena));
  EXPECT_TRUE(TKey::TupleNeEq(one, &arena, two, &arena));
  EXPECT_TRUE(TKey(one, &arena) == TKey(one, &arena));
  EXPECT_TRUE(TKey(one, &arena) != TKey(two, &arena));
  EXPECT_EQ(TKey::GetNumSameArenaFallbacks(), fallbacks);
  /* Ordering is left alone: in one ordered arena it still goes by offset. */
  EXPECT_TRUE(TKey::Compare(one, &arena, one_again, &arena) == Atom::TComparison::Lt);
  EXPECT_TRUE(TKey::Compare(one, &arena, two, &arena) == Atom::TComparison::Lt);
}
