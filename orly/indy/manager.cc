/* <orly/indy/manager.cc>

   Implements <orly/indy/manager.h>.

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

#include <orly/indy/manager.h>
#include <optional>

#include <base/debug_log.h>
#include <base/opt_ostream.h>
#include <base/shutting_down.h>
#include <orly/indy/file_sync.h>
#include <orly/server/meta_record.h>
#include <base/util/time.h>

#include <sstream>

using namespace std;
using namespace std::literals;
using namespace Base;
using namespace Io;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Util;
using namespace ::Util;

const int TManager::SavedRepoMagicNumber = 8754321;

const TUuid TManager::MinId("00000000-0000-0000-0000-000000000000");
const TUuid TManager::MaxId("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF");
const TUuid TManager::SystemRepoId("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA");

RECORD_ELEM(TManager::TSavedRepoObj, bool                               , IsSafe);
RECORD_ELEM(TManager::TSavedRepoObj, TManager::TSavedRepoObj::TRootPath , RootPath);
RECORD_ELEM(TManager::TSavedRepoObj, TManager::TSavedRepoObj::TOptSeq   , LowestSequenceNumber);
RECORD_ELEM(TManager::TSavedRepoObj, TManager::TSavedRepoObj::TOptSeq   , HighestSequenceNumber);
RECORD_ELEM(TManager::TSavedRepoObj, TSequenceNumber                    , NextUpdate);
RECORD_ELEM(TManager::TSavedRepoObj, TSequenceNumber                    , ReleasedUpTo);
RECORD_ELEM(TManager::TSavedRepoObj, int                                , State);

TManager::TManager(Disk::Util::TEngine *engine,
                   size_t replication_sync_slave_buf_size_mb,
                   chrono::milliseconds merge_mem_delay,
                   chrono::milliseconds merge_disk_delay,
                   chrono::milliseconds layer_cleaning_interval,
                   chrono::milliseconds replication_delay,
                   TState state,
                   bool allow_tailing,
                   bool allow_file_sync,
                   bool no_realtime,
                   TFd &&socket,
                   const std::function<void (const shared_ptr<function<void (const TFd &)>> &)> &wait_for_slave,
                   const std::function<void (TState)> &state_change_cb,
                   const std::function<void (const Base::TUuid &, const Base::TUuid &, const Base::TUuid &)> &update_replication_notification_cb,
                   const TIndexCb &on_replicate_index_id,
                   const TForEachIndexIdCb &for_each_index_cb,
                   const std::function<void (const std::function<bool (Fiber::TRunner *)> &)> &for_each_scheduler_cb,
                   Base::TScheduler *scheduler,
                   Fiber::TRunner *bg_fast_runner,
                   size_t block_slots_available_per_merger,
                   size_t max_repo_cache_size,
                   size_t temp_file_consol_thresh,
                   const std::vector<size_t> &merge_mem_cores,
                   const std::vector<size_t> &merge_disk_cores,
                   bool create_new)
    : L1::TManager(engine,
                   merge_mem_delay,
                   merge_disk_delay,
                   allow_tailing,
                   no_realtime,
                   layer_cleaning_interval,
                   scheduler,
                   block_slots_available_per_merger,
                   max_repo_cache_size,
                   temp_file_consol_thresh,
                   merge_mem_cores,
                   merge_disk_cores,
                   create_new),
      State(state),
      WaitForSlave(wait_for_slave),
      StateChangeCb(state_change_cb),
      ReplicationRead(false),
      ReplicationWork(false),
      ReplicationServicesStopping(false),
      ReplicationServicesStarted(0),
      ReplicationNextTime(chrono::steady_clock::now()),
      ReplicationDelay(replication_delay),
      UpdateReplicationNotificationCb(update_replication_notification_cb),
      OnReplicateIndexIdCb(on_replicate_index_id),
      ForEachIndexIdCb(for_each_index_cb),
      ForEachSchedulerCb(for_each_scheduler_cb),
      BGFastRunner(bg_fast_runner),
      ReplicationSyncSlaveBufSizeBytes(replication_sync_slave_buf_size_mb * 1024UL * 1024UL),
      AllowFileSync(allow_file_sync) {
  ReplicationQueueEpollFd = epoll_create1(0);
  ReplicationWorkEpollFd = epoll_create1(0);
  ReplicationEpollFd = epoll_create1(0);
  Zero(ReplicationQueueEvent);
  Zero(ReplicationWorkEvent);
  Zero(ReplicationEvent);
  ReplicationQueueEvent.events = EPOLLIN;
  ReplicationWorkEvent.events = EPOLLIN;
  ReplicationEvent.events = EPOLLIN;
  ReplicationQueueEvent.data.fd = ReplicationQueueSem.GetFd();
  ReplicationWorkEvent.data.fd = ReplicationWorkSem.GetFd();
  ReplicationEvent.data.fd = ReplicationSem.GetFd();
  IfLt0(epoll_ctl(ReplicationQueueEpollFd, EPOLL_CTL_ADD, ReplicationQueueSem.GetFd(), &ReplicationQueueEvent));
  IfLt0(epoll_ctl(ReplicationWorkEpollFd, EPOLL_CTL_ADD, ReplicationWorkSem.GetFd(), &ReplicationWorkEvent));
  IfLt0(epoll_ctl(ReplicationEpollFd, EPOLL_CTL_ADD, ReplicationSem.GetFd(), &ReplicationEvent));

  auto system_ttl = TTtl::max();
  if (create_new) {
    SystemRepo = NewSafeRepo(SystemRepoId, system_ttl);
  } else {
    syslog(LOG_INFO, "Reconstructing System Repo from disk");
    SystemRepo = GetRepo(SystemRepoId, system_ttl, std::optional<L0::TManager::TPtr<TRepo>>(), true, false);
    syslog(LOG_INFO, "DONE Reconstructing System Repo from disk");
  }
  switch (State) {
    case Solo : {
      /* acquire Context lock */ {
        lock_guard<mutex> lock(ContextLock);
        auto cb = make_shared<function<void (const TFd &)>>(std::bind(&TManager::OnSlaveJoin, this, placeholders::_1));
        WaitForSlave(cb);
      }  // release context lock
      StateChangeCb(Solo);
      break;
    }
    case Master : {
      throw std::runtime_error("Cannot start Manager as Master. Must start as Solo.");
      break;
    }
    case SyncSlave : {
      /* acquire Context lock */ {
        lock_guard<mutex> lock(ContextLock);
        Context = make_shared<TSlave>(this, socket);
        ReplicationRead = true;
        ReplicationWork = true;
        ReplicationQueueSem.Push();
        ReplicationWorkSem.Push();
      }  // release context lock
      StateChangeCb(SyncSlave);
      break;
    }
    case Slave : {
      throw std::runtime_error("Cannot start Manager as Slave. Must start as SyncSlave.");
    }
  }
}

TManager::~TManager() {
  /* Paused-and-written repos (test povs) hold MakeDirty() self-pins that
     nothing will ever release; drop them so the sweeps below see only
     genuinely referenced repos (#440). */
  ReleaseDirtySelfPins();
  CloseAllUnreferencedObjects();
  SystemRepo.Reset();
  PreDtor();
}

L0::TManager::TPtr<TRepo> TManager::NewSafeRepo(const TUuid &repo_id,
                                        const std::optional<TTtl> &ttl,
                                        const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo) {
  return New(repo_id, *ttl, parent_repo, true);
}

L0::TManager::TPtr<TRepo> TManager::NewFastRepo(const TUuid &repo_id,
                                        const std::optional<TTtl> &ttl,
                                        const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo) {
  return New(repo_id, *ttl, parent_repo, false);
}

void TManager::RunReplicationQueue() {
  ++ReplicationServicesStarted;
  Base::TPushOnExit exit_latch(ReplicationServicesExited);
  try {
    epoll_event event;
    int timeout = -1;
    for (;;) {
      lock_guard<mutex> epoll_lock(ReplicationQueueEpollLock);
      for (;;) {
        int ret = epoll_wait(ReplicationQueueEpollFd, &event, 1, timeout);
        if (ret < 0 && errno == EINTR) {
          if (Base::IsShuttingDown()) {
            throw std::runtime_error("RunReplicationQueue() Service Shutdown");
          } else {
            continue;
          }
        } else {
          IfLt0(ret);
          break;
        }
      }
      if (ReplicationServicesStopping) {
        syslog(LOG_INFO, "TManager::RunReplicationQueue shutting down (#440)");
        return;
      }
      ReplicationQueueSem.Pop();
      for (;;) {
        if (ReplicationRead) {
          assert(Context);
          if (!(ReplicationRead = Context->Queue())) {
            syslog(LOG_INFO, "TManager: joining context");
            Context->Join();
            if (ReplicationServicesStopping) {
              /* The connection died because StopReplicationServices()
                 hard-closed it (#461).  Don't run the failover transition --
                 Demote()/PromoteSlave() would re-arm WaitForSlave() mid-
                 teardown -- just leave. */
              syslog(LOG_INFO, "TManager::RunReplicationQueue shutting down (#461)");
              return;
            }
            std::lock_guard<std::mutex> lock(ContextLock);
            switch (State) {
              case Solo : {
                throw std::runtime_error("Solo should not be replicating.");
                break;
              }
              case Master : {
                Demote();
                break;
              }
              case SyncSlave : {
                throw std::runtime_error("Connection died while in SyncSlave state. Cannot promote to master; must fail!");
                break;
              }
              case Slave : {
                PromoteSlave();
                break;
              }
            }
            break;
          }
        }
      }
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "RunReplicationQueue caught exception [%s]", ex.what());
    throw;
  } catch (...) {
    DEBUG_LOG("RunReplicationQueue() Exiting");
    throw;
  }
  syslog(LOG_INFO, "RunReplicationQueue() Exiting");
}

void TManager::RunReplicationWork() {
  ++ReplicationServicesStarted;
  Base::TPushOnExit exit_latch(ReplicationServicesExited);
  try {
    epoll_event event;
    int timeout = -1;
    for (;;) {
      lock_guard<mutex> epoll_lock(ReplicationWorkEpollLock);
      for (;;) {
        int ret = epoll_wait(ReplicationWorkEpollFd, &event, 1, timeout);
        if (ret < 0 && errno == EINTR) {
          if (Base::IsShuttingDown()) {
            throw std::runtime_error("RunReplicationWork() Service Shutdown");
          } else {
            continue;
          }
        } else {
          IfLt0(ret);
          break;
        }
      }
      if (ReplicationServicesStopping) {
        syslog(LOG_INFO, "TManager::RunReplicationWork shutting down (#440)");
        return;
      }
      ReplicationWorkSem.Pop();
      for (;;) {
        if (ReplicationWork) {
          std::shared_ptr<TCommonContext> context;
          /* hold on to the context */ {
            std::lock_guard<std::mutex> lock(ContextLock);
            context = Context;
          }
          if (context) {
            ReplicationWork = context->Work();
          } else {
            break;
          }
        } else {
          break;
        }
      }
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "RunReplicationWork caught exception [%s]", ex.what());
    throw;
  } catch (...) {
    DEBUG_LOG("RunReplicationWork() Exiting");
    throw;
  }
  syslog(LOG_INFO, "RunReplicationWork() Exiting");
}

void TManager::RunReplicateTransaction() {
  ++ReplicationServicesStarted;
  Base::TPushOnExit exit_latch(ReplicationServicesExited);
  epoll_event event;
  int timeout = -1;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  for (;;) {
    try {
      lock_guard<mutex> epoll_lock(ReplicationEpollLock);
      for (;;) {
        int ret = epoll_wait(ReplicationEpollFd, &event, 1, timeout);
        if (ret < 0 && errno == EINTR) {
          if (Base::IsShuttingDown()) {
            throw std::runtime_error("RunReplicateTransaction() Service Shutdown");
          } else {
            continue;
          }
        } else {
          IfLt0(ret);
          break;
        }
      }
      if (ReplicationServicesStopping) {
        syslog(LOG_INFO, "TManager::RunReplicateTransaction shutting down (#440)");
        return;
      }
      SleepUntil(ReplicationNextTime);
      ReplicationNextTime = chrono::steady_clock::now() + ReplicationDelay;
      TReplicationStreamer replication_streamer;
      TState state_used;
      TReplicationQueue copy_queue;
      std::shared_ptr<TCommonContext> context;
      /* acquire Context lock */ {
        lock_guard<mutex> lock(ContextLock);
        state_used = State;
        switch (State) {
          case Solo : {
            /* acquire Replication lock */ {
              std::lock_guard<std::mutex> lock(ReplicationLock);
              ReplicationSem.Pop();
              copy_queue.Swap(ReplicationQueue);
              assert(ReplicationQueue.IsEmpty());
            }
            break;
          }
          case Master : {
            context = Context;
            /* acquire Replication lock */ {
              std::lock_guard<std::mutex> lock(ReplicationLock);
              ReplicationSem.Pop();
              copy_queue.Swap(ReplicationQueue);
              assert(ReplicationQueue.IsEmpty());
            }  // release Replication lock
            try {
              size_t num_trans_to_replicate = 0UL;
              for (TReplicationQueue::TItemCollection::TCursor csr(copy_queue.GetItemCollection()); csr; ++csr, ++num_trans_to_replicate) {
                switch (csr->GetKind()) {
                  case TReplicationQueue::TReplicationItem::Repo : {
                    replication_streamer.PushRepo(*reinterpret_cast<TRepoReplication *>(&*csr));
                    break;
                  }
                  case TReplicationQueue::TReplicationItem::Durable : {
                    replication_streamer.PushDurable(*reinterpret_cast<TDurableReplication *>(&*csr));
                    break;
                  }
                  case TReplicationQueue::TReplicationItem::Transaction : {
                    replication_streamer.PushTransaction(dynamic_cast<TTransactionReplication *>(&*csr)->GetReplica());
                    break;
                  }
                  case TReplicationQueue::TReplicationItem::IndexId : {
                    replication_streamer.PushIndexId(*reinterpret_cast<TIndexIdReplication *>(&*csr));
                    break;
                  }
                }
              }
              if (num_trans_to_replicate > 10000UL) {
                syslog(LOG_INFO, "Replicating [%ld] transactions", num_trans_to_replicate);
              }
            } catch (...) {
              throw;
            }
            break;
          }
          case SyncSlave : {
            /* acquire Replication lock */ {
              std::lock_guard<std::mutex> lock(ReplicationLock);
              ReplicationSem.Pop();
              ReplicationQueue.Clear();
            }
            break;
          }
          case Slave : {
            /* acquire Replication lock */ {
              std::lock_guard<std::mutex> lock(ReplicationLock);
              ReplicationSem.Pop();
              ReplicationQueue.Clear();
            }
            break;
          }
        }
      }  // release Context lock
      switch (state_used) {
        case Solo : {
          break;
        }
        case Master : {
          if (!replication_streamer.IsEmpty()) {
            try {
              Base::TTimer timer;
              //auto future = context->Write<void>(TSlave::PushNotificationsId, replication_streamer);
              auto future = context->Write<void>(TSlave::PushNotificationsId, replication_streamer);
              timer.Stop();
              if (timer.GetTotal() < 1s) {
                syslog(LOG_INFO, "Write TSlave::PushNotificationsId took [%fs]", ToSecondsDouble(timer.GetTotal()));
              }
              assert(future);
              future->Sync();  // wait for the future to complete
              if (!static_cast<bool>(*future)) {
                throw std::runtime_error("Future did not complete.");
              }
              /* now apply all the necessary replication notifications. */
              for (TReplicationQueue::TItemCollection::TCursor csr(copy_queue.GetItemCollection()); csr; ++csr) {
                switch (csr->GetKind()) {
                  case TReplicationQueue::TReplicationItem::Repo : {
                    break;
                  }
                  case TReplicationQueue::TReplicationItem::Durable : {
                    break;
                  }
                  case TReplicationQueue::TReplicationItem::IndexId : {
                    break;
                  }
                  case TReplicationQueue::TReplicationItem::Transaction : {
                    for (const auto &mutation : reinterpret_cast<TTransactionReplication *>(&*csr)->GetReplica().GetMutationList()) {
                      switch (mutation.GetKind()) {
                        case L1::TTransaction::TReplica::TMutation::Pusher: {
                          if (mutation.GetRepoId() == GlobalPovId) {
                            Base::TUuid session_id;
                            try {
                              Sabot::ToNative(*Sabot::State::TAny::TWrapper(mutation.GetUpdate().GetMetadata().NewState(mutation.GetUpdate().GetSuprena().get(), state_alloc)), session_id);
                            } catch (const exception &ex) {
                              syslog(LOG_ERR, "Exception while trying to access ession ID of update promoted to global: [%s]", ex.what());
                            }
                            Base::TUuid tracker_id;
                            Sabot::ToNative(*Sabot::State::TAny::TWrapper(mutation.GetUpdate().GetId().NewState(mutation.GetUpdate().GetSuprena().get(), state_alloc)), tracker_id);
                            //std::cout << "Calling UpdateReplicationNotificationCb() for global" << std::endl;
                            UpdateReplicationNotificationCb(session_id, mutation.GetRepoId(), tracker_id);
                          } else {
                            Server::TMetaRecord meta_record;
                            /* Not every replicated update carries a TMetaRecord: system-repo
                               commits (SaveIndexNamespaceMapping, SaveInstalledPackage) have void
                               metadata, and decoding that throws.  Skip the notification -- nobody
                               is waiting on a system commit -- instead of letting the exception
                               unwind past the rest of the batch's notifications (#499).  The
                               global-pov branch above guards its decode the same way. */
                            try {
                              Sabot::ToNative(*Sabot::State::TAny::TWrapper(mutation.GetUpdate().GetMetadata().NewState(mutation.GetUpdate().GetSuprena().get(), state_alloc)), meta_record);
                            } catch (const exception &/*ex*/) {
                              continue;
                            }
                            /* Notify every original update's own tracker id (the map key), not one
                               shared id for the whole mutation (#327) -- when updates are merged, a
                               single mutation's meta record can carry several originally-distinct
                               updates (see the identical per-entry pattern in
                               TRepoTetrisManager::TPlayer::TChild::Refresh), and each waiter is only
                               listening for its own update's id. */
                            for (const auto &item: meta_record.GetEntryByUpdateId()) {
                              const auto &entry = item.second;
                              //std::cout << "Calling UpdateReplicationNotificationCb() for private" << std::endl;
                              UpdateReplicationNotificationCb(entry.GetSessionId(), mutation.GetRepoId(), item.first);
                            }
                          }
                          break;
                        }
                        case L1::TTransaction::TReplica::TMutation::Popper: {
                          break;
                        }
                        case L1::TTransaction::TReplica::TMutation::Failer: {
                          break;
                        }
                        case L1::TTransaction::TReplica::TMutation::Pauser: {
                          break;
                        }
                        case L1::TTransaction::TReplica::TMutation::UnPauser: {
                          break;
                        }
                      }
                    }
                    break;
                  }
                }
              }
            } catch (const Rpc::TAnyFuture::TRemoteError &error) {
              syslog(LOG_ERR, "TManager: replication future failed: %s", error.what());
            }
          }
          break;
        }
        case SyncSlave : {
          break;
        }
        case Slave : {
          break;
        }
      }
    } catch (const std::system_error &err) {
      if (WasInterrupted(err)) {
        throw;
      } else {
        syslog(LOG_ERR, "RunReplicateTransaction system error: [%s]", err.what());
      }
    } catch (const std::exception &ex) {
      syslog(LOG_ERR, "RunReplicateTransaction error: [%s]", ex.what());
      if (Base::IsShuttingDown()) {
        throw;
      }
    } catch (...) {
      DEBUG_LOG("RunReplicateTransaction() Exiting");
      throw;
    }
  }
  DEBUG_LOG("RunReplicateTransaction() Exiting");
}

void TManager::StopReplicationServices() {
  ReplicationServicesStopping = true;
  /* Each push makes the loop's epoll_wait return; the loop then sees the
     flag (before consuming the semaphore) and returns. */
  ReplicationQueueSem.Push();
  ReplicationWorkSem.Push();
  ReplicationSem.Push();
  /* A CONNECTED pair's loops are parked past those epolls: the queue loop
     blocks in Read() on the socket, and a master-mode replicate loop can be
     parked in future->Sync() on a slave RPC -- neither is sem-wakeable, and
     an unresponsive peer would hold JoinReplicationServices() forever
     (#461).  Hard-close the socket: the read errors out, which fails every
     outstanding future (waking Sync()) and collapses the reader loop. */
  std::shared_ptr<TCommonContext> context;
  /* acquire Context lock */ {
    std::lock_guard<std::mutex> lock(ContextLock);
    context = Context;
  }  // release Context lock
  if (context) {
    context->Shutdown();
  }
}

void TManager::JoinReplicationServices() {
  /* Reap exactly as many exits as loops that actually entered; a loop
     whose fiber never got to run can't be waited for (and never touches
     us).  Every entered loop pushes Exited on its way out, including the
     exception paths. */
  for (size_t n = ReplicationServicesStarted; n > 0; --n) {
    ReplicationServicesExited.Pop();
  }
}

TManager::TMaster::TMaster(TManager *manager, const TFd &fd)
    : TMasterContext(fd), Manager(manager) {}

TManager::TMaster::~TMaster() {}

bool TManager::TMaster::Queue() {
  return TCommonContext::Queue();
}

bool TManager::TMaster::Work() {
  return TCommonContext::Work();
}

void TManager::TMaster::NotifyFinishSyncInventory() {
  std::lock_guard<std::mutex> lock(Manager->SlaveNotifyLock);
  Manager->SlaveNotifiedFinish = true;
  Manager->SlaveNotifyCond.notify_all();
}

TContextInputStreamer TManager::TMaster::FetchUpdates(const TUuid &repo_id, TSequenceNumber lowest, TSequenceNumber highest) {
  /* log scope */ {
    std::ostringstream ss;
    ss << repo_id;
    syslog(LOG_INFO, "TMaster::FetchUpdates(%s) [%lu -> %lu]", ss.str().c_str(), lowest, highest);
  }
  Base::TTimer timer;
  auto repo = Manager->ForceGetRepo(repo_id);
  assert(Manager->SlaveSyncViewMap.find(repo_id) != Manager->SlaveSyncViewMap.end());
  const auto &view = Manager->SlaveSyncViewMap.find(repo_id)->second;
  auto walker_ptr = repo->NewUpdateWalker(view, lowest, highest);
  TContextInputStreamer context;
  for (TUpdateWalker &walker = *walker_ptr; walker; ++walker) {
    context.AppendUpdate(repo_id, *walker);
  }
  timer.Stop();
  /* log scope */ {
    std::ostringstream ss;
    ss << repo_id;
    syslog(LOG_INFO, "TMaster::FetchUpdates(%s) [%lu -> %lu] took [%fs]", ss.str().c_str(), lowest, highest, ToSecondsDouble(timer.GetTotal()));
  }
  return context;
}

TManager::TMaster::TViewDef TManager::TMaster::GetView(const Base::TUuid &repo_id) {
  assert(Manager->SlaveSyncViewMap.find(repo_id) != Manager->SlaveSyncViewMap.end());
  TViewDef ret;
  const auto &view = Manager->SlaveSyncViewMap.find(repo_id)->second;
  Manager->AugmentViewMapWithDiskLayers(ret, view);
  return ret;
}

void TManager::AugmentViewMapWithDiskLayers(TMaster::TViewDef &view_def, const std::unique_ptr<Indy::TRepo::TView> &view) const {
  assert(view);
  const TRepo::TMapping *mapping = view->GetMapping();
  for (TRepo::TMapping::TEntryCollection::TCursor entry_csr(mapping->GetEntryCollection()); entry_csr; ++entry_csr) {
    const TRepo::TDataLayer *layer = entry_csr->GetLayer();
    if (layer->GetKind() == TRepo::TDataLayer::TKind::Disk) {
      const TDiskLayer *disk_layer = reinterpret_cast<const TDiskLayer *>(layer);
      assert(disk_layer->GetHighestSeq() >= disk_layer->GetLowestSeq());
      view_def.emplace_back(disk_layer->GetLowestSeq(), disk_layer->GetHighestSeq(), disk_layer->GetGenId(), disk_layer->GetNumKeys());
    }
  }
  std::sort(view_def.begin(), view_def.end(), [](const TMaster::TFileTuple &lhs, const TMaster::TFileTuple &rhs) {
    return std::get<0>(lhs) < std::get<0>(rhs);
  });
}

TFileSync TManager::TMaster::SyncFile(const Base::TUuid &file_id, size_t gen_id, size_t context) {
  return TFileSync(Manager->GetEngine(), file_id, gen_id, context);
}

TManager::TSlave::TSlave(TManager *manager, const TFd &fd)
    : TSlaveContext(fd), Manager(manager), SlushCoreVec(new TCoreVectorBuilder()), Flusher(make_shared<TFlusher>(Manager->GetEngine())) {}

TManager::TSlave::~TSlave() {}

bool TManager::TSlave::Queue() {
  return TCommonContext::Queue();
}

bool TManager::TSlave::Work() {
  return TCommonContext::Work();
}

void TManager::TSlave::ScheduleSyncInventory() {
  syslog(LOG_INFO, "TSlave: scheduling SyncInventory()");
  Fiber::TFrame *frame = Fiber::TFrame::LocalFramePool->Alloc();
  try {
    frame->Latch(Manager->BGFastRunner, this, static_cast<Fiber::TRunnable::TFunc>(&Orly::Indy::TManager::TSlave::SyncInventory));
  } catch (...) {
    Fiber::TFrame::LocalFramePool->Free(frame);
    throw;
  }
  //Manager->Scheduler->Schedule(std::bind(&TManager::TSlave::SyncInventory, this));
}

void TManager::TSlave::SyncInventory() {
  syslog(LOG_INFO, "TSlave: calling SyncInventory()");
  for (const auto &to_sync : ToSyncQueue) {
    const TUuid &repo_id = to_sync.RepoId;
    size_t ttl = to_sync.Ttl;
    const std::optional<TUuid> &parent_repo_id = to_sync.ParentRepoId;
    bool is_safe = to_sync.IsSafe;
    const std::optional<TSequenceNumber> &lowest = to_sync.Lowest;
    const std::optional<TSequenceNumber> &highest = to_sync.Highest;
    TSequenceNumber next_id = to_sync.NextId;
    /* log scope */ {
      std::ostringstream ss;
      ss << repo_id;
      syslog(LOG_INFO, "TSlave::Inventory(%s)", ss.str().c_str());
    }
    std::optional<L0::TManager::TPtr<TRepo>> parent_repo;
    if (parent_repo_id) {
      /* log scope */ {
        std::ostringstream ss;
        ss << *parent_repo_id;
        syslog(LOG_INFO, "TSlave::Inventory::ForgeGetRepo(%s)", ss.str().c_str());
      }
      parent_repo = Manager->ForceGetRepo(*parent_repo_id);
    }
    /* log scope */ {
      std::ostringstream ss;
      ss << repo_id;
      syslog(LOG_INFO, "TSlave::Inventory::GetRepo(%s)", ss.str().c_str());
    }
    auto repo = Manager->GetRepo(repo_id, chrono::seconds(ttl), parent_repo, is_safe, false);
    /* log scope */ {
      std::ostringstream ss;
      ss << repo_id;
      syslog(LOG_INFO, "done TSlave::Inventory::GetRepo(%s)", ss.str().c_str());
    }
    //repo->SetNextSequenceNumber(next_id);
    repo->SetReleasedUpTo(next_id > 0UL ? next_id - 1UL : 0UL);
    /* log scope */ {
      std::ostringstream ss;
      ss << repo_id;
      syslog(LOG_INFO, "TSlave::Inventory (%s) SetReleasedUpTo(%lu)", ss.str().c_str(), next_id > 0UL ? next_id - 1UL : 0UL);
    }


    std::optional<TSequenceNumber> snap_lowest;
    std::optional<TSequenceNumber> snap_highest;
    TSequenceNumber snap_next_id;
    repo->GetSnapshot(snap_lowest, snap_highest, snap_next_id);
    //std::cout << "TSlave::Inventory snapshot for (" << repo_id << ")\t[" << snap_lowest << " -> " << snap_highest << "] next=[" << snap_next_id << "]" << std::endl;



    if (lowest && highest) {
      //std::cout << "SetNextSequenceNumber [" << *lowest << "]" << std::endl;
      repo->SetNextSequenceNumber(*lowest);
      TSequenceNumber my_lowest = 0UL;
      const std::optional<TSequenceNumber> &high = repo->GetSequenceNumberLimit();
      if (high) {
        my_lowest = std::max(*lowest, *high);
      }
      assert(snap_next_id == my_lowest + 1);

      auto view_future = Write<TMaster::TViewDef>(TMaster::GetViewId, repo_id);
      assert(view_future);
      TMaster::TViewDef view_def = **view_future;

      if (view_def.size() && Manager->AllowFileSync) {
        /* low, high, gen_id, num_keys */
        for (const auto &view_file : view_def) {
          syslog(LOG_INFO, "Can Sync [%ld -> %ld] from [%ld]", std::get<0>(view_file), std::get<1>(view_file), std::get<2>(view_file));
        }

        /* do we need to fill in the front? if current low > first low from master disk view */
        if (my_lowest > std::get<0>(view_def[0])) {
          PullUpdateRange(repo_id, repo, my_lowest + 1, std::get<0>(view_def[0]) - 1);
        }
        /* fill in the middle disk files */
        TSequenceNumber highest_filled = 0UL;
        for (const auto &view_file : view_def) {
          if (std::get<0>(view_file) >= my_lowest && std::get<1>(view_file) <= *highest) {
            highest_filled = std::max(highest_filled, std::get<1>(view_file));
            syslog(LOG_INFO, "sync file [%ld] for [%ld -> %ld] with service [%p]", std::get<2>(view_file), std::get<0>(view_file), std::get<1>(view_file), Manager->GetEngine());
            auto file_future = Write<TFileSync>(TMaster::SyncFileId, repo_id, std::get<2>(view_file), reinterpret_cast<size_t>(Manager->GetEngine()));
            assert(file_future);
            TFileSync file = **file_future;
            repo->AddSyncedFileToRepo(file.GetStartingBlockId(), file.GetStartingBlockOffset(), file.GetFileLength(), std::get<0>(view_file), std::get<1>(view_file), std::get<3>(view_file));
            repo->UseSequenceNumbers((std::get<1>(view_file) - std::get<0>(view_file)) + 1);
            //std::cout << "UseSequenceNumbers[" << ((std::get<1>(view_file) - std::get<0>(view_file)) + 1) << "]" << std::endl;
          }
        }
        /* do we need to fill the end? if highest > highest_filled */
        if (*highest > highest_filled) {
          PullUpdateRange(repo_id, repo, highest_filled + 1, *highest);
        }
      } else {
        PullUpdateRange(repo_id, repo, *lowest, *highest);
      }
    }
    //std::cout << "SetNextSequenceNumber [" << next_id << "]" << std::endl;
    repo->SetNextSequenceNumber(next_id);
    repo->GetSnapshot(snap_lowest, snap_highest, snap_next_id);
    //std::cout << "TSlave::Inventory snapshot for (" << repo_id << ")\t[" << snap_lowest << " -> " << snap_highest << "] next=[" << snap_next_id << "]" << std::endl;
    assert(!snap_highest || (*snap_highest == snap_next_id - 1));
  }
  auto sync_future = Write<void>(TMaster::NotifyFinishSyncInventoryId);
  assert(sync_future);
  sync_future->Sync();  // wait for the future to complete
  if (!static_cast<bool>(*sync_future)) {
    throw std::runtime_error("Future did not complete.");
  }
  Indy::Fiber::FreeMyFrame(Fiber::TFrame::LocalFramePool);
}

void TManager::TSlave::PullUpdateRange(const Base::TUuid &repo_id, TManager::TPtr<Indy::TRepo> &repo, TSequenceNumber from, TSequenceNumber to) {
  syslog(LOG_INFO, "TSlave: PullUpdateRange from [%lu -> %lu]", from, to);
  Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed = Disk::Util::TVolume::TDesc::TStorageSpeed::Fast;
  const size_t max_update_pull = 50000;
  void *lhs_state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto entry_sort_func = [](const TUpdate::TEntry *lhs, const TUpdate::TEntry *rhs) {
    return lhs->GetEntryKey() <= rhs->GetEntryKey();
  };
  try {
    for (size_t i = from; i <= to; i += (max_update_pull + 1UL)) {
      TSequenceNumber cur_from = i;
      TSequenceNumber cur_to = std::min(i + max_update_pull, to);
      auto future = Write<TContextInputStreamer>(TMaster::FetchUpdatesId, repo_id, cur_from, cur_to);
      assert(future);
      Util::TContextInputStreamer context = **future;
      Base::TTimer timer;

      TMemoryLayer *mem_layer = new TMemoryLayer(Manager);
      size_t num_entry_inserted = 0UL;

      Atom::TSuprena arena;
      for (auto iter : context.UpdateVec) {
        /* Split entries by mutator: Assign-tagged go through the TUpdate
           TOpByKey ctor (existing semantics); non-Assign entries get
           registered via update->AddEntry(...,mutator) so the deferred
           commutative info survives the slave-replay path. Mirrors the
           fix in TRepo::GetLowestUpdate (PR #52) for the Tetris peek
           path. Without this, the slave reconstructs deferred {Add, n}
           entries as Assign(n) and silently loses updates on
           concurrent writes that landed on the master (#54). */
        TUpdate::TOpByKey op_by_key;
        for (auto entry : iter.EntryVec) {
          if (entry.Mutator == TMutator::Assign) {
            op_by_key[entry.IndexKey] = TKey(entry.Op, context.Suprena.get());
          }
          ++num_entry_inserted;
        }
        TUpdate *update = new TUpdate(op_by_key, TKey(iter.Metadata, context.Suprena.get()), TKey(iter.Id, context.Suprena.get()), lhs_state_alloc);
        update->SetSequenceNumber(iter.SequenceNumber);
        for (auto entry : iter.EntryVec) {
          if (entry.Mutator != TMutator::Assign) {
            update->AddEntry(entry.IndexKey, TKey(entry.Op, context.Suprena.get()), entry.Mutator);
          }
        }
        mem_layer->ImporterAppendUpdate(update);
      }
      /* sort and fix the mem_layer */ {
        std::vector<TUpdate::TEntry *> entry_vec;
        entry_vec.reserve(num_entry_inserted);
        for (TMemoryLayer::TUpdateCollection::TCursor update_csr(mem_layer->GetUpdateCollection()); update_csr; ++update_csr) {
          for (TUpdate::TEntryCollection::TCursor entry_csr(update_csr->GetEntryCollection()); entry_csr; ++entry_csr) {
            entry_vec.push_back(&*entry_csr);
          }
        }
        std::sort(entry_vec.begin(), entry_vec.end(), entry_sort_func);
        for (auto entry : entry_vec) {
          mem_layer->ImporterAppendEntry(entry);
        }
      }
      std::optional<TSequenceNumber> snap_lowest;
      std::optional<TSequenceNumber> snap_highest;
      TSequenceNumber snap_next_id;
      repo->GetSnapshot(snap_lowest, snap_highest, snap_next_id);
      //std::cout << "TSlave::Inventory snapshot for (" << repo_id << ")\t[" << snap_lowest << " -> " << snap_highest << "] next=[" << snap_next_id << "]" << std::endl;
      syslog(LOG_INFO, "AddImportLayer of size [%ld]", mem_layer->GetSize());
      /* by the time we fetch the updates, if they were in a fast repo..., they may be gone in the master.
         That means we'll have caught the push to the parent POV and will be applying that when we replay live transactions. */
      if (mem_layer->GetSize() > 0) {
        Base::TEventSemaphore sem;
        sem.Push();
        repo->AddImportLayer(mem_layer, sem, storage_speed);
      }
      timer.Stop();
      /* log scope */ {
        std::ostringstream ss;
        ss << repo_id;
        syslog(LOG_INFO, "TSlave: received %zu updates from repo %s, commit took [%fs]", context.UpdateVec.size(), ss.str().c_str(), ToSecondsDouble(timer.GetTotal()));
      }
    }
  } catch (...) {
    std::ostringstream ss;
    ss << repo_id;
    syslog(LOG_ERR, "Error caught while trying to fetch updates for repo [%s]", ss.str().c_str());
    throw;
  }
  repo->UseSequenceNumbers((to - from) + 1);
  //std::cout << "UseSequenceNumbers[" << ((to - from) + 1) << "]" << std::endl;
}

void TManager::TSlave::TFlusher::ConsumeOutput(const std::shared_ptr<const Io::TChunk> &chunk) {
  assert(chunk);
  const size_t num_bytes = chunk->GetSize();
  const char *start, *limit;
  chunk->GetData(start, limit);
  OutStream->Write(start, num_bytes);
}

std::shared_ptr<const Io::TChunk> TManager::TSlave::TFlusher::TryProduceInput() {
  if (!InStream) {
    InStream = std::make_unique<TDataInStream>(HERE, Disk::Source::SlaveSlush, Disk::RealTime, this, Engine->GetPageCache(), 0UL);
  }
  auto chunk = Pool->AcquireChunk();
  const size_t amt_to_read = chunk->GetRemainingSize();
  InStream->Read(chunk->GetBuffer(), amt_to_read);
  chunk->Commit(amt_to_read);
  return chunk;
}

void TManager::TSlave::TFlusher::Flush() {
  assert(OutStream);
  OutStream.reset(); /* flush the outbound stream */
  Trigger.Wait(); /* wait for everything to be flushed */
}

size_t TManager::TSlave::TFlusher::NewBlockCb(Disk::Util::TVolumeManager */*vol_man*/) {
  return Engine->ReserveBlock(Disk::Util::TVolume::TDesc::TStorageSpeed::Fast);
}

void TManager::TSlave::Inventory(const TUuid &repo_id,
                                 size_t ttl,
                                 const std::optional<TUuid> &parent_repo_id,
                                 bool is_safe,
                                 const std::optional<TSequenceNumber> &lowest,
                                 const std::optional<TSequenceNumber> &highest,
                                 TSequenceNumber next_id) {
  ToSyncQueue.emplace_back(repo_id, ttl, parent_repo_id, is_safe, lowest, highest, next_id);
}

void TManager::TSlave::Index(const TIndexMapReplica &index_map_replica) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  std::vector<TCore>::const_iterator index_iter = index_map_replica.GetCoreVec().GetCores().begin();
  std::vector<TCore>::const_iterator index_end = index_map_replica.GetCoreVec().GetCores().end();
  TCore::TArena *index_arena = index_map_replica.GetCoreVec().GetArena();
  Base::TUuid index_id;
  for (; index_iter != index_end; ++index_iter) {
    Atom::TSuprena suprena;
    syslog(LOG_INFO, "Replicate IndexId");
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(index_iter->NewState(index_arena, state_alloc)), index_id);
    ++index_iter;
    assert(index_iter != index_end);
    string pkg_key;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(index_iter->NewState(index_arena, state_alloc)), pkg_key);
    ++index_iter;
    assert(index_iter != index_end);
    Indy::TKey val(&suprena, Sabot::State::TAny::TWrapper(index_iter->NewState(index_arena, state_alloc)));
    Manager->OnReplicateIndexIdCb(index_id, pkg_key, val);
  }
}

void TManager::TSlave::PushNotifications(const TReplicationStreamer &replication_streamer) {
  assert(Indy::Fiber::TRunner::LocalRunner);
  Indy::Fiber::TSwitchToRunner RunnerSwitcher(Manager->BGFastRunner);
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  /* acquire Context lock */ {
    std::lock_guard<std::mutex> lock(Manager->ContextLock);
    switch (Manager->State) {
      case Solo : {
        break;
      }
      case Master : {
        break;
      }
      case SyncSlave : {
        try {
          /* Store the repo saves next */ {
            std::vector<TCore>::const_iterator repo_iter = replication_streamer.GetRepoVec().GetCores().begin();
            std::vector<TCore>::const_iterator repo_end = replication_streamer.GetRepoVec().GetCores().end();
            TCore::TArena *repo_arena = replication_streamer.GetRepoVec().GetArena();
            Base::TUuid repo_id;
            Sabot::TStdDuration repo_nsec_ttl;
            TTtl repo_ttl;
            bool is_safe;
            std::optional<Base::TUuid> opt_parent_repo_id;
            for (; repo_iter != repo_end; ++repo_iter) {
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), repo_id);
              ++repo_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), repo_nsec_ttl);
              repo_ttl = chrono::duration_cast<TTtl>(repo_nsec_ttl);
              ++repo_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), is_safe);
              ++repo_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), opt_parent_repo_id);
              L0::TManager::TPtr<L0::TManager::TRepo> opt_parent_repo = static_cast<bool>(opt_parent_repo_id) ? Manager->ForceGetRepo(*opt_parent_repo_id) : L0::TManager::TPtr<Indy::TRepo>();
              Manager->GetRepo(repo_id, repo_ttl, opt_parent_repo, is_safe, true);
            }
          }
          /* Store the durable saves next */ {
            auto now = chrono::system_clock::now();
            std::vector<TCore>::const_iterator durable_iter = replication_streamer.GetDurableVec().GetCores().begin();
            std::vector<TCore>::const_iterator durable_end = replication_streamer.GetDurableVec().GetCores().end();
            TCore::TArena *durable_arena = replication_streamer.GetDurableVec().GetArena();
            Base::TUuid durable_id;
            TTtl durable_ttl;
            std::string serialized_obj;
            assert(Manager->DurableManager);
            for (; durable_iter != durable_end; ++durable_iter) {
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(durable_iter->NewState(durable_arena, state_alloc)), durable_id);
              ++durable_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(durable_iter->NewState(durable_arena, state_alloc)), durable_ttl);
              ++durable_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(durable_iter->NewState(durable_arena, state_alloc)), serialized_obj);
              /* Fire-and-forget (null sem): the replication stream must not block per object on
                 the durable flush cadence.  The old stack sem here was never popped, which only
                 worked while Save() signalled synchronously before returning; with the sem now
                 pushed after the disk write (#277) it would be a use-after-free. */
              Manager->DurableManager->Save(durable_id, TDeadline(now + durable_ttl), durable_ttl, serialized_obj, nullptr);
            }
          }
          /* Store the transaction changes next */ {
            TCore::TArena *arena = replication_streamer.GetTransactionVec().GetArena();
            for (const auto &core : replication_streamer.GetTransactionVec().GetCores()) {
              SlushCoreVec->PushState(core.NewState(arena, state_alloc));
            }
            const size_t cur_slush_size_in_bytes = SlushCoreVec->GetCores().size() * sizeof(TCore) + SlushCoreVec->GetNumArenaBytes();
            if (cur_slush_size_in_bytes > Manager->ReplicationSyncSlaveBufSizeBytes) { /* flush to disk if we're too big */
              syslog(LOG_INFO, "Flushing slush cores to disk [%ld], MB=[%f]", SlushCoreVec->GetCores().size(), cur_slush_size_in_bytes / (1024.0 * 1024.0));
              TBinaryOutputOnlyStream strm(Flusher);
              Flusher->SetStoredOffset();
              SlushCoreVec->Write(strm);
              SlushCoreVec = std::make_unique<TCoreVectorBuilder>();
            }
          }
        } catch (const exception &ex) {
          syslog(LOG_ERR, "Exception in TManager::TSlave::PushNotifications()::SyncSlave [%s]", ex.what());
          throw;
        }
        break;
      }
      case Slave : {
        try {
          /* Store the index ids next */ {
            std::vector<TCore>::const_iterator index_iter = replication_streamer.GetIndexIdVec().GetCores().begin();
            std::vector<TCore>::const_iterator index_end = replication_streamer.GetIndexIdVec().GetCores().end();
            TCore::TArena *index_arena = replication_streamer.GetIndexIdVec().GetArena();
            Base::TUuid index_id;
            for (; index_iter != index_end; ++index_iter) {
              Atom::TSuprena suprena;
              syslog(LOG_INFO, "Replicate IndexId");
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(index_iter->NewState(index_arena, state_alloc)), index_id);
              ++index_iter;
              assert(index_iter != index_end);
              std::string pkg_key;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(index_iter->NewState(index_arena, state_alloc)), pkg_key);
              ++index_iter;
              assert(index_iter != index_end);
              Indy::TKey val(&suprena, Sabot::State::TAny::TWrapper(index_iter->NewState(index_arena, state_alloc)));
              Manager->OnReplicateIndexIdCb(index_id, pkg_key, val);
            }
          }
          /* Store the repo saves next */ {
            std::vector<TCore>::const_iterator repo_iter = replication_streamer.GetRepoVec().GetCores().begin();
            std::vector<TCore>::const_iterator repo_end = replication_streamer.GetRepoVec().GetCores().end();
            TCore::TArena *repo_arena = replication_streamer.GetRepoVec().GetArena();
            Base::TUuid repo_id;
            Sabot::TStdDuration repo_nsec_ttl;
            TTtl repo_ttl;
            bool is_safe;
            std::optional<Base::TUuid> opt_parent_repo_id;
            for (; repo_iter != repo_end; ++repo_iter) {
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), repo_id);
              ++repo_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), repo_nsec_ttl);
              repo_ttl = chrono::duration_cast<TTtl>(repo_nsec_ttl);
              ++repo_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), is_safe);
              ++repo_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(repo_iter->NewState(repo_arena, state_alloc)), opt_parent_repo_id);
              L0::TManager::TPtr<L0::TManager::TRepo> opt_parent_repo = static_cast<bool>(opt_parent_repo_id) ? Manager->ForceGetRepo(*opt_parent_repo_id) : L0::TManager::TPtr<Indy::TRepo>();
              Manager->GetRepo(repo_id, repo_ttl, opt_parent_repo, is_safe, true);
            }
          }
          /* Store the durable saves next */ {
            auto now = chrono::system_clock::now();
            std::vector<TCore>::const_iterator durable_iter = replication_streamer.GetDurableVec().GetCores().begin();
            std::vector<TCore>::const_iterator durable_end = replication_streamer.GetDurableVec().GetCores().end();
            TCore::TArena *durable_arena = replication_streamer.GetDurableVec().GetArena();
            Base::TUuid durable_id;
            TTtl durable_ttl;
            std::string serialized_obj;
            assert(Manager->DurableManager);
            for (; durable_iter != durable_end; ++durable_iter) {
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(durable_iter->NewState(durable_arena, state_alloc)), durable_id);
              ++durable_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(durable_iter->NewState(durable_arena, state_alloc)), durable_ttl);
              ++durable_iter;
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(durable_iter->NewState(durable_arena, state_alloc)), serialized_obj);
              /* Fire-and-forget (null sem): the replication stream must not block per object on
                 the durable flush cadence.  The old stack sem here was never popped, which only
                 worked while Save() signalled synchronously before returning; with the sem now
                 pushed after the disk write (#277) it would be a use-after-free. */
              Manager->DurableManager->Save(durable_id, TDeadline(now + durable_ttl), durable_ttl, serialized_obj, nullptr);
            }
          }
          /* Apply the transactions next */
          Base::TTimer timer;
          const size_t num_transactions_applied = ApplyCoreVectorTransactions(replication_streamer.GetTransactionVec().GetCores(), replication_streamer.GetTransactionVec().GetArena());
          timer.Stop();
          if (timer.GetTotal() > 1s) {
            syslog(LOG_INFO,
                   "Slave PushNotification [%ld] took [%fs]",
                   num_transactions_applied,
                   ToSecondsDouble(timer.GetTotal()));
          }
        } catch (const exception &ex) {
          syslog(LOG_ERR, "Exception in TManager::TSlave::PushNotifications()::Slave [%s]", ex.what());
          throw;
        }
        break;
      }
    }
  }
}

size_t TManager::TSlave::ApplyCoreVectorTransactions(const std::vector<TCore> &core_vec, TCore::TArena *arena) {
  size_t num_applied = 0UL;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  std::vector<TCore>::const_iterator iter = core_vec.begin();
  const std::vector<TCore>::const_iterator end = core_vec.end();
  size_t num_mutations_in_transaction;
  uint32_t action;
  Base::TUuid repo_id;
  TSequenceNumber seq_num;
  size_t num_kv;
  Base::TUuid index_id;
  for (; iter != end; ++iter) {
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(iter->NewState(arena, state_alloc)), num_mutations_in_transaction);
    auto apply_transaction = Manager->NewTransaction();
    ++num_applied;
    for (size_t m_num = 0; m_num < num_mutations_in_transaction; ++m_num) {
      ++iter;
      Sabot::ToNative(*Sabot::State::TAny::TWrapper(iter->NewState(arena, state_alloc)), action);
      ++iter;
      Sabot::ToNative(*Sabot::State::TAny::TWrapper(iter->NewState(arena, state_alloc)), repo_id);
      ++iter;
      Sabot::ToNative(*Sabot::State::TAny::TWrapper(iter->NewState(arena, state_alloc)), seq_num);
      auto repo = Manager->ForceGetRepo(repo_id);
      switch (action) {
        case TTransactionAction::Push : {
          //std::cout << "Apply transaction PUSH [" << repo_id << "]\t[" << seq_num << "]" << std::endl;
          TUpdate::TOpByKey op_by_key;
          ++iter;
          const TCore &meta_core = *iter;
          ++iter;
          const TCore &id_core = *iter;
          ++iter;
          Sabot::ToNative(*Sabot::State::TAny::TWrapper(iter->NewState(arena, state_alloc)), num_kv);
          for (size_t i = 0; i < num_kv; ++i) {
            ++iter;
            Sabot::ToNative(*Sabot::State::TAny::TWrapper(iter->NewState(arena, state_alloc)), index_id);
            ++iter;
            const TCore &key_core = *iter;
            ++iter;
            const TCore &val_core = *iter;
            op_by_key[TIndexKey(index_id, TKey(key_core, arena))] = TKey(val_core, arena);
          }
          //std::cout << "Push\t[" << repo_id << "]\t[" << seq_num << "]" << std::endl;
          apply_transaction->Push(repo, TUpdate::NewUpdate(op_by_key, TKey(meta_core, arena), TKey(id_core, arena)), seq_num);
          break;
        }
        case TTransactionAction::Pop : {
          //std::cout << "Apply transaction POP [" << repo_id << "]\t[" << seq_num << "]" << std::endl;
          //std::cout << "Pop\t[" << repo_id << "]\t[" << seq_num << "]" << std::endl;
          apply_transaction->Pop(repo, seq_num);
          break;
        }
        case TTransactionAction::Fail : {
          apply_transaction->Fail(repo, seq_num);
          break;
        }
        case TTransactionAction::Pause : {
          apply_transaction->Pause(repo, seq_num);
          break;
        }
        case TTransactionAction::UnPause : {
          apply_transaction->UnPause(repo, seq_num);
          break;
        }
        default : {
          throw std::runtime_error("Invalid replication action in TransitionToSlave");
          break;
        }
      }
    }
    apply_transaction->Prepare();
    apply_transaction->CommitAction();
  }
  return num_applied;
}

void TManager::TSlave::TransitionToSlave() {
  try {
    syslog(LOG_INFO, "TSlave::TransitionToSlave()...");
    /* acquire Context lock */ {
      std::lock_guard<std::mutex> lock(Manager->ContextLock);
      switch (Manager->State) {
        case Solo : {
          throw std::runtime_error("Solo can not transition to Slave.");
          break;
        }
        case Master : {
          throw std::runtime_error("Master can not transition to Slave.");
          break;
        }
        case SyncSlave : {
          syslog(LOG_INFO, "TSlave::TransitionToSlave() walking over SlushCoreVec()");
          Flusher->Flush();
          /* we have to rebuild all the core_vectors which we flushed to disk */ {
            Io::TBinaryInputOnlyStream in_stream(Flusher);
            //Disk::TStream in_stream(Disk::Source::SlaveSlush, 0UL, Disk::RealTime, Flusher.get(), 0UL);
            for (size_t i = 0; i < Flusher->GetOffsetVec().size(); ++i) {
              TCoreVector core_vec(in_stream);
              ApplyCoreVectorTransactions(core_vec.GetCores(), core_vec.GetArena());
            }
            syslog(LOG_INFO, "TSlave::TransitionToSlave(): core vector mem slush");
            ApplyCoreVectorTransactions(SlushCoreVec->GetCores(), SlushCoreVec->GetArena());
          }
          Flusher.reset();

          syslog(LOG_INFO, "TSlave::TransitionToSlave() changing state");
          Manager->State = Slave;
          Manager->StateChangeCb(Slave);
          break;
        }
        case Slave : {
          throw std::runtime_error("Slave can not transition to Slave; already in that state.");
          break;
        }
      }
    }  // release Context lock
  } catch (const std::exception &ex) {
    syslog(LOG_EMERG, "TSlave::TransitionToSlave failed [%s]", ex.what());
    abort();
  }
}

L0::TManager::TRepo *TManager::ConstructRepo(const TUuid &repo_id,
                                             const std::optional<TTtl> &ttl,
                                             const std::optional<L0::TManager::TPtr<TRepo>> &parent_repo,
                                             bool is_safe,
                                             bool create) {
  /* TEMP DEBUG */ {
    stringstream ss;
    ss << repo_id;
    syslog(LOG_INFO, "Create Repo [%s] with ttl=[%ld], is_safe=[%s], create=[%s]", ss.str().c_str(), ttl ? ttl->count() : 0, is_safe ? "true" : "false", create ? "true" : "false");
  }
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  if (repo_id != SystemRepoId) {
    assert(SystemRepo);
    if (create) {
      /* only bother marking it down if it's going to live past now. */
      //auto now = TDeadline::clock::now();
      assert(ttl);
      std::optional<Base::TUuid> opt_parent_id;
      if (parent_repo) {
        opt_parent_id = (*parent_repo)->GetId();
      }
      TRepoReplication *repo_replication = new TRepoReplication(repo_id, is_safe, *ttl, opt_parent_id);
      try {
        assert(ttl);
        if (ttl->count() > 0) {
          /* Perform transaction on System repo to construct this repo */ {
            TSuprena arena;
            auto transaction = NewTransaction(false /* don't replicate this transactions, we replicate system changes seperately */);
            transaction->Push(SystemRepo, TUpdate::NewUpdate(TUpdate::TOpByKey{
              { TIndexKey(SystemRepoIndexId, TKey(TSavedRepoKey(SavedRepoMagicNumber, repo_id), &arena, state_alloc)),
                TKey(TSavedRepoObj(is_safe,
                                   TSavedRepoObj::TRootPath(/* TODO(#173): fill in */),
                                   TSavedRepoObj::TOptSeq(),
                                   TSavedRepoObj::TOptSeq(),
                                   TSequenceNumber(1UL),
                                   TSequenceNumber(0UL), /* TODO(#173) : verify? */
                                   TSavedRepoObj::Normal), &arena, state_alloc)
                }}, TKey(), TKey(TUuid(TUuid::Twister), &arena, state_alloc)));
            transaction->Prepare();
            transaction->CommitAction();
          }
        }
        TRepo *repo = is_safe ?
          static_cast<TRepo *>(new TSafeRepo(this, repo_id, *ttl, parent_repo)) :
          static_cast<TRepo *>(new TFastRepo(this, repo_id, *ttl, parent_repo));
        /* NO THROWS */
        /* acquire replication queue lock */ {
          std::lock_guard<std::mutex> lock(GetReplicationQueueLock());
          Enqueue(repo_replication);
        }
        return repo;
      } catch (const std::exception &ex) {
        delete repo_replication;
        throw;
      }
    } else {
      TSuprena arena;
      TIndexKey search_key(SystemRepoIndexId, TKey(TSavedRepoKey(SavedRepoMagicNumber, repo_id), &arena, state_alloc));
      auto view = make_unique<Indy::TRepo::TView>(SystemRepo);
      auto walker_ptr = SystemRepo->NewPresentWalker(view, search_key, true);
      auto &walker = *walker_ptr;
      if (!walker) {
        throw std::runtime_error("Trying to open a repo that does not exist in the System.");
      }
      /* reconstruct repo from existing entry in System repo */
      assert(walker);
      assert(TKey((*walker).Key, (*walker).KeyArena) == search_key.GetKey());
      TSavedRepoObj saved_repo;

      Sabot::State::TAny::TWrapper obj_state((*walker).Op.NewState((*walker).OpArena, state_alloc));
      /* log scope */ {
        std::ostringstream ss;
        obj_state->Accept(Sabot::TStateDumper(ss));
        syslog(LOG_INFO, "TSlave: reconstruct from : %s", ss.str().c_str());
      }
      Sabot::ToNative(*obj_state, saved_repo);

      TStatus status;
      switch (saved_repo.State) {
        case TSavedRepoObj::Normal : {
          status = Normal;
          break;
        }
        case TSavedRepoObj::Paused : {
          status = Paused;
          break;
        }
        case TSavedRepoObj::Failed : {
          status = Failed;
          break;
        }
      }
      Indy::TRepo::TParentRepo parent_repo;
      /* This is the repo-reload boundary (ReconstructRepo -> ConstructRepo with a saved image).
         It is not implemented: the parent repo and ttl below were never resolved, so durable
         on-disk repo reload does not work (it was never ported, see issue #173). The construction
         sketch below is kept, unreached, as a starting point for whoever implements reload. */
      throw std::runtime_error("TManager::ConstructRepo: reconstructing a repo from a saved on-disk image is not implemented (the indy L0 reload-from-disk path was never ported, #173).");
      assert(ttl);
      return saved_repo.IsSafe ?
          static_cast<TRepo *>(new TSafeRepo(this, repo_id, *ttl /* todo fill in with correct val */, parent_repo, saved_repo.LowestSequenceNumber, saved_repo.HighestSequenceNumber, saved_repo.NextUpdate, status)) :
          static_cast<TRepo *>(new TFastRepo(this, repo_id, *ttl /* todo fill in with correct val */, parent_repo, saved_repo.LowestSequenceNumber, saved_repo.HighestSequenceNumber, saved_repo.NextUpdate, status));
    }
  }
  assert(!SystemRepo);
  assert(repo_id == SystemRepoId);
  if (!create && repo_id == SystemRepoId) {
    assert(is_safe);
    auto system_deadline = TDeadline::max();
    return TSafeRepo::ReConstructFromDisk(this, repo_id, system_deadline);
  }
  assert(ttl);
  return is_safe ?
        static_cast<TRepo *>(new TSafeRepo(this, repo_id, *ttl, Indy::TRepo::TParentRepo())) :
        static_cast<TRepo *>(new TFastRepo(this, repo_id, *ttl, Indy::TRepo::TParentRepo()));
}

L0::TManager::TRepo *TManager::ReconstructRepo(const TUuid &repo_id) {
  if (repo_id != SystemRepoId) { /* construct a regular repo */
    /* The repo's original ttl isn't persisted across on-disk reload (#173), so give it the
       same bounded default lifetime used elsewhere for shared povs (session.cc) rather than
       TDeadline::max(), which pinned every reloaded repo in memory forever. */
    auto deadline = TDeadline::clock::now() + std::chrono::seconds(1000);
    return TSafeRepo::ReConstructFromDisk(this, repo_id, deadline);
  }
  assert(!SystemRepo);
  assert(repo_id == SystemRepoId);
  auto system_deadline = TDeadline::max();
  return TSafeRepo::ReConstructFromDisk(this, repo_id, system_deadline);
}

bool TManager::CanLoad(const L0::TId &id) {
  assert(id != SystemRepoId);
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TSuprena arena;
  TIndexKey search_key(SystemRepoIndexId, TKey(TSavedRepoKey(SavedRepoMagicNumber, id), &arena, state_alloc));
  auto view = make_unique<Indy::TRepo::TView>(SystemRepo);
  auto walker_ptr = SystemRepo->NewPresentWalker(view, search_key, true);
  auto &walker = *walker_ptr;
  return static_cast<bool>(walker);
}

void TManager::SaveRepo(TRepo *base_repo) {
  if (base_repo->GetId() != SystemRepoId) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    Orly::Indy::TRepo *repo = dynamic_cast<Orly::Indy::TRepo *>(base_repo);
    assert(repo);
    /* Perform transaction on System repo to save this repo.  The system
       repo is node-local state rebuilt from replication control items
       (TRepoReplication) on the slave -- its transactions must stay off the
       replication stream, where the slave's independently numbered system
       repo dies applying them (#499). */ {
      TSuprena arena;
      auto transaction = NewTransaction(false);
      TStatus status = repo->GetStatus();
      int state = 0;
      switch (status) {
        case Normal : {
          state = TSavedRepoObj::Normal;
          break;
        }
        case Paused : {
          state = TSavedRepoObj::Paused;
          break;
        }
        case Failed : {
          state = TSavedRepoObj::Failed;
          break;
        }
      }
      auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ {
        TIndexKey(SystemRepoIndexId, TKey(TSavedRepoKey(SavedRepoMagicNumber, repo->GetId()), &arena, state_alloc)),
        TKey(TSavedRepoObj(repo->IsSafeRepo(),
                           TSavedRepoObj::TRootPath(/* TODO(#173): fill in */),
                           repo->GetSequenceNumberStart(),
                           repo->GetSequenceNumberLimit(),
                           repo->GetNextSequenceNumber(),
                           repo->GetReleasedUpTo(),
                           state), &arena, state_alloc)} }, TKey(), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
      transaction->Push(SystemRepo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
  }
}

void TManager::SaveIndexNamespaceMapping(const Base::TUuid &index_id, const std::string &namespace_name) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  /* Perform transaction on System repo to save this mapping.  Node-local:
     the mapping crosses to a slave as a TIndexIdReplication control item
     (or the join-time index-map walk), never as this transaction (#499). */ {
    TSuprena arena;
    auto transaction = NewTransaction(false);
    auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ {
      TIndexKey(SystemIDNSIndexId, TKey(make_tuple(index_id), &arena, state_alloc)),
      TKey(namespace_name, &arena, state_alloc)} }, TKey(), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
    transaction->Push(SystemRepo, update);
    transaction->Prepare();
    transaction->CommitAction();
  }
}

void TManager::SaveInstalledPackage(const std::vector<std::string> &package_name, uint64_t version, bool installed) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  /* Perform transaction on System repo to save this record.  Node-local:
     each server records its own installs; replicating this transaction
     would collide with the slave's independently numbered system repo
     (#499). */ {
    TSuprena arena;
    auto transaction = NewTransaction(false);
    auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{ {
      TIndexKey(SystemPackageIndexId, TKey(make_tuple(package_name, static_cast<int64_t>(version)), &arena, state_alloc)),
      TKey(installed, &arena, state_alloc)} }, TKey(), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
    transaction->Push(SystemRepo, update);
    transaction->Prepare();
    transaction->CommitAction();
  }
}

std::vector<std::pair<std::vector<std::string>, uint64_t>> TManager::GetInstalledPackages() {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TSuprena arena;
  std::vector<std::pair<std::vector<std::string>, uint64_t>> ret;
  /* The walker can yield several entries for the same key (one per update
     layer), and their relative order differs between and within generation
     files -- so pick the winner by sequence number explicitly rather than
     assuming any walk order. */
  std::map<std::tuple<std::vector<std::string>, int64_t>, std::pair<TSequenceNumber, bool>> current;
  auto view = make_unique<Indy::TRepo::TView>(SystemRepo);
  auto walker_ptr = SystemRepo->NewPresentWalker(view, TIndexKey(SystemPackageIndexId, TKey(make_tuple(Native::TFree<std::vector<std::string>>(), Native::TFree<int64_t>()), &arena, state_alloc)), true);
  for (auto &walker = *walker_ptr; walker; ++walker) {
    std::tuple<std::vector<std::string>, int64_t> name_and_version;
    bool installed = false;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*walker).Key.NewState((*walker).KeyArena, state_alloc)), name_and_version);
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*walker).Op.NewState((*walker).OpArena, state_alloc)), installed);
    auto &cur = current[name_and_version];
    if ((*walker).SequenceNumber >= cur.first) {
      cur = std::make_pair((*walker).SequenceNumber, installed);
    }
  }
  for (const auto &entry : current) {
    if (entry.second.second) {
      ret.emplace_back(std::get<0>(entry.first), static_cast<uint64_t>(std::get<1>(entry.first)));
    }
  }
  return ret;
}

std::unordered_map<Base::TUuid, std::string> TManager::GetIndexNamespaceMapping() {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TSuprena arena;
  std::unordered_map<Base::TUuid, std::string> ret;
  /* Several entries can appear for the same key (one per update layer) and
     the walk order is not authoritative; pick the winner by sequence number
     (same contract as GetInstalledPackages()). */
  std::unordered_map<Base::TUuid, TSequenceNumber> seq_of;
  auto view = make_unique<Indy::TRepo::TView>(SystemRepo);
  auto walker_ptr = SystemRepo->NewPresentWalker(view, TIndexKey(SystemIDNSIndexId, TKey(make_tuple(Native::TFree<Base::TUuid>()), &arena, state_alloc)), true);
  for (auto &walker = *walker_ptr; walker; ++walker) {
    std::tuple<Base::TUuid> index_id_tuple;
    std::string pkg_key;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*walker).Key.NewState((*walker).KeyArena, state_alloc)), index_id_tuple);
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*walker).Op.NewState((*walker).OpArena, state_alloc)), pkg_key);
    const auto &index_id = std::get<0>(index_id_tuple);
    auto &seq = seq_of[index_id];
    if ((*walker).SequenceNumber >= seq) {
      seq = (*walker).SequenceNumber;
      ret[index_id] = pkg_key;
    }
  }
  return ret;
}

void TManager::OnSlaveJoin(const Base::TFd &fd) {
  assert(Indy::Fiber::TRunner::LocalRunner);
  Indy::Fiber::TRunner *orig_slow_runner = Indy::Fiber::TRunner::LocalRunner;
  Indy::Fiber::TSwitchToRunner RunnerSwitcher(BGFastRunner);
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  switch (State) {
    case Solo : {
      try {
        syslog(LOG_NOTICE, "TMaster: slave joined");
        TSuprena arena;
        std::unique_ptr<Indy::TRepo::TView> view;
        std::shared_ptr<TPresentWalker> walker_ptr;
        /* acquire Context lock */ {
          lock_guard<mutex> lock(ContextLock);
          PromoteSolo(fd);
          view = make_unique<Indy::TRepo::TView>(SystemRepo);
          syslog(LOG_INFO, "TMaster: walking tuple(SavedRepoMagicNumber, free<uuid>)");
          walker_ptr = SystemRepo->NewPresentWalker(view, TIndexKey(SystemRepoIndexId, TKey(make_tuple(SavedRepoMagicNumber, Native::TFree<Base::TUuid>()), &arena, state_alloc)), true);
        }  // release Context lock
        /* sync all the index ids */ {
          TIndexMapReplica index_map_replica;
          ForEachIndexIdCb([&index_map_replica](
              const Base::TUuid &idx_id, const std::string &pkg_key, const Indy::TKey val) {
            index_map_replica.Push(idx_id, pkg_key, val);
          });
          std::shared_ptr<Rpc::TFuture<void>> future = Context->Write<void>(TSlave::IndexId, index_map_replica);
        }
        assert(walker_ptr);
        for (auto &walker = *walker_ptr; walker; ++walker) {
          TSavedRepoKey repo_id_tuple;

          Sabot::ToNative(*Sabot::State::TAny::TWrapper((*walker).Key.NewState((*walker).KeyArena, state_alloc)), repo_id_tuple);

          Base::TUuid repo_id = std::get<1>(repo_id_tuple);
          /* log scope */ {
            std::ostringstream ss;
            ss << repo_id;
            syslog(LOG_INFO, "TMaster: walking tuple(SavedRepoMagicNumber, free<uuid>) matched repo [%s]", ss.str().c_str());
          }
          auto repo = ForceGetRepo(repo_id);
          /* log scope */ {
            std::ostringstream ss;
            ss << repo_id;
            syslog(LOG_INFO, "TMaster: got forced repo [%s]", ss.str().c_str());
          }
          bool is_safe = repo->IsSafeRepo();

          assert(SlaveSyncViewMap.find(repo_id) == SlaveSyncViewMap.end());
          const std::unique_ptr<Indy::TRepo::TView> &sync_view = SlaveSyncViewMap.emplace(repo_id, std::make_unique<Indy::TRepo::TView>(repo)).first->second;

          std::optional<Base::TUuid> parent_repo_id;
          if (repo->GetParentRepo()) {
            parent_repo_id = (*repo->GetParentRepo())->GetId();
          }
          size_t ttl = repo->GetTtl().count();
          Indy::Fiber::TSwitchToRunner go_slow_for_future(orig_slow_runner);
          /* log scope */ {
            std::ostringstream ss;
            ss << "Calling Inventory on Slave (" << repo_id << ") [";
            if (sync_view->GetLower()) { ss << *sync_view->GetLower(); }
            ss << " -> ";
            if (sync_view->GetUpper()) { ss << *sync_view->GetUpper(); }
            ss << "]";
            syslog(LOG_INFO, "TMaster: %s", ss.str().c_str());
          }
          std::shared_ptr<Rpc::TFuture<void>> future = Context->Write<void>(TSlave::InventoryId, repo_id, ttl, parent_repo_id, is_safe, sync_view->GetLower(), sync_view->GetUpper(), sync_view->GetNextId());

          assert(future);
          future->Sync();  // wait for the future to complete
          if (!static_cast<bool>(*future)) {
            throw std::runtime_error("Future did not complete.");
          }
        }
        /* trigger the slave to start synchronizing it's inventory */ {
          /* set false */ {
            std::lock_guard<std::mutex> slave_notify_lock(SlaveNotifyLock);
            SlaveNotifiedFinish = false;
          }
          syslog(LOG_INFO, "TMaster: calling SyncInventoryId on slave");
          Indy::Fiber::TSwitchToRunner go_slow_for_future(orig_slow_runner);
          auto sync_future = Context->Write<void>(TSlave::SyncInventoryId);
          assert(sync_future);
          sync_future->Sync();  // wait for the future to complete
          if (!static_cast<bool>(*sync_future)) {
            throw std::runtime_error("Future did not complete.");
          }
          std::unique_lock<std::mutex> slave_notify_lock(SlaveNotifyLock);
          syslog(LOG_INFO, "TMaster: waiting for slave to say he's finished");
          while (!SlaveNotifiedFinish) {
            /* make sure the slave is still there */ {
              auto sync_future = Context->Write<void>(TSlave::PingId);
              assert(sync_future);
              sync_future->Sync();  // wait for the future to complete
              if (!static_cast<bool>(*sync_future)) {
                throw std::runtime_error("Future did not complete.");
              }
            }
            SlaveNotifyCond.wait_for(slave_notify_lock, 2000ms);
          }
          syslog(LOG_INFO, "TMaster: slave says he's finished");
        }

        Indy::Fiber::TSwitchToRunner go_slow_for_future(orig_slow_runner);
        auto sync_future = Context->Write<void>(TSlave::TransitionToSlaveId);
        assert(sync_future);
        sync_future->Sync();  // wait for the future to complete
        if (!static_cast<bool>(*sync_future)) {
          throw std::runtime_error("Future did not complete.");
        }
        SlaveSyncViewMap.clear();
        syslog(LOG_INFO, "TMaster: finished OnSlaveJoin::Sync()");
        break;
      } catch (const std::exception &ex) {
        syslog(LOG_ERR, "TManager::OnSlaveJoin caught exception [%s]", ex.what());
        SlaveSyncViewMap.clear();
        throw;
      }
    }
    case Master : {
      throw std::runtime_error("A Slave tried to join this Master; Slaves can only join a Solo.");
      break;
    }
    case SyncSlave : {
      throw std::runtime_error("A Slave tried to join this SyncSlave; Slaves can only join a Solo.");
      break;
    }
    case Slave : {
      throw std::runtime_error("A Slave tried to join this Slave; Slaves can only join a Solo.");
      break;
    }
  }
}

void TManager::Enqueue(TTransactionReplication *transaction_replication, L1::TTransaction::TReplica &&replica) NO_THROW {
  /* make sure you acquire the replication queue lock before calling enqueue */
  transaction_replication->SwapReplica(std::forward<L1::TTransaction::TReplica>(replica));
  if (ReplicationQueue.IsEmpty()) {
    ReplicationSem.Push();
  }
  ReplicationQueue.Insert(transaction_replication);
}

void TManager::Enqueue(TRepoReplication *repo_replication) NO_THROW {
  /* make sure you acquire the replication queue lock before calling enqueue */
  if (ReplicationQueue.IsEmpty()) {
    ReplicationSem.Push();
  }
  ReplicationQueue.Insert(repo_replication);
}

void TManager::EnqueueDurable(TDurableReplication *durable_replication) NO_THROW {
  std::lock_guard<std::mutex> lock(ReplicationLock);
  if (ReplicationQueue.IsEmpty()) {
    ReplicationSem.Push();
  }
  ReplicationQueue.Insert(durable_replication);
}

void TManager::Enqueue(TIndexIdReplication *index_replication) NO_THROW {
  syslog(LOG_INFO, "Enqueue IndexId replication");
  std::lock_guard<std::mutex> lock(ReplicationLock);
  if (ReplicationQueue.IsEmpty()) {
    ReplicationSem.Push();
  }
  ReplicationQueue.Insert(index_replication);
}

TDurableReplication *TManager::NewDurableReplication(const Base::TUuid &id, const TTtl &ttl, const std::string &serialized_form) const {
  return new TDurableReplication(id, ttl, serialized_form);
}

void TManager::DeleteDurableReplication(TDurableReplication *durable_replication) NO_THROW {
  delete durable_replication;
}

TTransactionReplication *TManager::NewTransactionReplication() {
  return new TTransactionReplication();
}

void TManager::DeleteTransactionReplication(TTransactionReplication *transaction_replication) NO_THROW {
  delete transaction_replication;
}

void TManager::Demote() {
  assert(State == Master);
  Context.reset();
  State = Solo;
  StateChangeCb(Solo);
  syslog(LOG_NOTICE, "TManager: demoted back to solo");
  auto cb = make_shared<function<void (const TFd &)>>(std::bind(&TManager::OnSlaveJoin, this, placeholders::_1));
  WaitForSlave(cb);
}

void TManager::PromoteSolo(const Base::TFd &fd) {
  assert(State == Solo);
  Context.reset();
  Context = make_shared<TMaster>(this, fd);
  ReplicationRead = true;
  ReplicationWork = true;
  ReplicationQueueSem.Push();
  ReplicationWorkSem.Push();
  State = Master;
  StateChangeCb(Master);
  syslog(LOG_NOTICE, "TManager: solo promoted to master");
}

void TManager::PromoteSlave() {
  assert(State == Slave);
  Context.reset();
  State = Solo;
  StateChangeCb(Solo);
  auto cb = make_shared<function<void (const TFd &)>>(std::bind(&TManager::OnSlaveJoin, this, placeholders::_1));
  WaitForSlave(cb);
  syslog(LOG_NOTICE, "TManager: slave promoted to solo");
}
