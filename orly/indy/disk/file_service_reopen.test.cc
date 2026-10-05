/* <orly/indy/disk/file_service_reopen.test.cc>

   Unit test for <orly/indy/disk/file_service.h>: after a restart, the blocks of both base images
   must stay allocated, not only each image's first block (#610).

   A base image larger than one block (more than NumElemPerBaseImageBlock files) is a chain: each
   block names the next. On reopen, only the two head blocks used to be marked used, so the rest
   of each chain looked free, and a data file could be written over the image the file map had
   just been loaded from.

   Each "boot" here builds a fresh volume and volume manager over the same memory device, so the
   allocator starts out knowing nothing, as it does after a real restart. Files are inserted one at
   a time, so each runner round applies exactly one op, and versions and image writes are
   predictable: with a one-block append log (NumSectorsPerBlock sectors), round 129 * k writes
   base image k, to image 1 for odd k and image 2 for even k.

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

#include <orly/indy/disk/file_service.h>

#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

#include <base/scheduler.h>
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

  /* One volume and its allocator over a device that outlives them: one boot of the machine. */
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

  /* Where the file service's fixed blocks live, as the system block records them. */
  struct TLayout {
    size_t Image1BlockId;
    size_t Image2BlockId;
    vector<size_t> AppendLogBlockVec;
  };

  const TFileService::TFileInitCb NoFileInit = [](TFileObj::TKind, const Base::TUuid &, size_t, size_t, size_t, size_t) {
    return true;
  };

  size_t AllocOne(DUtil::TVolumeManager *vol_man) {
    size_t block_id = 0UL;
    vol_man->TryAllocateSequentialBlocks(DUtil::TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const DUtil::TBlockRange &range) {
      block_id = range.first;
    });
    return block_id;
  }

  /* First boot: lay out the fixed blocks, as TDiskEngine does on create. */
  TLayout Create(DUtil::TVolumeManager *vol_man) {
    TLayout layout;
    layout.Image1BlockId = AllocOne(vol_man);
    layout.Image2BlockId = AllocOne(vol_man);
    layout.AppendLogBlockVec.push_back(AllocOne(vol_man));
    return layout;
  }

  /* Later boots: mark the fixed blocks used, as TDiskEngine does on reopen. */
  void MarkFixedBlocks(DUtil::TVolumeManager *vol_man, const TLayout &layout) {
    vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(layout.Image1BlockId, 1UL));
    vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(layout.Image2BlockId, 1UL));
    for (size_t block_id : layout.AppendLogBlockVec) {
      vol_man->MarkBlockRangeUsed(DUtil::TBlockRange(block_id, 1UL));
    }
  }

  void InsertFiles(TFileService &fs, const Base::TUuid &file_uid, size_t num_files) {
    for (size_t i = 1; i <= num_files; ++i) {
      TCompletionTrigger trigger;
      fs.InsertFile(file_uid, TFileObj::DataFile, i, i * 12UL, i * 123UL, i * 4096UL, i * 2013UL, i * 9UL, i * 2022UL, trigger);
      trigger.Wait();
    }
  }

  /* Run one insert and one remove through the file service, leaving its file map as it was. A
     file service destroyed before its runner fiber has ever run leaves that fiber latched, which a
     debug build reports as "Stack frame was not unwound properly" when the frame pool goes away;
     this makes sure the fiber has run. */
  void Settle(TFileService &fs) {
    const Base::TUuid sentinel_uid(Base::TUuid::Twister);
    TCompletionTrigger trigger;
    fs.InsertFile(sentinel_uid, TFileObj::DataFile, 1UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, trigger);
    trigger.Wait();
    fs.RemoveFile(sentinel_uid, 1UL, trigger);
    trigger.Wait();
  }

  /* Read a base image straight off the device: its version, and the blocks of its chain after
     the head, in order. Stops at the first block that doesn't read or carries another version. */
  size_t ReadImageChain(DUtil::TVolumeManager *vol_man, size_t head_block_id, vector<size_t> &chain) {
    auto buf_block = make_unique<TBufBlock>();
    const size_t *buf = reinterpret_cast<const size_t *>(buf_block->GetData());
    size_t block_id = head_block_id;
    size_t version = 0UL;
    for (;;) {
      try {
        TCompletionTrigger trigger;
        vol_man->ReadBlock(HERE, DUtil::CheckedBlock, Source::FileService, buf_block->GetData(), block_id, RealTime, trigger, false);
        trigger.Wait();
      } catch (const TDiskError &) {
        return version;
      }
      if (block_id == head_block_id) {
        version = buf[0];
      } else if (buf[0] != version) {
        return version;
      } else {
        chain.push_back(block_id);
      }
      if (buf[1] == static_cast<size_t>(-1) || version == 0UL) {
        return version;
      }
      block_id = buf[1];
    }
  }

  /* Overwrite one block with a valid, checksummed block of zeros: a version that matches no image. */
  void ZeroBlock(DUtil::TVolumeManager *vol_man, size_t block_id) {
    auto buf_block = make_unique<TBufBlock>();
    memset(buf_block->GetData(), 0, DUtil::PhysicalBlockSize);
    TCompletionTrigger trigger;
    vol_man->WriteBlock(HERE, DUtil::CheckedBlock, Source::FileService, buf_block->GetData(), block_id, RealTime, DUtil::TCacheInstr::NoCache, trigger);
    trigger.Wait();
  }

  /* Every block the allocator will still hand out. They are freed again before this returns,
     so the file service can still find room for its next base image. */
  unordered_set<size_t> AllocateEverything(DUtil::TVolumeManager *vol_man) {
    unordered_set<size_t> allocated;
    for (;;) {
      try {
        vol_man->TryAllocateSequentialBlocks(DUtil::TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const DUtil::TBlockRange &range) {
          for (size_t i = 0; i < range.second; ++i) {
            allocated.insert(range.first + i);
          }
        });
      } catch (const DUtil::TDiskFull &) {
        break;
      }
    }
    for (size_t block_id : allocated) {
      vol_man->FreeSequentialBlocks(DUtil::TBlockRange(block_id, 1UL));
    }
    return allocated;
  }

  /* Write 'num_files' files on a fresh file system, restart, let 'downtime' damage the device, and
     restart again with 'check' running against the reopened file service. 'check' gets the two
     images' chains as they were on disk before the downtime. */
  void RunRestart(size_t num_files,
                  const function<void (DUtil::TVolumeManager *, const vector<size_t> &, const vector<size_t> &)> &downtime,
                  const function<void (TFileService &, DUtil::TVolumeManager *, const vector<size_t> &, const vector<size_t> &)> &check,
                  const function<void (TFileService &)> &recheck = nullptr) {
    DUtil::TDiskController::TEvent::InitializeDiskEventPoolManager(1000UL);
    Orly::Indy::Fiber::TFiberTestRunner runner([&](std::mutex &mut, std::condition_variable &cond, bool &fin, Orly::Indy::Fiber::TRunner::TRunnerCons &runner_cons) {
      /* scope */ {
        Base::TScheduler scheduler(Base::TScheduler::TPolicy(4, 4, milliseconds(10)));
        TFramePoolManager *frame_pool_manager = Orly::Indy::Fiber::TFrame::LocalFramePool->GetPoolManager();
        auto device = make_unique<DUtil::TMemoryDevice>(512, 512, NumLogicalBlocks, true /* fsync */, true);
        const Base::TUuid file_uid(Base::TUuid::Twister);
        TLayout layout;
        /* first boot */ {
          TBoot boot(&scheduler, device.get());
          layout = Create(boot.GetVolMan());
          TFileService fs(&scheduler, runner_cons, frame_pool_manager, boot.GetVolMan(), layout.Image1BlockId, layout.Image2BlockId, layout.AppendLogBlockVec, NoFileInit, true, false);
          InsertFiles(fs, file_uid, num_files);
        }
        vector<size_t> chain_1, chain_2;
        /* downtime: read the images as they are, then damage them if the test wants to */ {
          TBoot boot(&scheduler, device.get());
          ReadImageChain(boot.GetVolMan(), layout.Image1BlockId, chain_1);
          ReadImageChain(boot.GetVolMan(), layout.Image2BlockId, chain_2);
          downtime(boot.GetVolMan(), chain_1, chain_2);
        }
        /* second boot */ {
          TBoot boot(&scheduler, device.get());
          MarkFixedBlocks(boot.GetVolMan(), layout);
          TFileService fs(&scheduler, runner_cons, frame_pool_manager, boot.GetVolMan(), layout.Image1BlockId, layout.Image2BlockId, layout.AppendLogBlockVec, NoFileInit, false, false);
          check(fs, boot.GetVolMan(), chain_1, chain_2);
          Settle(fs);
        }
        /* third boot, if the test wants one */
        if (recheck) {
          TBoot boot(&scheduler, device.get());
          MarkFixedBlocks(boot.GetVolMan(), layout);
          try {
            TFileService fs(&scheduler, runner_cons, frame_pool_manager, boot.GetVolMan(), layout.Image1BlockId, layout.Image2BlockId, layout.AppendLogBlockVec, NoFileInit, false, false);
            recheck(fs);
            Settle(fs);
          } catch (const std::exception &ex) {
            cout << "third boot failed: " << ex.what() << endl;
            EXPECT_TRUE(false);
          }
        }
      }
      std::lock_guard<std::mutex> lock(mut);
      fin = true;
      cond.notify_one();
    }, 4);
    DUtil::TDiskController::TEvent::FinalizeDiskEventPoolManager();
  }

  size_t CountAllocatable(const unordered_set<size_t> &allocated, const vector<size_t> &chain) {
    size_t n = 0UL;
    for (size_t block_id : chain) {
      n += allocated.count(block_id);
    }
    return n;
  }

}  // namespace

/* 1,100 files: image 1 was written at version 903 and image 2 at 1032, both two blocks long, and
   the append log holds versions 1033-1100. Image 2 loads; image 1 is the fallback. Neither chain
   may be handed out again. */
FIXTURE(BothChainsKeptAfterReopen) {
  RunRestart(1100UL,
             [](DUtil::TVolumeManager *, const vector<size_t> &, const vector<size_t> &) {},
             [](TFileService &fs, DUtil::TVolumeManager *vol_man, const vector<size_t> &chain_1, const vector<size_t> &chain_2) {
    EXPECT_EQ(fs.GetNumFiles(), 1100UL);
    EXPECT_EQ(chain_1.size(), 1UL);
    EXPECT_EQ(chain_2.size(), 1UL);
    const auto allocated = AllocateEverything(vol_man);
    EXPECT_FALSE(allocated.empty());
    EXPECT_EQ(CountAllocatable(allocated, chain_2), 0UL);  // the image that loaded
    EXPECT_EQ(CountAllocatable(allocated, chain_1), 0UL);  // the fallback
  });
}

/* As above, but the fallback's chain block was overwritten while the server was down. The image
   that loaded must still be kept; the broken one can't be loaded anyway, and its chain is not
   adopted, since a broken chain's links can't be trusted. */
FIXTURE(LoadedChainKeptWhenAlternateIsCorrupt) {
  RunRestart(1100UL,
             [](DUtil::TVolumeManager *vol_man, const vector<size_t> &chain_1, const vector<size_t> &) {
    if (EXPECT_EQ(chain_1.size(), 1UL)) {
      ZeroBlock(vol_man, chain_1[0]);
    }
  },
             [](TFileService &fs, DUtil::TVolumeManager *vol_man, const vector<size_t> &chain_1, const vector<size_t> &chain_2) {
    EXPECT_EQ(fs.GetNumFiles(), 1100UL);
    EXPECT_EQ(chain_2.size(), 1UL);
    const auto allocated = AllocateEverything(vol_man);
    EXPECT_EQ(CountAllocatable(allocated, chain_2), 0UL);
    EXPECT_EQ(CountAllocatable(allocated, chain_1), chain_1.size());
  });
}

/* 1,032 files: image 2 was just written at version 1032 and nothing has been appended since, so
   the append log still starts at version 904, right after image 1. Break image 2's chain and the
   file service falls back to image 1 plus the log, losing only the file that was in image 2
   alone. Image 1's chain is now the live one and must be kept. */
FIXTURE(FallbackChainKeptWhenNewestIsCorrupt) {
  RunRestart(1032UL,
             [](DUtil::TVolumeManager *vol_man, const vector<size_t> &, const vector<size_t> &chain_2) {
    if (EXPECT_EQ(chain_2.size(), 1UL)) {
      ZeroBlock(vol_man, chain_2[0]);
    }
  },
             [](TFileService &fs, DUtil::TVolumeManager *vol_man, const vector<size_t> &chain_1, const vector<size_t> &chain_2) {
    EXPECT_EQ(fs.GetNumFiles(), 1031UL);
    EXPECT_EQ(chain_1.size(), 1UL);
    const auto allocated = AllocateEverything(vol_man);
    EXPECT_EQ(CountAllocatable(allocated, chain_1), 0UL);
    EXPECT_EQ(CountAllocatable(allocated, chain_2), chain_2.size());
  });
}

/* The consequence: after a restart, data files take whatever the allocator hands out. Write over
   every such block, as data files would, and restart again. Both images must still load. */
FIXTURE(ImagesSurviveWritesAfterReopen) {
  RunRestart(1100UL,
             [](DUtil::TVolumeManager *, const vector<size_t> &, const vector<size_t> &) {},
             [](TFileService &fs, DUtil::TVolumeManager *vol_man, const vector<size_t> &, const vector<size_t> &) {
    EXPECT_EQ(fs.GetNumFiles(), 1100UL);
    for (size_t block_id : AllocateEverything(vol_man)) {
      ZeroBlock(vol_man, block_id);
    }
  },
             [](TFileService &fs) {
    EXPECT_EQ(fs.GetNumFiles(), 1100UL);
  });
}
