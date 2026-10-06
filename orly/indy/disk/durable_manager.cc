/* <orly/indy/disk/durable_manager.cc>

   Implements <orly/indy/disk/durable_manager.h>.

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

#include <orly/indy/disk/durable_manager.h>

#include <optional>

#include <base/booster.h>
#include <orly/indy/disk/util/hash_util.h>
#include <base/util/time.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy::Util;
using namespace Orly::Indy::Disk;

const Base::TUuid TDurableManager::DurableByIdFileId("20E91BAE-3465-4E9B-918F-C234DF84762A");

const Base::TUuid TDurableManager::TSortedByIdFile::NullId("00000000-0000-0000-0000-000000000000");

/* Make a durable file's blocks durable before the file map names it (#619). The file map's own
   write syncs only the device holding its append-log sector, and a durable file's blocks are
   striped across the others, so without this a power loss can leave an acknowledged save in the
   map with its blocks still in the device cache, and startup aborts reading it. Data files do the
   same before their InsertFile (data_file.cc, merge_data_file.cc). */
static void SyncDurableFileBlocks(Orly::Indy::Disk::Util::TEngine *engine, const Orly::Indy::Util::TBlockVec &block_vec) {
  std::vector<Orly::Indy::Disk::Util::TBlockRange> block_range_vec;
  for (const auto &iter : block_vec.GetSeqBlockMap()) {
    block_range_vec.emplace_back(iter.second.first, iter.second.second);
  }
  if (!block_range_vec.empty()) {
    engine->GetVolMan()->SyncToDisk(block_range_vec);
  }
}

TDurableManager::TMapping::~TMapping() {
  EntryCollection.DeleteEachMember();
}

void TDurableManager::TMemSlushLayer::FindMax(TSequenceNumber &cur_max_seq, const Base::TUuid &id, std::string &serialized_form_out) const {
  for (TEntryCollection::TCursor csr(&EntryCollection); csr; ++csr) {
    int uuid_comp = uuid_compare(csr->GetId(), id.GetRaw());
    if (uuid_comp > 0) {
      break;
    } else if (uuid_comp == 0 && csr->GetSeqNum() > cur_max_seq) {
      cur_max_seq = csr->GetSeqNum();
      serialized_form_out = csr->GetSerializedForm();
    }
  }
}

TDurableManager::TDiskOrderedLayer::~TDiskOrderedLayer() {
  if (GetMarkedForDelete()) {
    try {
      TCompletionTrigger completion_trigger;
      Indy::Util::TBlockVec block_vec;
      /* read the blocks in the file */ {
        TSortedInFile in_file(Engine, Low, GenId);
        size_t num_blocks = in_file.GetNumBlocks();
        TInStream in_stream(HERE, Source::FileRemoval, Low, &in_file, Engine->GetPageCache(), TSortedByIdFile::NumMetaFields * sizeof(size_t));
        size_t block_id;
        for (size_t i = 0; i < num_blocks; ++i) {
          in_stream.Read(block_id);
          block_vec.PushBack(block_id);
        }
      }
      Engine->RemoveFile(DurableByIdFileId, GenId, completion_trigger);
      /* wait for everything to flush */ {
        completion_trigger.Wait();
      }
      for (const auto &iter : block_vec.GetSeqBlockMap()) {
        Engine->FreeSeqBlocks(iter.second.first, iter.second.second);
      }
    } catch (const Disk::TDiskServiceShutdown &/*ex*/) {
      /*ignore, we're shutting down by force! */
    } catch (const std::exception &ex) {
      /* This destructor is noexcept (as all destructors are); anything escaping it is
         instant std::terminate, which is how the #386 lang_test abort presented.  A
         failure to reclaim one dead layer's blocks costs leaked disk space, not
         integrity -- log it loudly and keep the server alive. */
      syslog(LOG_ERR, "~TDiskOrderedLayer: failed to reclaim gen [%ld] file: %s; leaking its blocks", GenId, ex.what());
      GetManager()->RemovalFailed = true;
    }
  }
}

TDurableManager::TDurableManager(TScheduler *scheduler,
                                 Fiber::TRunner::TRunnerCons &runner_cons,
                                 Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                                 DurableManager::TManager *manager,
                                 Util::TEngine *engine,
                                 size_t max_cache_size,
                                 chrono::milliseconds write_delay,
                                 chrono::milliseconds merge_delay,
                                 chrono::milliseconds layer_cleaning_interval,
                                 size_t temp_file_consol_thresh,
                                 bool create,
                                 const TNotify *notify)
    : Durable::TManager(max_cache_size),
      Manager(manager),
      MergerScheduler(runner_cons),
      WriterScheduler(runner_cons),
      MergerFrame(nullptr),
      WriterFrame(nullptr),
      Engine(engine),
      CurMemoryLayer(new TMemSlushLayer(this)),
      SeqNum(1UL),
      NextSlushGenId(1UL),
      NextDurableByIdGenId(1UL),
      TempFileConsolThresh(temp_file_consol_thresh),
      DurableWriteDelay(write_delay),
      DurableMergeDelay(merge_delay),
      ShutDown(false),
      WriterRetired(false),
      Scheduler(scheduler),
      MappingCollection(this),
      RemovalCollection(this),
      LayerCleanerTimer(layer_cleaning_interval),
      LayerCleanerStopping(false),
      LayerCleanerStarted(false),
      Notify(notify) {
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> mapping_lock(MappingLock);
    new TMapping(this);
  }  // release Mapping lock

  if (!create) {
    /* if we are starting from a cold image, add the mappings for the files that already exist */


    std::vector<Disk::TFileObj> file_vec;
    Engine->AppendFileGenSet(DurableByIdFileId, file_vec);
    size_t max_gen_id = 0UL;
    for (const auto &fi : file_vec) {
      max_gen_id = std::max(max_gen_id, fi.GenId);
      AddMapping(new TDiskOrderedLayer(this, Engine, fi.GenId, fi.NumKeys));
    }
    NextDurableByIdGenId = max_gen_id + 1UL;
    /* Number new saves above every entry already on disk (#609). Loads and merges keep an id's
       entry with the highest sequence number, so starting again from 1 made every save after a
       restart lose to the copy it replaced. The sequence number is only in the entries, so this
       reads each entry's header once: one pass over the durable files, which merging keeps to
       about one copy of each live session and POV. */
    const auto scan_start = std::chrono::steady_clock::now();
    size_t num_entries = 0UL;
    for (const auto &fi : file_vec) {
      num_entries += fi.NumKeys;
      SeqNum = std::max(SeqNum, TSortedInFile(Engine, Medium, fi.GenId).FindMaxSeqNum(Medium));
    }
    const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scan_start).count();
    syslog(LOG_INFO, "TDurableManager: resuming sequence numbers after %ld, from %ld file(s) and %ld entries in %ldms",
           static_cast<long>(SeqNum), static_cast<long>(file_vec.size()), static_cast<long>(num_entries), static_cast<long>(scan_ms));
  }
  WriterHostHandle = scheduler->ScheduleCancelable([this, frame_pool_manager] {
    Fiber::LaunchSlowFiberSched(&WriterScheduler, frame_pool_manager);
    SchedulerExitedSem.Push();
  });
  WriterFrame = Fiber::TFrame::LocalFramePool->Alloc();
  try {
    WriterFrame->Latch(&WriterScheduler, this, static_cast<Fiber::TRunnable::TFunc>(&TDurableManager::RunWriter));
  } catch (...) {
    Fiber::TFrame::LocalFramePool->Free(WriterFrame);
    throw;
  }
  MergerHostHandle = scheduler->ScheduleCancelable([this, frame_pool_manager] {
    Fiber::LaunchSlowFiberSched(&MergerScheduler, frame_pool_manager);
    SchedulerExitedSem.Push();
  });
  MergerFrame = Fiber::TFrame::LocalFramePool->Alloc();
  try {
    MergerFrame->Latch(&MergerScheduler, this, static_cast<Fiber::TRunnable::TFunc>(&TDurableManager::RunMerger));
  } catch (...) {
    Fiber::TFrame::LocalFramePool->Free(MergerFrame);
    throw;
  }
}

TDurableManager::~TDurableManager() {
  /* Stop the writer first and only then delete the memory layer: the writer's exit path runs a
     final drain (#277) that flushes whatever is still in CurMemoryLayer (and wakes any savers
     still waiting on their durability sems), so deleting it before the writer is done would be
     a use-after-free -- and would silently drop the saves, which is what the old order did. */
  ShutDown = true;
  /* Cancel-or-join the two host jobs first (#462): a host that never got a scheduler worker
     never ran its runner loop, so the fiber latched onto it never ran either -- its finished
     sem below would never push (deadlock) and the loop could otherwise still start late,
     against the members we are about to destroy.  A failed cancel means the job has run or
     is running: the fiber handshakes below are then live and the exit sem will push.  (A
     cancelled host leaves its fiber latched-but-never-run; the frame is returned to the pool
     regardless -- it never ran, so it holds no resources beyond its slab storage.) */
  const bool writer_hosted = WriterHostHandle && !Scheduler->Cancel(WriterHostHandle);
  const bool merger_hosted = MergerHostHandle && !Scheduler->Cancel(MergerHostHandle);
  SlushSem.Push();
  MergeSem.Push();
  if (writer_hosted) {
    WriterFinishedSem.Pop();
  }
  if (merger_hosted) {
    MergerFinishedSem.Pop();
  }
  WriterScheduler.ShutDown();
  MergerScheduler.ShutDown();
  /* The runner loops live on scheduler jobs, not threads we can join; wait for each one that
     actually ran to exit before member destruction so a still-scanning runner can never touch
     its dying sibling. */
  if (writer_hosted) {
    SchedulerExitedSem.Pop();
  }
  if (merger_hosted) {
    SchedulerExitedSem.Pop();
  }
  Fiber::TFrame::LocalFramePool->Free(MergerFrame);
  Fiber::TFrame::LocalFramePool->Free(WriterFrame);
  delete CurMemoryLayer;
}

void TDurableManager::CleanDisk(const Durable::TDeadline &/*now*/, Durable::TSem *sem) {
  sem->Push();
}

void TDurableManager::RunLayerCleaner() {
  LayerCleanerStarted = true;
  Base::TPushOnExit exit_latch(LayerCleanerExited);
  if (Engine->IsDiskBased()) {
    /* if this is a disk based engine, allocate event pools */
    assert(!Disk::Util::TDiskController::TEvent::LocalEventPool);
    Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
  }
  TDurableLayer *durable_layer = nullptr;
  for (;;) {
    LayerCleanerTimer.Pop();
    if (LayerCleanerStopping) {
      syslog(LOG_INFO, "TDurableManager::RunLayerCleaner shutting down (#440)");
      return;
    }
    KickDiskFullRetries();
    for (;;) {
      /* Acquire Removal lock */ {
        std::lock_guard<std::mutex> removal_lock(RemovalLock);
        durable_layer = RemovalCollection.TryGetFirstMember();
        if (durable_layer) {
          durable_layer->RemoveFromCollection();
        }
      }  // release Removal lock
      if (durable_layer) {
        delete durable_layer;
        durable_layer = nullptr;
      } else {
        break;
      }
    }
  }
}

void TDurableManager::StopLayerCleaner() {
  LayerCleanerStopping = true;
  LayerCleanerTimer.FireNow();
}

void TDurableManager::JoinLayerCleaner() {
  /* A cleaner whose fiber never got to run can't be waited for (and never
     touches us); one that did run pushes Exited on its way out. */
  if (LayerCleanerStarted) {
    LayerCleanerExited.Pop();
  }
}

bool TDurableManager::CanLoad(const Durable::TId &id) {
  TSequenceNumber cur_max_seq_num = 0UL;
  std::string serialized_form_out;
  TMapping::TView view(this);
  view.GetCurLayer()->FindMax(cur_max_seq_num, id, serialized_form_out);
  for (TMapping::TEntryCollection::TCursor csr(view.GetMapping()->GetEntryCollection()); csr; ++csr) {
    csr->GetLayer()->FindMax(cur_max_seq_num, id, serialized_form_out);
  }
  return cur_max_seq_num > 0UL;
}

void TDurableManager::Delete(const Durable::TId &/*id*/, Durable::TSem */*sem*/) {
  syslog(LOG_EMERG, "implement TDurableManager::Delete");
  throw;
}

void TDurableManager::Save(const Durable::TId &id, const Durable::TDeadline &deadline, const Durable::TTtl &ttl, const std::string &serialized_form, Durable::TSem *sem) {
  assert(serialized_form.size() > 0);
  if (serialized_form.size() >= UINT32_MAX) {
    throw std::runtime_error("Serialized Durable size >= UINT32_MAX");
  }
  TDurableReplication *durable_replication = Manager->NewDurableReplication(id, ttl, serialized_form);
  try {
    std::string temp = serialized_form;
    bool signal_now = false;
    /* acquire data lock */ {
      std::lock_guard<std::mutex> data_lock(DataLock);
      TSequenceNumber new_seq_num = ++SeqNum;
      assert(CurMemoryLayer);
      /* The saver's sem rides on the entry and is pushed by the writer fiber once the layer
         holding it has actually been written to disk (#277).  If the writer has already
         retired (we're shutting down and the final drain has run), nobody would ever push
         it, so fall back to signalling immediately -- the pre-#277 behavior, and the save
         is at-risk exactly as it always was at shutdown. */
      signal_now = WriterRetired;
      new TMemSlushLayer::TDurableEntry(CurMemoryLayer, id, deadline, std::move(temp), new_seq_num, signal_now ? nullptr : sem);
      Manager->EnqueueDurable(durable_replication);
    }  // release data lock
    SlushSem.Push();
    //SlushCounter.Push();
    if (signal_now && sem) {
      sem->Push();
    }
  } catch (...) {
    delete durable_replication;
    throw;
  }
}

bool TDurableManager::TryLoad(const Durable::TId &id, std::string &serialized_form_out) {
  TSequenceNumber cur_max_seq_num = 0UL;
  TMapping::TView view(this);
  view.GetCurLayer()->FindMax(cur_max_seq_num, id, serialized_form_out);
  for (TMapping::TEntryCollection::TCursor csr(view.GetMapping()->GetEntryCollection()); csr; ++csr) {
    csr->GetLayer()->FindMax(cur_max_seq_num, id, serialized_form_out);
  }
  return cur_max_seq_num > 0UL;
}

void TDurableManager::AddMapping(TDurableLayer *layer) {
  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> mapping_lock(MappingLock);
    TMapping *last = MappingCollection.TryGetLastMember();
    last->Incr();
    assert(last);
    try {
      TMapping *new_mapping = new TMapping(this);
      for (TMapping::TEntryCollection::TCursor csr(last->GetEntryCollection()); csr; ++csr) {
        new TMapping::TEntry(new_mapping, csr->GetLayer());
      }
      new TMapping::TEntry(new_mapping, layer);
      last->Decr();
    } catch (const std::exception &ex) {
      syslog(LOG_ERR, "Error in TDurableManager::AddMapping [%s]", ex.what());
      throw;
    }
  }  // release Mapping lock
}

void TDurableManager::Flush() {
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    /* acquire DataLayer lock */ {
      std::lock_guard<std::mutex> data_lock(DataLock);
      assert(CurMemoryLayer);
      if (!CurMemoryLayer->GetNumEntries()) {
        return;
      }
    }  // release DataLayer lock
    if (std::chrono::steady_clock::now() > give_up) {
      syslog(LOG_WARNING, "TDurableManager::Flush: gave up waiting for the slush layer to drain");
      return;
    }
    SlushSem.Push();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void TDurableManager::RunWriter() {
  if (Engine->IsDiskBased()) {
    /* if this is a disk based engine, allocate event pools */
    assert(!Disk::Util::TDiskController::TEvent::LocalEventPool);
    Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
  }
  /* The writer flushes as soon as a save signals it, without waiting out DurableWriteDelay
     (#576). A TFlush delay sat here, but SleepUntil never slept, so this is the behaviour
     every caller has relied on; turning on a write-behind would need its own measurement. */
  SlushSem.Pop();
  for (;!ShutDown; SlushSem.Pop()) {
    /* Out of disk space is handled inside: the layer stays readable in memory and is retried
       on the next save or layer-cleaner tick (#590). */
    FlushCurLayer(false);
  }
  /* Final drain (#277): flush whatever is still sitting in the memory layer so shutting down
     doesn't silently drop saves, and mark the writer retired (under DataLock) so any save that
     lands after this signals its sem itself rather than waiting for a writer that is gone.  If
     the disk service is already being torn down by force, the flush can throw -- release the
     stranded savers anyway (FlushCurLayer's catch does) and let the destructor proceed. */
  try {
    FlushCurLayer(true);
    if (!UnflushedLayers.empty()) {
      syslog(LOG_ERR, "TDurableManager::RunWriter final drain: %s; unflushed saves lost", WriterFailed ? "writer failed" : "out of disk space");
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "TDurableManager::RunWriter final drain failed; unflushed saves lost [%s]", ex.what());
  }
  WriterFinishedSem.Push();
}

void TDurableManager::ReleaseSavers(TMemSlushLayer *mem_layer) {
  assert(mem_layer);
  for (TMemSlushLayer::TEntryCollection::TCursor csr(mem_layer->GetEntryCollection()); csr; ++csr) {
    if (Durable::TSem *sem = csr->Sem) {
      csr->Sem = nullptr;
      sem->Push();
    }
  }
}

bool TDurableManager::FlushCurLayer(bool retire_writer) {
  /* First whatever an earlier round could not write for lack of disk space (#590). While that
     still fails, leave new saves in the current layer instead of starting one layer per round:
     layers come from a fixed pool. */
  const bool had_unflushed = !UnflushedLayers.empty();
  if (had_unflushed && !WriteUnflushedLayers()) {
    std::lock_guard<std::mutex> data_lock(DataLock);
    if (retire_writer) {
      WriterRetired = true;
    }
    ReleaseSavers(CurMemoryLayer);
    return true;
  }
  bool rotated = false;
  /* acquire DataLayer lock */ {
    std::lock_guard<std::mutex> data_lock(DataLock);
    assert(CurMemoryLayer);
    if (retire_writer) {
      WriterRetired = true;
    }
    if (CurMemoryLayer->GetNumEntries()) {
      AddMapping(CurMemoryLayer);
      UnflushedLayers.push_back(CurMemoryLayer);
      CurMemoryLayer = new TMemSlushLayer(this);
      rotated = true;
    }
  }  // release DataLayer lock
  if (rotated) {
    WriteUnflushedLayers();
  }
  return had_unflushed || rotated;
}

bool TDurableManager::WriteUnflushedLayers() {
  if (WriterFailed) {
    for (TMemSlushLayer *layer : UnflushedLayers) {
      ReleaseSavers(layer);
    }
    return UnflushedLayers.empty();
  }
  /* Oldest first. A layer that hits a full disk stays in the mapping, where reads still find
     it, and is written on a later round. */
  while (!UnflushedLayers.empty()) {
    TMemSlushLayer *mem_layer = UnflushedLayers.front();
    try {
      WriteMemLayer(mem_layer);
    } catch (const Disk::Util::TDiskFull &ex) {
      /* Don't hold the savers until space comes back: that could be never, and a session
         waiting on its save would stop answering reads too. Their saves stay readable from
         memory; durability waits for the retry, and a crash before it loses them. */
      for (TMemSlushLayer *layer : UnflushedLayers) {
        ReleaseSavers(layer);
      }
      WriterRetryDue = true;
      const size_t prev = WriterDiskFullStreak++;
      if (ShouldLogDiskFullRetry(prev)) {
        syslog(LOG_ERR, "TDurableManager writer out of disk space [%s] (try %ld); %ld layer(s) held in memory, retrying",
               ex.what(), prev + 1UL, UnflushedLayers.size());
      }
      return false;
    } catch (const std::exception &ex) {
      /* Anything else: the disk service is being shut down by force, or an I/O error, such as
         the file service refusing file-map changes after a failed fsync (#621). The data is
         not durable, but stranding the savers would deadlock whoever waits on them; wake them.
         Then stop writing for good: the layers stay in the mapping, readable, as on a full
         disk. Letting the error out of the writer fiber ended the fiber, and the destructor
         then waited forever for it to finish. */
      for (TMemSlushLayer *layer : UnflushedLayers) {
        ReleaseSavers(layer);
      }
      WriterFailed = true;
      const bool shutting_down = dynamic_cast<const Disk::TDiskServiceShutdown *>(&ex) != nullptr;
      syslog(shutting_down ? LOG_ERR : LOG_CRIT, "TDurableManager writer failed [%s]; %ld layer(s) held in memory, not written until restart",
             ex.what(), UnflushedLayers.size());
      return false;
    }
    UnflushedLayers.pop_front();
  }
  WriterRetryDue = false;
  if (const size_t retries = WriterDiskFullStreak.exchange(0UL)) {
    syslog(LOG_ERR, "TDurableManager writer flushed after %ld retries for disk space", retries);
  }
  return true;
}

void TDurableManager::WriteMemLayer(TMemSlushLayer *old_mem_layer) {
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;
  auto now = Durable::TDeadline::clock::now();
  size_t gen_id = ++NextDurableByIdGenId;
  TSortedByIdFile sorted_by_id_file(old_mem_layer,
                                    Engine,
                                    storage_speed,
                                    gen_id,
                                    now.time_since_epoch().count(),
                                    TempFileConsolThresh,
                                    Medium);
  /* The sorted-by-id file's constructor waits on its completion triggers, so by here every
     block of the new file is confirmed written: the saves in this layer are durable and we
     can finally release their savers (#277). */
  ReleaseSavers(old_mem_layer);
  MergeSem.Push();

  /* acquire Mapping lock */ {
    std::lock_guard<std::mutex> mapping_lock(MappingLock);
    TMapping *cur_mapping = MappingCollection.TryGetLastMember();
    cur_mapping->Incr();
    try {
      TMapping *new_mapping = new TMapping(this);
      for (TMapping::TEntryCollection::TCursor cur_csr(cur_mapping->GetEntryCollection()); cur_csr; ++cur_csr) {
        /* if it's not old_mem_layer, keep it */
        if (cur_csr->GetLayer() != old_mem_layer) {
          new TMapping::TEntry(new_mapping, cur_csr->GetLayer());
        } else {
          cur_csr->GetLayer()->MarkForDelete();
        }
      }
      /* add our new sorted file */
      new TMapping::TEntry(new_mapping, new TDiskOrderedLayer(this, Engine, gen_id, sorted_by_id_file.GetNumDurable()));
      cur_mapping->Decr();
    } catch (...) {
      cur_mapping->Decr();
      throw;
    }
  }  // release Mapping lock
}

bool TDurableManager::ShouldLogDiskFullRetry(size_t prev) {
  /* 1, 2, 4, 8, ... : enough to see it is still happening without filling the log. */
  return (prev & (prev + 1UL)) == 0UL;
}

void TDurableManager::KickDiskFullRetries() {
  if (WriterRetryDue) {
    SlushSem.Push();
  }
  if (MergerRetryDue) {
    MergeSem.Push();
  }
}

void TDurableManager::RunMerger() {
  if (Engine->IsDiskBased()) {
    /* if this is a disk based engine, allocate event pools */
    assert(!Disk::Util::TDiskController::TEvent::LocalEventPool);
    Disk::Util::TDiskController::TEvent::LocalEventPool = new TThreadLocalGlobalPoolManager<Disk::Util::TDiskController::TEvent>::TThreadLocalPool(Disk::Util::TDiskController::TEvent::DiskEventPoolManager.get());
  }
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;
  /* Unpaced, like the writer above (#576). */
  MergeSem.Pop();
  for (;!ShutDown; MergeSem.Pop()) {
    if (MergerFailed) {
      /* Stopped for good after an I/O error (#621); the inputs stay readable. */
      continue;
    }
    if (MergerRetryDue && std::chrono::steady_clock::now() < MergerRetryAt) {
      /* Backing off after a full disk; the layer cleaner's tick wakes us again. */
      continue;
    }
    std::vector<size_t> gen_vec;
    std::map<size_t, std::vector<TDiskOrderedLayer *>> gen_to_gen_id_map;
    std::vector<TDiskOrderedLayer *> gen_layer_vec;
    TMapping::TView view(this);
    for (TMapping::TEntryCollection::TCursor csr(view.GetMapping()->GetEntryCollection()); csr; ++csr) {
      if (csr->GetLayer()->GetKind() == TDurableLayer::DiskOrdered && !csr->GetLayer()->GetMarkedTaken()) {
        TDiskOrderedLayer *disk_layer = reinterpret_cast<TDiskOrderedLayer *>(csr->GetLayer());
        size_t suggested_gen = Util::SuggestGeneration(disk_layer->GetNumDurable());
        auto ret = gen_to_gen_id_map.insert(std::make_pair(suggested_gen, std::vector<TDiskOrderedLayer *>{disk_layer}));
        if (!ret.second) {
          ret.first->second.push_back(disk_layer);
        }
      }
    }
    bool done_picking = false;
    for (const auto &iter : gen_to_gen_id_map) {
      if (iter.second.size() >= 3) {
        if (!done_picking) {
          done_picking = true;
          for (auto layer : iter.second) {
            gen_vec.push_back(layer->GetGenId());
            gen_layer_vec.push_back(layer);
            assert(!layer->GetMarkedTaken());
            layer->MarkTaken();
          }
        }
        break;
      }
    }
    if (gen_vec.size()) {
      auto now = Durable::TDeadline::clock::now();
      size_t gen_id = ++NextDurableByIdGenId;
      std::optional<TMergeSortedByIdFile> merge_sort_file_storage;
      try {
        merge_sort_file_storage.emplace(gen_vec, Engine, storage_speed, gen_id, now.time_since_epoch().count(), TempFileConsolThresh, Low, Notify);
      } catch (const Disk::Util::TDiskFull &ex) {
        /* The output freed what it had reserved and the inputs are untouched, so hand them
           back and try again later (#590). Merging is only housekeeping: the inputs stay
           readable. A Notify would see a retried merge twice, but the server passes none. */
        for (auto layer : gen_layer_vec) {
          layer->UnmarkTaken();
        }
        const size_t prev = MergerDiskFullStreak++;
        const std::chrono::milliseconds backoff(100L << std::min<size_t>(prev, 5UL));
        MergerRetryAt = std::chrono::steady_clock::now() + backoff;
        MergerRetryDue = true;
        if (ShouldLogDiskFullRetry(prev)) {
          syslog(LOG_ERR, "TDurableManager merger out of disk space [%s] (try %ld); inputs handed back, retrying in %ldms",
                 ex.what(), prev + 1UL, static_cast<long>(backoff.count()));
        }
        continue;
      } catch (const std::exception &ex) {
        /* Any other error: the disk service is shutting down, or an I/O error, such as the file
           service refusing file-map changes (#621). Hand the inputs back, as above, and stop
           merging; letting the error out of the merger fiber ended the fiber, and the
           destructor then waited forever for it to finish. */
        for (auto layer : gen_layer_vec) {
          layer->UnmarkTaken();
        }
        MergerFailed = true;
        const bool shutting_down = dynamic_cast<const Disk::TDiskServiceShutdown *>(&ex) != nullptr;
        syslog(shutting_down ? LOG_ERR : LOG_CRIT, "TDurableManager merger failed [%s]; not merging until restart", ex.what());
        continue;
      }
      MergerRetryDue = false;
      if (const size_t retries = MergerDiskFullStreak.exchange(0UL)) {
        syslog(LOG_ERR, "TDurableManager merger succeeded after %ld retries for disk space", retries);
      }
      TMergeSortedByIdFile &merge_sort_file = *merge_sort_file_storage;
      /* acquire Mapping lock */ {
        std::lock_guard<std::mutex> mapping_lock(MappingLock);
        TMapping *cur_mapping = MappingCollection.TryGetLastMember();
        cur_mapping->Incr();
        try {
          TMapping *new_mapping = new TMapping(this);
          for (TMapping::TEntryCollection::TCursor cur_csr(cur_mapping->GetEntryCollection()); cur_csr; ++cur_csr) {
            /* keep it if it's not one we just merged */
            bool found = false;

            for (auto layer : gen_layer_vec) {
              if (layer == cur_csr->GetLayer()) {
                layer->MarkForDelete();
                found = true;
                break;
              }
            }
            if (!found) {
              new TMapping::TEntry(new_mapping, cur_csr->GetLayer());
            }
          }
          /* add our new sorted file */
          new TMapping::TEntry(new_mapping, new TDiskOrderedLayer(this, Engine, gen_id, merge_sort_file.GetNumDurable()));
          cur_mapping->Decr();
        } catch (...) {
          cur_mapping->Decr();
          throw;
        }
      }  // release Mapping lock
      MergeSem.Push();
    }
  }
  MergerFinishedSem.Push();
}

TDurableManager::TSortedByIdFile::TSortedByIdFile(TMemSlushLayer *mem_layer,
                                                  Util::TEngine *engine,
                                                  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                                                  size_t gen_id,
                                                  size_t latest_deadline_count,
                                                  size_t temp_file_consol_thresh,
                                                  DiskPriority priority)
    : Engine(engine), StorageSpeed(storage_speed), FileSize(0UL), NumDurable(0UL) {
  bool file_inserted = false;
  try {
    Write(mem_layer, gen_id, latest_deadline_count, temp_file_consol_thresh, priority, file_inserted);
  } catch (...) {
    /* Nothing else will free the blocks of a file that never reached the file map (#590). */
    if (!file_inserted) {
      Engine->FreeAllBlocks(BlockVec);
    }
    throw;
  }
}

void TDurableManager::TSortedByIdFile::Write(TMemSlushLayer *mem_layer,
                                             size_t gen_id,
                                             size_t latest_deadline_count,
                                             size_t temp_file_consol_thresh,
                                             DiskPriority priority,
                                             bool &file_inserted) {
  THashSorter hash_sorter(HERE, Source::DurableSortFileHashIndex, temp_file_consol_thresh, StorageSpeed, Engine, true);
  assert(mem_layer);
  assert(Engine);
  size_t total_durable_serialized_space = 0UL;
  size_t total_bytes = 0UL;
  size_t num_blocks = 0UL;
  size_t num_hash_fields = 0UL;

  size_t byte_offset_of_hash_index = 0UL;

  TCompletionTrigger completion_trigger;

  /* calculate some offset */ {
    std::optional<Base::TUuid> last_id;
    for (TMemSlushLayer::TEntryCollection::TCursor csr(mem_layer->GetEntryCollection()); csr; ++csr) {
      TSerializedSize serialized_size = (*csr).GetSerializedSize();
      const uuid_t &cur_id = (*csr).GetId();
      const size_t cur_deadline_count = (*csr).GetDeadlineCount();
      if (!last_id || uuid_compare(cur_id, last_id->GetRaw()) != 0) {
        last_id = TUuid(cur_id);
        if (cur_deadline_count >= latest_deadline_count) {
          ++NumDurable;
          total_durable_serialized_space += serialized_size;  // size of serialized durable
        }
      }
    }

    num_hash_fields = Util::SuggestHashSize(NumDurable);

    total_bytes += NumMetaFields * sizeof(size_t);
    total_bytes += NumDurable * DurableEntrySize;
    total_bytes += total_durable_serialized_space;
    total_bytes += num_hash_fields * HashEntrySize;
  }

  /* calculate the number of blocks. */
  num_blocks = ceil(static_cast<double>(total_bytes) / Disk::Util::LogicalBlockSize);
  size_t num_bytes_for_block_data = num_blocks * sizeof(size_t);
  num_blocks = ceil(static_cast<double>(total_bytes + num_bytes_for_block_data) / Disk::Util::LogicalBlockSize);
  num_bytes_for_block_data = num_blocks * sizeof(size_t);
  num_blocks = ceil(static_cast<double>(total_bytes + num_bytes_for_block_data) / Disk::Util::LogicalBlockSize);
  num_bytes_for_block_data = num_blocks * sizeof(size_t);
  num_blocks = ceil(static_cast<double>(total_bytes + num_bytes_for_block_data) / Disk::Util::LogicalBlockSize);

  byte_offset_of_hash_index = (NumMetaFields + num_blocks) * sizeof(size_t);
  byte_offset_of_hash_index += NumDurable * DurableEntrySize;
  byte_offset_of_hash_index += total_durable_serialized_space;

  /* reserve the blocks. */
  /* Sessions and POVs are saved here: these files may use the space kept back from data (#590). */
  Engine->AppendReserveBlocks(StorageSpeed, num_blocks, BlockVec, Util::TAllocClass::Essential);
  #ifndef NDEBUG
  std::unordered_set<size_t> written_block_set;
  #endif

  unordered_map<size_t, shared_ptr<const TBufBlock>> block_collision_map;

  /* set up block collision map */ {
    auto ret = block_collision_map.insert(make_pair(byte_offset_of_hash_index / Disk::Util::LogicalBlockSize, nullptr));
    if (ret.second) { // fresh insert
      ret.first->second = std::shared_ptr<const TBufBlock>(new TBufBlock());
    }
  }

  /* write out the file */ {
    TDataOutStream out_stream(HERE,
                              Source::DurableSortFileData,
                              Engine->GetVolMan(),
                              0UL,
                              BlockVec,
                              block_collision_map,
                              completion_trigger,
                              priority,
                              true
                              #ifndef NDEBUG
                              ,written_block_set
                              #endif
                              );

    out_stream << NumDurable;  // NumEntries
    out_stream << num_blocks;  // NumBlocks
    out_stream << byte_offset_of_hash_index;  // HashIndexOffset
    out_stream << num_hash_fields;  // HashFieldSize

    /* write out the block ids */
    for (auto iter : BlockVec) {
      out_stream << iter;  // block_id
    }

    /* write out num_entries * (DurableEntry + string)
       1. durable id
       2. seq_num
       3. deadline count
       4. size of serialized string.
       5. serialized string.
    */
    size_t key_offset = 0UL;
    std::optional<Base::TUuid> last_id;
    for (TMemSlushLayer::TEntryCollection::TCursor csr(mem_layer->GetEntryCollection()); csr; ++csr) {



      TSerializedSize serialized_size = (*csr).GetSerializedSize();
      const uuid_t &cur_id = (*csr).GetId();
      const size_t cur_deadline_count = (*csr).GetDeadlineCount();
      if (!last_id || uuid_compare(cur_id, last_id->GetRaw()) != 0) {
        last_id = TUuid(cur_id);
        if (cur_deadline_count >= latest_deadline_count) {
          const size_t id_hash = *reinterpret_cast<size_t *>(const_cast<unsigned char *>(cur_id));
          out_stream.Write(&cur_id, sizeof(uuid_t));  // durable_id
          out_stream << (*csr).GetSeqNum();  // seq_num
          out_stream << (*csr).GetDeadlineCount();  // deadline_count
          out_stream << (*csr).GetSerializedSize();  // size of serialized string
          out_stream.Write((*csr).GetSerializedForm().data(), serialized_size); /* the serialized string. */
          hash_sorter.Emplace(cur_id, id_hash % num_hash_fields, key_offset);
          key_offset += DurableEntrySize + serialized_size;
        }
      }
    }
    FileSize = out_stream.GetOffset();
  }
  /* write out the hash index */ {
    THashSorter::TCursor hash_csr(&hash_sorter, 16UL);

    /* do the first pass over the hash map */ {
      TDataOutStream stream(HERE,
                            Source::DurableSortFileHash,
                            Engine->GetVolMan(),
                            byte_offset_of_hash_index,
                            BlockVec,
                            block_collision_map,
                            completion_trigger,
                            priority,
                            true
                            #ifndef NDEBUG
                            ,written_block_set
                            #endif
                            );

      for (size_t i = 0; i < num_hash_fields; ++i) {
        if (hash_csr && (*hash_csr).Hash <= i) {
          const THashObj &obj = *hash_csr;
          stream.Write(&obj.Id, sizeof(uuid_t));
          stream << obj.Offset;
          ++hash_csr;
        } else {
          stream.Write(&NullId.GetRaw(), sizeof(uuid_t));
          stream << 0UL;
        }
      }
      FileSize = stream.GetOffset();
    }  // do the first pass over the hash map
    if (hash_csr) {
      /* flush the collision blocks surrounding the hash map */ {
        Engine->GetVolMan()->WriteBlock(HERE,
                                        Disk::Util::PageCheckedBlock,
                                        Source::DurableSortFileHash,
                                        block_collision_map[byte_offset_of_hash_index / Disk::Util::LogicalBlockSize]->GetData(),
                                        BlockVec[byte_offset_of_hash_index / Disk::Util::LogicalBlockSize],
                                        priority,
                                        Disk::Util::CacheAll,
                                        completion_trigger);
      }
      /* wait for the first pass to finish flushing before doing the wrap around pass */ {
        completion_trigger.Wait();
      }

      TDataOutStream out_stream(HERE,
                                Source::DurableSortFileHash,
                                Engine->GetVolMan(),
                                byte_offset_of_hash_index,
                                BlockVec,
                                block_collision_map,
                                completion_trigger,
                                priority,
                                true
                                #ifndef NDEBUG
                                ,written_block_set
                                #endif
                                );
      TMyFile my_file(this);
      TInStream in_stream(HERE, Source::DurableSortFileHash, priority, &my_file, Engine->GetPageCache(), byte_offset_of_hash_index);
      uuid_t cur_id;
      size_t key_ptr = 0;
      for (size_t i = 0; i < num_hash_fields; ++i) {
        in_stream.Read(&cur_id, sizeof(uuid_t));
        in_stream.Read(key_ptr);
        if (memcmp(&cur_id, &NullId.GetRaw(), sizeof(uuid_t)) == 0) {
          assert(hash_csr);

          const THashObj &obj = *hash_csr;
          out_stream.Write(&obj.Id, sizeof(uuid_t));
          out_stream << obj.Offset;
          ++hash_csr;
        } else {
          out_stream.Write(&cur_id, sizeof(uuid_t));
          out_stream << key_ptr;
        }
        if (!hash_csr) {
          break;
        }
      }
      size_t left = std::min(Disk::Util::LogicalBlockSize - in_stream.GetOffset() % Disk::Util::LogicalBlockSize, FileSize - in_stream.GetOffset());
      char buf[left];
      in_stream.Read(buf, left);
      out_stream.Write(buf, left);
    }
    assert(!hash_csr);
  }
  /* flush collision blocks */ {
    for (auto iter : block_collision_map) {
      Engine->GetVolMan()->WriteBlock(HERE,
                                      Disk::Util::PageCheckedBlock,
                                      Source::DurableSortFileOther,
                                      iter.second->GetData(),
                                      BlockVec[iter.first],
                                      priority,
                                      Disk::Util::CacheAll,
                                      completion_trigger);
    }
    completion_trigger.Wait();
  }

  /* wait for the pages to get flushed */ {
    completion_trigger.Wait();
  }
  SyncDurableFileBlocks(Engine, BlockVec);
  /* wait for file entry to flush */ {
    file_inserted = true;
    Engine->InsertFile(DurableByIdFileId, TFileObj::TKind::DurableFile, gen_id, BlockVec.Front(), 0UL, BlockVec.Size() * Disk::Util::LogicalBlockSize, NumDurable, 0UL, 0UL, completion_trigger);
    completion_trigger.Wait();
  }
}

TDurableManager::TSortedInFile::TSortedInFile(Util::TPageCache *page_cache,
                                              DiskPriority priority,
                                              size_t /*gen_id*/,
                                              size_t starting_block_id,
                                              size_t starting_block_offset,
                                              size_t file_length)
    : PageCache(page_cache),
      FileLength(file_length),
      StartingBlockId(starting_block_id),
      StartingBlockOffset(starting_block_offset) {
  InStream = std::make_unique<TInStream>(HERE, Source::DurableFetch, priority, this, PageCache, 0);
  InStream->Read(NumEntries);
  InStream->Read(NumBlocks);
  InStream->Read(HashIndexOffset);
  InStream->Read(HashFieldSize);
}

TDurableManager::TSortedInFile::TSortedInFile(Util::TEngine *engine, DiskPriority priority, size_t gen_id)
    : PageCache(engine->GetPageCache()) {
  size_t num_keys;
  if (!engine->FindFile(DurableByIdFileId, gen_id, StartingBlockId, StartingBlockOffset, FileLength, num_keys)) {
    std::ostringstream ss;
    ss << DurableByIdFileId;
    syslog(LOG_ERR, "TSortedInFile() Can not find file %s[%ld]", ss.str().c_str(), gen_id);
    throw std::runtime_error("Could not find file.");
  }
  InStream = std::make_unique<TInStream>(HERE, Source::DurableFetch, priority, this, PageCache, 0);
  InStream->Read(NumEntries);
  InStream->Read(NumBlocks);
  InStream->Read(HashIndexOffset);
  InStream->Read(HashFieldSize);
}

TDurableManager::TSortedInFile::~TSortedInFile() {}

size_t TDurableManager::TSortedInFile::GetFileLength() const {
  return FileLength;
}

size_t TDurableManager::TSortedInFile::GetStartingBlock() const {
  return StartingBlockId;
}

void TDurableManager::TSortedInFile::ReadMeta(size_t offset, size_t &out) const {
  assert(InStream);
  size_t cur_offset = InStream->GetOffset();
  assert(cur_offset < FileLength);
  InStream->GoTo(offset);
  InStream->Read(out);
  InStream->GoTo(cur_offset);
}

size_t TDurableManager::TSortedInFile::FindPageIdOfByte(size_t offset) const {
  assert(offset < FileLength);
  size_t num_blocks_into_file = offset / Disk::Util::LogicalBlockSize;
  if (num_blocks_into_file == 0) {
    return (StartingBlockId * Disk::Util::PagesPerBlock) + ((offset % Disk::Util::LogicalBlockSize) / Disk::Util::LogicalPageSize);
  } else {
    size_t byte_offset_of_layout_block = (TSortedByIdFile::NumMetaFields + num_blocks_into_file) * sizeof(size_t);
    size_t ret;
    ReadMeta(byte_offset_of_layout_block, ret);
    ret = (ret * Disk::Util::PagesPerBlock) + ((offset % Disk::Util::LogicalBlockSize) / Disk::Util::LogicalPageSize);
    return ret;
  }
}

void TDurableManager::TSortedInFile::FindInHash(TSequenceNumber &cur_max_seq_num, const Durable::TId &id, std::string &serialized_form_out) const {
  assert(InStream);
  uuid_t cur_id;
  TInStream hash_stream(HERE, Source::DurableMergeFileHash, RealTime, this, PageCache, HashIndexOffset);
  const size_t id_hash = *reinterpret_cast<size_t *>(const_cast<unsigned char *>(id.GetRaw()));
  size_t hash = id_hash % HashFieldSize;
  hash_stream.GoTo(HashIndexOffset + hash * (sizeof(uuid_t) + sizeof(size_t)));
  bool done = false;
  for (size_t i = hash; i < HashFieldSize && !done; ++i) {
    hash_stream.Read(&cur_id, sizeof(uuid_t));
    if (memcmp(&cur_id, &TSortedByIdFile::NullId.GetRaw(), sizeof(uuid_t)) == 0) {
      done = true;
    } else {
      if (uuid_compare(id.GetRaw(), cur_id) == 0) {
        size_t byte_offset_of_durable;
        hash_stream.Read(byte_offset_of_durable);
        InStream->GoTo(GetStartOfDurableByIdIndex() + byte_offset_of_durable);
        uuid_t found_id;
        TSequenceNumber cur_seq;
        InStream->Read(&found_id, sizeof(uuid_t));
        InStream->Read(cur_seq);
        if (cur_seq > cur_max_seq_num) {
          size_t cur_deadline;
          TSerializedSize cur_serialized_size;
          cur_max_seq_num = cur_seq;
          InStream->Read(cur_deadline);
          InStream->Read(cur_serialized_size);
          serialized_form_out.resize(cur_serialized_size);
          InStream->Read(const_cast<char *>(serialized_form_out.data()), cur_serialized_size);
        }
        return;
      } else if (*reinterpret_cast<size_t *>(const_cast<unsigned char *>(cur_id)) % HashFieldSize > hash) {
        return;
      } else {
        hash_stream.Skip(sizeof(size_t));
      }
    }
  }
  if (!done) {
    hash_stream.GoTo(HashIndexOffset);
    for (size_t i = 0; i < hash && !done; ++i) {
      hash_stream.Read(&cur_id, sizeof(uuid_t));
      if (memcmp(&cur_id, &TSortedByIdFile::NullId.GetRaw(), sizeof(uuid_t)) == 0) {
        done = true;
      } else {
        if (uuid_compare(id.GetRaw(), cur_id) == 0) {
          size_t byte_offset_of_durable;
          hash_stream.Read(byte_offset_of_durable);
          InStream->GoTo(GetStartOfDurableByIdIndex() + byte_offset_of_durable);
          uuid_t found_id;
          TSequenceNumber cur_seq;
          InStream->Read(&found_id, sizeof(uuid_t));
          InStream->Read(cur_seq);
          if (cur_seq > cur_max_seq_num) {
            size_t cur_deadline;
            TSerializedSize cur_serialized_size;
            cur_max_seq_num = cur_seq;
            InStream->Read(cur_deadline);
            InStream->Read(cur_serialized_size);
            serialized_form_out.resize(cur_serialized_size);
            InStream->Read(const_cast<char *>(serialized_form_out.data()), cur_serialized_size);
          }
          return;
        } else if (*reinterpret_cast<size_t *>(const_cast<unsigned char *>(cur_id)) % HashFieldSize > hash) {
          return;
        } else {
          hash_stream.Skip(sizeof(size_t));
        }
      }
    }
  }
}

TDurableManager::TSequenceNumber TDurableManager::TSortedInFile::FindMaxSeqNum(DiskPriority priority) const {
  TSequenceNumber max_seq_num = 0UL;
  if (!NumEntries) {
    return max_seq_num;
  }
  /* Each entry: durable id, seq_num, deadline count, serialized size, serialized form. */
  TInStream entry_stream(HERE, Source::DurableFetch, priority, this, PageCache, GetStartOfDurableByIdIndex());
  TSequenceNumber cur_seq;
  size_t cur_deadline_count;
  TSerializedSize cur_serialized_size;
  for (size_t i = 0; i < NumEntries; ++i) {
    entry_stream.Skip(sizeof(uuid_t));
    entry_stream.Read(&cur_seq, sizeof(TSequenceNumber));
    entry_stream.Read(cur_deadline_count);
    entry_stream.Read(cur_serialized_size);
    entry_stream.Skip(cur_serialized_size);
    max_seq_num = std::max(max_seq_num, cur_seq);
  }
  return max_seq_num;
}

TDurableManager::TMergeSortedByIdFile::TMergeSortedByIdFile(const std::vector<size_t> &gen_vec,
                                                            Util::TEngine *engine,
                                                            Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                                                            size_t gen_id,
                                                            size_t latest_deadline_count,
                                                            size_t temp_file_consol_thresh,
                                                            DiskPriority priority,
                                                            const TNotify *notify)
    : Engine(engine), StorageSpeed(storage_speed), NumDurable(0UL), FileSize(0UL) {
  bool file_inserted = false;
  try {
    Write(gen_vec, gen_id, latest_deadline_count, temp_file_consol_thresh, priority, notify, file_inserted);
  } catch (...) {
    /* As in TSortedByIdFile (#590). */
    if (!file_inserted) {
      Engine->FreeAllBlocks(BlockVec);
    }
    throw;
  }
}

void TDurableManager::TMergeSortedByIdFile::Write(const std::vector<size_t> &gen_vec,
                                                  size_t gen_id,
                                                  size_t latest_deadline_count,
                                                  size_t temp_file_consol_thresh,
                                                  DiskPriority priority,
                                                  const TNotify *notify,
                                                  bool &file_inserted) {
  THashSorter hash_sorter(HERE, Source::DurableMergeFileHashIndex, temp_file_consol_thresh, StorageSpeed, Engine, true);
  std::vector<std::unique_ptr<TSortedInFile>> in_file_vec;
  for (auto iter : gen_vec) {
    in_file_vec.push_back(std::make_unique<TSortedInFile>(Engine, priority, iter));
  }

  size_t total_durable_serialized_space = 0UL;
  size_t total_bytes = 0UL;
  size_t num_blocks = 0UL;
  size_t num_hash_fields = 0UL;

  size_t byte_offset_of_hash_index = 0UL;

  TCompletionTrigger completion_trigger;

  /* calculations */ {

    TDurableSorter<size_t> sorter;
    TDurableSorter<size_t>::TMergeElement *durable_sorter_alloc = 0;
    std::vector<std::unique_ptr<TInStream>> in_stream_vec;
    std::vector<size_t> entries_left_vec;
    size_t pos = 0U;
    try {
      durable_sorter_alloc = reinterpret_cast<TDurableSorter<size_t>::TMergeElement *>(malloc(sizeof(TDurableSorter<size_t>::TMergeElement) * in_file_vec.size()));
      uuid_t cur_id;
      TSequenceNumber cur_seq;
      for (const std::unique_ptr<TSortedInFile> &iter : in_file_vec) {
        size_t num_entries_in_file = iter->GetNumEntries();
        size_t byte_offset_of_durable_index = iter->GetStartOfDurableByIdIndex();

        entries_left_vec.push_back(num_entries_in_file);
        in_stream_vec.push_back(std::make_unique<TInStream>(HERE, Source::DurableMergeFileScan, priority, iter.get(), Engine->GetPageCache(), byte_offset_of_durable_index));

        if (entries_left_vec.back() > 0) {
          TInStream &entry_stream = *in_stream_vec.back();
          entry_stream.Read(&cur_id, sizeof(uuid_t));
          entry_stream.Read(&cur_seq, sizeof(TSequenceNumber));
          new (durable_sorter_alloc + pos) TDurableSorter<size_t>::TMergeElement(&sorter, cur_id, cur_seq, pos);
          --entries_left_vec.back();
        }
        ++pos;
      }

      size_t cur_deadline_count;
      TSerializedSize cur_serialized_size;
      std::optional<Base::TUuid> last_id;
      while (!sorter.IsEmpty()) {
        TSequenceNumber pop_num;
        size_t pos = sorter.Pop(pop_num, cur_id);
        TInStream &entry_stream = *in_stream_vec[pos];
        entry_stream.Read(cur_deadline_count);
        entry_stream.Read(cur_serialized_size);
        entry_stream.Skip(cur_serialized_size);
        if (!last_id || uuid_compare(cur_id, last_id->GetRaw()) != 0) {
          last_id = TUuid(cur_id);
          if (cur_deadline_count >= latest_deadline_count) {
            ++NumDurable;
            total_durable_serialized_space += cur_serialized_size;  // size of serialized durable
          }
        }
        if (entries_left_vec[pos] > 0) {
          entry_stream.Read(&cur_id, sizeof(uuid_t));
          entry_stream.Read(&cur_seq, sizeof(TSequenceNumber));
          new (durable_sorter_alloc + pos) TDurableSorter<size_t>::TMergeElement(&sorter, cur_id, cur_seq, pos);
          --entries_left_vec[pos];
        }
      }
      free(durable_sorter_alloc);
    } catch (...) {
      free(durable_sorter_alloc);
      throw;
    }

    num_hash_fields = Util::SuggestHashSize(NumDurable);

    total_bytes += TSortedByIdFile::NumMetaFields * sizeof(size_t);
    total_bytes += NumDurable * TSortedByIdFile::DurableEntrySize;
    total_bytes += total_durable_serialized_space;
    total_bytes += num_hash_fields * TSortedByIdFile::HashEntrySize;

    /* calculate the number of blocks. */
    num_blocks = ceil(static_cast<double>(total_bytes) / Disk::Util::LogicalBlockSize);
    size_t num_bytes_for_block_data = num_blocks * sizeof(size_t);
    num_blocks = ceil(static_cast<double>(total_bytes + num_bytes_for_block_data) / Disk::Util::LogicalBlockSize);
    num_bytes_for_block_data = num_blocks * sizeof(size_t);
    num_blocks = ceil(static_cast<double>(total_bytes + num_bytes_for_block_data) / Disk::Util::LogicalBlockSize);
    num_bytes_for_block_data = num_blocks * sizeof(size_t);
    num_blocks = ceil(static_cast<double>(total_bytes + num_bytes_for_block_data) / Disk::Util::LogicalBlockSize);

    byte_offset_of_hash_index = (TSortedByIdFile::NumMetaFields + num_blocks) * sizeof(size_t);
    byte_offset_of_hash_index += NumDurable * TSortedByIdFile::DurableEntrySize;
    byte_offset_of_hash_index += total_durable_serialized_space;
  }

  /* reserve the blocks. */
  /* Essential like the writer's files (#590): each save of a session or POV (when a connection
     closes, say) supersedes its previous copy, so the writer keeps producing superseded copies,
     and this merge is what frees them. */
  Engine->AppendReserveBlocks(StorageSpeed, num_blocks, BlockVec, Util::TAllocClass::Essential);
  #ifndef NDEBUG
  std::unordered_set<size_t> written_block_set;
  #endif

  unordered_map<size_t, shared_ptr<const TBufBlock>> block_collision_map;
  /* fill in the block collision map */
  /* collision between durable index and string heap */
  auto ret = block_collision_map.insert(make_pair(byte_offset_of_hash_index / Disk::Util::LogicalBlockSize, nullptr));
  if (ret.second) { // fresh insert
    ret.first->second = std::shared_ptr<const TBufBlock>(new TBufBlock());
  }
  /* write out the file */ {
    TDataOutStream index_stream(HERE,
                                Source::DurableMergeFileData,
                                Engine->GetVolMan(),
                                0UL,
                                BlockVec,
                                block_collision_map,
                                completion_trigger,
                                priority,
                                true
                                #ifndef NDEBUG
                                ,written_block_set
                                #endif
                                );

    index_stream << NumDurable;  // NumEntries
    index_stream << num_blocks;  // NumBlocks
    index_stream << byte_offset_of_hash_index;  // HashIndexOffset
    index_stream << num_hash_fields;  // HashFieldSize

    /* write out the block ids */
    for (auto iter : BlockVec) {
      index_stream << iter;  // block_id
    }

    /* write out num_entries * (DurableEntry + serialized string)
       1. durable id
       2. seq_num
       3. deadline count
       4. size of serialized string.
       5. serialized string
    */

    TDurableSorter<size_t> sorter;
    TDurableSorter<size_t>::TMergeElement *durable_sorter_alloc = 0;
    std::vector<std::unique_ptr<TInStream>> index_in_stream_vec;
    std::vector<size_t> entries_left_vec;
    size_t pos = 0U;
    void *temp_space = nullptr;
    try {
      durable_sorter_alloc = reinterpret_cast<TDurableSorter<size_t>::TMergeElement *>(malloc(sizeof(TDurableSorter<size_t>::TMergeElement) * in_file_vec.size()));
      uuid_t cur_id;
      TSequenceNumber cur_seq;
      for (const std::unique_ptr<TSortedInFile> &iter : in_file_vec) {
        size_t num_entries_in_file = iter->GetNumEntries();
        size_t byte_offset_of_durable_index = iter->GetStartOfDurableByIdIndex();

        entries_left_vec.push_back(num_entries_in_file);
        index_in_stream_vec.push_back(std::make_unique<TInStream>(HERE, Source::DurableMergeFileScan, priority, iter.get(), Engine->GetPageCache(), byte_offset_of_durable_index));

        if (entries_left_vec.back() > 0) {
          TInStream &index_in_stream = *index_in_stream_vec.back();
          index_in_stream.Read(&cur_id, sizeof(uuid_t));
          index_in_stream.Read(&cur_seq, sizeof(TSequenceNumber));
          new (durable_sorter_alloc + pos) TDurableSorter<size_t>::TMergeElement(&sorter, cur_id, cur_seq, pos);
          --entries_left_vec.back();
        }
        ++pos;
      }

      size_t cur_deadline_count;
      TSerializedSize cur_serialized_size;
      std::optional<Base::TUuid> last_id;
      size_t key_offset = 0UL;
      while (!sorter.IsEmpty()) {
        TSequenceNumber pop_num;
        size_t pos = sorter.Pop(pop_num, cur_id);
        TInStream &index_in_stream = *index_in_stream_vec[pos];
        index_in_stream.Read(cur_deadline_count);
        index_in_stream.Read(cur_serialized_size);
        if (!last_id || uuid_compare(cur_id, last_id->GetRaw()) != 0) {
          last_id = TUuid(cur_id);
          if (cur_deadline_count >= latest_deadline_count) {
            const size_t id_hash = *reinterpret_cast<size_t *>(const_cast<unsigned char *>(cur_id));
            index_stream.Write(&cur_id, sizeof(uuid_t));  // durable_id
            index_stream << pop_num;  // seq_num
            index_stream << cur_deadline_count;  // deadline_count
            index_stream << cur_serialized_size;  // size of serialized string
            if ((temp_space = realloc(temp_space, cur_serialized_size)) == 0) {
              free(temp_space);
              temp_space = nullptr;
              syslog(LOG_EMERG, "bad alloc in DurableManager realloc [%d]", cur_serialized_size);
              throw std::bad_alloc();
            }
            index_in_stream.Read(temp_space, cur_serialized_size);
            index_stream.Write(temp_space, cur_serialized_size);
            hash_sorter.Emplace(cur_id, id_hash % num_hash_fields, key_offset);
            key_offset += TSortedByIdFile::DurableEntrySize + cur_serialized_size;
            if (notify) {
              (*notify)(pop_num, cur_id, Survived);
            }
          } else {
            index_in_stream.Skip(cur_serialized_size);
            if (notify) {
              (*notify)(pop_num, cur_id, Expired);
            }
          }
        } else {
          index_in_stream.Skip(cur_serialized_size);
          if (notify) {
            (*notify)(pop_num, cur_id, WasSuperceded);
          }
        }
        if (entries_left_vec[pos] > 0) {
          index_in_stream.Read(&cur_id, sizeof(uuid_t));
          index_in_stream.Read(&cur_seq, sizeof(TSequenceNumber));
          new (durable_sorter_alloc + pos) TDurableSorter<size_t>::TMergeElement(&sorter, cur_id, cur_seq, pos);
          --entries_left_vec[pos];
        }
      }
      free(temp_space);
      free(durable_sorter_alloc);
    } catch (...) {
      free(temp_space);
      free(durable_sorter_alloc);
      throw;
    }
  }
  /* write out the hash index */ {
    THashSorter::TCursor hash_csr(&hash_sorter, 16UL);

    /* do the first pass over the hash map */ {
      TDataOutStream stream(HERE,
                            Source::DurableMergeFileHash,
                            Engine->GetVolMan(),
                            byte_offset_of_hash_index,
                            BlockVec,
                            block_collision_map,
                            completion_trigger,
                            priority,
                            true
                            #ifndef NDEBUG
                            ,written_block_set
                            #endif
                            );

      for (size_t i = 0; i < num_hash_fields; ++i) {
        if (hash_csr && (*hash_csr).Hash <= i) {
          const THashObj &obj = *hash_csr;
          stream.Write(&obj.Id, sizeof(uuid_t));
          stream << obj.Offset;
          ++hash_csr;
        } else {
          stream.Write(&TSortedByIdFile::NullId.GetRaw(), sizeof(uuid_t));
          stream << 0UL;
        }
      }
      FileSize = stream.GetOffset();
    }  // do the first pass over the hash map
    if (hash_csr) {
      /* flush the collision blocks surrounding the hash map */ {
        Engine->GetVolMan()->WriteBlock(HERE,
                                        Disk::Util::PageCheckedBlock,
                                        Source::DurableMergeFileHash,
                                        block_collision_map[byte_offset_of_hash_index / Disk::Util::LogicalBlockSize]->GetData(),
                                        BlockVec[byte_offset_of_hash_index / Disk::Util::LogicalBlockSize],
                                        priority,
                                        Disk::Util::CacheAll,
                                        completion_trigger);
      }
      /* wait for the first pass to finish flushing before doing the wrap around pass */ {
        completion_trigger.Wait();
      }

      TDataOutStream out_stream(HERE,
                                Source::DurableMergeFileHash,
                                Engine->GetVolMan(),
                                byte_offset_of_hash_index,
                                BlockVec,
                                block_collision_map,
                                completion_trigger,
                                priority,
                                true
                                #ifndef NDEBUG
                                ,written_block_set
                                #endif
                                );
      TMyFile my_file(this);
      TInStream in_stream(HERE, Source::DurableMergeFileHash, priority, &my_file, Engine->GetPageCache(), byte_offset_of_hash_index);
      uuid_t cur_id;
      size_t key_ptr = 0;
      for (size_t i = 0; i < num_hash_fields; ++i) {
        in_stream.Read(&cur_id, sizeof(uuid_t));
        in_stream.Read(key_ptr);
        if (memcmp(&cur_id, &TSortedByIdFile::NullId.GetRaw(), sizeof(uuid_t)) == 0) {
          assert(hash_csr);

          const THashObj &obj = *hash_csr;
          out_stream.Write(&obj.Id, sizeof(uuid_t));
          out_stream << obj.Offset;
          ++hash_csr;
        } else {
          out_stream.Write(&cur_id, sizeof(uuid_t));
          out_stream << key_ptr;
        }
        if (!hash_csr) {
          break;
        }
      }
      size_t left = std::min(Disk::Util::LogicalBlockSize - in_stream.GetOffset() % Disk::Util::LogicalBlockSize, FileSize - in_stream.GetOffset());
      char buf[left];
      in_stream.Read(buf, left);
      out_stream.Write(buf, left);
    }
    assert(!hash_csr);
  }
  /* flush collision blocks */ {
    for (auto iter : block_collision_map) {
      Engine->GetVolMan()->WriteBlock(HERE,
                                      Disk::Util::PageCheckedBlock,
                                      Source::DurableMergeFileOther,
                                      iter.second->GetData(),
                                      BlockVec[iter.first],
                                      priority,
                                      Disk::Util::CacheAll,
                                      completion_trigger);
    }
    completion_trigger.Wait();
  }
  SyncDurableFileBlocks(Engine, BlockVec);
  /* wait for file entry to flush */ {
    file_inserted = true;
    Engine->InsertFile(DurableByIdFileId, TFileObj::TKind::DurableFile, gen_id, BlockVec.Front(), 0UL, BlockVec.Size() * Disk::Util::LogicalBlockSize, NumDurable, 0UL, 0UL, completion_trigger);
    completion_trigger.Wait();
  }
}
