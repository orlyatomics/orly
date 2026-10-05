/* <orly/indy/disk/in_file.test.cc>

   Unit test for <orly/indy/disk/in_file.h>.

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

#include <orly/indy/disk/in_file.h>

#include <cstring>
#include <memory>
#include <stdexcept>

#include <orly/indy/disk/out_stream.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber_test_runner.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;

typedef TStream<LogicalPageSize, LogicalBlockSize, PhysicalBlockSize, CheckedPage, 0UL> TDataInStream;
typedef TOutStream<LogicalPageSize, LogicalBlockSize, PhysicalBlockSize, PageCheckedBlock> TDataOutStream;

TBufBlock::TPool TBufBlock::Pool(PhysicalBlockSize, 100UL);

class TMyInFile
    : public TInFile {
  NO_COPY(TMyInFile);
  public:

  TMyInFile(const Orly::Indy::Util::TBlockVec &block_vec) : BlockVec(block_vec) {}

  virtual size_t GetFileLength() const override {
    return BlockVec.Size() * LogicalBlockSize;
  }

  virtual size_t GetStartingBlock() const override {
    return BlockVec.Front();
  }

  virtual void ReadMeta(size_t /*offset*/, size_t &/*out*/) const override {
    throw;
  }

  virtual size_t FindPageIdOfByte(size_t offset) const override {
    return (BlockVec[offset / LogicalBlockSize] * PagesPerBlock) + ((offset % LogicalBlockSize) / LogicalPageSize);
  }

  private:

  const Orly::Indy::Util::TBlockVec &BlockVec;

};

FIXTURE(Stream) {
  Orly::Indy::Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Orly::Indy::Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Sim::TMemEngine mem_engine(&scheduler,
                               64 /* disk space: 64 MB */,
                               16,
                               4096 /* page cache slots: 1GB */,
                               1 /* num page lru */,
                               16 /* block cache slots: 1GB */,
                               1 /* num block lru */);

    const size_t num_blocks_to_write = 40UL;

    Orly::Indy::Util::TBlockVec block_vec;
    mem_engine.GetEngine()->AppendReserveBlocks(TVolume::TDesc::Fast, num_blocks_to_write, block_vec);
    unordered_map<size_t, shared_ptr<const TBufBlock>> collision_map {};
    TCompletionTrigger trigger;

    const size_t num_to_write = (num_blocks_to_write * LogicalBlockSize) / sizeof(size_t);

    /* stream data out */ {
      std::unordered_set<size_t> written_block_set{};
      TDataOutStream out_stream(HERE, 0UL, mem_engine.GetVolMan(), 0UL, block_vec, collision_map,
                                trigger, RealTime, true
                                #ifndef NDEBUG
                                ,written_block_set
                                #endif
                                );
      for (size_t i = 0; i < num_to_write; ++i) {
        out_stream << i;
      }
    }
    TMyInFile in_file(block_vec);
    TDataInStream in_stream(HERE, 0UL, RealTime, &in_file, mem_engine.GetPageCache(), 0UL);
    size_t in_val;
    for (size_t i = 0; i < num_to_write; ++i) {
      in_stream.Read(in_val);
      EXPECT_EQ(in_val, i);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #596: a prefetch whose read fails. The cache's async loaders register each read on the stream's
   AsyncTrigger but used to complete it only on success, so a failed prefetch left ~TStream waiting
   forever, and AsyncMultiGet left the pages looking loading forever to anyone who then read them.

   A memory device fails a read only on a corrupt page, and aborts unless the caller passed
   abort_on_error = false, which the cache never does. So this device reports instead of aborting,
   and the fixture overwrites one page without its checksum. */
class TReportingMemoryDevice
    : public TMemoryDevice {
  NO_COPY(TReportingMemoryDevice);
  public:

  using TMemoryDevice::TMemoryDevice;

  virtual void Read(const Base::TCodeLocation &code_location, TBufKind buf_kind, uint8_t util_src, void *buf,
                    const TOffset offset, long long nbytes, DiskPriority priority, bool /*abort_on_error*/, TCompletionTrigger &trigger) override {
    TMemoryDevice::Read(code_location, buf_kind, util_src, buf, offset, nbytes, priority, false, trigger);
  }

  virtual void Read(const Base::TCodeLocation &code_location, TBufKind buf_kind, uint8_t util_src, void *buf,
                    const TOffset offset, long long nbytes, DiskPriority priority, bool /*abort_on_error*/, const TIOCallback &cb) override {
    TMemoryDevice::Read(code_location, buf_kind, util_src, buf, offset, nbytes, priority, false, cb);
  }

  virtual void ReadV(const Base::TCodeLocation &code_location, TBufKind buf_kind, uint8_t util_src,
                     const std::vector<void *> &buf_vec, const TOffset offset, long long nbytes, DiskPriority priority,
                     bool /*abort_on_error*/, TCompletionTrigger &trigger) override {
    TMemoryDevice::ReadV(code_location, buf_kind, util_src, buf_vec, offset, nbytes, priority, false, trigger);
  }

  virtual void ReadV(const Base::TCodeLocation &code_location, TBufKind buf_kind, uint8_t util_src,
                     const std::vector<void *> &buf_vec, const TOffset offset, long long nbytes, DiskPriority priority,
                     bool /*abort_on_error*/, TGroupRequest *group_request) override {
    TMemoryDevice::ReadV(code_location, buf_kind, util_src, buf_vec, offset, nbytes, priority, false, group_request);
  }

};

/* One block of sequential size_t values, written through a TOutStream, with logical page BadPage
   then overwritten by unchecksummed bytes. Reading a block's pages in order, TStream prefetches
   BadPage on its own (as a one-page AsyncMultiGet) while it moves onto page 4, before anything
   reads BadPage. The cache gets no write-through callback, so every read goes to the device. */
class TCorruptPageEnv {
  NO_COPY(TCorruptPageEnv);
  public:

  static constexpr size_t BadPage = 6UL;

  static constexpr size_t ValsPerPage = LogicalPageSize / sizeof(size_t);

  static constexpr size_t NumVals = LogicalBlockSize / sizeof(size_t);

  TCorruptPageEnv()
      : Scheduler(TScheduler::TPolicy(4, 10, milliseconds(10))),
        Device(512, 512, 32768 /* 16 MB */, true /* fsync */, true /* corruption check */),
        Volume(TVolume::TDesc{TVolume::TDesc::Striped, Device.GetDesc(), TVolume::TDesc::Fast, 1UL, 1UL, 1024UL, 8UL, 0.85},
               [](TCacheInstr, const TOffset, void *, size_t) {}, &Scheduler),
        VolMan(&Scheduler) {
    static_assert(LogicalPageSize % sizeof(size_t) == 0, "values must not straddle pages");
    Volume.AddDevice(&Device, 0);
    VolMan.AddNewVolume(&Volume);
    Cache = make_unique<TPageCache>(&VolMan, 1024, 1);
    VolMan.TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1, [this](const TBlockRange &range) {
      BlockVec.PushBack(range);
    });
    unordered_map<size_t, shared_ptr<const TBufBlock>> collision_map {};
    TCompletionTrigger trigger;
    /* stream data out */ {
      std::unordered_set<size_t> written_block_set{};
      TDataOutStream out_stream(HERE, 0UL, &VolMan, 0UL, BlockVec, collision_map,
                                trigger, RealTime, true
                                #ifndef NDEBUG
                                ,written_block_set
                                #endif
                                );
      for (size_t i = 0; i < NumVals; ++i) {
        out_stream << i;
      }
    }
    trigger.Wait();
    TMyInFile in_file(BlockVec);
    Base::TMemAlignedPtr<char> garbage = Base::MemAlignedAlloc<char>(getpagesize(), PhysicalPageSize);
    memset(garbage.get(), 0xA5, PhysicalPageSize);
    VolMan.Write(HERE, FullPage, 0UL, garbage.get(), in_file.FindPageIdOfByte(BadPage * LogicalPageSize) * PhysicalPageSize, PhysicalPageSize,
                 RealTime, TCacheInstr::NoCache, trigger);
    trigger.Wait();
  }

  TPageCache *GetCache() const {
    return Cache.get();
  }

  const Orly::Indy::Util::TBlockVec &GetBlockVec() const {
    return BlockVec;
  }

  private:

  TScheduler Scheduler;

  TReportingMemoryDevice Device;

  TVolume Volume;

  TVolumeManager VolMan;

  unique_ptr<TPageCache> Cache;

  Orly::Indy::Util::TBlockVec BlockVec;

};

/* The prefetch of BadPage fails and nothing reads it: destroying the stream must return, and must
   not throw (which would terminate). A later reader of BadPage then gets an error, not data. */
FIXTURE(PrefetchErrorUnread) {
  Orly::Indy::Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Orly::Indy::Fiber::TRunner::TRunnerCons &) {
    TCorruptPageEnv env;
    TMyInFile in_file(env.GetBlockVec());
    /* read up to page 4 */ {
      TDataInStream in_stream(HERE, 0UL, RealTime, &in_file, env.GetCache(), 0UL);
      size_t in_val;
      for (size_t i = 0; i < 4 * TCorruptPageEnv::ValsPerPage + 1; ++i) {
        in_stream.Read(in_val);
        EXPECT_EQ(in_val, i);
      }
    }
    bool threw = false;
    try {
      TDataInStream in_stream(HERE, 0UL, RealTime, &in_file, env.GetCache(), TCorruptPageEnv::BadPage * LogicalPageSize);
    } catch (const std::logic_error &) {
      threw = true;
    }
    EXPECT_TRUE(threw);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* The same failed prefetch, but the stream reads on into BadPage: everything before it reads back
   intact, moving onto BadPage throws, and destroying the stream then returns. */
FIXTURE(PrefetchErrorRead) {
  Orly::Indy::Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Orly::Indy::Fiber::TRunner::TRunnerCons &) {
    TCorruptPageEnv env;
    TMyInFile in_file(env.GetBlockVec());
    size_t threw_at = TCorruptPageEnv::NumVals;
    /* read until it fails */ {
      TDataInStream in_stream(HERE, 0UL, RealTime, &in_file, env.GetCache(), 0UL);
      size_t in_val;
      for (size_t i = 0; i < TCorruptPageEnv::NumVals; ++i) {
        try {
          in_stream.Read(in_val);
        } catch (const std::logic_error &) {
          threw_at = i;
          break;
        }
        EXPECT_EQ(in_val, i);
      }
    }
    /* A read moves the stream onto the next page as soon as it finishes the current one, so it is
       the read of the last value before BadPage that throws. */
    EXPECT_EQ(threw_at, TCorruptPageEnv::BadPage * TCorruptPageEnv::ValsPerPage - 1);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
