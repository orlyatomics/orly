/* <orly/indy/disk/durable_manager.h>

   Durable-state manager: tracks `Durable::TManager` objects that need
   periodic flush-to-disk, batches their serialized form into one stream,
   and replays them on startup. `TFlush` schedules the periodic flush
   tick; `TDurableManager` owns the per-object layers and the runner loop
   that processes them. Large header -- the public `TDurableManager`
   class declaration lives well below the top.

   Consumers: `orly/server/orlyi`, the replication path.

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

#include <chrono>
#include <cstdint>
#include <functional>

#include <base/class_traits.h>
#include <base/epoll.h>
#include <base/event_counter.h>
#include <base/event_semaphore.h>
#include <base/scheduler.h>
#include <base/timer_fd.h>
#include <base/uuid.h>
#include <base/inv_con/unordered_list.h>
#include <orly/durable/kit.h>
#include <orly/indy/disk/in_file.h>
#include <orly/indy/disk/out_stream.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/disk/util/index_manager.h>
#include <orly/indy/replication.h>
#include <orly/indy/util/block_vec.h>
#include <orly/indy/util/lockless_pool.h>
#include <orly/indy/util/pool.h>

namespace Orly {

  namespace Server {
    /* Forward Declarations. */
    class TIndyReporter;
  }

  namespace Indy {

    namespace Disk {

      namespace Util {

        class TDiskEngine;

      }

    }


    namespace DurableManager {

      class TManager
          : public Fiber::TRunnable {
        NO_COPY(TManager);
        public:

        virtual TDurableReplication *NewDurableReplication(const Durable::TId &id, const Durable::TTtl &ttl, const std::string &serialized_form) const = 0;

        virtual void DeleteDurableReplication(TDurableReplication *durable_replication) NO_THROW = 0;

        virtual void EnqueueDurable(TDurableReplication *durable_replication) NO_THROW = 0;

        protected:

        TManager() {}

        virtual ~TManager() {}

      };  // TManager

    }  // DurableManager

    namespace Disk {

      /* Helper Used for maintaining a simple counter of when to flush. */
      class TFlush {
        public:

        /* Sets the next flush for delay milliseconds after construction */
        TFlush(std::chrono::milliseconds delay);

        /* Sleeps until the current next flush time, then updates the flush time. */
        void WaitFor();
        void UpdateNext();

        private:
        std::chrono::milliseconds Delay;
        std::chrono::steady_clock::time_point Next;
      };

      class TDurableManager
          : public Durable::TManager {
        NO_COPY(TDurableManager);
        private:

        /* Forward Declarations. */
        class TDurableLayer;

        typedef InvCon::UnorderedList::TCollection<TDurableManager, TDurableLayer> TRemovalCollection;

        public:

        typedef uint64_t TSequenceNumber;
        typedef uint32_t TSerializedSize;

        typedef TStream<Util::LogicalPageSize, Util::LogicalBlockSize, Util::PhysicalBlockSize, Util::CheckedPage, 64UL> TInStream;
        typedef TOutStream<Disk::Util::LogicalPageSize, Disk::Util::LogicalBlockSize, Disk::Util::PhysicalBlockSize, Disk::Util::PageCheckedBlock> TDataOutStream;

        enum TOutcome { Survived, Expired, WasSuperceded };

        using TNotify = std::function<void (TSequenceNumber, Base::TUuid, TOutcome)>;

        TDurableManager(Base::TScheduler *scheduler,
                        Fiber::TRunner::TRunnerCons &runner_cons,
                        Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                        DurableManager::TManager *manager,
                        Util::TEngine *engine,
                        size_t max_cache_size,
                        std::chrono::milliseconds write_delay,
                        std::chrono::milliseconds merge_delay,
                        std::chrono::milliseconds layer_cleaning_interval,
                        size_t temp_file_consol_thresh,
                        bool create,
                        const TNotify *notify = nullptr);

        virtual ~TDurableManager();

        virtual bool CanLoad(const Durable::TId &id) override;

        virtual void CleanDisk(const Durable::TDeadline &now, Durable::TSem *sem) override;

        virtual void Delete(const Durable::TId &id, Durable::TSem *sem) override;

        virtual void RunLayerCleaner() override;

        virtual void Save(const Durable::TId &id, const Durable::TDeadline &deadline, const TTtl &ttl, const std::string &serialized_form, Durable::TSem *sem) override;

        virtual bool TryLoad(const Durable::TId &id, std::string &serialized_form_out) override;

        void RunWriter();

        void RunMerger();

        void static InitMappingPool(size_t num_obj) {
          TMapping::Pool.Init(num_obj);
        }

        static constexpr size_t GetMappingSize() {
          return sizeof(TMapping);
        }

        void static InitMappingEntryPool(size_t num_obj) {
          TMapping::TEntry::Pool.Init(num_obj);
        }

        static constexpr size_t GetMappingEntrySize() {
          return sizeof(TMapping::TEntry);
        }

        void static InitDurableLayerPool(size_t num_obj) {
          TDurableLayer::Pool.Init(num_obj);
        }

        static constexpr size_t GetDurableLayerSize() {
          return sizeof(TDurableLayer);
        }

        void static InitMemEntryPool(size_t num_obj) {
          TMemSlushLayer::TDurableEntry::Pool.Init(num_obj);
        }

        static constexpr size_t GetMemEntrySize() {
          return sizeof(TMemSlushLayer::TDurableEntry);
        }

        static const Base::TUuid DurableByIdFileId;

        private:

        /* Forward Declarations. */
        class TMemSlushLayer;
        class TMergeSortedByIdFile;

        /* Swap out the current memory slush layer (if non-empty), write it to disk, release the
           savers whose entries it holds (their durability sems, #277), and swap the new disk
           layer into the mapping.  Returns true iff there was anything to flush.  When
           'retire_writer' is set (the writer's shutdown drain), also marks the writer retired
           under DataLock so later saves signal their own sems instead of waiting forever. */
        bool FlushCurLayer(bool retire_writer);

        /* Push (and clear) the durability sem of every entry in the given layer.  Called by
           FlushCurLayer() once the layer is confirmed on disk -- or if the write failed, so a
           dying disk service cannot strand savers. */
        static void ReleaseSavers(TMemSlushLayer *mem_layer);

        class TSortedByIdFile {
          NO_COPY(TSortedByIdFile);
          public:

          TSortedByIdFile(TMemSlushLayer *mem_layer,
                          Util::TEngine *engine,
                          Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                          size_t gen_id,
                          size_t latest_deadline_count,
                          size_t temp_file_consol_thresh,
                          DiskPriority priority);

          inline size_t GetNumDurable() const;

          /*
             1. NumEntries
             2. NumBlocks
             3. HashIndexOffset
             4. HashFieldSize
          */

          static const size_t NumMetaFields = 4UL;
          /*
             1. durable id
             2. seq_num
             3. deadline count
             4. size of serialized string.
          */
            static const size_t DurableEntrySize = sizeof(uuid_t) + sizeof(TSequenceNumber) + sizeof(size_t) + sizeof(TSerializedSize);

          static const size_t HashEntrySize = sizeof(uuid_t) + sizeof(size_t);

          static const Base::TUuid NullId;

          private:

          class THashObj {
            public:

            THashObj(const uuid_t &id, size_t hash, size_t offset)
                : Hash(hash), Offset(offset) {
              uuid_copy(Id, id);
            }

            bool operator<(const THashObj &that) const {
              return Hash < that.Hash;
            }

            uuid_t Id;

            size_t Hash;

            size_t Offset;

          };  // THashObj

          typedef Util::TIndexManager<THashObj, Disk::Util::SortBufSize, Disk::Util::SortBufMinParallelSize> THashSorter;

          template <typename TOwner>
          class TMyFileTemp
              : public TInFile {
            NO_COPY(TMyFileTemp);
            public:

            TMyFileTemp(TOwner *file) : File(file) {}

            virtual ~TMyFileTemp() {}

            private:

            virtual size_t GetFileLength() const override {
              return File->FileSize;
            }

            virtual size_t GetStartingBlock() const override {
              assert(File->BlockVec.Size() > 0);
              return File->BlockVec.Front();
            }

            virtual void ReadMeta(size_t , size_t &) const override {
              throw;
            }

            virtual size_t FindPageIdOfByte(size_t offset) const override {
              assert(offset <= GetFileLength());
              return ((File->BlockVec[offset / Disk::Util::LogicalBlockSize]) * Disk::Util::PagesPerBlock) + ((offset % Disk::Util::LogicalBlockSize) / Disk::Util::LogicalPageSize);
            }

            TOwner *File;

          };  // TMyFileTemp

          typedef TMyFileTemp<TSortedByIdFile> TMyFile;

          Util::TEngine *const Engine;

          const Disk::Util::TVolume::TDesc::TStorageSpeed StorageSpeed;

          Indy::Util::TBlockVec BlockVec;
          size_t FileSize;

          size_t NumDurable;

          friend class TMergeSortedByIdFile;

        };  // TSortedByIdFile

        class TMergeSortedByIdFile {
          NO_COPY(TMergeSortedByIdFile);
          public:

          TMergeSortedByIdFile(const std::vector<size_t> &gen_vec,
                               Util::TEngine *engine,
                               Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                               size_t gen_id,
                               size_t latest_deadline_count,
                               size_t temp_file_consol_thresh,
                               DiskPriority priority,
                               const TNotify *notify);


          inline size_t GetNumDurable() const;

          private:

          typedef TSortedByIdFile::THashObj THashObj;

          typedef TSortedByIdFile::THashSorter THashSorter;

          typedef TSortedByIdFile::TMyFileTemp<TMergeSortedByIdFile> TMyFile;

          Util::TEngine *Engine;

          Disk::Util::TVolume::TDesc::TStorageSpeed StorageSpeed;

          size_t NumDurable;

          Indy::Util::TBlockVec BlockVec;
          size_t FileSize;

          static const size_t LocalCacheSize = 64;

          friend TMyFile;

        };  // TMergeSortedByIdFile

        class TSortedInFile
            : public TInFile {
          NO_COPY(TSortedInFile);
          public:

          TSortedInFile(Util::TPageCache *page_cache,
                        DiskPriority priority,
                        size_t gen_id,
                        size_t starting_block_id,
                        size_t starting_block_offset,
                        size_t file_length);

          TSortedInFile(Util::TEngine *engine, DiskPriority priority, size_t gen_id);

          virtual ~TSortedInFile();

          virtual size_t GetFileLength() const override;

          virtual size_t GetStartingBlock() const override;

          virtual void ReadMeta(size_t offset, size_t &out) const override;

          virtual size_t FindPageIdOfByte(size_t offset) const override;

          inline size_t GetNumEntries() const;

          inline size_t GetNumBlocks() const;

          inline size_t GetStartOfDurableByIdIndex() const;

          inline size_t GetStartOfHashIndex() const;

          void FindInHash(TSequenceNumber &cur_max_seq_num, const Durable::TId &id, std::string &serialized_form_out) const;

          private:

          Util::TPageCache *PageCache;

          size_t FileLength;

          size_t StartingBlockId;

          size_t StartingBlockOffset;

          size_t NumEntries;

          size_t NumBlocks;

          size_t HashIndexOffset;

          size_t HashFieldSize;

          std::unique_ptr<TInStream> InStream;

          static const size_t LocalCacheSize = 64;

        };  // TSortedInFile

        class TMapping {
          NO_COPY(TMapping);
          public:

          /* Forward Declarations. */
          class TEntry;

          typedef InvCon::UnorderedList::TMembership<TMapping, TDurableManager> TManagerMembership;

          typedef InvCon::UnorderedList::TCollection<TMapping, TEntry> TEntryCollection;

          class TEntry {
            NO_COPY(TEntry);
            public:

            typedef InvCon::UnorderedList::TMembership<TEntry, TMapping> TMappingMembership;

            inline TEntry(TMapping *mapping, TDurableLayer *layer);

            inline ~TEntry();

            inline TDurableLayer *GetLayer() const;

            static void *operator new(size_t size) {
              return Pool.Alloc(size);
            }

            static void operator delete(void *ptr, size_t) {
              Pool.Free(ptr);
            }

            private:

            TMappingMembership::TImpl MappingMembership;

            TDurableLayer *Layer;

            static Indy::Util::TLocklessPool Pool;

            friend class TDurableManager;
            friend class Server::TIndyReporter;

          };  // TEntry

          class TView {
            NO_COPY(TView);
            public:

            TView(TDurableManager *manager)
                : Manager(manager) {
              std::lock_guard<std::mutex> data_lock(Manager->DataLock);
              std::lock_guard<std::mutex> mapping_lock(Manager->MappingLock);
              Mapping = Manager->MappingCollection.TryGetLastMember();
              CurMemLayer = Manager->CurMemoryLayer;
              assert(CurMemLayer);
              CurMemLayer->Incr();
              Mapping->Incr();
            }

            ~TView() {
              CurMemLayer->Decr();
              std::lock_guard<std::mutex> mapping_lock(Manager->MappingLock);
              Mapping->Decr();
            }

            inline TMapping *GetMapping() const;

            inline TMemSlushLayer *GetCurLayer() const;

            private:

            TDurableManager *Manager;

            TMapping *Mapping;

            TMemSlushLayer *CurMemLayer;

          };  // TView

          inline TMapping(TDurableManager *manager);

          ~TMapping();

          inline void Incr();

          inline void Decr();

          inline size_t GetRefCount() const;

          TEntryCollection *GetEntryCollection() const;

          static void *operator new(size_t size) {
            return Pool.Alloc(size);
          }

          static void operator delete(void *ptr, size_t) {
            Pool.Free(ptr);
          }

          private:

          TManagerMembership::TImpl ManagerMembership;

          mutable TEntryCollection::TImpl EntryCollection;

          size_t RefCount;

          bool MarkedForDelete;

          static Indy::Util::TLocklessPool Pool;

          friend class TDurableManager;
          friend class Server::TIndyReporter;

        };  // TMapping

        typedef InvCon::UnorderedList::TCollection<TDurableManager, TMapping> TMappingCollection;

        class TDurableLayer {
          NO_COPY(TDurableLayer);
          public:

          typedef InvCon::UnorderedList::TMembership<TDurableLayer, TDurableManager> TRemovalMembership;

          enum TKind {MemSlush, DiskOrdered};

          inline virtual ~TDurableLayer();

          inline void Incr();

          inline void Decr();

          inline void RemoveFromCollection();

          virtual TKind GetKind() const = 0;

          inline bool GetMarkedTaken() const;

          inline void MarkTaken();

          inline bool GetMarkedForDelete() const;

          inline void MarkForDelete();

          virtual void FindMax(TSequenceNumber &cur_max_seq, const Base::TUuid &id, std::string &serialized_form_out) const = 0;

          static void *operator new(size_t size) {
            return Pool.Alloc(size);
          }

          static void operator delete(void *ptr, size_t) {
            Pool.Free(ptr);
          }

          protected:

          inline TDurableLayer(TDurableManager *manager);

          private:

          TDurableManager *Manager;

          TRemovalMembership::TImpl RemovalMembership;

          size_t RefCount;

          bool MarkedForDelete;

          bool MarkedTaken;

          static Indy::Util::TPool Pool;

          friend class TDurableManager;
          friend class Server::TIndyReporter;

        };  // TDurableLayer

        void AddMapping(TDurableLayer *layer);

        class TMemSlushLayer
            : public TDurableLayer {
          NO_COPY(TMemSlushLayer);
          public:

          class TDurableEntry {
            NO_COPY(TDurableEntry);
            public:

            class TKey {
              public:

              inline TKey(const uuid_t &id, TSequenceNumber seq_num);

              inline bool operator==(const TKey &that) const;

              inline bool operator!=(const TKey &that) const;

              inline bool operator<=(const TKey &that) const;

              private:

              uuid_t Id;

              const TSequenceNumber SeqNum;

              friend class TDurableEntry;

            };  // TKey

            typedef InvCon::OrderedList::TMembership<TDurableEntry, TMemSlushLayer, TDurableEntry::TKey> TSlushMembership;

            inline TDurableEntry(TMemSlushLayer *mem_layer, const Base::TUuid &id, const Durable::TDeadline &deadline, std::string &&serialized_form, TSequenceNumber seq_num, Durable::TSem *sem);

            inline const uuid_t &GetId() const;

            inline TSequenceNumber GetSeqNum() const;

            inline size_t GetDeadlineCount() const;

            inline TSerializedSize GetSerializedSize() const;

            inline const std::string &GetSerializedForm() const;

            static void *operator new(size_t size) {
              return Pool.Alloc(size);
            }

            static void operator delete(void *ptr, size_t) {
              Pool.Free(ptr);
            }

            private:

            TSlushMembership::TImpl SlushMembership;

            const size_t DeadlineCount;

            std::string SerializedForm;

            /* The saver's durability semaphore, if it wants one (may be null).  Save() parks it
               here instead of pushing it, and the writer fiber pushes it once the layer holding
               this entry has been confirmed written to disk (#277).  If the manager shuts down
               before the flush, the final drain pushes it so no saver is left blocked. */
            Durable::TSem *Sem;

            static Indy::Util::TPool Pool;

            friend class TDurableManager;
            friend class Server::TIndyReporter;

          };  // TDurableEntry

          typedef InvCon::OrderedList::TCollection<TMemSlushLayer, TDurableEntry, TDurableEntry::TKey> TEntryCollection;

          virtual TKind GetKind() const {
            return TDurableLayer::MemSlush;
          }

          inline TMemSlushLayer(TDurableManager *manager);

          virtual ~TMemSlushLayer();

          inline size_t GetNumEntries() const;

          inline size_t GetTotalSerializedSize() const;

          inline TEntryCollection *GetEntryCollection() const;

          virtual void FindMax(TSequenceNumber &cur_max_seq, const Base::TUuid &id, std::string &serialized_form_out) const;

          private:

          mutable TEntryCollection::TImpl EntryCollection;

          size_t NumEntries;

          size_t TotalSerializedSize;

          friend class TDurableManager;

        };  // TMemSlushLayer

        class TDiskOrderedLayer
            : public TDurableLayer {
          NO_COPY(TDiskOrderedLayer);
          public:

          virtual TKind GetKind() const {
            return TDurableLayer::DiskOrdered;
          }

          TDiskOrderedLayer(TDurableManager *manager, Util::TEngine *engine, size_t gen_id, size_t num_durable);

          virtual ~TDiskOrderedLayer();

          inline size_t GetGenId() const;

          inline size_t GetNumDurable() const;

          virtual void FindMax(TSequenceNumber &cur_max_seq, const Base::TUuid &id, std::string &serialized_form_out) const;

          private:

          Util::TEngine *const Engine;

          size_t GenId;

          size_t NumDurable;

        };  // TDiskOrderedLayer

        DurableManager::TManager *const Manager;

        Fiber::TRunner MergerScheduler;
        Fiber::TRunner WriterScheduler;
        Fiber::TFrame *MergerFrame;
        Fiber::TFrame *WriterFrame;
        Fiber::TSingleSem MergerFinishedSem;
        Fiber::TSingleSem WriterFinishedSem;

        Util::TEngine *const Engine;

        std::mutex DataLock;
        TMemSlushLayer *CurMemoryLayer;

        TSequenceNumber SeqNum;

        size_t NextSlushGenId;

        size_t NextDurableByIdGenId;

        size_t TempFileConsolThresh;

        std::chrono::milliseconds DurableWriteDelay;
        std::chrono::milliseconds DurableMergeDelay;

        /* Written by the destructor's thread, read each round by the writer and merger fibers. */
        std::atomic<bool> ShutDown;

        /* Covered by DataLock.  Set by the writer's shutdown drain (FlushCurLayer(true)); once
           set, Save() signals the caller's sem itself since no writer will ever flush again. */
        bool WriterRetired;

        /* Pushed once by each of the two scheduler jobs hosting WriterScheduler and
           MergerScheduler as their very last act.  The destructor pops it twice before member
           destruction so the runner loops can never scan a sibling TRunner we've already torn
           down (they run as scheduler jobs, so there is no thread to join). */
        Base::TEventSemaphore SchedulerExitedSem;

        Fiber::TSingleSem SlushSem;
        Fiber::TSingleSem MergeSem;

        mutable TMappingCollection::TImpl MappingCollection;

        mutable TRemovalCollection::TImpl RemovalCollection;
        Base::TTimerFd LayerCleanerTimer;

        std::mutex RemovalLock;

        std::mutex MappingLock;

        const TNotify *Notify;

        friend class Server::TIndyReporter;
        friend class Util::TDiskEngine;

      };  // TDurableManager

      /***************
        *** Inline ***
        *************/

      inline size_t TDurableManager::TSortedInFile::GetNumEntries() const {
        return NumEntries;
      }

      inline size_t TDurableManager::TSortedInFile::GetNumBlocks() const {
        return NumBlocks;
      }

      inline size_t TDurableManager::TSortedInFile::GetStartOfDurableByIdIndex() const {
        return (TSortedByIdFile::NumMetaFields + GetNumBlocks()) * sizeof(size_t);
      }

      inline size_t TDurableManager::TSortedInFile::GetStartOfHashIndex() const {
        return HashIndexOffset;
      }

      inline size_t TDurableManager::TSortedByIdFile::GetNumDurable() const {
        return NumDurable;
      }

      inline size_t TDurableManager::TMergeSortedByIdFile::GetNumDurable() const {
        return NumDurable;
      }

      inline TDurableManager::TMapping::TMapping(TDurableManager *manager)
          : ManagerMembership(this, &manager->MappingCollection),
          EntryCollection(this),
          RefCount(0U),
          MarkedForDelete(false) {}

      inline void TDurableManager::TMapping::Incr() {
        __sync_add_and_fetch(&RefCount, 1U);
      }

      inline void TDurableManager::TMapping::Decr() {
        size_t count = __sync_sub_and_fetch(&RefCount, 1U);
        if (this != ManagerMembership.TryGetCollection()->TryGetLastMember() && count == 0) {
          delete this;
        }
      }

      inline size_t TDurableManager::TMapping::GetRefCount() const {
        return RefCount;
      }

      inline TDurableManager::TMapping::TEntryCollection *TDurableManager::TMapping::GetEntryCollection() const {
        return &EntryCollection;
      }

      inline TDurableManager::TMapping::TEntry::TEntry(TMapping *mapping, TDurableLayer *layer)
          : MappingMembership(this, &mapping->EntryCollection),
            Layer(layer) {
        Layer->Incr();
      }

      inline TDurableManager::TMapping::TEntry::~TEntry() {
        assert(Layer);
        Layer->Decr();
      }

      inline TDurableManager::TDurableLayer *TDurableManager::TMapping::TEntry::GetLayer() const {
        return Layer;
      }

      inline TDurableManager::TDurableLayer::TDurableLayer(TDurableManager *manager)
        : Manager(manager),
          RemovalMembership(this),
          RefCount(0U),
          MarkedForDelete(false),
          MarkedTaken(false) {}

      inline TDurableManager::TDurableLayer::~TDurableLayer() {}

      inline void TDurableManager::TDurableLayer::Incr() {
        __sync_add_and_fetch(&RefCount, 1U);
      }

      inline void TDurableManager::TDurableLayer::Decr() {
        size_t count = __sync_sub_and_fetch(&RefCount, 1U);
        if (MarkedForDelete && count == 0) {
          std::lock_guard<std::mutex> removal_lock(Manager->RemovalLock);
          RemovalMembership.Insert(&Manager->RemovalCollection);
        }
      }

      inline void TDurableManager::TDurableLayer::RemoveFromCollection() {
        RemovalMembership.Remove();
      }

      inline bool TDurableManager::TDurableLayer::GetMarkedForDelete() const {
        return MarkedForDelete;
      }

      inline void TDurableManager::TDurableLayer::MarkForDelete() {
        MarkedForDelete = true;
      }

      inline bool TDurableManager::TDurableLayer::GetMarkedTaken() const {
        return MarkedTaken;
      }

      inline void TDurableManager::TDurableLayer::MarkTaken() {
        MarkedTaken = true;
      }

      inline TDurableManager::TMemSlushLayer::TMemSlushLayer(TDurableManager *manager)
          : TDurableLayer(manager), EntryCollection(this), NumEntries(0UL), TotalSerializedSize(0UL) {}

      inline TDurableManager::TMemSlushLayer::~TMemSlushLayer() {
        EntryCollection.DeleteEachMember();
      }

      inline size_t TDurableManager::TMemSlushLayer::GetNumEntries() const {
        return NumEntries;
      }

      inline size_t TDurableManager::TMemSlushLayer::GetTotalSerializedSize() const {
        return TotalSerializedSize;
      }

      inline TDurableManager::TMemSlushLayer::TEntryCollection *TDurableManager::TMemSlushLayer::GetEntryCollection() const {
        return &EntryCollection;
      }

      inline TDurableManager::TMemSlushLayer::TDurableEntry::TKey::TKey(const uuid_t &id, TSequenceNumber seq_num)
          : SeqNum(seq_num) {
        uuid_copy(Id, id);
      }

      inline bool TDurableManager::TMemSlushLayer::TDurableEntry::TKey::operator==(const TKey &that) const {
        return SeqNum == that.SeqNum && uuid_compare(Id, that.Id) == 0;
      }

      inline bool TDurableManager::TMemSlushLayer::TDurableEntry::TKey::operator!=(const TKey &that) const {
        return SeqNum != that.SeqNum || uuid_compare(Id, that.Id) != 0;
      }

      inline bool TDurableManager::TMemSlushLayer::TDurableEntry::TKey::operator<=(const TKey &that) const {
        int uuid_comp = uuid_compare(Id, that.Id);
        return uuid_comp < 0 || (uuid_comp == 0 && SeqNum > that.SeqNum);
      }

      inline TDurableManager::TMemSlushLayer::TDurableEntry::TDurableEntry(TMemSlushLayer *mem_layer,
                                                                           const Base::TUuid &id,
                                                                           const Durable::TDeadline &deadline,
                                                                           std::string &&serialized_form,
                                                                           TSequenceNumber seq_num,
                                                                           Durable::TSem *sem)
          : SlushMembership(this, TKey(id.GetRaw(), seq_num)),
            DeadlineCount(deadline.time_since_epoch().count()),
            Sem(sem) {
        std::swap(SerializedForm, serialized_form);
        ++(mem_layer->NumEntries);
        mem_layer->TotalSerializedSize += SerializedForm.size();
        SlushMembership.Insert(mem_layer->GetEntryCollection());
      }

      inline const uuid_t &TDurableManager::TMemSlushLayer::TDurableEntry::GetId() const {
        return SlushMembership.GetKey().Id;
      }

      inline TSequenceNumber TDurableManager::TMemSlushLayer::TDurableEntry::GetSeqNum() const {
        return SlushMembership.GetKey().SeqNum;
      }

      inline size_t TDurableManager::TMemSlushLayer::TDurableEntry::GetDeadlineCount() const {
        return DeadlineCount;
      }

      inline TDurableManager::TSerializedSize TDurableManager::TMemSlushLayer::TDurableEntry::GetSerializedSize() const {
        return SerializedForm.size();
      }

      inline const std::string &TDurableManager::TMemSlushLayer::TDurableEntry::GetSerializedForm() const {
        return SerializedForm;
      }

      inline TDurableManager::TDiskOrderedLayer::TDiskOrderedLayer(TDurableManager *manager, Util::TEngine *engine, size_t gen_id, size_t num_durable)
          : TDurableLayer(manager), Engine(engine), GenId(gen_id), NumDurable(num_durable) {}

      inline void TDurableManager::TDiskOrderedLayer::FindMax(TSequenceNumber &cur_max_seq, const Base::TUuid &id, std::string &serialized_form_out) const {
        TSortedInFile(Engine, RealTime, GenId).FindInHash(cur_max_seq, id, serialized_form_out);
      }

      inline TDurableManager::TMapping *TDurableManager::TMapping::TView::GetMapping() const {
        return Mapping;
      }

      inline TDurableManager::TMemSlushLayer *TDurableManager::TMapping::TView::GetCurLayer() const {
        return CurMemLayer;
      }

      inline size_t TDurableManager::TDiskOrderedLayer::GetGenId() const {
        return GenId;
      }

      inline size_t TDurableManager::TDiskOrderedLayer::GetNumDurable() const {
        return NumDurable;
      }

    }  // Disk

  }  // Indy

}  // Orly
