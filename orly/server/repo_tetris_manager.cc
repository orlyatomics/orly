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

#include <vector>

#include <orly/mynde/protocol.h> // For Mynde::PackageName
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
  bool success = TestAssertions(context);
  if (success) {
    /* swap the metadata with just the session ids if we're pushing to global */
    if (Player->Repo->GetId() == TSession::GlobalPovId) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      if (FuncHolderByUpdateId.size() == 1) {
        /* we're storing just a single session id. */
        PeekedUpdate->SetMetadata(Indy::TKey(MetaRecord.GetEntry(FuncHolderByUpdateId.begin()->first).GetSessionId(), &PeekedUpdate->GetSuprena(), state_alloc));
      } else {
        throw std::runtime_error("We don't currently support collapsed updates.");
      }
    }
    transaction->Push(Player->Repo, PeekedUpdate);
    transaction->Pop(Repo);
    ++(Player->RepoTetrisManager->PushCount);
    ++(Player->RepoTetrisManager->PopCount);
    for (const auto &item: FuncHolderByUpdateId) {
      const auto &entry = MetaRecord.GetEntry(item.first);

      //In the case of Mynde, the notification behavior is special.
      if (entry.GetPackageFqName() == Mynde::PackageName) {
        if (entry.GetMethodName() != "set") {
          throw std::runtime_error("Only memcachememcache.set is supported at this point in time.");
        }
        continue;
      }
      auto session = Player->RepoTetrisManager->DurableManager->Open<TSession>(entry.GetSessionId());
      if (session) {
        session->InsertNotification(Notification::TUpdateProgress::New(Player->Repo->GetId(), item.first, Notification::TUpdateProgress::Accepted));
      }
    }
    Flush();
  } else {
    ++FailureCount;
    if (FailureCount >= 10) {
      transaction->Fail(Repo);
      for (const auto &item: FuncHolderByUpdateId) {
        const auto &entry = MetaRecord.GetEntry(item.first);
        if (entry.GetPackageFqName() == Mynde::PackageName) {
          if (entry.GetMethodName() != "set") {
            throw std::runtime_error("Only memcachememcache.set is supported at this point in time.");
          }
          continue;
        }
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
    }
  }
  return success;
}

bool TRepoTetrisManager::TPlayer::TChild::Refresh(const unique_ptr<Indy::L1::TTransaction, function<void (Indy::L1::TTransaction *)>> &transaction) {
  assert(transaction);
  if (Repo->GetStatus() == Orly::Indy::Normal && Repo->GetSequenceNumberStart() && !PeekedUpdate) {
    PeekedUpdate = transaction->Peek(Repo);
    if (PeekedUpdate) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Sabot::ToNative(*Sabot::State::TAny::TWrapper(PeekedUpdate->GetMetadata().NewState(&PeekedUpdate->GetSuprena(), state_alloc)), MetaRecord);
      for (const auto &item: MetaRecord.GetEntryByUpdateId()) {
        const auto &entry = item.second;
        if (entry.GetPackageFqName() != Mynde::PackageName) {
          FuncHolderByUpdateId[item.first] =
              Player->RepoTetrisManager->PackageManager->Get(Package::TName{entry.GetPackageFqName()})
                  ->GetFunctionInfo(AsPiece(entry.GetMethodName()));
        } else {
          // Add an entry default constructed for mynde calls (We hard code the assertions and replay)
          FuncHolderByUpdateId[item.first];
        }
      }
      Age = 0;
      FailureCount = 0;
    }
  }
  ++Age;
  return static_cast<bool>(PeekedUpdate);
}

bool TRepoTetrisManager::TPlayer::TChild::SortsBefore(const TChild *lhs, const TChild *rhs) {
  assert(lhs);
  assert(rhs);
  return lhs->Age > rhs->Age;
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
     one. Refresh re-parses the metadata Flush just cleared. */
  Flush();
  return Refresh(transaction) && Play(transaction, context);
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
    if(entry.GetPackageFqName() == Mynde::PackageName) {
      if(entry.GetMethodName() != "set") {
        //TODO: Should probably be a custom exception that inherits from std::runtime_error...
        throw std::runtime_error("Only memcache set is supported for update assertions at the moment. And that has none.");
      }
      return true;
    }
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
        unordered_map<string, Indy::TKey> arg_map;
        for (const auto &iter : arg_by_name) {
          Atom::TCore core(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, iter.second)).get());
          arg_map.insert(make_pair(iter.first, Indy::TKey(core, &my_arena)));
        }
        item.second->Call(indy_context, arg_map);
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
        syslog(LOG_INFO, "exception while testing assertions in pov %s; %s", strm.str().c_str(), ex.what());
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
    /* Mynde entries hard-code their own assertion/replay handling
       (TestAssertions special-cases them); keep them on the conservative
       one-per-round path. */
    if (entry.GetPackageFqName() == Mynde::PackageName) {
      return false;
    }
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
}

void TRepoTetrisManager::TPlayer::OnUnpause() {
}

void TRepoTetrisManager::TPlayer::Play() {
  Base::TCPUTimer snapshot_timer, sort_timer, play_timer, commit_timer;
  Atom::TSuprena my_arena;
  try {
    /* Snapshot every child that is ready to participate this round. The Peek
       in Refresh attaches to `snapshot_txn`; it carries no Push/Pop so it costs
       nothing to discard, and it is the transaction the assertion-bearing
       (one-per-round) promotion below reuses. */
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
          }
        }  // txn destroyed here -> AppendUpdate applies the promotion
      }
      if (!promoted_commutative) {
        for (TChild *child: children) {
          if (!child->IsAssertionFree() && child->Play(snapshot_txn, context)) {
            break;
          }
        }
      }
    } else {
      /* Give each child a chance to play.  At most one will be permitted to
         promote (for now), but any number might fail due to age. */
      for (TChild *child: children) {
        if (child->Play(snapshot_txn, context)) {
          break;
        }
      }
    }
    play_timer.Stop();
    /* Commit the snapshot transaction (carries the at-most-one assertion-bearing
       promotion, if any; a no-op otherwise). */
    snapshot_txn->Prepare();
    snapshot_txn->CommitAction();
    commit_timer.Start();
    ++(RepoTetrisManager->RoundCount);
  } catch (const std::exception &ex) {
    syslog(LOG_EMERG, "Tetris::TPlayer::Play error : %s", ex.what());
    throw;
  }
  commit_timer.Stop();

  std::lock_guard<std::mutex> lock(RepoTetrisManager->TetrisTimerLock);
  RepoTetrisManager->TetrisSnapshotCPUTime.Push(ToSecondsDouble(snapshot_timer.GetTotal()));
  RepoTetrisManager->TetrisSortCPUTime.Push(ToSecondsDouble(sort_timer.GetTotal()));
  RepoTetrisManager->TetrisPlayCPUTime.Push(ToSecondsDouble(play_timer.GetTotal()));
  RepoTetrisManager->TetrisCommitCPUTime.Push(ToSecondsDouble(commit_timer.GetTotal()));
}

TTetrisManager::TPlayer *TRepoTetrisManager::NewPlayer(const TUuid &parent_pov_id, const TUuid &child_pov_id, bool is_paused, bool is_master) {
  return new TPlayer(this, parent_pov_id, child_pov_id, is_paused, is_master);
}
