/* <orly/indy/manager_base.h>

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

#include <atomic>
#include <cassert>
#include <chrono>
#include <optional>

#include <base/class_traits.h>
#include <base/cpu_clock.h>
#include <base/event_semaphore.h>
#include <base/sigma_calc.h>
#include <base/spin_lock.h>
#include <base/timer_fd.h>
#include <base/uuid.h>
#include <base/inv_con/unordered_list.h>
#include <base/inv_con/unordered_multimap.h>
#include <orly/indy/disk/util/engine.h>
#include <orly/indy/disk/utilization_reporter.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/present_walker.h>
#include <orly/indy/status.h>
#include <orly/indy/update.h>
#include <orly/indy/update_walker.h>
#include <orly/indy/util/lockless_pool.h>
#include <orly/server/tetris_manager.h>
#include <orly/time.h>

namespace Orly {

  namespace Server {
    /* Forward Declarations. */
    class TIndyReporter;
    class TServer;
  }

  namespace Indy {

    /* Forward Declarations. */
    class TMemoryLayer;
    class TDiskLayer;

    /* Forward Declarations. */
    class TManager;

    namespace L0 {

      /* We use standard UUIDs as our unique ids. */
      using TId = Base::TUuid;

      /* We characterize time-to-live in seconds. */
      using TTtl = std::chrono::seconds;

      /* We characterize a deadline as a point in time on the system clock. */
      using TDeadline = std::chrono::time_point<std::chrono::system_clock>;

      /* We use instances of this type when waiting for the disk to act. */
      using TSem = Base::TEventSemaphore;

      /* Used to disambiguate the private constructors of TPtr<>. */
      enum TNew { New };
      enum TOld { Old };

      /* The errors we throw. */
      DEFINE_ERROR(TAlreadyExists, std::runtime_error, "durable object already exists");
      DEFINE_ERROR(TDoesntExist, std::runtime_error, "durable object doesn't exist");
      DEFINE_ERROR(TWrongType, std::runtime_error, "durable object of wrong type");

      class TManager
          : public Fiber::TRunnable {
        NO_COPY(TManager);
        public:

        /* TAnyPtr and TPtr<> require this forward declaration. */
        class TObj;

        /* The base class for all pointers to durable objects. */
        class TAnyPtr {
          public:

          /* Do-little. */
          virtual ~TAnyPtr() {}

          protected:

          /* Do-little. */
          TAnyPtr() {}

          /* Force the pointer to give up its object WITHOUT decrementing the object's pointer count.
             Return the object at which the pointer was pointing, if any. */
          virtual TObj *ForceRelease() = 0;

          /* For ForceRelease(). */
          friend class TObj;

        };  // TAnyPtr

        /* A shared pointer to a durable object. */
        template <typename TSomeObj>
        class TPtr final
            : public TAnyPtr {
          public:

          /* Default-construct as a null pointer. */
          TPtr();

          /* Move-construct from a donor pointer of our own type, leaving the donor null.
             If the donor is already null, both pointers will end up null. */
          TPtr(TPtr &&that);

          /* Copy-construct from a pointer of our own type.
             The durable will be shared between the pointers.
             If the given pointer is null, so will the new pointer be. */
          TPtr(const TPtr &that);

          /* Copy-construct from a pointer of a different, but up-castable, type.
             The durable will be shared between the pointers.
             If the given pointer is null, so will the new pointer be. */
          template <typename TOtherObj>
          TPtr(const TPtr<TOtherObj> &that);

          /* Release our hold on the durable.
             If we're the last pointer, the durable will close. */
          ~TPtr();

          /* Release our hold on our existing durable, if any, and move-assign from a donor pointer, leaving the donor null.
             If the donor is already null, both pointers will end up null. */
          TPtr &operator=(TPtr &&that);

          /* Release our hold on our existing durable, if any, and assign from a pointer of our own type.
             The given pointer's durable will be shared between the pointers.
             If the given pointer is null, so will we be. */
          TPtr &operator=(const TPtr &that);

          /* True iff. this pointer and that one point to the same object. */
          bool operator==(const TPtr &that) const;

          /* True iff. this pointer and that one point to different objects. */
          bool operator!=(const TPtr &that) const;

          /* The address of the object at which we point, which is guaranteed to be a unique hash. */
          size_t GetHash() const;

          /* Release our hold on our existing durable, if any, and assign from a pointer of a different, but up-castable, type.
             The given pointer's durable will be shared between the pointers.
             If the given pointer is null, so will we be. */
          template <typename TOtherObj>
          TPtr &operator=(const TPtr<TOtherObj> &that);

          /* True iff. we're non-null. */
          operator bool() const;

          /* The durable to which we we point, if any. */
          TSomeObj &operator*() const;

          /* The durable to which we we point, if any. */
          TSomeObj *operator->() const;

          /* The durable to which we we point, if any. */
          TSomeObj *Get() const;

          /* Release our hold on our existing durable, if any, and reset to the default-constructed state; that is, become null. */
          TPtr &Reset();

          private:

          /* Adopt the given native pointer, which must not be null.
             This constructor is used (and should only be used) by TManager::New() or TManager::TOpen() to return newly created objects. */
          TPtr(TSomeObj *some_obj, TNew) noexcept;

          /* Adopt the given native pointer, which must not be null.
             Dynamic-cast the object to our type and, if the object is of the wrong type, throw.
             This constructor is used (and should only be used) by TManager::Open() to return objects already in use. */
          TPtr(TObj *obj, TOld);

          /* See base class. */
          virtual TObj *ForceRelease() override {
            auto *temp = SomeObj;
            SomeObj = nullptr;
            return temp;
          }

          /* The durable object we refer to (and keep alive), if any. */
          TSomeObj *SomeObj;

          /* For the private constructors. */
          friend class TManager;
          friend class TRepo;

        };  // TPtr<TSomeObj>

        /* The base class for durable objects. */
        class TObj {
          NO_COPY(TObj);
          public:

          /* If the object is closed, then this is the latest time at which the object is guaranteed to continue to exist.
             Once the deadline expires, the cleaner is free to destroy this object.
             If the object is open, this is unknown.
             An object which is open cannot be destroyed by the cleaner, so it has no deadline. */
          const std::optional<TDeadline> &GetDeadline() const {
            return Deadline;
          }

          /* The unique id of this durable object. */
          const TId &GetId() const {
            return Id;
          }

          /* Overide this to provide a human-readable c-string describing what kind of durable object this is.
             Do not return null. */
          virtual const char *GetKind() const noexcept;

          /* The manager to which this durable object belongs. */
          TManager *GetManager() const {
            return Manager;
          }

          /* The minimum amount of time this object will live after it is closed.
             If this value is zero, then this object will be destroyed immediately after it is closed. */
          const TTtl &GetTtl() const {
            return Ttl;
          }

          /* True iff. this object has ever been saved to disk; otherwise, false. */
          bool IsOnDisk() const {
            return OnDisk;
          }

          /* Make an entry in the syslog regarding this object. */
          void Log(int level, const char *action, const std::exception &ex) const noexcept;

          /* Make an entry in the syslog regarding this object. */
          void Log(int level, const char *action, const char *msg) const noexcept;

          /* Change the time-to-live.  The time must be non-negatvie and the object must be open. */
          void SetTtl(const TTtl &ttl);

          #if 0
          /* Override this function to stream private object state to serialized blob form.
             Call this version first, then stream your own data.
             The blob formed here must be readable by the stream-based constructor of your final type. */
          virtual void Write(Io::TBinaryOutputStream &strm) const;
          #endif

          protected:

          /* Caches the construction parameters.  The pointer to the manager must not be null.
             In your final type, provide your own constructor which takes these paramters and passes them to this constructor. */
          TObj(TManager *manager, const TId &id, const TTtl &ttl);

          #if 0
          /* Caches the manager and id and streams in the rest of the state.
             In your final type, provide your own constructor which takes these paramters and passes them to this constructor. */
          TObj(TManager *manager, const TId &id, Io::TBinaryInputStream &strm);
          #endif

          /* Do-little.  Keep the destructor of your final type private. */
          virtual ~TObj();

          /* Override to call back for each durable object at which we point.
             NOTE: Do NOT invoke any shared pointer copy-constructors or you will deadlock.
             The pointer you call back with may be altered.  In particular, you should expect it to be set null.
             The default implementation of this function returns no dependent objects. */
          virtual bool ForEachDependentPtr(const std::function<bool (TAnyPtr &)> &cb) noexcept;

          private:

          /* Increment the count of pointers currently sharing this durable object.
             This operation is syncrhonized between threads. */
          void OnPtrAcquire() noexcept;

          /* Decrement the count of pointers currently sharing this durable object.
             If this causes the count to reach zero, close the durable object.
             If the close requires an asynchronous disk operation, wait for it to complete.
             This operation is syncrhonized between threads; however, the lock will be released before waiting for the async operation, if any.
             NOTE: By the time this function returns, 'this' may be a bad pointer. */
          void OnPtrRelease() noexcept;

          /* Transition the pointer count from zero to one.
             This is done when the object is adopted by its first pointer.
             NOTE: This function assumes that the manager's mutex has already been obtained. */
          void OnPtrAdoptNew() noexcept;

          /* Increment the count of pointers currently sharing this durable object.
             This is done when the object is adopted by a pointer which is not the object's first.
             NOTE: This function assumes that the manager's mutex has already been obtained. */
          void OnPtrAdoptOld() noexcept;

          /* See accessor. */
          TManager *Manager;

          /* See accessor. */
          TId Id;

          /* The number of pointers currently sharing this durable object.
             If this count is greater than zero, it means the durable object is open.
             If this count is zero, it means the durable object is closed but being held in cache. */
          size_t PtrCount;

          /* See accessor. */
          bool OnDisk;

          /* See accessor. */
          TTtl Ttl;

          /* See accessor. */
          std::optional<TDeadline> Deadline;

          /* A semaphore to use during destruction, if we trigger asynchonous events. */
          TSem *Sem;

          /* For on-ptr-event functions. */
          template <typename>
          friend class TPtr;

          /* For ~TObj() & Deadline. */
          friend class TManager;

        };  // TObj

        class TRepo
            : public TManager::TObj {
          NO_COPY(TRepo);
          public:

          typedef std::optional<TManager::TPtr<L0::TManager::TRepo>> TParentRepo;

          /* Forward Declarations. */
          class TDataLayer;

          typedef InvCon::OrderedList::TMembership<TRepo, TManager, std::chrono::steady_clock::time_point> TQueueMembership;

          virtual ~TRepo();

          const Base::TUuid &GetId() const;

          inline TStatus GetStatus() const;

          virtual bool IsSafeRepo() const = 0;

          inline bool IsTailingAllowed() const;

          protected:

          /* Forward Declarations. */
          class TMapping;

          typedef InvCon::UnorderedList::TCollection<TRepo, TMapping> TMappingCollection;

          class TMapping {
            NO_COPY(TMapping);
            public:

            /* Forward Declarations. */
            class TEntry;

            typedef InvCon::UnorderedList::TMembership<TMapping, TRepo> TRepoMembership;

            typedef InvCon::OrderedList::TCollection<TMapping, TEntry, TSequenceNumber> TEntryCollection;

            class TEntry {
              NO_COPY(TEntry);
              public:

              typedef InvCon::OrderedList::TMembership<TEntry, TMapping, TSequenceNumber> TMappingMembership;

              TEntry(TMapping *mapping, TDataLayer *layer);

              ~TEntry();

              TDataLayer *GetLayer() const;

              TEntry *TryGetNextMember() const;

              TEntry *TryGetPrevMember() const;

              static void *operator new(size_t size) {
                return Pool.Alloc(size);
              }

              static void operator delete(void *ptr, size_t) {
                Pool.Free(ptr);
              }

              private:

              TMappingMembership::TImpl MappingMembership;

              TDataLayer *Layer;

              static Util::TPool Pool;

              friend class TManager;
              friend class Server::TIndyReporter;

            };  // TEntry

            TMapping(TRepo *repo);

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

            TRepoMembership::TImpl RepoMembership;

            mutable TEntryCollection::TImpl EntryCollection;

            size_t RefCount;

            bool MarkedForDelete;

            static Util::TPool Pool;

            friend class TManager;
            friend class Server::TIndyReporter;

          };  // TMapping

          public:

          class TDataLayer {
            NO_COPY(TDataLayer);
            public:

            enum TKind {
              Mem,
              Disk
            };

            typedef InvCon::UnorderedList::TMembership<TDataLayer, TManager> TRemovalMembership;

            virtual ~TDataLayer();

            inline void RemoveFromCollection();

            void Incr();

            void Decr();

            virtual std::unique_ptr<TPresentWalker> NewPresentWalker(const TIndexKey &from,
                                                                     const TIndexKey &to) const = 0;

            /* exact_point: the key is a fully-bound point read, so a layer may
               seek via a point-read accelerator instead of a head scan (#257).
               Defaults false so prefix/cursor callers keep the scan. */
            virtual std::unique_ptr<TPresentWalker> NewPresentWalker(const TIndexKey &key, bool exact_point = false) const = 0;

            virtual std::unique_ptr<TUpdateWalker> NewUpdateWalker(TSequenceNumber from) const = 0;

            virtual TKind GetKind() const = 0;

            inline bool GetMarkedTaken() const;

            inline void MarkTaken();

            inline void UnmarkTaken();

            inline bool GetMarkedForDelete() const;

            inline void MarkForDelete();

            virtual size_t GetSize() const = 0;

            virtual TSequenceNumber GetLowestSeq() const = 0;

            virtual TSequenceNumber GetHighestSeq() const = 0;

            static void *operator new(size_t size) {
              return Pool.Alloc(size);
            }

            static void operator delete(void *ptr, size_t) {
              Pool.Free(ptr);
            }

            protected:

            TDataLayer(TManager *manager);

            private:

            TManager *Manager;

            TRemovalMembership::TImpl RemovalMembership;

            size_t RefCount;

            bool MarkedForDelete;

            bool MarkedTaken;

            static Util::TPool Pool;

            friend class TManager;
            friend class Server::TIndyReporter;

          };  // TDataLayer

          virtual const TParentRepo &GetParentRepo() const = 0;

          protected:

          TRepo(TManager *manager, const Base::TUuid &repo_id, const TTtl &ttl, TStatus status);

          void PreDtor();

          virtual size_t MergeFiles(const std::vector<size_t> &gen_id_vec,
                                    Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                                    size_t max_block_cache_read_slots_allowed,
                                    size_t temp_file_consol_thresh,
                                    TSequenceNumber &out_saved_low_seq,
                                    TSequenceNumber &out_saved_high_seq,
                                    size_t &out_num_keys,
                                    TSequenceNumber release_up_to,
                                    bool can_tail,
                                    bool can_tail_tombstone);

          virtual void RemoveFile(size_t gen_id);

          virtual size_t WriteFile(TMemoryLayer *memory_layer,
                                   Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                                   TSequenceNumber &out_saved_low_seq,
                                   TSequenceNumber &out_saved_high_seq,
                                   size_t &out_num_keys,
                                   TSequenceNumber release_up_to);

          virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalkerFile(size_t gen_id,
                                                                             const TIndexKey &from,
                                                                             const TIndexKey &to) const;

          virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalkerFile(size_t gen_id,
                                                                             const TIndexKey &key) const;

          virtual std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalkerFile(size_t gen_id, TSequenceNumber from) const;

          inline void EnqueueMergeMem();

          inline void EnqueueMergeDisk();

          virtual void StepMergeMem() = 0;

          virtual void StepMergeDisk(size_t block_slots_available) = 0;

          virtual void StepTail(size_t block_slots_available) = 0;

          void MakeDirty();

          void RemoveFromDirty();

          inline void RemoveFromClosedBuffer();

          mutable TMappingCollection::TImpl MappingCollection;

          std::mutex MappingLock;

          TManager *Manager;

          TStatus Status;

          private:

          inline const std::chrono::steady_clock::time_point &GetTimeOfNextMergeMem() const;
          inline const std::chrono::steady_clock::time_point &GetTimeOfNextMergeDisk() const;

          inline void SetTimeOfNextMergeMem(const std::chrono::steady_clock::time_point &time);
          inline void SetTimeOfNextMergeDisk(const std::chrono::steady_clock::time_point &time);

          TPtr<TRepo> DirtyPtr;

          Base::TUuid Id;

          TQueueMembership::TImpl MergeMemMembership;
          TQueueMembership::TImpl MergeDiskMembership;

          friend class TManager;
          friend class Orly::Indy::TManager;
          friend class Orly::Indy::TMemoryLayer;
          friend class Orly::Indy::TDiskLayer;
          friend class Server::TIndyReporter;
          friend class Server::TServer;

        };  // TRepo

        typedef InvCon::UnorderedList::TCollection<TManager, TRepo::TDataLayer> TRemovalCollection;

        inline Disk::Util::TEngine *GetEngine() const;

        inline size_t GetTempFileConsolThresh() const;

        void RunLayerCleaner();

        /* Make RunLayerCleaner return: raise the stop flag and fire its
           timer so the blocking Pop() wakes.  Without this the cleaner
           fiber sits in a read on the timer fd forever, pinning its runner
           thread and (at shutdown) racing the fd's destruction (#440). */
        void StopLayerCleaner();

        /* Block until a stopped cleaner has actually returned (if it ever
           started): a stop is only a flag plus a wake, and the manager must
           not be destroyed while the cleaner is still inside its loop
           (#440). */
        void JoinLayerCleaner();

        /* Stop the RunMergeMem/RunMergeDisk loops and wait for every one
           that started to actually return (#440).  Must run on a fiber:
           the wake sems are fiber primitives.  Call before FlushMemMerges
           so the flush is the only drainer of the merge queues. */
        void StopMergeRunners();

        /* Merge every dirty repo's memory layer out to a disk file -- the
           flush half of an orderly shutdown (#440).  Call from a fiber,
           after StopMergeRunners(). */
        void FlushMemMerges();

        void RunMergeMem();

        void RunMergeDisk();

        void GetFileGenSet(const Base::TUuid &repo_id, std::vector<Disk::TFileObj> &file_vec);

        Server::TTetrisManager *GetTetrisManager() const;

        void SetTetrisManager(Server::TTetrisManager *tetris_manager);

        void ReportMergeCPUTime(std::chrono::nanoseconds &out_merge_mem, std::chrono::nanoseconds &out_merge_disk);

        virtual void ForEachScheduler(const std::function<bool (Fiber::TRunner *)> &cb) const = 0;

        static void InitMappingPool(size_t num_obj) {
          TRepo::TMapping::Pool.Init(num_obj);
        }

        static constexpr size_t GetMappingSize() {
          return sizeof(TRepo::TMapping);
        }

        static void InitMappingEntryPool(size_t num_obj) {
          TRepo::TMapping::TEntry::Pool.Init(num_obj);
        }

        static constexpr size_t GetMappingEntrySize() {
          return sizeof(TRepo::TMapping::TEntry);
        }

        static void InitDataLayerPool(size_t num_obj) {
          TRepo::TDataLayer::Pool.Init(num_obj);
        }

        static constexpr size_t GetDataLayerSize() {
          return sizeof(TRepo::TDataLayer);
        }

        Base::TSigmaCalc MergeMemAverageKeysCalc;
        std::mutex MergeMemCPULock;

        Base::TSigmaCalc MergeDiskAverageKeysCalc;
        std::mutex MergeDiskCPULock;

        protected:

        template <typename... TArgs>
        TPtr<TRepo> New(const TId &id, const TTtl &ttl, TArgs &&... args);

        template <typename TSomeObj>
        TPtr<TSomeObj> Open(const TId &id);

        TManager::TPtr<TRepo> OpenOrCreate(const TId &id,
                                           const std::optional<TTtl> &ttl,
                                           const std::optional<TPtr<TRepo>> &parent_repo,
                                           bool is_safe);

        std::mutex DurableMutex;
        std::condition_variable DurableCond;

        /* All the objects currently open as well as those which are closed but cached.
           These are objects which can be found by the Open() function.
           If a closed object (that is, one with a PtrCount of zero) is in this container, it will also be in ClosedObjs. */
        std::unordered_map<TId, TObj *> OpenableObjs;

        /* All the objects currently closed but still cached.
           They are in order by their deadlines, soonest deadlines first.
           The size of this container will never exceed MaxCacheSize.
           All of the objects in this container have a PtrCount of zero and also appear in OpenableObjs. */
        std::map<std::pair<TDeadline, TId>, TObj *> ClosedObjs;

        /* Search the disk for an object with the given id.
           If found, return true; else, return false.
           Assume that the mutex has already been obtained. */
        virtual bool CanLoad(const TId &id) = 0;


        /* Override to erase the given object from disk.
           If the object does not exist, there is a logic error in the program or a disk failure.
           Assume that the mutex has already been obtained.
           When the task is complete, push the semaphore.  Do not delete the semaphore. */
        virtual void Delete(const TId &id, TSem *sem) = 0;

        /* Override to save the given object to disk.
           Assume that the mutex has already been obtained.
           When the task is complete, push the semaphore.  Do not delete the semaphore. */
        virtual void Save(const TId &id, const TDeadline &deadline, const std::string &blob, TSem *sem) = 0;

        /* Override to search the disk for an object with the given id.
           If found, return the object's blob (via out-parameter) and return true; else, ignore the out-parameter and return false.
           Assume that the mutex has already been obtained. */
        virtual bool TryLoad(const TId &id, std::string &blob) = 0;

        private:

        /* Evict the given object from the set of openable objects, then destroy the object.
           NOTE 1: The object pointer passed to this function WILL BE BAD by the time this function returns.
           NOTE 2: This function assumes that the mutex has already been obtained. */
        void DestroyObj(TObj *obj) noexcept;

        /* If we're caching, insert the given object into the set of closed objects.
           Discard enough old objects to make room.
           Return true iff. the object is successfully cached.
           This function assumes that the mutex has already been obtained. */
        bool TryCacheObj(TObj *obj) noexcept;



        protected:

        typedef InvCon::OrderedList::TCollection<TManager, TRepo, std::chrono::steady_clock::time_point> TRepoQueue;

        TManager(Disk::Util::TEngine *engine,
                 std::chrono::milliseconds merge_mem_delay,
                 std::chrono::milliseconds merge_disk_delay,
                 bool allow_tailing,
                 bool no_realtime,
                 std::chrono::milliseconds layer_cleaning_interval,
                 Base::TScheduler *scheduler,
                 size_t block_slots_available_per_merger,
                 size_t max_repo_cache_size,
                 size_t temp_file_consol_thresh,
                 const std::vector<size_t> &merge_mem_cores,
                 const std::vector<size_t> &merge_disk_cores,
                 bool create_new);

        virtual ~TManager();

        /* Teardown step (#440): drop every repo's MakeDirty() self-pin.  A
           dirty repo pins itself open until its updates are released or its
           mem layer merges out, but a paused-and-written repo (compile-time
           test povs, paused players) is never promoted, so its pin would
           never drop and PreDtor would see a live ptr on every one.  By the
           time this runs everything durable has been flushed
           (FlushMemMerges) and a fast repo's data is volatile by design, so
           releasing the pins loses nothing.  Call before
           CloseAllUnreferencedObjects(). */
        void ReleaseDirtySelfPins();

        void CloseAllUnreferencedObjects();

        bool PreDtor();

        inline TManager::TPtr<TRepo> ForceOpenRepo(const Base::TUuid &repo_id);

        /* The repo with the given id iff it is LIVE in this manager (open,
           or closed but still cached); a null ptr otherwise.  Never
           constructs and never touches the (unported, #173) reload path:
           this is how a reloaded pov attaches to a repo that survived in
           memory without minting an empty one (#439). */
        TManager::TPtr<TRepo> TryOpenLiveRepo(const Base::TUuid &repo_id);

        virtual TRepo *ConstructRepo(const Base::TUuid &repo_id,
                                     const std::optional<TTtl> &ttl,
                                     const std::optional<TManager::TPtr<TRepo>> &parent_repo,
                                     bool is_safe,
                                     bool create) = 0;

        virtual TRepo *ReconstructRepo(const Base::TUuid &repo_id) = 0;

        virtual void SaveRepo(TRepo *repo) = 0;

        virtual void RunReplicationQueue() = 0;

        virtual void RunReplicationWork() = 0;

        virtual void RunReplicateTransaction() = 0;

        Base::TScheduler *Scheduler;

        private:

        void OnClose(TRepo *repo);

        void EnqueueMergeMem(TRepo *repo);

        void EnqueueMergeDisk(TRepo *repo);

        void RemoveLayersFromQueue();

        std::atomic<bool> ShuttingDown;

        bool AllowTailing;

        mutable TRemovalCollection::TImpl RemovalCollection;
        std::mutex RemovalLock;
        Base::TTimerFd LayerCleanerTimer;
        std::atomic<bool> LayerCleanerStopping;
        /* RunLayerCleaner() marks Started on entry and pushes Exited when it
           returns; JoinLayerCleaner() waits on that handshake (#440). */
        std::atomic<bool> LayerCleanerStarted;
        Base::TEventSemaphore LayerCleanerExited;

        mutable TRepoQueue::TImpl MergeMemQueue;
        std::mutex MergeMemLock;
        std::mutex MergeMemEpollLock;
        Fiber::TSingleSem MergeMemSem;
        /* Each merge loop counts itself in on entry and pushes Exited on the
           way out; StopMergeRunners() reaps exactly that many exits (#440). */
        std::atomic<size_t> MergeMemLoopsStarted;
        Base::TEventSemaphore MergeMemExited;

        mutable TRepoQueue::TImpl MergeDiskQueue;
        Fiber::TFiberLock MergeDiskLock;
        Fiber::TFiberLock MergeDiskEpollLock;
        Fiber::TSingleSem MergeDiskSem;
        std::atomic<size_t> MergeDiskLoopsStarted;
        Base::TEventSemaphore MergeDiskExited;

        std::chrono::milliseconds MergeMemDelay;
        std::chrono::milliseconds MergeDiskDelay;

        size_t BlockSlotsAvailablePerMerger;

        size_t MaxCacheSize;

        Disk::Util::TEngine *Engine;

        size_t TempFileConsolThresh;

        const std::vector<size_t> &MergeMemCores;

        const std::vector<size_t> &MergeDiskCores;

        Server::TTetrisManager *TetrisManager;

        std::function<void (TRepo *)> OnCloseCb;

        /* Merge CPU Timer Information */
        std::unordered_map<pthread_t, Base::thread_clock::time_point> MergeMemThreadCPUMap;
        std::unordered_map<pthread_t, Base::thread_clock::time_point> MergeDiskThreadCPUMap;
        std::mutex MergeThreadCPUMutex;

        friend class Orly::Server::TServer;

      };  // TManager

      inline const Base::TUuid &TManager::TRepo::GetId() const {
        return Id;
      }

      inline TStatus TManager::TRepo::GetStatus() const {
        return Status;
      }

      inline bool TManager::TRepo::IsTailingAllowed() const {
        return Manager->AllowTailing;
      }

      inline void TManager::TRepo::TMapping::Incr() {
        assert(!(this != RepoMembership.TryGetCollection()->TryGetLastMember() && RefCount == 0));
        __sync_add_and_fetch(&RefCount, 1U);
      }

      inline void TManager::TRepo::TMapping::Decr() {
        assert(RefCount > 0);
        size_t count = __sync_sub_and_fetch(&RefCount, 1U);
        if (this != RepoMembership.TryGetCollection()->TryGetLastMember() && count == 0) {
          delete this;
        }
      }

      inline size_t TManager::TRepo::TMapping::GetRefCount() const {
        return RefCount;
      }

      inline TManager::TRepo::TMapping::TEntryCollection *TManager::TRepo::TMapping::GetEntryCollection() const {
        return &EntryCollection;
      }

      inline void TManager::TRepo::TDataLayer::RemoveFromCollection() {
        assert(MarkedTaken);
        assert(RemovalMembership.TryGetCollection());
        RemovalMembership.Remove();
      }

      inline bool TManager::TRepo::TDataLayer::GetMarkedForDelete() const {
        return MarkedForDelete;
      }

      inline void TManager::TRepo::TDataLayer::MarkForDelete() {
        MarkedForDelete = true;
      }

      inline bool TManager::TRepo::TDataLayer::GetMarkedTaken() const {
        return MarkedTaken;
      }

      inline void TManager::TRepo::TDataLayer::MarkTaken() {
        MarkedTaken = true;
      }

      inline void TManager::TRepo::TDataLayer::UnmarkTaken() {
        MarkedTaken = false;
      }

      inline void TManager::TRepo::EnqueueMergeMem() {
        Manager->EnqueueMergeMem(this);
      }

      inline void TManager::TRepo::EnqueueMergeDisk() {
        Manager->EnqueueMergeDisk(this);
      }

      inline void TManager::TRepo::RemoveFromClosedBuffer() {
        /* Never called: the on-disk closed-object buffer is not wired up. Fail clearly if a
           path ever reaches it -- the indy L0 reload-from-disk path was never ported (#173). */
        throw std::logic_error("TManager::TRepo::RemoveFromClosedBuffer not implemented: the on-disk closed-object buffer is never wired up (the indy L0 reload-from-disk path was never ported, #173).");
      }

      inline const std::chrono::steady_clock::time_point &TManager::TRepo::GetTimeOfNextMergeMem() const {
        return MergeMemMembership.GetKey();
      }

      inline const std::chrono::steady_clock::time_point &TManager::TRepo::GetTimeOfNextMergeDisk() const {
        return MergeDiskMembership.GetKey();
      }

      inline void TManager::TRepo::SetTimeOfNextMergeMem(const std::chrono::steady_clock::time_point &time) {
        MergeMemMembership.SetKey(time);
      }

      inline void TManager::TRepo::SetTimeOfNextMergeDisk(const std::chrono::steady_clock::time_point &time) {
        MergeDiskMembership.SetKey(time);
      }

      inline Disk::Util::TEngine *TManager::GetEngine() const {
        return Engine;
      }

      inline size_t TManager::GetTempFileConsolThresh() const {
        return TempFileConsolThresh;
      }

      /*
       *  Definitions of TPtr<> members.
       */

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::TPtr()
          : SomeObj(nullptr) {}

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::TPtr(TPtr &&that) {
        SomeObj = that.SomeObj;
        that.SomeObj = nullptr;
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::TPtr(const TPtr &that) {
        if ((SomeObj = that.SomeObj) != nullptr) {
          SomeObj->OnPtrAcquire();
        }
      }

      template <typename TSomeObj>
      template <typename TOtherObj>
      TManager::TPtr<TSomeObj>::TPtr(const TPtr<TOtherObj> &that) {
        if ((SomeObj = reinterpret_cast<TSomeObj *>(that.SomeObj)) != nullptr) {
          SomeObj->OnPtrAcquire();
        }
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::~TPtr() {
        if (SomeObj) {
          SomeObj->OnPtrRelease();
        }
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj> &TManager::TPtr<TSomeObj>::operator=(TPtr &&that) {
        std::swap(SomeObj, that.SomeObj);
        return *this;
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj> &TManager::TPtr<TSomeObj>::operator=(const TPtr &that) {
        return *this = TPtr(that);
      }

      template <typename TSomeObj>
      bool TManager::TPtr<TSomeObj>::operator==(const TPtr &that) const {
        return SomeObj == that.SomeObj;
      }

      template <typename TSomeObj>
      bool TManager::TPtr<TSomeObj>::operator!=(const TPtr &that) const {
        return SomeObj != that.SomeObj;
      }

      template <typename TSomeObj>
      size_t TManager::TPtr<TSomeObj>::GetHash() const {
        return reinterpret_cast<size_t>(SomeObj);
      }

      template <typename TSomeObj>
      template <typename TOtherObj>
      TManager::TPtr<TSomeObj> &TManager::TPtr<TSomeObj>::operator=(const TPtr<TOtherObj> &that) {
        return *this = TPtr(that);
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::operator bool() const {
        return SomeObj != nullptr;
      }

      template <typename TSomeObj>
      TSomeObj &TManager::TPtr<TSomeObj>::operator*() const {
        return *SomeObj;
      }

      template <typename TSomeObj>
      TSomeObj *TManager::TPtr<TSomeObj>::operator->() const {
        return SomeObj;
      }

      template <typename TSomeObj>
      TSomeObj *TManager::TPtr<TSomeObj>::Get() const {
        return SomeObj;
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj> &TManager::TPtr<TSomeObj>::Reset() {
        return *this = TPtr();
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::TPtr(TSomeObj *some_obj, TNew) noexcept
          : SomeObj(some_obj) {
        assert(some_obj);
        some_obj->OnPtrAdoptNew();
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj>::TPtr(TObj *obj, TOld) {
        assert(obj);
        SomeObj = dynamic_cast<TSomeObj *>(obj);
        if (!SomeObj) {
          THROW_ERROR(TWrongType) << "expected \"" << typeid(TSomeObj).name() << "\", got \"" << typeid(obj).name() << '"';
        }
        obj->OnPtrAdoptOld();
      }

      /*
       *  Definitions of TManager templatized members.
       */

      template <typename... TArgs>
      TManager::TPtr<TManager::TRepo> TManager::New(const TId &id, const TTtl &ttl, TArgs &&... args) {
        std::pair<std::unordered_map<TId, TObj *>::iterator, bool> ret;
        /* Lock the manager and create the requested slot among the openable objects.
           If the slot already exists, throw. */ {
          std::lock_guard<std::mutex> lock(DurableMutex);
          ret = OpenableObjs.insert(std::pair<TId, TObj *>(id, nullptr));
          TObj *&openable_obj = ret.first->second;
          if (!ret.second || openable_obj) {
            THROW_ERROR(TAlreadyExists) << "in cache" << Base::EndOfPart << "id = " << id;
          }
        }
        try {
          /* Construct the new object and keep it in the openable set. */
          TRepo *repo = ConstructRepo(id, ttl, args..., true);
          std::lock_guard<std::mutex> lock(DurableMutex);
          TObj *&openable_obj = ret.first->second;
          assert(!openable_obj);
          openable_obj = repo;
          DurableCond.notify_all();
          return TPtr<TManager::TRepo>(repo, Orly::Indy::L0::New);
        } catch (...) {
          /* We already had the object on disk or the object's constructor failed.
             Either way, we need to dispose of the slot we made before continuing to handle the error. */
          std::lock_guard<std::mutex> lock(DurableMutex);
          OpenableObjs.erase(id);
          throw;
        }
      }

      template <typename TSomeObj>
      TManager::TPtr<TSomeObj> TManager::Open(const TId &id) {
        for (;;) {
          /* Lock the manager and find/create the requested slot among the openable objects. */
          std::unique_lock<std::mutex> lock(DurableMutex);
          auto ret = OpenableObjs.insert(std::pair<TId, TObj *>(id, nullptr));
          TObj *&openable_obj = ret.first->second;
          if (openable_obj) {
            /* We found an object with the given id. */
            TPtr<TSomeObj> ptr(openable_obj, Orly::Indy::L0::Old);
            const auto &deadline = openable_obj->GetDeadline();
            if (deadline) {
              /* The object is being re-opened from a closed state, so remove it from the set of closed objects. */
              size_t erased_from_closed = ClosedObjs.erase(std::make_pair(*deadline, id));
              assert(erased_from_closed == 1);
              openable_obj->Deadline.reset();
            }
            return ptr;
          } else if (ret.second) { /* freshly inserted */
            break;
          } else {
            DurableCond.wait(lock);
          }
        }
        try {
          /* Load the object from disk and keep it in the openable set. */
          #if 0
          std::string blob;
          if (!TryLoad(id, blob)) {
            THROW_ERROR(TDoesntExist) << "id = " << id;
          }
          Io::TBinaryInputOnlyStream strm(std::make_shared<Io::TPlayer>(std::make_shared<Io::TRecorder>(blob)));
          TSomeObj *some_obj = new TSomeObj(this, id, strm);
          openable_obj = some_obj;
          return TPtr<TSomeObj>(some_obj, Orly::Indy::L0::New);
          #endif
          throw std::logic_error("TManager::Open: loading a durable object from disk is not implemented (the indy L0 reload-from-disk path was never ported, #173).");
        } catch (...) {
          /* We could not find the object on disk or the object's constructor failed.
             Either way, we need to dispose of the slot we made before continuing to handle the error. */
          std::lock_guard<std::mutex> lock(DurableMutex);
          OpenableObjs.erase(id);
          throw;
        }
      }

      template <>
      TManager::TPtr<TManager::TRepo> TManager::Open(const TId &id);

      inline TManager::TPtr<TManager::TRepo> TManager::OpenOrCreate(const TId &id,
                                                                    const std::optional<TTtl> &ttl,
                                                                    const std::optional<TPtr<TRepo>> &parent_repo,
                                                                    bool is_safe) {
        std::pair<std::unordered_map<TId, TObj *>::iterator, bool> ret;
        for (;;) {
          /* Lock the manager and find/create the requested slot among the openable objects. */
          std::unique_lock<std::mutex> lock(DurableMutex);
          ret = OpenableObjs.insert(std::pair<TId, TObj *>(id, nullptr));
          TObj *&openable_obj = ret.first->second;
          if (openable_obj) {
            /* We found an object with the given id. */
            TPtr<TRepo> ptr(openable_obj, Orly::Indy::L0::Old);
            const auto &deadline = openable_obj->GetDeadline();
            if (deadline) {
              /* The object is being re-opened from a closed state, so remove it from the set of closed objects. */
              size_t erased_from_closed = ClosedObjs.erase(std::make_pair(*deadline, id));
              assert(erased_from_closed == 1);
              openable_obj->Deadline.reset();
            }
            return ptr;
          } else if (ret.second) { /* freshly inserted */
            break;
          } else {
            /* wait for it to get set */
            DurableCond.wait(lock);
          }
        }
        try {
          /* Load the object from disk and keep it in the openable set. */
          TRepo *repo = ConstructRepo(id, ttl, parent_repo, is_safe, true);
          std::lock_guard<std::mutex> lock(DurableMutex);
          TObj *&openable_obj = ret.first->second;
          assert(!openable_obj);
          openable_obj = repo;
          DurableCond.notify_all();
          return TPtr<TManager::TRepo>(repo, Orly::Indy::L0::New);
        } catch (...) {
          /* We could not find the object on disk or the object's constructor failed.
             Either way, we need to dispose of the slot we made before continuing to handle the error. */
          std::lock_guard<std::mutex> lock(DurableMutex);
          OpenableObjs.erase(id);
          throw;
        }
      }

      inline TManager::TPtr<TManager::TRepo> TManager::ForceOpenRepo(const Base::TUuid &repo_id) {
        return Open<TManager::TRepo>(repo_id);
      }

    }  // L0

  }  // Indy

}  // Orly
