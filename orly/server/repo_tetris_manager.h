/* <orly/server/repo_tetris_manager.h>

   The manager and players of repo-based tetris.

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

#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include <base/class_traits.h>
#include <base/thrower.h>
#include <orly/indy/context.h>
#include <orly/indy/manager.h>
#include <orly/package/manager.h>
#include <orly/server/meta_record.h>
#include <orly/server/session.h>
#include <orly/server/tetris_manager.h>

namespace Orly {

  namespace Server {

    class TRepoTetrisManager final
        : public TTetrisManager {
      public:

      /* Thrown when an update carries an assertion Tetris cannot test --
         today, any memcache update other than a bare `set`. */
      DEFINE_ERROR(TUnsupportedAssertion, std::runtime_error,
                   "update assertion not supported by tetris");

      TRepoTetrisManager(
          Base::TScheduler *scheduler,
          Indy::Fiber::TRunner::TRunnerCons &runner_cons,
          Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
          const std::function<void (Indy::Fiber::TRunner *)> &runner_setup_cb,
          bool is_master,
          Indy::TManager *repo_manager,
          Package::TManager *package_manager,
          Durable::TManager *durable_manager,
          bool log_assertion_failures,
          bool commutative_fastlane = false);

      virtual ~TRepoTetrisManager();

      std::atomic<size_t> PushCount;
      std::atomic<size_t> PopCount;
      std::atomic<size_t> FailCount;
      std::atomic<size_t> RoundCount;

      /* Total children Refresh()ed (re-snapshotted) across all rounds. Under
         the one-promote-per-round discipline this grows ~O(N^2) in the backlog,
         which is the #234 cap; the commutative fast-lane drives it toward O(N).
         Observable in the server stats so the cap is visible in CI. */
      std::atomic<size_t> ChildrenConsideredCount;

      /* If true, promote ALL ready assertion-free children per round (#234). */
      const bool CommutativeFastlane;

      Base::TSigmaCalc TetrisSnapshotCPUTime;
      Base::TSigmaCalc TetrisSortCPUTime;
      Base::TSigmaCalc TetrisPlayCPUTime;
      Base::TSigmaCalc TetrisCommitCPUTime;
      std::mutex TetrisTimerLock;

      private:

      class TPlayer final
          : public TTetrisManager::TPlayer {
        public:

        TPlayer(TRepoTetrisManager *repo_tetris_manager, const Base::TUuid &parent_pov_id, const Base::TUuid &child_pov_id, bool is_paused, bool is_master);

        virtual ~TPlayer();

        private:

        class TChild {
          NO_COPY(TChild);
          public:

          TChild(TPlayer *player, const Base::TUuid &child_pov_id);

          bool Play(
              const std::unique_ptr<Indy::L1::TTransaction, std::function<void (Indy::L1::TTransaction *)>> &transaction, Indy::TContext &context);

          bool Refresh(const std::unique_ptr<Indy::L1::TTransaction, std::function<void (Indy::L1::TTransaction *)>> &transaction);

          static bool SortsBefore(const TChild *lhs, const TChild *rhs);

          /* True iff this child carries no assertions -- every refreshed entry
             is a pure commutative field call (`+=` / `|=`, empty
             GetExpectedPredicateResults) and none is a Mynde entry. Such a
             child provably cannot conflict (architecture.md §5), so the
             fast-lane may promote it in the same round as any other
             assertion-free child. Requires a prior Refresh(). */
          bool IsAssertionFree() const;

          /* Fast-lane promotion (#234): drop any prior snapshot Peek, re-Peek
             this child on `transaction`, and -- if an update is present --
             promote it (Push to parent + Pop) on that SAME transaction.
             Returns true iff a promotion was issued. Keeping the Peek and the
             Pop on one transaction is load-bearing: a Pop on a transaction that
             did not Peek the child mints a second, independent popper while the
             snapshot peek's read View is still live, and PopLowest then fires
             against a child that may already have been promoted out from under
             it (crashes at K>=4). See concurrent-merge-throughput.md §8.2. */
          bool RepeekAndPlay(
              const std::unique_ptr<Indy::L1::TTransaction, std::function<void (Indy::L1::TTransaction *)>> &transaction, Indy::TContext &context);

          private:

          void Flush();

          bool TestAssertions(Indy::TContext &context) const;

          /* The player which owns us.  Never null. */
          TPlayer *Player;

          /* The number of rounds of tetris we have played. */
          size_t Age;

          /* The number of times we have tested our assertions and failed. */
          size_t FailureCount;

          /* The repo which backs up this child pov. */
          Indy::L0::TManager::TPtr<Indy::TRepo> Repo;

          std::shared_ptr<Indy::TUpdate> PeekedUpdate;

          TMetaRecord MetaRecord;

          std::unordered_map<Base::TUuid, Package::TFuncHolder::TPtr> FuncHolderByUpdateId;

        };  // TRepoTetrisManager::TPlayer::TChild

        /* See TRepoTetrisManager::TPlayer. */
        virtual void OnJoin(const Base::TUuid &child_pov_id) override;

        /* See TRepoTetrisManager::TPlayer. */
        virtual void OnPart(const Base::TUuid &child_pov_id) override;

        /* See TRepoTetrisManager::TPlayer. */
        virtual void OnPause() override;

        /* See TRepoTetrisManager::TPlayer. */
        virtual void OnUnpause() override;

        /* See TRepoTetrisManager::TPlayer. */
        virtual void Play() override;

        /* Our manager.  Never null. */
        TRepoTetrisManager *RepoTetrisManager;

        /* The repo which backs up this parent pov. */
        Indy::L0::TManager::TPtr<Indy::TRepo> Repo;

        /* Covers 'ChildByPovId', below. */
        std::mutex Mutex;

        /* A mapping from a child pov id to the object managing our relationship with that child. */
        std::unordered_map<Base::TUuid, TChild *> ChildByPovId;

      };  // TRepoTetrisManager::TPlayer

      virtual TTetrisManager::TPlayer *NewPlayer(const Base::TUuid &parent_pov_id, const Base::TUuid &child_pov_id, bool is_paused, bool is_master) override;

      private:

      /* Our manager of repos.  Never null. */
      Indy::TManager *RepoManager;

      /* Our manager of packages.  Never null. */
      Package::TManager *PackageManager;

      /* Our manager of durables.  Never null. */
      Durable::TManager *DurableManager;

      bool LogAssertionFailures;

    };  // TRepoTetrisManager

  }  // Server

}  // Orly
