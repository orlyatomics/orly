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
#include <utility>

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
