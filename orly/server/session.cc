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
#include <base/util/time.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Notification;
using namespace Orly::Server;
using namespace Util;

/* Write backpressure (#234, #584), applied after a write's transaction has
   committed: we hold no repo lock, and the merges and Tetris run on their own
   runners, so YieldSlow lets them make progress.

   Two signals. The first is the writer's memtable backlog past the
   high-watermark: the merge isn't promoting this POV fast enough. Every merge
   step copies the whole unpromoted backlog before it frees the old copy, so the
   backlog must stay well below the Update pool's capacity or one merge fills
   the pool (#584). Each POV's watermark is therefore capped at 1/32 of it, so
   several POVs backing up at once still leave room for their merges.

   Only a backlog that Tetris is promoting drains, so that wait applies only
   to one (#626). A paused POV keeps its backlog until it is unpaused, and a
   failed one keeps it for good; a write to either used to wait forever once
   the backlog passed the cap. Such a POV is held to the same cap a different
   way: RefuseWriteToStalledBacklog refuses its writes, before they commit,
   once its backlog has reached the cap. The wait also gives up once the
   backlog has not shrunk for 5 s, so a backlog that stops draining for some
   other reason (the parent's player paused for an import, a deferred Tetris
   join) can't hold a writer forever either.

   The backlog is capped in entries too, at 1/32 of the Update Entry pool
   (#628). The merge copies entries as well as updates, and a batch is a
   single update with an entry per write, so a cap in updates alone let a
   batching POV's backlog hold most of the Entry pool. A single update bigger
   than the cap waits only until it is promoted itself.

   The second is the Update / Update Entry pools past half full, whatever
   holds them: merges need that much room to copy into. That wait is
   bounded, because the pools also hold data that drains slowly or never (a
   fast POV keeps its writes in memory): past the deadline the write proceeds,
   and an allocation that then fails fails just this call.

   With memory admission on (#607) the second wait is skipped: admission keeps
   the merges' room by refusing writes before they allocate, so the wait would
   only make every write that lands past half full sit out its 5 s. Measured
   with a 25% reserve, it held writers to one batch per 5 s each, and reads on
   the same runners took as long. */
static size_t GetBacklogCap(size_t backlog_threshold) {
  return std::min(backlog_threshold, std::max<size_t>(Indy::TUpdate::GetUpdatePoolMaxBlocks() / 32, 1));
}

/* #628: a POV's backlog is capped in entries too, at 1/32 of the Update Entry pool. */
static size_t GetBacklogEntryCap() {
  return std::max<size_t>(Indy::TUpdate::GetEntryPool().GetMaxBlocks() / 32, 1);
}

/* #626: a write to a paused or failed POV whose backlog has reached the cap is refused before
   it commits, with the typed status of #607. Nothing promotes that backlog until the POV is
   unpaused, so waiting for it (as ApplyWriteBackpressure does for a POV that drains) would never
   end.

   Why refuse rather than let such a POV grow? Memory admission (#607) is global: a paused POV
   allowed to grow fills the update pools to the merges' reserve, and then every write to every
   POV is refused until it is unpaused. Measured with the paused-POV smoke (5,000-update pool),
   that happened after 1,900 writes to the paused POV. Held to the cap, a paused POV takes at
   most 1/32 of the Update pool, like any POV whose merges are behind, and the memory merge's
   copy of its backlog stays as small as theirs (#584).

   The cap is checked before commit, so concurrent writers can each pass it once and overshoot it
   by one write apiece; ApplyWriteBackpressure doesn't wait for a backlog that can't drain. */
static void RefuseWriteToStalledBacklog(const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, size_t backlog_threshold) {
  if (!backlog_threshold) {
    return;
  }
  const Indy::TStatus status = repo->GetStatus();
  if (status == Indy::Normal) {
    return;
  }
  const size_t cap = GetBacklogCap(backlog_threshold), entry_cap = GetBacklogEntryCap();
  const size_t backlog = repo->GetMemBacklogDepth();
  /* #628: in entries too, so a paused POV fed batches stops at the same share of the Entry
     pool as any other POV. */
  const size_t entries = repo->GetMemBacklogEntries();
  if (backlog < cap && entries < entry_cap) {
    return;
  }
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

static void ApplyWriteBackpressure(const Indy::L0::TManager::TPtr<Indy::TRepo> &repo, size_t backlog_threshold, bool wait_for_pools) {
  if (!backlog_threshold) {
    return;
  }
  backlog_threshold = GetBacklogCap(backlog_threshold);
  const size_t backlog_entry_threshold = GetBacklogEntryCap();
  constexpr double pool_threshold = 0.5;
  const auto pools_full = [wait_for_pools] {
    return wait_for_pools
        && (Indy::TUpdate::GetUpdatePoolUsedPct() > pool_threshold
            || Indy::TUpdate::GetUpdateEntryPoolUsedPct() > pool_threshold);
  };
  const auto pool_deadline = steady_clock::now() + seconds(5);
  constexpr auto backlog_stall = seconds(5);
  size_t lowest_backlog = std::numeric_limits<size_t>::max();
  auto backlog_deadline = steady_clock::now() + backlog_stall;
  size_t lowest_entries = std::numeric_limits<size_t>::max();
  const auto backlog_over = [&] {
    const size_t backlog = repo->GetMemBacklogDepth();
    /* #628: the entries too, which is what a batch's backlog is made of. */
    const size_t entries = repo->GetMemBacklogEntries();
    if ((backlog <= backlog_threshold && entries <= backlog_entry_threshold) || !repo->IsBacklogDraining()) {
      return false;
    }
    const auto now = steady_clock::now();
    if (backlog < lowest_backlog || entries < lowest_entries) {
      lowest_backlog = std::min(lowest_backlog, backlog);
      lowest_entries = std::min(lowest_entries, entries);
      backlog_deadline = now + backlog_stall;
    }
    return now < backlog_deadline;
  };
  for (;;) {
    if (backlog_over() || (pools_full() && steady_clock::now() < pool_deadline)) {
      Indy::Fiber::YieldSlow();
    } else {
      break;
    }
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
  size_t prev_assignment_count = std::atomic_fetch_add(&server->FastAssignmentCounter, 1UL);
  Indy::Fiber::TSwitchToRunner RunnerSwitcher(server->FastRunnerVec[prev_assignment_count % server->FastRunnerVec.size()].get());
  TCore result_core;
  Base::TTimer timer;
  Base::TTimer call_timer;
  bool had_effects = false;
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
    context.SetReadBudget(server->GetReadBudgetRows(), server->GetReadBudgetBytes(), &my_arena);
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
      auto transaction = server->GetRepoManager()->NewTransaction();
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
      /* Hold this write's room in the update pools, or refuse it, before it builds anything
         there (#607). Released once the transaction below has committed. */
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
    }
    /* Write backpressure (#234). The transaction above is now destroyed, so its
       Pusher has applied AppendUpdate to `repo`'s memtable. If that memtable has
       backed up past the high-watermark, the global merge is not draining this
       writer fast enough; cooperatively yield this fiber until it drains, so
       sustained accept paces to promote instead of growing the memtable without
       bound (bad_alloc at high K). See ApplyWriteBackpressure. */
    if (had_effects) {
      ApplyWriteBackpressure(repo, server->GetWriteBackpressureThreshold(), !server->IsMemoryAdmissionOn());
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
    return TMethodResult(indy_context.GetArena(), result_core, tracker);
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
  std::vector<Var::TVar> results = RunBatch(server, pov_id, calls, tracker, "TryBatch");
  // Aggregate the N per-call results into one list-typed core (one entry per
  // call, in order); the ws marshal renders it as a JSON array. Every call ran
  // the same method, so the results share a type.
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  Var::TVar list_var = Var::TVar::List(results, results.front().GetType());
  TCore list_core(&arena, Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, list_var)).get());
  return TMethodResult(&arena, list_core, tracker);
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
    std::optional<TTracker> &tracker, const char *what) {
  assert(Indy::Fiber::TRunner::LocalRunner);
  assert(!calls.empty());  // grammar guarantees N >= 1
  size_t prev_assignment_count = std::atomic_fetch_add(&server->FastAssignmentCounter, 1UL);
  Indy::Fiber::TSwitchToRunner RunnerSwitcher(server->FastRunnerVec[prev_assignment_count % server->FastRunnerVec.size()].get());
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
    bool mixed = false;
    /* One read budget for the whole batch, as for a single call in Try() (#694). */
    context.SetReadBudget(server->GetReadBudgetRows(), server->GetReadBudgetBytes(), &my_arena);
    call_timer.Start();
    for (const auto &call: calls) {
      const TClosure &closure = *call.Closure;
      if (!func_fq_name || *func_fq_name != *call.FqName || *func_method_name != closure.GetMethodName()) {
        mixed = mixed || func_fq_name;
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
      auto transaction = server->GetRepoManager()->NewTransaction();
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
         (Tetris won't promote a multi-entry update into the global pov). Each
         call's args are recorded under an index prefix ("<i>.<name>") so all N
         arg sets are preserved losslessly in the flat TArgByName map; argument
         names are identifiers, so the prefix can't collide with one. The entry
         names the first call's package and method. In a mixed batch (#255) every
         call's own package and method are recorded too, as "<i>.$package" (path
         joined with '/') and "<i>.$method"; '$' can't appear in an argument name.
         Predicate results span every call, in order. */
      TMetaRecord::TEntry::TArgByName meta_args_by_name;
      for (size_t i = 0; i < calls.size(); ++i) {
        const TClosure &closure = *calls[i].Closure;
        auto closure_arena = closure.GetArena().get();
        std::string prefix = std::to_string(i) + ".";
        for (const auto &item: closure.GetCoreByName()) {
          auto arg = Var::ToVar(*Sabot::State::TAny::TWrapper(item.second.NewState(closure_arena, state_alloc_1)));
          meta_args_by_name.insert(make_pair(prefix + item.first, arg));
        }
        if (mixed) {
          std::string package;
          for (const auto &part: *calls[i].FqName) {
            package += (package.empty() ? "" : "/") + part;
          }
          meta_args_by_name.insert(make_pair(prefix + "$package", Var::TVar(package)));
          meta_args_by_name.insert(make_pair(prefix + "$method", Var::TVar(closure.GetMethodName())));
        }
      }

      uint64_t random_seed = 0;
      if(indy_context.GetOptRandomSeed().IsKnown()) {
        random_seed = indy_context.GetOptRandomSeed().GetVal();
      }
      const Base::Chrono::TTimePnt run_time = GetRunTime(indy_context.GetOptNow());

      TMetaRecord meta_record(
          update_id,
          TMetaRecord::TEntry(
              GetId(), GetUserId(), *calls.front().FqName, calls.front().Closure->GetMethodName(),
              TMetaRecord::TEntry::TArgByName(meta_args_by_name.begin(), meta_args_by_name.end()),
              TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
              run_time, random_seed)
      );
      /* Hold this write's room in the update pools, or refuse it, before it builds anything
         there (#607). Released once the transaction below has committed. */
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
    }
    /* Write backpressure (#234), applied once per batch (one transaction). */
    if (had_effects) {
      ApplyWriteBackpressure(repo, server->GetWriteBackpressureThreshold(), !server->IsMemoryAdmissionOn());
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

void TSession::SeedTestPovSequence(TServer *server, const Base::TUuid &child_pov_id,
    const std::optional<Base::TUuid> &parent_pov_id) {
  assert(server);
  Indy::L0::TManager::TPtr<Indy::TRepo> parent_repo =
      parent_pov_id ? server->GetDurableManager()->Open<TPov>(*parent_pov_id)->GetRepo(server)
                    : server->GetGlobalRepo();
  auto child = server->GetDurableManager()->Open<TPov>(child_pov_id);
  child->GetRepo(server)->SetNextSequenceNumber(parent_repo->GetNextSequenceNumber());
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
        SeedTestPovSequence(server, spov, std::optional<Base::TUuid>());
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
  Indy::TUpdate::TWriteAdmission write_memory;
  server->CheckMemoryAdmission(write_memory, op_by_key.size());
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
    SeedTestPovSequence(server, case_pov, parent_pov_id);

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
    TServer *server, const std::optional<Base::TUuid> &parent_pov_id, TPov::TAudience audience, TPov::TPolicy policy, const seconds &time_to_live) {
  assert(server);
  auto durable_manager = server->GetDurableManager();
  TPov::TSharedParents shared_parents;
  if (parent_pov_id) {
    shared_parents = durable_manager->OpenAndVisit<TPov>(*parent_pov_id, [](const TPov &parent) { return parent.GetSharedParents(); });
    shared_parents.push_back(*parent_pov_id);
  }
  auto pov = durable_manager->New<TPov>(TUuid::Twister, time_to_live, GetId(), audience, policy, shared_parents);
  pov->GetRepo(server);
  Base::TUuid pov_id = pov->GetId();
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
