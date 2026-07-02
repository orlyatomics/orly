/* <orly/indy/manager.h>

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

#pragma once

#include <mutex>
#include <optional>
#include <thread>

#include <orly/atom/core_vector.h>
#include <orly/atom/core_vector_builder.h>
#include <orly/indy/disk/durable_manager.h>
#include <orly/indy/failover.h>
#include <orly/indy/replication.h>
#include <orly/indy/transaction_base.h>
#include <orly/indy/util/block_vec.h>

namespace Orly {

  namespace Indy {

    /* The id of the global point of view. */
    const Base::TUuid GlobalPovId("C4EF7C46-28C5-4000-8CCD-C8E799E2C3F3");

    /* The index id of the system repo. */
    const Base::TUuid SystemRepoIndexId("9D3DAB7C-2D75-452C-8200-30180FF584F1");
    /* The index id of the system repo space used to store index namespace mappings */
    const Base::TUuid SystemIDNSIndexId("9154D7AE-FA10-42D5-9A10-AC68664B0092");

    class TManager
        : public L1::TManager, public DurableManager::TManager {
      NO_COPY(TManager);
      public:

      enum TState {
        Solo,
        Master,
        SyncSlave,
        Slave
      };

      using TIndexCb = std::function<
          void(const Base::TUuid &idx_id, const std::string &pkg_key, const Indy::TKey &val)>;
      using TForEachIndexIdCb = std::function<void(const TIndexCb &)>;

      TManager(Disk::Util::TEngine *engine,
               size_t replication_sync_slave_buf_size_mb,
               std::chrono::milliseconds merge_mem_delay,
               std::chrono::milliseconds merge_disk_delay,
               std::chrono::milliseconds layer_cleaning_interval,
               std::chrono::milliseconds replication_delay,
               TState state,
               bool allow_tailing,
               bool allow_file_sync,
               bool no_realtime,
               Base::TFd &&socket,
               const std::function<void (const std::shared_ptr<std::function<void (const Base::TFd &)>> &)> &wait_for_slave,
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
               bool create_new);

      virtual ~TManager();

      inline TManager::TPtr<Indy::TRepo> NewSafeRepo(const Base::TUuid &repo_id,
                                                     const std::optional<TTtl> &ttl);

      inline TManager::TPtr<Indy::TRepo> NewSafeRepo(const Base::TUuid &repo_id,
                                                      const std::optional<TTtl> &ttl,
                                                      const TManager::TPtr<Indy::TRepo> &parent_repo);

      TManager::TPtr<Indy::TRepo> NewSafeRepo(const Base::TUuid &repo_id,
                                              const std::optional<TTtl> &ttl,
                                              const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo);

      inline TManager::TPtr<Indy::TRepo> NewFastRepo(const Base::TUuid &repo_id,
                                                     const std::optional<TTtl> &ttl);

      inline TManager::TPtr<Indy::TRepo> NewFastRepo(const Base::TUuid &repo_id,
                                                     const std::optional<TTtl> &ttl,
                                                     const TManager::TPtr<Indy::TRepo> &parent_repo);

      TManager::TPtr<Indy::TRepo> NewFastRepo(const Base::TUuid &repo_id,
                                              const std::optional<TTtl> &ttl,
                                              const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo);

      inline TManager::TPtr<Indy::TRepo> GetRepo(const Base::TUuid &repo_id,
                                                 const std::optional<TTtl> &deadline,
                                                 const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                                                 bool is_safe,
                                                 bool create = true);

      inline TManager::TPtr<Indy::TRepo> ForceGetRepo(const Base::TUuid &repo_id);

      inline TManager::TPtr<Indy::TRepo> GetSystemRepo() const;

      virtual void RunReplicationQueue() override;

      virtual void RunReplicationWork() override;

      virtual void RunReplicateTransaction() override;

      inline void SetDurableManager(const std::shared_ptr<Durable::TManager> &durable_manager) {
        DurableManager = durable_manager;
      }

      static const Base::TUuid MinId;
      static const Base::TUuid MaxId;
      static const Base::TUuid SystemRepoId;

      private:

      enum TTransactionPushType {
        Meta,
        Id,
        Entry,
      };

      /* Prepended to sync repo entries. */
      typedef std::tuple<int, Base::TUuid, Base::TUuid, TSequenceNumber, int> TSyncPrepend;

      typedef std::tuple<int, Base::TUuid> TSavedRepoKey;
      static const int SavedRepoMagicNumber;

      class TSavedRepoObj {
        public:

        typedef std::vector<Base::TUuid> TRootPath;
        typedef std::optional<TSequenceNumber> TOptSeq;

        static const int Normal = 0;
        static const int Paused = 1;
        static const int Failed = 2;

        TSavedRepoObj() {}

        TSavedRepoObj(bool is_safe,
                      const TRootPath &root_path,
                      const TOptSeq &lowest_seq,
                      const TOptSeq &highest_seq,
                      TSequenceNumber next_update,
                      TSequenceNumber released_up_to,
                      int state)
            : IsSafe(is_safe),
              RootPath(root_path),
              LowestSequenceNumber(lowest_seq),
              HighestSequenceNumber(highest_seq),
              NextUpdate(next_update),
              ReleasedUpTo(released_up_to),
              State(state) {
          assert(State == Normal || State == Paused || State == Failed);
        }

        bool operator==(const TSavedRepoObj &that) const {
          return IsSafe == that.IsSafe &&
            RootPath == that.RootPath &&
            LowestSequenceNumber == that.LowestSequenceNumber &&
            HighestSequenceNumber == that.HighestSequenceNumber &&
            NextUpdate == that.NextUpdate &&
            ReleasedUpTo == that.ReleasedUpTo &&
            State == that.State;
        }

        bool IsSafe;

        TRootPath RootPath;

        TOptSeq LowestSequenceNumber;

        TOptSeq HighestSequenceNumber;

        TSequenceNumber NextUpdate;

        TSequenceNumber ReleasedUpTo;

        int State;

      };

      class TMaster
          : public TMasterContext {
        NO_COPY(TMaster);
        public:

        TMaster(TManager *manager, const Base::TFd &fd);

        virtual ~TMaster();

        virtual bool Queue();

        virtual bool Work();

        virtual Util::TContextInputStreamer FetchUpdates(const Base::TUuid &repo_id, TSequenceNumber lowest, TSequenceNumber highest);

        virtual void NotifyFinishSyncInventory();

        virtual TViewDef GetView(const Base::TUuid &repo_id);

        virtual TFileSync SyncFile(const Base::TUuid &file_id, size_t gen_id, size_t context);

        virtual void Ping();

        private:

        TManager *Manager;

      };  // TMaster

      class TSlave
          : public TSlaveContext, Indy::Fiber::TRunnable {
        NO_COPY(TSlave);
        public:

        TSlave(TManager *manager, const Base::TFd &fd);

        virtual ~TSlave();

        virtual bool Queue() override;

        virtual bool Work() override;

        virtual void Inventory(const Base::TUuid &repo_id,
                               size_t ttl,
                               const std::optional<Base::TUuid> &parent_repo_id,
                               bool is_safe,
                               const std::optional<TSequenceNumber> &lowest,
                               const std::optional<TSequenceNumber> &highest,
                               TSequenceNumber next_id) override;

        virtual void Index(const TIndexMapReplica &index_map_replica) override;

        virtual void PushNotifications(const TReplicationStreamer &replication_streamer) override;

        size_t ApplyCoreVectorTransactions(const std::vector<Atom::TCore> &core_vec, Atom::TCore::TArena *arena);

        virtual void TransitionToSlave() override;

        virtual void Ping();

        virtual void ScheduleSyncInventory() override;

        private:

        void SyncInventory();

        void PullUpdateRange(const Base::TUuid &repo_id, TManager::TPtr<Indy::TRepo> &repo, TSequenceNumber from, TSequenceNumber to);

        class TFlusher
            : public Io::TOutputConsumer,
              public Io::TInputProducer,
              public Disk::TInFile {
          NO_COPY(TFlusher);
          public:

          typedef Disk::TStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::CheckedPage, 0UL /*local cache size */> TDataInStream;
          typedef Disk::TOutStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::PageCheckedBlock> TDataOutStream;

          TFlusher(Disk::Util::TEngine *engine)
              : Engine(engine),
                OutStream(new TDataOutStream(HERE,
                                             Disk::Source::SlaveSlush,
                                             Engine->GetVolMan(),
                                             0UL,
                                             BlockVec,
                                             CollisionMap,
                                             Trigger,
                                             Disk::Low,
                                             true,
                                             #ifndef NDEBUG
                                             WrittenBlockSet,
                                             #endif
                                             std::bind(&TFlusher::NewBlockCb, this, std::placeholders::_1))),
                Pool(std::make_shared<Io::TPool>(Io::TPool::TArgs())) {}

          virtual ~TFlusher() {
            InStream.reset();
            for (const auto &iter : BlockVec.GetSeqBlockMap()) {
              Engine->FreeSeqBlocks(iter.second.first, iter.second.second);
            }
          }

          virtual void ConsumeOutput(const std::shared_ptr<const Io::TChunk> &chunk) override;

          virtual std::shared_ptr<const Io::TChunk> TryProduceInput() override;

          const std::vector<size_t> &GetOffsetVec() const {
            return OffsetVec;
          }

          void SetStoredOffset() {
            OffsetVec.push_back(OutStream->GetOffset());
          }

          void Flush();

          private:

          virtual size_t GetFileLength() const override {
            return BlockVec.Size() * Disk::Util::LogicalBlockSize;
          }

          virtual size_t GetStartingBlock() const override {
            assert(BlockVec.Size() > 0);
            return BlockVec.Front();
          }

          virtual void ReadMeta(size_t /*offset*/, size_t &/*out*/) const override {
            throw std::logic_error("Manager::Flusher::ReadMeta should not be used.");
          }

          virtual size_t FindPageIdOfByte(size_t offset) const override {
            assert(offset <= GetFileLength());
            return ((BlockVec[offset / Disk::Util::LogicalBlockSize]) * Disk::Util::PagesPerBlock) + ((offset % Disk::Util::LogicalBlockSize) / Disk::Util::LogicalPageSize);
          }

          size_t NewBlockCb(Disk::Util::TVolumeManager */*vol_man*/);

          Disk::Util::TEngine *Engine;

          Util::TBlockVec BlockVec;

          std::unordered_map<size_t, std::shared_ptr<const Disk::TBufBlock>> CollisionMap;

          Disk::TCompletionTrigger Trigger;

          #ifndef NDEBUG
          std::unordered_set<size_t> WrittenBlockSet;
          #endif

          std::unique_ptr<TDataOutStream> OutStream;

          std::vector<size_t> OffsetVec;

          std::shared_ptr<Io::TPool> Pool;

          std::unique_ptr<TDataInStream> InStream;

        };  // TFlusher

        class TToSync {
          public:

          TToSync(const Base::TUuid &repo_id,
                  size_t ttl,
                  const std::optional<Base::TUuid> &parent_repo_id,
                  bool is_safe,
                  const std::optional<TSequenceNumber> &lowest,
                  const std::optional<TSequenceNumber> &highest,
                  TSequenceNumber next_id)
              : RepoId(repo_id),
                Ttl(ttl),
                ParentRepoId(parent_repo_id),
                IsSafe(is_safe),
                Lowest(lowest),
                Highest(highest),
                NextId(next_id) {}

          private:

          Base::TUuid RepoId;
          size_t Ttl;
          std::optional<Base::TUuid> ParentRepoId;
          bool IsSafe;
          std::optional<TSequenceNumber> Lowest;
          std::optional<TSequenceNumber> Highest;
          TSequenceNumber NextId;

          friend class TSlave;

        };  // TToSync

        TManager *Manager;

        /* queue of repos to synchronize */
        std::vector<TToSync> ToSyncQueue;

        /* A core-vec slush buffer representing the updates we've accumulated */
        std::unique_ptr<Orly::Atom::TCoreVectorBuilder> SlushCoreVec;

        std::shared_ptr<TFlusher> Flusher;

      };  // TSlave

      virtual TRepo *ConstructRepo(const Base::TUuid &repo_id,
                                   const std::optional<TTtl> &ttl,
                                   const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                                   bool is_safe,
                                   bool create) override;

      TRepo *ReconstructRepo(const Base::TUuid &repo_id) override;

      virtual bool CanLoad(const L0::TId &id) override;

      /* The L0 manager's load/save/delete of durable objects by id. Not wired up: the call sites
         are gated off (TObj::OnDisk is never set true; the loader is #if 0'd), so these are
         unreachable today. This is distinct from the server's working persistence (the spa
         checkpoint replays statements; orlyi runs on a real disk engine) -- it is specifically the
         in-process reload-from-on-disk-image path that was never ported. Fail clearly rather than
         with a bare placeholder if a path ever reaches them. See issue #173. */
      virtual void Delete(const L0::TId &/*id*/, L0::TSem */*sem*/) override {
        throw std::logic_error("TManager::Delete not implemented: durable on-disk object deletion was never ported (#173).");
      }

      virtual void Save(const L0::TId &/*id*/, const L0::TDeadline &/*deadline*/, const std::string &/*blob*/, L0::TSem */*sem*/) override {
        throw std::logic_error("TManager::Save not implemented: durable on-disk object save was never ported (#173).");
      }

      virtual bool TryLoad(const L0::TId &/*id*/, std::string &/*blob*/) override {
        throw std::logic_error("TManager::TryLoad not implemented: durable on-disk object load was never ported (#173).");
      }

      virtual void SaveRepo(TRepo *base_repo) override;

      void SaveIndexNamespaceMapping(const Base::TUuid &index_id, const std::string &namespace_name);

      std::unordered_map<Base::TUuid, std::string> GetIndexNamespaceMapping();

      void OnSlaveJoin(const Base::TFd &fd);

      inline virtual std::mutex &GetReplicationQueueLock() NO_THROW {
        return ReplicationLock;
      }

      virtual void Enqueue(TTransactionReplication *transaction_replication, L1::TTransaction::TReplica &&replica) NO_THROW;

      virtual void Enqueue(TRepoReplication *repo_replication) NO_THROW;

      void Enqueue(TIndexIdReplication *index_replication) NO_THROW;

      virtual TTransactionReplication *NewTransactionReplication();

      virtual void DeleteTransactionReplication(TTransactionReplication *transaction_replication) NO_THROW override;

      virtual void EnqueueDurable(TDurableReplication *durable_replication) NO_THROW override;

      /* TOOD */
      virtual TDurableReplication *NewDurableReplication(const Base::TUuid &id, const TTtl &ttl, const std::string &serialized_form) const override;

      virtual void DeleteDurableReplication(TDurableReplication *durable_replication) NO_THROW override;

      void Demote();

      void PromoteSolo(const Base::TFd &fd);

      void PromoteSlave();

      void AugmentViewMapWithDiskLayers(TMaster::TViewDef &view_def, const std::unique_ptr<Indy::TRepo::TView> &view) const;

      virtual void ForEachScheduler(const std::function<bool (Fiber::TRunner *)> &cb) const {
        ForEachSchedulerCb(cb);
      }

      TManager::TPtr<Indy::TRepo> SystemRepo;

      TState State;

      std::shared_ptr<TCommonContext> Context;
      std::mutex ContextLock;

      bool SlaveNotifiedFinish;
      std::mutex SlaveNotifyLock;
      std::condition_variable SlaveNotifyCond;

      std::function<void (const std::shared_ptr<std::function<void (const Base::TFd &)>> &)> WaitForSlave;

      std::function<void (TState)> StateChangeCb;

      bool ReplicationRead, ReplicationWork;

      TReplicationQueue ReplicationQueue;
      std::mutex ReplicationLock;

      Base::TFd ReplicationQueueEpollFd;
      std::mutex ReplicationQueueEpollLock;
      epoll_event ReplicationQueueEvent;
      Base::TEventSemaphore ReplicationQueueSem;

      Base::TFd ReplicationWorkEpollFd;
      std::mutex ReplicationWorkEpollLock;
      epoll_event ReplicationWorkEvent;
      Base::TEventSemaphore ReplicationWorkSem;

      Base::TFd ReplicationEpollFd;
      std::mutex ReplicationEpollLock;
      epoll_event ReplicationEvent;
      Base::TEventSemaphore ReplicationSem;
      std::chrono::steady_clock::time_point ReplicationNextTime;

      std::chrono::milliseconds ReplicationDelay;

      std::function<void (const Base::TUuid &, const Base::TUuid &, const Base::TUuid &)> UpdateReplicationNotificationCb;

      TIndexCb OnReplicateIndexIdCb;
      TForEachIndexIdCb ForEachIndexIdCb;

      std::function<void (const std::function<bool (Fiber::TRunner *)> &)> ForEachSchedulerCb;

      std::shared_ptr<Durable::TManager> DurableManager;

      Indy::Fiber::TRunner *BGFastRunner;

      size_t ReplicationSyncSlaveBufSizeBytes;

      bool AllowFileSync;

      std::unordered_map<Base::TUuid, std::unique_ptr<Indy::TRepo::TView>> SlaveSyncViewMap;

      friend class Orly::Server::TServer;

    };  // TManager

    namespace L0 {

      template<>
      template<>
      inline TManager::TPtr<Indy::TRepo>::TPtr(const TPtr<L0::TManager::TRepo> &that) {
        if ((SomeObj = dynamic_cast<Indy::TRepo *>(that.SomeObj)) != nullptr) {
          SomeObj->OnPtrAcquire();
        }
      }

    }  // L0

    /***************
      *** inline ***
      *************/

    inline TManager::TPtr<Indy::TRepo> TManager::NewSafeRepo(const Base::TUuid &repo_id,
                                                             const std::optional<TTtl> &ttl) {
      return NewSafeRepo(repo_id, ttl, std::optional<TManager::TPtr<L0::TManager::TRepo>>());
    }

    inline TManager::TPtr<Indy::TRepo> TManager::NewSafeRepo(const Base::TUuid &repo_id,
                                                             const std::optional<TTtl> &ttl,
                                                             const TManager::TPtr<Indy::TRepo> &parent_repo) {
      return NewSafeRepo(repo_id, ttl, std::optional<L0::TManager::TPtr<L0::TManager::TRepo>>(parent_repo));
    }

    inline TManager::TPtr<Indy::TRepo> TManager::NewFastRepo(const Base::TUuid &repo_id,
                                                             const std::optional<TTtl> &ttl) {
      return NewFastRepo(repo_id, ttl, std::optional<TManager::TPtr<L0::TManager::TRepo>>());
    }

    inline TManager::TPtr<Indy::TRepo> TManager::NewFastRepo(const Base::TUuid &repo_id,
                                                              const std::optional<TTtl> &ttl,
                                                              const TManager::TPtr<Indy::TRepo> &parent_repo) {
      return NewFastRepo(repo_id, ttl, std::optional<L0::TManager::TPtr<L0::TManager::TRepo>>(parent_repo));
    }

    inline TManager::TPtr<Indy::TRepo> TManager::GetRepo(const Base::TUuid &repo_id,
                                                          const std::optional<TTtl> &ttl,
                                                          const std::optional<TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                                                          bool is_safe,
                                                          bool create) {
      return create ? OpenOrCreate(repo_id, ttl, parent_repo, is_safe) : ForceOpenRepo(repo_id);
    }

    inline TManager::TPtr<Indy::TRepo> TManager::ForceGetRepo(const Base::TUuid &repo_id) {
      return ForceOpenRepo(repo_id);
    }

    inline TManager::TPtr<Indy::TRepo> TManager::GetSystemRepo() const {
      return SystemRepo;
    }

    inline void TManager::TMaster::Ping() {}

    inline void TManager::TSlave::Ping() {}

  }  // Indy

}  // Orly