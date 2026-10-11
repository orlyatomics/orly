/* <orly/server/operation_table.h>

   The exactly-once table (#733): what a retry with an operation id is told about the original.

   It maps (POV id, operation id) to the outcome of the write that id named, so a client that lost
   a reply can send the same request again and get the first answer back instead of a second
   write. See docs/design/durable-acks.md section 8.2.1 for the rules this implements.

   A request is reserved before its method runs, and completed once it commits (or released if it
   didn't write). A safe POV's entries are also recorded in the write's meta record, which is how
   recovery gets them back, and in the log's checkpoint payload, which is how they survive the
   records that held them being retired. A fast POV's are only here.

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
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <orly/method_result.h>
#include <orly/server/meta_record.h>
#include <orly/var.h>

namespace Orly {

  namespace Server {

    class TOperationTable final {
      NO_COPY(TOperationTable);
      public:

      /* The tag that starts a checkpoint payload (#733), then a u16 section count, then the sections.
         A section is a u16 type, a u16 flags, a u32 length and that many bytes. */
      static constexpr uint16_t PayloadVersion = 1;

      /* The section that holds this table. */
      static constexpr uint16_t OperationsSection = 1;

      /* A section flag: a reader that doesn't know the type must refuse the store, not skip it. */
      static constexpr uint16_t SectionRequired = 1;

      /* What a snapshot may take when the caller names no budget: the default 1 MiB checkpoint slot
         less its 116-byte header, and 16 KiB kept for other sections. The server passes its own. */
      static constexpr size_t SnapshotBudgetBytes = 1024UL * 1024UL - 116UL - 16UL * 1024UL;

      struct TConfig {

        /* The most completed entries kept, across every POV. The oldest goes first, and only its own
           POV's watermark rises. */
        size_t RetentionCount = 4096;

        /* How old, by the id's own timestamp, an entry may be before it is dropped and the id
           refused. */
        std::chrono::seconds RetentionAge{3600};

        /* The largest encoded result kept; a bigger one is dropped, and a retry told so. */
        size_t ResultBytes = 256;

        /* The time in ms since the epoch. The system clock if empty; a test sets its own. */
        std::function<uint64_t ()> Clock;

      };  // TConfig

      /* What a completed request left behind. */
      struct TOutcome {

        /* The version the write committed at. */
        uint64_t SequenceNumber = 0;

        /* The log position of the write's record, so a retry waits for the same sync as the
           original, or 0 if there is none to wait for: the write was logged by a run before this
           one, or never logged. */
        uint64_t Lsn = 0;

        /* True if the write is in its meta record, and so comes back from recovery. */
        bool Persistent = false;

        /* The encoded result (see EncodeResult), or empty if it was over the cap. */
        std::optional<std::string> Result;

        /* See HashCalls. */
        uint64_t Hash = 0;

      };  // TOutcome

      /* What Reserve found. */
      struct TReservation {

        /* True if the id is new and the caller must run the request, then Complete() or
           Release() it. False if it is a retry, and Outcome is the first answer. */
        bool IsNew = true;

        TOutcome Outcome;

      };  // TReservation

      TOperationTable();

      explicit TOperationTable(const TConfig &config);

      const TConfig &GetConfig() const {
        return Config;
      }

      /* The time an id carries, in ms since the epoch. Throws std::invalid_argument if it isn't a
         version 7 UUID. */
      static uint64_t GetTimestampMs(const Base::TUuid &op_id);

      /* Claim the id for a request with the given hash. Throws TOperationExpired if the id is too
         old to answer, TOperationIdReused if the id names a different request, and
         TOperationInFlight if the same request is still running; std::invalid_argument if it
         isn't a version 7 UUID. */
      TReservation Reserve(const Base::TUuid &pov_id, const Base::TUuid &op_id, uint64_t hash);

      /* The request committed. Records its outcome, and applies retention. */
      void Complete(const Base::TUuid &pov_id, const Base::TUuid &op_id, TOutcome &&outcome);

      /* The request ended without committing a write. The id can be tried again. */
      void Release(const Base::TUuid &pov_id, const Base::TUuid &op_id) noexcept;

      /* Add an entry recovery found in a replayed record. */
      void Restore(const Base::TUuid &pov_id, const Base::TUuid &op_id, TOutcome &&outcome);

      /* The entries a checkpoint at `head_lsn` must keep: the persistent ones whose records it
         will no longer replay, trimmed oldest first to fit `budget` bytes. Never empty: it holds
         at least the tag. */
      std::string Snapshot(uint64_t head_lsn, size_t budget = SnapshotBudgetBytes);

      /* Load a snapshot. Throws std::runtime_error if its version tag isn't known, it is cut short,
         or it holds a section marked required that this server doesn't know. An unknown section
         that isn't required is skipped. */
      void Load(const std::string &payload);

      /* The number of completed entries, for tests and stats. */
      size_t GetCount() const;

      private:

      struct TSlot {
        bool IsDone = false;
        TOutcome Outcome;
      };

      struct TPovTable {

        /* Ordered by operation id, which for a version 7 UUID is ordered by time. */
        std::map<Base::TUuid, TSlot> SlotByOp;

        /* Ids at or below this are refused: an entry they could have named was dropped. */
        uint64_t ExpiredBeforeMs = 0;

      };  // TPovTable

      uint64_t NowMs() const;

      /* Requires Mutex. Drops completed entries, oldest first, while there are more than the count
         or the oldest is past the age. */
      void Prune();

      /* Reads the body of the operations section. */
      void LoadOperations(const std::string &body);

      const TConfig Config;

      mutable std::mutex Mutex;

      std::unordered_map<Base::TUuid, TPovTable> TableByPov;

      /* The completed entries, oldest first: (time in the id, POV id, operation id). */
      std::set<std::tuple<uint64_t, Base::TUuid, Base::TUuid>> Done;

    };  // TOperationTable

    /* A hash of what a request asked for, to catch an id used for two requests by mistake. It is
       ComputeChecksum64 over a canonical binary encoding, so it doesn't change when a value's text
       form does, which matters because it is stored in the checkpoint. The encoding is in
       docs/design/durable-acks.md section 8.2.1; the same function hashes the live request and the
       replayed record. */
    uint64_t HashCalls(const std::vector<TMetaRecord::TEntry::TCall> &calls);

    /* A method result as the bytes the table keeps, with no tracker. */
    std::string EncodeResult(const Var::TVar &result);

    /* The result those bytes hold. */
    TMethodResult DecodeResult(const std::string &bytes);

  }  // Server

}  // Orly
