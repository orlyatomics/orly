/* <orly/indy/wal_retention.h>

   Decides how far the WAL's head may advance (#755): which records recovery still needs.

   After a restart only the root repos' data files come back (#173), so the WAL is the only copy
   of everything else a safe repo holds.  A record is needed while any of these is true:

     * it pushed an update into a live safe repo, and that update is not yet in a root's data file
       (either still in the repo it was pushed to, or promoted on up and not yet flushed there);
     * it is the Pov record of a live safe pov, which is how recovery rebuilds that pov.

   Each such fact is a pin on an LSN, and the head is the lowest pin.  Pins are taken under the
   same lock as the append that writes the record, so the head can never pass a record that is
   needed but not yet pinned.

   A long-lived safe pov would otherwise hold the head at its creation record forever, so once its
   Pov record is more than half the ring behind the write position it is copied forward: appended
   again (flagged Copied), and the pin moves to the copy once the copy is durable.

   An update in a safe child reaches a root only by being promoted, a record for each level, and
   until then it holds the head.  So the ring keeps room for those promotions (#959): each pin on an
   unpromoted update carries the room its promotions are owed (TWal::Reserve), which its promotion
   spends and repays.  Without it, other records could fill the ring up to the head, and then the
   promotion that would move the head could never get room.

   Not handled: an update that sits unpromoted in a safe child (a paused pov) still holds the head,
   and TWal never overwrites a record past the checkpointed head, so once the ring is full up to
   it, writes are refused (Disk::TWalRingFull, retriable) until the pov is unpaused or goes.  They
   are refused rather than left waiting (#891): a record the WAL took but couldn't write would hold
   up the root flush that has to wait for it, and so the checkpoint that would make room.

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

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <orly/indy/disk/wal.h>
#include <orly/indy/sequence_number.h>

namespace Orly {

  namespace Indy {

    namespace L1 {
      class TManager;
    }

    class TWalRetention {
      NO_COPY(TWalRetention);
      public:

      /* An update a transaction pushed into a safe repo.  IsRoot: the repo has no parent, so a data
         file of its own is where the update ends up.  Levels: the promotions that will write a
         record on its way to a root (#959); 0 for a root. */
      struct TPush {
        Base::TUuid RepoId;
        TSequenceNumber Seq;
        bool IsRoot;
        size_t Levels = 0UL;
      };

      /* An update a transaction popped out of a safe repo (to promote it). */
      struct TPop {
        Base::TUuid RepoId;
        TSequenceNumber Seq;
      };

      explicit TWalRetention(L1::TManager *manager);

      /* The promotion room (#959) the updates these pops would take out of their repos are owed: what
         a promotion popping them may spend (TWal::Reserve's credit). */
      size_t GetPromotionCredit(const std::vector<TPop> &pops);

      /* Append a transaction's record and pin it for the updates it pushed.  Called from
         ~TTransaction with the replication queue lock held, into the room the transaction reserved
         before it applied (#891) if `reservation` is given.  The pins of the updates it pops repay
         their promotion room, and those of the updates it pushes owe theirs.  Throws what
         TWal::Append throws. */
      uint64_t AppendTxn(const std::string &wire, const std::vector<TPush> &pushes, const std::vector<TPop> &pops, Disk::TWalReservation *reservation = nullptr);

      /* Append a safe pov's Pov record and pin it for as long as the pov is live.  Throws
         Disk::TWalRingFull if the ring has no room for it (#891); with keep_headroom (a pov a
         client asked for), if it would need the headroom TWal::TRoom::KeepHeadroom leaves. */
      uint64_t AppendPov(const Base::TUuid &pov_id, const std::string &body, bool keep_headroom = false);

      /* Recovery.  Between BeginReplay and EndReplay, the transactions recovery replays (which
         have no WAL to append to) are pinned at the LSN of the record being replayed. */
      void BeginReplay();
      void SetReplayLsn(uint64_t lsn, size_t record_bytes);
      void EndReplay();

      /* Recovery made the WAL: owe it the promotion room of the pins the replay left (#959).  Pins
         taken before this (by promotions that ran first) are owed too, once. */
      void NoteWalAttached();

      /* Pin a replayed transaction at the current replay LSN.  False if not replaying. */
      bool NoteReplayedTxn(const std::vector<TPush> &pushes, const std::vector<TPop> &pops);

      /* Pin the Pov record recovery restored a safe pov from. */
      void NoteRestoredPov(const Base::TUuid &pov_id, uint64_t lsn, const std::string &body);

      /* The write-ahead rule: before a root repo writes a data file holding its updates up to
         `high_seq`, wait until every record that pushed one of them is durable. */
      void WaitWriteAhead(const Base::TUuid &root_id, TSequenceNumber high_seq);

      /* A root repo's data file holding its updates [low_seq, high_seq] is now durable: release
         those pins, and wake the checkpointer. */
      void OnRootFlushed(const Base::TUuid &root_id, TSequenceNumber low_seq, TSequenceNumber high_seq);

      /* The lowest LSN recovery still needs.  Drops the pins of repos that have gone. */
      uint64_t ComputeHead();

      /* Copy old Pov records forward if they are holding the ring, then write a WAL checkpoint at
         the head.  Skipped when the head hasn't moved, unless forced.  Throws on a WAL failure. */
      void Checkpoint(uint64_t global_flushed_seq, bool force = false);

      /* What a checkpoint carries as its payload, given the head it is taken at (#733): the state
         that the records before the head held and recovery will no longer replay. Set once, before
         the first Checkpoint. */
      using TPayloadProvider = std::function<std::string (uint64_t head_lsn)>;
      void SetCheckpointPayload(TPayloadProvider provider);

      /* A ceiling on the head a checkpoint is taken at, asked for each checkpoint: the head is the
         lower of the pins and this. For state the pins don't cover, which the checkpoint payload
         won't hold either (#861). Set once, before the first Checkpoint. */
      void SetHeadFloor(std::function<uint64_t ()> floor);

      /* For the checkpointer thread: wait up to `timeout` for a flush to make a checkpoint worth
         taking.  False once Stop() is called. */
      bool WaitForWork(std::chrono::milliseconds timeout);

      void Stop();

      private:

      struct TPin {
        /* The lowest record recovery needs to rebuild this update. */
        uint64_t Lsn;
        /* The record that pushed it here, which must be durable before a data file holds it. */
        uint64_t TxnLsn;
        /* The promotion room it is owed (#959). */
        Disk::TPromotionDebt Debt;
      };

      struct TRepoPins {
        bool IsRoot = false;
        std::map<TSequenceNumber, TPin> Pins;
      };

      struct TPovPin {
        uint64_t Lsn;
        std::string Body;
      };

      /* Requires Mutex.  The debts of the pins these pops take. */
      std::vector<Disk::TPromotionDebt> GetPopDebtsLocked(const std::vector<TPop> &pops) const;

      /* Requires Mutex.  Pins the pushes, each owed `debt_group_bytes` for each of its levels, and
         drops the pins the pops take.  Returns the debts of the pins it dropped: the pops' first,
         as GetPopDebtsLocked lists them, then any a push replaced. */
      std::vector<Disk::TPromotionDebt> PinTxn(uint64_t lsn, const std::vector<TPush> &pushes, const std::vector<TPop> &pops, size_t debt_group_bytes);

      /* Requires Mutex.  Owe `wal` the debt of every pin, if it is not owed already. */
      void OweWalLocked(Disk::TWal *wal);

      /* Requires Mutex.  Repay the WAL the debts of pins dropped, but for the first `already_repaid`,
         which the record that dropped them repaid. */
      void RepayDroppedLocked(std::vector<Disk::TPromotionDebt> dropped, size_t already_repaid);

      /* Requires Mutex.  The head and the pin that sets it, ignoring liveness. */
      uint64_t ComputeHeadLocked(Disk::TWal *wal) const;

      /* Drop the pins of repos and povs that are no longer live.  Takes Mutex itself, but asks
         about liveness without it. */
      void DropDeadPins();

      /* Copy forward every Pov record more than half the ring behind; move its pin once durable. */
      void CopyPovsForward(Disk::TWal *wal);

      L1::TManager *const Manager;

      /* Covers everything below.  Taken inside the replication queue lock and the TWal's own
         lock is taken inside it, never the other way round. */
      std::mutex Mutex;

      std::unordered_map<Base::TUuid, TRepoPins> Repos;

      std::unordered_map<Base::TUuid, TPovPin> Povs;

      bool Replaying = false;

      uint64_t ReplayLsn = 0UL;

      /* The size of the record being replayed, for its pushes' promotion room. */
      size_t ReplayRecordBytes = 0UL;

      /* The WAL the pins' promotion debts are owed to; null until NoteWalAttached. */
      Disk::TWal *DebtWal = nullptr;

      /* Serialises Checkpoint(). */
      std::mutex CheckpointMutex;

      std::function<uint64_t ()> HeadFloor;

      std::optional<uint64_t> LastCheckpointHead;

      TPayloadProvider PayloadProvider;

      /* For WaitForWork(). */
      std::mutex WorkMutex;
      std::condition_variable WorkCv;
      bool WorkPending = false;
      bool Stopping = false;

    };  // TWalRetention

  }  // Indy

}  // Orly
