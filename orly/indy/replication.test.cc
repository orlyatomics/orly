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
    EXPECT_EQ(got[i].Ttl.count(), sent[i].Ttl.count());
    EXPECT_EQ(got[i].Obj, sent[i].Obj);
  }
}
