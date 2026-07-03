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
#include <vector>

#include <orly/indy/disk/sim/mem_engine.h>

#include <base/test/kit.h>

using namespace std;
using namespace std::chrono;
using namespace Base;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;

/* Write and read back one raw block through the volume manager (the mem
   device completes synchronously, so the callback overloads need no fiber
   context). */
static void RoundTripBlock(TVolumeManager *vol_man, const size_t block_id, uint8_t *buf) {
  const size_t offset = block_id * PhysicalBlockSize;
  TCompletionTrigger write_trigger;
  bool write_done = false;
  vol_man->Write(HERE, FullBlock, 0 /* util_src */, buf, offset, PhysicalBlockSize, RealTime,
                 TCacheInstr::NoCache, write_trigger,
                 [&write_done](TDiskResult result, const char */*err_str*/) {
    EXPECT_TRUE(result == TDiskResult::Success);
    write_done = true;
  });
  EXPECT_TRUE(write_done);
  memset(buf, 0, PhysicalBlockSize);
  TCompletionTrigger read_trigger;
  bool read_done = false;
  vol_man->Read(HERE, FullBlock, 0 /* util_src */, buf, offset, PhysicalBlockSize, RealTime, read_trigger,
                [&read_done](TDiskResult result, const char */*err_str*/) {
    EXPECT_TRUE(result == TDiskResult::Success);
    read_done = true;
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
