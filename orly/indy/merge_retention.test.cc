/* <orly/indy/merge_retention.test.cc>

   Exhaustive small-scope test of the merge and retention rules, with broken variants
   that must fail (#856).

   Generates every sequence of events (write, flush, merge adjacent files, open/release reader,
   advance retention window) up to a bounded depth for 2-3 keys. After every event, checks
   reads at all versions against an oracle computed from the complete write log.

   Verifies:
     1. Today's pruning rule (#592, #602): superseded versions dropped, no window.
     2. Option A retention policy (#745): superseded versions kept while inside the retention
        window or held by an open reader.
     3. Negative controls (broken variants):
        - Dropping a non-superseded version (must fail)
        - Dropping a version inside the retention window (must fail)
        - Dropping a version pinned by an open reader (must fail)
        - Pairing non-adjacent files / bad generation ordering (must fail)
        - Answering from partial state when outside the window (must fail)

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

#include <orly/indy/merge_retention.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy;

namespace {

  using TKey = int64_t;
  using TVal = int64_t;

  struct TEntry {
    TKey Key;
    TVal Val;
    TSequenceNumber Seq;

    [[nodiscard]] TSequenceNumber GetSeqNum() const noexcept { return Seq; }
  };

  using TFile = std::vector<TEntry>;

  /* Read outcome for oracle comparison. */
  enum class TReadOutcome {
    Value,
    NotFound,
    Unavailable
  };

  struct TReadResult {
    TReadOutcome Outcome;
    TVal Value = 0;

    bool operator==(const TReadResult &other) const noexcept {
      return Outcome == other.Outcome && (Outcome != TReadOutcome::Value || Value == other.Value);
    }
    bool operator!=(const TReadResult &other) const noexcept {
      return !(*this == other);
    }
  };

  /* Model state of the repo. */
  struct TRepoState {
    TSequenceNumber NextSeq = 1UL;
    std::vector<TEntry> MemLayer;
    std::vector<TFile> DiskFiles; // Index 0 is oldest, back() is newest
    TSequenceNumber Horizon = 0UL;
    std::set<TSequenceNumber> OpenReaders;

    /* Full write log for the oracle. */
    std::vector<TEntry> FullLog;
  };

  /* The oracle: computes the true answer for (key, at_version) from scratch using FullLog. */
  TReadResult ReadOracle(const TRepoState &state, TKey key, TSequenceNumber at_version, bool has_window) {
    if (at_version >= state.NextSeq) {
      return {TReadOutcome::Unavailable};
    }
    const bool in_window = (at_version >= state.Horizon);
    const bool pinned = (state.OpenReaders.count(at_version) > 0);
    if (!in_window && !pinned) {
      /* Version is below horizon and not held by any reader -> version_unavailable. */
      return {TReadOutcome::Unavailable};
    }
    std::optional<TEntry> best;
    for (const auto &entry : state.FullLog) {
      if (entry.Key == key && entry.Seq <= at_version) {
        if (!best || entry.Seq > best->Seq) {
          best = entry;
        }
      }
    }
    if (best) {
      return {TReadOutcome::Value, best->Val};
    }
    return {TReadOutcome::NotFound};
  }

  /* Reads from the model repo state searching layers newest to oldest. */
  TReadResult ReadRepo(const TRepoState &state, TKey key, TSequenceNumber at_version, bool allow_partial = false) {
    if (at_version >= state.NextSeq) {
      return {TReadOutcome::Unavailable};
    }
    const bool in_window = (at_version >= state.Horizon);
    const bool pinned = (state.OpenReaders.count(at_version) > 0);
    if (!allow_partial && !in_window && !pinned) {
      return {TReadOutcome::Unavailable};
    }

    // 1. Check memory layer (newest)
    for (auto it = state.MemLayer.rbegin(); it != state.MemLayer.rend(); ++it) {
      if (it->Key == key && it->Seq <= at_version) {
        return {TReadOutcome::Value, it->Val};
      }
    }
    // 2. Check disk files from newest to oldest
    for (auto file_it = state.DiskFiles.rbegin(); file_it != state.DiskFiles.rend(); ++file_it) {
      std::optional<TEntry> best;
      for (const auto &entry : *file_it) {
        if (entry.Key == key && entry.Seq <= at_version) {
          if (!best || entry.Seq > best->Seq) {
            best = entry;
          }
        }
      }
      if (best) {
        return {TReadOutcome::Value, best->Val};
      }
    }
    return {TReadOutcome::NotFound};
  }

  /* Merge two adjacent files using the retention policy. */
  TFile MergePair(const TFile &older,
                  const TFile &newer,
                  const TMergeRetentionPolicy &policy,
                  bool drop_too_many = false) {
    std::map<TKey, std::vector<TEntry>> entries_by_key;
    for (const auto &e : older) entries_by_key[e.Key].push_back(e);
    for (const auto &e : newer) entries_by_key[e.Key].push_back(e);

    TFile merged;
    for (auto &pair : entries_by_key) {
      auto &vec = pair.second;
      std::sort(vec.begin(), vec.end(), [](const TEntry &a, const TEntry &b) {
        return a.Seq > b.Seq;
      });

      if (drop_too_many) {
        // Mutation: drop one version too many (drops the newest version).
        if (!vec.empty()) {
          vec.erase(vec.begin());
        }
      }

      const auto kept_seqs = FilterRetainedVersions(vec, policy);
      const std::set<TSequenceNumber> kept_set(kept_seqs.begin(), kept_seqs.end());
      for (const auto &entry : vec) {
        if (kept_set.count(entry.Seq)) {
          merged.push_back(entry);
        }
      }
    }
    std::sort(merged.begin(), merged.end(), [](const TEntry &a, const TEntry &b) {
      return a.Key < b.Key || (a.Key == b.Key && a.Seq < b.Seq);
    });
    return merged;
  }

  /* Configuration for an exhaustive model run. */
  struct TRunConfig {
    bool HasWindow = false;
    size_t MaxDepth = 8;
    size_t MaxWrites = 4;
    size_t NumKeys = 2;

    /* Negative control switches: */
    bool BrokenDropTooMany = false;
    bool BrokenIgnoreWindow = false;
    bool BrokenIgnoreReader = false;
    bool BrokenBadPairing = false;
    bool BrokenAnswerPartial = false;
  };

  struct TRunStats {
    size_t StatesExplored = 0;
    size_t Mismatches = 0;
  };

  void Explore(TRepoState &state,
               size_t depth,
               size_t total_writes,
               const TRunConfig &config,
               TRunStats &stats,
               std::vector<std::string> &trace) {
    ++stats.StatesExplored;

    // Build the active policy
    TMergeRetentionPolicy policy;
    policy.HasWindow = config.HasWindow && !config.BrokenIgnoreWindow;
    policy.Horizon = state.Horizon;
    if (!config.BrokenIgnoreReader) {
      policy.OpenReaders = &state.OpenReaders;
    }

    // 1. Oracle check at current state across all keys and all past versions
    for (int64_t k = 0; k < static_cast<int64_t>(config.NumKeys); ++k) {
      for (TSequenceNumber v = 0; v <= state.NextSeq; ++v) {
        const auto expected = ReadOracle(state, k, v, config.HasWindow);
        const auto actual = ReadRepo(state, k, v, config.BrokenAnswerPartial);
        if (expected != actual) {
          const bool is_broken = config.BrokenDropTooMany || config.BrokenIgnoreWindow || config.BrokenIgnoreReader || config.BrokenBadPairing || config.BrokenAnswerPartial;
          if (!is_broken && stats.Mismatches < 3) {
            std::cerr << "UNEXPECTED MISMATCH:" << std::endl;
            for (const auto &step : trace) {
              std::cerr << "  " << step << std::endl;
            }
            std::cerr << "key=" << k << " at_v=" << v
                      << " Horizon=" << state.Horizon
                      << " OpenReaders=[";
            for (auto r : state.OpenReaders) std::cerr << r << " ";
            std::cerr << "] expected=" << (expected.Outcome == TReadOutcome::Value ? std::to_string(expected.Value) : expected.Outcome == TReadOutcome::NotFound ? "NotFound" : "Unavailable")
                      << " actual=" << (actual.Outcome == TReadOutcome::Value ? std::to_string(actual.Value) : actual.Outcome == TReadOutcome::NotFound ? "NotFound" : "Unavailable")
                      << " (disk_files=" << state.DiskFiles.size() << ")"
                      << std::endl;
          }
          ++stats.Mismatches;
        }


      }
    }

    if (depth >= config.MaxDepth) {
      return;
    }

    // 2. Event: Write(key)
    if (total_writes < config.MaxWrites) {
      for (int64_t k = 0; k < static_cast<int64_t>(config.NumKeys); ++k) {
        TSequenceNumber seq = state.NextSeq++;
        TVal val = 100 * k + seq;
        state.MemLayer.push_back({k, val, seq});
        state.FullLog.push_back({k, val, seq});
        trace.push_back("Write(k=" + std::to_string(k) + ", seq=" + std::to_string(seq) + ")");

        Explore(state, depth + 1, total_writes + 1, config, stats, trace);

        trace.pop_back();
        state.FullLog.pop_back();
        state.MemLayer.pop_back();
        state.NextSeq--;
      }
    }

    // 3. Event: Flush
    if (!state.MemLayer.empty()) {
      TFile flushed = std::move(state.MemLayer);
      state.MemLayer.clear();
      state.DiskFiles.push_back(flushed);
      trace.push_back("Flush()");

      Explore(state, depth + 1, total_writes, config, stats, trace);

      trace.pop_back();
      state.DiskFiles.pop_back();
      state.MemLayer = std::move(flushed);
    }

    // 4. Event: Merge adjacent disk files
    if (state.DiskFiles.size() >= 2) {
      for (size_t i = 0; i + 1 < state.DiskFiles.size(); ++i) {
        size_t idx_a = i;
        size_t idx_b = (config.BrokenBadPairing && i + 2 < state.DiskFiles.size()) ? i + 2 : i + 1;

        TFile file_a = state.DiskFiles[idx_a];
        TFile file_b = state.DiskFiles[idx_b];

        TSequenceNumber highest_merged = 0;
        for (const auto &e : file_a) highest_merged = std::max(highest_merged, e.Seq);
        for (const auto &e : file_b) highest_merged = std::max(highest_merged, e.Seq);

        TFile merged = MergePair(file_a, file_b, policy, config.BrokenDropTooMany);

        TSequenceNumber old_horizon = state.Horizon;
        if (!config.HasWindow) {
          // Today's rule: horizon rises to the highest sequence number merged.
          state.Horizon = std::max(state.Horizon, highest_merged);
        }

        // Apply merge in-place
        state.DiskFiles.erase(state.DiskFiles.begin() + idx_b);
        state.DiskFiles[idx_a] = merged;
        trace.push_back("Merge(" + std::to_string(idx_a) + ", " + std::to_string(idx_b) + ")");

        Explore(state, depth + 1, total_writes, config, stats, trace);

        trace.pop_back();
        state.DiskFiles[idx_a] = file_a;
        state.DiskFiles.insert(state.DiskFiles.begin() + idx_b, file_b);
        state.Horizon = old_horizon;
      }
    }

    // 5. Event: OpenReader(v)
    if (config.HasWindow && state.OpenReaders.size() < 2) {
      for (TSequenceNumber v = state.Horizon; v < state.NextSeq; ++v) {
        if (state.OpenReaders.count(v) == 0) {
          state.OpenReaders.insert(v);
          trace.push_back("OpenReader(" + std::to_string(v) + ")");
          Explore(state, depth + 1, total_writes, config, stats, trace);
          trace.pop_back();
          state.OpenReaders.erase(v);
        }
      }
    }

    // 6. Event: ReleaseReader(v)
    if (!state.OpenReaders.empty()) {
      auto copy = state.OpenReaders;
      for (TSequenceNumber v : copy) {
        state.OpenReaders.erase(v);
        trace.push_back("ReleaseReader(" + std::to_string(v) + ")");
        Explore(state, depth + 1, total_writes, config, stats, trace);
        trace.pop_back();
        state.OpenReaders.insert(v);
      }
    }

    // 7. Event: AdvanceHorizon(v)
    if (config.HasWindow) {
      for (TSequenceNumber v = state.Horizon + 1; v < state.NextSeq; ++v) {
        TSequenceNumber old_h = state.Horizon;
        state.Horizon = v;
        trace.push_back("AdvanceHorizon(" + std::to_string(v) + ")");
        Explore(state, depth + 1, total_writes, config, stats, trace);
        trace.pop_back();
        state.Horizon = old_h;
      }
    }
  }

  TRunStats RunModel(const TRunConfig &config) {
    TRepoState state;
    TRunStats stats;
    std::vector<std::string> trace;
    Explore(state, 0, 0, config, stats, trace);
    return stats;
  }


}  // namespace

/* 1. Today's rule (#592, #602): superseded versions dropped, no window. */
FIXTURE(PruningRuleWithoutWindow) {
  TRunConfig config{
    .HasWindow = false,
    .MaxDepth = 7,
    .MaxWrites = 4,
    .NumKeys = 2,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.StatesExplored, 100UL);
  EXPECT_EQ(stats.Mismatches, 0UL);
}

/* 2. Negative control on today's rule: dropping a non-superseded version MUST fail. */
FIXTURE(BrokenPruningRuleMustFail) {
  TRunConfig config{
    .HasWindow = false,
    .MaxDepth = 7,
    .MaxWrites = 4,
    .NumKeys = 2,
    .BrokenDropTooMany = true,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.Mismatches, 0UL);
}

/* 3. Negative control on today's rule: answering from partial state when outside window MUST fail. */
FIXTURE(AnsweringFromPartialStateMustFail) {
  TRunConfig config{
    .HasWindow = false,
    .MaxDepth = 7,
    .MaxWrites = 4,
    .NumKeys = 2,
    .BrokenAnswerPartial = true,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.Mismatches, 0UL);
}

/* 4. Negative control on generation pairing: merging non-adjacent files MUST fail. */
FIXTURE(BrokenGenerationPairingMustFail) {
  TRunConfig config{
    .HasWindow = false,
    .MaxDepth = 8,
    .MaxWrites = 4,
    .NumKeys = 2,
    .BrokenBadPairing = true,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.Mismatches, 0UL);
}

/* 5. Option A retention policy (#745): window & reader pins preserve history. */
FIXTURE(RetentionRuleWithWindow) {
  TRunConfig config{
    .HasWindow = true,
    .MaxDepth = 7,
    .MaxWrites = 3,
    .NumKeys = 2,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.StatesExplored, 100UL);
  EXPECT_EQ(stats.Mismatches, 0UL);
}

/* 6. Negative control on Option A: dropping versions inside window MUST fail. */
FIXTURE(BrokenWindowRuleMustFail) {
  TRunConfig config{
    .HasWindow = true,
    .MaxDepth = 7,
    .MaxWrites = 3,
    .NumKeys = 2,
    .BrokenIgnoreWindow = true,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.Mismatches, 0UL);
}

/* 7. Negative control on Option A: dropping versions held by an open reader MUST fail. */
FIXTURE(BrokenReaderRuleMustFail) {
  TRunConfig config{
    .HasWindow = true,
    .MaxDepth = 8,
    .MaxWrites = 3,
    .NumKeys = 1,
    .BrokenIgnoreReader = true,
  };
  const auto stats = RunModel(config);
  EXPECT_GT(stats.Mismatches, 0UL);
}
