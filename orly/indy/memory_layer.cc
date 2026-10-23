/* <orly/indy/memory_layer.cc>

   Implements <orly/indy/memory_layer.h>.

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

#include <orly/indy/memory_layer.h>

#include <orly/sabot/order_states.h>
#include <orly/sabot/to_native.h>

#include <algorithm>
#include <array>

using namespace std;
using namespace Base;
using namespace Orly::Indy;

/* #257 skip-list seek accelerator helpers. */
namespace {

  /* Number of express lanes a node joins (0 .. SkipMaxLevel-1), geometric with
     p=1/2. Derived deterministically from the entry's index-key hash so no
     global RNG state is needed and the structure is reproducible. The key hash
     (not the sequence number) is the seed because batched writes put many
     entries under one update sharing a single SeqNum -- seeding on SeqNum would
     give every entry in a batch the same height and collapse the express
     lanes. */
  [[nodiscard]] inline size_t SkipHeight(size_t key_hash) noexcept {
    uint64_t h = static_cast<uint64_t>(key_hash) * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    size_t lanes = 0;
    while (lanes + 1 < TUpdate::TEntry::SkipMaxLevel && (h & 1ULL)) {
      h >>= 1;
      ++lanes;
    }
    return lanes;
  }

  /* Where key sorts relative to the keys that unify with pattern, in level-0 order: Lt if before
     all of them, Gt if after all of them, Eq if it can't say. Different index ids decide it alone.
     Within one, the keys are tuples of the index's key type, compared member by member, so the
     pattern's members are compared with key's while they are fully bound and the comparison stops
     at the first that is not: a member that is a free, or holds one, has a type of its own, which
     never orders equal to the key member's type. Comparing against the whole pattern instead would
     decide at that member by type alone, whatever the values before it. Every key of the index
     agrees on how many members that is, so the keys it calls Lt are a head of the index's run and
     the ones it calls Gt a tail, and none of either can unify with the pattern. */
  [[nodiscard]] Orly::Atom::TComparison ComparePatternPrefixBySabot(const TIndexKey &key, const TIndexKey &pattern) {
    if (key.GetIndexId() != pattern.GetIndexId()) {
      return key.GetIndexId() < pattern.GetIndexId() ? Orly::Atom::TComparison::Lt : Orly::Atom::TComparison::Gt;
    }
    void *state_alloc = alloca(Orly::Sabot::State::GetMaxStateSize() * 4);
    void *pattern_state_alloc = static_cast<uint8_t *>(state_alloc) + Orly::Sabot::State::GetMaxStateSize();
    void *key_elem_alloc = static_cast<uint8_t *>(pattern_state_alloc) + Orly::Sabot::State::GetMaxStateSize();
    void *pattern_elem_alloc = static_cast<uint8_t *>(key_elem_alloc) + Orly::Sabot::State::GetMaxStateSize();
    void *pin_alloc = alloca(Orly::Sabot::State::GetMaxStatePinSize() * 2);
    void *pattern_pin_alloc = static_cast<uint8_t *>(pin_alloc) + Orly::Sabot::State::GetMaxStatePinSize();
    void *type_alloc = alloca(Orly::Sabot::Type::GetMaxTypeSize() * 2);
    void *pattern_type_alloc = static_cast<uint8_t *>(type_alloc) + Orly::Sabot::Type::GetMaxTypeSize();
    Orly::Sabot::State::TAny::TWrapper key_state(key.GetKey().GetCore().NewState(key.GetKey().GetArena(), state_alloc));
    Orly::Sabot::State::TAny::TWrapper pattern_state(pattern.GetKey().GetCore().NewState(pattern.GetKey().GetArena(), pattern_state_alloc));
    const auto *key_tuple = dynamic_cast<const Orly::Sabot::State::TTuple *>(&*key_state);
    const auto *pattern_tuple = dynamic_cast<const Orly::Sabot::State::TTuple *>(&*pattern_state);
    if (!key_tuple || !pattern_tuple) {
      return Orly::Atom::TComparison::Eq;  // not a key shape we can narrow: the whole index's run
    }
    Orly::Sabot::State::TTuple::TPin::TWrapper key_pin(key_tuple->Pin(pin_alloc));
    Orly::Sabot::State::TTuple::TPin::TWrapper pattern_pin(pattern_tuple->Pin(pattern_pin_alloc));
    const size_t count = std::min(key_pin->GetElemCount(), pattern_pin->GetElemCount());
    for (size_t elem_idx = 0; elem_idx < count; ++elem_idx) {
      Orly::Sabot::State::TAny::TWrapper key_elem(key_pin->NewElem(elem_idx, key_elem_alloc));
      Orly::Sabot::State::TAny::TWrapper pattern_elem(pattern_pin->NewElem(elem_idx, pattern_elem_alloc));
      if (!Orly::Atom::IsEq(Orly::Sabot::OrderTypes(*Orly::Sabot::Type::TAny::TWrapper(key_elem->GetType(type_alloc)),
                                        *Orly::Sabot::Type::TAny::TWrapper(pattern_elem->GetType(pattern_type_alloc))))) {
        return Orly::Atom::TComparison::Eq;
      }
      const Orly::Atom::TComparison comp = Orly::Sabot::OrderStates(*key_elem, *pattern_elem);
      if (!Orly::Atom::IsEq(comp)) {
        return comp;
      }
    }
    return Orly::Atom::TComparison::Eq;
  }

  /* ComparePatternPrefixBySabot against one pattern, without the Sabot states, dynamic casts and type objects it builds for
     every key. The pattern's members are copied out once; each key's are copied out per comparison and ordered core to core
     while both are simple scalars or the pattern's is a free (TCore::TryQuickOrderPatternMember). A key or pattern of another
     shape, or a member with no quick order, goes to Sabot, so the answer is always Sabot's. */
  class TPatternPrefix {
    public:

    /* Patterns and keys with more members than this always go to Sabot. */
    static constexpr size_t MaxElems = 8UL;

    explicit TPatternPrefix(const TIndexKey &pattern)
        : Pattern(pattern),
          Quick(pattern.GetKey().GetCore().TryCopyTupleElems(pattern.GetKey().GetArena(), Elems.data(), MaxElems, Count)) {}

    [[nodiscard]] Orly::Atom::TComparison Compare(const TIndexKey &key) const {
      if (key.GetIndexId() != Pattern.GetIndexId()) {
        return key.GetIndexId() < Pattern.GetIndexId() ? Orly::Atom::TComparison::Lt : Orly::Atom::TComparison::Gt;
      }
      if (Quick) {
        std::array<Orly::Atom::TCore, MaxElems> key_elems;
        size_t key_count = 0UL;
        Orly::Atom::TCore::TArena *const key_arena = key.GetKey().GetArena();
        if (key.GetKey().GetCore().TryCopyTupleElems(key_arena, key_elems.data(), MaxElems, key_count)) {
          const size_t count = std::min(key_count, Count);
          size_t elem_idx = 0UL;
          for (; elem_idx < count; ++elem_idx) {
            bool same_type = false;
            Orly::Atom::TComparison comp = Orly::Atom::TComparison::Eq;
            if (!key_elems[elem_idx].TryQuickOrderPatternMember(key_arena, Elems[elem_idx], Pattern.GetKey().GetArena(), same_type, comp)) {
              break;
            }
            if (!same_type) {
              return Orly::Atom::TComparison::Eq;
            }
            if (!Orly::Atom::IsEq(comp)) {
              return comp;
            }
          }
          if (elem_idx == count) {
            return Orly::Atom::TComparison::Eq;
          }
        }
      }
      return ComparePatternPrefixBySabot(key, Pattern);
    }

    private:

    const TIndexKey &Pattern;

    /* The pattern's members, when Quick. */
    std::array<Orly::Atom::TCore, MaxElems> Elems;

    size_t Count = 0UL;

    /* True when the pattern is a tuple of at most MaxElems members, copied into Elems. */
    bool Quick;
  };

}  // namespace

Orly::Atom::TComparison TMemoryLayer::ComparePatternPrefix(const TIndexKey &key, const TIndexKey &pattern) {
  return TPatternPrefix(pattern).Compare(key);
}

Orly::Atom::TComparison TMemoryLayer::ComparePatternPrefixBySabot(const TIndexKey &key, const TIndexKey &pattern) {
  return ::ComparePatternPrefixBySabot(key, pattern);
}

TMemoryLayer::TMemoryLayer(L0::TManager *manager)
    : TDataLayer(manager),
      UpdateCollection(this),
      EntryCollection(this) {
  for (auto &head : SkipHead) {
    head.store(nullptr, std::memory_order_relaxed);
  }
  for (auto &tail : SkipTail) {
    tail = nullptr;
  }
}

bool TMemoryLayer::EntryAfter(const TUpdate::TEntry *a, const TUpdate::TEntry *b) noexcept {
  return a->GetEntryKey() > b->GetEntryKey();
}

void TMemoryLayer::LinkEntry(TUpdate::TEntry *entry) NO_THROW {
  assert(entry->MemoryLayerMembership.TryGetCollection() != &EntryCollection);
  ++Size;
  const size_t lanes = SkipHeight(entry->GetIndexKey().GetHash());
  /* Single-writer per layer (DataLock / the merge thread), so the writer reads
     its own structure with relaxed loads. update[li] is the entry's
     predecessor on express lane li (null: the lane's head). */
  TUpdate::TEntry *update[TUpdate::TEntry::SkipMaxLevel];
  TUpdate::TEntry *pred = EntryCollection.TryGetLastMember();  // level-0 predecessor
  if (!pred || !EntryAfter(pred, entry)) {
    /* At or after the tail: append. Every lane's predecessor is its tail. */
    for (size_t li = 0; li < lanes; ++li) {
      update[li] = SkipTail[li];
    }
  } else {
    /* Out of order. Descend the lanes to the last entry that does not sort
       after this one, as SeekRun does, then finish on level 0. The lanes are
       sorted the same way as level 0, so the walk below is a step or two. */
    const size_t cur_top = SkipListLevel.load(std::memory_order_relaxed);
    TUpdate::TEntry *node = nullptr;
    for (size_t li = cur_top; li-- > 0;) {
      TUpdate::TEntry *next = node ? node->SkipFwd[li].load(std::memory_order_relaxed)
                                   : SkipHead[li].load(std::memory_order_relaxed);
      while (next && !EntryAfter(next, entry)) {
        node = next;
        next = node->SkipFwd[li].load(std::memory_order_relaxed);
      }
      if (li < lanes) {
        update[li] = node;
      }
    }
    for (size_t li = cur_top; li < lanes; ++li) {
      update[li] = nullptr;  // new top lanes start from the head
    }
    pred = node;
    TUpdate::TEntry *next = node ? node->MemoryLayerMembership.TryGetNextMember()
                                 : EntryCollection.TryGetFirstMember();
    while (next && !EntryAfter(next, entry)) {
      pred = next;
      next = pred->MemoryLayerMembership.TryGetNextMember();
    }
    /* The tail sorts after the entry, so something does. */
    assert(next);
  }
  /* Level 0 first: it is the authoritative order readers finish on. */
  entry->MemoryLayerMembership.InsertAfter(&EntryCollection, pred ? &pred->MemoryLayerMembership : nullptr);
  /* Then the express lanes, bottom-up with release stores: a concurrent reader
     that observes the node on lane li (via an acquire load of the
     predecessor's forward pointer) also observes the node's own forward
     pointer on that lane, and every lower lane was already published -- so a
     descent always resolves. */
  for (size_t li = 0; li < lanes; ++li) {
    std::atomic<TUpdate::TEntry *> &pred_fwd = update[li] ? update[li]->SkipFwd[li] : SkipHead[li];
    TUpdate::TEntry *const succ = pred_fwd.load(std::memory_order_relaxed);
    entry->SkipFwd[li].store(succ, std::memory_order_relaxed);
    pred_fwd.store(entry, std::memory_order_release);
    if (!succ) {
      SkipTail[li] = entry;
    }
  }
  if (lanes > SkipListLevel.load(std::memory_order_relaxed)) {
    SkipListLevel.store(lanes, std::memory_order_release);
  }
}

template <typename TBefore>
TMemoryLayer::TEntryCollection::TCursor TMemoryLayer::SeekPast(const TBefore &before) const {
  /* Descend the express lanes to the last entry before() is true of, then
     finish on level 0 (EntryCollection). Stale upper lanes (a concurrent
     insert in flight) only shorten the jump -- the level-0 walk below always
     reaches the right place, so the result is never wrong. */
  TUpdate::TEntry *node = nullptr;
  const size_t cur_top = SkipListLevel.load(std::memory_order_acquire);
  for (size_t li = cur_top; li-- > 0;) {
    TUpdate::TEntry *next = node ? node->SkipFwd[li].load(std::memory_order_acquire)
                                 : SkipHead[li].load(std::memory_order_acquire);
    while (next && before(*next)) {
      node = next;
      next = node->SkipFwd[li].load(std::memory_order_acquire);
    }
  }
  TEntryCollection::TCursor csr = node
      ? TEntryCollection::TCursor(&node->MemoryLayerMembership, InvCon::TOrient::Fwd)
      : TEntryCollection::TCursor(GetEntryCollection());
  while (csr && before(*csr)) {
    ++csr;
  }
  return csr;
}

TMemoryLayer::TEntryCollection::TCursor TMemoryLayer::SeekRun(const TIndexKey &key) const {
  return SeekPast([&key](const TUpdate::TEntry &entry) { return entry.GetIndexKey() < key; });
}

TMemoryLayer::TEntryCollection::TCursor TMemoryLayer::SeekPattern(const TIndexKey &pattern) const {
  const TPatternPrefix prefix(pattern);
  return SeekPast([&prefix](const TUpdate::TEntry &entry) {
    return Atom::IsLt(prefix.Compare(entry.GetIndexKey()));
  });
}

TMemoryLayer::~TMemoryLayer() {
  UpdateCollection.DeleteEachMember();
  EntryCollection.DeleteEachMember();
}

void TMemoryLayer::Insert(TUpdate *update) NO_THROW {
  LinkEntries(update);
  PublishUpdate(update);
}

void TMemoryLayer::LinkEntries(TUpdate *update) NO_THROW {
  for (TUpdate::TEntryCollection::TCursor csr(&update->EntryCollection/*, InvCon::TOrient::Rev*/); csr; ++csr) {
    LinkEntry(&*csr);
  }
}

void TMemoryLayer::PublishUpdate(TUpdate *update) NO_THROW {
  update->MemoryLayerMembership.ReverseInsert(&UpdateCollection);
}

void TMemoryLayer::ReverseInsert(TUpdate *update) NO_THROW {
  for (TUpdate::TEntryCollection::TCursor csr(&update->EntryCollection); csr; ++csr) {
    LinkEntry(&*csr);
  }
  update->MemoryLayerMembership.ReverseInsert(&UpdateCollection);
}

std::unique_ptr<TPresentWalker> TMemoryLayer::NewPresentWalker(const TIndexKey &from,
                                                               const TIndexKey &to) const {
  return make_unique<TRangePresentWalker>(this, from, to);
}

std::unique_ptr<TPresentWalker> TMemoryLayer::NewPresentWalker(const TIndexKey &key, bool exact_point) const {
  return make_unique<TMatchPresentWalker>(this, key, exact_point);
}

std::unique_ptr<Orly::Indy::TUpdateWalker> TMemoryLayer::NewUpdateWalker(TSequenceNumber from) const {
  return make_unique<TUpdateWalker>(this, from);
}

TMemoryLayer::TMatchPresentWalker::TMatchPresentWalker(const TMemoryLayer *layer,
                                                       const TIndexKey &key,
                                                       bool exact_point)
    : Orly::Indy::TPresentWalker(Match),
      Layer(layer),
      Key(key),
      Csr(Layer->GetEntryCollection()),
      ExactPoint(exact_point) {
  Key.GetKey().CheckFreesTrail();
  if (ExactPoint) {
    /* Fully-bound point read (operator[]/Exists): jump straight to key's run
       via the skip-list accelerator instead of scanning the ordered list from
       the head -- the #257 quadratic. SeekRun lands on the first entry with
       index-key >= key; if that is exactly key we hand it to the normal
       run-walk below, otherwise the key is absent in this layer and we stop
       without the forward scan a head-start would incur. */
    Csr = Layer->SeekRun(Key);
    if (!(Csr && Csr->GetIndexKey() == Key)) {
      Valid = false;
      Cached = true;  // nothing here; suppress the head-scan in Refresh()
    }
  } else {
    /* A pattern (or a whole key not flagged exact): start at the first entry
       that could unify with it, skipping other indexes and the keys whose
       bound leading members order before the pattern's, rather than matching
       every entry from the head of the layer. */
    Csr = Layer->SeekPattern(Key);
  }
  Refresh();
}

TMemoryLayer::TMatchPresentWalker::operator bool() const {
  Refresh();
  return Valid;
}

const TPresentWalker::TItem &TMemoryLayer::TMatchPresentWalker::operator*() const {
  Refresh();
  assert(Valid);
  return Item;
}

TMemoryLayer::TMatchPresentWalker &TMemoryLayer::TMatchPresentWalker::operator++() {
  Cached = false;
  ++Csr;
  Refresh();
  return *this;
}

void TMemoryLayer::TMatchPresentWalker::Refresh() const {
  if (Valid && !Cached) {
    const Atom::TCore &key_core = Key.GetKey().GetCore();
    Atom::TCore::TArena *const key_arena = Key.GetKey().GetArena();
    const bool key_is_tuple = key_core.IsTuple();
    void *key_state_alloc = nullptr;
    void *search_state_alloc = nullptr;
    for (; Csr; ++Csr) {
      Cached = true;
      const auto &csr_index_id = Csr->GetIndexKey().GetIndexId();
      if (Key.GetIndexId() < csr_index_id) {
        Valid = false;
        return;
      }
      if (Key.GetIndexId() == csr_index_id) {
        const Atom::TCore &cur_core = Csr->GetKey().GetCore();
        Atom::TCore::TArena *const cur_arena = Csr->GetKey().GetArena();
        Sabot::TMatchResult result;
        if (key_is_tuple && cur_core.IsTuple()) {
          result = key_core.PrefixMatch(key_arena, cur_core, cur_arena);
        } else {
          if (!key_state_alloc) {
            key_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
            search_state_alloc = reinterpret_cast<uint8_t *>(key_state_alloc) + Sabot::State::GetMaxStateSize();
          }
          Sabot::State::TAny::TWrapper key_state(key_core.NewState(key_arena, key_state_alloc));
          Sabot::State::TAny::TWrapper cur_state(cur_core.NewState(cur_arena, search_state_alloc));
          result = MatchPrefixState(*key_state, *cur_state);
        }
        switch (result) {
          case Sabot::TMatchResult::NoMatch: {
            /* Once a match has been seen, the first NoMatch ends the run. That is sound only because free members trail
               (see TKey::CheckFreesTrail): the matches then form one contiguous run in key order. */
            if (PassedMatch) {
              Valid = false;
              return;
            }
            [[fallthrough]];
          }
          case Sabot::TMatchResult::PrefixMatch: {
            /* Past the keys that could unify with the pattern: none further on in the index can. */
            if (Atom::IsGt(ComparePatternPrefix(Csr->GetIndexKey(), Key))) {
              Valid = false;
              return;
            }
            break;
          }
          case Sabot::TMatchResult::Unifies: {
            PassedMatch = true;
            Item.KeyArena = cur_arena;
            Item.OpArena = cur_arena;
            Item.Key = cur_core;
            Item.Op = Csr->GetOp();
            Item.SequenceNumber = Csr->GetSequenceNumber();
            Item.Mutator = Csr->GetMutator();
            /* Logical update id -- the per-transaction TUuid set in
               session.cc. Survives Tetris renumbering across repos,
               used by the context-walker fold to dedup duplicates
               that exist briefly in both child + parent repos during
               a Tetris push/pop window. */ {
              void *id_state_alloc = alloca(Sabot::State::GetMaxStateSize());
              const TUpdate *update_ptr = Csr->GetUpdate();
              Sabot::ToNative(
                  *Sabot::State::TAny::TWrapper(update_ptr->GetId().NewState(&update_ptr->GetSuprena(), id_state_alloc)),
                  Item.UpdateId);
            }
            return;
          }
        }
      }
    }
    Valid = false;
  }
}

TMemoryLayer::TRangePresentWalker::TRangePresentWalker(const TMemoryLayer *layer,
                                                       const TIndexKey &from,
                                                       const TIndexKey &to)
    : Orly::Indy::TPresentWalker(Range),
      Layer(layer),
      From(from),
      To(to),
      /* Seek to the first entry at or after From through the skip list's
         express lanes, as a point read does, instead of scanning the layer
         from its head (#735). */
      Csr(Layer->SeekRun(From)) {
  assert(From.GetIndexId() == To.GetIndexId());
  Refresh();
}

TMemoryLayer::TRangePresentWalker::operator bool() const {
  Refresh();
  return Valid;
}

const TPresentWalker::TItem &TMemoryLayer::TRangePresentWalker::operator*() const {
  Refresh();
  assert(Valid);
  assert(Cached);
  return Item;
}

TMemoryLayer::TRangePresentWalker &TMemoryLayer::TRangePresentWalker::operator++() {
  assert(Valid);
  Cached = false;
  ++Csr;
  Refresh();
  return *this;
}

void TMemoryLayer::TRangePresentWalker::Refresh() const {
  if (Valid && !Cached) {
    const Atom::TCore &to_core = To.GetKey().GetCore();
    Atom::TCore::TArena *const to_arena = To.GetKey().GetArena();
    const bool to_is_tuple = to_core.IsTuple();
    void *to_state_alloc = nullptr;
    void *cur_state_alloc = nullptr;
    for (; Csr; ++Csr) {
      Cached = true;
      const auto &csr_index_id = Csr->GetIndexKey().GetIndexId();
      if (From.GetIndexId() < csr_index_id) {
        Valid = false;
        return;
      }
      if (From.GetIndexId() == csr_index_id) {
        /* The seek in the constructor left Csr at or after From, so only
           the upper end needs checking. To is either a whole key or a
           pattern whose free members are its rightmost ones (#735); a key
           unifying with To is in range either way, and otherwise the key
           is past the range once it orders after To. */
        if (Csr->GetKey() > To.GetKey()) {
          const Atom::TCore &cur_core = Csr->GetKey().GetCore();
          Atom::TCore::TArena *const cur_arena = Csr->GetKey().GetArena();
          Sabot::TMatchResult match_res;
          if (to_is_tuple && cur_core.IsTuple()) {
            match_res = to_core.PrefixMatch(to_arena, cur_core, cur_arena);
          } else {
            if (!to_state_alloc) {
              to_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
              cur_state_alloc = static_cast<uint8_t *>(to_state_alloc) + Sabot::State::GetMaxStateSize();
            }
            Sabot::State::TAny::TWrapper to_state(to_core.NewState(to_arena, to_state_alloc));
            Sabot::State::TAny::TWrapper cur_state(cur_core.NewState(cur_arena, cur_state_alloc));
            match_res = MatchPrefixState(*to_state, *cur_state);
          }
          if (match_res != Sabot::TMatchResult::Unifies) {
            Valid = false;
            return;
          }
        }
        Item.KeyArena = Csr->GetKey().GetArena();
        Item.OpArena = Csr->GetKey().GetArena();
        Item.Key = Csr->GetKey().GetCore();
        Item.Op = Csr->GetOp();
        Item.SequenceNumber = Csr->GetSequenceNumber();
        Item.Mutator = Csr->GetMutator();
        /* See TMatchPresentWalker; same plumbing for the Tetris-window dedup. */ {
          void *id_state_alloc = alloca(Sabot::State::GetMaxStateSize());
          const TUpdate *update_ptr = Csr->GetUpdate();
          Sabot::ToNative(
              *Sabot::State::TAny::TWrapper(update_ptr->GetId().NewState(&update_ptr->GetSuprena(), id_state_alloc)),
              Item.UpdateId);
        }
        return;
      }
    }
    Valid = false;
  }
}

TMemoryLayer::TUpdateWalker::TUpdateWalker(const TMemoryLayer *layer, TSequenceNumber from)
    : Layer(layer), From(from), Csr(Layer->GetUpdateCollection()) {
  Refresh();
}

TMemoryLayer::TUpdateWalker::operator bool() const {
  return Valid;
}

const TUpdateWalker::TItem &TMemoryLayer::TUpdateWalker::operator*() const {
  assert(Valid);
  Item.MainArena = &Csr->GetSuprena();
  Item.SequenceNumber = Csr->GetSequenceNumber();
  Item.Metadata = Csr->GetMetadata();
  Item.Id = Csr->GetId();
  Item.EntryVec.clear();
  for (TUpdate::TEntryCollection::TCursor csr(Csr->GetEntryCollection()); csr; ++csr) {
    Item.EntryVec.emplace_back(csr->GetIndexKey(), csr->GetOp(), csr->GetMutator());
  }
  return Item;
}

TMemoryLayer::TUpdateWalker &TMemoryLayer::TUpdateWalker::operator++() {
  ++Csr;
  Refresh();
  return *this;
}

void TMemoryLayer::TUpdateWalker::Refresh() {
  Valid = false;
  for (;Csr; ++Csr) {
    if (Csr->GetSequenceNumber() >= From) {
      Valid = true;
      break;
    }
  }
}

void TMemoryLayer::ImporterAppendUpdate(TUpdate *update) {
  assert(update);
  update->MemoryLayerMembership.ReverseInsert(&UpdateCollection);
}

void TMemoryLayer::ImporterAppendEntry(TUpdate::TEntry *entry) {
  assert(entry);
  LinkEntry(entry);
}

/* #892: copies arrive in memory-layer order, (index id, key, seq). An update's EntryCollection is
   ordered by key alone and AddEntry walks back from the tail to find each entry's place, so adding
   them in that order is O(N^2) for an update spanning several indexes. So add them in key order
   instead. The sort is stable, so entries with equal keys (in different indexes) keep memory-layer
   order, as they'd have had when added one by one. This layer's own collection is ordered by
   index id first, so the copies are linked in the original order. The mutator is passed on (#227).
   If AddEntry throws, nothing has been linked into this layer yet, and the copies made so far
   belong to their target updates, which the layer's owner deletes. */
void TMemoryLayer::ImporterAppendCopies(const std::vector<TCopiedEntry> &copies, void *state_alloc) {
  std::vector<size_t> order(copies.size());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&copies](size_t lhs, size_t rhs) {
    return copies[lhs].Source->GetKey() < copies[rhs].Source->GetKey();
  });
  std::vector<TUpdate::TEntry *> added(copies.size(), nullptr);
  for (const size_t i : order) {
    const TUpdate::TEntry &src = *copies[i].Source;
    /* #227: MUST pass the mutator -- the 2-arg AddEntry defaults it to Assign (update.h), which
       silently turns a commutative Add into an Assign and caps the read-time fold. */
    added[i] = copies[i].Target->AddEntry(src.GetIndexKey(), TKey(src.GetOp(), &src.GetSuprena()), src.GetMutator());
  }
  for (TUpdate::TEntry *entry : added) {
    ImporterAppendEntry(entry);
  }
}
