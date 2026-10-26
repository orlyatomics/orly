/* <orly/atom/note_interner2.test.cc>

   Unit test for <orly/atom/note_interner2.h>.

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

#include <orly/atom/note_interner2.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Atom;

static const TCore::TNote *Propose(TNoteInterner &note_interner, const char *c_str) {
  return note_interner.Propose(TCore::TNote::New(c_str, c_str + strlen(c_str), false));
}

FIXTURE(Typical) {
  const char
      *str_a = "hello",
      *str_b = "doctor";
  TNoteInterner
      interner_a,
      interner_b;
  auto
      note_a1 = Propose(interner_a, str_a),
      note_a2 = Propose(interner_a, str_a),
      note_b1 = Propose(interner_a, str_b),
      note_b2 = Propose(interner_b, str_b);
  EXPECT_EQ(note_a1, note_a2);
  EXPECT_NE(note_a1, note_b1);
  EXPECT_NE(note_a1, note_b2);
  EXPECT_NE(note_b1, note_b2);
  EXPECT_TRUE(interner_a.IsKnown(note_a1));
  EXPECT_TRUE(interner_a.IsKnown(note_b1));
  EXPECT_TRUE(interner_b.IsKnown(note_b2));
  EXPECT_TRUE(interner_a.IsKnown(note_b2));
  EXPECT_TRUE(interner_b.IsKnown(note_b1));
  EXPECT_FALSE(interner_a.IsKnown(nullptr));
  EXPECT_FALSE(interner_b.IsKnown(nullptr));
  EXPECT_TRUE(interner_a.IsOwned(note_a1));
  EXPECT_TRUE(interner_a.IsOwned(note_b1));
  EXPECT_TRUE(interner_b.IsOwned(note_b2));
  EXPECT_FALSE(interner_a.IsOwned(note_b2));
  EXPECT_FALSE(interner_b.IsOwned(note_b1));
  EXPECT_FALSE(interner_a.IsOwned(nullptr));
  EXPECT_FALSE(interner_b.IsOwned(nullptr));
  EXPECT_EQ(interner_a.GetSize(), 2U);
  EXPECT_EQ(interner_b.GetSize(), 1U);
  unordered_set<const TCore::TNote *> notes;
  for (const auto &iter : interner_a.GetNotes()) {
    notes.insert(iter);
  }
  EXPECT_EQ(notes.size(), 2U);
  EXPECT_TRUE(notes.find(note_a1) != notes.end());
  EXPECT_TRUE(notes.find(note_b1) != notes.end());
  EXPECT_TRUE(notes.find(note_b2) == notes.end());
}

/* Enough notes to make the set grow several times: each distinct note is kept once, each repeat hands back
   the one kept, and the byte count counts only the kept ones (#1018). */
FIXTURE(ManyNotes) {
  TNoteInterner interner;
  std::vector<const TCore::TNote *> first;
  size_t bytes = 0UL;
  for (int i = 0; i < 5000; ++i) {
    const string str = "note " + to_string(i) + string(i % 40, 'x');
    first.push_back(Propose(interner, str.c_str()));
    bytes += sizeof(TCore::TNote) + str.size() + 1UL;
  }
  EXPECT_EQ(interner.GetSize(), 5000U);
  EXPECT_EQ(interner.GetByteSize(), bytes);
  bool all_same = true;
  for (int i = 0; i < 5000; ++i) {
    const string str = "note " + to_string(i) + string(i % 40, 'x');
    all_same = all_same && Propose(interner, str.c_str()) == first[i] && interner.IsOwned(first[i]);
  }
  EXPECT_TRUE(all_same);
  EXPECT_EQ(interner.GetSize(), 5000U);
  EXPECT_EQ(interner.GetByteSize(), bytes);
}

/* Abandon hands the notes over without deleting them and leaves the interner empty and usable (#1025). */
FIXTURE(AbandonLeavesNotesAlive) {
  const TCore::TNote *kept;
  {
    TNoteInterner interner;
    kept = Propose(interner, "hello");
    Propose(interner, "doctor");
    EXPECT_EQ(interner.GetSize(), 2U);
    interner.Abandon();
    EXPECT_EQ(interner.GetSize(), 0U);
    EXPECT_EQ(interner.GetByteSize(), 0U);
    EXPECT_FALSE(interner.IsKnown(kept));
    EXPECT_TRUE(Propose(interner, "hello") != kept);
    EXPECT_EQ(interner.GetSize(), 1U);
  }
  /* Still readable after the interner is gone; a sanitizer would flag a use after free here. */
  EXPECT_TRUE(kept->GetRawSize() >= strlen("hello"));
}
