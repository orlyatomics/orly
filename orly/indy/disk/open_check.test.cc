/* <orly/indy/disk/open_check.test.cc>

   Unit test for <orly/indy/disk/open_check.h> (#700).

   Each case lays out a store on a memory device, with a real file service whose file map holds
   crafted files: a file's blocks are the FileSize blocks from StartingBlockId, so a test can say
   exactly which blocks a file owns without writing data files. The store is closed and reopened
   the way TDiskEngine reopens one (fixed blocks marked used, then every file's blocks), with
   corruption injected at a chosen step, and the check runs against the reopened store. The
   fault-injection harness (orly/indy/fault_injection.test.cc) runs the same check on real data
   and durable files after every simulated power loss.

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

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <base/scheduler.h>
#include <orly/indy/disk/file_service.h>
#include <orly/indy/disk/indy_util_reporter.h>
#include <orly/indy/fiber/fiber_test_runner.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Orly::Indy::Disk;

/* Base has a Util namespace too. */
namespace DUtil = Orly::Indy::Disk::Util;

TBufBlock::TPool TBufBlock::Pool(DUtil::PhysicalBlockSize, 100);

namespace {

  using TFramePoolManager = Base::TThreadLocalGlobalPoolManager<Orly::Indy::Fiber::TFrame, size_t, Orly::Indy::Fiber::TRunner *>;

  /* 2048 blocks of 64 KiB. */
  constexpr size_t NumLogicalBlocks = 262144UL;

  const Base::TUuid RepoA("5B1E7C2A-0D4F-4E8B-9A36-2C7D1F0E8B41");
  const Base::TUuid RepoB("9C2F4A7E-1B3D-4F60-8E25-7A0C3D9B6E12");

  /* One volume and its allocator over a device that outlives them. */
  class TBoot {
    NO_COPY(TBoot);
    public:

    TBoot(Base::TScheduler *scheduler, DUtil::TMemoryDevice *device) {
      DUtil::TCacheCb cache_cb = [](DUtil::TCacheInstr, const DUtil::TOffset, void *, size_t) {};
      Volume = make_unique<DUtil::TVolume>(DUtil::TVolume::TDesc{DUtil::TVolume::TDesc::Striped, device->GetDesc(), DUtil::TVolume::TDesc::Fast, 1UL, 1UL, 1024UL, 8UL, 0.85}, cache_cb, scheduler);
      Volume->AddDevice(device, 0UL);
      VolMan = make_unique<DUtil::TVolumeManager>(scheduler);
      VolMan->AddNewVolume(Volume.get());
    }

    ~TBoot() {
      VolMan.reset();
      Volume.reset();
    }

    DUtil::TVolumeManager *GetVolMan() const {
      return VolMan.get();
    }

    private:

    unique_ptr<DUtil::TVolume> Volume;
    unique_ptr<DUtil::TVolumeManager> VolMan;

  };  // TBoot

  /* A file to put in the map: its repo, sequence range and number of blocks. */
  struct TSpec {
    Base::TUuid Repo;
    size_t Lowest;
    size_t Highest;
    size_t NumBlocks;
  };

  /* A file as laid out: its spec, gen id and first block. */
  struct TLaidOut {
    TSpec Spec;
    size_t GenId;
    size_t FirstBlock;
  };

  /* Where the file service's fixed blocks live. */
  struct TLayout {
    size_t Image1BlockId;
    size_t Image2BlockId;
    vector<size_t> AppendLogBlockVec;
    vector<TLaidOut> Files;
  };

  size_t AllocRange(DUtil::TVolumeManager *vol_man, size_t num_blocks) {
    size_t first = 0UL, got = 0UL;
    vol_man->TryAllocateSequentialBlocks(DUtil::TVolume::TDesc::TStorageSpeed::Fast, num_blocks, [&](const DUtil::TBlockRange &range) {
      if (!got) {
        first = range.first;
      }
      if (range.first != first + got) {
        throw logic_error("not contiguous");
      }
      got += range.second;
    });
    return first;
  }

  /* The walker the check uses: a crafted file's blocks are FileSize blocks from its start. */
  void ForEachCraftedRange(const Base::TUuid &, const TFileObj &file, const function<void (const pair<size_t, size_t> &)> &cb) {
    if (file.FileSize) {
      cb(make_pair(file.StartingBlockId, file.FileSize));
    }
  }

  /* What a test can change about the reopen. */
  struct TCorruption {

    /* Runs on the first boot, after the files are in the map; may change the layout. */
    function<void (TFileService &, DUtil::TVolumeManager *, TLayout &)> BeforeClose;

    /* Whether the reopen's startup walk marks a file's blocks used, as TDiskEngine's does. */
    function<bool (size_t gen_id)> MarkFile = [](size_t) { return true; };

    /* Runs on the reopened store before the check. */
    function<void (TFileService &, DUtil::TVolumeManager *, const TLayout &)> AfterOpen;

  };  // TCorruption

  /* Lay out 'specs' on a fresh store, close it, reopen it and run the check. 'expect' gets the
     result and the layout. Returns the reopen's wall time in seconds. */
  double RunReopen(const vector<TSpec> &specs, const TCorruption &corruption, const function<void (const TOpenCheck &, const TLayout &)> &expect) {
    double open_seconds = 0.0;
    DUtil::TDiskController::TEvent::InitializeDiskEventPoolManager(1000UL);
    Orly::Indy::Fiber::TFiberTestRunner runner([&](std::mutex &mut, std::condition_variable &cond, bool &fin, Orly::Indy::Fiber::TRunner::TRunnerCons &runner_cons) {
      /* scope */ {
        Base::TScheduler scheduler(Base::TScheduler::TPolicy(4, 4, milliseconds(10)));
        TFramePoolManager *frame_pool_manager = Orly::Indy::Fiber::TFrame::LocalFramePool->GetPoolManager();
        auto device = make_unique<DUtil::TMemoryDevice>(512, 512, NumLogicalBlocks, true /* fsync */, true);
        TLayout layout;
        const TFileService::TFileInitCb no_init = [](TFileObj::TKind, const Base::TUuid &, size_t, size_t, size_t, size_t) {
          return true;
        };
        /* first boot */ {
          TBoot boot(&scheduler, device.get());
          auto *vol_man = boot.GetVolMan();
          layout.Image1BlockId = AllocRange(vol_man, 1UL);
          layout.Image2BlockId = AllocRange(vol_man, 1UL);
          layout.AppendLogBlockVec.push_back(AllocRange(vol_man, 1UL));
          TFileService fs(&scheduler, runner_cons, frame_pool_manager, vol_man, layout.Image1BlockId, layout.Image2BlockId,
                          layout.AppendLogBlockVec, no_init, true, false);
          size_t gen_id = 0UL;
          for (const auto &spec : specs) {
            TLaidOut file{spec, ++gen_id, spec.NumBlocks ? AllocRange(vol_man, spec.NumBlocks) : 0UL};
            TCompletionTrigger trigger;
            fs.InsertFile(spec.Repo, TFileObj::DataFile, file.GenId, file.FirstBlock, 0UL, spec.NumBlocks, 1UL, spec.Lowest, spec.Highest, trigger);
            trigger.Wait();
            layout.Files.push_back(file);
          }
          if (corruption.BeforeClose) {
            corruption.BeforeClose(fs, vol_man, layout);
          }
        }
        /* reopen, as TDiskEngine does */ {
          const auto start = steady_clock::now();
          TBoot boot(&scheduler, device.get());
          auto *vol_man = boot.GetVolMan();
          vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(layout.Image1BlockId, 1UL));
          vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(layout.Image2BlockId, 1UL));
          for (size_t block_id : layout.AppendLogBlockVec) {
            vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(block_id, 1UL));
          }
          const TFileService::TFileInitCb init = [&](TFileObj::TKind, const Base::TUuid &, size_t gen_id, size_t starting_block_id, size_t, size_t file_length) {
            if (file_length && corruption.MarkFile(gen_id)) {
              /* A block another file already marked is skipped rather than fatal here, so a
                 test can build a store where two files claim one block. */
              for (size_t i = 0UL; i < file_length; ++i) {
                try {
                  vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(starting_block_id + i, 1UL));
                } catch (const exception &) {}
              }
            }
            return true;
          };
          TFileService fs(&scheduler, runner_cons, frame_pool_manager, vol_man, layout.Image1BlockId, layout.Image2BlockId,
                          layout.AppendLogBlockVec, init, false, false);
          if (corruption.AfterOpen) {
            corruption.AfterOpen(fs, vol_man, layout);
          }
          const auto check_start = steady_clock::now();
          const TOpenCheck check = CheckOpenConsistency(vol_man, &fs, {}, ForEachCraftedRange);
          const auto end = steady_clock::now();
          open_seconds = duration<double>(end - start).count();
          cout << "  reopen with " << check.NumFiles << " files: " << duration<double>(check_start - start).count() * 1000.0
               << " ms to open, " << duration<double>(end - check_start).count() * 1000.0 << " ms to check ("
               << check.NumOwned << " owned, " << check.NumHeld << " held, " << check.Leaked.size() << " leaked, "
               << check.Unheld.size() << " unheld, " << check.Shared.size() << " shared, " << check.SeqProblems.size()
               << " seq problems)" << endl;
          expect(check, layout);
          /* Leave the file service's runner having run, so its frame unwinds (see
             file_service_reopen.test.cc's Settle). */
          const Base::TUuid sentinel_uid(Base::TUuid::Twister);
          TCompletionTrigger trigger;
          fs.InsertFile(sentinel_uid, TFileObj::DataFile, 1UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, trigger);
          trigger.Wait();
          fs.RemoveFile(sentinel_uid, 1UL, trigger);
          trigger.Wait();
        }
      }
      std::lock_guard<std::mutex> lock(mut);
      fin = true;
      cond.notify_one();
    }, 4);
    DUtil::TDiskController::TEvent::FinalizeDiskEventPoolManager();
    return open_seconds;
  }

  /* Ten files over two repos, with disjoint sequence ranges in each. */
  vector<TSpec> CleanSpecs() {
    vector<TSpec> specs;
    for (size_t i = 0UL; i < 5UL; ++i) {
      specs.push_back({RepoA, i * 100UL + 1UL, i * 100UL + 100UL, 1UL + i % 3UL});
      specs.push_back({RepoB, i * 10UL + 1UL, i * 10UL + 10UL, 2UL});
    }
    return specs;
  }

  TFileObj DataFile(size_t gen_id, size_t lowest, size_t highest) {
    return TFileObj(TFileObj::DataFile, gen_id, 0UL, 0UL, 0UL, 0UL, lowest, highest);
  }

}  // namespace

/* Sequence ranges, as a pure function. */

FIXTURE(DisjointRangesHaveNoProblems) {
  EXPECT_TRUE(FindSeqRangeProblems({}).empty());
  EXPECT_TRUE(FindSeqRangeProblems({DataFile(3, 21, 30), DataFile(1, 1, 10), DataFile(2, 11, 20)}).empty());
  EXPECT_TRUE(FindSeqRangeProblems({DataFile(1, 5, 5), DataFile(2, 6, 6)}).empty());
}

FIXTURE(DurableFilesAreSkipped) {
  EXPECT_TRUE(FindSeqRangeProblems({DataFile(1, 1, 10), TFileObj(TFileObj::DurableFile, 2, 0, 0, 0, 0, 0, 0)}).empty());
}

FIXTURE(NestedRangeIsAMergeLeftover) {
  const auto problems = FindSeqRangeProblems({DataFile(1, 1, 10), DataFile(5, 1, 30), DataFile(2, 11, 20)});
  EXPECT_EQ(problems.size(), 2UL);
  for (const auto &problem : problems) {
    EXPECT_EQ(problem.Kind, TSeqRangeProblem::Nested);
    EXPECT_EQ(problem.OtherGenId, 5UL);
  }
  /* Identical ranges: a fold of a single file. */
  const auto same = FindSeqRangeProblems({DataFile(1, 1, 10), DataFile(2, 1, 10)});
  EXPECT_EQ(same.size(), 1UL);
  EXPECT_EQ(same.front().Kind, TSeqRangeProblem::Nested);
}

FIXTURE(PartialOverlapIsReported) {
  const auto problems = FindSeqRangeProblems({DataFile(1, 1, 10), DataFile(2, 10, 20)});
  EXPECT_EQ(problems.size(), 1UL);
  EXPECT_EQ(problems.front().Kind, TSeqRangeProblem::PartialOverlap);
  EXPECT_EQ(problems.front().GenId, 2UL);
  EXPECT_EQ(problems.front().OtherGenId, 1UL);
}

FIXTURE(InvertedRangeIsReported) {
  const auto problems = FindSeqRangeProblems({DataFile(1, 1, 10), DataFile(2, 30, 20)});
  EXPECT_EQ(problems.size(), 1UL);
  EXPECT_EQ(problems.front().Kind, TSeqRangeProblem::Inverted);
  EXPECT_EQ(problems.front().GenId, 2UL);
}

/* The whole check, against a reopened store. */

FIXTURE(CleanStorePasses) {
  RunReopen(CleanSpecs(), TCorruption(), [](const TOpenCheck &check, const TLayout &) {
    EXPECT_TRUE(check.IsClean());
    EXPECT_EQ(check.NumFiles, 10UL);
    /* Two images, one append log block, and 19 blocks of files. */
    EXPECT_EQ(check.NumOwned, 22UL);
    EXPECT_EQ(check.NumHeld, 22UL);
    const string text = DescribeOpenCheck(check);
    EXPECT_TRUE(text.find("RESULT: ok\n") != string::npos);
    EXPECT_TRUE(text.find("10 files") != string::npos);
    ReportOpenCheck(check);
  });
}

/* #620's class: a file removed from the map without freeing its blocks. */
FIXTURE(LeakedBlocksAreReportedNotFatal) {
  TCorruption corruption;
  corruption.BeforeClose = [](TFileService &fs, DUtil::TVolumeManager *, TLayout &layout) {
    TCompletionTrigger trigger;
    fs.RemoveFile(layout.Files[3].Spec.Repo, layout.Files[3].GenId, trigger);
    trigger.Wait();
  };
  /* ...and, after the reopen, a block allocated that no file takes. */
  size_t stray = 0UL;
  corruption.AfterOpen = [&stray](TFileService &, DUtil::TVolumeManager *vol_man, const TLayout &) {
    stray = AllocRange(vol_man, 1UL);
  };
  RunReopen(CleanSpecs(), corruption, [&stray](const TOpenCheck &check, const TLayout &layout) {
    /* The removed file's blocks were freed by the restart (nothing walked them), so only the
       stray block is held and unowned. */
    EXPECT_TRUE(check.Leaked == vector<size_t>{stray});
    EXPECT_TRUE(check.IsSafe());
    EXPECT_FALSE(check.IsClean());
    EXPECT_EQ(check.NumFiles, 9UL);
    (void)layout;
    const string text = DescribeOpenCheck(check);
    EXPECT_TRUE(text.find("leaked: 1 blocks held but owned by nothing: " + to_string(stray)) != string::npos);
    EXPECT_TRUE(text.find("RESULT: problems\n") != string::npos);
    ReportOpenCheck(check);
  });
}

/* A file removed while the store runs, its blocks never freed: the leak before the restart. */
FIXTURE(LeakWithoutRestartIsReported) {
  TCorruption corruption;
  vector<size_t> leaked;
  corruption.AfterOpen = [&leaked](TFileService &fs, DUtil::TVolumeManager *, const TLayout &layout) {
    const auto &file = layout.Files[4];
    TCompletionTrigger trigger;
    fs.RemoveFile(file.Spec.Repo, file.GenId, trigger);
    trigger.Wait();
    for (size_t i = 0UL; i < file.Spec.NumBlocks; ++i) {
      leaked.push_back(file.FirstBlock + i);
    }
  };
  RunReopen(CleanSpecs(), corruption, [&leaked](const TOpenCheck &check, const TLayout &) {
    EXPECT_TRUE(check.Leaked == leaked);
    EXPECT_TRUE(check.IsSafe());
  });
}

/* #610's class: the startup walk misses a live file's blocks, so they look free. */
FIXTURE(UnmarkedLiveBlocksAreFatal) {
  TCorruption corruption;
  corruption.MarkFile = [](size_t gen_id) { return gen_id != 6UL; };
  RunReopen(CleanSpecs(), corruption, [](const TOpenCheck &check, const TLayout &layout) {
    const auto &file = layout.Files[5];
    EXPECT_EQ(file.GenId, 6UL);
    vector<size_t> expected;
    for (size_t i = 0UL; i < file.Spec.NumBlocks; ++i) {
      expected.push_back(file.FirstBlock + i);
    }
    EXPECT_TRUE(check.Unheld == expected);
    EXPECT_FALSE(check.IsSafe());
    const auto report = [&check]() { ReportOpenCheck(check); };
    EXPECT_THROW_FUNC(TOpenCheckFailed, report);
  });
}

/* A live file's block freed (waiting for discard, which would erase it). */
FIXTURE(FreedLiveBlockIsFatal) {
  TCorruption corruption;
  size_t freed = 0UL;
  corruption.AfterOpen = [&freed](TFileService &, DUtil::TVolumeManager *vol_man, const TLayout &layout) {
    freed = layout.Files[2].FirstBlock;
    vol_man->FreeSequentialBlocks(DUtil::TBlockRange(freed, 1UL));
  };
  RunReopen(CleanSpecs(), corruption, [&freed](const TOpenCheck &check, const TLayout &) {
    EXPECT_TRUE(check.Unheld == vector<size_t>{freed});
    const auto report = [&check]() { ReportOpenCheck(check); };
    EXPECT_THROW_FUNC(TOpenCheckFailed, report);
  });
}

/* A crafted image where two files claim the same block. */
FIXTURE(SharedBlockIsFatal) {
  TCorruption corruption;
  size_t shared = 0UL;
  corruption.BeforeClose = [&shared](TFileService &fs, DUtil::TVolumeManager *, TLayout &layout) {
    shared = layout.Files[0].FirstBlock;
    TCompletionTrigger trigger;
    fs.InsertFile(RepoB, TFileObj::DataFile, 100UL, shared, 0UL, 1UL, 1UL, 1000UL, 1001UL, trigger);
    trigger.Wait();
  };
  RunReopen(CleanSpecs(), corruption, [&shared](const TOpenCheck &check, const TLayout &) {
    EXPECT_TRUE(check.Shared == vector<size_t>{shared});
    const string text = DescribeOpenCheck(check);
    EXPECT_TRUE(text.find("shared: 1 blocks are owned twice: " + to_string(shared)) != string::npos);
    EXPECT_TRUE(text.find("RESULT: UNSAFE\n") != string::npos);
    const auto report = [&check]() { ReportOpenCheck(check); };
    EXPECT_THROW_FUNC(TOpenCheckFailed, report);
  });
}

/* A crafted image whose repo has overlapping and inverted ranges. */
FIXTURE(OverlappingRangesAreReported) {
  auto specs = CleanSpecs();
  specs.push_back({RepoA, 150UL, 160UL, 1UL});   // gen 11: inside gen 3's [101, 200]
  specs.push_back({RepoA, 450UL, 600UL, 1UL});   // gen 12: partly over gen 9's [401, 500]
  specs.push_back({RepoB, 90UL, 80UL, 1UL});     // gen 13: inverted
  RunReopen(specs, TCorruption(), [](const TOpenCheck &check, const TLayout &) {
    EXPECT_TRUE(check.IsSafe());
    EXPECT_TRUE(check.Leaked.empty());
    EXPECT_EQ(check.SeqProblems.size(), 3UL);
    size_t nested = 0UL, partial = 0UL, inverted = 0UL;
    for (const auto &[repo_id, problem] : check.SeqProblems) {
      switch (problem.Kind) {
        case TSeqRangeProblem::Nested: {
          ++nested;
          EXPECT_EQ(problem.GenId, 11UL);
          EXPECT_EQ(repo_id, RepoA);
          break;
        }
        case TSeqRangeProblem::PartialOverlap: {
          ++partial;
          EXPECT_EQ(problem.GenId, 12UL);
          break;
        }
        case TSeqRangeProblem::Inverted: {
          ++inverted;
          EXPECT_EQ(problem.GenId, 13UL);
          EXPECT_EQ(repo_id, RepoB);
          break;
        }
      }
    }
    EXPECT_EQ(nested, 1UL);
    EXPECT_EQ(partial, 1UL);
    EXPECT_EQ(inverted, 1UL);
    ReportOpenCheck(check);
  });
}

/* What the check costs next to the open, with many files: here 1,500, each a block, and a
   base image long enough to need a chain. */
FIXTURE(CostWithManyFiles) {
  vector<TSpec> specs;
  for (size_t i = 0UL; i < 1500UL; ++i) {
    specs.push_back({i % 2UL ? RepoA : RepoB, i * 10UL + 1UL, i * 10UL + 10UL, 1UL});
  }
  RunReopen(specs, TCorruption(), [](const TOpenCheck &check, const TLayout &) {
    EXPECT_TRUE(check.IsClean());
    EXPECT_EQ(check.NumFiles, 1500UL);
    EXPECT_EQ(check.NumOwned, check.NumHeld);
  });
}
