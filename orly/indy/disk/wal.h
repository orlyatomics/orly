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
        Meta = 4U
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

      /* Alternating checkpoint header (80 bytes, packed) */
      struct TCheckpointHeader {
        char Magic[8];             // "ORLYCHK1"
        uint16_t FormatVersion;    // 1
        uint16_t Reserved;         // 0
        Base::TUuid StoreId;       // 16 bytes
        uint64_t CheckpointNum;    // Monotonically increasing counter
        uint64_t HeadLsn;          // Oldest unretired LSN
        uint64_t DurableLsn;       // Highest durable LSN
        uint64_t GlobalFlushedSeq; // Highest flushed sequence of global POV
        uint32_t PayloadLen;       // Additional checkpoint payload bytes
        uint32_t Reserved2;        // 0
        uint64_t PayloadChecksum;  // Checksum of payload bytes
        uint64_t HeaderChecksum;   // Checksum of preceding 72 bytes
      };

      static_assert(sizeof(TCheckpointHeader) == 84, "TCheckpointHeader size must be 84 bytes");
      static constexpr size_t CheckpointHeaderChecksumCoveredBytes = offsetof(TCheckpointHeader, HeaderChecksum);
      static_assert(CheckpointHeaderChecksumCoveredBytes == 76, "TCheckpointHeader checksum covers 76 bytes");
      #pragma pack(pop)

      /* Checkpoint data snapshot */
      struct TCheckpoint {
        uint64_t CheckpointNum = 0UL;
        uint64_t HeadLsn = 0UL;
        uint64_t DurableLsn = 0UL;
        uint64_t GlobalFlushedSeq = 0UL;
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

      struct TScanResult {
        TScanStatus Status = TScanStatus::Empty;
        uint64_t LastValidLsn = 0UL;
        uint64_t LastValidGroupNum = 0UL;
        uint32_t LastValidLap = 1U;
        uint64_t StoppedAtGroupNum = 0UL;
        uint64_t DamagedLsnStart = 0UL;
        uint64_t DamagedLsnEnd = 0UL;
        std::string ProblemDescription;
        std::vector<TScannedRecord> Records;
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

      /* The Write-Ahead Log class */
      class TWal {
        NO_COPY(TWal);
        public:

        struct TConfig {
          Util::TOffset BaseOffset = 0UL;
          size_t CapacityBytes = 1024UL * 1024UL; // 1 MiB ring
          Util::TOffset CheckpointSlot0Offset = 0UL;
          Util::TOffset CheckpointSlot1Offset = 4096UL;
          Base::TUuid StoreId = Base::TUuid::Null;
          size_t MaxInFlightGroups = 2UL;
          bool AutoSealOnQuiet = true;
          std::chrono::microseconds QuietInterval = std::chrono::microseconds(500);
          bool NoSync = false;       // Test-only negative control: skip device sync while acknowledging (#755)
          bool EarlyAck = false;     // Test-only negative control: acknowledge before group sync (#755)
        };

        TWal(Util::TDevice *device, const TConfig &config, uint64_t start_lsn = 1UL, uint64_t start_group_num = 1UL, uint32_t start_lap = 1U);
        ~TWal();

        /* Append a record. Returns assigned LSN. Thread-safe. */
        uint64_t Append(TWalRecordType type, const void *data, size_t size, uint8_t flags = 0U);

        /* Convenience: append and wait for durability. Returns assigned LSN. */
        uint64_t AppendAndWait(TWalRecordType type, const void *data, size_t size, uint8_t flags = 0U);

        /* Wait until record with lsn is durably synced and sealed. */
        void WaitForDurable(uint64_t lsn);

        /* Wait with timeout. Returns true if durable, false on timeout. */
        bool WaitForDurable(uint64_t lsn, std::chrono::milliseconds timeout);

        /* Flush all pending records through durable sync and seal. */
        void Flush();

        /* Write a checkpoint to the inactive checkpoint slot. */
        void Checkpoint(uint64_t head_lsn, uint64_t global_flushed_seq, const void *payload = nullptr, size_t payload_len = 0UL);

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
        static TScanResult Scan(Util::TDevice *device, const TConfig &config, uint64_t start_group_num = 1UL, uint64_t start_lsn = 1UL, uint32_t start_lap = 1U, uint64_t start_ring_offset = 0UL);
        static std::optional<TCheckpoint> ReadNewestCheckpoint(Util::TDevice *device, Util::TOffset slot0, Util::TOffset slot1, const Base::TUuid &store_id);

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

        void LeaderMain();
        void SyncerMain();
        void Stop();

        Util::TDevice *Device;
        const TConfig Config;

        mutable std::mutex Mutex;
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

        bool Stopping;
        bool Failed;

        std::deque<TPendingRecord> PendingRecords;
        std::deque<TInFlightGroup> InFlightGroups;

        std::thread LeaderThread;
        std::thread SyncerThread;
      };

    }  // Disk

  }  // Indy

}  // Orly
