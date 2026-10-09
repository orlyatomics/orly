/* <orly/indy/disk/integrity_scrub.test.cc>

   Unit test for <orly/indy/disk/integrity_scrub.h> (#748).

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

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <base/scheduler.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/disk_test.h>
#include <orly/indy/disk/durable_manager.h>
#include <orly/indy/disk/file_service.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/fiber/fiber_test_runner.h>
#include <orly/indy/transaction_base.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;
namespace DUtil = Orly::Indy::Disk::Util;

static const size_t BlockSize = Disk::Util::PhysicalBlockSize;

Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::Pool(sizeof(Disk::TDurableManager::TMapping), "Durable Mapping", 1000UL);
Orly::Indy::Util::TLocklessPool Disk::TDurableManager::TMapping::TEntry::Pool(sizeof(Disk::TDurableManager::TMapping::TEntry), "Durable Mapping Entry", 10000UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TDurableLayer::Pool(std::max(sizeof(Disk::TDurableManager::TMemSlushLayer), sizeof(Disk::TDurableManager::TDiskOrderedLayer)), "Durable Layer", 2000UL);
Orly::Indy::Util::TPool Disk::TDurableManager::TMemSlushLayer::TDurableEntry::Pool(sizeof(Disk::TDurableManager::TMemSlushLayer::TDurableEntry), "Durable Entry", 10000UL);

Disk::TBufBlock::TPool Disk::TBufBlock::Pool(BlockSize, 2000UL);

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(L0::TManager::TRepo::TMapping), "Repo Mapping", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(L0::TManager::TRepo::TMapping::TEntry), "Repo Mapping Entry", 100UL);
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer", 100UL);
Orly::Indy::Util::TPool L1::TTransaction::TMutation::Pool(std::max(std::max(sizeof(L1::TTransaction::TPusher), sizeof(L1::TTransaction::TPopper)), sizeof(L1::TTransaction::TStatusChanger)), "Transaction::TMutation", 100UL);
Orly::Indy::Util::TPool L1::TTransaction::Pool(sizeof(L1::TTransaction), "Transaction", 100UL);
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 750010UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 1500020UL);

FIXTURE(ReportSemantics) {
  /* Clean report */ {
    TScrubReport report;
    EXPECT_TRUE(report.IsClean());
    EXPECT_TRUE(report.IsSafe());
    EXPECT_FALSE(report.HasFatalCorruption());
    string desc = report.Describe();
    EXPECT_TRUE(desc.find("RESULT: ok") != string::npos);
    report.Report();
  }

  /* Non-fatal problem: leaked blocks */ {
    TScrubReport report;
    report.OpenCheck.Leaked.push_back(42UL);
    EXPECT_FALSE(report.IsClean());
    EXPECT_TRUE(report.IsSafe());
    EXPECT_FALSE(report.HasFatalCorruption());
    string desc = report.Describe();
    EXPECT_TRUE(desc.find("leaked: 1 blocks") != string::npos);
    EXPECT_TRUE(desc.find("RESULT: problems") != string::npos);
    report.Report();
  }

  /* Unsafe open check problem: unheld block owned by live file */ {
    TScrubReport report;
    report.OpenCheck.Unheld.push_back(10UL);
    EXPECT_FALSE(report.IsClean());
    EXPECT_FALSE(report.IsSafe());
    EXPECT_FALSE(report.HasFatalCorruption());
    string desc = report.Describe();
    EXPECT_TRUE(desc.find("unheld: 1 blocks") != string::npos);
    EXPECT_TRUE(desc.find("RESULT: UNSAFE") != string::npos);
    report.Report();
  }

  /* Fatal corruption: BadChecksum */ {
    TScrubReport report;
    report.Problems.push_back({TScrubProblem::BadChecksum, TUuid(TUuid::Twister), 1UL, "checksum mismatch"});
    EXPECT_FALSE(report.IsClean());
    EXPECT_FALSE(report.IsSafe());
    EXPECT_TRUE(report.HasFatalCorruption());
    string desc = report.Describe();
    EXPECT_TRUE(desc.find("bad checksum") != string::npos);
    EXPECT_TRUE(desc.find("RESULT: UNSAFE") != string::npos);
  }

  /* Fatal corruption: KeyOrderViolation */ {
    TScrubReport report;
    report.Problems.push_back({TScrubProblem::KeyOrderViolation, TUuid(TUuid::Twister), 1UL, "keys not in order"});
    EXPECT_FALSE(report.IsClean());
    EXPECT_FALSE(report.IsSafe());
    EXPECT_TRUE(report.HasFatalCorruption());
    string desc = report.Describe();
    EXPECT_TRUE(desc.find("key order violation") != string::npos);
    EXPECT_TRUE(desc.find("RESULT: UNSAFE") != string::npos);
  }

  /* Fatal corruption: CorruptPage */ {
    TScrubReport report;
    report.Problems.push_back({TScrubProblem::CorruptPage, TUuid(TUuid::Twister), 1UL, "corrupted page"});
    EXPECT_FALSE(report.IsClean());
    EXPECT_FALSE(report.IsSafe());
    EXPECT_TRUE(report.HasFatalCorruption());
  }

  /* Fatal corruption: FormatError */ {
    TScrubReport report;
    report.Problems.push_back({TScrubProblem::FormatError, TUuid(TUuid::Twister), 1UL, "invalid header"});
    EXPECT_FALSE(report.IsClean());
    EXPECT_FALSE(report.IsSafe());
    EXPECT_TRUE(report.HasFatalCorruption());
  }

  /* Problem kind names and formatting */ {
    TScrubProblem prob{TScrubProblem::KeyOrderViolation, TUuid(TUuid::Twister), 2UL, "details here"};
    EXPECT_EQ(string(prob.GetKindName()), "key order violation");
    string pdesc = prob.Describe();
    EXPECT_TRUE(pdesc.find("key order violation: file") != string::npos);
    EXPECT_TRUE(pdesc.find("gen [2]: details here") != string::npos);
  }

  /* max_problems truncation */ {
    TScrubReport report;
    for (size_t i = 0; i < 15; ++i) {
      report.Problems.push_back({TScrubProblem::UnreadableFile, nullopt, nullopt, "file cannot be read"});
    }
    string desc = report.Describe(5);
    EXPECT_TRUE(desc.find("... (10 more problems)") != string::npos);
  }
}

namespace {

  class TDataFileBlockReader
      : public TReadFile<LogicalPageSize, LogicalBlockSize, PhysicalBlockSize, CheckedPage> {
    NO_COPY(TDataFileBlockReader);
    public:
    typedef TStream<LogicalPageSize, LogicalBlockSize, PhysicalBlockSize, CheckedPage, 0UL> TInStream;
    TDataFileBlockReader(TPageCache *page_cache, size_t gen_id, size_t starting_block_id, size_t starting_block_offset, size_t file_length)
        : TReadFile(HERE, Source::FileRemoval, page_cache, Base::TUuid(), RealTime, gen_id, starting_block_id, starting_block_offset, file_length) {}
    using TReadFile::GetStartingBlockOffset;
    using TReadFile::GetNumMetaBlocks;
    using TReadFile::GetNumSequentialBlockPairings;
  };

}  // namespace

FIXTURE(CleanDataFileScrub) {
  DUtil::TDiskController::TEvent::InitializeDiskEventPoolManager(1000UL);
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    auto *frame_pool_manager = Orly::Indy::Fiber::TFrame::LocalFramePool->GetPoolManager();

    auto device = make_unique<TMemoryDevice>(512, 512, 262144UL, true /* fsync */, true);
    TCacheCb cache_cb = [](TCacheInstr, const TOffset, void *, size_t) {};
    auto volume = make_unique<TVolume>(TVolume::TDesc{TVolume::TDesc::Striped, device->GetDesc(), TVolume::TDesc::Fast, 1UL, 1UL, 1024UL, 8UL, 0.85}, cache_cb, &scheduler);
    volume->AddDevice(device.get(), 0UL);
    auto vol_man = make_unique<TVolumeManager>(&scheduler);
    vol_man->AddNewVolume(volume.get());

    auto page_cache = make_unique<TPageCache>(vol_man.get(), 1024UL, 1UL);
    auto block_cache = make_unique<TBlockCache>(vol_man.get(), 256UL, 1UL);

    size_t img1 = 0UL, img2 = 0UL, log1 = 0UL;
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { img1 = r.first; });
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { img2 = r.first; });
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { log1 = r.first; });

    const TFileService::TFileInitCb no_init = [](TFileObj::TKind, const Base::TUuid &, size_t, size_t, size_t, size_t) {
      return true;
    };
    TFileService fs(&scheduler, runner_cons, frame_pool_manager, vol_man.get(), img1, img2, {log1}, no_init, true, false);
    auto engine = make_unique<TEngine>(vol_man.get(), page_cache.get(), block_cache.get(), &fs, false);

    Base::TUuid file_id(TUuid::Best);
    TSequenceNumber seq_num = 0U;
    TUuid int_idx(TUuid::Twister);
    TUuid str_idx(TUuid::Twister);

    /* Write data file with strictly ascending keys */ {
      TSuprena arena;
      TMockMem mem_layer;
      Insert(mem_layer, ++seq_num, int_idx, 10L, 1L);
      Insert(mem_layer, ++seq_num, int_idx, 20L, 2L);
      Insert(mem_layer, ++seq_num, int_idx, 30L, 3L);
      Insert(mem_layer, ++seq_num, str_idx, 100L, string("alpha"));
      Insert(mem_layer, ++seq_num, str_idx, 101L, string("beta"));
      Insert(mem_layer, ++seq_num, str_idx, 102L, string("gamma"));

      size_t data_gen_id = 1;
      TDataFile data_file(engine.get(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
      EXPECT_EQ(data_file.GetNumKeys(), 6UL);
    }

    auto for_each_range = [&](const Base::TUuid &, const TFileObj &file, const function<void (const pair<size_t, size_t> &)> &cb) {
      if (file.Kind == TFileObj::DataFile && file.FileSize > 0) {
        TDataFileBlockReader reader(page_cache.get(), file.GenId, file.StartingBlockId, file.StartingBlockOffset, file.FileSize);
        TDataFileBlockReader::TInStream in_stream(HERE, Source::System, RealTime, &reader, page_cache.get(),
                                                  (reader.GetStartingBlockOffset() * LogicalBlockSize) + (TData::NumMetaFields * sizeof(size_t)));
        size_t block_id;
        for (size_t i = 0UL; i < reader.GetNumMetaBlocks(); ++i) {
          in_stream.Read(block_id);
          cb(make_pair(block_id, 1UL));
        }
        size_t num_contig_blocks;
        for (size_t i = 0UL; i < reader.GetNumSequentialBlockPairings(); ++i) {
          in_stream.Read(block_id);
          in_stream.Read(num_contig_blocks);
          cb(make_pair(block_id, num_contig_blocks));
        }
      }
    };

    /* Run integrity scrub */ {
      size_t yield_count = 0UL;
      TScrubOptions options;
      options.Priority = RealTime;
      options.YieldEveryNKeys = 2UL;

      auto yield_cb = [&yield_count]() {
        ++yield_count;
      };

      TScrubReport report = RunIntegrityScrub(
          vol_man.get(),
          &fs,
          page_cache.get(),
          {},
          for_each_range,
          options,
          yield_cb);

      EXPECT_TRUE(report.IsClean());
      EXPECT_TRUE(report.IsSafe());
      EXPECT_FALSE(report.HasFatalCorruption());
      EXPECT_EQ(report.NumDataFiles, 1UL);
      EXPECT_EQ(report.NumKeysScrubbed, 6UL);
      EXPECT_GE(yield_count, 3UL);
      EXPECT_TRUE(report.Problems.empty());
      EXPECT_GT(report.Seconds, 0.0);
      string desc = report.Describe();
      EXPECT_TRUE(desc.find("RESULT: ok") != string::npos);
      report.Report();
    }

    /* Sentinel cleanup */ {
      const Base::TUuid sentinel_uid(TUuid::Twister);
      TCompletionTrigger trig;
      fs.InsertFile(sentinel_uid, TFileObj::DataFile, 2UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, trig);
      trig.Wait();
      fs.RemoveFile(sentinel_uid, 2UL, trig);
      trig.Wait();
    }

    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 4);
  DUtil::TDiskController::TEvent::FinalizeDiskEventPoolManager();
}

FIXTURE(BlockAccountingIntegration) {
  DUtil::TDiskController::TEvent::InitializeDiskEventPoolManager(1000UL);
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &runner_cons) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    auto *frame_pool_manager = Orly::Indy::Fiber::TFrame::LocalFramePool->GetPoolManager();

    auto device = make_unique<TMemoryDevice>(512, 512, 65536UL, true /* fsync */, true);
    TCacheCb cache_cb = [](TCacheInstr, const TOffset, void *, size_t) {};
    auto volume = make_unique<TVolume>(TVolume::TDesc{TVolume::TDesc::Striped, device->GetDesc(), TVolume::TDesc::Fast, 1UL, 1UL, 1024UL, 8UL, 0.85}, cache_cb, &scheduler);
    volume->AddDevice(device.get(), 0UL);
    auto vol_man = make_unique<TVolumeManager>(&scheduler);
    vol_man->AddNewVolume(volume.get());

    size_t img1 = 0UL, img2 = 0UL, log1 = 0UL;
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { img1 = r.first; });
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { img2 = r.first; });
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { log1 = r.first; });

    const TFileService::TFileInitCb no_init = [](TFileObj::TKind, const Base::TUuid &, size_t, size_t, size_t, size_t) {
      return true;
    };
    TFileService fs(&scheduler, runner_cons, frame_pool_manager, vol_man.get(), img1, img2, {log1}, no_init, true, false);

    const Base::TUuid repo_id("5B1E7C2A-0D4F-4E8B-9A36-2C7D1F0E8B41");
    size_t file_block = 0UL;
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 2UL, [&](const TBlockRange &r) { file_block = r.first; });

    TCompletionTrigger trigger;
    fs.InsertFile(repo_id, TFileObj::DataFile, 1UL, file_block, 0UL, 2UL, 1UL, 1UL, 100UL, trigger);
    trigger.Wait();

    auto for_each_range = [](const Base::TUuid &, const TFileObj &file, const function<void (const pair<size_t, size_t> &)> &cb) {
      if (file.FileSize) {
        cb(make_pair(file.StartingBlockId, file.FileSize));
      }
    };

    /* Clean run without content decoding */ {
      TScrubOptions options;
      options.CheckContents = false;
      TScrubReport report = RunIntegrityScrub(vol_man.get(), &fs, nullptr, {}, for_each_range, options);
      EXPECT_TRUE(report.IsClean());
      EXPECT_TRUE(report.IsSafe());
      EXPECT_FALSE(report.HasFatalCorruption());
      EXPECT_EQ(report.NumFiles, 1UL);
      EXPECT_TRUE(report.Problems.empty());
    }

    /* Inject leaked block: mark block used without file claiming it */ {
      size_t leaked_block = 0UL;
      vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1UL, [&](const TBlockRange &r) { leaked_block = r.first; });
      TScrubOptions options;
      options.CheckContents = false;
      TScrubReport report = RunIntegrityScrub(vol_man.get(), &fs, nullptr, {}, for_each_range, options);
      EXPECT_FALSE(report.IsClean());
      EXPECT_TRUE(report.IsSafe());
      EXPECT_FALSE(report.HasFatalCorruption());
      EXPECT_EQ(report.OpenCheck.Leaked.size(), 1UL);
      string desc = report.Describe();
      EXPECT_TRUE(desc.find("RESULT: problems") != string::npos);
    }

    /* Sentinel cleanup */ {
      const Base::TUuid sentinel_uid(TUuid::Twister);
      TCompletionTrigger trig;
      fs.InsertFile(sentinel_uid, TFileObj::DataFile, 2UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, trig);
      trig.Wait();
      fs.RemoveFile(sentinel_uid, 2UL, trig);
      trig.Wait();
    }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  }, 4);
  DUtil::TDiskController::TEvent::FinalizeDiskEventPoolManager();
}

