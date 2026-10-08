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
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

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

      /* Thrown when an update carries an assertion Tetris cannot test. */
      DEFINE_ERROR(TUnsupportedAssertion, std::runtime_error,
                   "update assertion not supported by tetris");

      /* Thrown when a global-pov update carries more than one original
         update's metadata: promoting it would need a collapsed meta record,
         which the promotion path does not build (#376). */
      DEFINE_ERROR(TCollapsedUpdateError, std::runtime_error,
                   "collapsed updates not supported by tetris promotion");

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

          /* Takes this child's promotion hold on `transaction` and says whether the child can play
             this round: it has an update (or holds one already) and isn't paused.  Copies nothing,
             so it makes no pool claim; it runs under the player's mutex. */
          bool Refresh(const std::unique_ptr<Indy::L1::TTransaction, std::function<void (Indy::L1::TTransaction *)>> &transaction);

          /* Copies this child's lowest update out on `transaction`, which Refresh must have
             taken the hold on, and parses its metadata; true at once if we hold one already.
             False if there is no update, or no room to copy it (#607).  Only for a child the
             round may promote, outside the player's mutex (#660). */
          bool Peek(const std::unique_ptr<Indy::L1::TTransaction, std::function<void (Indy::L1::TTransaction *)>> &transaction);

          static bool SortsBefore(const TChild *lhs, const TChild *rhs);

          /* True iff this child carries no assertions -- every refreshed entry
             is a pure commutative field call (`+=` / `|=`, empty
             GetExpectedPredicateResults). Such a child provably cannot
             conflict (architecture.md §5), so the fast-lane may promote it in
             the same round as any other assertion-free child. Requires a
             prior Peek(). */
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

          /* Drop the peeked update and what was parsed from it; the next Peek copies it again. */
          void Flush();

          /* Call once the transaction a Play promoted on has been applied: records, for the
             durable version (#750), the version the parent gave the promoted update. */
          void FinishPromotion();

          private:

          bool TestAssertions(Indy::TContext &context) const;


          /* The player which owns us.  Never null. */
          TPlayer *Player;

          /* The number of rounds we have been ready to play since our last promotion (or failure). */
          size_t Age;

          /* The number of times we have tested our assertions and failed. */
          size_t FailureCount;

          /* The repo which backs up this child pov. */
          Indy::L0::TManager::TPtr<Indy::TRepo> Repo;

          std::shared_ptr<Indy::TUpdate> PeekedUpdate;

          /* The sequence number PeekedUpdate had in the child's repo. */
          std::optional<Indy::TSequenceNumber> PeekedSeq;

          /* The last promotion into the global repo, until FinishPromotion: our own sequence number
             for the update, and the one the global repo gave it once its transaction applied. */
          std::optional<Indy::TSequenceNumber> PromotedOwnSeq;
          std::optional<Indy::TSequenceNumber> PromotedParentSeq;

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

        /* See TRepoTetrisManager::TPlayer.  Plays rounds, one after another, for up to TurnBudget
           (#801): until a round promotes nothing, the budget runs out, or KeepPlaying() is false.
           Each round is exactly what one call used to be: its own snapshot of the children, its
           own context on the parent, its own transactions, committed before the next round
           starts, so every assertion is tested against the parent as the rounds before it left
           it.  What the turn saves is the yield between rounds, and a durable round trip per
           promotion (see DeliverAccepted). */
        virtual void Play() override;

        /* One round, as above.  True iff it promoted something. */
        bool PlayRound();

        /* Tells the sessions whose updates this turn promoted (#801).  Each promotion used to open
           its session in the durable manager and drop it again at once, inside the round.  A
           session whose client has gone is closed, so that drop saved it to the durable store, and
           waited for the save on an OS semaphore, blocking the Tetris runner's thread: ~6 ms per
           promotion on a disk-backed server, 93% of a round.  Now a round only records what it
           promoted, once its transaction has committed, and the turn hands each session its
           notifications through a pin it keeps (SessionPins), so a session is opened once, and
           saved once when the pin goes, not once per promotion.  Never throws: the promotions it
           reports have committed; a session that can't be opened gets no notification. */
        void DeliverAccepted() noexcept;

        /* Drops every session pin.  A closed session is saved, and its lease starts, as each one
           goes.  Never throws. */
        void ReleaseSessionPins() noexcept;

        /* How long one Play() may run rounds before it lets Main() yield the runner. */
        static constexpr std::chrono::milliseconds TurnBudget{5};

        /* Pins are dropped when a turn promotes nothing, when the player pauses or dies, when
           there are more than MaxSessionPins of them, and after PinHoldLimit, so a closed session
           whose updates are promoted for a long time is still saved that often. */
        static constexpr std::chrono::milliseconds PinHoldLimit{1000};
        static constexpr size_t MaxSessionPins = 1024UL;

        /* Our manager.  Never null. */
        TRepoTetrisManager *RepoTetrisManager;

        /* (session id, update id) for each update promoted in the round being played, and
           in the rounds of this turn that have committed.  A round's entries move to
           TurnAccepted only once its transaction has committed. */
        std::vector<std::pair<Base::TUuid, Base::TUuid>> RoundAccepted, TurnAccepted;

        /* See DeliverAccepted. Only ever touched by our own fiber. */
        std::unordered_map<Base::TUuid, Durable::TPtr<TSession>> SessionPins;
        std::chrono::steady_clock::time_point PinnedSince;

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
