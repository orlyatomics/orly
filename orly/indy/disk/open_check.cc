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
#include <optional>
#include <limits>
#include <set>
#include <sstream>

#include <cassert>
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

  /* Block ids are logical addresses, sparse across the volumes' extents. This maps them to a
     dense index, so a set of them is a bit per block the volumes actually have. */
  class TBlockIndex {
    public:

    explicit TBlockIndex(vector<pair<size_t, size_t>> extents)
        : Extents(std::move(extents)) {
      sort(Extents.begin(), Extents.end());
      for (const auto &extent : Extents) {
        Bases.push_back(Size);
        Size += extent.second;
      }
    }

    /* The dense index of a block, or nothing for a block outside every extent. */
    optional<size_t> Find(size_t block_id) const {
      auto iter = upper_bound(Extents.begin(), Extents.end(), make_pair(block_id, numeric_limits<size_t>::max()));
      if (iter == Extents.begin()) {
        return nullopt;
      }
      --iter;
      if (block_id - iter->first >= iter->second) {
        return nullopt;
      }
      return Bases[iter - Extents.begin()] + (block_id - iter->first);
    }

    /* The block id at a dense index. */
    size_t GetBlockId(size_t index) const {
      auto iter = upper_bound(Bases.begin(), Bases.end(), index);
      assert(iter != Bases.begin());
      --iter;
      return Extents[iter - Bases.begin()].first + (index - *iter);
    }

    /* True if every block of the range lies in one extent. */
    bool Contains(const pair<size_t, size_t> &range) const {
      if (range.second > Size) {
        return false;
      }
      const auto first = Find(range.first);
      return range.second == 0UL || (first && Find(range.first + range.second - 1UL) == *first + range.second - 1UL);
    }

    size_t GetSize() const {
      return Size;
    }

    private:

    vector<pair<size_t, size_t>> Extents;

    vector<size_t> Bases;

    size_t Size = 0UL;

  };  // TBlockIndex

  /* A bit per block, by dense index. */
  class TBlockSet {
    public:

    explicit TBlockSet(size_t size)
        : Bits(size, false) {}

    /* Sets the bit; returns false if it was already set. */
    bool Insert(size_t index) {
      assert(index < Bits.size());
      if (Bits[index]) {
        return false;
      }
      Bits[index] = true;
      ++Size;
      return true;
    }

    bool Contains(size_t index) const {
      return Bits[index];
    }

    size_t GetSize() const {
      return Size;
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

  /* A file's block ranges, read in full before any is used. Throws if a range lies outside
     the volumes or the list is longer than they are: the walk must never loop over a block
     list that a file's metadata gets wrong. */
  vector<pair<size_t, size_t>> ReadRanges(const TForEachFileBlockRange &for_each_file_block_range,
                                          const Base::TUuid &file_uid, const TFileObj &file, const TBlockIndex &index) {
    vector<pair<size_t, size_t>> ranges;
    size_t total = 0UL;
    for_each_file_block_range(file_uid, file, [&](const pair<size_t, size_t> &range) {
      if (!index.Contains(range) || (total += range.second) > index.GetSize()) {
        ostringstream msg;
        msg << "block range [" << range.first << ", +" << range.second << "] lies outside the volumes";
        throw runtime_error(msg.str());
      }
      ranges.push_back(range);
    });
    return ranges;
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
  const TBlockIndex index(vol_man->GetBlockExtents());
  TBlockSet owned(index.GetSize());
  set<size_t> shared;
  auto own = [&](size_t block_id) {
    const auto pos = index.Find(block_id);
    if (!pos) {
      check.OutOfRange.push_back(block_id);
    } else if (!owned.Insert(*pos)) {
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
    /* A file that leaves the map while we read it may have had its blocks freed and reused,
       so what we read may be anything: count it only if it is still in the map after. */
    try {
      const auto ranges = ReadRanges(for_each_file_block_range, file_uid, file, index);
      if (IsInMap(file_service, file_uid, file.GenId)) {
        for (const auto &range : ranges) {
          for (size_t i = 0UL; i < range.second; ++i) {
            own(range.first + i);
          }
        }
      }
    } catch (const exception &ex) {
      if (IsInMap(file_service, file_uid, file.GenId)) {
        check.Unreadable.push_back(DescribeFile(file_uid, file) + ": " + ex.what());
      }
    }
    if (file.Kind == TFileObj::DataFile) {
      data_files_by_repo[file_uid].push_back(file);
    }
  }
  check.NumOwned = owned.GetSize();
  TBlockSet held(index.GetSize());
  vol_man->ForEachHeldBlock([&](size_t block_id) {
    if (const auto pos = index.Find(block_id)) {
      held.Insert(*pos);
    }
  });
  check.NumHeld = held.GetSize();
  vector<size_t> unheld, leaked;
  for (size_t pos = 0UL; pos < index.GetSize(); ++pos) {
    const bool is_owned = owned.Contains(pos), is_held = held.Contains(pos);
    if (is_owned && !is_held) {
      unheld.push_back(index.GetBlockId(pos));
    } else if (is_held && !is_owned) {
      leaked.push_back(index.GetBlockId(pos));
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
        for (const auto &range : ReadRanges(for_each_file_block_range, file_uid, file, index)) {
          for (size_t i = 0UL; i < range.second; ++i) {
            if (candidates.count(range.first + i)) {
              confirmed.insert(range.first + i);
            }
          }
        }
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
        for (const auto &range : ReadRanges(for_each_file_block_range, file_uid, file, index)) {
          for (size_t i = 0UL; i < range.second; ++i) {
            candidates.erase(range.first + i);
          }
        }
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
  if (!check.OutOfRange.empty()) {
    syslog(LOG_CRIT, "open check: %ld blocks owned by the system block or the file service lie outside every volume. Blocks: %s",
           check.OutOfRange.size(), ListBlocks(check.OutOfRange).c_str());
  }
  if (!check.Shared.empty()) {
    syslog(LOG_CRIT, "open check: %ld blocks are owned twice; freeing one owner would free the other's. Blocks: %s",
           check.Shared.size(), ListBlocks(check.Shared).c_str());
  }
  if (!check.IsSafe()) {
    ostringstream msg;
    msg << "open check failed: " << check.Unheld.size() << " blocks owned but not held, " << check.Shared.size()
        << " owned twice, " << check.OutOfRange.size() << " outside every volume; continuing would let new files overwrite live data (see the log; --open_check=false skips this)";
    throw TOpenCheckFailed(msg.str());
  }
}

std::string Orly::Indy::Disk::DescribeOpenCheck(const TOpenCheck &check, size_t max_blocks) {
  ostringstream strm;
  strm << "open check: " << check.NumFiles << " files, " << check.NumOwned << " blocks owned, " << check.NumHeld
       << " held, in " << fixed << setprecision(3) << check.Seconds * 1000.0 << " ms\n";
  for (const auto &line : check.Unreadable) {
    strm << "unreadable: the block list of " << line << " can't be read\n";
  }
  if (!check.Leaked.empty()) {
    strm << "leaked: " << check.Leaked.size() << " blocks held but owned by nothing: " << ListBlocks(check.Leaked, max_blocks)
         << (check.MergeRunning ? " (a disk merge was running)" : "") << "\n";
  }
  for (const auto &[repo_id, problem] : check.SeqProblems) {
    strm << "sequence range: repo " << repo_id << ": " << problem.Describe() << "\n";
  }
  if (!check.Unheld.empty()) {
    strm << "unheld: " << check.Unheld.size() << " blocks owned by a live file but free or waiting for discard: "
         << ListBlocks(check.Unheld, max_blocks) << "\n";
  }
  if (!check.OutOfRange.empty()) {
    strm << "out of range: " << check.OutOfRange.size() << " blocks owned by the system block or the file service lie outside every volume: "
         << ListBlocks(check.OutOfRange, max_blocks) << "\n";
  }
  if (!check.Shared.empty()) {
    strm << "shared: " << check.Shared.size() << " blocks are owned twice: " << ListBlocks(check.Shared, max_blocks) << "\n";
  }
  strm << "RESULT: " << (check.IsClean() ? "ok" : check.IsSafe() ? "problems" : "UNSAFE") << "\n";
  return strm.str();
}
