/* <orly/indy/disk/wal.h>

   Write-ahead log (WAL) library for durable acknowledgments (#755 Stage 1).

   A ring on a block range on a storage device (Disk::Util::TDevice) with
   pipelined group commit, dedicated OS leader and syncer threads, group
   headers, hash chaining, piggybacked seals, checkpoints, copy-forward,
   and crash-recovery classification.

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

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <base/class_traits.h>
#include <base/mem_aligned_ptr.h>
#include <base/murmur.h>
#include <base/uuid.h>
#include <orly/indy/disk/priority.h>
#include <orly/indy/disk/result.h>
#include <orly/indy/disk/util/volume_manager.h>

namespace Orly {

  namespace Indy {

    namespace Disk {

      /* Exceptions */
      class TWalIoError : public std::runtime_error {
        public:
        explicit TWalIoError(const std::string &msg) : std::runtime_error(msg) {}
      };

      class TWalCorruptError : public std::runtime_error {
        public:
        explicit TWalCorruptError(const std::string &msg) : std::runtime_error(msg) {}
      };

      /* The ring has no room it can promise a record (#891).  Nothing was appended, and retrying
         once a checkpoint has moved the head on may succeed. */
      class TWalRingFull : public std::runtime_error {
        public:
        explicit TWalRingFull(const std::string &msg) : std::runtime_error(msg) {}
      };

      /* A client write into a paused safe repo (or a pause) refused because the paused repos'
         unpromoted updates already hold as much of the ring as they may (#963): each must be
         copied forward while its originals are still in the ring, so together they can't be let
         take more than about half of it.  Retriable, as TWalRingFull: unpausing or discarding a
         paused repo, or promoting it, makes room.  Only writes into paused repos get it. */
      class TWalPausedFull : public TWalRingFull {
        public:
        explicit TWalPausedFull(const std::string &msg) : TWalRingFull(msg) {}
      };

      /* A record a write keeping headroom (TWal::TRoom::KeepHeadroom) asked for is too big ever to be
         safe to take (#891): its promotion, a record about as big, has only the headroom to be
         written into once client writes have filled the rest, so a record bigger than half the
         headroom could be accepted and then never promoted.  Not retryable: split the write. */
      class TWalRecordTooLarge : public std::runtime_error {
        public:
        explicit TWalRecordTooLarge(const std::string &msg) : std::runtime_error(msg) {}
      };

      /* 4 KiB group / page alignment for Direct I/O and log groups */
      static constexpr size_t WalAlignment = 4096UL;

      /* CRC32C (Castagnoli) checksum helper */
      uint32_t ComputeCrc32c(const void *data, size_t length, uint32_t initial = 0U);

      /* 64-bit checksum helper based on Base::Murmur */
      uint64_t ComputeChecksum64(const void *data, size_t length, uint64_t seed = 0UL);

      /* Constants */
      static constexpr uint64_t WalMagic = 0x314c4157594c524fULL; // "ORLYWAL1"
      static constexpr uint16_t WalFormatVersion = 1U;
      static constexpr uint64_t WalCheckpointMagic = 0x314b4348594c524fULL; // "ORLYCHK1"

      enum class TWalRecordType : uint8_t {
        Txn = 1U,
        Pov = 2U,
        Seal = 3U,
        Meta = 4U,
        /* One part of a copy of a paused safe repo's unpromoted updates (#963; TWalRepoCopy).  A
           binary from before it would skip it, so a store gets the WalRepoCopy feature
           (Util::TStoreFormat) before its first one is written, and such a binary refuses the
           store. */
        RepoCopy = 5U
      };

      namespace WalFlags {
        static constexpr uint16_t SealOnly = 0x0001U;
        static constexpr uint16_t CheckpointMarker = 0x0002U;
      }

      namespace WalRecordFlags {
        static constexpr uint8_t Copied = 0x0001U;
      }

      #pragma pack(push, 1)
      /* Group header (88 bytes, packed) */
      struct TGroupHeader {
        char Magic[8];             // "ORLYWAL1"
        uint16_t FormatVersion;    // 1
        uint16_t Flags;            // WalFlags
        Base::TUuid StoreId;       // 16 bytes
        uint32_t Lap;              // Incremented each time ring wraps
        uint64_t GroupNum;         // Consecutive across laps
        uint64_t FirstLsn;         // First LSN in this group (0 for seal-only)
        uint32_t RecordCount;      // Number of records
        uint32_t PayloadLen;       // Byte length of records before 4 KiB padding
        uint64_t SealedThrough;    // Records with LSN <= this are guaranteed synced before this write
        uint64_t Chain;            // Previous group's HeaderChecksum
        uint64_t PayloadChecksum;  // 64-bit checksum of payload records
        uint64_t HeaderChecksum;   // 64-bit checksum of preceding 80 bytes of header
      };

      static_assert(sizeof(TGroupHeader) == 88, "TGroupHeader size must be 88 bytes");
      static constexpr size_t GroupHeaderChecksumCoveredBytes = offsetof(TGroupHeader, HeaderChecksum);
      static_assert(GroupHeaderChecksumCoveredBytes == 80, "TGroupHeader checksum covers 80 bytes");

      /* Per-record header (20 bytes, packed) */
      struct TRecordHeader {
        uint32_t Length;           // Total length: sizeof(TRecordHeader) + body size
        uint8_t Type;              // TWalRecordType
        uint8_t Flags;             // WalRecordFlags
        uint16_t Reserved;         // 0
        uint64_t Lsn;              // Assigned LSN
        uint32_t BodyCrc32c;       // CRC32C of record body
      };

      static_assert(sizeof(TRecordHeader) == 20, "TRecordHeader size must be 20 bytes");

      /* Alternating checkpoint header (packed) */
      struct TCheckpointHeader {
        char Magic[8];             // "ORLYCHK1"
        uint16_t FormatVersion;    // 1
        uint16_t Reserved;         // 0
        Base::TUuid StoreId;       // 16 bytes
        uint64_t CheckpointNum;    // Monotonically increasing counter
        uint64_t HeadLsn;          // Oldest unretired LSN
        uint64_t DurableLsn;       // Highest durable LSN
        uint64_t GlobalFlushedSeq; // Highest flushed sequence of global POV
        uint64_t HeadGroupNum;     // Group number of oldest unretired group
        uint32_t HeadLap;          // Lap of oldest unretired group
        uint32_t Reserved2;        // 0
        uint64_t HeadRingOffset;   // Byte ring offset of oldest unretired group
        uint64_t HeadChain;        // Checksum of group preceding the head group
        uint32_t PayloadLen;       // Additional checkpoint payload bytes
        uint32_t Reserved3;        // 0
        uint64_t PayloadChecksum;  // Checksum of payload bytes
        uint64_t HeaderChecksum;   // Checksum of preceding covered bytes
      };

      static_assert(sizeof(TCheckpointHeader) == 116, "TCheckpointHeader size must be 116 bytes");
      static constexpr size_t CheckpointHeaderChecksumCoveredBytes = offsetof(TCheckpointHeader, HeaderChecksum);
      static_assert(CheckpointHeaderChecksumCoveredBytes == 108, "TCheckpointHeader checksum covers 108 bytes");
      #pragma pack(pop)

      /* Checkpoint data snapshot */
      struct TCheckpoint {
        uint64_t CheckpointNum = 0UL;
        uint64_t HeadLsn = 0UL;
        uint64_t DurableLsn = 0UL;
        uint64_t GlobalFlushedSeq = 0UL;
        uint64_t HeadGroupNum = 0UL;
        uint32_t HeadLap = 1U;
        uint64_t HeadRingOffset = 0UL;
        uint64_t HeadChain = 0UL;
        std::vector<char> Payload;
      };

      /* Scanned record from recovery */
      struct TScannedRecord {
        uint64_t Lsn = 0UL;
        TWalRecordType Type = TWalRecordType::Txn;
        uint8_t Flags = 0U;
        std::vector<char> Body;
      };

      /* Recovery classification (§5.3 & §6) */
      enum class TScanStatus {
        Clean,                // Clean stop at end of log
        TornTail,             // Unsealed or never-synced tail (safe to truncate)
        DamagedAcknowledged,  // Invalid group that was sealed by a later group (corruption!)
        Corrupt,              // Structural or header corruption
        Empty                 // Log is empty
      };

      struct TGroupLocation {
        uint64_t GroupNum = 0UL;
        uint32_t Lap = 1U;
        uint64_t Offset = 0UL;
        uint64_t Chain = 0UL;
        uint64_t FirstLsn = 0UL;
        uint64_t LastLsn = 0UL;
      };

      struct TScanResult {
        TScanStatus Status = TScanStatus::Empty;
        uint64_t LastValidLsn = 0UL;
        uint64_t LastValidGroupNum = 0UL;
        uint32_t LastValidLap = 1U;
        uint64_t NextRingOffset = 0UL;
        uint64_t LastGroupChecksum = 0UL;
        uint64_t StartGroupNum = 1UL;
        uint64_t StoppedAtGroupNum = 0UL;
        uint64_t DamagedLsnStart = 0UL;
        uint64_t DamagedLsnEnd = 0UL;
        std::string ProblemDescription;
        std::vector<TScannedRecord> Records;
        std::vector<TGroupLocation> GroupLocations;

        TGroupLocation FindGroupForLsn(uint64_t lsn) const {
          for (const auto &loc : GroupLocations) {
            if (loc.LastLsn >= lsn) {
              return loc;
            }
          }
          TGroupLocation tail;
          tail.GroupNum = (LastValidGroupNum > 0) ? (LastValidGroupNum + 1UL) : StartGroupNum;
          tail.Lap = LastValidLap;
          tail.Offset = NextRingOffset;
          tail.Chain = LastGroupChecksum;
          tail.FirstLsn = (LastValidLsn > 0) ? (LastValidLsn + 1UL) : 1UL;
          tail.LastLsn = tail.FirstLsn;
          return tail;
        }
      };

      inline std::ostream &operator<<(std::ostream &strm, TWalRecordType type) {
        switch (type) {
          case TWalRecordType::Txn:  return strm << "Txn";
          case TWalRecordType::Pov:  return strm << "Pov";
          case TWalRecordType::Seal: return strm << "Seal";
          case TWalRecordType::Meta: return strm << "Meta";
        }
        return strm << static_cast<int>(type);
      }

      inline std::ostream &operator<<(std::ostream &strm, TScanStatus status) {
        switch (status) {
          case TScanStatus::Clean: return strm << "Clean";
          case TScanStatus::TornTail: return strm << "TornTail";
          case TScanStatus::DamagedAcknowledged: return strm << "DamagedAcknowledged";
          case TScanStatus::Corrupt: return strm << "Corrupt";
          case TScanStatus::Empty: return strm << "Empty";
        }
        return strm << static_cast<int>(status);
      }

      class TWal;

      /* Room the ring owes the promotions an update has yet to go through (#959): one group of
         GroupBytes (TWal::GetPromotionGroupBytes) for each of Levels promotions, up to the root. */
      struct TPromotionDebt {
        size_t GroupBytes = 0UL;
        size_t Levels = 0UL;
      };

      /* Room a TWal holds for one record that is not appended yet (#891): see TWal::Reserve.  Empty
         when default-constructed or once used or released.  Releases its room when destroyed, so it
         must not outlive its TWal. */
      class TWalReservation {
        NO_COPY(TWalReservation);
        public:

        TWalReservation() = default;

        TWalReservation(TWalReservation &&that) noexcept;

        TWalReservation &operator=(TWalReservation &&that) noexcept;

        ~TWalReservation() { Release(); }

        explicit operator bool() const { return Wal != nullptr; }

        /* The body size the room was reserved for. */
        size_t GetSize() const { return Size; }

        /* Give the room back without appending. */
        void Release();

        private:

        friend class TWal;

        TWal *Wal = nullptr;

        size_t Size = 0UL;

        /* What the room was charged as: see TWal::GetRecordGroupBytes. */
        size_t GroupBytes = 0UL;

        /* The promotion room the record will leave owed (#959), held since Reserve. */
        std::vector<TPromotionDebt> Owes;

      };  // TWalReservation

      /* The Write-Ahead Log class */
      class TWal {
        NO_COPY(TWal);
        public:

        struct TConfig {
          Util::TOffset BaseOffset = 0UL;
          size_t CapacityBytes = 1024UL * 1024UL; // 1 MiB ring
          Util::TOffset CheckpointSlot0Offset = 0UL;
          Util::TOffset CheckpointSlot1Offset = 4096UL;
          /* The length of each checkpoint slot in bytes, which bounds header plus payload.  Zero means
             the slots are not bounded (tests that lay them out by hand). */
          size_t CheckpointSlotBytes = 0UL;
          Base::TUuid StoreId = Base::TUuid::Null;
          size_t MaxInFlightGroups = 2UL;
          bool AutoSealOnQuiet = true;
          std::chrono::microseconds QuietInterval = std::chrono::microseconds(500);
          bool NoSync = false;       // Test-only negative control: skip device sync while acknowledging (#755)
          bool EarlyAck = false;     // Test-only negative control: acknowledge before group sync (#755)
        };

        /* The head arguments name the oldest record recovery still needs and where its group starts,
           as the newest checkpoint recorded them; retained_groups are the groups a recovery scan found
           from there on (TScanResult::GroupLocations), so a later checkpoint can name their positions
           and the leader won't overwrite them. */
        TWal(Util::TDevice *device, const TConfig &config, uint64_t start_lsn = 1UL, uint64_t start_group_num = 1UL, uint32_t start_lap = 1U, uint64_t start_ring_offset = 0UL, uint64_t previous_checksum = 0UL, uint64_t head_lsn = 0UL, uint64_t head_group_num = 0UL, uint32_t head_lap = 0U, uint64_t head_ring_offset = 0UL, uint64_t head_chain = 0UL,
             const std::vector<TGroupLocation> &retained_groups = std::vector<TGroupLocation>());
        ~TWal();

        /* Which part of the ring a reservation may use.  Writes a client asked for leave the last
           eighth of the ring to the server's own (promotions, pauses, copied Pov records), so a
           full ring never stops the writes that let its head move on. */
        enum class TRoom {
          All,
          KeepHeadroom
        };

        /* Ring-space accounting (#891).  The leader never overwrites a group at or after the
           checkpointed head, and the head moves only once the records before it are no longer
           needed, which for a record that pushed into a root's memory layer means once that layer
           is in a data file.  The flush that writes the layer first waits for those records to be
           durable (write-ahead), so a record the leader can't write until the head moves would
           strand the flush, and the flush the checkpoint: the server would hang for good.

           So the WAL takes a record only if it can promise it room: every record taken but not yet
           written, together with the groups that will seal them, fits between the write position
           and the checkpointed head, with the head where it is.  A record that doesn't fit is
           refused (TWalRingFull) before anything is appended.

           Reserve() takes that promise ahead of the append, for a caller that must not fail once
           it has started (a transaction reserves before it applies, and appends from its
           destructor).  It throws TWalRingFull if there is no room, after waiting up to `wait` for
           a checkpoint to make some; a fiber must not wait.  With TRoom::KeepHeadroom it throws
           TWalRecordTooLarge, at once, for a record bigger than half the headroom.

           Promotion room (#959).  An update pushed into a safe child stays in the ring until it
           reaches a root's data file, and it gets there only by being promoted, a record about as
           big for each level.  If those promotions had to compete for room with everything else,
           the ring could fill up to the record that pushed the update, and then nothing would
           fit, the promotion included: the head could never move again.  So the ring keeps that
           room for them.  A record that pushes into a safe child owes it (`owes`, held from
           Reserve until the record is appended or the reservation released, then until the
           retention repays it), and no record may take room the debt is owed, except a
           promotion spending the room its own pops were owed (`credit`). */
        TWalReservation Reserve(size_t size, TRoom room, std::chrono::milliseconds wait = std::chrono::milliseconds(0), size_t credit = 0UL,
                                std::vector<TPromotionDebt> owes = std::vector<TPromotionDebt>());

        /* What a record does to the promotion debt: the debts of the updates it popped, now repaid,
           and those of the updates it pushed, now owed. */
        struct TPromotionDebtChange {
          std::vector<TPromotionDebt> Repaid;
          std::vector<TPromotionDebt> Owed;
        };

        /* Append a record. Returns assigned LSN. Thread-safe.  Throws TWalRingFull if there is no
           room for it (with TRoom::All, and the debt it would owe). */
        uint64_t Append(TWalRecordType type, const void *data, size_t size, uint8_t flags = 0U, const TPromotionDebtChange &debt = TPromotionDebtChange());

        /* Append a record into room reserved for it, which it uses up.  Never refused for room, so
           long as the record is no bigger than the room.  The debt the reservation held gives way
           to `debt`, in the same step. */
        uint64_t Append(TWalReservation &reservation, TWalRecordType type, const void *data, size_t size, uint8_t flags = 0U,
                        const TPromotionDebtChange &debt = TPromotionDebtChange());

        /* Reserve room, from TRoom::All, for records of these body sizes all at once, spending the
           promotion debt `spend` (#963): the records are admitted together, counting `spend` as
           free, and if they are, `spend` is repaid in the same step, so no record taken in between
           can take the room it was kept.  Throws TWalRingFull, having changed nothing, if they
           don't fit.  A caller that then appends none of them gives the room back by releasing
           the reservations, but not the debt: that it must owe again itself. */
        std::vector<TWalReservation> ReserveGroup(const std::vector<size_t> &sizes, const std::vector<TPromotionDebt> &spend);

        /* Change the promotion debt without a record: an update gone with its repo, or the debts
           recovery rebuilt, owed to the WAL recovery made. */
        void ChangePromotionDebt(const TPromotionDebtChange &debt);

        /* The most a record of `size` body bytes can add to the group it goes in, in whole pages.
           A reservation is charged this and a page more, for the seal-only group that may have to
           follow it. */
        static size_t GetRecordGroupBytes(size_t size);

        /* What one promotion of an update is owed, if the record that pushed it had `size` body
           bytes: a reservation's charge for a record that big plus PromotionSlackBytes, for what a
           promotion adds (its pop, its parent). */
        static size_t GetPromotionGroupBytes(size_t size);

        static constexpr size_t PromotionSlackBytes = 512UL;

        /* Ring bytes promised to records taken but not yet written, seals included.  For tests and
           status. */
        size_t GetOwedBytes() const;

        /* Ring bytes owed to promotions not yet written.  For tests and status. */
        size_t GetPromotionDebtBytes() const;

        /* Convenience: append and wait for durability. Returns assigned LSN. */
        uint64_t AppendAndWait(TWalRecordType type, const void *data, size_t size, uint8_t flags = 0U);

        /* Wait until record with lsn is durably synced and sealed. */
        void WaitForDurable(uint64_t lsn);

        /* Wait with timeout. Returns true if durable, false on timeout. */
        bool WaitForDurable(uint64_t lsn, std::chrono::milliseconds timeout);

        /* Flush all pending records through durable sync and seal. */
        void Flush();

        /* The most payload a checkpoint can carry: the slot length less the header.  A server sizes its
           snapshots to this. */
        static size_t MaxCheckpointPayload(size_t slot_bytes) {
          return slot_bytes > sizeof(TCheckpointHeader) ? slot_bytes - sizeof(TCheckpointHeader) : 0UL;
        }

        /* Write a checkpoint to the inactive checkpoint slot, naming head_lsn as the oldest record
           recovery still needs.  The head never moves back.  Checkpoints are serialised, and the
           leader never overwrites a group at or after the head that the newest synced checkpoint
           names: when the ring is full up to it, appends wait for a checkpoint to move it. */
        void Checkpoint(uint64_t head_lsn, uint64_t global_flushed_seq, const void *payload = nullptr, size_t payload_len = 0UL);

        /* How far, in ring bytes, the write position is past the start of the group holding lsn, or
           0 if lsn is no longer in the ring (or not yet written). */
        size_t GetRingBytesBehind(uint64_t lsn) const;

        /* The ring's size in bytes. */
        size_t GetCapacityBytes() const { return Config.CapacityBytes; }

        /* The last part of the ring that only TRoom::All may use (#891). */
        size_t GetHeadroomBytes() const { return HeadroomBytes; }

        /* How many records this ring has refused as full (TWalRingFull from Reserve or Append; not
           ReserveGroup's).  The retention reads it to tell that the ring is short of room now. */
        size_t GetRingFullRefusals() const { return RingFullRefusals.load(std::memory_order_relaxed); }

        /* Turn TConfig::AutoSealOnQuiet on or off from now on: off, the newest group is not durable
           until another follows it. */
        void SetAutoSealOnQuietForTest(bool on);

        /* Call `hook` in the syncer, without Mutex, before each device sync from now on.  Set it
           before the first Append. */
        void SetBeforeSyncForTest(std::function<void ()> hook);

        /* Copy-forward unpromoted records near the head. Returns assigned new LSNs. */
        std::vector<uint64_t> CopyForward(const std::vector<std::pair<TWalRecordType, std::vector<char>>> &records);

        /* Status & query accessors */
        bool IsFailed() const;
        uint64_t GetLastAppendedLsn() const;
        uint64_t GetDurableLsn() const;
        uint64_t GetHeadLsn() const;
        uint32_t GetLap() const;
        uint64_t GetGroupNum() const;

        /* Static recovery scanners */
        static TScanResult Scan(Util::TDevice *device, const TConfig &config, uint64_t start_group_num = 1UL, uint64_t start_lsn = 1UL, uint32_t start_lap = 1U, uint64_t start_ring_offset = 0UL, uint64_t start_chain = 0UL);
        static std::optional<TCheckpoint> ReadNewestCheckpoint(Util::TDevice *device, Util::TOffset slot0, Util::TOffset slot1, const Base::TUuid &store_id, size_t slot_bytes = 0UL);

        private:

        struct TPendingRecord {
          uint64_t Lsn = 0UL;
          TWalRecordType Type = TWalRecordType::Txn;
          uint8_t Flags = 0U;
          std::vector<char> Encoded; // TRecordHeader + body
        };

        struct TInFlightGroup {
          uint64_t GroupNum = 0UL;
          uint64_t FirstLsn = 0UL;
          uint64_t LastLsn = 0UL;
          uint64_t SealedThrough = 0UL;
          uint32_t Lap = 1U;
          size_t Offset = 0UL;
          size_t Size = 0UL;
          bool IsSealOnly = false;
        };

        struct TGroupSummary {
          uint64_t GroupNum = 0UL;
          uint64_t FirstLsn = 0UL;
          uint64_t LastLsn = 0UL;
          uint32_t Lap = 1U;
          size_t Offset = 0UL;
          uint64_t HeaderChecksum = 0UL;
          uint64_t Chain = 0UL;
        };

        void LeaderMain();
        void SyncerMain();
        void Stop();

        /* Ring-space accounting (#891); see Reserve().  All require Mutex. */

        /* What the records taken but not written still need from the ring, seals included. */
        size_t OwedBytesLocked() const;

        /* How much of the ring a sequence of groups, none bigger than `max_group`, is sure to fit
           in from the write position without the checkpointed head moving.  A group that doesn't
           fit before the end of the ring wraps, wasting the rest of the lap. */
        size_t UsableBytesLocked(size_t max_group) const;

        /* The biggest group the leader could build from the records taken, plus more charged
           `extra` group bytes, none bigger than `biggest_extra` (all of `extra` by default: one
           record). */
        size_t MaxGroupLocked(size_t extra, size_t biggest_extra = SIZE_MAX) const;

        /* Whether records charged `group_bytes` in all fit the promise; `biggest_group` is the
           biggest of them (all of `group_bytes` by default: one record). */
        bool AdmitsLocked(size_t group_bytes, TRoom room, size_t credit = 0UL, size_t new_debt = 0UL, size_t new_debt_group = 0UL,
                          size_t biggest_group = SIZE_MAX) const;

        /* Owe or repay promotion room.  Repaying more than is owed (which the accounting never
           should) is logged, and clamps at nothing owed. */
        void OweLocked(const std::vector<TPromotionDebt> &debts);
        void RepayLocked(const std::vector<TPromotionDebt> &debts);

        /* Throw TWalRingFull, for a record of `size` body bytes. */
        [[noreturn]] void ThrowRingFullLocked(size_t size, TRoom room) const;

        /* Take a reservation's charge, or give it back. */
        void ChargeLocked(size_t group_bytes);
        void UnchargeLocked(size_t group_bytes);

        /* Account for a record of `encoded` bytes joining PendingRecords: it goes in the last group
           the leader will build from them, as the leader will put it, or starts a new one. */
        void QueueRecordLocked(size_t encoded);

        /* The leader has taken the first of those groups, of `payload` bytes. */
        void DequeueGroupLocked(size_t payload);

        /* Throws TWalIoError if failed or stopping, and for a record no group could hold. */
        void CheckAppendableLocked(size_t size) const;

        uint64_t AppendLocked(TWalRecordType type, const void *data, size_t size, uint8_t flags);

        friend class TWalReservation;

        void ReleaseReservation(TWalReservation &reservation);

        Util::TDevice *Device;
        const TConfig Config;

        mutable std::mutex Mutex;

        /* Config.AutoSealOnQuiet unless a test changed it.  Covered by Mutex. */
        bool AutoSealOnQuiet;
        std::condition_variable LeaderCv;
        std::condition_variable SyncerCv;
        std::condition_variable DurableCv;

        uint64_t NextLsn;
        uint64_t NextGroupNum;
        uint32_t CurrentLap;
        size_t CurrentRingOffset;
        uint64_t PreviousGroupChecksum;

        uint64_t HighestSyncedLsn;
        uint64_t HighestSealedLsn;
        uint64_t DurableLsn;
        uint64_t HeadLsn;
        uint64_t HeadGroupNum;
        uint32_t HeadLap;
        uint64_t HeadRingOffset;
        uint64_t HeadChain;

        /* The head position the newest synced checkpoint names: what recovery would scan from now,
           so the leader must not overwrite it (§5.6). */
        uint32_t CheckpointedHeadLap;
        uint64_t CheckpointedHeadRingOffset;

        /* Serialises Checkpoint(), so two callers never pick the same slot. */
        std::mutex CheckpointMutex;
        std::optional<uint64_t> LastCheckpointNum;

        bool Stopping;
        bool Failed;

        /* The leader has stopped: no group joins InFlightGroups from now on, so the syncer may stop
           once it is empty.  Stopping alone is not enough, since the leader may still have records
           to write.  Covered by Mutex. */
        bool LeaderStopped = false;

        /* SetBeforeSyncForTest's hook. */
        std::function<void ()> BeforeSyncForTest;

        /* The most payload the leader puts in one group (a single bigger record goes alone), and the
           group that payload makes. */
        size_t MaxGroupPayload;
        size_t MaxBatchGroupBytes;

        /* What a reservation keeping headroom leaves free (TRoom::KeepHeadroom). */
        size_t HeadroomBytes;

        /* See GetRingFullRefusals. */
        mutable std::atomic<size_t> RingFullRefusals{0UL};

        /* The charges (GetRecordGroupBytes) of the reservations not yet used or released, and their
           sum.  Each may be appended alone, so each may make a group of its own. */
        std::multiset<size_t> ReservedGroupBytes;
        size_t ReservedGroupBytesSum;

        /* Promotion room owed (#959): see Reserve.  The group size of each debt, so the biggest
           can be allowed for where a group wraps, and the sum of GroupBytes * Levels. */
        std::multiset<size_t> DebtGroupBytes;
        size_t DebtBytesSum = 0UL;

        /* The groups the leader will build from PendingRecords, as it builds them (in order, each
           taking records while they fit in MaxGroupPayload): each one's payload, the multiset of
           their sizes in the ring, and the sum of those sizes. */
        std::deque<size_t> PendingGroupPayloads;
        std::multiset<size_t> PendingGroupBytes;
        size_t PendingGroupBytesSum;

        /* Whether a group with records in it has been given a position since the last seal-only
           group was (one more seal-only group may follow it). */
        bool Unsealed;

        /* Notified whenever the room the accounting can promise may have grown. */
        std::condition_variable SpaceCv;

        std::deque<TPendingRecord> PendingRecords;
        std::deque<TInFlightGroup> InFlightGroups;
        std::deque<TGroupSummary> ActiveGroups;

        std::thread LeaderThread;
        std::thread SyncerThread;
      };

    }  // Disk

  }  // Indy

}  // Orly
