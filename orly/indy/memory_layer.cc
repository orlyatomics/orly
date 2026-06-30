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

#include <orly/sabot/to_native.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy;

/* #257 skip-list seek accelerator helpers. */
namespace {

  /* Strict EntryCollection order: (IndexId asc, key asc, SeqNum desc). Both
     entries are concrete (stored keys never carry free vars), so the key
     comparison never throws. */
  inline bool EntryBefore(const TUpdate::TEntry *a, const TUpdate::TEntry *b) {
    if (a->GetIndexKey() < b->GetIndexKey()) {
      return true;
    }
    if (b->GetIndexKey() < a->GetIndexKey()) {
      return false;
    }
    return a->GetSequenceNumber() > b->GetSequenceNumber();
  }

  /* Number of express lanes a node joins (0 .. SkipMaxLevel-1), geometric with
     p=1/2. Derived deterministically from the entry's index-key hash so no
     global RNG state is needed and the structure is reproducible. The key hash
     (not the sequence number) is the seed because batched writes put many
     entries under one update sharing a single SeqNum -- seeding on SeqNum would
     give every entry in a batch the same height and collapse the express
     lanes. */
  inline size_t SkipHeight(size_t key_hash) {
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

}  // namespace

TMemoryLayer::TMemoryLayer(L0::TManager *manager)
    : TDataLayer(manager),
      UpdateCollection(this),
      EntryCollection(this),
      SkipListLevel(0UL),
      Size(0UL) {
  for (auto &head : SkipHead) {
    head.store(nullptr, std::memory_order_relaxed);
  }
}

void TMemoryLayer::SkipInsert(TUpdate::TEntry *entry) NO_THROW {
  const size_t lanes = SkipHeight(entry->GetIndexKey().GetHash());
  if (lanes == 0) {
    /* Level-0 only -- already linked into EntryCollection by the caller. */
    return;
  }
  /* Single-writer per layer (DataLock / the merge thread), so the writer reads
     its own structure with relaxed loads. Find the predecessor at each lane. */
  TUpdate::TEntry *update[TUpdate::TEntry::SkipMaxLevel];
  const size_t cur_top = SkipListLevel.load(std::memory_order_relaxed);
  TUpdate::TEntry *node = nullptr;
  for (size_t li = cur_top; li-- > 0;) {
    TUpdate::TEntry *next = node ? node->SkipFwd[li].load(std::memory_order_relaxed)
                                 : SkipHead[li].load(std::memory_order_relaxed);
    while (next && EntryBefore(next, entry)) {
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
  /* Link bottom-up with release stores: a concurrent reader that observes the
     node on lane li (via an acquire load of the predecessor's forward pointer)
     also observes the node's own forward pointer on that lane, and every lower
     lane was already published -- so a descent always resolves. */
  for (size_t li = 0; li < lanes; ++li) {
    std::atomic<TUpdate::TEntry *> &pred_fwd = update[li] ? update[li]->SkipFwd[li] : SkipHead[li];
    entry->SkipFwd[li].store(pred_fwd.load(std::memory_order_relaxed), std::memory_order_relaxed);
    pred_fwd.store(entry, std::memory_order_release);
  }
  if (lanes > cur_top) {
    SkipListLevel.store(lanes, std::memory_order_release);
  }
}

TMemoryLayer::TEntryCollection::TCursor TMemoryLayer::SeekRun(const TIndexKey &key) const {
  /* Descend the express lanes to the largest entry whose index-key is strictly
     less than key, then finish on level 0 (EntryCollection). Stale upper lanes
     (a concurrent insert in flight) only shorten the jump -- the level-0 walk
     below always reaches the right place, so the result is never wrong. */
  TUpdate::TEntry *node = nullptr;
  const size_t cur_top = SkipListLevel.load(std::memory_order_acquire);
  for (size_t li = cur_top; li-- > 0;) {
    TUpdate::TEntry *next = node ? node->SkipFwd[li].load(std::memory_order_acquire)
                                 : SkipHead[li].load(std::memory_order_acquire);
    while (next && next->GetIndexKey() < key) {
      node = next;
      next = node->SkipFwd[li].load(std::memory_order_acquire);
    }
  }
  TEntryCollection::TCursor csr = node
      ? TEntryCollection::TCursor(&node->MemoryLayerMembership, InvCon::TOrient::Fwd)
      : TEntryCollection::TCursor(GetEntryCollection());
  while (csr && csr->GetIndexKey() < key) {
    ++csr;
  }
  return csr;
}

TMemoryLayer::~TMemoryLayer() {
  UpdateCollection.DeleteEachMember();
  EntryCollection.DeleteEachMember();
}

void TMemoryLayer::Insert(TUpdate *update) NO_THROW {
  for (TUpdate::TEntryCollection::TCursor csr(&update->EntryCollection/*, InvCon::TOrient::Rev*/); csr; ++csr) {
    ++Size;
    csr->MemoryLayerMembership.ReverseInsert(&EntryCollection);
    SkipInsert(&*csr);  // level 0 (EntryCollection) first, then express lanes (#257)
  }
  update->MemoryLayerMembership.ReverseInsert(&UpdateCollection);
}

void TMemoryLayer::ReverseInsert(TUpdate *update) NO_THROW {
  for (TUpdate::TEntryCollection::TCursor csr(&update->EntryCollection); csr; ++csr) {
    ++Size;
    csr->MemoryLayerMembership.ReverseInsert(&EntryCollection);
    SkipInsert(&*csr);  // level 0 (EntryCollection) first, then express lanes (#257)
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
      Valid(true),
      Cached(false),
      PassedMatch(false),
      ExactPoint(exact_point) {
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
  }
  Refresh();
}

TMemoryLayer::TMatchPresentWalker::~TMatchPresentWalker() {}

inline TMemoryLayer::TMatchPresentWalker::operator bool() const {
  Refresh();
  return Valid;
}

const TPresentWalker::TItem &TMemoryLayer::TMatchPresentWalker::operator*() const {
  Refresh();
  assert(Valid);
  return Item;
}

inline TMemoryLayer::TMatchPresentWalker &TMemoryLayer::TMatchPresentWalker::operator++() {
  Cached = false;
  ++Csr;
  Refresh();
  return *this;
}

void TMemoryLayer::TMatchPresentWalker::Refresh() const {
  if (Valid && !Cached) {
    void *key_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
    void *search_state_alloc = reinterpret_cast<uint8_t *>(key_state_alloc) + Sabot::State::GetMaxStateSize();
    Sabot::State::TAny::TWrapper key_state(Key.GetKey().GetCore().NewState(Key.GetKey().GetArena(), key_state_alloc));
    for (;Csr; ++Csr) {
      Cached = true;
      Atom::TComparison index_id_comp = Atom::CompareOrdered(Key.GetIndexId(), Csr->GetIndexKey().GetIndexId());
      switch (index_id_comp) {
        case Atom::TComparison::Lt: {
          Valid = false;
          return;
        }
        case Atom::TComparison::Eq: {
          Sabot::State::TAny::TWrapper cur_state(Csr->GetKey().GetCore().NewState(Csr->GetKey().GetArena(), search_state_alloc));
          Sabot::TMatchResult result = MatchPrefixState(*key_state, *cur_state);
          switch (result) {
            case Sabot::TMatchResult::NoMatch: {
              if (PassedMatch) {
                Valid = false;
                return;
              }
              break;
            }
            case Sabot::TMatchResult::PrefixMatch: {
              break;
            }
            case Sabot::TMatchResult::Unifies: {
              PassedMatch = true;
              Item.KeyArena = Csr->GetKey().GetArena();
              Item.OpArena = Csr->GetKey().GetArena();
              Item.Key = Csr->GetKey().GetCore();
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
              break;
            }
          }
          break;
        }
        case Atom::TComparison::Gt: {
          break;
        }
        case Atom::TComparison::Ne: {
          throw;
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
      Csr(Layer->GetEntryCollection()),
      Valid(true), Cached(false), PassedMatch(false) {
  assert(From.GetIndexId() == To.GetIndexId());
  Refresh();
}

TMemoryLayer::TRangePresentWalker::~TRangePresentWalker() {}

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
    void *key_state_alloc = alloca(Sabot::State::GetMaxStateSize());
    void *key_state_alloc_2 = alloca(Sabot::State::GetMaxStateSize());
    void *key_state_alloc_3 = alloca(Sabot::State::GetMaxStateSize());
    void *search_state_alloc = alloca(Sabot::State::GetMaxStateSize());
    Sabot::State::TAny::TWrapper key_state(PassedMatch ?
                                           To.GetKey().GetCore().NewState(To.GetKey().GetArena(), key_state_alloc) :
                                           From.GetKey().GetCore().NewState(From.GetKey().GetArena(), key_state_alloc_2));
    for (;Csr; ++Csr) {
      Cached = true;
      Atom::TComparison index_id_comp = Atom::CompareOrdered(From.GetIndexId(), Csr->GetIndexKey().GetIndexId());
      switch (index_id_comp) {
        case Atom::TComparison::Lt: {
          Valid = false;
          return;
        }
        case Atom::TComparison::Eq: {
          Sabot::State::TAny::TWrapper cur_state(Csr->GetKey().GetCore().NewState(Csr->GetKey().GetArena(), search_state_alloc));
          Atom::TComparison comp = OrderStates(*cur_state, *key_state);
          if (!PassedMatch) {
            if (Atom::IsGe(comp)) {
              PassedMatch = true;
              Sabot::State::TAny::TWrapper to_state(To.GetKey().GetCore().NewState(To.GetKey().GetArena(), key_state_alloc_3));
              Atom::TComparison comp = OrderStates(*cur_state, *key_state);
              if (Atom::IsGt(comp)) {
                Valid = false;
                return;
              }
            } else {
              continue;
            }
          } else {
            if (Atom::IsGt(comp)) {
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
        case Atom::TComparison::Gt: {
          break;
        }
        case Atom::TComparison::Ne: {
          throw;
        }
      }
    }
    Valid = false;
  }
}


TMemoryLayer::TUpdateWalker::TUpdateWalker(const TMemoryLayer *layer, TSequenceNumber from)
    : Layer(layer), From(from), Csr(Layer->GetUpdateCollection()), Valid(false) {
  Refresh();
}

TMemoryLayer::TUpdateWalker::~TUpdateWalker() {}

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
  ++Size;
  entry->MemoryLayerMembership.ReverseInsert(&EntryCollection);
  SkipInsert(entry);  // level 0 (EntryCollection) first, then express lanes (#257)
}