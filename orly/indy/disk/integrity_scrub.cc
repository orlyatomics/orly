/* <orly/indy/disk/integrity_scrub.cc>

   Implements <orly/indy/disk/integrity_scrub.h>.

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

#include <orly/indy/disk/integrity_scrub.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <syslog.h>

#include <orly/indy/disk/durable_manager.h>
#include <orly/indy/disk/file_service.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/util/volume_manager.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/key.h>
#include <orly/sabot/all.h>

using namespace std;
using namespace Orly;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Sabot;
namespace DUtil = Orly::Indy::Disk::Util;

const char *TScrubProblem::GetKindName() const {
  switch (Kind) {
    case BlockAccounting:   return "block accounting";
    case SeqRange:          return "sequence range";
    case UnreadableFile:    return "unreadable file";
    case CorruptPage:       return "corrupt page";
    case BadChecksum:       return "bad checksum";
    case KeyOrderViolation: return "key order violation";
    case FormatError:       return "format error";
  }
  return "unknown";
}

string TScrubProblem::Describe() const {
  ostringstream strm;
  strm << GetKindName() << ": ";
  if (FileUid) {
    strm << "file " << *FileUid;
    if (GenId) {
      strm << " gen [" << *GenId << "]";
    }
    strm << ": ";
  }
  strm << Details;
  return strm.str();
}

bool TScrubReport::HasFatalCorruption() const {
  for (const auto &p : Problems) {
    if (p.Kind == TScrubProblem::CorruptPage ||
        p.Kind == TScrubProblem::BadChecksum ||
        p.Kind == TScrubProblem::KeyOrderViolation ||
        p.Kind == TScrubProblem::FormatError) {
      return true;
    }
  }
  return false;
}

bool TScrubReport::IsSafe() const {
  return OpenCheck.IsSafe() && !HasFatalCorruption();
}

bool TScrubReport::IsClean() const {
  return OpenCheck.IsClean() && Problems.empty();
}

string TScrubReport::Describe(size_t max_problems) const {
  ostringstream strm;
  strm << "integrity scrub: " << NumFiles << " files (" << NumDataFiles << " data, "
       << NumDurableFiles << " durable), " << NumKeysScrubbed << " keys, "
       << NumHistoryKeysScrubbed << " history keys, "
       << NumDurableEntriesScrubbed << " durable entries, "
       << NumBlocksScrubbed << " blocks, in "
       << fixed << setprecision(3) << Seconds * 1000.0 << " ms\n";

  if (!OpenCheck.Leaked.empty()) {
    strm << "leaked: " << OpenCheck.Leaked.size() << " blocks held but owned by nothing\n";
  }
  if (!OpenCheck.Unheld.empty()) {
    strm << "unheld: " << OpenCheck.Unheld.size() << " blocks owned by a live file but free or waiting for discard\n";
  }
  if (!OpenCheck.Shared.empty()) {
    strm << "shared: " << OpenCheck.Shared.size() << " blocks are owned twice\n";
  }
  if (!OpenCheck.OutOfRange.empty()) {
    strm << "out of range: " << OpenCheck.OutOfRange.size() << " blocks outside every volume\n";
  }
  for (const auto &[repo_id, problem] : OpenCheck.SeqProblems) {
    strm << "sequence range: repo " << repo_id << ": " << problem.Describe() << "\n";
  }
  for (size_t i = 0UL; i < Problems.size() && i < max_problems; ++i) {
    strm << "problem: " << Problems[i].Describe() << "\n";
  }
  if (Problems.size() > max_problems) {
    strm << "... (" << (Problems.size() - max_problems) << " more problems)\n";
  }
  strm << "RESULT: " << (IsClean() ? "ok" : IsSafe() ? "problems" : "UNSAFE") << "\n";
  return strm.str();
}

void TScrubReport::Report() const {
  ostringstream summary;
  summary << "integrity scrub: " << NumFiles << " files (" << NumDataFiles << " data, "
          << NumDurableFiles << " durable), " << NumKeysScrubbed << " keys, "
          << NumDurableEntriesScrubbed << " durable entries, "
          << NumBlocksScrubbed << " blocks, in "
          << fixed << setprecision(3) << Seconds * 1000.0 << " ms";
  if (IsClean()) {
    syslog(LOG_INFO, "%s; consistent", summary.str().c_str());
    return;
  }
  syslog(LOG_ERR, "%s; PROBLEMS FOUND (#748)", summary.str().c_str());
  for (const auto &problem : Problems) {
    syslog(LOG_ERR, "integrity scrub: %s", problem.Describe().c_str());
  }
}

namespace {

  using TFileVec = vector<pair<Base::TUuid, TFileObj>>;

  TFileVec SnapshotFiles(TFileService *file_service) {
    TFileVec files;
    file_service->ForEachFile([&files](const Base::TUuid &file_uid, const TFileObj &file) {
      files.emplace_back(file_uid, file);
      return true;
    });
    return files;
  }

  bool FileStillInMap(TFileService *file_service, const Base::TUuid &file_uid, size_t gen_id) {
    size_t block_id, block_offset, file_size, num_keys;
    return file_service->FindFile(file_uid, gen_id, block_id, block_offset, file_size, num_keys);
  }

  class TScrubDataFileReader
      : public TReadFile<DUtil::LogicalPageSize, DUtil::LogicalBlockSize, DUtil::PhysicalBlockSize, DUtil::CheckedPage> {
    NO_COPY(TScrubDataFileReader);
    public:

    using TArena = TDiskArena<DUtil::LogicalPageSize, DUtil::LogicalBlockSize, DUtil::PhysicalBlockSize, DUtil::CheckedPage, 128, true>;

    TScrubDataFileReader(DUtil::TPageCache *page_cache,
                         const Base::TUuid &file_id,
                         DiskPriority priority,
                         size_t gen_id,
                         size_t starting_block_id,
                         size_t starting_block_offset,
                         size_t file_length)
        : TReadFile(HERE,
                    Source::System,
                    page_cache,
                    file_id,
                    priority,
                    gen_id,
                    starting_block_id,
                    starting_block_offset,
                    file_length) {}

    using TReadFile::GetIndexByIdMap;
    using TReadFile::TIndexFile;

  };  // TScrubDataFileReader

  void ScrubDataFile(const Base::TUuid &file_uid,
                     const TFileObj &file,
                     DUtil::TPageCache *page_cache,
                     TFileService *file_service,
                     const TScrubOptions &options,
                     const function<void()> &do_yield,
                     TScrubReport &report) {
    try {
      TScrubDataFileReader reader(page_cache,
                                  file_uid,
                                  options.Priority,
                                  file.GenId,
                                  file.StartingBlockId,
                                  file.StartingBlockOffset,
                                  file.FileSize);

      auto main_arena = make_unique<TScrubDataFileReader::TArena>(&reader, page_cache, options.Priority);
      const auto &index_map = reader.GetIndexByIdMap();

      for (const auto &idx_pair : index_map) {
        const Base::TUuid &index_id = idx_pair.first;
        const auto &idx_file = idx_pair.second;
        auto index_arena = make_unique<TScrubDataFileReader::TArena>(idx_file.get(), page_cache, options.Priority);

        TKey prev_key;
        bool has_prev = false;

        for (TScrubDataFileReader::TIndexFile::TKeyCursor csr(idx_file.get()); csr; ++csr) {
          const auto &item = *csr;
          if (!item.Key.IsTuple()) {
            report.Problems.push_back({TScrubProblem::FormatError, file_uid, file.GenId,
                                       "current key is not a tuple"});
            break;
          }

          TKey cur_key(item.Key, index_arena.get());
          void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
          (void)cur_key.GetState(state_alloc);
          TKey cur_val(item.Value, main_arena.get());
          (void)cur_val.GetState(state_alloc);

          if (has_prev) {
            if (!(prev_key < cur_key)) {
              ostringstream msg;
              msg << "current keys not in strictly ascending order in index " << index_id;
              report.Problems.push_back({TScrubProblem::KeyOrderViolation, file_uid, file.GenId, msg.str()});
              break;
            }
          }
          prev_key = cur_key;
          has_prev = true;
          ++report.NumKeysScrubbed;

          if (options.YieldEveryNKeys && (report.NumKeysScrubbed % options.YieldEveryNKeys == 0)) {
            do_yield();
          }
        }

        for (TScrubDataFileReader::TIndexFile::THistoryKeyCursor csr(idx_file.get()); csr; ++csr) {
          const auto &item = *csr;
          if (!item.Key.IsTuple()) {
            report.Problems.push_back({TScrubProblem::FormatError, file_uid, file.GenId,
                                       "history key is not a tuple"});
            break;
          }

          TKey cur_key(item.Key, index_arena.get());
          void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
          (void)cur_key.GetState(state_alloc);
          TKey cur_val(item.Value, main_arena.get());
          (void)cur_val.GetState(state_alloc);

          ++report.NumHistoryKeysScrubbed;

          if (options.YieldEveryNKeys && (report.NumHistoryKeysScrubbed % options.YieldEveryNKeys == 0)) {
            do_yield();
          }
        }
      }

      ++report.NumDataFiles;
      report.NumBlocksScrubbed += file.FileSize;
    } catch (const exception &ex) {
      if (FileStillInMap(file_service, file_uid, file.GenId)) {
        string err = ex.what();
        TScrubProblem::TKind kind =
            (err.find("corrupt") != string::npos || err.find("checksum") != string::npos)
            ? TScrubProblem::BadChecksum : TScrubProblem::CorruptPage;
        report.Problems.push_back({kind, file_uid, file.GenId, err});
      }
    }
  }

  void ScrubDurableFile(const Base::TUuid &file_uid,
                        const TFileObj &file,
                        DUtil::TPageCache *page_cache,
                        TFileService *file_service,
                        const TScrubOptions &options,
                        const function<void()> &do_yield,
                        TScrubReport &report) {
    try {
      TDurableManager::TSortedInFile sorted_in_file(page_cache,
                                                    options.Priority,
                                                    file.GenId,
                                                    file.StartingBlockId,
                                                    file.StartingBlockOffset,
                                                    file.FileSize);
      const size_t num_entries = sorted_in_file.GetNumEntries();
      if (num_entries > 0) {
        using TDurableInStream = TStream<DUtil::LogicalPageSize, DUtil::LogicalBlockSize,
                                         DUtil::PhysicalBlockSize, DUtil::CheckedPage, 0UL>;
        TDurableInStream entry_stream(HERE, Source::DurableFetch, options.Priority,
                                      &sorted_in_file, page_cache,
                                      sorted_in_file.GetStartOfDurableByIdIndex());
        for (size_t i = 0; i < num_entries; ++i) {
          uuid_t cur_id;
          TSequenceNumber cur_seq;
          size_t cur_deadline;
          TDurableManager::TSerializedSize cur_sz;
          entry_stream.Read(&cur_id, sizeof(uuid_t));
          entry_stream.Read(&cur_seq, sizeof(TSequenceNumber));
          entry_stream.Read(cur_deadline);
          entry_stream.Read(cur_sz);
          entry_stream.Skip(cur_sz);
          ++report.NumDurableEntriesScrubbed;

          if (options.YieldEveryNKeys && (report.NumDurableEntriesScrubbed % options.YieldEveryNKeys == 0)) {
            do_yield();
          }
        }
      }
      ++report.NumDurableFiles;
      report.NumBlocksScrubbed += file.FileSize;
    } catch (const exception &ex) {
      if (FileStillInMap(file_service, file_uid, file.GenId)) {
        report.Problems.push_back({TScrubProblem::CorruptPage, file_uid, file.GenId, ex.what()});
      }
    }
  }

}  // namespace

TScrubReport Orly::Indy::Disk::RunIntegrityScrub(DUtil::TVolumeManager *vol_man,
                                                 TFileService *file_service,
                                                 DUtil::TPageCache *page_cache,
                                                 const vector<size_t> &engine_blocks,
                                                 const TForEachFileBlockRange &for_each_file_block_range,
                                                 const TScrubOptions &options,
                                                 const function<void()> &yield_cb) {
  const auto start = chrono::steady_clock::now();
  TScrubReport report;

  const auto do_yield = [&yield_cb]() {
    if (yield_cb) {
      yield_cb();
    } else if (Fiber::TFrame::LocalFrame) {
      Fiber::Yield();
    }
  };

  /* Step 1: Block accounting and sequence ranges (#700). */
  if (options.CheckBlockAccounting) {
    report.OpenCheck = CheckOpenConsistency(vol_man, file_service, engine_blocks, for_each_file_block_range);
    for (const auto &unreadable : report.OpenCheck.Unreadable) {
      report.Problems.push_back({TScrubProblem::UnreadableFile, nullopt, nullopt, unreadable});
    }
    for (const auto &[repo_id, prob] : report.OpenCheck.SeqProblems) {
      report.Problems.push_back({TScrubProblem::SeqRange, repo_id, prob.GenId, prob.Describe()});
    }
    if (!report.OpenCheck.Unheld.empty()) {
      ostringstream msg;
      msg << report.OpenCheck.Unheld.size() << " unheld blocks (owned by live file but free or waiting for discard)";
      report.Problems.push_back({TScrubProblem::BlockAccounting, nullopt, nullopt, msg.str()});
    }
    if (!report.OpenCheck.Shared.empty()) {
      ostringstream msg;
      msg << report.OpenCheck.Shared.size() << " shared blocks (owned twice)";
      report.Problems.push_back({TScrubProblem::BlockAccounting, nullopt, nullopt, msg.str()});
    }
    if (!report.OpenCheck.OutOfRange.empty()) {
      ostringstream msg;
      msg << report.OpenCheck.OutOfRange.size() << " blocks outside every volume";
      report.Problems.push_back({TScrubProblem::BlockAccounting, nullopt, nullopt, msg.str()});
    }
  }

  /* Step 2: Content decoding & key ordering checks. */
  if (options.CheckContents && page_cache) {
    const TFileVec files = SnapshotFiles(file_service);
    report.NumFiles = files.size();

    for (const auto &[file_uid, file] : files) {
      if (!FileStillInMap(file_service, file_uid, file.GenId)) {
        continue;
      }
      switch (file.Kind) {
        case TFileObj::DataFile: {
          ScrubDataFile(file_uid, file, page_cache, file_service, options, do_yield, report);
          break;
        }
        case TFileObj::DurableFile: {
          ScrubDurableFile(file_uid, file, page_cache, file_service, options, do_yield, report);
          break;
        }
      }
      do_yield();
    }
  } else {
    report.NumFiles = report.OpenCheck.NumFiles;
  }

  report.Seconds = chrono::duration<double>(chrono::steady_clock::now() - start).count();
  return report;
}
