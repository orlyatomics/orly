/* <orly/server/server.h>

   The Orly server.

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
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <base/class_traits.h>
#include <base/debug_log.h>
#include <base/event_semaphore.h>
#include <base/fd.h>
#include <base/log.h>
#include <base/scheduler.h>
#include <base/timer_fd.h>
#include <base/uuid.h>
#include <base/socket/address.h>
#include <orly/durable/kit.h>
#include <orly/indy/manager.h>
#include <orly/indy/disk/durable_manager.h>
#include <orly/indy/disk/indy_util_reporter.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/disk/util/disk_engine.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/fiber/jump_runnable.h>
#include <orly/notification/all.h>
#include <orly/notification/pov_failure.h>
#include <orly/notification/system_shutdown.h>
#include <orly/notification/update_progress.h>
#include <orly/package/manager.h>
#include <orly/server/repo_tetris_manager.h>
#include <orly/server/session.h>
#include <orly/server/ws.h>
#include <orly/type/type_czar.h>

namespace Orly {

  namespace Server {

    class TIndexType {
      public:

      TIndexType(std::string &&package_key, Indy::TKey &&val)
          : PackageKey(std::move(package_key)), Val(std::move(val)) {}

      inline size_t GetHash() const {
        return Base::ChainHashes(PackageKey, Val);
      }

      inline bool operator==(const TIndexType &that) const {
        return PackageKey == that.PackageKey && Val == that.Val;
      }

      const std::string &GetPackageKey() const {
        return PackageKey;
      }

      inline const Indy::TKey &GetVal() const {
        return Val;
      }

      private:

      std::string PackageKey;
      Indy::TKey Val;

    };  // TIndexType

  }  // Server

}  // Orly

namespace std {

  /* A standard hasher for Orly::Server::TIndexType. */
  template <>
  struct hash<Orly::Server::TIndexType> {
    typedef size_t result_type;
    typedef Orly::Server::TIndexType argument_type;
    size_t operator()(const argument_type &that) const {
      return that.GetHash();
    }
  };

}  // std

namespace Orly {

  namespace Server {

    /* Forward Declarations. */
    class TServer;

    class TIndyReporter {
      NO_COPY(TIndyReporter);
      public:

      /* Stops the accept loop (#440): flags it and wakes the blocked
         accept() with a socket shutdown. */
      void Stop();

      TIndyReporter(const TServer *server, Base::TScheduler *scheduler, int port_number);

      private:

      void AcceptClientConnections();

      void ServeClient(Base::TFd &fd);

      void AddReport(std::stringstream &ss) const;

      const TServer *Server;

      /* Set by Stop(); checked by AcceptClientConnections(). */
      std::atomic<bool> Stopping{false};

      Base::TFd Socket;

      Base::TScheduler *Scheduler;

      mutable Base::TTimer ReportTimer;

      typedef std::chrono::system_clock TClock;

      typedef std::chrono::time_point<TClock, std::chrono::nanoseconds> TTimePoint;

      static const size_t NanoToSecond = 1000000000UL;

    };  // TIndyReporter

    class TServer final
        : public TSession::TServer, public Indy::Fiber::TRunnable,
          public TWs::TSessionManager {
      NO_COPY(TServer);
      public:

      static constexpr size_t NumSlowRunners = 8UL;

      using TTimePoint = std::chrono::time_point<std::chrono::system_clock>;

      /* Command-line arguments. */
      class TCmd
          : public Base::TLog::TCmd {
        public:

        /* The port on which TServer::MainSocket listens for clients. */
        in_port_t PortNumber;

        /* The port on which the server listens for websocket clients. */
        in_port_t WsPortNumber;

        /* The port on which TServer::WaitForSlave listens for a slave. */
        in_port_t SlavePortNumber;

        /* The maximum number of connection requests to backlog against MainSocket. */
        int ConnectionBacklog;

        /* The maximum number of durable objects to keep cached in memory. */
        size_t DurableCacheSize;

        /* The maximum number of milliseconds a connection can remain idle before the server hangs up. */
        size_t IdleConnectionTimeout;

        /* The minimum number of milliseconds between rounds of housecleaning. */
        size_t HousecleaningInterval;

        /* The minimum number of milliseconds between rounds of layer cleaning. */
        size_t LayerCleaningInterval;

        /* Run in memory simulation mode. */
        bool MemorySim;

        /* The amount of memory in MB to allocate for a memory simulated volume. */
        size_t MemorySimMB;

        /* The amount of memory in MB to allocate for a memory simulated slow volume. */
        size_t MemorySimSlowMB;

        /* The number of files that can be in a single generation of temporary files before they get merged into the next generation. */
        size_t TempFileConsolidationThreshold;

        std::string InstanceName;

        size_t PageCacheSizeMB;

        size_t BlockCacheSizeMB;

        size_t FileServiceAppendLogMB;

        size_t DiskMaxAioNum;

        double HighDiskUtilizationThreshold;

        bool DiscardOnCreate;

        size_t ReplicationSyncBufMB;

        size_t MergeMemInterval;

        size_t MergeDiskInterval;

        size_t ReplicationInterval;

        size_t DurableWriteInterval;

        size_t DurableMergeInterval;

        std::string StartingState;

        size_t NumMemMergeThreads;

        size_t NumDiskMergeThreads;

        /* The number of threads to use for answering websocket requests. */
        size_t NumWsThreads;

        size_t MaxRepoCacheSize;

        std::vector<size_t> FastCoreVec;

        std::vector<size_t> SlowCoreVec;

        std::vector<size_t> DiskControllerCoreVec;

        std::vector<size_t> MemMergeCoreVec;

        std::vector<size_t> DiskMergeCoreVec;

        /* Fill the hardware-derived default core assignment into any of the
           *CoreVec members the user did NOT specify on the command line, leaving
           user-provided vectors untouched. Must be called AFTER Parse() so the
           flags override the defaults rather than appending to them (issue #240). */
        void ResolveCoreVecDefaults();

        size_t NumFiberFrames;

        size_t NumDiskEvents;

        /* The port on which we respond to HTTP by giving a status report. */
        in_port_t ReportingPortNumber;

        /* Support for tailing. */
        bool AllowTailing;

        /* We can use this to sync when the data layout has changed. */
        bool AllowFileSync;

        /* If true, realtime thread priorities will not be used (realtime
           priorities require root privileges). */
        bool NoRealtime;

        /* Controls whether fsync is used when writing to disk */
        bool DoFsync;

        bool LogAssertionFailures;

        /* If true, the global-POV merge promotes ALL ready commutative
           (assertion-free) children per round instead of one, collapsing the
           O(N^2) per-round re-snapshot/re-sort into O(N). The per-child-
           transaction promotion preserves the one-Pusher-per-repo invariant, and
           assertion-bearing children keep the one-per-round discipline. Default
           OFF (issue #234): a K=24 soak measured it throughput- and
           sustainability-neutral -- the binding concurrent-write constraint is
           the per-write acceptance path, not the merge tournament (see
           docs/design/concurrent-merge-throughput.md S8.7/S8.10). The flag and
           its #237 race fix + write backpressure remain available for the
           genuine deep-backlog regime. */
        bool TetrisCommutativeFastlane;

        /* High-watermark for write backpressure (#234). When a writer's POV
           child repo has more than this many un-promoted updates backed up in
           its memtable, the global merge is falling behind that writer; the
           accept path cooperatively yields its fiber until the merge drains the
           backlog below the watermark, so sustained accept paces to promote
           instead of growing memtables without bound (bad_alloc at high K). 0
           disables backpressure. */
        size_t TetrisBackpressureThreshold;

        /******** Object Pools ********/

        size_t DurableMappingPoolSize;
        size_t DurableMappingEntryPoolSize;
        size_t DurableLayerPoolSize;
        size_t DurableMemEntryPoolSize;

        size_t RepoMappingPoolSize;
        size_t RepoMappingEntryPoolSize;
        size_t RepoDataLayerPoolSize;

        size_t TransactionMutationPoolSize;
        size_t TransactionPoolSize;

        size_t UpdatePoolSize;
        size_t UpdateEntryPoolSize;

        size_t DiskBufferBlockPoolSize;

        /******** End Object Pools ********/

        Socket::TAddress AddressOfMaster;

        std::string PackageDirectory;

        bool Create;

        protected:

        /* Our meta-type. */
        class TMeta
            : public Base::TLog::TCmd::TMeta {
          protected:

          /* Registers our fields. */
          TMeta(const char *desc);

        };  // TServer::TCmd::TMeta

        /* Construct with defaults. */
        TCmd();

      };  // TServer::TCmd

      /* Launches background tasks using the given scheduler and takes its arguments from the cmd object. */
      TServer(Base::TScheduler *scheduler, const TCmd &cmd);

      /* Handles all the complicated aspects of TServer's constructor in a separate frame. */
      void Init();

      /* Clean up and go. */
      virtual ~TServer();

      /* See TSession::TServer. */
      virtual const std::shared_ptr<Durable::TManager> &GetDurableManager() const override {
        return DurableManager;
      }

      /* See TPov::TServer. */
      virtual const Indy::L0::TManager::TPtr<Indy::TRepo> &GetGlobalRepo() const override {
        return GlobalRepo;
      }

      /* See TSession::TServer. */
      virtual const Package::TManager &GetPackageManager() const override {
        return PackageManager;
      }

      /* See TPov::TServer. */
      virtual Orly::Indy::TManager *GetRepoManager() const override {
        return RepoManager.get();
      }

      virtual Base::TScheduler *GetScheduler() const override {
        assert(Scheduler);
        return Scheduler;
      }

      /* Write-backpressure high-watermark (#234); 0 disables. See TCmd. */
      size_t GetWriteBackpressureThreshold() const override {
        return Cmd.TetrisBackpressureThreshold;
      }

      /* Called when the websockets server wishes to create a new session. */
      virtual TWs::TSessionPin *NewSession() override;

      /* Called when the websockets server wishes to resume an old session. */
      virtual TWs::TSessionPin *ResumeSession(const Base::TUuid &id) override;

      virtual bool ForEachIndex(const std::function<
          bool(const std::string &pkg, const std::string &key_type, const std::string &val_type)> &cb) const final;

      /* Install the named package and run its compile-time test{} blocks on this
         (indy) engine, returning true iff every test passed. Used by orlyc to
         run tests on indy instead of the legacy SPA engine (#262). Installs,
         opens a throwaway session, and runs the suite on a fiber (durable/
         transaction work asserts it runs in a fiber context). */
      /* Orderly stop (#440): stops serving, tears down the fiber-entangled
         managers ON a fiber (their destructors block on fiber semaphores and
         must run while the runners are still alive), then stops and joins
         every thread this server owns.  After this the server must not be
         used; destruction itself remains deferred (docs/teardown-design.md).
         Idempotent.  Must be called from a non-fiber thread. */
      void Shutdown();

      bool RunPackageTests(const std::vector<std::string> &package_name, uint64_t version, bool verbose);

      private:
      /* A live connection to a client. */
      class TConnection final
          : public Rpc::TContext {
        NO_COPY(TConnection);
        public:

        /* The session associated with this connection.  Never null. */
        const Durable::TPtr<TSession> &GetSession() const {
          return Session;
        }

        /* Ignore me.  I'm just here temporarily. */
        std::string Echo(const std::string &msg) {
          return Server->Echo(msg);
        }

        /* See <orly/protocol.h>. */
        void InstallPackage(const std::vector<std::string> &package_name, uint64_t version) {
          Server->InstallPackage(package_name, version);
        }

        /* See <orly/protocol.h>. */
        Base::TUuid NewFastPrivatePov(const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live) {
          return Session->NewFastPrivatePov(Server, parent_pov_id, time_to_live);
        }

        /* See <orly/protocol.h>. */
        Base::TUuid NewSafePrivatePov(const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live) {
          return Session->NewSafePrivatePov(Server, parent_pov_id, time_to_live);
        }

        /* See <orly/protocol.h>. */
        Base::TUuid NewFastSharedPov(const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live) {
          return Session->NewFastSharedPov(Server, parent_pov_id, time_to_live);
        }

        /* See <orly/protocol.h>. */
        Base::TUuid NewSafeSharedPov(const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live) {
          return Session->NewSafeSharedPov(Server, parent_pov_id, time_to_live);
        }

        /* See <orly/protocol.h>. */
        void SetTimeToLive(const Base::TUuid &durable_id, const std::chrono::seconds &time_to_live) {
          Session->SetTimeToLive(Server, durable_id, time_to_live);
        }

        /* See <orly/protocol.h>. */
        void SetUserId(const Base::TUuid &user_id) {
          Session->SetUserId(Server, user_id);
        }

        /* See <orly/protocol.h>. */
        void PausePov(const Base::TUuid &pov_id) {
          Session->PausePov(Server, pov_id);
        }

        /* See <orly/protocol.h>. */
        void UnpausePov(const Base::TUuid &pov_id) {
          Session->UnpausePov(Server, pov_id);
        }

        /* See <orly/protocol.h>. */
        void UninstallPackage(const std::vector<std::string> &package_name, uint64_t version) {
          Server->UninstallPackage(package_name, version);
        }

        /* See <orly/protocol.h>. */
        TMethodResult Try(const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure) {
          return Session->Try(Server, pov_id, fq_name, closure);
        }

        /* Batch of N calls of one method, folded into a single transaction (#253). */
        TMethodResult TryBatch(
            const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const std::vector<TClosure> &closures) {
          return Session->TryBatch(Server, pov_id, fq_name, closures);
        }

        /* See <orly/protocol.h>. */
        TMethodResult TryTracked(const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure) {
          return Session->TryTracked(Server, pov_id, fq_name, closure);
        }

        /* See <orly/protocol.h>. */
        TMethodResult DoInPast(
            const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure, const Base::TUuid &tracking_id) {
          return Session->DoInPast(Server, pov_id, fq_name, closure, tracking_id);
        }

        /* See <orly/protocol.h>. */
        void BeginImport() {
          Server->BeginImport();
        }

        /* See <orly/protocol.h>. */
        void EndImport() {
          Server->EndImport();
        }

        /* See <orly/protocol.h>. */
        void TailGlobalPov() {
          Server->TailGlobalPov();
        }

        /* See <orly/protocol.h>. */
        std::string ImportCoreVector(const std::string &file_pattern, const std::string &pkg_name, int64_t num_load_threads, int64_t num_merge_threads, int64_t merge_simultaneous) {
          return Server->ImportCoreVector(file_pattern, pkg_name, num_load_threads, num_merge_threads, merge_simultaneous);
        }

        /* Run the RPC I/O with the client.  This function is called by ServeClient() after the handshake has
           been negotiated.  This function doesn't return until the client hangs up.  It will throw exceptions
           if the client times out, commits a syntax error, or otherwise does something weird. */
        void Run(Base::TFd &fd);

        /* Run the given jump-runnable on the server's websockets runner. */
        void RunWs(Indy::Fiber::TJumpRunnable &&jump_runnable) {
          Server->RunWs(std::move(jump_runnable));
        }

        /* Construct a new connection for the given server, connected to the given session.  Neither the server
           pointer nor the session pointer may be null.  If the server already has a connection to the given session,
           this function returns null. */
        static std::shared_ptr<TConnection> New(TServer *server, const Durable::TPtr<TSession> &session);

        class TConnectionRunnable
            : public Indy::Fiber::TRunnable {
          NO_COPY(TConnectionRunnable);
          public:

          TConnectionRunnable(Indy::Fiber::TRunner *runner, const std::shared_ptr<const Rpc::TAnyRequest> &request);

          ~TConnectionRunnable();

          void Compute();

          private:

          Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;

          Indy::Fiber::TFrame *Frame;

          std::shared_ptr<const Rpc::TAnyRequest> Request;

        };


        private:

        /* The RPC protocol spoken by our end of this conversation. */
        class TProtocol
            : public Rpc::TProtocol {
          NO_COPY(TProtocol);
          public:

          /* Our singleton. */
          static const TProtocol Protocol;

          private:

          /* Registers the RPC entry points. */
          TProtocol();

        };  // TServer::TConnection::TProtocol

        TConnection(TServer *server, const Durable::TPtr<TSession> &session);

        static void OnRelease(TConnection *connection);

        TServer *const Server;

        const Durable::TPtr<TSession> Session;

      };  // TServer::TConnection

      /* Constructed by NewSession() and ResumeSession() to hold a session open for
         the websockets server. */
      class TSessionPin final
          : public TWs::TSessionPin {
        public:

        /* Construct a new session. */
        explicit TSessionPin(TServer *server);

        /* Resume an old session. */
        TSessionPin(TServer *server, const Base::TUuid &id);

        /* Pass-throughs to the session object we have pinned. */
        virtual void BeginImport() const override;
        virtual void EndImport() const override;
        virtual const Base::TUuid &GetId() const override;
        virtual void Import(const std::string &, const std::string &, int64_t, int64_t, int64_t) const override;
        virtual void InstallPackage(const std::vector<std::string> &, uint64_t) const override;
        virtual Base::TUuid NewPov(bool, bool, const std::optional<Base::TUuid> &) const override;
        virtual void PausePov(const Base::TUuid &) const override;
        virtual void SetTtl(const Base::TUuid &, const std::chrono::seconds &) const override;
        virtual void SetUserId(const Base::TUuid &) const override;
        virtual void Tail() const override;
        virtual TMethodResult Try(const TMethodRequest &) const override;
        virtual TMethodResult TryBatch(
            const Base::TUuid &, const std::vector<std::string> &, const std::vector<TClosure> &) const override;
        virtual void UninstallPackage(const std::vector<std::string> &, uint64_t) const override;
        virtual void UnpausePov(const Base::TUuid &) const override;

        private:

        /* Our link to the reset of the server. */
        std::shared_ptr<TConnection> Conn;

      };  // TServer::TSessionPin

      class TServeClientRunnable
          : public Indy::Fiber::TRunnable {
        NO_COPY(TServeClientRunnable);
        public:

        TServeClientRunnable(TServer *server, Indy::Fiber::TRunner *runner, Base::TFd &&fd, const Socket::TAddress &client_address)
            : Server(server),
              Fd(fd),
              ClientAddress(client_address) {
          FramePool = Indy::Fiber::TFrame::LocalFramePool;
          Frame = FramePool->Alloc();
          try {
              Frame->Latch(runner, this, static_cast<Indy::Fiber::TRunnable::TFunc>(&TServeClientRunnable::Serve));
          } catch (...) {
            FramePool->Free(Frame);
            throw;
          }
        }

        virtual ~TServeClientRunnable() {
        }

        void Serve() {
          Server->ServeClient(Fd, ClientAddress);
          Indy::Fiber::FreeMyFrame(FramePool);
          delete this;
        }

        private:

        TServer *Server;

        Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;
        Indy::Fiber::TFrame *Frame;

        /* TODO(#364): Push <strm/fd.h> up to here, that should own our Fd and be our common Io library */
        Base::TFd Fd;

        const Socket::TAddress ClientAddress;

      };  // TServeClientRunnable

      /* Accepts connections from clients on our main socket.  Launched as a thread by the constructor. */
      void AcceptClientConnections();

      /* See <orly/protocol.h>. */
      void BeginImport();

      /* Performs housecleaning operations at a rate regulated by HousecleaningTimer. */
      void CleanHouse();

      /* Ignore me! */
      std::string Echo(const std::string &msg);

      /* See <orly/protocol.h>. */
      void EndImport();

      /* See <orly/protocol.h>. */
      void TailGlobalPov();

      /* See <orly/protocol.h>. */
      std::string Import(const std::string &file, int64_t xact_count);

      /* See <orly/protocol.h>. */
      std::string ImportCoreVector(const std::string &file_pattern, const std::string &pkg_name, int64_t num_load_threads, int64_t num_merge_threads, int64_t merge_simultaneous);

      /* See <orly/protocol.h>. */
      void InstallPackage(const std::vector<std::string> &package_name, uint64_t version);

      /* Run the given jump-runnable on our websockets runner. */
      void RunWs(Indy::Fiber::TJumpRunnable &&jump_runnable) {
        jump_runnable(FramePoolManager.get(), &WsRunner);
      }

      /* Serves a client on the given fd.  Launched as a thread by AcceptClientConnections() when a client connects. */
      void ServeClient(Base::TFd &fd, const Socket::TAddress &client_address);

      void StateChangeCb(Orly::Indy::TManager::TState state);

      /* See <orly/protocol.h>. */
      void UninstallPackage(const std::vector<std::string> &package_name, uint64_t version);

      void WaitForSlave();

      /* Schedule a job that hosts the given runner's scheduler loop
         (Fiber::LaunchSlowFiberSched), bracketed by the entered/exited
         accounting Shutdown() joins on (see RunnerHostsEntered). */
      void ScheduleRunnerHost(Indy::Fiber::TRunner *runner);

      Indy::Fiber::TFrame *Frame;

      std::vector<Indy::Fiber::TFrame *> MergeMemFrameVec;
      std::vector<std::unique_ptr<Indy::Fiber::TRunner>> MergeMemRunnerVec;
      std::vector<Indy::Fiber::TFrame *> MergeDiskFrameVec;
      std::vector<std::unique_ptr<Indy::Fiber::TRunner>> MergeDiskRunnerVec;
      Indy::Fiber::TRunner DurableLayerCleanerRunner;
      Indy::Fiber::TRunner RepoLayerCleanerRunner;
      Indy::Fiber::TRunner BGFastRunner;
      Indy::Fiber::TRunner WaitForSlaveRunner;
      Indy::Fiber::TRunner RunReplicationQueueRunner;
      Indy::Fiber::TRunner RunReplicationWorkRunner;
      Indy::Fiber::TRunner RunReplicateTransactionRunner;
      Indy::Fiber::TRunner WsRunner;
      std::unordered_set<Indy::Fiber::TRunner *> ForEachSchedCallbackExtraSet;

      std::unordered_map<TIndexType, Base::TUuid> IndexByIndexId;
      std::unordered_set<Base::TUuid> IndexIdSet;
      Atom::TSuprena IndexMapArena;
      mutable std::mutex IndexMapMutex;

      /* Used for simulation, backed by memory. */
      std::unique_ptr<Indy::Disk::Sim::TMemEngine> SimMemEngine;

      /* Used when not running in memory simulation mode. */
      std::unique_ptr<Indy::Disk::Util::TDiskEngine> DiskEngine;

      std::unique_ptr<Orly::Indy::TManager> RepoManager;

      Orly::Indy::L0::TManager::TPtr<Indy::TRepo> GlobalRepo;

      Orly::Indy::TManager::TState RepoState;

      Orly::Package::TManager PackageManager;

      /* This is necessary to make the type singletons and interners visible to the packages being loaded. */
      Type::TTypeCzar TypeCzar;

      /* The scheduler we use to launch jobs.  Set by the constructor and never changed. */
      Base::TScheduler *const Scheduler;

      /* The command-line arguments we constructed with. */
      const TCmd &Cmd;

      /* The manager of durable objects.  Set by the constructor and never null. */
      std::shared_ptr<Durable::TManager> DurableManager;

      /* Tetris lives here. */
      TRepoTetrisManager *TetrisManager;

      /* The timer waited for by CleanHouse(). */
      Base::TTimerFd HousecleaningTimer;

      /* CleanHouse() marks Started on entry and pushes Exited when it
         returns, so Shutdown() can wait for the housekeeper to actually be
         out of DurableManager->Clean() before tearing the manager down
         (#440). */
      std::atomic<bool> HousekeeperStarted{false};
      Base::TEventSemaphore HousekeeperExited;

      /* Every scheduler-hosted runner loop (ScheduleRunnerHost) counts
         itself in on entry and pushes Exited on the way out, so Shutdown()
         can wait for the loops to actually return before the runner objects
         (all TServer members) are destroyed -- a ShutDown() is only a flag,
         and a loop between its last KeepRunning check and its exit would
         otherwise race member destruction (#463).  As with the service-loop
         joins, a loop whose job never got a scheduler worker never counted
         itself and is not waited for (#462). */
      std::atomic<size_t> RunnerHostsEntered{0};
      Base::TEventSemaphore RunnerHostExited;

      /* The socket on which AcceptClientConnections() listens. */
      Base::TFd MainSocket;

      /* The socket on which WaitForSlave() listens.  A member (rather than
         a local of WaitForSlave) so Shutdown() can shut it down to wake the
         blocked accept (#440). */
      Base::TFd SlaveSocket;

      /* Serializes WaitForSlave()'s publication of SlaveSocket against
         Shutdown()'s wake-up shutdown(2) of it: TFd is not atomic, and
         without the handshake Shutdown() could shut down a stale fd (or
         -1) and leave the accept parked forever (#440). */
      std::mutex SlaveSocketLock;

      /* Covers ConnectionBySessionId. */
      std::mutex ConnectionMutex;

      /* All our open connections. */
      std::unordered_map<Base::TUuid, std::weak_ptr<TConnection>> ConnectionBySessionId;

      std::unique_ptr<TIndyReporter> Reporter;

      std::shared_ptr<Orly::Indy::Disk::TIndyUtilReporter> UtilReporter;

      std::shared_ptr<std::function<void (const Base::TFd &)>> WaitForSlaveActionCb;

      /* Set once Shutdown() has run; makes it idempotent and tells the
         destructor the managers are already gone.  Atomic: the service
         loops (accept, housekeeper, slave wait) read it from their own
         threads. */
      std::atomic<bool> ShutdownCalled{false};

      bool InitFinished;
      std::condition_variable InitCond;
      std::mutex InitMutex;

      /* Set by Init() if it fails; the constructor rethrows it after the
         InitFinished handshake so a server that could not initialize does
         not limp along half-built (#435). */
      std::exception_ptr InitError;

      /* The websockets server object */
      std::unique_ptr<TWs> Ws;

      /* The thread on which WsRunner runs. */
      std::thread WsThread;

      friend class TIndyReporter;

    };  // TServer

  }  // Server

}  // Orly
