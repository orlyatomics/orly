/* <orly/server/session.h>

   An user's session.

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

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <base/class_traits.h>
#include <base/event_semaphore.h>
#include <optional>
#include <base/sigma_calc.h>
#include <base/thread_local_sigma_calc.h>
#include <base/thrower.h>
#include <base/uuid.h>
#include <orly/durable/kit.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/method_request.h>
#include <orly/method_result.h>
#include <orly/notification/notification.h>
#include <orly/package/manager.h>
#include <orly/server/pov.h>

namespace Orly {

  namespace Server {

    /* An open session. */
    class TSession final
        : public Durable::TObj {
      NO_COPY(TSession);
      public:

      /* Convenience. */
      using TNotification = Notification::TNotification;

      class TServer
          : public TPov::TServer {
        public:

        virtual ~TServer() {}

        virtual const std::shared_ptr<Durable::TManager> &GetDurableManager() const = 0;

        virtual const Package::TManager &GetPackageManager() const = 0;

        virtual Base::TScheduler *GetScheduler() const = 0;

        /* Write-backpressure high-watermark (#234); 0 disables. The accept path
           in Try() yields until a writer's POV child drains below this. */
        virtual size_t GetWriteBackpressureThreshold() const = 0;

        /* Per-`Try` latency/counter statistics. These are pushed on every read
           and write (the hot path) and folded into a single aggregate by the
           periodic reporter. TThreadLocalSigmaCalc keeps a private accumulator
           per producing thread so Push() takes no globally-contended lock --
           the old per-`Try` `TryTimeLock` mutex is gone. */
        static Base::TThreadLocalSigmaCalc TryReadTimeCalc;
        static Base::TThreadLocalSigmaCalc TryReadCPUTimeCalc;
        static Base::TThreadLocalSigmaCalc TryWriteTimeCalc;
        static Base::TThreadLocalSigmaCalc TryWriteCPUTimeCalc;
        static Base::TThreadLocalSigmaCalc TryWalkerCountCalc;
        static Base::TThreadLocalSigmaCalc TryWalkerConsTimerCalc;
        static Base::TThreadLocalSigmaCalc TryCallCPUTimerCalc;
        static Base::TThreadLocalSigmaCalc TryReadCallTimerCalc;
        static Base::TThreadLocalSigmaCalc TryWriteCallTimerCalc;
        static Base::TThreadLocalSigmaCalc TryFetchCountCalc;
        static Base::TThreadLocalSigmaCalc TryHashHitCountCalc;

        static Base::TThreadLocalSigmaCalc TryWriteSyncHitCalc;
        static Base::TThreadLocalSigmaCalc TryWriteSyncTimeCalc;
        static Base::TThreadLocalSigmaCalc TryReadSyncHitCalc;
        static Base::TThreadLocalSigmaCalc TryReadSyncTimeCalc;

        protected:

        TServer(size_t num_runners) : RunnerCons(num_runners), SlowAssignmentCounter(0UL), FastAssignmentCounter(0UL) {}

        void InitalizeFramePoolManager(size_t num_frames, size_t frame_stack_size, Indy::Fiber::TRunner *runner) {
          FramePoolManager = std::unique_ptr<Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>>(
            new Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>(num_frames, frame_stack_size, runner));
        }

        Indy::Fiber::TRunner::TRunnerCons RunnerCons;
        std::vector<std::unique_ptr<Indy::Fiber::TRunner>> SlowRunnerVec;
        std::vector<std::unique_ptr<std::thread>> SlowRunnerThreadVec;
        std::atomic<size_t> SlowAssignmentCounter;
        std::unique_ptr<Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>> FramePoolManager;
        std::vector<std::unique_ptr<Indy::Fiber::TRunner>> FastRunnerVec;
        std::atomic<size_t> FastAssignmentCounter;
        std::vector<std::unique_ptr<std::thread>> FastRunnerThreadVec;

        friend class TSession;

      };  // TServer

      /* Temporary.  Once all the RPC entry points are implmented, remove this error. */
      DEFINE_ERROR(TStubbed, std::logic_error, "the RPC entry point is not yet implemented");

      /* See <orly/protocol.h>. */
      TMethodResult DoInPast(
          TServer *server, const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure,
          const Base::TUuid &tracking_id);

      /* Call back for each pending notification, in order of increasing sequence number. */
      bool ForEachNotification(const std::function<bool (uint32_t, const TNotification *)> &cb) const;

      const TNotification *GetFirstNotification(uint32_t &seq_number);

      virtual const char *GetKind() const noexcept override {
        return "Session";
      }

      /* The number of pending notifications. */
      size_t GetNotificationCount() const {
        std::lock_guard<std::mutex> lock(NotificationMutex);
        return NotificationBySeqNumber.size();
      }

      const Base::TEventSemaphore &GetNotificationSem() const {
        return NotificationSem;
      }

      /* The id of the user who owns this session.  If the session is anonymous, this is unknown. */
      const std::optional<Base::TUuid> &GetUserId() const {
        return UserId;
      }

      /* See <orly/protocol.h>. */
      Base::TUuid NewFastPrivatePov(TServer *server, const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live);

      /* See <orly/protocol.h>. */
      Base::TUuid NewFastSharedPov(TServer *server, const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live);

      /* See <orly/protocol.h>. */
      Base::TUuid NewSafePrivatePov(TServer *server, const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live);

      /* See <orly/protocol.h>. */
      Base::TUuid NewSafeSharedPov(TServer *server, const std::optional<Base::TUuid> &parent_pov_id, const std::chrono::seconds &time_to_live);

      /* See <orly/protocol.h>. */
      void PausePov(TServer *server, const Base::TUuid &pov_id);

      /* Insert the given notification into the pending set and return the sequence number that is assigned to it.
         If this function fails, it will delete the notification before throwing. */
      uint32_t InsertNotification(Notification::TNotification *notification);

      /* Remove the notification with the given sequence number.
         If notification doesn't exist (never existed or has already been discarded), do nothing. */
      void RemoveNotification(uint32_t seq_number);

      /* See <orly/protocol.h>. */
      void SetTimeToLive(TServer *server, const Base::TUuid &durable_id, const std::chrono::seconds &ttl);

      /* See <orly/protocol.h>. */
      void SetUserId(TServer *server, const Base::TUuid &user_id);

      /* See <orly/protocol.h>. */
      TMethodResult Try(TServer *server, const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure);

      /* Batched write (#253): invoke one method (resolved once) against each of
         the given argument closures, accumulating every call's effects into a
         single transaction committed once. Returns a list-typed result with one
         entry per call, in order. All-or-nothing: a throw in any call aborts the
         whole batch before commit. */
      TMethodResult TryBatch(TServer *server, const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const std::vector<TClosure> &closures);

      /* See <orly/protocol.h>. */
      bool RunTestSuite(TServer *server, const std::vector<std::string> &package_name, uint64_t package_version, bool verbose);

      /* Return the notification with the given sequence number.  If there is no such notification, return null. */
      TNotification *TryGetNotification(uint32_t seq_number) const;

      /* See <orly/protocol.h>. */
      TMethodResult TryTracked(TServer *server, const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure);

      /* See <orly/protocol.h>. */
      void UnpausePov(TServer *server, const Base::TUuid &pov_id);

      /* The id of the global point of view. */
      static const Base::TUuid GlobalPovId;

      /* Add the given pov to the collection of povs we'll keep open.
         Public only because the memcache connection path constructs its
         private pov outside the session (server.cc); when #371 moves that
         to pooled, session-owned povs, this can go private. */
      void AddPov(const Durable::TPtr<TPov> &pov);

      private:

      TSession(Durable::TManager *manager, const Base::TUuid &id, const Durable::TTtl &ttl);

      TSession(Durable::TManager *manager, const Base::TUuid &id, Io::TBinaryInputStream &strm);

      /* Calls Cleanup(). */
      virtual ~TSession();

      /* Run a test func against the POV represented by 'pov_id' and commit any
         effects it produces directly into that POV's repo. The compile-time
         test runner relies on indy's parent-chain read fallthrough for
         cross-level visibility (a child test POV reads its ancestors), so there
         is no Tetris promotion step here -- unlike the SPA flux model, an indy
         transaction can write straight into a (paused) shared POV. The commit
         mirrors the normal write path (Try), including the #49 deferred
         commutative-entry handling. */
      void RunFuncCommit(TServer *server,
          const std::vector<std::string> &package_name,
          const std::function<void(Package::TContext &ctx)> &func,
          const Base::TUuid &pov_id);

      /* Run every test case in 'test_block' against a fresh paused shared child
         of 'parent_pov_id' (one per case, for sibling isolation), recursing into
         each passing case's SubCases. Returns the &&-accumulation of all case
         results. Mirrors SPA's RunTest/RunTestBlock semantics and output. */
      bool RunTestBlock(TServer *server,
          const std::vector<std::string> &package_name,
          const Base::TUuid &parent_pov_id,
          const Package::TTestBlock &test_block, bool verbose);

      /* Seed a freshly-created test POV's sequence counter from 'parent_pov_id'
         (or the global POV when unset) so sequence numbers increase
         monotonically down the test POV chain. The point-read fold
         (present_walker) resolves a key present at several chain levels by
         sequence number; child repos otherwise start at 1, so a child's write
         could not shadow an ancestor's older value and a subcase would read the
         stale ancestor value. Compile-time tests are single-threaded, so no
         lock is taken. */
      void SeedTestPovSequence(TServer *server, const Base::TUuid &child_pov_id,
          const std::optional<Base::TUuid> &parent_pov_id);

      /* Stream out. */
      virtual void Write(Io::TBinaryOutputStream &strm) const override;

      void Cleanup();

      Base::TUuid NewPov(
          TServer *server, const std::optional<Base::TUuid> &parent_pov_id, TPov::TAudience audience, TPov::TPolicy policy,
          const std::chrono::seconds &time_to_live);

      /* See base class. */
      virtual bool ForEachDependentPtr(const std::function<bool (Durable::TAnyPtr &)> &cb) noexcept override;

      /* See accessor. */
      std::optional<Base::TUuid> UserId;

      /* The sequence number to assign to the next notification. */
      uint32_t NextSeqNumber;

      /* The queue of pending notifications. */
      std::map<uint32_t, TNotification *> NotificationBySeqNumber;
      mutable std::mutex NotificationMutex;
      mutable Base::TEventSemaphore NotificationSem;

      /* Povs to keep alive while we're alive. */
      std::vector<Durable::TPtr<TPov>> Povs;
      std::mutex PovMutex;

      /* For access to constructors/destructor. */
      friend class Durable::TManager;

    };  // TSession

  }  // Server

}  // Orly
