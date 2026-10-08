/* <orly/server/repo_tetris_manager.cc>

   Implements <orly/server/repo_tetris_manager.h>

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

#include <orly/server/repo_tetris_manager.h>

#include <sstream>
#include <vector>

#include <orly/notification/pov_failure.h>
#include <orly/notification/update_progress.h>
#include <base/util/time.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly::Indy;
using namespace Orly::Server;
using namespace ::Util;

TRepoTetrisManager::TRepoTetrisManager(
    TScheduler *scheduler,
    Fiber::TRunner::TRunnerCons &runner_cons,
    Base::TThreadLocalGlobalPoolManager<Indy::Fiber::TFrame, size_t, Indy::Fiber::TRunner *> *frame_pool_manager,
    const std::function<void (Indy::Fiber::TRunner *)> &runner_setup_cb,
    bool is_master,
    Indy::TManager *repo_manager,
    Package::TManager *package_manager,
    Durable::TManager *durable_manager,
    bool log_assertion_failures,
    bool commutative_fastlane)
    : TTetrisManager(scheduler, runner_cons, frame_pool_manager, runner_setup_cb, is_master),
      PushCount(0UL),
      PopCount(0UL),
      FailCount(0UL),
      RoundCount(0UL),
      ChildrenConsideredCount(0UL),
      CommutativeFastlane(commutative_fastlane),
      RepoManager(repo_manager),
      PackageManager(package_manager),
      DurableManager(durable_manager),
      LogAssertionFailures(log_assertion_failures) {
  assert(repo_manager);
  assert(package_manager);
  assert(durable_manager);
}

TRepoTetrisManager::~TRepoTetrisManager() {
  StopAllPlayers();
}

TRepoTetrisManager::TPlayer::TPlayer(TRepoTetrisManager *repo_tetris_manager, const TUuid &parent_pov_id, const TUuid &child_pov_id, bool is_paused, bool is_master)
    : TTetrisManager::TPlayer(repo_tetris_manager), RepoTetrisManager(repo_tetris_manager) {
  Repo = repo_tetris_manager->RepoManager->ForceGetRepo(parent_pov_id);
  OnJoin(child_pov_id);
  Start(is_paused, is_master);
}

TRepoTetrisManager::TPlayer::~TPlayer() {
  /* Our last rounds may have left notifications undelivered (Main() stops calling Play() once
     we have no children), and pins to drop. */
  DeliverAccepted();
  ReleaseSessionPins();
  assert(ChildByPovId.size() == 1UL);
  for (const auto &item: ChildByPovId) {
    delete item.second;
  }
}

TRepoTetrisManager::TPlayer::TChild::TChild(TPlayer *player, const TUuid &child_pov_id)
    : Player(player), Age(0), FailureCount(0) {
  Repo = player->RepoTetrisManager->RepoManager->ForceGetRepo(child_pov_id);
}

bool TRepoTetrisManager::TPlayer::TChild::Play(
    const unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> &transaction, Indy::TContext &context) {
  assert(transaction);
  /* A POV under review that refuses conflicts (#746): don't promote an update that would
     overwrite or delete a key its parent chain changed after the fork.  The POV waits, blocked,
     until it is forced, discarded or paused; that isn't an assertion failure, so it doesn't count
     towards failing the POV. */
  if (auto watch = Repo->GetOwnForkWatch(); watch && watch->GetMode() == Indy::TForkWatch::TMode::Refuse) {
    const auto &start = Repo->GetSequenceNumberStart();
    if (start && watch->ShouldBlock(*PeekedUpdate, *start)) {
      return false;
    }
  }
  bool success = TestAssertions(context);
  if (success) {
    /* swap the metadata with just the session ids if we're pushing to global */
    if (Player->Repo->GetId() == TSession::GlobalPovId) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      if (FuncHolderByUpdateId.size() == 1) {
        /* we're storing just a single session id. */
        PeekedUpdate->SetMetadata(Indy::TKey(MetaRecord.GetEntry(FuncHolderByUpdateId.begin()->first).GetSessionId(), &PeekedUpdate->GetSuprena(), state_alloc));
      } else {
        throw TCollapsedUpdateError(HERE);
      }
    }
    /* Claim the parent's copy of the update before Push makes it (#607). If there's no
       room, Play drops every child's peeked copy, so the player waits holding nothing. */
    Indy::TUpdate::TCopyClaim claim;
    size_t num_entries = 0UL;
    for (Indy::TUpdate::TEntryCollection::TCursor csr(PeekedUpdate->GetEntryCollection()); csr; ++csr) {
      ++num_entries;
    }
    if (!claim.TryAcquire(1UL, num_entries)) {
      throw std::bad_alloc();
    }
    /* Name the child, for the fork watches of the parent's chain (#746). */
    transaction->Push(Player->Repo, PeekedUpdate, std::nullopt, Repo->GetId());
    claim.Release();
    transaction->Pop(Repo);
    PromotedOwnSeq.reset();
    PromotedParentSeq.reset();
    if (Player->Repo->GetId() == TSession::GlobalPovId && PeekedSeq) {
      PromotedOwnSeq = PeekedSeq;
      PromotedLog = Repo->GetPromotionLog();
      transaction->ReportCommitSequenceNumber(&PromotedParentSeq);
    }
    ++(Player->RepoTetrisManager->PushCount);
    ++(Player->RepoTetrisManager->PopCount);
    /* The sessions hear about it once the round has committed (DeliverAccepted, #801). */
    for (const auto &item: FuncHolderByUpdateId) {
      Player->RoundAccepted.emplace_back(MetaRecord.GetEntry(item.first).GetSessionId(), item.first);
    }
    Flush();
    /* The child's next update starts at the back of the queue (#660). */
    Age = 0;
  } else {
    ++FailureCount;
    if (FailureCount >= 10) {
      transaction->Fail(Repo);
      for (const auto &item: FuncHolderByUpdateId) {
        const auto &entry = MetaRecord.GetEntry(item.first);
        auto session = Player->RepoTetrisManager->DurableManager->Open<TSession>(entry.GetSessionId());
        if (session) {
          session->InsertNotification(Notification::TPovFailure::New(Repo->GetId()));
        }
      }
      ++(Player->RepoTetrisManager->FailCount);
      stringstream ss;
      ss << Repo->GetId();
      syslog(LOG_INFO, "Failing Repo [%s]", ss.str().c_str());
      Flush();
      Age = 0;
    }
  }
  return success;
}

bool TRepoTetrisManager::TPlayer::TChild::Refresh(const unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> &transaction) {
  assert(transaction);
  if (!Repo->GetSequenceNumberStart()) {
    ++Age;
    return static_cast<bool>(PeekedUpdate);
  }
  /* Take the child's promotion hold on this round's transaction, even when we still hold an
     update peeked in an earlier round: the hold is what keeps a pause from committing while this
     round may still promote the child (#636).  It is refused while a pause of the child is
     pending, and the child then sits the round out.  Holding it also orders our status read after
     any pause that has committed.

     We take only the hold here, under the player's mutex, and copy nothing: Peek copies the update
     out later, outside the mutex, and only for a child the round may promote (#660). */
  bool held = false;
  try {
    held = transaction->HoldForPromotion(Repo);
  } catch (const std::bad_alloc &) {
    return false;
  }
  if (!held || Repo->GetStatus() != Orly::Indy::Normal) {
    return false;
  }
  ++Age;
  return true;
}

bool TRepoTetrisManager::TPlayer::TChild::Peek(const unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> &transaction) {
  assert(transaction);
  if (PeekedUpdate) {
    return true;
  }
  /* Refresh took the hold on this transaction, so this Peek reads through that same popper, and
     Play's Pop promotes it (Peek->Pop on one mutation, as RepeekAndPlay requires). */
  std::shared_ptr<Indy::TUpdate> peeked;
  try {
    peeked = transaction->Peek(Repo);
  } catch (const std::bad_alloc &) {
    /* No room to copy this child's update out (#607): it sits this round out. A hold ends with
       the round's transaction. */
    return false;
  }
  if (!peeked) {
    return false;
  }
  PeekedUpdate = std::move(peeked);
  /* The copy carries no sequence number.  Only this player pops the child, so its oldest unpopped
     update is the one we copied. */
  PeekedSeq = Repo->GetSequenceNumberStart();
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Sabot::ToNative(*Sabot::State::TAny::TWrapper(PeekedUpdate->GetMetadata().NewState(&PeekedUpdate->GetSuprena(), state_alloc)), MetaRecord);
  for (const auto &item: MetaRecord.GetEntryByUpdateId()) {
    const auto &entry = item.second;
    FuncHolderByUpdateId[item.first] =
        Player->RepoTetrisManager->PackageManager->Get(Package::TName{entry.GetPackageFqName()})
            ->GetFunctionInfo(AsPiece(entry.GetMethodName()));
  }
  /* A new update: its assertions haven't failed yet.  Age is not reset here: it counts the rounds
     this child has been ready since its last promotion, copied out or not, so a child's place in
     the queue doesn't depend on when its turn to be copied came (#660). */
  FailureCount = 0;
  return true;
}

bool TRepoTetrisManager::TPlayer::TChild::SortsBefore(const TChild *lhs, const TChild *rhs) {
  assert(lhs);
  assert(rhs);
  return lhs->Age > rhs->Age;
}

void TRepoTetrisManager::TPlayer::TChild::FinishPromotion() {
  if (PromotedOwnSeq && PromotedParentSeq && PromotedLog) {
    PromotedLog->Note(*PromotedOwnSeq, *PromotedParentSeq, Player->Repo->GetDurableSequenceNumber());
  }
  PromotedOwnSeq.reset();
  PromotedParentSeq.reset();
  PromotedLog.reset();
}

void TRepoTetrisManager::TPlayer::TChild::Flush() {
  PeekedUpdate.reset();
  MetaRecord = TMetaRecord();
  FuncHolderByUpdateId.clear();
}

bool TRepoTetrisManager::TPlayer::TChild::RepeekAndPlay(
    const unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> &transaction, Indy::TContext &context) {
  assert(transaction);
  /* Drop the snapshot-phase Peek (it lives on a different transaction) so the
     Refresh below re-Peeks this child on `transaction`; the subsequent Pop in
     Play then promotes that very popper (Peek->Pop) instead of minting a second
     one. Peek re-parses the metadata Flush just cleared. */
  Flush();
  return Refresh(transaction) && Peek(transaction) && Play(transaction, context);
}

namespace Orly {
  namespace Rt {
    template <typename TVal>
    std::ostream &operator<<(std::ostream &out, const TOpt<TVal> &that) {

      if(that.IsKnown()) {
        out<<that.GetVal();
      }

      return out;
    }

  }
}

bool TRepoTetrisManager::TPlayer::TChild::TestAssertions(Indy::TContext &context) const {
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  for (const auto &item: FuncHolderByUpdateId) {
    const auto &entry = MetaRecord.GetEntry(item.first);
    const auto &expected_predicate_results = entry.GetExpectedPredicateResults();
    if (expected_predicate_results.size()) {
      Atom::TSuprena my_arena;
      Rt::TOpt<Base::TUuid> user_id;
      if (entry.GetUserId()) {
        user_id = Base::TUuid(entry.GetUserId()->GetRaw());
      }
      Base::TUuid session_id(entry.GetSessionId().GetRaw());
      Indy::TIndyContext indy_context(user_id, session_id, context, &my_arena,
          Player->RepoTetrisManager->GetScheduler(), entry.GetRunTimestamp(), entry.GetRandomSeed());
      const auto &arg_by_name = entry.GetArgByName();
      try {
        /* Replay the calls this entry records, in order, each with its own args, on one context,
           as TSession::RunBatch ran them (#751). A batch records index-prefixed args, and in a
           mixed batch each call's own method: replaying the entry as one call of its first method
           looked up an arg name the batch never recorded, and failed the POV. */
        for (const auto &call: entry.GetCalls()) {
          std::shared_ptr<const Package::TFuncHolder> func = item.second;
          if (call.PackageFqName != entry.GetPackageFqName() || call.MethodName != entry.GetMethodName()) {
            func = Player->RepoTetrisManager->PackageManager->Get(Package::TName{call.PackageFqName})
                ->GetFunctionInfo(AsPiece(call.MethodName));
          }
          Package::TArgMap arg_map;
          for (const auto &iter : call.ArgByName) {
            Atom::TCore core(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, iter.second)).get());
            arg_map.insert(make_pair(iter.first, Indy::TKey(core, &my_arena)));
          }
          func->Call(indy_context, arg_map);
        }
        if (vector<bool>(expected_predicate_results.begin(), expected_predicate_results.end()) != indy_context.GetPredicateResults()) {
          if (Player->RepoTetrisManager->LogAssertionFailures) {
            stringstream ss;
            ss << "package=[";
            for (const auto &elem : entry.GetPackageFqName()) {
              ss << "/" << elem;
            }
            ss << "], method=[" << entry.GetMethodName() << "]"
               << ", user_id=[" << user_id << "]"
               << ", session_id=[" << session_id << "]"
               << ", ARGS {";
            for (const auto &iter : arg_by_name) {
              ss << " " << iter.first << "=[" << iter.second << "]";
            }
            ss << "} KEYS {";
            for (const auto &iter : indy_context.GetEffects()) {
              ss << "[" << iter.first.GetKey() << "] ";
            }
            ss << "}";
            syslog(LOG_INFO, "Assertion Failure %s", ss.str().c_str());
          }
          return false;
        }
      } catch (const exception &ex) {
        stringstream strm;
        strm << Repo->GetId();
        /* Not an assertion conflict: the replay itself couldn't run. Every retry fails the same
           way, so this POV is about to fail; say so where it shows (#751). */
        syslog(LOG_ERR, "exception while testing assertions in pov %s (method %s); %s", strm.str().c_str(),
            entry.GetMethodName().c_str(), ex.what());
        return false;
      }
    }
  }
  return true;
}

bool TRepoTetrisManager::TPlayer::TChild::IsAssertionFree() const {
  if (FuncHolderByUpdateId.empty()) {
    /* Nothing refreshed -- be conservative and let the ordinary
       one-per-round path handle it. */
    return false;
  }
  for (const auto &item: FuncHolderByUpdateId) {
    const auto &entry = MetaRecord.GetEntry(item.first);
    /* Any non-empty expected-predicate set means this is an assertion-bearing
       read-modify-write that must be tested against the round-start snapshot
       one at a time. */
    if (entry.GetExpectedPredicateResults().size()) {
      return false;
    }
  }
  return true;
}

void TRepoTetrisManager::TPlayer::OnJoin(const TUuid &child_pov_id) {
  lock_guard<mutex> lock(Mutex);
  auto child = new TChild(this, child_pov_id);
  try {
    ChildByPovId.insert(make_pair(child_pov_id, child));
  } catch (...) {
    delete child;
    throw;
  }
}

void TRepoTetrisManager::TPlayer::OnPart(const TUuid &child_pov_id) {
  lock_guard<mutex> lock(Mutex);
  auto iter = ChildByPovId.find(child_pov_id);
  if (iter != ChildByPovId.end()) {
    delete iter->second;
    ChildByPovId.erase(iter);
  }
}

void TRepoTetrisManager::TPlayer::OnPause() {
  /* A pause can last a long time: don't hold sessions open through it. */
  DeliverAccepted();
  ReleaseSessionPins();
}

void TRepoTetrisManager::TPlayer::DeliverAccepted() noexcept {
  if (TurnAccepted.empty()) {
    return;
  }
  auto *durable_manager = RepoTetrisManager->DurableManager;
  const TUuid parent_id = Repo->GetId();
  for (const auto &[session_id, update_id] : TurnAccepted) {
    try {
      auto iter = SessionPins.find(session_id);
      if (iter == SessionPins.end()) {
        if (SessionPins.empty()) {
          PinnedSince = steady_clock::now();
        }
        iter = SessionPins.emplace(session_id, durable_manager->Open<TSession>(session_id)).first;
      }
      if (iter->second) {
        iter->second->InsertNotification(Notification::TUpdateProgress::New(parent_id, update_id, Notification::TUpdateProgress::Accepted));
      }
    } catch (const std::exception &ex) {
      /* The promotion has committed either way; only its notification is lost. */
      std::ostringstream strm;
      strm << session_id;
      syslog(LOG_ERR, "tetris: could not tell session [%s] about a promoted update: %s", strm.str().c_str(), ex.what());
    }
  }
  TurnAccepted.clear();
}

void TRepoTetrisManager::TPlayer::ReleaseSessionPins() noexcept {
  /* Each pin's TPtr releases in the erase; a closed session saves and waits for the save. */
  while (!SessionPins.empty()) {
    SessionPins.erase(SessionPins.begin());
  }
}

void TRepoTetrisManager::TPlayer::Play() {
  const auto start = steady_clock::now();
  bool promoted = false;
  try {
    while (PlayRound()) {
      promoted = true;
      if (!KeepPlaying() || steady_clock::now() - start >= TurnBudget) {
        break;
      }
    }
  } catch (...) {
    /* The rounds that committed before this one stay committed: tell their sessions. */
    DeliverAccepted();
    throw;
  }
  DeliverAccepted();
  if (!promoted || SessionPins.size() > MaxSessionPins || steady_clock::now() - PinnedSince >= PinHoldLimit) {
    ReleaseSessionPins();
  }
}

void TRepoTetrisManager::TPlayer::OnUnpause() {
}

bool TRepoTetrisManager::TPlayer::PlayRound() {
  Base::TCPUTimer snapshot_timer, sort_timer, play_timer, commit_timer;
  Atom::TSuprena my_arena;
  RoundAccepted.clear();
  bool promoted = false;
  try {
    /* Snapshot every child that is ready to participate this round. Refresh takes each child's
       promotion hold on `snapshot_txn`; it carries no Push/Pop so it costs nothing to discard,
       and it is the transaction the assertion-bearing (one-per-round) promotion below reuses.
       Refresh copies nothing, so no pool claim is made under the player's mutex. */
    unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> snapshot_txn = RepoTetrisManager->RepoManager->NewTransaction();
    vector<TChild *> children;
    snapshot_timer.Start();
    /* extra */ {
      lock_guard<mutex> lock(Mutex);
      children.reserve(ChildByPovId.size());
      for (const auto &item: ChildByPovId) {
        TChild *child = item.second;
        if (child->Refresh(snapshot_txn)) {
          children.push_back(child);
        }
      }
    }
    /* Only the fast lane copies every ready child's update out before playing, because it must
       read each one's metadata to classify it. The one-per-round path below copies a child's
       update only when its turn comes: a round promotes at most one child, and copying all of
       them first took the whole pool when many children had backlogs, so the copy for the one
       promotion never fit and no round ever completed (#660). */
    if (RepoTetrisManager->CommutativeFastlane) {
      std::erase_if(children, [&snapshot_txn](TChild *child) { return !child->Peek(snapshot_txn); });
    }
    snapshot_timer.Stop();
    RepoTetrisManager->ChildrenConsideredCount += children.size();
    /* Sort by decreasing promote-ness. */
    sort_timer.Start();
    sort(children.begin(), children.end(), TChild::SortsBefore);
    sort_timer.Stop();

    play_timer.Start();
    Indy::TContext context(Repo, &my_arena);
    if (RepoTetrisManager->CommutativeFastlane) {
      /* #234 fast-lane. Assertion-free children are pure commutative field
         calls that provably cannot conflict (architecture.md §5), so promote
         ALL ready ones this round -- each in its own transaction so the
         one-Pusher-per-repo invariant and each child's own session-id metadata
         (load-bearing for replication notifications) are preserved exactly.
         A transaction applies its Push on destruction, so each promotion is
         scoped to one loop iteration.

         Assertion-bearing (read-modify-write) children keep today's discipline:
         at most one per round, tested against the round-start snapshot. We only
         take that path when NO commutative promotion happened this round, so an
         RMW assertion is never evaluated against a parent the same round's
         commutative writes have already mutated. */
      bool promoted_commutative = false;
      for (TChild *child: children) {
        if (child->IsAssertionFree()) {
          /* Promote on a dedicated transaction, re-Peeking the child on THAT
             transaction first. The snapshot-phase Peek attached to snapshot_txn
             and was only used to classify + sort; if we Pop'd on a fresh txn
             whose Peek lived on snapshot_txn, the Peek and the Pop would land on
             two different poppers (two different transactions) for the same
             child repo -- the Pop creates a brand-new Pop-state popper while the
             snapshot_txn popper still holds the read View. Re-binding the Peek
             to this txn makes the Pop promote the very popper the Peek created
             (Peek->Pop on one mutation), so Peek/Push/Pop/commit are one atomic
             unit and the child's PopLowest (which may Part-delete it) runs
             exactly once against the snapshot it was tested on. */
          unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> txn = RepoTetrisManager->RepoManager->NewTransaction();
          if (child->RepeekAndPlay(txn, context)) {
            txn->Prepare();
            txn->CommitAction();
            promoted_commutative = true;
            /* Applies the promotion. */
            txn.reset();
            child->FinishPromotion();
            TurnAccepted.insert(TurnAccepted.end(), RoundAccepted.begin(), RoundAccepted.end());
            RoundAccepted.clear();
          }
        }
      }
      promoted = promoted_commutative;
      if (!promoted_commutative) {
        for (TChild *child: children) {
          if (!child->IsAssertionFree() && child->Play(snapshot_txn, context)) {
            promoted = true;
            break;
          }
        }
      }
    } else {
      /* Give each child a chance to play.  At most one will be permitted to
         promote (for now), but any number might fail due to age. */
      for (TChild *child: children) {
        if (child->Peek(snapshot_txn) && child->Play(snapshot_txn, context)) {
          promoted = true;
          break;
        }
      }
    }
    play_timer.Stop();
    /* Commit the snapshot transaction (carries the at-most-one assertion-bearing
       promotion, if any; a no-op otherwise). */
    commit_timer.Start();
    snapshot_txn->Prepare();
    snapshot_txn->CommitAction();
    /* Applies the promotion, before the next round of this turn takes its snapshot. */
    snapshot_txn.reset();
    for (TChild *child: children) {
      child->FinishPromotion();
    }
    commit_timer.Stop();
    TurnAccepted.insert(TurnAccepted.end(), RoundAccepted.begin(), RoundAccepted.end());
    RoundAccepted.clear();
    ++(RepoTetrisManager->RoundCount);
  } catch (const std::bad_alloc &) {
    /* Out of pool space: TTetrisManager::TPlayer::Main logs it, rate-limited, and plays the
       round again (#607). Drop the children's peeked copies first, so that while it waits the
       player holds nothing the merges or another round could use; the next round peeks
       again. This round's transaction never committed, so its promotions didn't happen. */
    RoundAccepted.clear();
    {
      lock_guard<mutex> lock(Mutex);
      for (const auto &item: ChildByPovId) {
        item.second->Flush();
      }
    }
    throw;
  } catch (const std::exception &ex) {
    RoundAccepted.clear();
    syslog(LOG_EMERG, "Tetris::TPlayer::Play error : %s", ex.what());
    throw;
  }

  std::lock_guard<std::mutex> lock(RepoTetrisManager->TetrisTimerLock);
  RepoTetrisManager->TetrisSnapshotCPUTime.Push(ToSecondsDouble(snapshot_timer.GetTotal()));
  RepoTetrisManager->TetrisSortCPUTime.Push(ToSecondsDouble(sort_timer.GetTotal()));
  RepoTetrisManager->TetrisPlayCPUTime.Push(ToSecondsDouble(play_timer.GetTotal()));
  RepoTetrisManager->TetrisCommitCPUTime.Push(ToSecondsDouble(commit_timer.GetTotal()));
  return promoted;
}

TTetrisManager::TPlayer *TRepoTetrisManager::NewPlayer(const TUuid &parent_pov_id, const TUuid &child_pov_id, bool is_paused, bool is_master) {
  return new TPlayer(this, parent_pov_id, child_pov_id, is_paused, is_master);
}
