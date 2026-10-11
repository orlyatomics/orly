/* <orly/indy/merge_retention.h>

   Pure functions and policies for version retention during disk merges (#856, #745).

   A disk merge drops superseded versions to bound disk growth (#592, #602).
   Under today's pruning rule, any superseded version is dropped (no retention window).
   Under the retention policy (Option A in #745), a version is dropped only when:
     1. it is superseded by a newer version for that key, AND
     2. it is outside the POV's retention window (seq < horizon), AND
     3. it is not held by an open reader.

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

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include <orly/indy/sequence_number.h>

namespace Orly::Indy {

  /* Pure retention decision policy (#856, #745). */
  struct TMergeRetentionPolicy {
    /* If false, today's pruning rule (#592, #602): every superseded version is dropped.
       If true, Option A (#745): superseded versions inside the window or pinned by a reader are kept. */
    bool HasWindow = false;

    /* The lower bound of the retention window. Versions < Horizon are outside the window. */
    TSequenceNumber Horizon = 0UL;

    /* Readers currently pinning versions. */
    const std::set<TSequenceNumber> *OpenReaders = nullptr;

    /* Returns true if an open reader holds this version. */
    [[nodiscard]] bool IsHeldByReader(TSequenceNumber seq) const noexcept {
      return OpenReaders && OpenReaders->count(seq) > 0;
    }

    /* Core retention decision for one version of a key.
       - seq: the version's sequence number.
       - is_superseded: true if a newer version for this key is retained in the output.
       Returns true if the version must be kept, false if it may be dropped. */
    [[nodiscard]] bool ShouldRetain(TSequenceNumber seq, bool is_superseded) const noexcept {
      if (!is_superseded) {
        return true;  // The newest version for the key is always kept.
      }
      if (!HasWindow) {
        return false; // Today's pruning rule: superseded versions are dropped.
      }
      if (seq >= Horizon) {
        return true;  // Retained inside the retention window.
      }
      if (IsHeldByReader(seq)) {
        return true;  // Retained because an open reader pins this version.
      }
      return false;   // Outside window, superseded, not held by any reader -> dropped.
    }
  };

  /* Given a key's versions ordered from newest to oldest:
     Returns the sequence numbers that are retained under the retention policy. */
  template <typename TVersion>
  inline std::vector<TSequenceNumber> FilterRetainedVersions(
      const std::vector<TVersion> &versions_newest_first,
      const TMergeRetentionPolicy &policy) {
    std::vector<TSequenceNumber> kept;
    if (versions_newest_first.empty()) {
      return kept;
    }

    if (!policy.HasWindow) {
      // Today's pruning rule (#592, #602): keep only the newest version.
      kept.push_back(versions_newest_first.front().GetSeqNum());
      return kept;
    }

    // Under retention window / Option A (#745):
    // A version is kept if:
    // 1. It is inside the retention window (seq >= policy.Horizon).
    // 2. OR it is the base version as of policy.Horizon (newest version with seq < policy.Horizon).
    // 3. OR it is needed by an open reader (for each open reader at R, the newest version with seq <= R).

    bool kept_base_for_horizon = false;

    std::set<TSequenceNumber> needed_by_readers;
    if (policy.OpenReaders) {
      for (TSequenceNumber r : *policy.OpenReaders) {
        for (const auto &v : versions_newest_first) {
          TSequenceNumber seq = v.GetSeqNum();
          if (seq <= r) {
            needed_by_readers.insert(seq);
            break;
          }
        }
      }
    }

    for (const auto &v : versions_newest_first) {
      TSequenceNumber seq = v.GetSeqNum();
      bool keep = false;

      if (seq >= policy.Horizon) {
        keep = true;
      } else if (!kept_base_for_horizon) {
        keep = true;
        kept_base_for_horizon = true;
      }

      if (needed_by_readers.count(seq) > 0) {
        keep = true;
      }

      if (keep) {
        kept.push_back(seq);
      }
    }

    return kept;
  }


}  // namespace Orly::Indy
