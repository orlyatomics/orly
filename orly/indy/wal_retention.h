/* <orly/indy/wal_retention.h>

   Decides how far the WAL's head may advance (#755): which records recovery still needs.

   After a restart only the root repos' data files come back (#173), so the WAL is the only copy
   of everything else a safe repo holds.  A record is needed while any of these is true:

     * it pushed an update into a live safe repo, and that update is not yet in a root's data file
       (either still in the repo it was pushed to, or promoted on up and not yet flushed there);
     * it is the Pov record of a live safe pov, which is how recovery rebuilds that pov.
     * it is the record that paused a live safe repo, which is the only place the pause is kept (#961).

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

   An update that sits unpromoted in a paused safe repo would hold the head for as long as the
   repo stays paused, so such a repo is copied forward too (#963): once its oldest pin is more than
   half the ring behind, all its unpromoted updates [a, b], and its pause, are appended again as
   RepoCopy records (TWalRepoCopyPart), and once they are durable its pins move to the copy.
   Recovery replays the newest complete copy of a repo in place of every record of the repo
   before it.  The room for the copy is kept the same way as the promotion room: every pin of a
   paused repo owes one more group of its promotion size (its copy room), which the copy spends,
   and which is owed again once a checkpoint has passed the records the copy replaced.  Together
   the paused repos may hold no more than half the ring (TWalPausedFull), so a copy always has
   somewhere to go.  A store gets the WalRepoCopy feature before its first copy is written, so a
   binary that would skip the copies refuses the store instead.

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
#include <unordered_set>
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

    /* One part of a copy of a paused safe repo (#963), the body of a RepoCopy record.  A copy is a
       set of parts, all with the same SetId, numbered 0 to Count - 1, appended in that order; it
       counts only if every part is there.  Each part carries some of the copy's transactions, as
       ~TTransaction would write each: one per update, pushing it at its own sequence number, and
       in the last part, one pausing the repo at Start.  Next is the sequence number the repo's
       next update takes. */
    struct TWalRepoCopyPart {
      static constexpr uint32_t Magic = 0x59504352U;
      static constexpr uint8_t Version = 1U;
      static constexpr uint8_t PausedFlag = 1U;
      Base::TUuid RepoId;
      Base::TUuid SetId;
      uint32_t Part = 0U;
      uint32_t Count = 0U;
      TSequenceNumber Start = 0UL;
      TSequenceNumber Limit = 0UL;
      TSequenceNumber Next = 0UL;
      bool Paused = true;
      std::vector<std::string> Wires;

      std::string Encode() const;

      /* Throws Disk::TWalCorruptError if it isn't one: a record the WAL's checksums passed, so a bug,
         and dropping it could lose updates. */
      static TWalRepoCopyPart Decode(const char *data, size_t size);
    };

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

      /* A pause or unpause a transaction committed to a safe repo (#961).  Seq is the repo's lowest
         unpromoted sequence number, as the record carries it; unset when the repo had none. */
      struct TStatusChange {
        Base::TUuid RepoId;
        bool Paused;
        std::optional<TSequenceNumber> Seq;
      };

      explicit TWalRetention(L1::TManager *manager);

      /* The promotion room (#959) the updates these pops would take out of their repos are owed: what
         a promotion popping them may spend (TWal::Reserve's credit).  Their copy room (#963) too. */
      size_t GetPromotionCredit(const std::vector<TPop> &pops);

      /* What TTransaction::Prepare reserves for a transaction, before its sequence numbers are
         known: the room it may spend (`Credit`: its pops', and the copy room of the repos it
         unpauses) and the room it will owe (`Owes`: its pushes' promotions, and copy room for
         those into a repo it leaves paused, and for the updates of the repos it pauses; #963).
         With `enforce_cap`, throws Disk::TWalPausedFull if it would take the room the paused repos
         hold past GetPausedCapBytes. */
      struct TPlannedPush {
        Base::TUuid RepoId;
        size_t Levels = 0UL;
      };
      struct TRoomPlan {
        size_t Credit = 0UL;
        std::vector<Disk::TPromotionDebt> Owes;
      };
      TRoomPlan PlanRoom(const std::vector<TPlannedPush> &pushes, const std::vector<TPop> &pops,
                         const std::vector<TStatusChange> &statuses, size_t wire_size, bool enforce_cap);

      /* How much of the ring the paused safe repos' unpromoted updates may hold for themselves: for
         each, its promotions and its copy (#963).  Half the ring, unless a test set it. */
      size_t GetPausedCapBytes();
      size_t GetPausedFootprintBytes();
      void SetPausedCapBytesForTest(size_t bytes);

      /* Called once, with no lock held, before the first RepoCopy record is written (#963): it gives
         the store the WalRepoCopy feature.  If it throws, nothing is copied.  None: copies are
         allowed (tests). */
      void SetIncompatGate(std::function<void ()> gate);

      /* Recovery (#963): which copies of paused repos the replay (Step 7 of server.cc's recovery)
         replays, and so which repos' mutations it skips until then.  A repo's newest complete copy
         set in `records` (all its parts there) rebuilds the repo where its first part is: every
         mutation of the repo in a Txn record before that is read past (the copy holds what they
         left), and the records after it are replayed as usual.  Throws Disk::TWalCorruptError on a
         RepoCopy record that isn't one. */
      struct TReplayRecord {
        uint64_t Lsn;
        Disk::TWalRecordType Type;
        const char *Data;
        size_t Size;
      };
      struct TReplayPlan {
        /* The chosen copies, by the LSN of their first part: the parts, in order. */
        std::map<uint64_t, std::vector<TWalRepoCopyPart>> Copies;
        /* The repos the chosen copies are of. */
        std::unordered_set<Base::TUuid> Skip;
      };
      static TReplayPlan PlanReplay(const std::vector<TReplayRecord> &records);

      /* Append a transaction's record and pin it for the updates it pushed and for the repos it
         paused (#961).  Called from ~TTransaction with the replication queue lock held, into the
         room the transaction reserved before it applied (#891) if `reservation` is given.  The pins
         of the updates it pops repay their promotion room, and those of the updates it pushes owe
         theirs; a pause pin owes none.  Throws what TWal::Append throws. */
      uint64_t AppendTxn(const std::string &wire, const std::vector<TPush> &pushes, const std::vector<TPop> &pops,
                         const std::vector<TStatusChange> &statuses, Disk::TWalReservation *reservation = nullptr);

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
      bool NoteReplayedTxn(const std::vector<TPush> &pushes, const std::vector<TPop> &pops, const std::vector<TStatusChange> &statuses);

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

      /* Copy old Pov records, paused safe repos (#963), and the records that paused live safe repos,
         forward if they are holding the ring, then write a WAL checkpoint at the head.  Skipped when
         the head hasn't moved, unless forced.  Throws on a WAL failure. */
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
        /* Whether it is owed its copy room too (#963): one more group of Debt.GroupBytes, while its
           repo is paused, but for between a copy spending it and a checkpoint passing the records
           the copy replaced. */
        bool CopyOwed = false;
      };

      struct TRepoPins {
        bool IsRoot = false;
        std::map<TSequenceNumber, TPin> Pins;
        /* What the pins hold of the ring for their promotions and their copy: GroupBytes for each
           level and one more, summed (#963). */
        size_t Footprint = 0UL;
        /* A copy moved the pins (#963): once a checkpoint's head passes this, the records it
           replaced are gone from the ring, and the copy room is owed again. */
        std::optional<uint64_t> ReoweAfter;
      };

      /* A RepoCopy set written, waiting to be durable before the pins move to it (#963).  While it
         waits the repo isn't copied again, so a copy that is slow to land isn't written over and
         over. */
      struct TPendingCopy {
        uint64_t FirstLsn;
        uint64_t LastLsn;
        std::map<TSequenceNumber, uint64_t> OldLsns;
        uint64_t OldPauseLsn;
        uint64_t ReoweAfter;
      };

      /* A Pov or pause record copied, waiting to be durable before its pin moves. */
      struct TPendingMove {
        uint64_t OldLsn;
        uint64_t NewLsn;
      };

      /* Requires Mutex.  What a transaction does to the room owed (#959, #963), as PinTxn will pin
         it.  Without `have_seqs` (Prepare), the pushes' sequence numbers are not known yet, so
         none is taken to replace a pin. */
      Disk::TWal::TPromotionDebtChange PlanTxnLocked(const std::vector<TPush> &pushes, const std::vector<TPop> &pops,
                                                     const std::vector<TStatusChange> &statuses, size_t debt_group_bytes,
                                                     bool have_seqs = true) const;

      /* Requires Mutex.  Add a pin; drop the pins in [begin, end), adding what they were owed to
         `repaid`. */
      void AddPinLocked(TRepoPins &repo, TSequenceNumber seq, const TPin &pin, std::vector<Disk::TPromotionDebt> *repaid);
      void ErasePinsLocked(TRepoPins &repo, std::map<TSequenceNumber, TPin>::iterator begin, std::map<TSequenceNumber, TPin>::iterator end,
                           std::vector<Disk::TPromotionDebt> *repaid);

      /* Requires Mutex.  The room paused repos hold for themselves (GetPausedFootprintBytes). */
      size_t GetPausedFootprintLocked() const;

      /* Requires Mutex.  The copy room a pin is owed. */
      static Disk::TPromotionDebt GetCopyDebt(const TPin &pin) {
        return Disk::TPromotionDebt{pin.Debt.GroupBytes, 1UL};
      }

      static size_t GetFootprint(const TPin &pin) {
        return pin.Debt.GroupBytes * (pin.Debt.Levels + 1UL);
      }

      struct TPovPin {
        uint64_t Lsn;
        std::string Body;
      };

      /* The record that paused a safe repo still paused (#961).  A pause leaves no other trace:
         the repo's Pov record carries no status, and a paused repo with nothing pending has no
         update pins either, so without this pin the head moved past the pause and recovery
         brought the repo back unpaused.  It is a pin on a record, not on an update, so it owes the
         WAL no promotion room and no pop or flush repays or drops it. */
      struct TStatusPin {
        uint64_t Lsn;
      };

      /* Requires Mutex.  Pins the pushes, each owed `debt_group_bytes` for each of its levels, and
         drops the pins the pops take; pins and unpins the repos `statuses` pause and unpause, and
         marks their pins' copy room owed or not.  The room owed changes as PlanTxnLocked says,
         which the caller sees to. */
      void PinTxn(uint64_t lsn, const std::vector<TPush> &pushes, const std::vector<TPop> &pops,
                  const std::vector<TStatusChange> &statuses, size_t debt_group_bytes);

      /* Requires Mutex.  Owe `wal` the debt of every pin, if it is not owed already. */
      void OweWalLocked(Disk::TWal *wal);

      /* Requires Mutex.  Repay the WAL these debts, if it is owed them yet. */
      void RepayLocked(std::vector<Disk::TPromotionDebt> repaid);

      /* Requires Mutex.  The head and the pin that sets it, ignoring liveness. */
      uint64_t ComputeHeadLocked(Disk::TWal *wal) const;

      /* Drop the pins of repos and povs that are no longer live.  Takes Mutex itself, but asks
         about liveness without it. */
      void DropDeadPins();

      /* Copy forward every Pov record more than half the ring behind; move its pin once durable. */
      void CopyPovsForward(Disk::TWal *wal);

      /* Requires Mutex.  The repo's lowest unpromoted sequence number, from its update pins: a
         paused repo is never a root, so every update it holds is pinned until it is popped. */
      std::optional<TSequenceNumber> LowestPinnedSeqLocked(const Base::TUuid &repo_id) const;
      /* Write a fresh pause record for every paused safe repo whose pause is more than half the
         ring behind, and move its pin there once durable (#961).  The record is an ordinary
         transaction record holding one pause, as the pause itself wrote, so recovery replays it
         as one. */
      void CopyPausesForward(Disk::TWal *wal);

      /* Copy forward every paused safe repo whose oldest pin, or pause, is more than half the ring
         behind (#963), and move its pins once the copy is durable.  If the ring has refused a
         record since the last pass, also every one holding the head below anything else's pins:
         with the promotion room and the headroom kept, the ring can be too full to take a big
         record before a paused repo is half the ring behind, and then nothing would carry it
         there. */
      void CopyPausedReposForward(Disk::TWal *wal);

      /* Copy one; false if it wasn't (no room, or the repo isn't as its pins say). */
      bool CopyRepoForward(Disk::TWal *wal, const Base::TUuid &repo_id);

      /* Requires Mutex.  Move the pins a durable copy replaced to it. */
      void MoveCopiedPinsLocked(const Base::TUuid &repo_id, const TPendingCopy &copy);

      /* Requires Mutex.  Owe again the copy room of the paused repos whose replaced records the
         checkpoint at `head` has passed. */
      void ReoweCopyRoomLocked(uint64_t head);

      L1::TManager *const Manager;

      /* Covers everything below.  Taken inside the replication queue lock and the TWal's own
         lock is taken inside it, never the other way round. */
      std::mutex Mutex;

      std::unordered_map<Base::TUuid, TRepoPins> Repos;

      std::unordered_map<Base::TUuid, TPovPin> Povs;
      std::unordered_map<Base::TUuid, TStatusPin> Paused;

      std::unordered_map<Base::TUuid, TPendingCopy> PendingCopies;
      std::unordered_map<Base::TUuid, TPendingMove> PendingPovMoves;
      std::unordered_map<Base::TUuid, TPendingMove> PendingPauseMoves;

      /* See SetIncompatGate.  RepoCopyAllowed: the gate has run. */
      std::function<void ()> IncompatGate;
      bool RepoCopyAllowed = false;

      /* TWal::GetRingFullRefusals as the last CopyPausedReposForward saw it. */
      size_t SeenRingFullRefusals = 0UL;

      std::optional<size_t> PausedCapBytesForTest;

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
