/* <orly/indy/transaction_base.cc>

   Implements <orly/indy/transaction_base.h>.

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

#include <orly/indy/transaction_base.h>
#include <algorithm>
#include <optional>
#include <sstream>
#include <thread>

#include <syslog.h>

#include <base/debug_log.h>
#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
#include <orly/indy/disk/wal.h>
#include <orly/indy/wal_retention.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/replication.h>

using namespace std;
using namespace Base;
using namespace Orly::Atom;
using namespace Orly::Indy::L1;

/* Reserve WAL room for a record (#891).  With no give_up, refuse at once if there is none.  With
   one, keep trying until there is room or give_up says to stop: room comes from checkpoints, which
   run on their own thread and don't wait on the caller.  A fiber must not block its runner, so on
   one we only ever ask without waiting, and yield between asks with a short, growing pause so the
   loop doesn't spin. */
static Orly::Indy::Disk::TWalReservation ReserveWalRoom(
    Orly::Indy::Disk::TWal &wal, size_t size, Orly::Indy::Disk::TWal::TRoom room, const std::function<bool ()> &give_up,
    size_t credit, const std::vector<Orly::Indy::Disk::TPromotionDebt> &owes) {
  using Orly::Indy::Disk::TWalRingFull;
  if (!give_up) {
    return wal.Reserve(size, room, chrono::milliseconds(0), credit, owes);
  }
  const bool on_fiber = Orly::Indy::Fiber::TFrame::LocalFrame != nullptr;
  constexpr auto max_pause = chrono::milliseconds(2);
  chrono::microseconds pause(50);
  for (uint64_t attempt = 1;; ++attempt) {
    try {
      return wal.Reserve(size, room, on_fiber ? chrono::milliseconds(0) : chrono::milliseconds(100), credit, owes);
    } catch (const TWalRingFull &ex) {
      if (give_up()) {
        throw;
      }
      /* The 1st, 2nd, 4th, 8th... refusal: enough to show it is stuck without flooding the log. */
      if (!(attempt & (attempt - 1))) {
        syslog(LOG_WARNING, "TTransaction: waiting for WAL room for a %zu-byte record (refused %lu times so far): %s", size, attempt, ex.what());
      }
    }
    if (on_fiber) {
      Orly::Indy::Fiber::YieldSlow();
      this_thread::sleep_for(pause);
      pause = min<chrono::microseconds>(pause * 2, max_pause);
    }
  }
}

/* The promotions that will write a WAL record for an update pushed into `repo` on its way to a root
   (#959): one for each step up from a safe repo.  Once it reaches a repo that isn't safe, no pin
   holds the head for it, so later steps owe nothing. */
static size_t CountPromotionsAhead(const Orly::Indy::L0::TManager::TRepo *repo) {
  size_t levels = 0UL;
  for (const auto *cur = repo; cur && cur->IsSafeRepo();) {
    const auto &parent = cur->GetParentRepo();
    if (!(parent && *parent)) {
      break;
    }
    ++levels;
    cur = parent->operator->();
  }
  return levels;
}

/* Test-only; empty in production. See header. */
std::function<void ()> TTransaction::OnCommitBetweenPushAndPopForTest;

/* Whether a replicated status change (pause, unpause, fail) lines up with our copy of the repo.
   follow_or_discard is the master's lowest unpromoted sequence number when the change committed;
   unset means the change is local and always applies.  EmptyRepoSequenceNumber means the master's
   repo had nothing pending, so ours must be empty too (#655): dereferencing our empty lowest
   sequence number, as this used to, is undefined.  A repo whose lowest sequence number differs
   (either side empty, or a different number) is at a different point in the stream: discard. */
static bool FollowsStatusChange(const Orly::Indy::L0::TManager::TPtr<Orly::Indy::TRepo> &repo, const std::optional<Orly::Indy::TSequenceNumber> &follow_or_discard) {
  if (!follow_or_discard) {
    return true;
  }
  const std::optional<Orly::Indy::TSequenceNumber> &start = repo->GetSequenceNumberStart();
  if (*follow_or_discard == Orly::Indy::EmptyRepoSequenceNumber) {
    return !start;
  }
  /* Never behind the master: that would mean we lost updates it had. */
  assert(!start || *start >= *follow_or_discard);
  return start && *start == *follow_or_discard;
}

/* Whether a replicated or replayed pause or unpause would change nothing: the repo already has
   that status.  Recovery replays a pause twice when the WAL holds both the pause and the copy of
   it the WAL wrote to keep it (#961), and an unpause whose pause the ring no longer holds; the
   second pause (or the unpause of a repo that was never paused here) is discarded rather than
   reaching TRepo::ChangeStatus, which takes a repo only to a different status.  A local change
   (no follow_or_discard) is the caller's to get right, as before. */
static bool AlreadyHasStatus(const Orly::Indy::L0::TManager::TPtr<Orly::Indy::TRepo> &repo, const std::optional<Orly::Indy::TSequenceNumber> &follow_or_discard,
                             Orly::Indy::TStatus status) {
  return follow_or_discard && repo->GetStatus() == status;
}

bool TTransaction::Push(const L0::TManager::TPtr<TRepo> &repo, const shared_ptr<TUpdate> &update, const std::optional<TSequenceNumber> &ensure_or_discard,
                        const Base::TUuid &promoted_from) {
  Prepared = false;
  EnsureOrDiscard = EnsureOrDiscard || ensure_or_discard;
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  if (mutation) {
    assert(false);  // Cannot attach a Pusher to a repo with an existing mutation
    throw std::runtime_error("Cannot attach a Pusher to a repo with an existing mutation.");
  }

  assert (!ensure_or_discard || (repo->GetNextSequenceNumber() >= *ensure_or_discard));
  if (!ensure_or_discard || (repo->GetNextSequenceNumber() == *ensure_or_discard)) {
    new TPusher(this, repo, update, promoted_from);
    return true;
  } else if (ensure_or_discard && repo->GetNextSequenceNumber() < *ensure_or_discard) {
    syslog(LOG_ERR, "MAJOR ERROR: missing data! ensure_or_discard =[%ld] vs. GetNextSequenceNumber =[%ld]", *ensure_or_discard, repo->GetNextSequenceNumber());
    throw std::logic_error("Let's check what we're doing here in transaction::push()");
  } else if (ensure_or_discard) {
    syslog(LOG_DEBUG, "TTransaction::Push: discarding [%ld]", *ensure_or_discard);
  }
  return false;
}

bool TTransaction::Pop(const L0::TManager::TPtr<TRepo> &repo, const std::optional<TSequenceNumber> &ensure_or_discard) {
  Prepared = false;
  EnsureOrDiscard = EnsureOrDiscard || ensure_or_discard;
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  if (!mutation) {
    assert (!ensure_or_discard || (!repo->GetSequenceNumberStart() || *(repo->GetSequenceNumberStart()) >= *ensure_or_discard));
    if (!ensure_or_discard || (repo->GetSequenceNumberStart() && *(repo->GetSequenceNumberStart()) == *ensure_or_discard)) {
      new TPopper(this, repo, TPopper::Pop);
      return true;
    } else if (ensure_or_discard) {
      syslog(LOG_DEBUG, "TTransaction::Pop: discarding [%ld]", *ensure_or_discard);
    }
  } else {
    switch (mutation->GetKind()) {
      case TMutation::Pusher : {
        assert(false);  // Cannot attach a Popper and Pusher to the same repo
        throw std::runtime_error("Cannot attach a Popper and Pusher to the same repo.");
        break;
      };
      case TMutation::Popper : {
        /* We've already got a popper, make sure it's in the right state. */
        TPopper &popper = *dynamic_cast<TPopper *>(mutation);
        switch (popper.GetState()) {
          case TPopper::Peek : {
            assert (!ensure_or_discard || (!repo->GetSequenceNumberStart() || *(repo->GetSequenceNumberStart()) >= *ensure_or_discard));
            if (!ensure_or_discard || (repo->GetSequenceNumberStart() && *(repo->GetSequenceNumberStart()) == *ensure_or_discard)) {
              popper.SetState(TPopper::Pop);
              return true;
            }
            break;
          }
          case TPopper::Pop : {
            assert (!ensure_or_discard || (!repo->GetSequenceNumberStart() || *(repo->GetSequenceNumberStart()) >= *ensure_or_discard));
            if (!ensure_or_discard || (repo->GetSequenceNumberStart() && *(repo->GetSequenceNumberStart()) == *ensure_or_discard)) {
              return true;
            }
            /* ensure_or_discard is set but the repo's sequence start does not match it. The assert
               above guarantees start >= ensure, so the repo has advanced past the requested point
               (or has no start yet): this Pop is stale. Discard it -- leave the existing popper as it
               is and fall through to return false, exactly as the no-mutation discard path above does. */
            break;
          }
          case TPopper::Fail : {
            assert (!ensure_or_discard || (!repo->GetSequenceNumberStart() || *(repo->GetSequenceNumberStart()) >= *ensure_or_discard));
            if (!ensure_or_discard || (repo->GetSequenceNumberStart() && *(repo->GetSequenceNumberStart()) == *ensure_or_discard)) {
              popper.SetState(TPopper::Pop);
              return true;
            }
            /* Stale Pop (sequence start past ensure_or_discard): discard it and leave the popper in
               Fail rather than promoting it to Pop. Fall through to return false. */
            break;
          }
        }
        break;
      };
      case TMutation::StatusChanger : {
        assert(false);  // Cannot attach a Popper and StatusChanger to the same repo
        throw std::runtime_error("Cannot attach a Popper and StatusChanger to the same repo.");
        break;
      };
    }
  }
  return false;
}

bool TTransaction::Fail(const L0::TManager::TPtr<TRepo> &repo, const std::optional<TSequenceNumber> &follow_or_discard) {
  Prepared = false;
  EnsureOrDiscard = EnsureOrDiscard || follow_or_discard;
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  if (!mutation) {
    if (FollowsStatusChange(repo, follow_or_discard)) {
      new TPopper(this, repo, TPopper::Fail);
      return true;
    }
  } else {
    switch (mutation->GetKind()) {
      case TMutation::Pusher : {
        assert(false);  // Cannot attach a Popper and Pusher to the same repo
        throw std::runtime_error("Cannot attach a Popper and Pusher to the same repo.");
        break;
      };
      case TMutation::Popper : {
        /* We've already got a popper, make sure it's in the right state. */
        TPopper &popper = *dynamic_cast<TPopper *>(mutation);
        switch (popper.GetState()) {
          case TPopper::Peek : {
            if (FollowsStatusChange(repo, follow_or_discard)) {
              popper.SetState(TPopper::Fail);
              return true;
            }
            /* Stale Fail (sequence start past follow_or_discard): discard it and leave the popper in
               Peek rather than transitioning to Fail. Fall through to return false. */
            break;
          }
          case TPopper::Pop : {
            if (FollowsStatusChange(repo, follow_or_discard)) {
              popper.SetState(TPopper::Fail);
              return true;
            }
            /* Stale Fail (sequence start past follow_or_discard): discard it and leave the popper in
               Pop rather than transitioning to Fail. Fall through to return false. */
            break;
          }
          case TPopper::Fail : {
            break;
          }
        }
        break;
      };
      case TMutation::StatusChanger : {
        assert(false);  // Cannot attach a Popper and StatusChanger to the same repo
        throw std::runtime_error("Cannot attach a Popper and StatusChanger to the same repo.");
        break;
      };
    }
  }
  return false;
}

bool TTransaction::Pause(const L0::TManager::TPtr<TRepo> &repo, const std::optional<TSequenceNumber> &follow_or_discard) {
  Prepared = false;
  EnsureOrDiscard = EnsureOrDiscard || follow_or_discard;
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  if (mutation) {
    assert(false);  // Cannot attach a StatusChanger to a repo with an existing mutation
    throw std::runtime_error("Cannot attach a StatusChanger to a repo with an existing mutation.");
  }
  if (FollowsStatusChange(repo, follow_or_discard) && !AlreadyHasStatus(repo, follow_or_discard, Paused)) {
    /* Let any Tetris round that has already peeked this repo finish promoting it before the
       pause can commit, and keep new rounds from peeking it (#636; see TRepo::BeginPause).  We
       wait here, before the commit, because the round's own commit needs the replication queue
       lock our commit will hold.  A round is short, and on a slave no round runs at all. */
    repo->BeginPause();
    try {
      while (repo->IsHeldForPromotion()) {
        if (Fiber::TFrame::LocalFrame) {
          Fiber::YieldSlow();
        } else {
          std::this_thread::yield();
        }
      }
      auto *status_changer = new TStatusChanger(this, repo, Paused);
      status_changer->HoldsPause = true;
    } catch (...) {
      repo->EndPause();
      throw;
    }
    return true;
  }
  return false;
}

bool TTransaction::UnPause(const L0::TManager::TPtr<TRepo> &repo, const std::optional<TSequenceNumber> &follow_or_discard) {
  Prepared = false;
  EnsureOrDiscard = EnsureOrDiscard || follow_or_discard;
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  if (mutation) {
    assert(false);  // Cannot attach a StatusChanger to a repo with an existing mutation
    throw std::runtime_error("Cannot attach a StatusChanger to a repo with an existing mutation.");
  }
  if (FollowsStatusChange(repo, follow_or_discard) && !AlreadyHasStatus(repo, follow_or_discard, Normal)) {
    new TStatusChanger(this, repo, Normal);
    return true;
  }
  return false;
}

const std::shared_ptr<Orly::Indy::TUpdate> &TTransaction::Peek(const L0::TManager::TPtr<TRepo> &repo) {
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  TPopper *popper = nullptr;
  if (mutation) {
    switch (mutation->GetKind()) {
      case TMutation::Pusher : {
        assert(false);  // Cannot attach a Popper and Pusher to the same repo
        throw std::runtime_error("Cannot attach a Popper and Pusher to the same repo.");
        break;
      };
      case TMutation::Popper : {
        /* We've already got a popper, and peek is not an end state. */
        popper = dynamic_cast<TPopper *>(mutation);
        break;
      };
      case TMutation::StatusChanger : {
        assert(false);  // Cannot attach a Popper and StatusChanger to the same repo
        throw std::runtime_error("Cannot attach a Popper and StatusChanger to the same repo.");
        break;
      };
    }
  } else {
    popper = NewPeeker(repo);
    if (!popper) {
      /* A pause of this repo is pending or under way (#636): nothing to promote. */
      static const std::shared_ptr<TUpdate> no_update;
      return no_update;
    }
  }
  popper->DoPeek();
  const auto &ret = popper->GetUpdate();
  return ret;
}

bool TTransaction::HoldForPromotion(const L0::TManager::TPtr<TRepo> &repo) {
  TMutation *mutation = MutationCollection.TryGetFirstMember(repo->GetId());
  if (mutation) {
    if (mutation->GetKind() != TMutation::Popper) {
      assert(false);  // Cannot attach a Popper alongside a Pusher or StatusChanger
      throw std::runtime_error("Cannot attach a Popper alongside another kind of mutation on the same repo.");
    }
    return dynamic_cast<TPopper *>(mutation)->HoldsPromotion;
  }
  return NewPeeker(repo) != nullptr;
}

TTransaction::TPopper *TTransaction::NewPeeker(const L0::TManager::TPtr<TRepo> &repo) {
  if (!repo->TryHoldForPromotion()) {
    return nullptr;
  }
  TPopper *popper;
  try {
    popper = new TPopper(this, repo, TPopper::Peek);
  } catch (...) {
    repo->ReleasePromotionHold();
    throw;
  }
  popper->HoldsPromotion = true;
  return popper;
}

void TTransaction::Prepare() {
  if (!TransactionCompletion) {
    /* acquire TransactionCompletion lock */ {
      std::lock_guard<std::mutex> lock(Manager->TransactionCompletionLock);
      TransactionCompletion = new TTransactionCompletion(Manager);
    }
  }
  Replica.Reset();
  TouchesSafeRepo = false;
  SafePushes.clear();
  SafePops.clear();
  SafeStatuses.clear();
  size_t num_push = 0UL;
  std::vector<const TPusher *> safe_pushers;
  std::vector<std::pair<Base::TUuid, TSequenceNumber>> release_set;
  for (TMutationCollection::TCursor csr(&MutationCollection); csr; ++csr) {
    switch (csr->GetKind()) {
      case TMutation::Pusher : {
        const TPusher *pusher = dynamic_cast<TPusher *>(&*csr);
        pusher->MyMutation = Replica.Push(pusher->GetRepoId(), pusher->Update);

        if (pusher->Repo->IsSafeRepo()) {
          TouchesSafeRepo = true;
          ++num_push;
          /* No parent: a data file of the repo's own is where the update ends up. */
          const auto &parent = pusher->Repo->GetParentRepo();
          SafePushes.push_back(TSafePush{pusher->MyMutation, !(parent && *parent), CountPromotionsAhead(pusher->Repo.operator->())});
          /* Its persistence notification is set below, once the WAL room is reserved. */
          safe_pushers.push_back(pusher);
        }


        break;
      }
      case TMutation::Popper : {
        const TPopper *popper = dynamic_cast<TPopper *>(&*csr);
        if (popper->Repo->IsSafeRepo()) {
          TouchesSafeRepo = true;
        }
        switch (popper->State) {
          case TPopper::Peek : {
            /* Do nothing */
            break;
          }
          case TPopper::Pop : {
            assert(popper->Repo->GetSequenceNumberStart());
            popper->MyMutation = Replica.Pop(popper->GetRepoId(), popper->Repo->GetSequenceNumberStart());
            assert(popper->MyMutation->GetSequenceNumber());
            release_set.push_back(make_pair(popper->GetRepoId(), *(popper->MyMutation->GetSequenceNumber())));
            if (popper->Repo->IsSafeRepo()) {
              SafePops.emplace_back(popper->GetRepoId(), *(popper->MyMutation->GetSequenceNumber()));
            }
            break;
          }
          case TPopper::Fail : {
            popper->MyMutation = Replica.Fail(popper->GetRepoId(), popper->Repo->GetSequenceNumberStart());
            break;
          }
        }
        break;
      }
      case TMutation::StatusChanger : {
        const TStatusChanger *status_changer = dynamic_cast<TStatusChanger *>(&*csr);
        if (status_changer->Repo->IsSafeRepo()) {
          TouchesSafeRepo = true;
        }
        switch (status_changer->Status) {
          case Normal : {
            status_changer->MyMutation = Replica.UnPause(status_changer->GetRepoId(), status_changer->Repo->GetSequenceNumberStart());
            break;
          }
          case Paused : {
            status_changer->MyMutation = Replica.Pause(status_changer->GetRepoId(), status_changer->Repo->GetSequenceNumberStart());
            break;
          }
          case Failed : {
            assert(false); /* a status changer should not be failing a repo */
            throw;
            break;
          }
        }
        if (status_changer->Repo->IsSafeRepo()) {
          SafeStatuses.emplace_back(status_changer->MyMutation, status_changer->Status == Paused);
        }
        break;
      }
    }
  }
  TManager *my_manager = Manager;
  bool ensure_or_discard = EnsureOrDiscard;
  std::shared_ptr<std::function<void (TTransactionCompletion::TTransactionResult)>> func = make_shared<std::function<void (TTransactionCompletion::TTransactionResult)>>([release_set, my_manager, ensure_or_discard](TTransactionCompletion::TTransactionResult result) {
    switch (result) {
      case TTransactionCompletion::Completed: {
        for (const auto &entry : release_set) {
          /* The popped repo may be gone by now: the completion waits for the parent's update to
             reach disk, and the manager can discard an expired pov in the meantime.  Its updates
             are already in the parent, so there is nothing left to release.  ForceOpenRepo would
             construct an empty repo under the old id, which then asserts in ReleaseUpdate (or, in
             release, comes back as a resurrected pov), so only touch a repo that is live (#665). */
          auto base_repo = my_manager->TryOpenLiveRepo(entry.first);
          if (!base_repo) {
            std::ostringstream strm;
            strm << entry.first;
            syslog(LOG_INFO, "TTransaction: repo [%s] was discarded before its pop of [%ld] completed; nothing to release (#665)", strm.str().c_str(), entry.second);
            continue;
          }
          TRepo *repo = dynamic_cast<TRepo *>(base_repo.Get());
          assert(repo);
          repo->ReleaseUpdate(entry.second, ensure_or_discard);
        }
        break;
      }
      case TTransactionCompletion::Failed: {
        syslog(LOG_ERR, "Transaction failed");
        break;
      }
    }
  });
  /* Reserve the room for this transaction's WAL record before anything is applied (#891): a
     record the WAL took but couldn't write before its checkpointed head would hold up the flush
     of every root it pushed into, and with it the checkpoint that would make the room.  Sized by
     serialising it as ~TTransaction will; only the sequence numbers are missing, and they take
     the same room whatever they are. */
  WalReservation.Release();
  Disk::TWal *wal = TouchesSafeRepo ? Manager->GetWal() : nullptr;
  if (wal) {
    TReplicationStreamer streamer;
    streamer.PushTransaction(Replica, true);
    auto recorder = std::make_shared<Io::TRecorder>();
    /* serialize */ {
      Io::TBinaryOutputOnlyStream strm(recorder);
      streamer.Write(strm);
      strm.Flush();
    }
    std::string wire;
    recorder->CopyOut(wire);
    /* A promotion may spend the room kept for it, and its pushes owe room for the promotions still
       ahead of them (#959).  A paused repo's updates owe the room for their copy too, and an
       unpause gives it back (#963).  A transaction that would refuse at once if the ring were
       full (a client's) is refused too if it would take the paused repos past their share of the
       ring; one that waits for room (a slave applying its master's) never is, as the master
       already took it. */
    size_t credit = 0UL;
    std::vector<Disk::TPromotionDebt> owes;
    if (TWalRetention *retention = Manager->GetWalRetention()) {
      std::vector<TWalRetention::TPlannedPush> pushes;
      pushes.reserve(SafePushes.size());
      for (const auto &push : SafePushes) {
        pushes.push_back(TWalRetention::TPlannedPush{push.Mutation->GetRepoId(), push.Levels});
      }
      std::vector<TWalRetention::TPop> pops;
      pops.reserve(SafePops.size());
      for (const auto &pop : SafePops) {
        pops.push_back(TWalRetention::TPop{pop.first, pop.second});
      }
      std::vector<TWalRetention::TStatusChange> statuses;
      statuses.reserve(SafeStatuses.size());
      for (const auto &status : SafeStatuses) {
        statuses.push_back(TWalRetention::TStatusChange{status.first->GetRepoId(), status.second, status.first->GetSequenceNumber()});
      }
      auto plan = retention->PlanRoom(pushes, pops, statuses, wire.size(), !WalGiveUp);
      credit = plan.Credit;
      owes = std::move(plan.Owes);
    }
    WalReservation = ReserveWalRoom(*wal, wire.size(), WalRoom, WalGiveUp, credit, owes);
  }
  /* Only now that nothing above can refuse the transaction: a refused one leaves its updates as
     they were. */
  TTransactionCompletion *transaction_completion = TransactionCompletion;
  for (const TPusher *pusher : safe_pushers) {
    pusher->Update->SetPersistenceNotification([transaction_completion](TUpdate::TPersistenceNotification::TResult result) {
      switch (result) {
        case TUpdate::TPersistenceNotification::Completed: {
          transaction_completion->RegisterCompletion();
          break;
        }
        case TUpdate::TPersistenceNotification::Failed: {
          transaction_completion->RegisterFailure();
          break;
        }
      }
    });
  }
  TransactionCompletion->SetCb(func);
  TransactionCompletion->SetWaitFor(num_push);
  Prepared = true;
}

TTransaction::TTransactionCompletion::TTransactionCompletion(TManager *manager)
    : WaitFor(0U), NumCompleted(0U), ManagerMembership(this, &manager->TransactionCompletionCollection) {}

TTransaction::TTransactionCompletion::~TTransactionCompletion() {
  assert(ManagerMembership.TryGetCollector());
  /* acquire TransactionCompletion lock */ {
    std::lock_guard<std::mutex> lock(ManagerMembership.TryGetCollector()->TransactionCompletionLock);
    ManagerMembership.Remove();
  }  // release TransactionCompletion lock
}

void TTransaction::TTransactionCompletion::SetWaitFor(size_t wait_for) {
  WaitFor = wait_for;
  if (WaitFor == 0) {
    assert(Cb);
    (*Cb)(Completed);
    delete this;
  }
}

void TTransaction::TTransactionCompletion::RegisterCompletion() {
  bool del = false;
  /* lock life-span */ {
    std::lock_guard<std::mutex> lock(Mutex);
    ++NumCompleted;
    if (NumCompleted == WaitFor) {
      (*Cb)(Completed);
      del = true;
    }
  }
  if (del) {
    delete this;
  }
}

void TTransaction::TTransactionCompletion::RegisterFailure() {
  /* lock life-span */ {
    std::lock_guard<std::mutex> lock(Mutex);
    (*Cb)(Failed);
  }
  delete this;
}

TTransaction::TTransaction(TManager *manager, bool should_replicate)
    : Manager(manager),
      ManagerMembership(this),
      MutationCollection(this),
      CommitFlag(false),
      ShouldReplicate(should_replicate),
      Prepared(false),
      EnsureOrDiscard(false),
      TransactionReplication(Manager->NewTransactionReplication()),
      TransactionCompletion(nullptr) {}

TTransaction::~TTransaction() NO_THROW {
  assert(!CommitFlag || Prepared);

  /* hold replication queue lock */ {
    std::lock_guard<std::mutex> lock(Manager->GetReplicationQueueLock());

    /* first delete the pushers */
    for(TMutation *mutation = MutationCollection.TryGetFirstMember(); mutation;) {
      TMutation *my_mutation = mutation;
      mutation = mutation->TryGetNextMember();
      if (my_mutation->GetKind() == TMutation::Pusher) {
        delete my_mutation;
      }
    }

    /* Test-only window between the push-apply and pop-apply passes. */
    if (CommitFlag && OnCommitBetweenPushAndPopForTest) {
      OnCommitBetweenPushAndPopForTest();
    }

    /* now delete the rest */
    for(TMutation *mutation = MutationCollection.TryGetFirstMember(); mutation; mutation = MutationCollection.TryGetFirstMember()) {
      delete mutation;
    }

    if (CommitFlag) {
      /* The WAL record, and what the WAL must keep for recovery to rebuild these updates (#755). */
      TWalRetention *retention = TouchesSafeRepo ? Manager->GetWalRetention() : nullptr;
      if (retention) {
        try {
          std::vector<TWalRetention::TPush> pushes;
          pushes.reserve(SafePushes.size());
          for (const auto &push : SafePushes) {
            /* A push the repo refused has no sequence number; it isn't there to rebuild. */
            if (push.Mutation->GetSequenceNumber()) {
              pushes.push_back(TWalRetention::TPush{push.Mutation->GetRepoId(), *push.Mutation->GetSequenceNumber(), push.IsRoot, push.Levels});
            }
          }
          std::vector<TWalRetention::TPop> pops;
          pops.reserve(SafePops.size());
          for (const auto &pop : SafePops) {
            pops.push_back(TWalRetention::TPop{pop.first, pop.second});
          }
          std::vector<TWalRetention::TStatusChange> statuses;
          statuses.reserve(SafeStatuses.size());
          for (const auto &status : SafeStatuses) {
            statuses.push_back(TWalRetention::TStatusChange{status.first->GetRepoId(), status.second, status.first->GetSequenceNumber()});
          }
          if (Manager->GetWal()) {
            TReplicationStreamer streamer;
            streamer.PushTransaction(Replica);
            auto recorder = std::make_shared<Io::TRecorder>();
            /* serialize */ {
              Io::TBinaryOutputOnlyStream strm(recorder);
              streamer.Write(strm);
              strm.Flush();
            }
            std::string wire;
            recorder->CopyOut(wire);
            if (WalReservation && wire.size() > WalReservation.GetSize()) {
              syslog(LOG_ERR, "~TTransaction: WAL record of %ld bytes is bigger than the %ld Prepare reserved for it (#891)", wire.size(), WalReservation.GetSize());
            }
            const uint64_t lsn = retention->AppendTxn(wire, pushes, pops, statuses, &WalReservation);
            if (CommitLsnOut) {
              *CommitLsnOut = lsn;
            }
          } else {
            /* Recovery replaying the WAL, before there is one to append to: pin the record being
               replayed.  Does nothing otherwise. */
            retention->NoteReplayedTxn(pushes, pops, statuses);
          }
        } catch (const std::exception &ex) {
          syslog(LOG_ERR, "~TTransaction: WAL append failed: %s", ex.what());
        }
      }
      if (ShouldReplicate) {
        Manager->Enqueue(TransactionReplication, std::move(Replica));
      } else {
        Manager->DeleteTransactionReplication(TransactionReplication);
      }
    } else {
      delete TransactionCompletion;
      Manager->DeleteTransactionReplication(TransactionReplication);
    }
    /* Not committed (or nothing to append): give the room back. */
    WalReservation.Release();
  }
}

TTransaction::TReplica::TReplica(TReplica &&that)
    : MutationList(std::move(that.MutationList)) {}

TTransaction::TReplica::TReplica(const TReplica &that)
    : MutationList(that.MutationList) {}

TTransaction::TReplica::TReplica() {}

TTransaction::TReplica::~TReplica() {}

TTransaction::TReplica &TTransaction::TReplica::operator=(TReplica &&that) {
  std::swap(MutationList, that.MutationList);
  return *this;
}

TTransaction::TReplica &TTransaction::TReplica::operator=(const TReplica &that) {
  return *this = TReplica(that);
}

TTransaction::TReplica::TMutation::TUpdate::TUpdate()
    : Suprena(nullptr) {}

TTransaction::TReplica::TMutation::TUpdate::TUpdate(TUpdate &&that)
    : Suprena(std::move(that.Suprena)),
      Metadata(that.Metadata),
      Id(that.Id),
      OpByKey(std::move(that.OpByKey)) {}

TTransaction::TReplica::TMutation::TUpdate::TUpdate(const TUpdate &that)
    : Suprena(make_shared<TSuprena>()) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Metadata = TCore(Suprena.get(), state_alloc, that.Suprena.get(), that.Metadata);
  Id = TCore(Suprena.get(), state_alloc, that.Suprena.get(), that.Id);
  for (const auto &op_by_key : that.OpByKey) {
    OpByKey.emplace_back(TIndexKey(op_by_key.IndexKey.GetIndexId(), TKey(TCore(Suprena.get(), state_alloc, that.Suprena.get(), op_by_key.IndexKey.GetKey().GetCore()), Suprena.get())),
                         TCore(Suprena.get(), state_alloc, that.Suprena.get(), op_by_key.Op),
                         op_by_key.Mutator);
  }
}

TTransaction::TReplica::TMutation::TUpdate::TUpdate(const Orly::Indy::TUpdate *that)
    : Suprena(make_shared<TSuprena>()) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Metadata = TCore(Suprena.get(), state_alloc, &that->GetSuprena(), that->GetMetadata());
  Id = TCore(Suprena.get(), state_alloc, &that->GetSuprena(), that->GetId());
  for (Orly::Indy::TUpdate::TEntryCollection::TCursor csr(that->GetEntryCollection()); csr; ++csr) {
    OpByKey.emplace_back(TIndexKey(csr->GetIndexKey().GetIndexId(), TKey(TCore(Suprena.get(), state_alloc, &csr->GetSuprena(), csr->GetKey().GetCore()), Suprena.get())),
                         TCore(Suprena.get(), state_alloc, &csr->GetSuprena(), csr->GetOp()),
                         csr->GetMutator());
  }
}

TTransaction::TReplica::TMutation::TUpdate::~TUpdate() {}

TTransaction::TReplica::TMutation::TUpdate &TTransaction::TReplica::TMutation::TUpdate::operator=(TUpdate &&that) {
  std::swap(Suprena, that.Suprena);
  Metadata = that.Metadata;
  Id = that.Id;
  std::swap(OpByKey, that.OpByKey);
  return *this;
}

TTransaction::TReplica::TMutation::TUpdate &TTransaction::TReplica::TMutation::TUpdate::operator=(const TUpdate &that) {
  return *this = TUpdate(that);
}

void TTransaction::TReplica::TMutation::TUpdate::Write(Io::TBinaryOutputStream &strm, const Atom::TCore::TRemap &remap) const {
  strm << TCore(Metadata, remap)  // (Atom::TCore) Metadata
      << TCore(Id, remap)  // (Atom::TCore) Id
      << OpByKey.size();  // (size_t) num pairs of key = op
  //cout << "Streaming [" << TKey(Metadata, Suprena.get()) << "][" << TKey(Id, Suprena.get()) << "][" << OpByKey.size() << "]" << endl;
  for (const auto &op_by_key : OpByKey) {
    strm << op_by_key.IndexKey.GetIndexId()   // (Base::TUuid) index_id
        << TCore(op_by_key.IndexKey.GetKey().GetCore(), remap)  // (Atom::TCore) key
        << TCore(op_by_key.Op, remap)  // (Atom::TCore) op
        << static_cast<uint8_t>(op_by_key.Mutator);  // (uint8_t) mutator
  }
}

void TTransaction::TReplica::TMutation::TUpdate::Read(Io::TBinaryInputStream &strm, const Atom::TCore::TRemap &remap) {
  size_t num_op_by_key;
  Base::TUuid temp_index_id;
  Atom::TCore temp_key;
  Atom::TCore temp_op;
  strm >> Metadata  // (Atom::TCore) Metadata
      >> Id  // (Atom::TCore) Id
      >> num_op_by_key;  // (size_t) num pairs of key = op
  Metadata.Remap(remap);
  Id.Remap(remap);
  //cout << "Reading [" << TKey(Metadata, Suprena.get()) << "][" << TKey(Id, Suprena.get()) << "][" << num_op_by_key << "]" << endl;
  try {
    OpByKey.reserve(num_op_by_key);
  } catch (const exception &ex) {
    DEBUG_LOG("Exception in TTransaction::TReplica::TMutation::TUpdate::Read [%s] trying to reserve [%ld]", ex.what(), num_op_by_key);
    throw;
  }
  uint8_t temp_mutator_raw;
  for (size_t i = 0; i < num_op_by_key; ++i) {
    strm >> temp_index_id  // (Base::TUuid) index_id
        >> temp_key  // (Atom::TCore) key
        >> temp_op   // (Atom::TCore) op
        >> temp_mutator_raw;  // (uint8_t) mutator
    temp_key.Remap(remap);
    temp_op.Remap(remap);
    OpByKey.emplace_back(TIndexKey(temp_index_id, TKey(temp_key, Suprena.get())), temp_op, static_cast<TMutator>(temp_mutator_raw));
  }
}

TTransaction::TReplica::TMutation::TMutation(TMutation &&that)
    : Kind(that.Kind),
      RepoId(std::move(that.RepoId)),
      Update(std::move(that.Update)),
      SequenceNumber(that.SequenceNumber) {}

TTransaction::TReplica::TMutation::TMutation(const TMutation &that)
    : Kind(that.Kind),
      RepoId(that.RepoId),
      Update(that.Update),
      SequenceNumber(that.SequenceNumber) {}

TTransaction::TReplica::TMutation::TMutation(TKind kind, const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num)
    : Kind(kind),
      RepoId(repo_id),
      SequenceNumber(seq_num) {
  assert(Kind != Pusher);
}

TTransaction::TReplica::TMutation::TMutation(TKind kind, const Base::TUuid &repo_id, const Orly::Indy::TUpdate *update, const std::optional<TSequenceNumber> &seq_num)
    : Kind(kind),
      RepoId(repo_id),
      Update(update),
      SequenceNumber(seq_num) {
  assert(Kind == Pusher);
}

TTransaction::TReplica::TMutation::TMutation(TKind kind, const Base::TUuid &repo_id, TUpdate &&update, const std::optional<TSequenceNumber> &seq_num)
    : Kind(kind),
      RepoId(repo_id),
      Update(std::move(update)),
      SequenceNumber(seq_num) {
  assert(Kind == Pusher);
}

TTransaction::TReplica::TMutation::~TMutation() {}

TTransaction::TReplica::TMutation &TTransaction::TReplica::TMutation::operator=(TMutation &&that) {
  std::swap(Kind, that.Kind);
  std::swap(RepoId, that.RepoId);
  std::swap(Update, that.Update);
  std::swap(SequenceNumber, that.SequenceNumber);
  return *this;
}

TTransaction::TReplica::TMutation &TTransaction::TReplica::TMutation::operator=(const TMutation &that) {
  return *this = TMutation(that);
}

TTransaction::TReplica::TMutation *TTransaction::TReplica::Push(const Base::TUuid &repo_id, const TUpdate *update) {
  MutationList.emplace_front(TMutation::Pusher, repo_id, update, std::optional<TSequenceNumber>());
  return &MutationList.front();
}

TTransaction::TReplica::TMutation *TTransaction::TReplica::Pop(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num) {
  MutationList.emplace_front(TMutation::Popper, repo_id, seq_num);
  return &MutationList.front();
}

TTransaction::TReplica::TMutation *TTransaction::TReplica::Fail(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num) {
  MutationList.emplace_front(TMutation::Failer, repo_id, seq_num);
  return &MutationList.front();
}

TTransaction::TReplica::TMutation *TTransaction::TReplica::Pause(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num) {
  MutationList.emplace_front(TMutation::Pauser, repo_id, seq_num);
  return &MutationList.front();
}

TTransaction::TReplica::TMutation *TTransaction::TReplica::UnPause(const Base::TUuid &repo_id, const std::optional<TSequenceNumber> &seq_num) {
  MutationList.emplace_front(TMutation::UnPauser, repo_id, seq_num);
  return &MutationList.front();
}

void TTransaction::TReplica::Reset() {
  MutationList.clear();
}

TTransaction::TMutation::TMutation(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo)
    : TransactionMembership(this, repo->GetId(), &transaction->MutationCollection),
      Repo(repo),
      MyMutation(nullptr) {}

TTransaction::TMutation::~TMutation() NO_THROW {}

TTransaction::TPusher::TPusher(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo, const std::shared_ptr<TUpdate> &update,
                               const Base::TUuid &promoted_from)
    : TMutation(transaction, repo), PromotedFrom(promoted_from) {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Update = new TUpdate(update.get(), state_alloc);
}

TTransaction::TPusher::~TPusher() NO_THROW {
  /* This destructor is the commit point for a pushed update and is NO_THROW.
     AppendUpdate is engineered not to throw (the one allocating call it makes,
     the Tetris join, is guarded internally -- #250), but we keep a catch-all
     here so no allocation ever added to the commit path can turn a recoverable
     failure into std::terminate. */
  try {
    assert(TransactionMembership.TryGetCollector());
    if (TransactionMembership.TryGetCollector()->GetCommitFlag()) {
      assert(MyMutation);
      const std::optional<TSequenceNumber> seq_num = Repo->AppendUpdate(Update, MyMutation->GetNextUpdate(), PromotedFrom);
      MyMutation->SetSequenceNumber(seq_num);
      if (auto *out = TransactionMembership.TryGetCollector()->CommitSequenceNumberOut) {
        *out = seq_num;
      }
    } else {
      delete Update;
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "~TPusher: swallowing exception on commit path to avoid terminate (#250): %s", ex.what());
  }
}

TTransaction::TPopper::TPopper(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo, TState state)
    : TMutation(transaction, repo),
      State(state),
      HoldsPromotion(false) {}

TTransaction::TPopper::~TPopper() NO_THROW {
  /* NO_THROW commit point; guard the whole body so an allocation failure on the
     promote/fail path degrades to a logged error rather than terminate (#250). */
  try {
    assert(TransactionMembership.TryGetCollector());
    if (TransactionMembership.TryGetCollector()->GetCommitFlag()) {
      switch(State) {
        case Peek : {
          break;
        };
        case Pop : {
          assert(MyMutation);
          MyMutation->SetSequenceNumber(Repo->PopLowest(MyMutation->GetNextUpdate()));
          break;
        };
        case Fail : {
          assert(MyMutation);
          MyMutation->SetSequenceNumber(Repo->ChangeStatus(Failed, MyMutation->GetNextUpdate()));
          break;
        };
      }
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "~TPopper: swallowing exception on commit path to avoid terminate (#250): %s", ex.what());
  }
  /* Last: our pop (if any) has landed, so a pause waiting on the repo may now commit (#636). */
  if (HoldsPromotion) {
    Repo->ReleasePromotionHold();
  }
}

void TTransaction::TPopper::DoPeek() const {
  if (!View) {
    View = make_shared<TRepo::TView>(Repo);
    Update = Repo->GetLowestUpdate();
  }
}

TTransaction::TStatusChanger::TStatusChanger(TTransaction *transaction, const L0::TManager::TPtr<TRepo> &repo, TStatus status)
    : TMutation(transaction, repo), Status(status), HoldsPause(false) {}

TTransaction::TStatusChanger::~TStatusChanger() NO_THROW {
  /* NO_THROW commit point; guard the whole body so an allocation failure on the
     status-change path degrades to a logged error rather than terminate (#250). */
  try {
    assert(TransactionMembership.TryGetCollector());
    if (TransactionMembership.TryGetCollector()->GetCommitFlag()) {
      switch (Status) {
        case Normal : {
          assert(MyMutation);
          MyMutation->SetSequenceNumber(Repo->ChangeStatus(Status, MyMutation->GetNextUpdate()));
          break;
        }
        case Paused : {
          assert(MyMutation);
          MyMutation->SetSequenceNumber(Repo->ChangeStatus(Status, MyMutation->GetNextUpdate()));
          break;
        }
        case Failed : {
          assert(false);  // A status changer should never be failing a repo. Only tetris should fail repo's through a popper
          break;
        }
      }
    }
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "~TStatusChanger: swallowing exception on commit path to avoid terminate (#250): %s", ex.what());
  }
  /* Last: the pause has committed (or been discarded), so Tetris may peek the repo again. */
  if (HoldsPause) {
    Repo->EndPause();
  }
}

unique_ptr<TTransaction, std::function<void (TTransaction *)>> TManager::NewTransaction(bool should_replicate) {
  TTransaction *transaction = new TTransaction(this, should_replicate);
  /* no-throws */
  /* acquire transaction lock */ {
    std::lock_guard<std::mutex> lock(TransactionLock);
    TransactionCollection.Insert(&transaction->ManagerMembership);
  }  // release transaction lock
  return std::unique_ptr<TTransaction, std::function<void (TTransaction *)>>(transaction, OnCloseTransactionCb);
}

TManager::TManager(Disk::Util::TEngine *engine,
                   std::chrono::milliseconds merge_mem_delay,
                   std::chrono::milliseconds merge_disk_delay,
                   bool allow_tailing,
                   bool prune_merge_history,
                   bool no_realtime,
                   std::chrono::milliseconds layer_cleaning_interval,
                   Base::TScheduler *scheduler,
                   size_t block_slots_available_per_merger,
                   size_t max_repo_cache_size,
                   size_t temp_file_consol_thresh,
                   const std::vector<size_t> &merge_mem_cores,
                   const std::vector<size_t> &merge_disk_cores,
                   bool create_new)
    : L0::TManager(engine,
                   merge_mem_delay,
                   merge_disk_delay,
                   allow_tailing,
                   prune_merge_history,
                   no_realtime,
                   layer_cleaning_interval,
                   scheduler,
                   block_slots_available_per_merger,
                   max_repo_cache_size,
                   temp_file_consol_thresh,
                   merge_mem_cores,
                   merge_disk_cores,
                   create_new),
      TransactionCollection(this),
      TransactionCompletionCollection(this),
      OnCloseTransactionCb(std::bind(&TManager::OnCloseTransaction, this, std::placeholders::_1)),
      WalRetention(std::make_unique<TWalRetention>(this)) {}

TManager::~TManager() {}

void TManager::OnCloseTransaction(TTransaction *transaction) {
  /* acquire transaction lock */ {
    std::lock_guard<std::mutex> lock(TransactionLock);
    transaction->ManagerMembership.Remove();
  }  // release transaction lock
  delete transaction;
}