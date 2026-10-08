/* <orly/server/tetris_manager.h>

   The base for all managers and players of the tetris.

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
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <base/class_traits.h>
#include <chrono>

#include <base/event_semaphore.h>
#include <base/scheduler.h>
#include <base/uuid.h>
#include <orly/indy/fiber/fiber.h>

namespace Orly {

  namespace Server {

    /* The base for all managers of tetris players. */
    class TTetrisManager {
      NO_COPY(TTetrisManager);
      public:

      /* True iff. the given player is paused. */
      bool IsPlayerPaused(const Base::TUuid &parent_pov_id) const;

      /* Advise tetris that a child pov has something to tell its parent parent pov.
         If the parent pov is new to the manager, this will launch a new tetris player.
         The player may or may not have started by the time this function returns. */
      void Join(const Base::TUuid &parent_pov_id, const Base::TUuid &child_pov_id);

      /* Advise tetris that a child pov has nothing more to tell its parent pov.
         If this is the parent pov's last child, this will kill the parent's tetris player.
         The player may or may not have stopped by the time this function returns. */
      void Part(const Base::TUuid &parent_pov_id, const Base::TUuid &child_pov_id);

      /* Cause the player for the given pov to stop playing.  The player will continue to
         exist and will continue to handle children joining and parting, but it will not
         attempt to make progress.  This function will not return until the player has
         paused. */
      void PausePlayer(const Base::TUuid &parent_pov_id);

      /* Cause a player that was previously paused to resume normal operations. */
      void UnpausePlayer(const Base::TUuid &parent_pov_id);

      void BecomeMaster();

      /* The number of parent povs with children still waiting to be promoted into them, not
         counting paused ones (their children wait for an unpause).  Zero means every update
         committed to a pov has reached its parent, all the way up.  A graceful shutdown waits
         for zero before its final flush, so acknowledged writes reach the global pov, and so
         disk (#744). */
      size_t GetUnpausedPlayerCount() const;

      /* Stop promoting (#769): from now on no player starts a round, though the players stay
         (StopAllPlayers still stops them).  Waits up to `timeout` for the rounds in flight to
         finish, and returns false if one is still running then.  A graceful shutdown calls it
         when it gives up waiting for the backlogs, so that what it counts as lost is exactly
         what is lost, and the global pov's flush isn't chasing promotions.  Not on a fiber: it
         blocks its thread. */
      bool HaltPromotion(std::chrono::milliseconds timeout);

      protected:

      /* The base class for all players of the tetris. */
      class TPlayer
          : public Indy::Fiber::TRunnable {
        NO_COPY(TPlayer);
        public:

        /* Increments the child count and calls OnJoin(). */
        void Join(const Base::TUuid &child_pov_id);

        /* Decrements the child count.
           If the result is non-zero, this function then calls OnPart() and returns true.
           If the result is zero, this function does not call OnPart() and returns false. */
        bool Part(const Base::TUuid &child_pov_id);

        /* Ask Main() to pause at the top of its next round.  Call under the manager's Mutex; then
           release it and AwaitPause() (#657). */
        void RequestPause();

        /* Park until Main() has acknowledged the pause most recently requested.  Returns at once if
           it already has.  Never call it holding the manager's Mutex: the player may need that mutex
           (its commits join and part children) to finish the round it is in (#657). */
        void AwaitPause();

        /* Stops this player and causes it to self-destruct.  Blocks until the player does so. */
        void Stop();

        /* Resume normal operations. */
        void Unpause();

        void OnClose();

        void BecomeMaster();

        protected:

        /* Caches the pointer to the tetris manager and gets ready to run but doesn't launch a job.
           You must call Start() in the constructor of your derived player or tetris will not be played. */
        TPlayer(TTetrisManager *tetris_manager);

        /* A players always dies by self-destruction, either because the last of its children parts from it
           or because the manager calls Stop().  If the player is self-destructing as a result of Stop(),
           Main() unblocks the stopper after the delete completes. */
        virtual ~TPlayer();

        /* Overide to respond to a child pov joining to us.
           Note that this function is not called for the joining of our first child, as that is implied by our construction. */
        virtual void OnJoin(const Base::TUuid &child_pov_id) = 0;

        /* Overide to respond to a child pov parting from us.
           Note that this function is not called for the parting of our last child, as that causes us to self-destruct. */
        virtual void OnPart(const Base::TUuid &child_pov_id) = 0;

        /* Override to respond to the player being paused.
           This is called by the player thread after it ack's the pauser and before it goes to sleep waiting to unpause. */
        virtual void OnPause() = 0;

        /* Override to respond to the player being unpaused.
           This is called by the player thread after it wakes up but before it begins playing again. */
        virtual void OnUnpause() = 0;

        /* Override to play tetris.  This function will be called repeatedly as long as we have children or
           until we are told to stop, so play as efficiently as possible.  This function runs in its own thread,
           separate from the threads which call OnJoin() and OnPart(), so use proper syncrhonization around any
           data structures shared between these functions.  Main() yields the runner after every call (#584), so a
           call that does a lot should stop after a few milliseconds, and as soon as KeepPlaying() is false. */
        virtual void Play() = 0;

        /* Call this in the constructor of your derived player in order to launch the job which will play tetris. */
        void Start(bool is_paused, bool is_master);

        /* True while Main() would call Play() again at once: we still have a child, and no pause
           is waiting to be acknowledged.  Play() may make more than one promotion per call (#801);
           between them it checks this, so a pause, a stop, or the last child parting ends the
           call as promptly as the end of a round used to. */
        bool KeepPlaying() const {
          return ChildCount.load() && !Paused.load();
        }

        private:

        /* Loops, calling Play(), as long as there are child points of view joined to us.
           When last last child goes, this function deletes this player and exits. */
        void Main();

        /* Our manager.  Never null. */
        TTetrisManager *TetrisManager;

        /* The number of children currently joined to us.  This starts at one, as we must be constructed with a single child.
           Main() will run as long as this value is non-zero. Atomic because Join/Part mutate it under the manager's
           Mutex while the player's own Main() fiber reads it unlocked each round (#262 follow-up: TSan data race). */
        std::atomic<size_t> ChildCount;

        /* Threads still touching us after dropping ChildCount (Part() then OnClose(), and Stop()), or
           waiting on our pause outside the manager's Mutex (PausePlayer(), UnpausePlayer()); Main()
           won't free us until it drains (#636, #657). */
        std::atomic<size_t> ToucherCount;

        /* Usually null; Stop() points this at a flag on its own stack.  Main() copies the pointer
           to its stack before 'delete this' and flips the flag as its very last act, so neither side
           touches this object (or the stopper's stack) after the other is done with it.  The old
           handshake had Stop() wait on a member TSafeSync that the destructor completed, letting the
           stopper's wakeup race the delete (#280). */
        std::atomic<std::atomic<bool> *> StopFlag;

        /* Set by RequestPause() (or by Start() for a player born paused); Main() sees it, completes
           PausedSync (re-activating an AwaitPause()), and parks itself on PauseWake.  Atomic:
           written under the manager's Mutex, read by Main() without it. */
        std::atomic<bool> Paused;
        Indy::Fiber::TSafeSync PausedSync;

        /* Set by Main() before it acknowledges a pause, cleared by Unpause().  While it is set,
           Main() stays parked on PauseWake -- unless it has no children left, so a paused player
           whose last child parts (or that is stopped) still exits instead of sleeping where no
           unpause can reach it: Part() takes it out of the map, so UnpausePlayer() can't find it
           (#657).  Unpause(), OnClose() and Stop() all push PauseWake; Main() re-checks. */
        std::atomic<bool> PauseHeld;
        Indy::Fiber::TSingleSem PauseWake;

        Base::TEventSemaphore CanWork;

        /* The fiber frame used to run our logic, and the pool it came from: always our manager's
           PlayerFramePool, never the constructing thread's (#633). */
        Indy::Fiber::TFrame *TetrisFrame;
        Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;

        /* For ToucherCount. */
        friend class TTetrisManager;

      };  // TTetrisManager::TPlayer

      /* Caches the pointer to the scheduler. */
      TTetrisManager(Base::TScheduler *scheduler,
                     Indy::Fiber::TRunner::TRunnerCons &runner_cons,
                     Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                     const std::function<void (Indy::Fiber::TRunner *)> &runner_setup_cb,
                     bool is_master);

      /* You must call StopAllPlayers() in the destructor of your derived tetris manager or this destructor will fail. */
      virtual ~TTetrisManager();

      /* Override to construct a new tetris player connecting the given parent and child points of view. */
      virtual TPlayer *NewPlayer(const Base::TUuid &parent_pov_id, const Base::TUuid &child_pov_id, bool is_paused, bool is_master) = 0;

      /* Call this in the destructor of your derived tetris manager.  It will block until all tetris has stopped. */
      void StopAllPlayers();

      Base::TScheduler *GetScheduler() const {
        return Scheduler;
      }

      private:

      /* The scheduler we use when creating indy contexts. */
      Base::TScheduler *Scheduler;

      /* The Fiber Runners we use when launching tetris players. */
      Indy::Fiber::TRunner FiberScheduler;
      std::unique_ptr<std::thread> FiberThread;
      Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool *FramePool;

      /* Every player's frame comes from this pool (#633).  A player is constructed on whatever
         thread calls Join() -- a fast runner committing a write, the slow runner at startup, or
         WsRunner running `unpause` -- and TFrame::LocalFramePool is not something those threads
         can be relied on to have: WsRunner's thread never installs one, so taking the frame from
         it segfaulted every unpause.  A TThreadLocalPool is single-owner for allocation (its
         AvailableQueue is unsynchronized), so we serialize Alloc() under PlayerFrameMutex and let
         any thread call it.  Free() is a lock-free cross-thread push and needs no lock; peers may
         steal from our free list exactly as from any other pool.  Destroyed after ~TTetrisManager
         has joined FiberThread, so every player frame has been handed back by then. */
      std::mutex PlayerFrameMutex;
      std::unique_ptr<Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool> PlayerFramePool;

      /* Covers 'PlayerByParentPovId', 'PausedSet' and 'Stopping', below.

         A plain mutex, not a fiber lock, and nothing may park while holding it (#657).  Commits take
         it from inside their apply: AppendUpdate joins and PopLowest parts a child, holding the repo
         manager's replication queue lock (a std::mutex) and the child's DataLock.  A fiber lock
         parks a contended waiter and hands the lock to it on its own runner; if that runner's thread
         is meanwhile OS-blocked on the replication queue lock (another player's commit on the
         Tetris runner), the new owner never runs, and the commit that holds the replication queue
         lock never finishes.  With a plain mutex a contended commit blocks its thread for as long as
         the holder takes, and every holder here is brief and never waits for a fiber.  The waits
         that used to happen under it -- PausePlayer() for the player's acknowledgement, and a
         paused Join() for the new player's -- now happen outside it (PauseGate). */
      mutable std::mutex Mutex;

      /* Serializes PausePlayer() and UnpausePlayer(), which park waiting for a player to
         acknowledge a pause.  Taken before Mutex and never by a commit, so parking under it holds
         up only other pausers (#657). */
      Indy::Fiber::TFiberLock PauseGate;

      /* A mapping from parent point of view's id to its running tetris player.
         A particular parent pov will appear in this map only after at least one call to Join() has named it,
         and will remain in this map until all its children have been removed by calls to Part().  Therefore,
         this map contains only those (and all those) parent points of view who should be playing tetris. */
      std::unordered_map<Base::TUuid, TPlayer *> PlayerByParentPovId;

      /* The ids of the parent points of view which are currently paused. */
      std::unordered_set<Base::TUuid> PausedSet;

      /* Set (under Mutex) by StopAllPlayers() before it stops the players it snatched from the
         map.  Join() checks it so a piece that lands mid-teardown doesn't spawn a fresh player that
         nobody would ever stop. */
      bool Stopping;

      /* The number of players constructed but not yet destroyed.  A player whose last child parted
         leaves the map immediately but keeps running until its Main() finishes the round and
         self-destructs; StopAllPlayers() spins this count down to zero so no player fiber can still
         be touching povs (or us) when our caller's destructor starts tearing them down (#280). */
      std::atomic<size_t> LivePlayerCount;

      /* #769: see HaltPromotion.  A player counts itself in PlayingCount before it checks
         Halted, and HaltPromotion sets Halted before it reads PlayingCount (both seq_cst), so
         once HaltPromotion has seen the count at zero no round can start. */
      std::atomic<bool> Halted {false};
      std::atomic<size_t> PlayingCount {0UL};

      /* Covers 'IsMaster'.  Only ever taken under Mutex. */
      bool IsMaster;
      std::mutex MasterMutex;

    };  // TTetrisManager

  }  // Server

}  // Orly
