/* <orly/server/pov_review.cc>

   Implements <orly/server/pov_review.h>.

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

#include <orly/server/pov_review.h>

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <orly/atom/suprena.h>
#include <orly/indy/context.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/repo.h>
#include <orly/indy/transaction_base.h>
#include <orly/rt/mutate.h>
#include <orly/var/new_sabot.h>
#include <orly/var/orlyify.h>
#include <orly/var/sabot_to_var.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Server;

namespace {

  using TRepoPtr = Indy::L0::TManager::TPtr<Indy::TRepo>;

  Var::TVar CoreToVar(const Atom::TCore &core, Atom::TCore::TArena *arena) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    return Var::ToVar(*Sabot::State::TAny::TWrapper(core.NewState(arena, state_alloc)));
  }

  Indy::TKey VarToKey(const Var::TVar &var, Atom::TSuprena *arena) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    return Indy::TKey(Atom::TCore(arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, var)).get()), arena);
  }

  /* Key order, then index (value type) order. */
  struct TKeyOrder {
    bool operator()(const Indy::TIndexKey &lhs, const Indy::TIndexKey &rhs) const {
      const Atom::TComparison comp = lhs.GetKey().Compare(rhs.GetKey());
      if (Atom::IsLt(comp)) {
        return true;
      }
      if (Atom::IsGt(comp)) {
        return false;
      }
      return lhs.GetIndexId() < rhs.GetIndexId();
    }
  };

  void ChangeStatus(Indy::TManager *repo_manager, const TRepoPtr &repo, bool pause) {
    auto transaction = repo_manager->NewTransaction();
    if (pause) {
      transaction->Pause(repo);
    } else {
      transaction->UnPause(repo);
    }
    transaction->Prepare();
    transaction->CommitAction();
  }

  void CheckHasParent(const TRepoPtr &repo, const char *what) {
    if (!repo->GetParentRepo()) {
      ostringstream strm;
      strm << what << ": the global POV has no parent";
      throw invalid_argument(strm.str());
    }
  }

  TPovConflict ToPovConflict(const Indy::TForkWatch::TConflict &conflict) {
    return TPovConflict{conflict.Number, CoreToVar(conflict.Key.GetKey().GetCore(), conflict.Key.GetKey().GetArena()),
                        conflict.IsDelete, conflict.Raced};
  }

}  // namespace

/* Operations. */

void Orly::Server::WatchFork(const TRepoPtr &repo, TConflictMode mode) {
  if (mode == TConflictMode::None) {
    return;
  }
  CheckHasParent(repo, "conflicts");
  vector<TUuid> chain;
  vector<TRepoPtr> chain_repos;
  Indy::L0::TManager::TPtr<Indy::L0::TManager::TRepo> cur_repo = repo;
  for (; cur_repo->GetParentRepo(); cur_repo = *cur_repo->GetParentRepo()) {
    TRepoPtr parent = *cur_repo->GetParentRepo();
    chain.push_back(parent->GetId());
    chain_repos.push_back(parent);
  }
  auto watch = make_shared<Indy::TForkWatch>(
      repo->GetId(), std::move(chain), mode == TConflictMode::Refuse ? Indy::TForkWatch::TMode::Refuse : Indy::TForkWatch::TMode::Report);
  repo->SetOwnForkWatch(watch);
  for (const auto &chain_repo: chain_repos) {
    chain_repo->AddForkWatch(watch);
  }
}

TPovDiff Orly::Server::DiffPov(const TRepoPtr &repo, const TPovDiffOptions &options) {
  if (options.Since) {
    /* The save-point hook (#745). */
    throw invalid_argument("diff_pov: .since needs named save points, which need versioned reads (#745); Orly doesn't have them yet");
  }
  CheckHasParent(repo, "diff_pov");
  if (options.Limit < 1 || options.Limit > TPovDiffOptions::MaxLimit) {
    throw invalid_argument("diff_pov: bad .limit");
  }
  Atom::TSuprena arena;
  optional<Indy::TKey> start, stop, after;
  if (options.Start) {
    start = VarToKey(*options.Start, &arena);
  }
  if (options.Stop) {
    stop = VarToKey(*options.Stop, &arena);
  }
  if (options.After) {
    after = VarToKey(*options.After, &arena);
  }
  /* Every entry of every unpromoted update, by key, oldest first.  Each repo numbers its own
     updates, so the order within the POV's repo is its own sequence order (#791). */
  struct TRec {
    TMutator Mutator;
    bool IsTombstone;
    Atom::TCore Op;
  };
  map<Indy::TIndexKey, vector<TRec>, TKeyOrder> by_key;
  TPovDiff diff;
  /* extra */ {
    auto view = make_unique<Indy::TRepo::TView>(repo);
    if (view->GetLower() && view->GetUpper()) {
      diff.Updates = *view->GetUpper() - *view->GetLower() + 1UL;
      auto walker = repo->NewUpdateWalker(view, *view->GetLower(), *view->GetUpper());
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      for (auto &cur = *walker; cur; ++cur) {
        const auto &item = *cur;
        for (const auto &entry: item.EntryVec) {
          const Indy::TKey &key = entry.IndexKey.GetKey();
          if ((start && key < *start) || (stop && !(key < *stop)) || (after && !(*after < key))) {
            continue;
          }
          auto iter = by_key.find(entry.IndexKey);
          if (iter == by_key.end()) {
            iter = by_key.emplace(Indy::TIndexKey(entry.IndexKey.GetIndexId(), Indy::TKey(&arena, state_alloc, key)), vector<TRec>()).first;
          }
          const bool is_tombstone = entry.Op.IsTombstone();
          iter->second.push_back(TRec{entry.Mutator, is_tombstone,
                                      is_tombstone ? Atom::TCore() : Atom::TCore(&arena, state_alloc, item.MainArena, entry.Op)});
        }
      }
    }
  }
  if (by_key.empty()) {
    return diff;
  }
  /* The parent's values, through the parent's own view of its chain. */
  TRepoPtr parent = *repo->GetParentRepo();
  Atom::TSuprena context_arena;
  Indy::TContext context(parent, &context_arena);
  const Indy::TIndexKey *last_key = nullptr;
  for (const auto &item: by_key) {
    if (diff.Changes.size() >= options.Limit && last_key && !(last_key->GetKey() == item.first.GetKey())) {
      diff.Next = CoreToVar(last_key->GetKey().GetCore(), last_key->GetKey().GetArena());
      ostringstream strm;
      Var::Orlyify(strm, *diff.Next);
      diff.NextLiteral = strm.str();
      break;
    }
    const auto &recs = item.second;
    TPovChange change;
    change.Key = CoreToVar(item.first.GetKey().GetCore(), item.first.GetKey().GetArena());
    /* extra */ {
      const Indy::TKey before = context[item.first];
      if (!before.GetCore().IsVoid()) {
        change.Before = CoreToVar(before.GetCore(), before.GetArena());
      }
    }
    /* The newest overwrite or delete in the POV, if any: the POV's own base for the key. */
    ptrdiff_t base = -1;
    for (size_t i = 0; i < recs.size(); ++i) {
      if (recs[i].Mutator == TMutator::Assign) {
        base = static_cast<ptrdiff_t>(i);
      }
    }
    if (base < 0) {
      /* Commutative updates only: they apply on top of whatever the parent holds. */
      bool one_op = true;
      for (const auto &rec: recs) {
        one_op = one_op && rec.Mutator == recs.front().Mutator;
      }
      optional<Var::TVar> acc = change.Before;
      for (const auto &rec: recs) {
        const Var::TVar rhs = CoreToVar(rec.Op, &arena);
        /* An absent key starts from the operator's identity (#151), which leaves rhs. */
        acc = acc ? Rt::Mutate(*acc, rec.Mutator, rhs) : rhs;
      }
      change.After = acc;
      if (one_op) {
        change.Kind = TPovChange::TKind::Delta;
        change.Op = recs.front().Mutator;
        Var::TVar delta = CoreToVar(recs.front().Op, &arena);
        for (size_t i = 1; i < recs.size(); ++i) {
          delta = Rt::Mutate(delta, change.Op, CoreToVar(recs[i].Op, &arena));
        }
        change.Delta = std::move(delta);
      } else {
        /* Mixed operators on one key: no single delta says it; give the values. */
        change.Kind = change.Before ? TPovChange::TKind::Changed : TPovChange::TKind::Added;
      }
    } else {
      optional<Var::TVar> acc;
      if (!recs[base].IsTombstone) {
        acc = CoreToVar(recs[base].Op, &arena);
      }
      for (size_t i = static_cast<size_t>(base) + 1; i < recs.size(); ++i) {
        const Var::TVar rhs = CoreToVar(recs[i].Op, &arena);
        acc = acc ? Rt::Mutate(*acc, recs[i].Mutator, rhs) : rhs;
      }
      change.After = acc;
      if (!change.After && !change.Before) {
        /* Made and deleted again in the POV: no change. */
        continue;
      }
      if (change.After && change.Before) {
        bool same = false;
        try {
          same = *change.After == *change.Before;
        } catch (const exception &) {
          /* Different types: not the same. */
        }
        if (same) {
          continue;
        }
      }
      change.Kind = !change.After ? TPovChange::TKind::Removed
                  : change.Before ? TPovChange::TKind::Changed : TPovChange::TKind::Added;
    }
    diff.Changes.push_back(std::move(change));
    last_key = &item.first;
  }
  return diff;
}

TPovDiscard Orly::Server::DiscardPov(Indy::TManager *repo_manager, const TRepoPtr &repo) {
  assert(repo_manager);
  CheckHasParent(repo, "discard_pov");
  if (repo->GetStatus() == Indy::Failed) {
    throw invalid_argument("discard_pov: the POV has failed, and a failed POV stays failed; make a new one");
  }
  /* Pausing takes the POV out of its parent's Tetris, after any promotion under way lands
     (#636), so nothing promotes what we pop. */
  const bool was_normal = repo->GetStatus() == Indy::Normal;
  if (was_normal) {
    ChangeStatus(repo_manager, repo, true);
  }
  TPovDiscard result;
  try {
    optional<Indy::TSequenceNumber> lowest, highest;
    Indy::TSequenceNumber next;
    repo->GetSnapshot(lowest, highest, next);
    if (lowest && highest) {
      /* A pop with no push releases at once, and a repo releases its updates in order, so wait
         for what Tetris promoted before the pause to be released first: that waits for a safe
         parent to persist it, which is usually long done. */
      const auto deadline = chrono::steady_clock::now() + chrono::seconds(30);
      while (repo->GetReleasedUpTo() + 1UL < *lowest) {
        if (chrono::steady_clock::now() > deadline) {
          throw runtime_error("discard_pov: the POV's last promotion has not reached its parent's disk after 30 s; try again");
        }
        if (Indy::Fiber::TFrame::LocalFrame) {
          Indy::Fiber::YieldSlow();
        } else {
          this_thread::yield();
        }
      }
      const size_t entries_before = repo->GetMemBacklogEntries();
      for (Indy::TSequenceNumber seq = *lowest; seq <= *highest; ++seq) {
        auto transaction = repo_manager->NewTransaction();
        transaction->Pop(repo);
        transaction->Prepare();
        transaction->CommitAction();
        ++result.Updates;
      }
      const size_t entries_after = repo->GetMemBacklogEntries();
      result.Entries = entries_before > entries_after ? entries_before - entries_after : 0UL;
    }
    if (auto watch = repo->GetOwnForkWatch()) {
      watch->Refork();
    }
  } catch (...) {
    if (was_normal) {
      ChangeStatus(repo_manager, repo, false);
    }
    throw;
  }
  if (was_normal) {
    ChangeStatus(repo_manager, repo, false);
  }
  return result;
}

TPovPromote Orly::Server::PromotePov(Indy::TManager *repo_manager, const TRepoPtr &repo, bool force) {
  assert(repo_manager);
  CheckHasParent(repo, "promote_pov");
  if (repo->GetStatus() == Indy::Failed) {
    throw invalid_argument("promote_pov: the POV has failed");
  }
  TPovPromote result;
  optional<Indy::TSequenceNumber> lowest, highest;
  Indy::TSequenceNumber next;
  repo->GetSnapshot(lowest, highest, next);
  if (lowest && highest) {
    result.Pending = *highest - *lowest + 1UL;
  }
  auto watch = repo->GetOwnForkWatch();
  if (watch) {
    result.Mark = watch->GetState().ConflictCount;
  }
  if (watch && watch->GetMode() == Indy::TForkWatch::TMode::Refuse && highest) {
    if (force) {
      watch->Force(*highest);
    } else {
      /* Test the whole backlog now, so a refusal leaves the POV as it was. */
      set<Indy::TIndexKey, TKeyOrder> seen;
      auto view = make_unique<Indy::TRepo::TView>(repo);
      if (view->GetLower() && view->GetUpper()) {
        auto walker = repo->NewUpdateWalker(view, *view->GetLower(), *view->GetUpper());
        for (auto &cur = *walker; cur; ++cur) {
          for (const auto &entry: (*cur).EntryVec) {
            if (entry.Mutator == TMutator::Assign && watch->IsChanged(entry.IndexKey) && seen.insert(entry.IndexKey).second) {
              result.Conflicts.push_back(TPovConflict{0UL, CoreToVar(entry.IndexKey.GetKey().GetCore(), entry.IndexKey.GetKey().GetArena()),
                                                      entry.Op.IsTombstone(), false});
            }
          }
        }
      }
      if (!result.Conflicts.empty()) {
        result.Status = TPovPromote::TStatus::Refused;
        return result;
      }
    }
  }
  if (repo->GetStatus() == Indy::Paused) {
    ChangeStatus(repo_manager, repo, false);
  }
  return result;
}

TPovReview Orly::Server::ReviewPov(const TRepoPtr &repo, uint64_t after) {
  CheckHasParent(repo, "review_pov");
  TPovReview review;
  switch (repo->GetStatus()) {
    case Indy::Normal: review.Status = 'N'; break;
    case Indy::Paused: review.Status = 'P'; break;
    case Indy::Failed: review.Status = 'F'; break;
  }
  optional<Indy::TSequenceNumber> lowest, highest;
  Indy::TSequenceNumber next;
  repo->GetSnapshot(lowest, highest, next);
  if (lowest && highest) {
    review.Pending = *highest - *lowest + 1UL;
    review.PendingEntries = repo->GetMemBacklogEntries();
  }
  if (auto watch = repo->GetOwnForkWatch()) {
    review.Mode = watch->GetMode() == Indy::TForkWatch::TMode::Refuse ? TConflictMode::Refuse : TConflictMode::Report;
    const auto state = watch->GetState();
    review.Blocked = state.Blocked;
    review.Overflowed = state.Overflowed;
    review.ChangedKeys = state.ChangedKeys;
    review.ConflictCount = state.ConflictCount;
    watch->ForEachBlockingKey([&review](const Indy::TForkWatch::TConflict &conflict) {
      review.BlockedOn.push_back(ToPovConflict(conflict));
    });
    watch->ForEachConflict(after, [&review](const Indy::TForkWatch::TConflict &conflict) {
      review.Conflicts.push_back(ToPovConflict(conflict));
    });
  }
  return review;
}
