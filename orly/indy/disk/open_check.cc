/* <orly/indy/disk/open_check.cc>

   Implements <orly/indy/disk/open_check.h>.

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

#include <orly/indy/disk/open_check.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include <syslog.h>

#include <orly/indy/disk/file_service.h>
#include <orly/indy/disk/util/volume_manager.h>

using namespace std;
using namespace Orly::Indy::Disk;

const char *TSeqRangeProblem::GetKindName() const {
  switch (Kind) {
    case Inverted: {
      return "inverted";
    }
    case Nested: {
      return "nested";
    }
    case PartialOverlap: {
      return "partial overlap";
    }
  }
  return "unknown";
}

string TSeqRangeProblem::Describe() const {
  ostringstream strm;
  if (Kind == Inverted) {
    strm << "file [" << GenId << "] has an inverted sequence range (lowest above highest)";
  } else {
    strm << "files [" << OtherGenId << "] and [" << GenId << "] have overlapping sequence ranges (" << GetKindName() << ")";
  }
  return strm.str();
}

vector<TSeqRangeProblem> Orly::Indy::Disk::FindSeqRangeProblems(const vector<TFileObj> &files) {
  vector<TSeqRangeProblem> problems;
  vector<const TFileObj *> sorted;
  for (const auto &file : files) {
    if (file.Kind != TFileObj::DataFile) {
      continue;
    }
    if (file.LowestSeq > file.HighestSeq) {
      problems.push_back({TSeqRangeProblem::Inverted, file.GenId, file.GenId});
      continue;
    }
    sorted.push_back(&file);
  }
  sort(sorted.begin(), sorted.end(), [](const TFileObj *lhs, const TFileObj *rhs) {
    return lhs->LowestSeq < rhs->LowestSeq || (lhs->LowestSeq == rhs->LowestSeq && lhs->HighestSeq > rhs->HighestSeq);
  });
  /* The file reaching furthest so far. Any file starting at or before its end overlaps it. */
  const TFileObj *reach = nullptr;
  for (const TFileObj *file : sorted) {
    if (reach && file->LowestSeq <= reach->HighestSeq) {
      /* Sorted by lowest, so 'file' starts inside 'reach'; it is nested unless it ends past it. */
      problems.push_back({file->HighestSeq <= reach->HighestSeq ? TSeqRangeProblem::Nested : TSeqRangeProblem::PartialOverlap,
                          file->GenId, reach->GenId});
    }
    if (!reach || file->HighestSeq > reach->HighestSeq) {
      reach = file;
    }
  }
  return problems;
}

namespace {

  /* A bit per block id, grown as needed. */
  class TBlockSet {
    public:

    /* Sets the bit; returns false if it was already set. */
    bool Insert(size_t block_id) {
      if (block_id >= Bits.size()) {
        Bits.resize(std::max(block_id + 1UL, Bits.size() * 2UL), false);
      }
      if (Bits[block_id]) {
        return false;
      }
      Bits[block_id] = true;
      ++Size;
      return true;
    }

    bool Contains(size_t block_id) const {
      return block_id < Bits.size() && Bits[block_id];
    }

    void Erase(size_t block_id) {
      if (Contains(block_id)) {
        Bits[block_id] = false;
        --Size;
      }
    }

    size_t GetSize() const {
      return Size;
    }

    size_t GetLimit() const {
      return Bits.size();
    }

    private:

    vector<bool> Bits;

    size_t Size = 0UL;

  };  // TBlockSet

  using TFileVec = vector<pair<Base::TUuid, TFileObj>>;

  TFileVec GetFiles(TFileService *file_service) {
    TFileVec files;
    file_service->ForEachFile([&files](const Base::TUuid &file_uid, const TFileObj &file) {
      files.emplace_back(file_uid, file);
      return true;
    });
    return files;
  }

  bool IsInMap(TFileService *file_service, const Base::TUuid &file_uid, size_t gen_id) {
    size_t block_id, block_offset, file_size, num_keys;
    return file_service->FindFile(file_uid, gen_id, block_id, block_offset, file_size, num_keys);
  }

  string DescribeFile(const Base::TUuid &file_uid, const TFileObj &file) {
    ostringstream strm;
    strm << (file.Kind == TFileObj::DataFile ? "data" : "durable") << " file " << file_uid << " gen [" << file.GenId << "]";
    return strm.str();
  }

  /* Up to 'max' ids, then how many more. */
  string ListBlocks(const vector<size_t> &blocks, size_t max = 16UL) {
    ostringstream strm;
    for (size_t i = 0UL; i < blocks.size() && i < max; ++i) {
      strm << (i ? " " : "") << blocks[i];
    }
    if (blocks.size() > max) {
      strm << " ... (" << blocks.size() - max << " more)";
    }
    return strm.str();
  }

}  // namespace

TOpenCheck Orly::Indy::Disk::CheckOpenConsistency(Util::TVolumeManager *vol_man,
                                                  TFileService *file_service,
                                                  const vector<size_t> &engine_blocks,
                                                  const TForEachFileBlockRange &for_each_file_block_range) {
  const auto start = chrono::steady_clock::now();
  TOpenCheck check;
  check.MergeRunning = vol_man->GetPendingClaims() > 0UL;
  /* The map first, then the blocks. A file removed in between frees its blocks only after it
     has left the map, so its blocks can only look unheld, and that is re-checked below. */
  const TFileVec files = GetFiles(file_service);
  check.NumFiles = files.size();
  TBlockSet owned;
  set<size_t> shared;
  auto own = [&](size_t block_id) {
    if (!owned.Insert(block_id)) {
      shared.insert(block_id);
    }
  };
  for (size_t block_id : engine_blocks) {
    own(block_id);
  }
  vector<size_t> service_blocks;
  file_service->AppendOwnBlocks(service_blocks);
  for (size_t block_id : service_blocks) {
    own(block_id);
  }
  map<Base::TUuid, vector<TFileObj>> data_files_by_repo;
  for (const auto &[file_uid, file] : files) {
    try {
      for_each_file_block_range(file_uid, file, [&](const pair<size_t, size_t> &range) {
        for (size_t i = 0UL; i < range.second; ++i) {
          own(range.first + i);
        }
      });
    } catch (const exception &ex) {
      check.Unreadable.push_back(DescribeFile(file_uid, file) + ": " + ex.what());
    }
    if (file.Kind == TFileObj::DataFile) {
      data_files_by_repo[file_uid].push_back(file);
    }
  }
  check.NumOwned = owned.GetSize();
  TBlockSet held;
  vol_man->ForEachHeldBlock([&held](size_t block_id) {
    held.Insert(block_id);
  });
  check.NumHeld = held.GetSize();
  vector<size_t> unheld, leaked;
  for (size_t block_id = 0UL; block_id < max(owned.GetLimit(), held.GetLimit()); ++block_id) {
    const bool is_owned = owned.Contains(block_id), is_held = held.Contains(block_id);
    if (is_owned && !is_held) {
      unheld.push_back(block_id);
    } else if (is_held && !is_owned) {
      leaked.push_back(block_id);
    }
  }
  /* An unheld block counts only if it is still not held and its owner still owns it: is
     still in the map, or, for the file service's own blocks, still in its lists. */
  if (!unheld.empty()) {
    set<size_t> candidates;
    for (size_t block_id : unheld) {
      if (!vol_man->IsBlockHeld(block_id)) {
        candidates.insert(block_id);
      }
    }
    set<size_t> confirmed;
    for (size_t block_id : engine_blocks) {
      if (candidates.count(block_id)) {
        confirmed.insert(block_id);
      }
    }
    vector<size_t> service_blocks_now;
    file_service->AppendOwnBlocks(service_blocks_now);
    for (size_t block_id : service_blocks_now) {
      if (candidates.count(block_id)) {
        confirmed.insert(block_id);
      }
    }
    for (const auto &[file_uid, file] : files) {
      if (confirmed.size() == candidates.size() || !IsInMap(file_service, file_uid, file.GenId)) {
        continue;
      }
      try {
        for_each_file_block_range(file_uid, file, [&](const pair<size_t, size_t> &range) {
          for (size_t i = 0UL; i < range.second; ++i) {
            if (candidates.count(range.first + i)) {
              confirmed.insert(range.first + i);
            }
          }
        });
      } catch (const exception &) {
        /* Already reported as unreadable. */
      }
    }
    check.Unheld.assign(confirmed.begin(), confirmed.end());
  }
  /* A leaked block counts only if it is still held and no file that reached the map since
     owns it: a merge or durable write holds its blocks before its file is in the map. */
  if (!leaked.empty()) {
    set<size_t> candidates;
    for (size_t block_id : leaked) {
      if (vol_man->IsBlockHeld(block_id)) {
        candidates.insert(block_id);
      }
    }
    set<pair<Base::TUuid, size_t>> seen;
    for (const auto &[file_uid, file] : files) {
      seen.emplace(file_uid, file.GenId);
    }
    for (const auto &[file_uid, file] : GetFiles(file_service)) {
      if (candidates.empty()) {
        break;
      }
      if (seen.count(make_pair(file_uid, file.GenId))) {
        continue;
      }
      try {
        for_each_file_block_range(file_uid, file, [&](const pair<size_t, size_t> &range) {
          for (size_t i = 0UL; i < range.second; ++i) {
            candidates.erase(range.first + i);
          }
        });
      } catch (const exception &) {
        /* A new file we can't read: leave its blocks as leaked rather than guess. */
      }
    }
    vector<size_t> service_blocks_now;
    file_service->AppendOwnBlocks(service_blocks_now);
    for (size_t block_id : service_blocks_now) {
      candidates.erase(block_id);
    }
    check.Leaked.assign(candidates.begin(), candidates.end());
  }
  check.MergeRunning = check.MergeRunning || vol_man->GetPendingClaims() > 0UL;
  check.Shared.assign(shared.begin(), shared.end());
  for (const auto &[repo_id, repo_files] : data_files_by_repo) {
    for (const auto &problem : FindSeqRangeProblems(repo_files)) {
      check.SeqProblems.emplace_back(repo_id, problem);
    }
  }
  check.Seconds = chrono::duration<double>(chrono::steady_clock::now() - start).count();
  return check;
}

void Orly::Indy::Disk::ReportOpenCheck(const TOpenCheck &check) {
  ostringstream summary;
  summary << "open check: " << check.NumFiles << " files, " << check.NumOwned << " blocks owned, " << check.NumHeld
          << " held, in " << fixed << setprecision(3) << check.Seconds * 1000.0 << " ms";
  if (check.IsClean()) {
    syslog(LOG_INFO, "%s; consistent", summary.str().c_str());
    return;
  }
  syslog(LOG_ERR, "%s; PROBLEMS FOUND (#700)", summary.str().c_str());
  for (const auto &line : check.Unreadable) {
    syslog(LOG_ERR, "open check: can't read the block list of %s; its blocks are counted as leaked", line.c_str());
  }
  if (!check.Leaked.empty()) {
    syslog(LOG_ERR, "open check: %ld LEAKED blocks, held but owned by no file, base image or append log; "
           "they stay unusable until a restart rebuilds the block map%s. Blocks: %s",
           check.Leaked.size(),
           check.MergeRunning ? " (a disk merge was running, and its unfinished output looks like this)" : "",
           ListBlocks(check.Leaked).c_str());
  }
  for (const auto &[repo_id, problem] : check.SeqProblems) {
    ostringstream strm;
    strm << repo_id;
    const int priority = problem.Kind == TSeqRangeProblem::Nested ? LOG_WARNING : LOG_ERR;
    syslog(priority, "open check: repo %s: %s%s", strm.str().c_str(), problem.Describe().c_str(),
           problem.Kind == TSeqRangeProblem::Nested ? "; a merge's leftover input, which reloading the repo drops" : "");
  }
  if (!check.Unheld.empty()) {
    syslog(LOG_CRIT, "open check: %ld blocks are owned by a live file, base image or append log but are FREE or waiting "
           "for discard; a new file could be written over them. Blocks: %s",
           check.Unheld.size(), ListBlocks(check.Unheld).c_str());
  }
  if (!check.Shared.empty()) {
    syslog(LOG_CRIT, "open check: %ld blocks are owned twice; freeing one owner would free the other's. Blocks: %s",
           check.Shared.size(), ListBlocks(check.Shared).c_str());
  }
  if (!check.IsSafe()) {
    ostringstream msg;
    msg << "open check failed: " << check.Unheld.size() << " blocks owned but not held, " << check.Shared.size()
        << " owned twice; continuing would let new files overwrite live data (see the log; --open_check=false skips this)";
    throw TOpenCheckFailed(msg.str());
  }
}
