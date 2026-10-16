/* <orly/indy/disk/key_filter.cc>

   Implements <orly/indy/disk/key_filter.h>.

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

#include <orly/indy/disk/key_filter.h>

#include <alloca.h>
#include <syslog.h>

#include <cassert>
#include <cstring>
#include <new>

using namespace std;
using namespace Orly;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;

namespace {

  /* The file's hash index takes its slot as the key's hash modulo the table size, so the low bits
     of these hashes are well spread but nothing promises the high ones are. Mix them first (the
     finaliser of MurmurHash3). */
  inline uint64_t Mix(uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
  }

  /* The block a hash falls in: its mixed hash scaled to [0, num_blocks) (Lemire's reduction). */
  inline size_t BlockOf(uint64_t mixed, size_t num_blocks) noexcept {
    return static_cast<size_t>((static_cast<unsigned __int128>(mixed) * num_blocks) >> 64);
  }

  /* The bits a hash sets within its block: 7 positions of 9 bits each (512 bits a block), taken
     from a second mix, which the block number didn't use. */
  inline uint64_t BitsOf(uint64_t mixed) noexcept {
    return Mix(mixed ^ 0x9e3779b97f4a7c15ULL);
  }

  /* Every live file filter's bytes, and their number. */
  atomic<size_t> TotalBytes(0UL);
  atomic<size_t> TotalFiles(0UL);

  /* The last 64 MB step of TotalBytes logged, so the total is logged as it grows and shrinks but
     not for every file. */
  atomic<size_t> LoggedStep(0UL);

  constexpr size_t LogStepShift = 26UL;

  void Account(ptrdiff_t bytes, ptrdiff_t files) noexcept {
    const size_t total = TotalBytes.fetch_add(static_cast<size_t>(bytes)) + static_cast<size_t>(bytes);
    const size_t num_files = TotalFiles.fetch_add(static_cast<size_t>(files)) + static_cast<size_t>(files);
    const size_t step = total >> LogStepShift;
    if (LoggedStep.exchange(step) != step) {
      syslog(LOG_INFO, "disk layer key filters hold %zu bytes over %zu files", total, num_files);
    }
  }

}  // namespace

static_assert(TKeyFilter::NumProbes * 9UL <= 64UL, "a block's bit positions must come from one 64-bit hash");

TKeyFilter::TKeyFilter(const vector<uint64_t> &hashes) {
  if (hashes.empty()) {
    return;
  }
  const size_t num_bits = hashes.size() * BitsPerKey;
  const size_t num_blocks = (num_bits + 511UL) / 512UL;
  Blocks.reset(new TBlock[num_blocks]);
  memset(Blocks.get(), 0, num_blocks * sizeof(TBlock));
  NumBlocks = num_blocks;
  for (uint64_t hash : hashes) {
    const uint64_t mixed = Mix(hash);
    uint64_t *const words = Blocks[BlockOf(mixed, NumBlocks)].Words;
    uint64_t bits = BitsOf(mixed);
    for (size_t i = 0; i < NumProbes; ++i, bits >>= 9) {
      const size_t bit = bits & 511UL;
      words[bit >> 6] |= 1ULL << (bit & 63UL);
    }
  }
}

bool TKeyFilter::MayContain(uint64_t hash) const noexcept {
  if (!NumBlocks) {
    return false;
  }
  const uint64_t mixed = Mix(hash);
  const uint64_t *const words = Blocks[BlockOf(mixed, NumBlocks)].Words;
  uint64_t bits = BitsOf(mixed);
  for (size_t i = 0; i < NumProbes; ++i, bits >>= 9) {
    const size_t bit = bits & 511UL;
    if (!(words[bit >> 6] & (1ULL << (bit & 63UL)))) {
      return false;
    }
  }
  return true;
}

TFileKeyFilter::TProbe::TProbe(const TIndexKey &key)
    : IndexId(&key.GetIndexId()), Arity(0UL), Hash(0UL) {
  /* Fully bound, as TReadFile::TIndexFile::FindInHash counts it: every member of the tuple
     defined. Only then does it look the key up by the key's own hash. */
  const TKey &k = key.GetKey();
  Atom::TCore::TArena *const arena = k.GetArena();
  const Atom::TCore &core = k.GetCore();
  if (!arena || !core.IsTuple()) {
    return;
  }
  const Atom::TCore::TOffset *const off = core.TryGetOffset();
  const uint32_t *const elem_count = core.TryGetElemCount();
  if (!off || !elem_count || !*elem_count) {
    return;
  }
  void *pin_alloc = alloca(sizeof(Atom::TCore::TArena::TFinalPin));
  Atom::TCore::TArena::TFinalPin::TWrapper pin(arena->Pin(*off, sizeof(Atom::TCore::TNote) + (sizeof(Atom::TCore) * *elem_count), pin_alloc));
  if (pin->GetNote()->GetTupleNumNonFree() != *elem_count) {
    return;
  }
  Arity = *elem_count;
  Hash = k.GetHash();
}

std::atomic<bool> TFileKeyFilter::Enabled(true);

TFileKeyFilter::TFileKeyFilter(vector<TIndex> &&indexes) noexcept
    : Indexes(std::move(indexes)), NumBytes(0UL) {
  for (const auto &index : Indexes) {
    NumBytes += index.Filter.GetNumBytes();
  }
  Account(static_cast<ptrdiff_t>(NumBytes), 1);
}

TFileKeyFilter::~TFileKeyFilter() {
  Account(-static_cast<ptrdiff_t>(NumBytes), -1);
}

bool TFileKeyFilter::MayContain(const TProbe &probe) const noexcept {
  for (const auto &index : Indexes) {
    if (index.IndexId == *probe.IndexId) {
      if (!index.HasFilter || !probe.Arity || probe.Arity != index.Arity) {
        return true;
      }
      return index.Filter.MayContain(probe.Hash);
    }
  }
  /* The file has no key in this index, so the reader would find none (TPresentWalkFile::Init). */
  return false;
}

size_t TFileKeyFilter::GetTotalBytes() noexcept {
  return TotalBytes.load(memory_order_relaxed);
}

size_t TFileKeyFilter::GetTotalFiles() noexcept {
  return TotalFiles.load(memory_order_relaxed);
}

void TKeyFilterBuilder::Push(uint64_t hash) noexcept {
  if (Dropped) {
    return;
  }
  if (Hashes.size() >= TFileKeyFilter::MaxKeysPerIndex) {
    Dropped = true;
    vector<uint64_t>().swap(Hashes);
    return;
  }
  try {
    Hashes.push_back(hash);
  } catch (const bad_alloc &) {
    Dropped = true;
    vector<uint64_t>().swap(Hashes);
  }
}

TFileKeyFilter::TIndex TKeyFilterBuilder::Make(const Base::TUuid &index_id, size_t arity) noexcept {
  TFileKeyFilter::TIndex index{index_id, arity, false, TKeyFilter()};
  if (!Dropped) {
    try {
      index.Filter = TKeyFilter(Hashes);
      index.HasFilter = true;
    } catch (const bad_alloc &) {}
  }
  vector<uint64_t>().swap(Hashes);
  Dropped = false;
  return index;
}
