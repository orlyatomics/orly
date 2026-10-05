/* <orly/indy/disk/util/volume_manager.test.cc>

   Unit test for <orly/indy/disk/util/volume_manager.h>.

   Pins the device-geometry contract behind #386: Desc.Capacity counts only
   the payload blocks (the superblock is provided by the device in addition
   to them, and every I/O shifts by SuperBytes to skip it), so every block
   the allocator hands out -- including the very last physical block of each
   device -- must be both writable and readable.  From 2014 until #386,
   CheckRange() (and TMemoryDevice::ReadImpl's assert) charged SuperBytes
   against Capacity, so the final physical block of every volume was
   allocatable and writable but threw on first read; in a nearly-full volume
   that read happened inside the durable layer cleaner's noexcept destructor
   and aborted the whole server.

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

#include <orly/indy/disk/util/volume_manager.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include <unistd.h>

#include <base/mem_aligned_ptr.h>
#include <orly/indy/disk/sim/mem_engine.h>

#include <base/test/kit.h>

using namespace std;
using namespace std::chrono;
using namespace Base;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;

/* Write and read back one raw block through the volume manager (the mem
   device completes synchronously, so the callback overloads need no fiber
   context). With a callback, the volume registers the I/O on the trigger but
   reports it only to the callback, so the callback must complete the trigger;
   the trigger's destructor waits for that (#590). */
static void RoundTripBlock(TVolumeManager *vol_man, const size_t block_id, uint8_t *buf) {
  const size_t offset = block_id * PhysicalBlockSize;
  TCompletionTrigger write_trigger;
  bool write_done = false;
  vol_man->Write(HERE, FullBlock, 0 /* util_src */, buf, offset, PhysicalBlockSize, RealTime,
                 TCacheInstr::NoCache, write_trigger,
                 [&write_done, &write_trigger](TDiskResult result, const char *err_str) {
    EXPECT_TRUE(result == TDiskResult::Success);
    write_done = true;
    write_trigger.Callback(result, err_str);
  });
  EXPECT_TRUE(write_done);
  memset(buf, 0, PhysicalBlockSize);
  TCompletionTrigger read_trigger;
  bool read_done = false;
  vol_man->Read(HERE, FullBlock, 0 /* util_src */, buf, offset, PhysicalBlockSize, RealTime, read_trigger,
                [&read_done, &read_trigger](TDiskResult result, const char *err_str) {
    EXPECT_TRUE(result == TDiskResult::Success);
    read_done = true;
    read_trigger.Callback(result, err_str);
  });
  EXPECT_TRUE(read_done);
}

/* Allocate every block the fast volume has, exactly as production does, and
   round-trip the highest-addressed one: its device-local range ends at the
   device's Desc.Capacity, which the pre-#386 CheckRange (and
   TMemoryDevice::ReadImpl assert) rejected -- writable, unreadable. */
FIXTURE(EveryAllocatableBlockReadable) {
  const size_t num_fast_mb = 64;
  const size_t num_fast_blocks = (num_fast_mb * 1024 * 1024) / PhysicalBlockSize;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler,
                             num_fast_mb /* fast mem: 64 MB */,
                             16 /* slow mem: 16 MB */,
                             4096 /* page cache slots */,
                             1 /* num page lru */,
                             16 /* block cache slots */,
                             1 /* num block lru */);
  TVolumeManager *vol_man = mem_engine.GetVolMan();
  /* drain the fast volume's allocator so we hold its device-tail block. */
  vector<TBlockRange> ranges;
  size_t num_allocated = 0;
  size_t max_block_id = 0;
  while (num_allocated < num_fast_blocks) {
    vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const TBlockRange &range) {
      ranges.push_back(range);
      num_allocated += range.second;
      max_block_id = max(max_block_id, range.first + range.second - 1);
    });
  }
  EXPECT_EQ(num_allocated, num_fast_blocks);
  auto buf_storage = make_unique<uint8_t[]>(PhysicalBlockSize + 4096);
  uint8_t *buf = buf_storage.get() + (4096 - (reinterpret_cast<uintptr_t>(buf_storage.get()) % 4096)) % 4096;
  /* the first block and the device-tail block. */
  const size_t probe_ids[] = {ranges.front().first, max_block_id};
  for (size_t block_id : probe_ids) {
    for (size_t i = 0; i < PhysicalBlockSize; ++i) {
      buf[i] = static_cast<uint8_t>(block_id + i);
    }
    const uint8_t expected_first = buf[0];
    RoundTripBlock(vol_man, block_id, buf);
    EXPECT_EQ(buf[0], expected_first);
    EXPECT_EQ(buf[PhysicalBlockSize - 1], static_cast<uint8_t>(expected_first + PhysicalBlockSize - 1));
  }
  for (const auto &range : ranges) {
    vol_man->FreeSequentialBlocks(range);
  }
}

/* Write admission (#590): with a data floor set, Ordinary allocations stop while
   that much is still available, and Essential ones (the durable store, the file
   map) can still use it. GetSpace() must agree with what the allocator hands out:
   the 2 MB slow volume has 32 blocks, less than one word of the block map, and
   until #590 a partial last word was never scanned, so those blocks counted as
   free but could not be allocated. */
FIXTURE(DataFloorKeepsSpaceForEssential) {
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler,
                             8 /* fast mem: 8 MB */,
                             2 /* slow mem: 2 MB */,
                             4096 /* page cache slots */,
                             1 /* num page lru */,
                             16 /* block cache slots */,
                             1 /* num block lru */);
  TVolumeManager *vol_man = mem_engine.GetVolMan();
  const TSpace before = vol_man->GetSpace();
  EXPECT_GT(before.Total, 0UL);
  EXPECT_EQ(before.GetAvailable(), before.Total - before.Used + before.DiscardPending);
  const size_t floor = 2UL << 20;
  vol_man->SetDataFloor(floor);
  vector<TBlockRange> ranges;
  const auto take = [&](TAllocClass alloc_class) -> size_t {
    size_t taken = 0UL;
    for (;;) {
      try {
        vol_man->TryAllocateSequentialBlocks(TVolume::TDesc::TStorageSpeed::Fast, 1UL, [&](const TBlockRange &range) {
          ranges.push_back(range);
          taken += range.second;
        }, alloc_class);
      } catch (const TDiskFull &) {
        return taken;
      }
    }
  };
  const size_t ordinary = take(TAllocClass::Ordinary);
  EXPECT_GT(ordinary, 0UL);
  /* Ordinary stopped with the floor still available, and not much more. */
  const size_t at_floor = vol_man->GetSpace().GetAvailable();
  EXPECT_GE(at_floor, floor);
  EXPECT_LT(at_floor, floor + PhysicalBlockSize);
  /* Essential takes what is left, across both volumes. */
  const size_t essential = take(TAllocClass::Essential);
  EXPECT_EQ(essential * PhysicalBlockSize, at_floor);
  EXPECT_EQ(vol_man->GetSpace().GetAvailable(), 0UL);
  for (const auto &range : ranges) {
    vol_man->FreeSequentialBlocks(range);
  }
  /* Freed blocks wait for discard, and count as available meanwhile. */
  EXPECT_EQ(vol_man->GetSpace().GetAvailable(), before.GetAvailable());
}

/* A claim counts while it lives. One that ends in an exception (a merge that ran
   out of space and will retry) is remembered for a while after; one that ends
   normally is not. */
FIXTURE(PendingClaims) {
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler, 8, 2, 4096, 1, 16, 1);
  TVolumeManager *vol_man = mem_engine.GetVolMan();
  EXPECT_EQ(vol_man->GetPendingClaims(), 0UL);
  /* claims */ {
    TVolumeManager::TClaim a(vol_man, 100UL);
    TVolumeManager::TClaim b(vol_man, 50UL);
    EXPECT_EQ(vol_man->GetPendingClaims(), 150UL);
  }
  EXPECT_EQ(vol_man->GetPendingClaims(), 0UL);
  EXPECT_EQ(vol_man->GetRecentPeakClaims(), 0UL);
  try {
    TVolumeManager::TClaim a(vol_man, 100UL);
    TVolumeManager::TClaim b(vol_man, 50UL);
    throw TDiskFull("test");
  } catch (const TDiskFull &) {}
  EXPECT_EQ(vol_man->GetPendingClaims(), 0UL);
  EXPECT_EQ(vol_man->GetRecentPeakClaims(), 150UL);
  /* A null volume manager (no disk engine) is a no-op. */
  TVolumeManager::TClaim none(nullptr, 100UL);
}

/* A memory device whose group-request ReadV submit throws on its ThrowOn-th call (counting from 1;
   0 never throws), as TPersistentDevice::ReadV does when it can't allocate a disk event. */
class TThrowingSubmitDevice
    : public TMemoryDevice {
  NO_COPY(TThrowingSubmitDevice);
  public:

  using TMemoryDevice::TMemoryDevice;

  using TMemoryDevice::ReadV;

  size_t ThrowOn = 0UL;

  size_t NumSubmits = 0UL;

  virtual void ReadV(const Base::TCodeLocation &code_location, TBufKind buf_kind, uint8_t util_src,
                     const std::vector<void *> &buf_vec, const TOffset offset, long long nbytes, DiskPriority priority,
                     bool abort_on_error, TGroupRequest *group_request) override {
    if (++NumSubmits == ThrowOn) {
      throw std::runtime_error("injected submit failure");
    }
    TMemoryDevice::ReadV(code_location, buf_kind, util_src, buf_vec, offset, nbytes, priority, abort_on_error, group_request);
  }

};

/* #594: a vectored read with a callback is one group request: one trigger registration, completed
   by the group once each of its I/Os (one per page on a memory device) has completed. A submit that
   threw part way left the group short, so its callback never ran and the trigger's destructor
   waited forever. Whichever submit throws, the callback must run exactly once, with an error, and
   the trigger must then be destroyable. (The mem device completes synchronously, so this needs no
   fiber; without a fiber the trigger's destructor spins rather than parks.) */
FIXTURE(GroupSubmitThrows) {
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  TThrowingSubmitDevice device(512, 512, 32768 /* 16 MB */, true /* fsync */, true /* corruption check */);
  TVolume volume(TVolume::TDesc{TVolume::TDesc::Striped, device.GetDesc(), TVolume::TDesc::Fast, 1UL, 1UL, 1024UL, 8UL, 0.85},
                 [](TCacheInstr, const TOffset, void *, size_t) {}, &scheduler);
  volume.AddDevice(&device, 0);
  TVolumeManager vol_man(&scheduler);
  vol_man.AddNewVolume(&volume);
  size_t block_id = 0UL;
  vol_man.TryAllocateSequentialBlocks(TVolume::TDesc::Fast, 1, [&block_id](const TBlockRange &range) {
    block_id = range.first;
  });
  constexpr size_t num_pages = 4UL;
  Base::TMemAlignedPtr<char> data = Base::MemAlignedAlloc<char>(getpagesize(), num_pages * PhysicalPageSize);
  void *buf_array[num_pages];
  for (size_t i = 0; i < num_pages; ++i) {
    buf_array[i] = data.get() + i * PhysicalPageSize;
  }
  /* Read the pages, with the throw_on-th submit throwing; returns how many times the callback ran
     and with what. */
  auto read = [&](size_t throw_on, TDiskResult &result) {
    device.ThrowOn = throw_on;
    device.NumSubmits = 0UL;
    size_t num_cb = 0UL;
    bool threw = false;
    /* trigger scope: its destructor waits for the registration */ {
      TCompletionTrigger trigger;
      try {
        vol_man.ReadV(HERE, FullPage, 0 /* util_src */, buf_array, num_pages, block_id * PhysicalBlockSize, num_pages * PhysicalPageSize,
                      RealTime, trigger, [&num_cb, &result, &trigger](TDiskResult cb_result, const char *err_str) {
          ++num_cb;
          result = cb_result;
          trigger.Callback(cb_result, err_str);
        });
      } catch (const std::runtime_error &) {
        threw = true;
      }
      EXPECT_EQ(threw, throw_on != 0UL);
    }
    return num_cb;
  };
  TDiskResult result = TDiskResult::Error;
  EXPECT_EQ(read(0UL, result), 1UL);
  EXPECT_TRUE(result == TDiskResult::Success);
  /* One submit per page; the cases below need the group to have several. */
  EXPECT_EQ(device.NumSubmits, num_pages);
  for (size_t throw_on = 1; throw_on <= num_pages; ++throw_on) {
    result = TDiskResult::Success;
    EXPECT_EQ(read(throw_on, result), 1UL);
    EXPECT_TRUE(result == TDiskResult::Error);
  }
}
