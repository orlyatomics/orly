/* <orly/indy/replication.test.cc>

   Unit test for <orly/indy/replication.h>: a durable save's ttl survives the replication
   stream (#676 follow-up).

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

#include <orly/indy/replication.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <base/uuid.h>
#include <orly/indy/disk/buf_block.h>
#include <orly/indy/memory_layer.h>
#include <orly/indy/transaction_base.h>
#include <base/io/binary_input_only_stream.h>
#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Orly;
using namespace Orly::Indy;

/* The object pools are process-global statics each binary defines for itself; these are the ones
   the replication code links against.  None is exercised here. */
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(L0::TManager::TRepo::TMapping), "Repo Mapping", 10UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(L0::TManager::TRepo::TMapping::TEntry), "Repo Mapping Entry", 10UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 10UL);
Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(max(max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation", 10UL);
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction", 10UL);
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(Disk::Util::PhysicalBlockSize, 10UL);
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 10UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 10UL);

/* What a master sends for each durable save -- id, ttl, serialized form -- must be what the slave
   reads back.  The ttl was pushed as a bare seconds count and did not come back as the ttl sent,
   so the slave saved each replicated pov with a garbage deadline, and its durable layer dropped
   the record as expired at the next write or merge whenever that deadline fell below now.  After
   a failover such a pov failed with "durable object doesn't exist". */
FIXTURE(DurableTtlSurvivesTheStream) {
  struct TSave {
    Base::TUuid Id;
    TTtl Ttl;
    string Obj;
  };
  const vector<TSave> sent = {
    { Base::TUuid(Base::TUuid::Twister), seconds(600), "a pov" },
    { Base::TUuid(Base::TUuid::Twister), seconds(1), "a short one" },
    { Base::TUuid(Base::TUuid::Twister), seconds(86400), "a long one" },
    /* Never expires: too long for nanoseconds, so it arrives as the most they can count. */
    { Base::TUuid(Base::TUuid::Twister), TTtl::max(), "a forever one" },
  };
  string wire;
  /* master side */ {
    TReplicationStreamer streamer;
    for (const auto &save : sent) {
      TDurableReplication replication(save.Id, save.Ttl, save.Obj);
      streamer.PushDurable(replication);
    }
    auto recorder = make_shared<Io::TRecorder>();
    /* extra */ {
      Io::TBinaryOutputOnlyStream strm(recorder);
      streamer.Write(strm);
      strm.Flush();
    }
    recorder->CopyOut(wire);
  }
  /* slave side */
  TReplicationStreamer received;
  Io::TBinaryInputOnlyStream strm(make_shared<Io::TPlayer>(make_shared<Io::TRecorder>(wire)));
  received.Read(strm);
  vector<TSave> got;
  received.ForEachDurable([&got](const Base::TUuid &id, const TTtl &ttl, const string &obj) {
    got.push_back(TSave{ id, ttl, obj });
  });
  EXPECT_EQ(got.size(), sent.size());
  for (size_t i = 0; i < got.size() && i < sent.size(); ++i) {
    EXPECT_TRUE(got[i].Id == sent[i].Id);
    EXPECT_EQ(got[i].Ttl.count(), min(sent[i].Ttl, duration_cast<TTtl>(nanoseconds::max())).count());
    EXPECT_EQ(got[i].Obj, sent[i].Obj);
  }
}

namespace {

  /* What a transaction stream decodes to, core by core.  The keys refer to the stream's arena,
     so it is kept with them. */
  struct TDecoded {
    unique_ptr<TReplicationStreamer> Received = make_unique<TReplicationStreamer>();
    vector<TKey> Keys;
  };

  TDecoded DecodeTransactions(const string &wire) {
    TDecoded decoded;
    Io::TBinaryInputOnlyStream strm(make_shared<Io::TPlayer>(make_shared<Io::TRecorder>(wire)));
    decoded.Received->Read(strm);
    for (const auto &core : decoded.Received->GetTransactionVec().GetCores()) {
      decoded.Keys.emplace_back(core, decoded.Received->GetTransactionVec().GetArena());
    }
    return decoded;
  }

  string WriteStream(const TReplicationStreamer &streamer) {
    auto recorder = make_shared<Io::TRecorder>();
    /* extra */ {
      Io::TBinaryOutputOnlyStream strm(recorder);
      streamer.Write(strm);
      strm.Flush();
    }
    string wire;
    recorder->CopyOut(wire);
    return wire;
  }

}  // namespace

/* A transaction's WAL record is built before it applies, to size its room, and written after,
   once its mutations have their sequence numbers.  Filling those in must give the record that
   building it afresh would, and the size it was built at must be the size it writes. */
FIXTURE(TransactionSequenceNumbersFilledInLater) {
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const Base::TUuid idx_id(Base::TUuid::Twister), repo_a(Base::TUuid::Twister), repo_b(Base::TUuid::Twister);
  auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{
      { TIndexKey(idx_id, TKey(make_tuple(string("a key long enough not to be held in a core"), int64_t(1)), &arena, state_alloc)), TKey(string("a value long enough not to be held in a core"), &arena, state_alloc) },
      { TIndexKey(idx_id, TKey(make_tuple(string("b"), int64_t(2)), &arena, state_alloc)), TKey(int64_t(7), &arena, state_alloc) },
    }, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));
  L1::TTransaction::TReplica replica;
  auto *push = replica.Push(repo_a, update.get());
  auto *pop = replica.Pop(repo_b, 5UL);
  auto *pause = replica.Pause(repo_a, nullopt);
  auto *unpause = replica.UnPause(repo_b, 9UL);
  auto *fail = replica.Fail(repo_a, 11UL);
  TReplicationStreamer before;
  before.PushTransaction(replica, true);
  const size_t size_before = before.GetWriteSize();
  /* As the mutations apply. */
  push->SetSequenceNumber(42UL);
  pop->SetSequenceNumber(6UL);
  pause->SetSequenceNumber(nullopt);
  unpause->SetSequenceNumber(1234567890123UL);
  fail->SetSequenceNumber(12UL);
  EXPECT_TRUE(before.UpdateSequenceNumbers(replica));
  const string filled = WriteStream(before);
  TReplicationStreamer afresh;
  afresh.PushTransaction(replica);
  const string fresh = WriteStream(afresh);
  EXPECT_EQ(filled.size(), size_before);
  EXPECT_EQ(fresh.size(), afresh.GetWriteSize());
  EXPECT_EQ(filled.size(), fresh.size());
  /* Notes are written in the order of a set of their addresses, so two builds of one transaction
     need not match byte for byte once there are notes; what they hold must. */
  const auto filled_dec = DecodeTransactions(filled), fresh_dec = DecodeTransactions(fresh);
  const auto &filled_keys = filled_dec.Keys, &fresh_keys = fresh_dec.Keys;
  EXPECT_EQ(filled_keys.size(), fresh_keys.size());
  for (size_t i = 0; i < filled_keys.size() && i < fresh_keys.size(); ++i) {
    EXPECT_TRUE(filled_keys[i] == fresh_keys[i]);
  }
  /* Filling in changes the sequence numbers and nothing else: one changed after the push
     changes one core. */
  TReplicationStreamer unfilled;
  unfilled.PushTransaction(replica, true);
  replica.GetMutationList().front().SetSequenceNumber(43UL);
  const auto changed_dec = DecodeTransactions(WriteStream(unfilled));
  const auto &changed_keys = changed_dec.Keys;
  size_t num_differ = 0UL;
  for (size_t i = 0; i < changed_keys.size() && i < fresh_keys.size(); ++i) {
    num_differ += !(changed_keys[i] == fresh_keys[i]);
  }
  EXPECT_EQ(num_differ, 0UL);
  EXPECT_TRUE(unfilled.UpdateSequenceNumbers(replica));
  const auto refilled_dec = DecodeTransactions(WriteStream(unfilled));
  const auto &refilled_keys = refilled_dec.Keys;
  num_differ = 0UL;
  for (size_t i = 0; i < refilled_keys.size() && i < fresh_keys.size(); ++i) {
    num_differ += !(refilled_keys[i] == fresh_keys[i]);
  }
  EXPECT_EQ(num_differ, 1UL);
  const string refilled = WriteStream(unfilled);
  /* A replica of another shape is refused, and so is a second transaction. */
  L1::TTransaction::TReplica other;
  other.Pop(repo_b, 1UL);
  EXPECT_FALSE(unfilled.UpdateSequenceNumbers(other));
  /* A refusal changes nothing. */
  EXPECT_TRUE(WriteStream(unfilled) == refilled);
  unfilled.PushTransaction(other);
  EXPECT_FALSE(unfilled.UpdateSequenceNumbers(replica));
}

FIXTURE(TransactionSequenceNumbersFilledInLaterBytes) {
  /* One key, and every other value sits in its core, so there is at most one note at each depth
     and the stream's bytes are fixed: filling in must give exactly the bytes of a stream built
     after the fact, for every kind of mutation. */
  Atom::TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  const Base::TUuid idx_id(Base::TUuid::Twister), repo_a(Base::TUuid::Twister), repo_b(Base::TUuid::Twister);
  auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{
      { TIndexKey(idx_id, TKey(make_tuple(int64_t(1)), &arena, state_alloc)), TKey(int64_t(7), &arena, state_alloc) },
    }, TKey(&arena), TKey(Base::TUuid(Base::TUuid::Best), &arena, state_alloc));
  L1::TTransaction::TReplica replica;
  auto *push = replica.Push(repo_a, update.get());
  auto *pop = replica.Pop(repo_b, 5UL);
  auto *pause = replica.Pause(repo_a, nullopt);
  auto *unpause = replica.UnPause(repo_b, 9UL);
  auto *fail = replica.Fail(repo_a, 11UL);
  TReplicationStreamer before;
  before.PushTransaction(replica, true);
  const string unfilled = WriteStream(before);
  push->SetSequenceNumber(42UL);
  pop->SetSequenceNumber(6UL);
  pause->SetSequenceNumber(3UL);
  unpause->SetSequenceNumber(nullopt);
  fail->SetSequenceNumber(1234567890123UL);
  EXPECT_TRUE(before.UpdateSequenceNumbers(replica));
  const string filled = WriteStream(before);
  TReplicationStreamer afresh;
  afresh.PushTransaction(replica);
  const string fresh = WriteStream(afresh);
  EXPECT_EQ(filled.size(), before.GetWriteSize());
  EXPECT_EQ(fresh.size(), afresh.GetWriteSize());
  EXPECT_TRUE(filled == fresh);
  EXPECT_FALSE(filled == unfilled);
  /* Filling in again with the same numbers changes nothing. */
  EXPECT_TRUE(before.UpdateSequenceNumbers(replica));
  EXPECT_TRUE(WriteStream(before) == fresh);
}
