/* <orly/indy/update.cc>

   Implements <orly/indy/update.h>.

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

#include <orly/indy/update.h>

#include <algorithm>

using namespace std;
using namespace Base;
using namespace Orly::Atom;
using namespace Orly::Indy;

TUpdate::TPersistenceNotification::TPersistenceNotification(std::function<void (TResult)> cb)
    : Cb(std::move(cb)) {}

TUpdate::TPersistenceNotification::~TPersistenceNotification() = default;

TUpdate::TWriteAdmission::~TWriteAdmission() {
  if (NumUpdates) {
    Pool.ReleaseAdmitted(NumUpdates);
    TEntry::Pool.ReleaseAdmitted(NumEntries);
  }
}

bool TUpdate::TWriteAdmission::TryAcquire(size_t num_entries) {
  assert(!NumUpdates);
  constexpr size_t updates = 2UL;
  const size_t entries = num_entries * 2UL;
  if (!Pool.TryAdmit(updates, &Refusal)) {
    EntryPoolRefused = false;
    return false;
  }
  if (!TEntry::Pool.TryAdmit(entries, &Refusal)) {
    EntryPoolRefused = true;
    Pool.ReleaseAdmitted(updates);
    return false;
  }
  NumUpdates = updates;
  NumEntries = entries;
  return true;
}

bool TUpdate::TWriteAdmission::ArmRoomWake() const {
  Util::TPool &pool = EntryPoolRefused ? TEntry::Pool : Pool;
  /* TryAdmit admits once in use + promised + claimed + asked <= Limit. */
  const size_t level = Refusal.Limit > Refusal.Asked ? Refusal.Limit - Refusal.Asked : 0UL;
  if (pool.GetRoomWakeLevel() == level) {
    return false;
  }
  pool.ArmRoomWake(level);
  return true;
}

void TUpdate::TWriteAdmission::DisarmRoomWakes() {
  Pool.DisarmRoomWake();
  TEntry::Pool.DisarmRoomWake();
}

TUpdate::TCopyClaim::~TCopyClaim() {
  Release();
}

bool TUpdate::TCopyClaim::TryAcquire(size_t num_updates, size_t num_entries, bool keep_reserve) {
  assert(!NumUpdates && !NumEntries);
  if (!Pool.TryClaim(num_updates, keep_reserve)) {
    return false;
  }
  if (!TEntry::Pool.TryClaim(num_entries, keep_reserve)) {
    Pool.ReleaseClaim(num_updates);
    return false;
  }
  NumUpdates = num_updates;
  NumEntries = num_entries;
  return true;
}

void TUpdate::TCopyClaim::ReleaseSome(size_t num_updates, size_t num_entries) {
  num_updates = std::min(num_updates, NumUpdates);
  num_entries = std::min(num_entries, NumEntries);
  Pool.ReleaseClaim(num_updates);
  TEntry::Pool.ReleaseClaim(num_entries);
  NumUpdates -= num_updates;
  NumEntries -= num_entries;
}

void TUpdate::TCopyClaim::Release() {
  Pool.ReleaseClaim(NumUpdates);
  TEntry::Pool.ReleaseClaim(NumEntries);
  NumUpdates = 0UL;
  NumEntries = 0UL;
}

void TUpdate::SetPoolReservePct(size_t pct) {
  pct = std::min<size_t>(pct, 100UL);
  Pool.SetReserve(Pool.GetMaxBlocks() / 100UL * pct + Pool.GetMaxBlocks() % 100UL * pct / 100UL);
  TEntry::Pool.SetReserve(TEntry::Pool.GetMaxBlocks() / 100UL * pct + TEntry::Pool.GetMaxBlocks() % 100UL * pct / 100UL);
}

shared_ptr<TUpdate> TUpdate::NewUpdate(const TOpByKey &op_by_key, const TKey &metadata, const TKey &id) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  return std::shared_ptr<TUpdate>(new TUpdate(op_by_key, metadata, id, state_alloc));
}

shared_ptr<TUpdate> TUpdate::NewUpdate(const TOpByKey &op_by_key, const vector<TNewEntry> &more, const TKey &metadata, const TKey &id) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  return std::shared_ptr<TUpdate>(new TUpdate(op_by_key, metadata, id, state_alloc, more));
}

shared_ptr<TUpdate> TUpdate::ReconstructUpdate(TSequenceNumber seq_num) {
  return std::shared_ptr<TUpdate>(new TUpdate(seq_num));
}

TUpdate *TUpdate::CopyUpdate(TUpdate *that, void *state_alloc) {
  //void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  return new TUpdate(that, state_alloc);
}

TUpdate *TUpdate::ShallowCopy(TUpdate *that, void *state_alloc) {
  return new TUpdate(state_alloc, that);
}

bool TUpdate::TEntry::TEntryKey::operator==(const TUpdate::TEntry::TEntryKey &that) const {
  return Entry->GetSequenceNumber() == that.Entry->GetSequenceNumber() && Entry->IndexKey == that.Entry->IndexKey;
}

bool TUpdate::TEntry::TEntryKey::operator!=(const TUpdate::TEntry::TEntryKey &that) const {
  return !(*this == that);
}

bool TUpdate::TEntry::TEntryKey::operator<=(const TUpdate::TEntry::TEntryKey &that) const {
  if (Entry->IndexKey.GetIndexId() < that.Entry->IndexKey.GetIndexId()) {
    return true;
  }
  if (that.Entry->IndexKey.GetIndexId() < Entry->IndexKey.GetIndexId()) {
    return false;
  }
  const auto comp = Entry->GetKey().Compare(that.Entry->GetKey());
  return Atom::IsLt(comp) || (IsEq(comp) && (Entry->GetSequenceNumber() >= that.Entry->GetSequenceNumber()));
}

bool TUpdate::TEntry::TEntryKey::operator>(const TUpdate::TEntry::TEntryKey &that) const {
  if (Entry->IndexKey.GetIndexId() < that.Entry->IndexKey.GetIndexId()) {
    return false;
  }
  if (that.Entry->IndexKey.GetIndexId() < Entry->IndexKey.GetIndexId()) {
    return true;
  }
  const auto comp = Entry->GetKey().Compare(that.Entry->GetKey());
  return Atom::IsGt(comp) || (IsEq(comp) && (Entry->GetSequenceNumber() < that.Entry->GetSequenceNumber()));
}

TUpdate::TEntry::TEntryKey::TEntryKey(const TEntry *entry)
    : Entry(entry) {}

TUpdate::TEntry::TEntry(TUpdate *update, const TIndexKey &index_key, const TKey &op, void *state_alloc, TMutator mutator)
    : IndexKey(index_key.GetIndexId(), TKey(&update->Suprena, state_alloc, index_key.GetKey())),
      UpdateMembership(this, IndexKey.GetKey(), InvCon::TOrient::Rev, &update->EntryCollection),
      MemoryLayerMembership(this, TEntryKey(this)),
      Op(&update->Suprena, Sabot::State::TAny::TWrapper(op.GetCore().NewState(op.GetArena(), state_alloc))),
      Mutator(mutator) {
  for (auto &fwd : SkipFwd) {
    fwd.store(nullptr, std::memory_order_relaxed);
  }
  void *type_alloc = alloca(Sabot::Type::GetMaxTypeSize());
  #ifndef NDEBUG
  Sabot::AssertTuple(*Sabot::Type::TAny::TWrapper(GetKey().GetCore().GetType(&update->Suprena, type_alloc)));
  #endif
  IndexKey.GetKey().GetCore().TrySetStoredHash(GetKey().GetHash());
}

TUpdate::TUpdate(const TOpByKey &op_by_key, const TKey &metadata, const TKey &id, void *state_alloc, const vector<TNewEntry> &more)
    : EntryCollection(this),
      MemoryLayerMembership(this, 0UL),
      Metadata(&Suprena, Sabot::State::TAny::TWrapper(metadata.GetCore().NewState(metadata.GetArena(), state_alloc))),
      Id(&Suprena, Sabot::State::TAny::TWrapper(id.GetCore().NewState(id.GetArena(), state_alloc))) {
  /* EntryCollection is ordered by key alone, and each entry finds its place walking back from
     the tail, so entries added in key order each append in O(1). op_by_key is ordered by index
     id first, so across several indexes its order isn't key order, and in that order each entry
     can walk back past most of those already in: O(N^2) for an update spanning several indexes
     (#879). So sort them all by key first. The sort is stable, so entries with equal keys (in
     different indexes) keep the order they'd have been linked in: op_by_key's, then more's. */
  struct TPending {
    const TIndexKey *IndexKey;
    const TKey *Op;
    TMutator Mutator;
  };
  vector<TPending> pending;
  pending.reserve(op_by_key.size() + more.size());
  for (const auto &[index_key, op_key] : op_by_key) {
    pending.push_back({ &index_key, &op_key, TMutator::Assign });
  }
  for (const auto &[index_key, op_key, mutator] : more) {
    pending.push_back({ &index_key, &op_key, mutator });
  }
  std::ranges::stable_sort(pending, [](const TKey &lhs, const TKey &rhs) { return lhs < rhs; },
                           [](const TPending &entry) -> const TKey & { return entry.IndexKey->GetKey(); });
  try {
    for (const auto &entry : pending) {
      new TEntry(this, *entry.IndexKey, *entry.Op, state_alloc, entry.Mutator);
    }
  } catch (...) {
    EntryCollection.DeleteEachMember();
    throw;
  }
}

TUpdate::TUpdate(const TUpdate *that, void *state_alloc)
    : EntryCollection(this),
      MemoryLayerMembership(this, that->GetSequenceNumber()),
      Metadata(&Suprena, Sabot::State::TAny::TWrapper(that->Metadata.NewState(&that->Suprena, state_alloc))),
      Id(&Suprena, Sabot::State::TAny::TWrapper(that->Id.NewState(&that->Suprena, state_alloc))),
      PersistenceNotification(that->PersistenceNotification) {
  try {
    for (TEntryCollection::TCursor csr(&that->EntryCollection, InvCon::TOrient::Fwd /*Rev*/); csr; ++csr) {
      new TEntry(this, csr->GetIndexKey(), TKey(csr->GetOp(), static_cast<TCore::TArena *>(&that->Suprena)), state_alloc, csr->GetMutator());
    }
  } catch (...) {
    EntryCollection.DeleteEachMember();
    /* Rethrow (#607). Swallowing this returned a copy with no entries: a transaction's copy of
       a write (TPusher) then committed it, so a write that ran out of Update Entry pool was
       acknowledged and lost, a Tetris promotion moved an empty update into the parent, and
       the root's next flush crashed on a memory layer with updates but no entries. */
    throw;
  }
}

/* Shallow Copy! */
TUpdate::TUpdate(void *state_alloc, const TUpdate *that)
    : EntryCollection(this),
      MemoryLayerMembership(this, that->GetSequenceNumber()),
      Metadata(&Suprena, Sabot::State::TAny::TWrapper(that->Metadata.NewState(&that->Suprena, state_alloc))),
      Id(&Suprena, Sabot::State::TAny::TWrapper(that->Id.NewState(&that->Suprena, state_alloc))),
      PersistenceNotification(that->PersistenceNotification) {}

TUpdate::TUpdate(TSequenceNumber seq_num)
    : EntryCollection(this),
      MemoryLayerMembership(this, seq_num) {}

TUpdate::~TUpdate() {
  EntryCollection.DeleteEachMember();
}

size_t TUpdate::ComputeLogicalBytes() const noexcept {
  size_t count = 0UL;
  for (TEntryCollection::TCursor csr(&EntryCollection); csr; ++csr) {
    ++count;
  }
  return count * (sizeof(Atom::TCore) + sizeof(Atom::TCore)) + Suprena.GetByteSize();
}