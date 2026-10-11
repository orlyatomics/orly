/* <orly/server/session.cc>

   Implements <orly/server/session.h>.

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

#include <orly/server/session.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>

#include <orly/atom/suprena.h>
#include <orly/indy/context.h>
#include <orly/notification/all.h>
#include <orly/server/insufficient_memory.h>
#include <orly/server/insufficient_storage.h>
#include <orly/server/read_too_large.h>
#include <orly/server/write_too_large.h>
#include <orly/server/meta_record.h>
#include <orly/var/mutation.h>
#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
#include <base/util/time.h>
#include <orly/indy/disk/wal.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Notification;
using namespace Orly::Server;
using namespace Util;

/* Write backpressure (#234, #584, #721).

   The first signal is the writer's POV backlog: the updates Tetris has yet to promote from the
   POV's memtable to its parent. Every merge step copies the whole unpromoted backlog before it
   frees the old copy, and a memory merge claims a whole copy of it up front (#629), so the
   backlog must stay well below the Update pool's capacity or one merge fills the pool (#584).
   Each POV's backlog is therefore capped at 1/32 of it, so several POVs backing up at once still
   leave room for their merges. It is capped in entries too, at 1/32 of the Update Entry pool
   (#628): the merge copies entries as well as updates, and a batch is a single update with an
   entry per write.

   The cap is held before the write commits (#721). ReserveBacklogRoom takes the write's room in
   the backlog (TRepo::TBacklogReservation) and, while there is none, waits for the backlog to
   drain: we hold no repo lock, no pool blocks and no context views (#722), and the merges and Tetris run on their own
   runners, so YieldSlow lets them make progress. The cap used to be checked after the commit, so
   every writer that came in while the backlog was under it landed: with 8 writers of 200-write
   batches, a backlog capped at 1,250 entries reached 3,000 and more, and its merge's claim got
   other writers refused.

   A writer that has waited 5 s without the backlog shrinking (the parent's player paused for an
   import, merges far behind) is refused with insufficient_memory. It used to write anyway, past
   the cap. A refusal leaves nothing half done, the client contract retries it, and the cap, which
   is what bounds the merge's claim, still holds.

   Only a backlog that Tetris is promoting drains (#626). A paused POV keeps its backlog until it
   is unpaused, and a failed one keeps it for good, so a write to either that finds no room is
   refused at once; RefuseWriteToStalledBacklog refuses most of them earlier, before they do any
   work. A POV that is neither paused nor yet in its parent's Tetris (its join was deferred under
   memory pressure, #250; TBacklogReservation::IsUndrainable) is let through without room, because
   only its next AppendUpdate retries the join.

   The second signal is the Update / Update Entry pools past half full, whatever holds them:
   merges need that much room to copy into. ApplyWriteBackpressure waits on it after the commit.
   That wait is bounded, because the pools also hold data that drains slowly or never (a fast POV
   keeps its writes in memory): past the deadline the write proceeds, and an allocation that then
   fails fails just this call.

   With memory admission on (#607) the second wait is skipped: admission keeps the merges' room
   by refusing writes before they allocate, so the wait would only make every write that lands
   past half full sit out its 5 s. Measured with a 25% reserve, it held writers to one batch per
   5 s each, and reads on the same runners took as long. */
size_t Orly::Server::GetWriterBacklogCap(size_t backlog_threshold) {
  return std::min(backlog_threshold, std::max<size_t>(Indy::TUpdate::GetUpdatePoolMaxBlocks() / 32, 1));
}

/* #628: a POV's backlog is capped in entries too, at 1/32 of the Update Entry pool. */
size_t Orly::Server::GetWriterBacklogEntryCap() {
  return std::max<size_t>(Indy::TUpdate::GetEntryPool().GetMaxBlocks() / 32, 1);
}

static std::atomic<size_t> StalledBacklogRefusals {0UL};

size_t Orly::Server::GetStalledBacklogRefusals() {
  return StalledBacklogRefusals.load();
}

/* #626: the refusal of a write to a paused or failed POV whose backlog is full. */
[[noreturn]] static void ThrowFullStalledBacklog(Indy::TStatus status, size_t backlog, size_t entries, size_t cap, size_t entry_cap) {
  std::ostringstream msg;
  msg << "insufficient memory: write refused; this POV is " << (status == Indy::Paused ? "paused" : "failed")
      << " and already holds " << backlog << " unpromoted updates (" << entries << " entries), the most one POV may hold ("
      << cap << " updates, " << entry_cap << " entries)";
  if (status == Indy::Paused) {
    msg << "; writes are accepted again once it is unpaused and they have been promoted";
  }
  msg << "; reads still work";
  throw TInsufficientMemory(msg.str());
}

/* #626: a write to a paused or failed POV whose backlog has reached the cap is refused before
   it does any work, with the typed status of #607. Nothing promotes that backlog until the POV
   is unpaused, so waiting for it would never end.

   Why refuse rather than let such a POV grow? Memory admission (#607) is global: a paused POV
   allowed to grow fills the update pools to the merges' reserve, and then every write to every
   POV is refused until it is unpaused. Measured with the paused-POV smoke (5,000-update pool),
   that happened after 1,900 writes to the paused POV. Held to the cap, a paused POV takes at
   most 1/32 of the Update pool, like any POV whose merges are behind, and the memory merge's
   copy of its backlog stays as small as theirs (#584).

   This early check reads the backlog without holding room in it; ReserveBacklogRoom, just
   before the commit, is what keeps concurrent writers from passing it together (#721). */
static void RefuseWriteToStalledBacklog(const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, size_t backlog_threshold) {
  if (!backlog_threshold) {
    return;
  }
  const Indy::TStatus status = repo->GetStatus();
  if (status == Indy::Normal) {
    return;
  }
  const size_t cap = GetWriterBacklogCap(backlog_threshold), entry_cap = GetWriterBacklogEntryCap();
  const size_t backlog = repo->GetMemBacklogDepth();
  /* #628: in entries too, so a paused POV fed batches stops at the same share of the Entry
     pool as any other POV. */
  const size_t entries = repo->GetMemBacklogEntries();
  if (backlog < cap && entries < entry_cap) {
    return;
  }
  ThrowFullStalledBacklog(status, backlog, entries, cap, entry_cap);
}

/* #721: takes room in repo's backlog for a write of num_entries entries, waiting while the
   backlog drains, or throws TInsufficientMemory. See the comment above GetWriterBacklogCap. */
static void ReserveBacklogRoom(TSession::TServer *server, Indy::TRepo::TBacklogReservation &room,
    const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, size_t backlog_threshold, size_t num_entries) {
  if (!backlog_threshold) {
    return;
  }
  const size_t cap = GetWriterBacklogCap(backlog_threshold), entry_cap = GetWriterBacklogEntryCap();
  constexpr auto backlog_stall = seconds(5);
  size_t lowest_backlog = std::numeric_limits<size_t>::max();
  size_t lowest_entries = std::numeric_limits<size_t>::max();
  auto backlog_deadline = steady_clock::now() + backlog_stall;
  while (!room.TryReserve(cap, entry_cap, num_entries)) {
    /* A stop has begun: give up the wait now, refused, rather than hold the stop's connection
       drain for up to the 5 s below (#769). */
    server->RefuseWriteIfStopping();
    const Indy::TStatus status = repo->GetStatus();
    const size_t backlog = repo->GetMemBacklogDepth();
    const size_t entries = repo->GetMemBacklogEntries();
    if (status != Indy::Normal) {
      ThrowFullStalledBacklog(status, backlog, entries, cap, entry_cap);
    }
    if (room.IsUndrainable()) {
      return;
    }
    const auto now = steady_clock::now();
    if (backlog < lowest_backlog || entries < lowest_entries) {
      lowest_backlog = std::min(lowest_backlog, backlog);
      lowest_entries = std::min(lowest_entries, entries);
      backlog_deadline = now + backlog_stall;
    } else if (now >= backlog_deadline) {
      ++StalledBacklogRefusals;
      std::ostringstream msg;
      msg << "insufficient memory: write refused; this POV's backlog holds " << backlog << " unpromoted updates ("
          << entries << " entries), the most one POV may hold is " << cap << " updates and " << entry_cap
          << " entries, this write has " << num_entries << ", and the backlog has not shrunk for "
          << duration_cast<seconds>(backlog_stall).count() << " s; retry later; reads still work";
      throw TInsufficientMemory(msg.str());
    }
    Indy::Fiber::YieldSlow();
  }
}

static void ApplyWriteBackpressure(bool wait_for_pools) {
  if (!wait_for_pools) {
    return;
  }
  constexpr double pool_threshold = 0.5;
  const auto deadline = steady_clock::now() + seconds(5);
  while ((Indy::TUpdate::GetUpdatePoolUsedPct() > pool_threshold || Indy::TUpdate::GetUpdateEntryPoolUsedPct() > pool_threshold)
         && steady_clock::now() < deadline) {
    Indy::Fiber::YieldSlow();
  }
}

/* The run_time recorded in an update's TMetaRecord. Rt::TContext::Now()
   memoizes wall time into OptNow on first use, so this is Known whenever the
   client supplied `now` or the executed method (or a predicate) evaluated it;
   the fallback fires only when nothing touched time at all. Fall back to the
   commit-time wall clock: replay can't observe the difference (a replayed
   predicate that reads `now` implies the original did, forcing the Known
   path), and the durable record shouldn't claim a sentinel date (#494). */
static Base::Chrono::TTimePnt GetRunTime(const Rt::TOpt<Base::Chrono::TTimePnt> &opt_now) {
  return opt_now.IsKnown() ? opt_now.GetVal() : Base::Chrono::Now();
}

/* Tetris replays a batch's recorded calls to test its predicate results at promotion (#751), so
   they must read back as exactly the calls that ran: same count, order, package, method and arg
   names. If not, refuse before anything commits, rather than acknowledge a write whose promotion
   can only fail. */
static void CheckRecordedCalls(const TMetaRecord::TEntry &entry, const std::vector<TMetaRecord::TEntry::TCall> &ran) {
  auto recorded = entry.GetCalls();
  bool same = recorded.size() == ran.size();
  for (size_t i = 0; same && i < ran.size(); ++i) {
    same = recorded[i].PackageFqName == ran[i].PackageFqName && recorded[i].MethodName == ran[i].MethodName &&
        recorded[i].ArgByName.size() == ran[i].ArgByName.size() &&
        std::equal(recorded[i].ArgByName.begin(), recorded[i].ArgByName.end(), ran[i].ArgByName.begin(),
            [](const auto &lhs, const auto &rhs) { return lhs.first == rhs.first; });
  }
  if (!same) {
    throw std::logic_error("refusing a batch whose meta record doesn't replay as the calls that ran (#751)");
  }
}

TMethodResult TSession::DoInPast(
    TServer */*server*/, const TUuid &/*pov_id*/, const vector<string> &/*fq_name*/, const TClosure &/*closure*/, const TUuid &/*tracking_id*/) {
  THROW_ERROR(TStubbed) << "DoInPast";
}

bool TSession::ForEachNotification(const function<bool (uint32_t, const TNotification *)> &cb) const {
  lock_guard<mutex> lock(NotificationMutex);
  for (const auto &item: NotificationBySeqNumber) {
    if (!cb(item.first, item.second)) {
      return false;
    }
  }
  return true;
}

const TNotification *TSession::GetFirstNotification(uint32_t &seq_number) {
  lock_guard<mutex> lock(NotificationMutex);
  assert(!NotificationBySeqNumber.empty());
  auto iter = NotificationBySeqNumber.begin();
  seq_number = iter->first;
  return iter->second;
}

TUuid TSession::NewFastPrivatePov(TServer *server, const std::optional<TUuid> &parent_pov_id, const seconds &time_to_live) {
  assert(server);
  return NewPov(server, parent_pov_id, TPov::TAudience::Private, TPov::TPolicy::Fast, time_to_live);
}

TUuid TSession::NewFastSharedPov(TServer *server, const std::optional<TUuid> &parent_pov_id, const seconds &time_to_live) {
  return NewPov(server, parent_pov_id, TPov::TAudience::Shared, TPov::TPolicy::Fast, time_to_live);
}

TUuid TSession::NewSafePrivatePov(TServer *server, const std::optional<TUuid> &parent_pov_id, const seconds &time_to_live) {
  return NewPov(server, parent_pov_id, TPov::TAudience::Private, TPov::TPolicy::Safe, time_to_live);
}

TUuid TSession::NewSafeSharedPov(TServer *server, const std::optional<TUuid> &parent_pov_id, const seconds &time_to_live) {
  return NewPov(server, parent_pov_id, TPov::TAudience::Shared, TPov::TPolicy::Safe, time_to_live);
}

TUuid TSession::NewReviewPov(TServer *server, const std::optional<TUuid> &parent_pov_id, const seconds &time_to_live,
                             bool is_safe, bool is_shared, TConflictMode mode) {
  assert(server);
  return NewPov(server, parent_pov_id, is_shared ? TPov::TAudience::Shared : TPov::TAudience::Private,
                is_safe ? TPov::TPolicy::Safe : TPov::TPolicy::Fast, time_to_live, mode);
}

Indy::L0::TManager::TPtr<Indy::TRepo> TSession::OpenReviewRepo(TServer *server, const TUuid &pov_id, const char *what, bool must_be_ours) {
  assert(server);
  if (pov_id == GlobalPovId) {
    ostringstream strm;
    strm << what << ": the global POV has no parent";
    throw invalid_argument(strm.str());
  }
  auto pov = server->GetDurableManager()->Open<TPov>(pov_id);
  if (must_be_ours && (pov->GetAudience() != TPov::TAudience::Private || pov->GetSessionId() != GetId())) {
    /* A shared POV holds other sessions' writes too, and a private one belongs to the session
       that made it. */
    ostringstream strm;
    strm << what << ": only the session that made a private POV may discard its changes";
    throw invalid_argument(strm.str());
  }
  auto repo = pov->GetRepo(server);
  AddPov(pov);
  return repo;
}

std::optional<uint64_t> TSession::GetDurableVersion(TServer *server, const TUuid &pov_id) {
  assert(server);
  auto global_repo = server->GetRepoManager()->ForceGetRepo(GlobalPovId);
  const auto global_durable = global_repo->GetDurableSequenceNumber();
  if (pov_id == GlobalPovId) {
    return global_durable;
  }
  auto pov = server->GetDurableManager()->Open<TPov>(pov_id);
  if (!pov) {
    DEFINE_ERROR(error_t, runtime_error, "unknown pov_id");
    THROW_ERROR(error_t) << pov_id;
  }
  return pov->GetRepo(server)->GetPromotedDurableSequenceNumber(global_durable);
}

TPovDiff TSession::DiffPov(TServer *server, const TUuid &pov_id, const TPovDiffOptions &options) {
  /* Run where reads run, as Try does: a binary connection's fiber is on a slow runner. */
  std::optional<Indy::Fiber::TSwitchToRunner> runner_switcher;
  if (auto *runner = server->NextFastRunner()) {
    runner_switcher.emplace(runner);
  }
  return Orly::Server::DiffPov(OpenReviewRepo(server, pov_id, "diff_pov"), options);
}

TPovDiscard TSession::DiscardPov(TServer *server, const TUuid &pov_id) {
  /* Run where reads run, as Try does: a binary connection's fiber is on a slow runner. */
  std::optional<Indy::Fiber::TSwitchToRunner> runner_switcher;
  if (auto *runner = server->NextFastRunner()) {
    runner_switcher.emplace(runner);
  }
  return Orly::Server::DiscardPov(server->GetRepoManager(), OpenReviewRepo(server, pov_id, "discard_pov", /* must_be_ours */ true));
}

TPovPromote TSession::PromotePov(TServer *server, const TUuid &pov_id, bool force) {
  /* Run where reads run, as Try does: a binary connection's fiber is on a slow runner. */
  std::optional<Indy::Fiber::TSwitchToRunner> runner_switcher;
  if (auto *runner = server->NextFastRunner()) {
    runner_switcher.emplace(runner);
  }
  return Orly::Server::PromotePov(server->GetRepoManager(), OpenReviewRepo(server, pov_id, "promote_pov"), force);
}

TPovReview TSession::ReviewPov(TServer *server, const TUuid &pov_id, uint64_t after) {
  /* Run where reads run, as Try does: a binary connection's fiber is on a slow runner. */
  std::optional<Indy::Fiber::TSwitchToRunner> runner_switcher;
  if (auto *runner = server->NextFastRunner()) {
    runner_switcher.emplace(runner);
  }
  return Orly::Server::ReviewPov(OpenReviewRepo(server, pov_id, "review_pov"), after);
}

void TSession::PausePov(TServer *server, const TUuid &pov_id) {
  assert(server);
  auto pov = server->GetDurableManager()->Open<TPov>(pov_id);
  auto repo = pov->GetRepo(server);
  std::unique_ptr<Indy::L1::TTransaction, std::function<void (Indy::L1::TTransaction *)>> transaction = server->GetRepoManager()->NewTransaction();
  transaction->Pause(repo);
  transaction->Prepare();
  transaction->CommitAction();
  AddPov(pov);
}

std::optional<uint32_t> TSession::InsertNotification(TNotification *notification) {
  lock_guard<mutex> lock(NotificationMutex);
  if (!QueuesNotifications) {
    delete notification;
    return std::nullopt;
  }
  uint32_t result = NextSeqNumber++;
  try {
    NotificationBySeqNumber.insert(make_pair(result, notification));
    NotificationSem.Push();
  } catch (...) {
    --NextSeqNumber;
    delete notification;
    throw;
  }
  return result;
}

void TSession::RemoveNotification(uint32_t seq_number) {
  lock_guard<mutex> lock(NotificationMutex);
  auto iter = NotificationBySeqNumber.find(seq_number);
  assert(iter != NotificationBySeqNumber.end());
  delete iter->second;
  NotificationBySeqNumber.erase(iter);
  NotificationSem.Pop();
}

void TSession::SetTimeToLive(TServer *server, const TUuid &durable_id, const seconds &time_to_live) {
  assert(server);
  throw std::runtime_error("TSession::SetTimeToLive is currently not enabled.");
  server->GetDurableManager()->Open<TObj>(durable_id)->SetTtl(time_to_live);
}

void TSession::SetUserId(TServer */*server*/, const TUuid &user_id) {
  if (UserId) {
    DEFINE_ERROR(error_t, runtime_error, "user_id already set");
    THROW_ERROR(error_t) << "existing uid = " << user_id;
  }
  UserId = user_id;
}

TMethodResult TSession::Try(TServer *server, const TUuid &pov_id, const vector<string> &fq_name, const TClosure &closure) {
  assert(Indy::Fiber::TRunner::LocalRunner);
  std::optional<Indy::Fiber::TSwitchToRunner> RunnerSwitcher;
  if (auto *runner = server->NextFastRunner()) {
    RunnerSwitcher.emplace(runner);
  }
  TCore result_core;
  Base::TTimer timer;
  Base::TTimer call_timer;
  bool had_effects = false;
  std::optional<Indy::TSequenceNumber> commit_seq;
  std::optional<uint64_t> commit_lsn;
  std::optional<TTracker> tracker = std::optional<TTracker>();
  size_t walker_count = 0UL;
  TSuprena my_arena;
  try {
    // Convert the args to vars.
    Package::TArgMap prog_args;
    void *state_alloc_1 = alloca(Sabot::State::GetMaxStateSize() * 2);
    void *state_alloc_2 = reinterpret_cast<uint8_t *>(state_alloc_1) + Sabot::State::GetMaxStateSize();
    auto arena = closure.GetArena().get();
    for (const auto &item: closure.GetCoreByName()) {
      prog_args.insert(make_pair(item.first, Indy::TKey(item.second, arena)));
    }
    // Open the pov and its repo and prepare the data and package contexts.
    auto pov = server->GetDurableManager()->Open<TPov>(pov_id);
    if (!pov) {
      DEFINE_ERROR(error_t, runtime_error, "unknown pov_id");
      THROW_ERROR(error_t) << pov_id;
    }
    AddPov(pov);
    auto repo = pov->GetRepo(server);
    Indy::TContext context(repo, &my_arena);
    Rt::TOpt<Base::TUuid> user_id;
    if (UserId) {
      user_id = UserId->GetRaw();
    }
    Base::TUuid session_id = GetId().GetRaw();
    Indy::TIndyContext indy_context(user_id, session_id, context, &my_arena, server->GetScheduler(),
      Rt::TOpt<Base::Chrono::TTimePnt>(), Rt::TOpt<uint64_t>());
    // Func it.
    auto func = server->GetPackageManager().Get(Package::TName{fq_name})->GetFunctionInfo(AsPiece(closure.GetMethodName()));
    Package::TContext::TEffects effects;
    /* Bound what the call may walk and build (#694). Lifted once it returns: resolving a
       write's effects and building its update are bounded by write admission instead. */
    context.SetReadBudget(server->GetReadBudgetRows(), server->GetReadBudgetBytes(), &my_arena,
        server->GetReadBudgetSteps());
    call_timer.Start();
    result_core = func->Call(indy_context, prog_args);
    call_timer.Stop();
    context.CheckArenaBudget();
    context.ClearReadBudget();
    effects = indy_context.MoveEffects();
    if (!effects.empty()) {
      had_effects = true;
      /* Refuse the write, before it takes memory it would have to flush, while disk space is
         low; a read (no effects) is never refused (#590). */
      server->CheckWriteAdmission();
      RefuseWriteToStalledBacklog(repo, server->GetWriteBackpressureThreshold());
      /* Declared before the transaction so it outlives it (#721): see TBacklogReservation. */
      Indy::TRepo::TBacklogReservation backlog_room(&*repo);
      auto transaction = server->GetRepoManager()->NewTransaction();
      transaction->ReportCommitSequenceNumber(&commit_seq);
      transaction->ReportCommitLsn(&commit_lsn);
      Indy::TUpdate::TOpByKey op_by_key;
      /* Deferred entries from #49 phase 2: defer-safe commutative
         mutations skip the read-modify-write and get registered with
         their mutator preserved, after NewUpdate constructs the rest.
         This is the actual concurrent-write fix -- two sessions both
         doing `+= 5` now each emit {Add, 5} and the read path folds
         them, instead of both resolving against a stale read and
         producing a lost update. */
      std::vector<std::tuple<Indy::TIndexKey, Indy::TKey, TMutator>> deferred_entries;
      for (const auto &item: effects) {
        auto key = item.first;
        /* Defer-safe path: single TMutation with a commutative+associative
           mutator. Skip the read entirely; emit RHS + mutator directly.
           Anything else (Assign, Delete, partial changes, non-commutative
           mutators) falls through to the existing resolve-to-value path. */
        if (auto *mut = dynamic_cast<const Var::TMutation *>(item.second.get())) {
          if (Var::IsDeferSafeCommutative(mut->GetMutator())) {
            deferred_entries.emplace_back(
                key,
                Indy::TKey(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_2, mut->GetRhs())).get()),
                mut->GetMutator());
            continue;
          }
        }
        Var::TVar val;
        if (!item.second->IsDelete()) {
          if (!item.second->IsFinal()) {
            val = Var::ToVar(*Sabot::State::TAny::TWrapper(context[key].GetState(state_alloc_1)));
          }
          item.second->Apply(val);
          op_by_key[key] =
              Indy::TKey(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_2, val)).get());
        }
        else {
          op_by_key[key] =
              Indy::TKey(Native::TTombstone::Tombstone, &my_arena, state_alloc_2);
        }
      }
      TUuid update_id(TUuid::Twister);
      tracker = TTracker(update_id, seconds(0));
      const auto &predicate_results = indy_context.GetPredicateResults();
      TMetaRecord::TEntry::TArgByName meta_args_by_name;
      auto closure_arena = closure.GetArena().get();
      for (const auto &item: closure.GetCoreByName()) {
        auto arg = Var::ToVar(*Sabot::State::TAny::TWrapper(item.second.NewState(closure_arena, state_alloc_1)));
        meta_args_by_name.insert(make_pair(item.first, arg));
      }

      //NOTE: Could do these inline, but this is less fugly to write out because they are so long.
      uint64_t random_seed = 0;
      if(indy_context.GetOptRandomSeed().IsKnown()) {
        random_seed = indy_context.GetOptRandomSeed().GetVal();
      }
      const Base::Chrono::TTimePnt run_time = GetRunTime(indy_context.GetOptNow());

      TMetaRecord meta_record(
          update_id,
          TMetaRecord::TEntry(
              GetId(), GetUserId(), fq_name, closure.GetMethodName(),
              TMetaRecord::TEntry::TArgByName(meta_args_by_name.begin(), meta_args_by_name.end()),
              TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
              run_time, random_seed)
      );
      /* Hold this write's room in its POV's backlog, waiting for the backlog to drain, or
         refuse it (#721). Before memory admission, so a waiting writer holds no pool blocks.

         The statement has read everything it will (the call, then its effects' resolution
         above), so it drops its context's views first (#722). They pin this repo's and every
         ancestor's mapping and memtable as of the call, and with them the memory layers the
         merges this wait is waiting for have replaced; held across the wait, they keep that
         memory from being freed. The results are already in the arena. */
      context.ReleaseViews();
      ReserveBacklogRoom(server, backlog_room, repo, server->GetWriteBackpressureThreshold(), op_by_key.size() + deferred_entries.size());
      /* Hold this write's room in the update pools, or refuse it, before it builds anything
         there (#607). Released once the transaction below has committed. A write that doesn't
         fit waits a bounded time for the merges to make room (#765), holding no pool blocks, no
         lock and, since the views went above, no memory the merges would free. */
      Indy::TUpdate::TWriteAdmission write_memory;
      server->CheckMemoryAdmission(write_memory, op_by_key.size() + deferred_entries.size());
      /* Nothing is committed until CommitAction, so running out of pool here (the merges can
         take the reserve too) is a refusal, not a failed write (#607). */
      try {
        auto update = Indy::TUpdate::NewUpdate(op_by_key, Indy::TKey(meta_record, &my_arena, state_alloc_1), Indy::TKey(update_id, &my_arena, state_alloc_2));
        /* Register the defer-safe commutative mutations gathered above.
           These don't go through TOpByKey because op_by_key is a map and
           TUpdate's TOpByKey ctor always tags entries Assign -- the
           AddEntry overload (added in #49 phase 1) takes the mutator.

           #perf: the deferred entries arrive in write order. AddEntry
           ReverseInserts each into update->EntryCollection, which is ordered by
           TKey; in write order across several indices each insert scans O(N) to
           find its slot, so committing a transaction of N commutative writes
           (e.g. a batched bulk load) is O(N^2) -- the dominant cost of a large
           batch once the per-write read was removed. Sorting by TKey first makes
           each ReverseInsert append in O(1) (O(N log N) total). Correct
           regardless of sort quality: ReverseInsert always finds the right slot;
           only the scan length depends on the order. */
        std::ranges::sort(deferred_entries, {},
                          [](const auto &entry) -> const Indy::TKey & {
                            return std::get<0>(entry).GetKey();
                          });
        for (auto &entry : deferred_entries) {
          update->AddEntry(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
        }
        transaction->Push(repo, update);
      } catch (const std::bad_alloc &) {
        server->RefuseWriteOutOfMemory();
        throw;
      }
      transaction->Prepare();
      transaction->CommitAction();
      transaction.reset();
    }
    /* Write backpressure on the pools (#584), with memory admission off. The backlog cap was
       held before the commit (#721). See ApplyWriteBackpressure. */
    if (had_effects) {
      ApplyWriteBackpressure(server->GetWriteBackpressureThreshold() && !server->IsMemoryAdmissionOn());
    }
    walker_count = context.GetWalkerCount();
    timer.Stop();
    /* Record per-`Try` stats. TThreadLocalSigmaCalc::Push is lock-free across
       threads (each thread accumulates into its own calculator), so concurrent
       writers/readers no longer serialize here on a global mutex. */
    if (had_effects) {
      TServer::TryWriteTimeCalc.Push(ToSecondsDouble(timer.GetTotal()));
      TServer::TryWriteCallTimerCalc.Push(ToSecondsDouble(call_timer.GetTotal()));
    } else {
      TServer::TryReadTimeCalc.Push(ToSecondsDouble(timer.GetTotal()));
      TServer::TryReadCallTimerCalc.Push(ToSecondsDouble(call_timer.GetTotal()));
    }
    TServer::TryWalkerCountCalc.Push(walker_count);
    TServer::TryWalkerConsTimerCalc.Push(ToSecondsDouble(context.GetPresentWalkConsTimer().GetTotal()));
    TMethodResult method_result(indy_context.GetArena(), result_core, tracker);
    method_result.SetCommitSequenceNumber(commit_seq);
    method_result.SetCommitLsn(commit_lsn);
    return method_result;
  } catch (const TInsufficientStorage &) {
    /* Not an error in the server: the server's admission log records the refusals (#590). */
    throw;
  } catch (const TInsufficientMemory &) {
    /* Likewise (#607). */
    throw;
  } catch (const TWriteTooLarge &) {
    /* A client error, not a server one (#687). */
    throw;
  } catch (const TReadTooLarge &) {
    /* Likewise (#694). */
    throw;
  } catch (const exception &ex) {
    syslog(LOG_ERR, "Error in Session::Try : [%s]", ex.what());
    throw;
  }
}

/* Batched writes. TryBatch (#253) invokes one method against each of N argument
   closures; TryMulti (#255) names a package and method per call. Both run in
   RunBatch, on the SAME context; every call's effects accumulate into one effect
   set (TContext::AddEffect already Augments same-key changes -- the identical
   accumulation a single method doing several `+=` to one key relies on), which
   folds into ONE TUpdate committed ONCE. Mirrors Try() and reuses its exact
   deferred-entry fold; the only differences are the call loop, the per-call
   results, and a batch-shaped meta record. */
TMethodResult TSession::TryBatch(TServer *server, const TUuid &pov_id, const vector<string> &fq_name, const vector<TClosure> &closures) {
  std::vector<TCallView> calls;
  calls.reserve(closures.size());
  for (const auto &closure: closures) {
    calls.push_back(TCallView{&fq_name, &closure});
  }
  std::optional<TTracker> tracker;
  std::optional<uint64_t> commit_seq;
  std::optional<uint64_t> commit_lsn;
  std::vector<Var::TVar> results = RunBatch(server, pov_id, calls, tracker, "TryBatch", &commit_seq, &commit_lsn);
  // Aggregate the N per-call results into one list-typed core (one entry per
  // call, in order); the ws marshal renders it as a JSON array. Every call ran
  // the same method, so the results share a type.
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Var::TVar list_var = Var::TVar::List(results, results.front().GetType());
  TCore list_core(&arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, list_var)).get());
  TMethodResult result(&arena, list_core, tracker);
  result.SetCommitSequenceNumber(commit_seq);
  result.SetCommitLsn(commit_lsn);
  return result;
}

std::vector<Var::TVar> TSession::TryMulti(TServer *server, const TUuid &pov_id, const vector<TBatchCall> &calls) {
  std::vector<TCallView> views;
  views.reserve(calls.size());
  for (const auto &call: calls) {
    views.push_back(TCallView{&call.FqName, &call.Closure});
  }
  std::optional<TTracker> tracker;
  return RunBatch(server, pov_id, views, tracker, "TryMulti");
}

vector<Var::TVar> TSession::RunBatch(TServer *server, const TUuid &pov_id, const vector<TCallView> &calls,
    std::optional<TTracker> &tracker, const char *what,
    std::optional<uint64_t> *commit_seq_out,
    std::optional<uint64_t> *commit_lsn_out) {
  assert(Indy::Fiber::TRunner::LocalRunner);
  assert(!calls.empty());  // grammar guarantees N >= 1
  std::optional<Indy::Fiber::TSwitchToRunner> RunnerSwitcher;
  if (auto *runner = server->NextFastRunner()) {
    RunnerSwitcher.emplace(runner);
  }
  Base::TTimer timer;
  Base::TTimer call_timer;
  bool had_effects = false;
  size_t walker_count = 0UL;
  TSuprena my_arena;
  try {
    void *state_alloc_1 = alloca(Sabot::State::GetMaxStateSize() * 2);
    void *state_alloc_2 = reinterpret_cast<uint8_t *>(state_alloc_1) + Sabot::State::GetMaxStateSize();
    void *call_state_alloc = alloca(Sabot::State::GetMaxStateSize());
    // Open the pov and its repo and prepare the data and package contexts -- ONCE for the whole batch.
    auto pov = server->GetDurableManager()->Open<TPov>(pov_id);
    if (!pov) {
      DEFINE_ERROR(error_t, runtime_error, "unknown pov_id");
      THROW_ERROR(error_t) << pov_id;
    }
    AddPov(pov);
    auto repo = pov->GetRepo(server);
    Indy::TContext context(repo, &my_arena);
    Rt::TOpt<Base::TUuid> user_id;
    if (UserId) {
      user_id = UserId->GetRaw();
    }
    Base::TUuid session_id = GetId().GetRaw();
    Indy::TIndyContext indy_context(user_id, session_id, context, &my_arena, server->GetScheduler(),
      Rt::TOpt<Base::Chrono::TTimePnt>(), Rt::TOpt<uint64_t>());
    // Run each call against the same context. Each call reads the SAME pre-batch
    // snapshot (no read-your-writes within a batch -- this is a write-coalescing
    // primitive, not a transaction script); effects accumulate across calls.
    // A function is resolved again only when the call names a different method
    // than the one before it, so a same-method batch (#253) resolves it once.
    std::vector<Var::TVar> results;
    results.reserve(calls.size());
    const std::vector<std::string> *func_fq_name = nullptr;
    const std::string *func_method_name = nullptr;
    std::shared_ptr<const Package::TFuncHolder> func;
    /* One read budget for the whole batch, as for a single call in Try() (#694). */
    context.SetReadBudget(server->GetReadBudgetRows(), server->GetReadBudgetBytes(), &my_arena,
        server->GetReadBudgetSteps());
    call_timer.Start();
    for (const auto &call: calls) {
      const TClosure &closure = *call.Closure;
      if (!func_fq_name || *func_fq_name != *call.FqName || *func_method_name != closure.GetMethodName()) {
        func = server->GetPackageManager().Get(Package::TName{*call.FqName})->GetFunctionInfo(AsPiece(closure.GetMethodName()));
        func_fq_name = call.FqName;
        func_method_name = &closure.GetMethodName();
      }
      Package::TArgMap prog_args;
      auto arena = closure.GetArena().get();
      for (const auto &item: closure.GetCoreByName()) {
        prog_args.insert(make_pair(item.first, Indy::TKey(item.second, arena)));
      }
      TCore call_core = func->Call(indy_context, prog_args);
      results.push_back(Var::ToVar(*Sabot::State::TAny::TWrapper(
          Indy::TKey(call_core, indy_context.GetArena()).GetState(call_state_alloc))));
    }
    call_timer.Stop();
    context.CheckArenaBudget();
    context.ClearReadBudget();
    Package::TContext::TEffects effects = indy_context.MoveEffects();
    if (!effects.empty()) {
      had_effects = true;
      /* Refuse the write, before it takes memory it would have to flush, while disk space is
         low; a read (no effects) is never refused (#590). */
      server->CheckWriteAdmission();
      RefuseWriteToStalledBacklog(repo, server->GetWriteBackpressureThreshold());
      /* Declared before the transaction so it outlives it (#721): see TBacklogReservation. */
      Indy::TRepo::TBacklogReservation backlog_room(&*repo);
      std::optional<Indy::TSequenceNumber> commit_seq;
      std::optional<uint64_t> commit_lsn;
      auto transaction = server->GetRepoManager()->NewTransaction();
      transaction->ReportCommitSequenceNumber(&commit_seq);
      transaction->ReportCommitLsn(&commit_lsn);
      Indy::TUpdate::TOpByKey op_by_key;
      /* Identical deferred-entry fold to Try() (#49/#232): defer-safe commutative
         mutations skip the read and emit RHS + mutator directly; everything else
         resolves against the snapshot into op_by_key. The effect set here spans
         all N calls, already Augment-merged per key by AddEffect. */
      std::vector<std::tuple<Indy::TIndexKey, Indy::TKey, TMutator>> deferred_entries;
      for (const auto &item: effects) {
        auto key = item.first;
        if (auto *mut = dynamic_cast<const Var::TMutation *>(item.second.get())) {
          if (Var::IsDeferSafeCommutative(mut->GetMutator())) {
            deferred_entries.emplace_back(
                key,
                Indy::TKey(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_2, mut->GetRhs())).get()),
                mut->GetMutator());
            continue;
          }
        }
        Var::TVar val;
        if (!item.second->IsDelete()) {
          if (!item.second->IsFinal()) {
            val = Var::ToVar(*Sabot::State::TAny::TWrapper(context[key].GetState(state_alloc_1)));
          }
          item.second->Apply(val);
          op_by_key[key] =
              Indy::TKey(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_2, val)).get());
        }
        else {
          op_by_key[key] =
              Indy::TKey(Native::TTombstone::Tombstone, &my_arena, state_alloc_2);
        }
      }
      TUuid update_id(TUuid::Twister);
      tracker = TTracker(update_id, seconds(0));
      const auto &predicate_results = indy_context.GetPredicateResults();
      /* One meta record for the whole batch: one update, one tracker, one entry
         (Tetris won't promote a multi-entry update into the global pov). The entry names the
         first call's package and method, and records every call's own (TEntry::EncodeBatch).
         Predicate results span every call, in order. */
      std::vector<TMetaRecord::TEntry::TCall> recorded_calls;
      recorded_calls.reserve(calls.size());
      for (const auto &call: calls) {
        const TClosure &closure = *call.Closure;
        auto closure_arena = closure.GetArena().get();
        TMetaRecord::TEntry::TCall recorded{*call.FqName, closure.GetMethodName(), {}};
        for (const auto &item: closure.GetCoreByName()) {
          recorded.ArgByName.insert(make_pair(
              item.first, Var::ToVar(*Sabot::State::TAny::TWrapper(item.second.NewState(closure_arena, state_alloc_1)))));
        }
        recorded_calls.push_back(std::move(recorded));
      }
      TMetaRecord::TEntry::TArgByName meta_args_by_name = TMetaRecord::TEntry::EncodeBatch(recorded_calls);

      uint64_t random_seed = 0;
      if(indy_context.GetOptRandomSeed().IsKnown()) {
        random_seed = indy_context.GetOptRandomSeed().GetVal();
      }
      const Base::Chrono::TTimePnt run_time = GetRunTime(indy_context.GetOptNow());

      TMetaRecord meta_record(
          update_id,
          TMetaRecord::TEntry(
              GetId(), GetUserId(), *calls.front().FqName, calls.front().Closure->GetMethodName(),
              std::move(meta_args_by_name),
              TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
              run_time, random_seed)
      );
      /* Tetris replays this record's calls to test its predicate results before it promotes the
         update (#751). Read them back as Tetris will, and refuse the write now, before anything
         is committed, if they aren't the calls that ran: an update whose replay can't run would
         be acknowledged and then never promoted, and would fail its POV. */
      if (!predicate_results.empty()) {
        CheckRecordedCalls(meta_record.GetEntry(update_id), recorded_calls);
      }
      /* Hold this write's room in its POV's backlog, waiting for the backlog to drain, or
         refuse it (#721). Before memory admission, so a waiting writer holds no pool blocks.

         The statement has read everything it will (the call, then its effects' resolution
         above), so it drops its context's views first (#722). They pin this repo's and every
         ancestor's mapping and memtable as of the call, and with them the memory layers the
         merges this wait is waiting for have replaced; held across the wait, they keep that
         memory from being freed. The results are already in the arena. */
      context.ReleaseViews();
      ReserveBacklogRoom(server, backlog_room, repo, server->GetWriteBackpressureThreshold(), op_by_key.size() + deferred_entries.size());
      /* Hold this write's room in the update pools, or refuse it, before it builds anything
         there (#607). Released once the transaction below has committed. A write that doesn't
         fit waits a bounded time for the merges to make room (#765), holding no pool blocks, no
         lock and, since the views went above, no memory the merges would free. */
      Indy::TUpdate::TWriteAdmission write_memory;
      server->CheckMemoryAdmission(write_memory, op_by_key.size() + deferred_entries.size());
      /* Nothing is committed until CommitAction, so running out of pool here (the merges can
         take the reserve too) is a refusal, not a failed write (#607). */
      try {
        auto update = Indy::TUpdate::NewUpdate(op_by_key, Indy::TKey(meta_record, &my_arena, state_alloc_1), Indy::TKey(update_id, &my_arena, state_alloc_2));
        std::ranges::sort(deferred_entries, {},
                          [](const auto &entry) -> const Indy::TKey & {
                            return std::get<0>(entry).GetKey();
                          });
        for (auto &entry : deferred_entries) {
          update->AddEntry(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
        }
        transaction->Push(repo, update);
      } catch (const std::bad_alloc &) {
        server->RefuseWriteOutOfMemory();
        throw;
      }
      transaction->Prepare();
      transaction->CommitAction();
      transaction.reset();
      if (commit_seq_out) {
        *commit_seq_out = commit_seq;
      }
      if (commit_lsn_out) {
        *commit_lsn_out = commit_lsn;
      }
    }
    /* Write backpressure on the pools (#584), applied once per batch (one transaction). */
    if (had_effects) {
      ApplyWriteBackpressure(server->GetWriteBackpressureThreshold() && !server->IsMemoryAdmissionOn());
    }
    walker_count = context.GetWalkerCount();
    timer.Stop();
    if (had_effects) {
      TServer::TryWriteTimeCalc.Push(ToSecondsDouble(timer.GetTotal()));
      TServer::TryWriteCallTimerCalc.Push(ToSecondsDouble(call_timer.GetTotal()));
    } else {
      TServer::TryReadTimeCalc.Push(ToSecondsDouble(timer.GetTotal()));
      TServer::TryReadCallTimerCalc.Push(ToSecondsDouble(call_timer.GetTotal()));
    }
    TServer::TryWalkerCountCalc.Push(walker_count);
    TServer::TryWalkerConsTimerCalc.Push(ToSecondsDouble(context.GetPresentWalkConsTimer().GetTotal()));
    return results;
  } catch (const TInsufficientStorage &) {
    /* Not an error in the server: the server's admission log records the refusals (#590). */
    throw;
  } catch (const TInsufficientMemory &) {
    /* Likewise (#607). */
    throw;
  } catch (const TWriteTooLarge &) {
    /* A client error, not a server one (#687). */
    throw;
  } catch (const TReadTooLarge &) {
    /* Likewise (#694). */
    throw;
  } catch (const exception &ex) {
    syslog(LOG_ERR, "Error in Session::%s : [%s]", what, ex.what());
    throw;
  }
}

bool TSession::RunTestSuite(TServer *server,
    const std::vector<std::string> &package_name,
    uint64_t /*package_version*/, bool verbose) {
  assert(server);
  /* The package is installed by the caller (mirrors orlyc's SPA flow:
     Install then RunTestSuite). */
  bool succeeded = true;
  server->GetPackageManager().Get(Package::TName{package_name})->ForEachTest(
      [this, server, &package_name, &succeeded, verbose](const Package::TTest *test) -> bool {
        assert(test);
        /* One paused shared POV per top-level test{} section. The with-block's
           writes land here and are visible to every case via read fallthrough;
           pausing keeps them from being promoted to the global POV.

           Test POVs live only as long as their section or case (#683). They are
           made with a zero ttl and discarded (DiscardTestPov) once the section
           or case is done, which destroys the POV and its repo. They used to be
           held open until the run ended, and a written repo also pinned itself
           (its paused writes are never promoted), so every section kept its
           repos' data layers: the repo data-layer pool ran out after about 300
           cases. */
        Base::TUuid spov = NewFastSharedPov(server, std::optional<Base::TUuid>(), std::chrono::seconds(0));
        TTestPovDiscard discard_spov{this, server, spov};
        PausePov(server, spov);
        if (test->WithBlock) {
          RunFuncCommit(server, package_name,
              [test](Package::TContext &ctx) {
                assert(test->WithBlock->Runner);
                test->WithBlock->Runner(ctx, Package::TArgMap());
              },
              spov);
        }
        succeeded = RunTestBlock(server, package_name, spov, test->SubCases, verbose) && succeeded;
        return true;
      });
  return succeeded;
}

TNotification *TSession::TryGetNotification(uint32_t seq_number) const {
  lock_guard<mutex> lock(NotificationMutex);
  auto iter = NotificationBySeqNumber.find(seq_number);
  return (iter != NotificationBySeqNumber.end()) ? iter->second : nullptr;
}

TMethodResult TSession::TryTracked(TServer */*server*/, const TUuid &/*pov_id*/, const vector<string> &/*fq_name*/, const TClosure &/*closure*/) {
  THROW_ERROR(TStubbed) << "TryTracked";
}

void TSession::UnpausePov(TServer *server, const TUuid &pov_id) {
  assert(server);
  auto pov = server->GetDurableManager()->Open<TPov>(pov_id);
  auto repo = pov->GetRepo(server);
  auto transaction = server->GetRepoManager()->NewTransaction();
  transaction->UnPause(repo);
  transaction->Prepare();
  transaction->CommitAction();
  AddPov(pov);
}

const TUuid TSession::GlobalPovId = Orly::Indy::GlobalPovId;

TSession::TSession(Durable::TManager *manager, const Base::TUuid &id, const Durable::TTtl &ttl)
    : TObj(manager, id, ttl), NextSeqNumber(1) {}

TSession::TSession(Durable::TManager *manager, const Base::TUuid &id, Io::TBinaryInputStream &strm)
    : TObj(manager, id, strm) {
  try {
    size_t size;
    strm >> UserId >> NextSeqNumber >> size;
    for (size_t i = 0; i < size; ++i) {
      pair<uint32_t, TNotification *> item;
      strm >> item.first;
      if (item.first >= NextSeqNumber) {
        syslog(LOG_ERR, "SyntaxError item.first >= NextSeqNumber [%d >= %d]", item.first, NextSeqNumber);
        throw Io::TInputConsumer::TSyntaxError();
      }
      try {
        item.second = Notification::New(strm);
      } catch (...) {
        syslog(LOG_ERR, "Notification::New() error");
      }
      try {
        if (!NotificationBySeqNumber.insert(item).second) {
          syslog(LOG_ERR, "SyntaxError !NotificationBySeqNumber.insert(item).second");
          throw Io::TInputConsumer::TSyntaxError();
        }
      } catch (...) {
        delete item.second;
        throw;
      }
    }
    NotificationSem.Push(size);
  } catch (...) {
    Cleanup();
    throw;
  }
}

TSession::~TSession() {
  Cleanup();
}

void TSession::RunFuncCommit(TServer *server,
    const std::vector<std::string> &package_name,
    const function<void(Package::TContext &ctx)> &func,
    const Base::TUuid &pov_id) {
  assert(server);
  assert(func);

  Durable::TPtr<TPov> pov = server->GetDurableManager()->Open<TPov>(pov_id);
  if (!pov) {
    DEFINE_ERROR(error_t, runtime_error, "unknown pov_id");
    THROW_ERROR(error_t) << pov_id;
  }
  AddPov(pov);
  const Indy::L0::TManager::TPtr<Indy::TRepo> &repo = pov->GetRepo(server);
  TSuprena my_arena;
  Indy::TContext context(repo, &my_arena);
  Rt::TOpt<Base::TUuid> user_id;
  if (UserId) {
    user_id = UserId->GetRaw();
  }
  Base::TUuid session_id = GetId().GetRaw();
  Indy::TIndyContext indy_context(user_id, session_id, context, &my_arena, server->GetScheduler(),
      Rt::TOpt<Base::Chrono::TTimePnt>(), Rt::TOpt<uint64_t>());
  func(indy_context);
  Package::TContext::TEffects effects(indy_context.MoveEffects());

  if (effects.empty()) {
    return;
  }
  server->CheckWriteAdmission();

  /* Commit the effects straight into this POV's repo, resolving every mutation
     against the current value (read-modify-write), exactly as SPA's compile-time
     test path does. We deliberately do NOT use the #49 deferred commutative path
     here: that optimization defers each `+=`/`*=` as a standalone {mutator, rhs}
     entry and relies on the read-time fold to combine them, but the fold cannot
     mix mutators -- a `*=` deferred over a `+=`-built base across the test POV
     chain folds to the wrong value. Compile-time tests are single-threaded, so
     the concurrency benefit of deferral does not apply; resolving now matches
     SPA byte-for-byte (#262). */
  auto transaction = server->GetRepoManager()->NewTransaction();
  Indy::TUpdate::TOpByKey op_by_key;
  void *state_alloc_1 = alloca(Sabot::State::GetMaxStateSize() * 2);
  void *state_alloc_2 = reinterpret_cast<uint8_t *>(state_alloc_1) + Sabot::State::GetMaxStateSize();
  for (const auto &item: effects) {
    Indy::TIndexKey key = item.first;
    if (item.second->IsDelete()) {
      op_by_key[key] =
          Indy::TKey(Native::TTombstone::Tombstone, &my_arena, state_alloc_2);
      continue;
    }
    Var::TVar val;
    if (!item.second->IsFinal()) {
      if (context.Exists(key)) {
        /* Base present (possibly only as a commutative contribution in an
           ancestor test POV, which the read folds): resolve the change against
           it. This is what makes a `*=` over a `+=`-built value -- or any op
           following a different op on the same key across the chain -- come out
           right. */
        val = Var::ToVar(*Sabot::State::TAny::TWrapper(context[key].GetState(state_alloc_1)));
      } else if (auto *mut = dynamic_cast<const Var::TMutation *>(item.second.get());
                 mut && Var::IsDeferSafeCommutative(mut->GetMutator())) {
        /* Absent key + a defer-safe commutative op (`+=`, `*=`, `|=`, min/max):
           bare-commutative upsert from the monoid identity (#151). For these
           monoids identity (+) rhs == rhs, so emit the rhs directly. Reading the
           absent key instead would throw ("could not translate from sabot
           state"); skipping the read matches SPA. */
        op_by_key[key] =
            Indy::TKey(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_2, mut->GetRhs())).get());
        continue;
      } else {
        /* Absent key + a non-commutative op: surface the same error SPA does. */
        val = Var::ToVar(*Sabot::State::TAny::TWrapper(context[key].GetState(state_alloc_1)));
      }
    }
    item.second->Apply(val);
    op_by_key[key] =
        Indy::TKey(&my_arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_2, val)).get());
  }
  TUuid update_id(TUuid::Twister);
  /* Compile-time tests need no replication/notification metadata, so the meta
     record carries empty args; the predicate results still ride along so the
     update is well-formed. */
  const auto &predicate_results = indy_context.GetPredicateResults();
  uint64_t random_seed = 0;
  if (indy_context.GetOptRandomSeed().IsKnown()) {
    random_seed = indy_context.GetOptRandomSeed().GetVal();
  }
  const Base::Chrono::TTimePnt run_time = GetRunTime(indy_context.GetOptNow());
  TMetaRecord meta_record(
      update_id,
      TMetaRecord::TEntry(
          GetId(), GetUserId(), package_name, std::string(),
          TMetaRecord::TEntry::TArgByName(),
          TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
          run_time, random_seed));
  /* No wait at admission (#765): this still holds its context's views, which pin the memory
     the merges would free. */
  Indy::TUpdate::TWriteAdmission write_memory;
  server->CheckMemoryAdmission(write_memory, op_by_key.size(), false);
  /* Nothing is committed until CommitAction, so running out of pool here (the merges can
     take the reserve too) is a refusal, not a failed write (#607). */
  try {
    auto update = Indy::TUpdate::NewUpdate(op_by_key, Indy::TKey(meta_record, &my_arena, state_alloc_1), Indy::TKey(update_id, &my_arena, state_alloc_2));
    transaction->Push(repo, update);
  } catch (const std::bad_alloc &) {
    server->RefuseWriteOutOfMemory();
    throw;
  }
  transaction->Prepare();
  transaction->CommitAction();
}

bool TSession::RunTestBlock(TServer *server,
    const std::vector<std::string> &package_name,
    const Base::TUuid &parent_pov_id,
    const Package::TTestBlock &test_block, bool verbose) {
  assert(server);
  bool result = true;
  for (const auto *test: test_block) {
    assert(test);
    /* Each case gets its own paused shared child of parent_pov_id: it inherits
       the parent's writes (with-block + enclosing case) by read fallthrough,
       its own writes stay isolated from sibling cases (paused => not promoted
       up), and its SubCases run against it so they read-your-writes. */
    Base::TUuid case_pov = NewFastSharedPov(server, parent_pov_id, std::chrono::seconds(0));
    /* Discarded once this case and its SubCases are done, on every path out
       of this iteration (#683; see RunTestSuite). */
    TTestPovDiscard discard_case_pov{this, server, case_pov};
    PausePov(server, case_pov);

    if (verbose) {
      std::cout << test->Loc;
      if (test->Name.size() > 0) {
        std::cout << ' ' << test->Name;
      }
      std::cout << " executing...";
    }

    bool passed = false;
    try {
      RunFuncCommit(server, package_name,
          [test, &passed](Package::TContext &ctx) {
            assert(test->Func);
            assert(test->Func->Runner);
            Atom::TCore::TExtensibleArena *arena = ctx.GetArena();
            Atom::TCore ret = test->Func->Runner(ctx, Package::TArgMap());
            void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
            Sabot::ToNative(*Sabot::State::TAny::TWrapper(ret.NewState(arena, state_alloc)), passed);
          },
          case_pov);

      if (passed) {
        if (verbose) {
          std::cout << " PASSED" << std::endl;
        }
        result = RunTestBlock(server, package_name, case_pov, test->SubCases, verbose) && result;
        continue;
      }
      if (!verbose) {
        std::cout << test->Loc;
        if (test->Name.size()) {
          std::cout << ' ' << test->Name;
        }
      }
      std::cout << " FAILED";
    } catch (const std::exception &ex) {
      passed = false;
      if (!verbose) {
        std::cout << test->Loc;
        if (test->Name.size()) {
          std::cout << ' ' << test->Name;
        }
      }
      std::cout << " FAILED: " << ex.what();
    }

    /* Reached only on failure (the pass path continues above). */
    std::cout << " (child tests will not be executed)" << std::endl;
    result = false;
  }
  return result;
}

void TSession::AddPov(const Durable::TPtr<TPov> &pov) {
  /* Copy the pointer before taking PovMutex, and drop the copy (if unused) after releasing it:
     both take the durable manager's Mutex, which comes first in the lock order (see Povs). */
  Durable::TPtr<TPov> copy = pov;
  std::lock_guard<std::mutex> lock(PovMutex);
  if (find(Povs.begin(), Povs.end(), copy) == Povs.end()) {
    Povs.push_back(std::move(copy));
  }
}

void TSession::DiscardTestPov(TServer *server, const Base::TUuid &pov_id) noexcept {
  assert(server);
  Durable::TPtr<TPov> discarded;
  /* extra */ {
    std::lock_guard<std::mutex> lock(PovMutex);
    auto iter = find_if(Povs.begin(), Povs.end(),
        [&pov_id](const Durable::TPtr<TPov> &pov) { return pov->GetId() == pov_id; });
    if (iter == Povs.end()) {
      return;
    }
    discarded = std::move(*iter);
    Povs.erase(iter);
  }
  try {
    discarded->GetRepo(server)->ReleaseDirtyPin();
  } catch (const std::exception &ex) {
    syslog(LOG_ERR, "Error in Session::DiscardTestPov : [%s]", ex.what());
  }
  /* 'discarded' closes the pov here, outside PovMutex, and with it the repo. */
}

void TSession::Write(Io::TBinaryOutputStream &strm) const {
  lock_guard<mutex> lock(NotificationMutex);
  TObj::Write(strm);
  strm << UserId << NextSeqNumber << NotificationBySeqNumber.size();
  for (const auto &item: NotificationBySeqNumber) {
    strm << item.first;
    Notification::Write(strm, item.second);
  }
}

void TSession::Cleanup() {
  for (const auto &item: NotificationBySeqNumber) {
    delete item.second;
  }
}

TUuid TSession::NewPov(
    TServer *server, const std::optional<Base::TUuid> &parent_pov_id, TPov::TAudience audience, TPov::TPolicy policy, const seconds &time_to_live,
    TConflictMode conflict_mode) {
  assert(server);
  auto durable_manager = server->GetDurableManager();
  TPov::TSharedParents shared_parents;
  if (parent_pov_id) {
    shared_parents = durable_manager->OpenAndVisit<TPov>(*parent_pov_id, [](const TPov &parent) { return parent.GetSharedParents(); });
    shared_parents.push_back(*parent_pov_id);
  }
  auto pov = durable_manager->New<TPov>(TUuid::Twister, time_to_live, GetId(), audience, policy, shared_parents);
  /* The fork (#746): conflicts are tracked from here, before anyone can write to the POV. */
  WatchFork(pov->GetRepo(server), conflict_mode);
  Base::TUuid pov_id = pov->GetId();
  if (policy == TPov::TPolicy::Safe && server->GetWal()) {
    auto recorder = std::make_shared<Io::TRecorder>();
    /* serialize */ {
      Io::TBinaryOutputOnlyStream strm(recorder);
      const bool has_parent = parent_pov_id.has_value();
      const Base::TUuid parent_id = has_parent ? *parent_pov_id : Base::TUuid::Null;
      const char aud = static_cast<char>(audience);
      const char pol = static_cast<char>(policy);
      const int64_t ttl_sec = time_to_live.count();
      const size_t num_parents = shared_parents.size();
      strm << pov_id << GetId() << has_parent << parent_id << aud << pol << ttl_sec << num_parents;
      for (const auto &p_id : shared_parents) {
        strm << p_id;
      }
      strm.Flush();
    }
    std::string wire;
    recorder->CopyOut(wire);
    server->GetWal()->AppendAndWait(Indy::Disk::TWalRecordType::Pov, wire.data(), wire.size());
  }
  AddPov(std::move(pov));
  return pov_id;
}

bool TSession::ForEachDependentPtr(const function<bool (Durable::TAnyPtr &)> &cb) noexcept {
  std::lock_guard<std::mutex> lock(PovMutex);
  for (auto &pov: Povs) {
    if (!cb(pov)) {
      return false;
    }
  }
  Povs.clear();
  return true;
}
