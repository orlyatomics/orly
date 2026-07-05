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

#include <thread>

#include <base/debug_log.h>
#include <orly/indy/disk/util/volume_manager.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy;
using namespace Orly::Server;

bool TTetrisManager::IsPlayerPaused(const TUuid &parent_pov_id) const {
  //lock_guard<mutex> lock(Mutex);
  Fiber::TFiberLock::TLock lock(FiberMutex);
  return PausedSet.find(parent_pov_id) != PausedSet.end();
}

void TTetrisManager::Join(const TUuid &parent_pov_id, const TUuid &child_pov_id) {
  /* Lock the map and locate (or create) the slot where this parent pov's player would be. */
  //lock_guard<mutex> lock(Mutex);
  Fiber::TFiberLock::TLock lock(FiberMutex);
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
      //lock_guard<mutex> lock(MasterLock);
      Fiber::TFiberLock::TLock master_lock(MasterFiberMutex);
      player = NewPlayer(parent_pov_id, child_pov_id, PausedSet.find(parent_pov_id) != PausedSet.end(), IsMaster);
    } catch (...) {
      PlayerByParentPovId.erase(iter);
      throw;
    }
  }
}

void TTetrisManager::Part(const TUuid &parent_pov_id, const TUuid &child_pov_id) {
  /* Lock the map and locate the slot where this parent pov's player would be. */
  //lock_guard<mutex> lock(Mutex);
  Fiber::TFiberLock::TLock lock(FiberMutex);
  auto iter = PlayerByParentPovId.find(parent_pov_id);
  if (iter != PlayerByParentPovId.end()) {
    /* We found the player, so part the child from it. */
    if (!iter->second->Part(child_pov_id)) {
      /* This was the parent's last child, so we'll let it self-destruct quietly in another thread
         and remove it from the map.  The next time we try to join to this parent pov, we'll launch
         another player, even if the old one is still in the process of self-destructing. */
      iter->second->OnClose();
      PlayerByParentPovId.erase(iter);
    }
  }
}

void TTetrisManager::PausePlayer(const TUuid &parent_pov_id) {
  //lock_guard<mutex> lock(Mutex);
  Fiber::TFiberLock::TLock lock(FiberMutex);
  PausedSet.insert(parent_pov_id);
  auto iter = PlayerByParentPovId.find(parent_pov_id);
  if (iter != PlayerByParentPovId.end()) {
    iter->second->Pause();
  }
}

void TTetrisManager::UnpausePlayer(const TUuid &parent_pov_id) {
  //lock_guard<mutex> lock(Mutex);
  Fiber::TFiberLock::TLock lock(FiberMutex);
  if (PausedSet.erase(parent_pov_id)) {
    auto iter = PlayerByParentPovId.find(parent_pov_id);
    if (iter != PlayerByParentPovId.end()) {
      iter->second->Unpause();
    }
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

void TTetrisManager::TPlayer::Pause() {
  /* Flag the player and park this fiber until Main() acknowledges: TSafeSync
     holds the waiting frame and re-activates it on Complete() (#376). */
  Paused = true;
  PausedSync.WaitForMore(1);
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
  ChildCount = 0;
  /* Wake the player in case it is still waiting for permission to work. */
  CanWork.Push();
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
  assert(Unpaused);
  /* Signal the player thread to continue. */
  UnpausedSync.Complete();
  Unpaused = false;
  //Unpaused->Push();
  //Unpaused = nullptr;
}

void TTetrisManager::TPlayer::OnClose() {
  CanWork.Push();
}

TTetrisManager::TPlayer::~TPlayer() {
  Fiber::FreeMyFrame(FramePool);
  /* Last: once this count drops, StopAllPlayers() may return and the manager's owner may start
     destroying povs, so nothing after this line may touch shared state. */
  --(TetrisManager->LivePlayerCount);
}

TTetrisManager::TPlayer::TPlayer(TTetrisManager *tetris_manager)
    : TetrisManager(tetris_manager), ChildCount(1), StopFlag(nullptr), Paused(false), Unpaused(false) {
  assert(tetris_manager);
  assert(Fiber::TFrame::LocalFramePool);
  ++(tetris_manager->LivePlayerCount);
  FramePool = Fiber::TFrame::LocalFramePool;
  TetrisFrame = FramePool->Alloc();
}

void TTetrisManager::TPlayer::Start(bool is_paused, bool is_master) {
  if (is_master) {
    CanWork.Push();
  }
  if (is_paused) {
    //TEventSemaphore sem;
    //Paused = &sem;
    Paused = true;
    PausedSync.WaitForMore(1);
    try {
      TetrisFrame->Latch(&TetrisManager->FiberScheduler, this, static_cast<Fiber::TRunnable::TFunc>(&TTetrisManager::TPlayer::Main));
    } catch (...) {
      FramePool->Free(TetrisFrame);
      TetrisFrame = nullptr;
      throw;
    }
    /* Park until the newly latched Main() acknowledges the pause -- the
       frame-parking re-activation queue the 2014 stub wished for is what
       TSafeSync is (#376). */
    PausedSync.Sync();
  } else {
    try {
      TetrisFrame->Latch(&TetrisManager->FiberScheduler, this, static_cast<Fiber::TRunnable::TFunc>(&TTetrisManager::TPlayer::Main));
    } catch (...) {
      FramePool->Free(TetrisFrame);
      TetrisFrame = nullptr;
      throw;
    }
  }
}

void TTetrisManager::TPlayer::Main() {
  try {
    //DEBUG_LOG("tetris player %p: entering Main()", this);
    /* wait to see if we're allowed to play tetris. This will trigger if we're master, if we just became master, or if this player should be destroyed. */
    CanWork.Pop();
    for (;;) {
      if (Paused) {
        PausedSync.Complete();
        Paused = false;
        DEBUG_LOG("tetris player %p: pausing", this);
        OnPause();
        Unpaused = true;
        UnpausedSync.WaitForMore(1);
        DEBUG_LOG("tetris player %p: paused", this);
        UnpausedSync.Sync();
        DEBUG_LOG("tetris player %p: unpausing", this);
        OnUnpause();
        DEBUG_LOG("tetris player %p: unpaused", this);
      } else if (ChildCount) {
        //DEBUG_LOG("tetris player %p: playing tetris; child_count = %ld", this, ChildCount);
        Play();
        if (usleep(0) < 0) {
          DEBUG_LOG("tetris player %p: signal detected", this);
          break;
        }
      } else {
        break;
      }
    }
    //DEBUG_LOG("tetris player %p: self-destructing", this);
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
    : Scheduler(scheduler), FiberScheduler(runner_cons), Stopping(false), LivePlayerCount(0UL), IsMaster(is_master) {
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
     to die -- waiting while holding FiberMutex deadlocks the pair (#280).  Once 'Stopping' is
     set, Join() stops spawning players, so the map stays empty for our caller's destructor. */
  std::unordered_map<TUuid, TPlayer *> players;
  /* extra */ {
    Fiber::TFiberLock::TLock lock(FiberMutex);
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
  //lock_guard<mutex> lock(Mutex);
  Fiber::TFiberLock::TLock lock(FiberMutex);
  //lock_guard<mutex> master_lock(MasterLock);
  Fiber::TFiberLock::TLock master_lock(MasterFiberMutex);
  IsMaster = true;
  for (const auto &item: PlayerByParentPovId) {
    item.second->BecomeMaster();
  }
}
