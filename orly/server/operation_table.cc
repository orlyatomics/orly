/* <orly/server/operation_table.cc>

   Implements <orly/server/operation_table.h>.

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

#include <orly/server/operation_table.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

#include <base/io/binary_input_only_stream.h>
#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
#include <orly/atom/suprena.h>
#include <orly/indy/disk/wal.h>
#include <orly/server/operation_error.h>
#include <orly/var/new_sabot.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Server;

namespace {

  /* A snapshot's integers are little endian, whatever the machine. */
  template <typename TInt>
  void Put(string &out, TInt val) {
    for (size_t i = 0; i < sizeof(TInt); ++i) {
      out.push_back(static_cast<char>((static_cast<uint64_t>(val) >> (8 * i)) & 0xff));
    }
  }

  void PutUuid(string &out, const TUuid &id) {
    out.append(reinterpret_cast<const char *>(id.GetRaw()), sizeof(uuid_t));
  }

  /* Reads a snapshot, refusing one that is cut short. */
  class TReader final {
    public:

    explicit TReader(const string &bytes)
        : Bytes(bytes) {}

    template <typename TInt>
    TInt Get() {
      Need(sizeof(TInt));
      uint64_t val = 0;
      for (size_t i = 0; i < sizeof(TInt); ++i) {
        val |= static_cast<uint64_t>(static_cast<uint8_t>(Bytes[Pos + i])) << (8 * i);
      }
      Pos += sizeof(TInt);
      return static_cast<TInt>(val);
    }

    TUuid GetUuid() {
      Need(sizeof(uuid_t));
      uuid_t raw;
      memcpy(raw, Bytes.data() + Pos, sizeof(raw));
      Pos += sizeof(raw);
      return TUuid(raw);
    }

    string GetBytes(size_t len) {
      Need(len);
      string out = Bytes.substr(Pos, len);
      Pos += len;
      return out;
    }

    private:

    void Need(size_t len) const {
      if (Bytes.size() - Pos < len) {
        throw runtime_error("operation table snapshot is cut short");
      }
    }

    const string &Bytes;

    size_t Pos = 0;

  };  // TReader

  /* One entry's bytes in a snapshot: id, version, status, result, hash. */
  string EncodeEntry(const TUuid &op_id, const TOperationTable::TOutcome &outcome) {
    string out;
    PutUuid(out, op_id);
    Put<uint64_t>(out, outcome.SequenceNumber);
    Put<uint8_t>(out, outcome.Result ? 0 : 1);
    const string &result = outcome.Result ? *outcome.Result : string();
    Put<uint16_t>(out, static_cast<uint16_t>(result.size()));
    out += result;
    Put<uint64_t>(out, outcome.Hash);
    return out;
  }

  /* A length and then the bytes, so "ab","c" stays apart from "a","bc". */
  void PutBytes(string &out, const string &bytes) {
    Put<uint32_t>(out, static_cast<uint32_t>(bytes.size()));
    out += bytes;
  }

}  // namespace

TOperationTable::TOperationTable() : TOperationTable(TConfig()) {}

TOperationTable::TOperationTable(const TConfig &config)
    : Config(config) {}

uint64_t TOperationTable::GetTimestampMs(const TUuid &op_id) {
  const auto &raw = op_id.GetRaw();
  if ((raw[6] >> 4) != 7) {
    throw invalid_argument("an operation id must be a version 7 UUID (its first 48 bits are the time in ms)");
  }
  uint64_t ms = 0;
  for (size_t i = 0; i < 6; ++i) {
    ms = (ms << 8) | raw[i];
  }
  return ms;
}

TOperationTable::TReservation TOperationTable::Reserve(const TUuid &pov_id, const TUuid &op_id, uint64_t hash) {
  const uint64_t ts = GetTimestampMs(op_id);
  const uint64_t now = NowMs();
  const uint64_t age_ms = static_cast<uint64_t>(Config.RetentionAge.count()) * 1000UL;
  lock_guard<mutex> lock(Mutex);
  auto &table = TableByPov[pov_id];
  auto iter = table.SlotByOp.find(op_id);
  if (iter != table.SlotByOp.end()) {
    if (iter->second.Outcome.Hash != hash) {
      throw TOperationIdReused("operation id reused: this id already names a different request");
    }
    if (!iter->second.IsDone) {
      throw TOperationInFlight("operation in flight: this request is still running; retry shortly");
    }
    return TReservation{false, iter->second.Outcome};
  }
  if (ts <= table.ExpiredBeforeMs || ts + age_ms < now) {
    throw TOperationExpired(
        "operation expired: this id is older than the server keeps outcomes, so it can't say whether the write ran");
  }
  TSlot slot;
  slot.Outcome.Hash = hash;
  table.SlotByOp.emplace(op_id, std::move(slot));
  return TReservation{};
}

void TOperationTable::Complete(const TUuid &pov_id, const TUuid &op_id, TOutcome &&outcome) {
  lock_guard<mutex> lock(Mutex);
  auto &table = TableByPov[pov_id];
  auto &slot = table.SlotByOp[op_id];
  slot.IsDone = true;
  slot.FloorLsn = 0;
  slot.Outcome = std::move(outcome);
  Done.emplace(GetTimestampMs(op_id), pov_id, op_id);
  Prune();
}

void TOperationTable::BeginCommit(const TUuid &pov_id, const TUuid &op_id, uint64_t floor_lsn) {
  lock_guard<mutex> lock(Mutex);
  auto pov_iter = TableByPov.find(pov_id);
  if (pov_iter == TableByPov.end()) {
    return;
  }
  auto iter = pov_iter->second.SlotByOp.find(op_id);
  if (iter != pov_iter->second.SlotByOp.end() && !iter->second.IsDone) {
    iter->second.FloorLsn = max<uint64_t>(floor_lsn, 1);
  }
}

uint64_t TOperationTable::HeadFloor() const {
  lock_guard<mutex> lock(Mutex);
  uint64_t floor = UINT64_MAX;
  for (const auto &pov: TableByPov) {
    for (const auto &slot: pov.second.SlotByOp) {
      if (!slot.second.IsDone && slot.second.FloorLsn) {
        floor = min(floor, slot.second.FloorLsn);
      }
    }
  }
  return floor;
}

void TOperationTable::Release(const TUuid &pov_id, const TUuid &op_id) noexcept {
  lock_guard<mutex> lock(Mutex);
  auto table_iter = TableByPov.find(pov_id);
  if (table_iter == TableByPov.end()) {
    return;
  }
  auto &table = table_iter->second;
  auto iter = table.SlotByOp.find(op_id);
  if (iter != table.SlotByOp.end() && !iter->second.IsDone) {
    table.SlotByOp.erase(iter);
  }
  if (table.SlotByOp.empty() && table.ExpiredBeforeMs == 0) {
    TableByPov.erase(table_iter);
  }
}

void TOperationTable::Restore(const TUuid &pov_id, const TUuid &op_id, TOutcome &&outcome) {
  Complete(pov_id, op_id, std::move(outcome));
}

/* Why a slot that isn't done can't just be left out (#861): a slot is done only in Complete, after
   the transaction has appended its record, and nothing between the two holds the WAL head. The
   record's pin drops when its update reaches a root's data file (TWalRetention::OnRootFlushed), and
   with no other pin the head is the next LSN. So a checkpoint can land there, with the record
   behind its head, the slot not done and not in this snapshot, and no replay to restore it.
   BeginCommit()/HeadFloor() close it: the checkpointer takes its head no higher than HeadFloor()
   (TWalRetention::SetHeadFloor, wired in TServer), so such a record is at or past the head,
   replayed, and RestoreOperation puts the slot back. */
string TOperationTable::Snapshot(uint64_t head_lsn, size_t budget) {
  struct TItem {
    uint64_t Ts;
    const TUuid *PovId;
    const TUuid *OpId;
    string Bytes;
  };
  lock_guard<mutex> lock(Mutex);
  vector<TItem> items;
  map<TUuid, uint64_t> watermark_by_pov;
  for (const auto &pov: TableByPov) {
    for (const auto &slot: pov.second.SlotByOp) {
      const auto &outcome = slot.second.Outcome;
      if (slot.second.IsDone && outcome.Persistent && outcome.Lsn < head_lsn) {
        items.push_back(TItem{GetTimestampMs(slot.first), &pov.first, &slot.first, EncodeEntry(slot.first, outcome)});
      }
    }
    if (pov.second.ExpiredBeforeMs) {
      watermark_by_pov[pov.first] = pov.second.ExpiredBeforeMs;
    }
  }
  /* Trim the oldest until it fits, raising the watermark of the POV each came from, so what was
     trimmed is refused after a restart and never run a second time. A POV header is 28 bytes. */
  sort(items.begin(), items.end(), [](const TItem &lhs, const TItem &rhs) { return lhs.Ts < rhs.Ts; });
  size_t total = 16;  /* the 12-byte payload and section header, and the 4-byte POV count */
  set<TUuid> povs;
  for (const auto &item: items) {
    total += item.Bytes.size();
    povs.insert(*item.PovId);
  }
  for (const auto &item: watermark_by_pov) {
    povs.insert(item.first);
  }
  total += povs.size() * 28;
  size_t first = 0;
  while (first < items.size() && total > budget) {
    const auto &item = items[first++];
    total -= item.Bytes.size();
    auto &watermark = watermark_by_pov[*item.PovId];
    watermark = max(watermark, item.Ts);
  }
  map<TUuid, vector<const TItem *>> items_by_pov;
  for (size_t i = first; i < items.size(); ++i) {
    items_by_pov[*items[i].PovId].push_back(&items[i]);
  }
  for (const auto &item: watermark_by_pov) {
    items_by_pov[item.first];
  }
  string body;
  Put<uint32_t>(body, static_cast<uint32_t>(items_by_pov.size()));
  for (const auto &pov: items_by_pov) {
    PutUuid(body, pov.first);
    auto watermark = watermark_by_pov.find(pov.first);
    Put<uint64_t>(body, watermark == watermark_by_pov.end() ? 0UL : watermark->second);
    Put<uint32_t>(body, static_cast<uint32_t>(pov.second.size()));
    for (const auto *item: pov.second) {
      body += item->Bytes;
    }
  }
  string out;
  Put<uint16_t>(out, PayloadVersion);
  Put<uint16_t>(out, 1);
  Put<uint16_t>(out, OperationsSection);
  Put<uint16_t>(out, SectionRequired);
  Put<uint32_t>(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

void TOperationTable::Load(const string &payload) {
  TReader reader(payload);
  const auto version = reader.Get<uint16_t>();
  if (version != PayloadVersion) {
    throw runtime_error("the checkpoint's operation table has version " + to_string(version) +
        ", which this server does not know");
  }
  const auto section_count = reader.Get<uint16_t>();
  for (uint16_t s = 0; s < section_count; ++s) {
    const auto type = reader.Get<uint16_t>();
    const auto flags = reader.Get<uint16_t>();
    const auto len = reader.Get<uint32_t>();
    string body = reader.GetBytes(len);
    if (type == OperationsSection) {
      LoadOperations(body);
    } else if (flags & SectionRequired) {
      throw runtime_error("the checkpoint has a required section of type " + to_string(type) +
          ", which this server does not know");
    }
  }
}

void TOperationTable::LoadOperations(const string &body) {
  TReader reader(body);
  const auto pov_count = reader.Get<uint32_t>();
  for (uint32_t p = 0; p < pov_count; ++p) {
    const TUuid pov_id = reader.GetUuid();
    const auto watermark = reader.Get<uint64_t>();
    const auto entry_count = reader.Get<uint32_t>();
    {
      lock_guard<mutex> lock(Mutex);
      auto &table = TableByPov[pov_id];
      table.ExpiredBeforeMs = max(table.ExpiredBeforeMs, watermark);
    }
    for (uint32_t e = 0; e < entry_count; ++e) {
      const TUuid op_id = reader.GetUuid();
      TOutcome outcome;
      outcome.SequenceNumber = reader.Get<uint64_t>();
      const auto status = reader.Get<uint8_t>();
      const auto len = reader.Get<uint16_t>();
      string result = reader.GetBytes(len);
      if (status == 0) {
        outcome.Result = std::move(result);
      }
      outcome.Hash = reader.Get<uint64_t>();
      outcome.Persistent = true;
      Restore(pov_id, op_id, std::move(outcome));
    }
  }
}

size_t TOperationTable::GetCount() const {
  lock_guard<mutex> lock(Mutex);
  return Done.size();
}

uint64_t TOperationTable::NowMs() const {
  if (Config.Clock) {
    return Config.Clock();
  }
  return static_cast<uint64_t>(
      chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count());
}

void TOperationTable::Prune() {
  const uint64_t now = NowMs();
  const uint64_t age_ms = static_cast<uint64_t>(Config.RetentionAge.count()) * 1000UL;
  const uint64_t cutoff = now > age_ms ? now - age_ms : 0UL;
  while (!Done.empty()) {
    const auto iter = Done.begin();
    const uint64_t ts = get<0>(*iter);
    if (Done.size() <= Config.RetentionCount && ts >= cutoff) {
      break;
    }
    /* Only the POV that owned the entry stops answering for ids that old. */
    auto pov_iter = TableByPov.find(get<1>(*iter));
    if (pov_iter != TableByPov.end()) {
      pov_iter->second.SlotByOp.erase(get<2>(*iter));
      pov_iter->second.ExpiredBeforeMs = max(pov_iter->second.ExpiredBeforeMs, ts);
    }
    Done.erase(iter);
  }
}

uint64_t Orly::Server::HashCalls(const vector<TMetaRecord::TEntry::TCall> &calls) {
  string bytes;
  Put<uint32_t>(bytes, static_cast<uint32_t>(calls.size()));
  for (const auto &call: calls) {
    Put<uint32_t>(bytes, static_cast<uint32_t>(call.PackageFqName.size()));
    for (const auto &part: call.PackageFqName) {
      PutBytes(bytes, part);
    }
    PutBytes(bytes, call.MethodName);
    uint32_t arg_count = 0;
    for (const auto &arg: call.ArgByName) {
      if (arg.first != TMetaRecord::TEntry::OpIdKey && arg.first != TMetaRecord::TEntry::OpOutcomeKey) {
        ++arg_count;
      }
    }
    Put<uint32_t>(bytes, arg_count);
    /* The map is ordered by name, bytewise. */
    for (const auto &arg: call.ArgByName) {
      if (arg.first == TMetaRecord::TEntry::OpIdKey || arg.first == TMetaRecord::TEntry::OpOutcomeKey) {
        continue;
      }
      PutBytes(bytes, arg.first);
      PutBytes(bytes, EncodeResult(arg.second));
    }
  }
  return Indy::Disk::ComputeChecksum64(bytes.data(), bytes.size());
}

string Orly::Server::EncodeResult(const Var::TVar &result) {
  auto arena = make_shared<Atom::TSuprena>();
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Atom::TCore core(arena.get(), Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, result)).get());
  TMethodResult method_result(arena, core, nullopt);
  auto recorder = make_shared<Io::TRecorder>();
  /* write */ {
    Io::TBinaryOutputOnlyStream strm(recorder);
    method_result.Write(strm);
  }
  string bytes;
  recorder->CopyOut(bytes);
  return bytes;
}

TMethodResult Orly::Server::DecodeResult(const string &bytes) {
  auto recorder = make_shared<Io::TRecorder>(bytes);
  Io::TBinaryInputOnlyStream strm(make_shared<Io::TPlayer>(recorder));
  TMethodResult method_result;
  strm >> method_result;
  return method_result;
}
