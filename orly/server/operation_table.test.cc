/* <orly/server/operation_table.test.cc>

   Unit test for <orly/server/operation_table.h>: the states a request with an operation id can be
   found in (#733), retention, and the checkpoint payload that carries the table across the log's
   retirement of the records that held it.

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
#include <stdexcept>

#include <orly/server/operation_error.h>
#include <orly/type/type_czar.h>
#include <orly/var/util.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Server;

Type::TTypeCzar TypeCzar;

/* The table's idea of now, which a test moves. */
static uint64_t Now = 10'000'000UL;

static TOperationTable::TConfig MakeConfig(size_t count = 4096, size_t seconds = 3600, size_t result_bytes = 256) {
  TOperationTable::TConfig config;
  config.RetentionCount = count;
  config.RetentionAge = chrono::seconds(seconds);
  config.ResultBytes = result_bytes;
  config.Clock = [] { return Now; };
  return config;
}

/* A version 7 UUID that says it was made at `ms`; `n` tells ids made at the same time apart. */
static TUuid MakeId(uint64_t ms, uint8_t n = 0) {
  uuid_t raw;
  memset(raw, 0, sizeof(raw));
  for (size_t i = 0; i < 6; ++i) {
    raw[i] = static_cast<uint8_t>(ms >> (8 * (5 - i)));
  }
  raw[6] = 0x70;
  raw[8] = 0x80;
  raw[15] = n;
  return TUuid(raw);
}

static const TUuid Pov(TUuid::Twister);
static const TUuid OtherPov(TUuid::Twister);

static TOperationTable::TOutcome MakeOutcome(uint64_t seq, uint64_t lsn, bool persistent, uint64_t hash,
                                              optional<string> result = string("r")) {
  TOperationTable::TOutcome outcome;
  outcome.SequenceNumber = seq;
  outcome.Lsn = lsn;
  outcome.Persistent = persistent;
  outcome.Result = std::move(result);
  outcome.Hash = hash;
  return outcome;
}

/* A new id is reserved, and once completed a retry gets the first answer back. */
FIXTURE(NewThenHit) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto id = MakeId(Now);
  auto first = table.Reserve(Pov, id, 7);
  EXPECT_TRUE(first.IsNew);
  table.Complete(Pov, id, MakeOutcome(42, 9, true, 7));
  auto retry = table.Reserve(Pov, id, 7);
  EXPECT_FALSE(retry.IsNew);
  EXPECT_EQ(retry.Outcome.SequenceNumber, 42UL);
  EXPECT_EQ(retry.Outcome.Lsn, 9UL);
  EXPECT_TRUE(retry.Outcome.Result.has_value());
  EXPECT_EQ(*retry.Outcome.Result, string("r"));
  EXPECT_EQ(table.GetCount(), 1UL);
}

/* The same id on another POV is another request. */
FIXTURE(PerPov) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto id = MakeId(Now);
  table.Reserve(Pov, id, 1);
  table.Complete(Pov, id, MakeOutcome(1, 1, true, 1));
  EXPECT_TRUE(table.Reserve(OtherPov, id, 2).IsNew);
}

/* A retry while the first is running is told so, and can go again once the first gave up. */
FIXTURE(InFlight) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto id = MakeId(Now);
  table.Reserve(Pov, id, 3);
  auto retry = [&] { table.Reserve(Pov, id, 3); };
  EXPECT_THROW_FUNC(TOperationInFlight, retry);
  table.Release(Pov, id);
  EXPECT_TRUE(table.Reserve(Pov, id, 3).IsNew);
}

/* An id that names a different request is refused, running or done. */
FIXTURE(Reused) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto id = MakeId(Now);
  table.Reserve(Pov, id, 3);
  auto running = [&] { table.Reserve(Pov, id, 4); };
  EXPECT_THROW_FUNC(TOperationIdReused, running);
  table.Complete(Pov, id, MakeOutcome(1, 1, true, 3));
  auto done = [&] { table.Reserve(Pov, id, 4); };
  EXPECT_THROW_FUNC(TOperationIdReused, done);
}

/* An id older than the retention age is refused rather than run. */
FIXTURE(ExpiredByAge) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig(512, 60));
  auto old_id = MakeId(Now - 61'000UL);
  auto old_call = [&] { table.Reserve(Pov, old_id, 1); };
  EXPECT_THROW_FUNC(TOperationExpired, old_call);
  EXPECT_TRUE(table.Reserve(Pov, MakeId(Now - 59'000UL), 1).IsNew);
}

/* A slave refuses an id from before it joined unless it has the id's entry, in every POV (#862). */
FIXTURE(UnknownBeforeJoin) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto seen = MakeId(Now - 2000UL);
  table.Restore(Pov, seen, MakeOutcome(5, 3, true, 1));
  table.RefuseUnknownBefore(Now - 1000UL);
  EXPECT_FALSE(table.Reserve(Pov, seen, 1).IsNew);
  auto unseen = [&] { table.Reserve(Pov, MakeId(Now - 1500UL), 1); };
  EXPECT_THROW_FUNC(TOperationExpired, unseen);
  auto unseen_other = [&] { table.Reserve(OtherPov, MakeId(Now - 1000UL), 1); };
  EXPECT_THROW_FUNC(TOperationExpired, unseen_other);
  EXPECT_TRUE(table.Reserve(OtherPov, MakeId(Now - 999UL), 1).IsNew);
  /* It only rises. */
  table.RefuseUnknownBefore(Now - 5000UL);
  auto still = [&] { table.Reserve(Pov, MakeId(Now - 1200UL), 1); };
  EXPECT_THROW_FUNC(TOperationExpired, still);
}

/* An entry that retention dropped leaves its id refused, not forgotten: running it again would
   write twice. */
FIXTURE(ExpiredByCount) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig(2));
  auto a = MakeId(Now - 3000UL), b = MakeId(Now - 2000UL), c = MakeId(Now - 1000UL);
  for (const auto &id: {a, b, c}) {
    table.Reserve(Pov, id, 1);
    table.Complete(Pov, id, MakeOutcome(1, 1, true, 1));
  }
  EXPECT_EQ(table.GetCount(), 2UL);
  auto dropped = [&] { table.Reserve(Pov, a, 1); };
  EXPECT_THROW_FUNC(TOperationExpired, dropped);
  EXPECT_FALSE(table.Reserve(Pov, b, 1).IsNew);
  EXPECT_FALSE(table.Reserve(Pov, c, 1).IsNew);
  /* An id older than the watermark is refused even if it was never seen. */
  auto older = [&] { table.Reserve(Pov, MakeId(Now - 3500UL), 1); };
  EXPECT_THROW_FUNC(TOperationExpired, older);
}

/* The count is for the whole table, not per POV: the oldest entry anywhere goes first, and only
   its own POV's watermark rises. */
FIXTURE(CountIsGlobal) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig(3));
  auto a = MakeId(Now - 4000UL), b = MakeId(Now - 3000UL), c = MakeId(Now - 2000UL), d = MakeId(Now - 1000UL);
  table.Reserve(Pov, a, 1);
  table.Complete(Pov, a, MakeOutcome(1, 1, true, 1));
  table.Reserve(OtherPov, b, 1);
  table.Complete(OtherPov, b, MakeOutcome(2, 2, true, 1));
  table.Reserve(OtherPov, c, 1);
  table.Complete(OtherPov, c, MakeOutcome(3, 3, true, 1));
  EXPECT_EQ(table.GetCount(), 3UL);
  table.Reserve(OtherPov, d, 1);
  table.Complete(OtherPov, d, MakeOutcome(4, 4, true, 1));
  EXPECT_EQ(table.GetCount(), 3UL);
  /* a, the oldest, belonged to Pov: it is refused, and Pov's watermark is the only one that rose. */
  auto dropped = [&] { table.Reserve(Pov, a, 1); };
  EXPECT_THROW_FUNC(TOperationExpired, dropped);
  EXPECT_FALSE(table.Reserve(OtherPov, b, 1).IsNew);
  /* An id for OtherPov between a and b is still new there: its watermark did not move. */
  EXPECT_TRUE(table.Reserve(OtherPov, MakeId(Now - 3500UL), 1).IsNew);
}

/* Only a version 7 UUID carries a time.  Random stamps version 4; Twister's bits are all random,
   so one in sixteen of its ids reads as version 7. */
FIXTURE(NotVersion7) {
  TOperationTable table(MakeConfig());
  auto call = [&] { table.Reserve(Pov, TUuid(TUuid::Random), 1); };
  EXPECT_THROW_FUNC(invalid_argument, call);
}

/* A snapshot holds the persistent entries the log no longer replays, and a table loaded from it
   answers a retry as the first did. */
FIXTURE(SnapshotRoundTrip) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto kept = MakeId(Now - 3000UL), in_log = MakeId(Now - 2000UL), fast = MakeId(Now - 1000UL, 1);
  auto over_cap = MakeId(Now - 500UL);
  table.Reserve(Pov, kept, 11);
  table.Complete(Pov, kept, MakeOutcome(5, 10, true, 11));
  table.Reserve(Pov, in_log, 12);
  table.Complete(Pov, in_log, MakeOutcome(6, 20, true, 12));
  table.Reserve(Pov, fast, 13);
  table.Complete(Pov, fast, MakeOutcome(7, 0, false, 13));
  table.Reserve(OtherPov, over_cap, 14);
  table.Complete(OtherPov, over_cap, MakeOutcome(8, 5, true, 14, nullopt));
  auto payload = table.Snapshot(15);
  TOperationTable loaded(MakeConfig());
  loaded.Load(payload);
  EXPECT_EQ(loaded.GetCount(), 2UL);
  auto hit = loaded.Reserve(Pov, kept, 11);
  EXPECT_FALSE(hit.IsNew);
  EXPECT_EQ(hit.Outcome.SequenceNumber, 5UL);
  EXPECT_EQ(*hit.Outcome.Result, string("r"));
  /* The log still has these, so they come from replay, not the snapshot. */
  EXPECT_TRUE(loaded.Reserve(Pov, in_log, 12).IsNew);
  EXPECT_TRUE(loaded.Reserve(Pov, fast, 13).IsNew);
  auto big = loaded.Reserve(OtherPov, over_cap, 14);
  EXPECT_FALSE(big.IsNew);
  EXPECT_FALSE(big.Outcome.Result.has_value());
  EXPECT_EQ(big.Outcome.SequenceNumber, 8UL);
}

/* #861: a write whose record is appended but not yet completed. Nothing holds the WAL head there
   (its update may already be in a data file), so a checkpoint is taken at the lower of the pins and
   HeadFloor(), as TWalRetention does. Without that, the head passes the record, the snapshot lacks
   the not-done slot, replay lacks the record, and a retry after a crash runs the write again. */
FIXTURE(CheckpointBetweenCommitAndComplete) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto op = MakeId(Now - 1000UL);
  const uint64_t record_lsn = 20UL;
  /* The pins are gone, so the head is the next LSN. */
  const uint64_t pin_head = record_lsn + 1UL;
  table.Reserve(Pov, op, 31);
  EXPECT_EQ(table.HeadFloor(), UINT64_MAX);
  table.BeginCommit(Pov, op, record_lsn);
  /* The record is appended, the slot is not done, and the checkpoint lands. */
  const uint64_t head = std::min(pin_head, table.HeadFloor());
  EXPECT_EQ(head, record_lsn);
  auto payload = table.Snapshot(head);
  /* Crash, then recover: load the checkpoint, replay the records at or past the head. */
  TOperationTable recovered(MakeConfig());
  recovered.Load(payload);
  if (record_lsn >= head) {
    recovered.Restore(Pov, op, MakeOutcome(9, record_lsn, true, 31));
  }
  auto hit = recovered.Reserve(Pov, op, 31);
  EXPECT_FALSE(hit.IsNew);
  EXPECT_EQ(hit.Outcome.SequenceNumber, 9UL);
  /* Complete and Release both lift the floor. */
  table.Complete(Pov, op, MakeOutcome(9, record_lsn, true, 31));
  EXPECT_EQ(table.HeadFloor(), UINT64_MAX);
  auto op2 = MakeId(Now - 900UL);
  table.Reserve(Pov, op2, 32);
  table.BeginCommit(Pov, op2, 30);
  EXPECT_EQ(table.HeadFloor(), 30UL);
  table.Release(Pov, op2);
  EXPECT_EQ(table.HeadFloor(), UINT64_MAX);
}

/* #866: a slave appends a replicated record before Restore() makes its slot, so there is no slot
   for BeginCommit() to mark. HoldHead() floors the head across that window instead. */
FIXTURE(CheckpointBetweenReplicatedAppendAndRestore) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto op = MakeId(Now - 1000UL);
  const uint64_t record_lsn = 20UL;
  const uint64_t pin_head = record_lsn + 1UL;
  auto token = table.HoldHead(record_lsn);
  /* The record is appended, Restore() hasn't run, and the checkpoint lands. */
  const uint64_t head = std::min(pin_head, table.HeadFloor());
  EXPECT_EQ(head, record_lsn);
  auto payload = table.Snapshot(head);
  table.Restore(Pov, op, MakeOutcome(9, record_lsn, true, 31));
  table.ReleaseHead(token);
  EXPECT_EQ(table.HeadFloor(), UINT64_MAX);
  /* Crash, then recover: the record is at the head, so replay restores the entry. */
  TOperationTable recovered(MakeConfig());
  recovered.Load(payload);
  if (record_lsn >= head) {
    recovered.Restore(Pov, op, MakeOutcome(9, record_lsn, true, 31));
  }
  auto hit = recovered.Reserve(Pov, op, 31);
  EXPECT_FALSE(hit.IsNew);
  EXPECT_EQ(hit.Outcome.SequenceNumber, 9UL);
  /* Holds stack: equal floors each need a release, and the lowest wins. */
  auto a = table.HoldHead(40);
  auto b = table.HoldHead(40);
  auto c = table.HoldHead(35);
  EXPECT_EQ(table.HeadFloor(), 35UL);
  table.ReleaseHead(c);
  EXPECT_EQ(table.HeadFloor(), 40UL);
  table.ReleaseHead(a);
  EXPECT_EQ(table.HeadFloor(), 40UL);
  table.ReleaseHead(b);
  EXPECT_EQ(table.HeadFloor(), UINT64_MAX);
}

/* A snapshot too big for its budget drops the oldest entries and refuses their ids after the
   load, as if retention had dropped them. */
FIXTURE(SnapshotTrim) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  vector<TUuid> ids;
  for (size_t i = 0; i < 20; ++i) {
    ids.push_back(MakeId(Now - 20'000UL + i * 100UL));
    table.Reserve(Pov, ids.back(), i);
    table.Complete(Pov, ids.back(), MakeOutcome(i + 1, 1, true, i));
  }
  const size_t budget = 16 + 28 + 5 * (16 + 8 + 1 + 2 + 1 + 8);
  auto payload = table.Snapshot(100, budget);
  EXPECT_TRUE(payload.size() <= budget);
  TOperationTable loaded(MakeConfig());
  loaded.Load(payload);
  EXPECT_EQ(loaded.GetCount(), 5UL);
  auto oldest = [&] { loaded.Reserve(Pov, ids.front(), 0); };
  EXPECT_THROW_FUNC(TOperationExpired, oldest);
  EXPECT_FALSE(loaded.Reserve(Pov, ids.back(), 19).IsNew);
}

/* An empty table still snapshots, and reloads empty. */
FIXTURE(SnapshotEmpty) {
  TOperationTable table(MakeConfig());
  auto payload = table.Snapshot(100);
  EXPECT_FALSE(payload.empty());
  TOperationTable loaded(MakeConfig());
  loaded.Load(payload);
  EXPECT_EQ(loaded.GetCount(), 0UL);
}

/* A payload from some other version, or cut short, is refused rather than half read. */
FIXTURE(BadPayload) {
  TOperationTable table(MakeConfig());
  string unknown_tag("\x09\x00\x00\x00\x00\x00", 6);
  auto unknown = [&] { table.Load(unknown_tag); };
  EXPECT_THROW_FUNC(runtime_error, unknown);
  string cut("\x01\x00\x01\x00\x00\x00", 6);
  auto truncated = [&] { table.Load(cut); };
  EXPECT_THROW_FUNC(runtime_error, truncated);
}

/* A reader skips a section it doesn't know if it isn't required, and refuses the store if it is. */
FIXTURE(Sections) {
  Now = 10'000'000UL;
  TOperationTable table(MakeConfig());
  auto id = MakeId(Now - 1000UL);
  table.Reserve(Pov, id, 11);
  table.Complete(Pov, id, MakeOutcome(5, 10, true, 11));
  auto payload = table.Snapshot(15);
  /* Bump the section count to 2 and append a section of type 77 with 3 bytes. */
  string with_optional = payload;
  with_optional[2] = 2;
  with_optional += string("\x4d\x00\x00\x00\x03\x00\x00\x00" "abc", 11);
  TOperationTable loaded(MakeConfig());
  loaded.Load(with_optional);
  EXPECT_EQ(loaded.GetCount(), 1UL);
  EXPECT_FALSE(loaded.Reserve(Pov, id, 11).IsNew);
  string with_required = payload;
  with_required[2] = 2;
  with_required += string("\x4d\x00\x01\x00\x03\x00\x00\x00" "abc", 11);
  TOperationTable refusing(MakeConfig());
  auto load = [&] { refusing.Load(with_required); };
  EXPECT_THROW_FUNC(runtime_error, load);
}

/* A result round-trips through the bytes the table keeps. */
FIXTURE(ResultRoundTrip) {
  auto bytes = EncodeResult(Var::TVar(int64_t(1234)));
  EXPECT_TRUE(!bytes.empty());
  auto result = DecodeResult(bytes);
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto var = Var::ToVar(*Sabot::State::TAny::TWrapper(
      Indy::TKey(result.GetValue(), result.GetArena().get()).GetState(state_alloc)));
  EXPECT_TRUE(var.TryAs<Var::TInt>() != nullptr);
  EXPECT_EQ(var.TryAs<Var::TInt>()->GetVal(), int64_t(1234));
}

/* The hash ignores the reserved keys, and tells different requests apart. */
FIXTURE(Hash) {
  using TCall = TMetaRecord::TEntry::TCall;
  TCall plain{{"pkg"}, "put", {{"k", Var::TVar(string("a"))}}};
  TCall keyed = plain;
  keyed.ArgByName.insert({string(TMetaRecord::TEntry::OpIdKey), Var::TVar(string("x"))});
  keyed.ArgByName.insert({string(TMetaRecord::TEntry::OpOutcomeKey), Var::TVar(int64_t(1))});
  EXPECT_EQ(HashCalls({plain}), HashCalls({keyed}));
  TCall other_arg{{"pkg"}, "put", {{"k", Var::TVar(string("b"))}}};
  TCall other_method{{"pkg"}, "get", {{"k", Var::TVar(string("a"))}}};
  EXPECT_TRUE(HashCalls({plain}) != HashCalls({other_arg}));
  EXPECT_TRUE(HashCalls({plain}) != HashCalls({other_method}));
  EXPECT_TRUE(HashCalls({plain}) != HashCalls({plain, plain}));
}
