/* <orly/server/tetris_manager.cc>

   Implements <orly/server/tetris_manager.h>.

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

#include <orly/server/tetris_manager.h>

#include <new>
#include <thread>

#include <base/debug_log.h>
#include <orly/indy/disk/util/volume_manager.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy;
using namespace Orly::Server;

bool TTetrisManager::IsPlayerPaused(const TUuid &parent_pov_id) const {
  lock_guard<mutex> lock(Mutex);
  return PausedSet.find(parent_pov_id) != PausedSet.end();
}

void TTetrisManager::Join(const TUuid &parent_pov_id, const TUuid &child_pov_id) {
  /* Lock the map and locate (or create) the slot where this parent pov's player would be. */
  lock_guard<mutex> lock(Mutex);
  if (Stopping) {
    /* We're tearing down: no new players.  The piece stays unpromoted, exactly as if it had
       arrived just after the player it would have joined was stopped. */
    return;
  }
  auto iter = PlayerByParentPovId.insert(pair<TUuid, TPlayer *>(parent_pov_id, nullptr)).first;
  TPlayer *&player = iter->second;
  if (player) {
    /* The player for this parent pov already exists, so join the child to it. */
    player->Join(child_pov_id);
  } else {
    /* The player for this parent pov doesn't exist, so create one, starting with this child. */
    try {
      lock_guard<mutex> master_lock(MasterMutex);
      player = NewPlayer(parent_pov_id, child_pov_id, PausedSet.find(parent_pov_id) != PausedSet.end(), IsMaster);
    } catch (...) {
      PlayerByParentPovId.erase(iter);
      throw;
    }
  }
}

void TTetrisManager::Part(const TUuid &parent_pov_id, const TUuid &child_pov_id) {
  /* Lock the map and locate the slot where this parent pov's player would be. */
  lock_guard<mutex> lock(Mutex);
  auto iter = PlayerByParentPovId.find(parent_pov_id);
  if (iter != PlayerByParentPovId.end()) {
    /* We found the player, so part the child from it.  Once its child count reaches zero the
       player may free itself at any moment, so we keep it alive (ToucherCount) until we are done
       with it (#636). */
    TPlayer *player = iter->second;
    ++(player->ToucherCount);
    if (!player->Part(child_pov_id)) {
      /* This was the parent's last child, so we'll let it self-destruct quietly in another thread
         and remove it from the map.  The next time we try to join to this parent pov, we'll launch
         another player, even if the old one is still in the process of self-destructing. */
      PlayerByParentPovId.erase(iter);
      player->OnClose();
    }
    /* Last touch. */
    --(player->ToucherCount);
  }
}

void TTetrisManager::PausePlayer(const TUuid &parent_pov_id) {
  Fiber::TFiberLock::TLock gate(PauseGate);
  /* Ask under Mutex, but wait for the answer outside it (#657).  The player may be partway
     through a round whose commits join or part children, which takes Mutex: waiting for the
     player while holding Mutex deadlocked the pair, with the player's commit holding the
     replication queue lock, so every other commit stopped too.  BeginImport() pauses the global
     pov's player this way. */
  TPlayer *player = nullptr;
  /* extra */ {
    lock_guard<mutex> lock(Mutex);
    PausedSet.insert(parent_pov_id);
    auto iter = PlayerByParentPovId.find(parent_pov_id);
    if (iter != PlayerByParentPovId.end()) {
      player = iter->second;
      /* Keep it alive while we wait: its last child may part meanwhile.  It still acknowledges
         the pause first, because Main() tests Paused before ChildCount. */
      ++(player->ToucherCount);
      player->RequestPause();
    }
  }
  if (player) {
    player->AwaitPause();
    --(player->ToucherCount);
  }
}

void TTetrisManager::UnpausePlayer(const TUuid &parent_pov_id) {
  Fiber::TFiberLock::TLock gate(PauseGate);
  TPlayer *player = nullptr;
  /* extra */ {
    lock_guard<mutex> lock(Mutex);
    if (PausedSet.erase(parent_pov_id)) {
      auto iter = PlayerByParentPovId.find(parent_pov_id);
      if (iter != PlayerByParentPovId.end()) {
        player = iter->second;
        ++(player->ToucherCount);
      }
    }
  }
  if (player) {
    /* A player constructed paused (a child joined a paused parent) no longer waits for its first
       round to acknowledge the pause, so it may not have yet (#657).  Wait for that before
       unpausing.  A player PausePlayer() paused has acknowledged already: this returns at once. */
    player->AwaitPause();
    player->Unpause();
    --(player->ToucherCount);
  }
}

void TTetrisManager::TPlayer::Join(const Base::TUuid &child_pov_id) {
  ++ChildCount;
  OnJoin(child_pov_id);
}

bool TTetrisManager::TPlayer::Part(const Base::TUuid &child_pov_id) {
  bool result = --ChildCount;
  if (result) {
    OnPart(child_pov_id);
  }
  return result;
}

void TTetrisManager::TPlayer::RequestPause() {
  PausedSync.WaitForMore(1);
  Paused = true;
}

void TTetrisManager::TPlayer::AwaitPause() {
  /* TSafeSync holds the waiting frame and re-activates it on Complete() (#376); if Main() has
     already completed it, this returns at once. */
  PausedSync.Sync();
}

void TTetrisManager::TPlayer::Stop() {
  assert(!StopFlag.load());
  /* Park a flag from our own stack where Main() can find it, then zero the child count so
     Main() falls out of its loop and self-destructs.  Main() flips the flag through a stack
     copy of the pointer after 'delete this' completes, so we never dereference 'this' once
     the count hits zero and the player never touches our stack after the flip. */
  std::atomic<bool> stopped(false);
  StopFlag.store(&stopped);
  /* Keep ourselves alive across the zeroing and the push: Main() frees us once the count is zero
     and ToucherCount has drained (#636). */
  ++ToucherCount;
  ChildCount = 0;
  /* Wake the player in case it is still waiting for permission to work, or is paused (#657). */
  CanWork.Push();
  PauseWake.Push();
  --ToucherCount;
  /* Wait for the player fiber to finish self-destructing.  It runs on the manager's fiber
     scheduler, a different thread, so yielding here cannot starve it.  Server shutdown tears
     us down from a plain thread, so only fiber-yield when we actually are a fiber. */
  while (!stopped.load()) {
    if (Fiber::TFrame::LocalFrame) {
      Fiber::YieldSlow();
    } else {
      std::this_thread::yield();
    }
  }
}

void TTetrisManager::TPlayer::Unpause() {
  assert(PauseHeld);
  /* Signal the player thread to continue. */
  PauseHeld = false;
  PauseWake.Push();
}

void TTetrisManager::TPlayer::OnClose() {
  CanWork.Push();
  /* A paused player whose last child just parted must still exit (#657). */
  PauseWake.Push();
}

TTetrisManager::TPlayer::~TPlayer() {
  Fiber::FreeMyFrame(FramePool);
  /* Last: once this count drops, StopAllPlayers() may return and the manager's owner may start
     destroying povs, so nothing after this line may touch shared state. */
  --(TetrisManager->LivePlayerCount);
}

TTetrisManager::TPlayer::TPlayer(TTetrisManager *tetris_manager)
    : TetrisManager(tetris_manager), ChildCount(1), ToucherCount(0UL), StopFlag(nullptr), Paused(false), PauseHeld(false) {
  assert(tetris_manager);
  /* Take the frame from our manager's pool, not TFrame::LocalFramePool: we may be running on a
     thread that has no pool of its own (#633, WsRunner during `unpause`).  See PlayerFramePool. */
  FramePool = Base::AssertTrue(tetris_manager->PlayerFramePool.get());
  /* extra */ {
    std::lock_guard<std::mutex> lock(tetris_manager->PlayerFrameMutex);
    TetrisFrame = FramePool->Alloc();
  }
  /* Count ourselves only once nothing else here can throw: a throwing constructor never runs
     the destructor that would drop the count again, and StopAllPlayers() would wait forever. */
  ++(tetris_manager->LivePlayerCount);
}

void TTetrisManager::TPlayer::Start(bool is_paused, bool is_master) {
  if (is_master) {
    CanWork.Push();
  }
  /* Born paused: Main() sees the request before it plays a round.  We used to park here until
     Main() acknowledged it, but we run inside Join(), under the manager's Mutex and usually inside
     the joining child's commit, and Main() runs on the Tetris runner, whose thread another
     player's commit can be holding blocked on the replication queue lock that our commit holds
     (#657).  Whoever unpauses waits for the acknowledgement instead (UnpausePlayer()). */
  if (is_paused) {
    RequestPause();
  }
  try {
    TetrisFrame->Latch(&TetrisManager->FiberScheduler, this, static_cast<Fiber::TRunnable::TFunc>(&TTetrisManager::TPlayer::Main));
  } catch (...) {
    FramePool->Free(TetrisFrame);
    TetrisFrame = nullptr;
    throw;
  }
}

void TTetrisManager::TPlayer::Main() {
  try {
    //DEBUG_LOG("tetris player %p: entering Main()", this);
    /* wait to see if we're allowed to play tetris. This will trigger if we're master, if we just became master, or if this player should be destroyed. */
    CanWork.Pop();
    for (;;) {
      if (Paused) {
        /* Ready the unpause before acknowledging the pause: once AwaitPause() returns, an
           UnpausePlayer() may call Unpause() at once (#657). */
        Paused = false;
        PauseHeld = true;
        PausedSync.Complete();
        DEBUG_LOG("tetris player %p: pausing", this);
        OnPause();
        DEBUG_LOG("tetris player %p: paused", this);
        /* Stay paused until Unpause(), or until there is nothing left to play for: our last child
           parted, or we are being stopped.  Neither of those can reach a sleeping paused player any
           other way (#657). */
        while (PauseHeld && ChildCount) {
          PauseWake.Pop();
        }
        DEBUG_LOG("tetris player %p: unpausing", this);
        OnUnpause();
        DEBUG_LOG("tetris player %p: unpaused", this);
      } else if (ChildCount) {
        //DEBUG_LOG("tetris player %p: playing tetris; child_count = %ld", this, ChildCount);
        try {
          Play();
        } catch (const std::bad_alloc &) {
          /* Out of pool space mid-round (#584). Nothing was committed: a
             transaction destroyed without CommitAction discards its pushes,
             and the child keeps its peeked update for the next round. Before
             this, the exception killed the player fiber silently and its POV
             never promoted again. Yield (below) so the merges and the layer
             cleaner can free space, then play again. */
          /* Since #607 a pool miss on a fiber fails at once, without waiting, so this can come
             round on every round until the merges free space. Log the 1st, 2nd, 4th, ... */
          static std::atomic<size_t> pool_misses(0UL);
          const size_t misses = ++pool_misses;
          if ((misses & (misses - 1UL)) == 0UL) {
            syslog(LOG_ERR, "tetris player %p: out of pool space; retrying the round (%ld times so far)", static_cast<void *>(this), misses);
          }
        }
        /* Let other fibers on this runner run between rounds (#584). Without
           this the player monopolized the runner, and a fiber that hopped here
           -- the repo layer cleaner, visiting every runner to drop a dead
           file's caches -- never ran again, so no dead layer was ever freed.
           It must be YieldSlow: a plain Yield re-queues locally, and the
           runner keeps draining its local queue without ever taking in frames
           handed over from other runners (TRunner::Run). */
        Fiber::YieldSlow();
        if (usleep(0) < 0) {
          DEBUG_LOG("tetris player %p: signal detected", this);
          break;
        }
      } else {
        break;
      }
    }
    //DEBUG_LOG("tetris player %p: self-destructing", this);
    /* Our last child may have been parted (or we were stopped) by another thread, which drops
       our child count to zero and then still touches us: OnClose() and Stop() push CanWork.
       Seeing the zero count we would otherwise free ourselves, and close CanWork's eventfd, under
       its feet (#636: pausing a player's only child from WsRunner threw "Bad file descriptor"
       out of the NO_THROW commit path).  Those callers bracket the whole thing with
       ToucherCount, so wait for it to drain, as do PausePlayer() and UnpausePlayer() while they
       wait on us outside the manager's Mutex (#657).  Spin with YieldSlow, so other frames on
       this runner -- a pauser's re-activation among them -- keep running. */
    while (ToucherCount.load()) {
      Fiber::YieldSlow();
    }
    /* If a Stop() is waiting on us, its stack flag must flip only after we are completely
       dead.  Copy the pointer to our stack first; 'this' is invalid after the delete. */
    std::atomic<bool> *stop_flag = StopFlag.load();
    delete this;
    if (stop_flag) {
      stop_flag->store(true);
    }
    //DEBUG_LOG("tetris player %p: exiting Main()", this);
  } catch (const std::exception &ex) {
    syslog(LOG_EMERG, "TetrisManager::Player exception: %s", ex.what());
    throw;
  }
}

void TTetrisManager::TPlayer::BecomeMaster() {
  CanWork.Push();
}

TTetrisManager::TTetrisManager(Base::TScheduler *scheduler,
                               Orly::Indy::Fiber::TRunner::TRunnerCons &runner_cons,
                               Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
                               const std::function<void (Indy::Fiber::TRunner *)> &runner_setup_cb,
                               bool is_master)
    : Scheduler(scheduler), FiberScheduler(runner_cons),
      PlayerFramePool(std::make_unique<Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *>::TThreadLocalPool>(
          Base::AssertTrue(frame_pool_manager))),
      Stopping(false), LivePlayerCount(0UL), IsMaster(is_master) {
  assert(scheduler);
  Base::TEventSemaphore setup_is_complete;
  auto launch_sched = [this, runner_setup_cb, &setup_is_complete](Fiber::TRunner *runner,
                                                                  Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager) {
    if (!Fiber::TFrame::LocalFramePool) {
      Fiber::TFrame::LocalFramePool = new Base::TThreadLocalGlobalPoolManager<Fiber::TFrame, size_t, Fiber::TRunner *>::TThreadLocalPool(frame_pool_manager);
      FramePool = Fiber::TFrame::LocalFramePool;
    }
    runner_setup_cb(runner);
    setup_is_complete.Push();
    runner->Run();
    delete FramePool;
  };
  FiberThread = std::make_unique<std::thread>(std::bind(launch_sched, &FiberScheduler, frame_pool_manager));
  setup_is_complete.Pop();
}

TTetrisManager::~TTetrisManager() {
  assert(PlayerByParentPovId.empty());
  FiberScheduler.ShutDown();
  assert(FiberThread);
  assert(FiberThread->get_id() != std::this_thread::get_id());
  FiberThread->join();
}

void TTetrisManager::StopAllPlayers() {
  /* Snatch the whole player map under the lock, but do the actual stopping after releasing it:
     a player that is mid-Play() may be re-entering the manager right now (promotion calls back
     into Join()/Part(), e.g. from indy/repo.cc AppendUpdate), and Stop() waits for that player
     to die -- waiting while holding Mutex deadlocks the pair (#280).  Once 'Stopping' is
     set, Join() stops spawning players, so the map stays empty for our caller's destructor. */
  std::unordered_map<TUuid, TPlayer *> players;
  /* extra */ {
    lock_guard<mutex> lock(Mutex);
    Stopping = true;
    players.swap(PlayerByParentPovId);
  }
  for (const auto &item: players) {
    item.second->Stop();
  }
  /* Also wait out the players that already left the map on their own: a player whose last child
     parted is erased immediately but its fiber can still be finishing the round, touching povs our
     caller is about to destroy.  Their destructors drop this count as their final shared-state
     touch, so once it reads zero no player can interfere with the teardown. */
  while (LivePlayerCount.load()) {
    if (Fiber::TFrame::LocalFrame) {
      Fiber::YieldSlow();
    } else {
      std::this_thread::yield();
    }
  }
}

void TTetrisManager::BecomeMaster() {
  lock_guard<mutex> lock(Mutex);
  lock_guard<mutex> master_lock(MasterMutex);
  IsMaster = true;
  for (const auto &item: PlayerByParentPovId) {
    item.second->BecomeMaster();
  }
}
