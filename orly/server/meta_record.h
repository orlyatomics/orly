/* <orly/server/meta_record.h>

   Per-invocation metadata captured for every `orlyi` method call.
   `TMetaRecord::TEntry` holds session id, optional user id,
   fully-qualified package name, method name, the named arg map,
   expected predicate results, timestamp, and random seed. Used for
   replay and audit -- the random seed in particular lets a
   deterministic replay reproduce side effects that depended on
   `RandomInt`.

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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <orly/sabot/all.h>

namespace Orly {

  namespace Server {

    class TMetaRecord {
      public:

      class TEntry {
        public:

        using TPackageFqName = std::vector<std::string>;

        using TArgByName = std::map<std::string, Var::TVar>;

        /* Why not vector of bool?  Because up yours, STL explicit specialization with weird return types on operator[], that's why. */
        using TExpectedPredicateResults = std::vector<uint8_t>;

        TEntry() {}

        TEntry(
            const Base::TUuid &session_id, const std::optional<Base::TUuid> &user_id, const TPackageFqName &package_fq_name, const std::string &method_name, TArgByName &&arg_by_name,
            TExpectedPredicateResults &&expected_predicate_results, Base::Chrono::TTimePnt now, uint64_t random_seed)
            : SessionId(session_id), UserId(user_id), PackageFqName(package_fq_name), MethodName(method_name), ArgByName(std::move(arg_by_name)),
              ExpectedPredicateResults(std::move(expected_predicate_results)), RunTimestamp(now), RandomSeed(random_seed) {}

        /* One method call an entry records (#751). An entry records one call, or a whole batch
           of them (TSession::RunBatch, #253/#255), all committed as one update. */
        struct TCall {

          TPackageFqName PackageFqName;

          std::string MethodName;

          TArgByName ArgByName;

        };  // TCall

        /* The arg map that records a batch of calls in one entry, whose own package and method
           are the first call's. Call i's args are recorded under an index prefix ("<i>.<name>");
           argument names are identifiers, so the prefix can't collide with one. If the calls don't
           all name the same package and method, each call's own are recorded too, as
           "<i>.$package" (the path joined with '/') and "<i>.$method". "$calls" holds the number
           of calls, so a batch of calls without args still records how many there were. '$' can't
           appear in an argument name. */
        static TArgByName EncodeBatch(const std::vector<TCall> &calls);

        /* The calls this entry records, in the order they ran, each with its own package, method
           and args. Tetris replays them in this order, on one context, to test the entry's
           expected predicate results at promotion (#751). Throws std::runtime_error if the
           record is malformed. Batch records written before "$calls" existed (v0.2.0) are read
           by their index prefixes. */
        std::vector<TCall> GetCalls() const;

        const TArgByName &GetArgByName() const {
          return ArgByName;
        }

        const TExpectedPredicateResults &GetExpectedPredicateResults() const {
          return ExpectedPredicateResults;
        }

        const std::string &GetMethodName() const {
          return MethodName;
        }

        const TPackageFqName &GetPackageFqName() const {
          return PackageFqName;
        }

        uint64_t GetRandomSeed() const {
          return RandomSeed;
        }

        const Base::Chrono::TTimePnt &GetRunTimestamp() const {
          return RunTimestamp;
        }

        const Base::TUuid &GetSessionId() const {
          return SessionId;
        }

        const std::optional<Base::TUuid> &GetUserId() const {
          return UserId;
        }

        private:

        Base::TUuid SessionId;

        std::optional<Base::TUuid> UserId;

        TPackageFqName PackageFqName;

        std::string MethodName;

        TArgByName ArgByName;

        TExpectedPredicateResults ExpectedPredicateResults;

        Base::Chrono::TTimePnt RunTimestamp;

        /* 64 bits (#358): the full mt19937_64 seed. Widening changes this
           record's serialized shape; meta records in databases written by
           older builds would not read back as this type (no cross-build
           on-disk compatibility is promised anywhere in this tree). */
        uint64_t RandomSeed;

      };  // TMetaRecord::TEntry

      using TEntryByUpdateId = std::map<Base::TUuid, TEntry>;

      TMetaRecord() {}

      TMetaRecord(const Base::TUuid &update_id, TEntry &&entry) {
        EntryByUpdateId.insert(std::make_pair(update_id, std::forward<TEntry>(entry)));
      }

      const TEntry &GetEntry(const Base::TUuid &id) const;

      const TEntryByUpdateId &GetEntryByUpdateId() const {
        return EntryByUpdateId;
      }

      private:

      TEntryByUpdateId EntryByUpdateId;

    };  // TMetaRecord

  }  // Server

}  // Orly
