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
#include <optional>

#include <orly/atom/suprena.h>
#include <orly/indy/context.h>
#include <orly/notification/all.h>
#include <orly/server/meta_record.h>
#include <orly/spa/orly_args.h>
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

uint32_t TSession::InsertNotification(TNotification *notification) {
  lock_guard<mutex> lock(NotificationMutex);
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
    Spa::TArgs::TOrlyArg prog_args;
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
      Rt::TOpt<Base::Chrono::TTimePnt>(), Rt::TOpt<uint32_t>());
    // Func it.
    auto func = server->GetPackageManager().Get(Package::TName{fq_name})->GetFunctionInfo(AsPiece(closure.GetMethodName()));
    Package::TContext::TEffects effects;
    call_timer.Start();
    result_core = func->Call(indy_context, prog_args);
    call_timer.Stop();
    effects = indy_context.MoveEffects();
    if (!effects.empty()) {
      had_effects = true;
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
      uint32_t random_seed = 0;
      if(indy_context.GetOptRandomSeed().IsKnown()) {
        random_seed = indy_context.GetOptRandomSeed().GetVal();
      }
      Base::Chrono::TTimePnt run_time = Base::Chrono::CreateTimePnt(2013, 10, 23, 17, 47, 14, 0, 0);
      if(indy_context.GetOptNow().IsKnown()) {
        run_time = indy_context.GetOptNow().GetVal();
      }

      TMetaRecord meta_record(
          update_id,
          TMetaRecord::TEntry(
              GetId(), GetUserId(), fq_name, closure.GetMethodName(),
              TMetaRecord::TEntry::TArgByName(meta_args_by_name.begin(), meta_args_by_name.end()),
              TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
              run_time, random_seed)
      );
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
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* Write backpressure (#234). The transaction above is now destroyed, so its
       Pusher has applied AppendUpdate to `repo`'s memtable. If that memtable has
       backed up past the high-watermark, the global merge is not draining this
       writer fast enough; cooperatively yield this fiber until it drains, so
       sustained accept paces to promote instead of growing the memtable without
       bound (bad_alloc at high K). We hold no repo lock here and the merge runs
       on its own runner, so YieldSlow lets it make progress; the loop re-reads
       the live depth and always terminates because every memtable drains
       (Tetris promote for children, disk merge for safe repos). */
    if (had_effects) {
      const size_t backpressure_threshold = server->GetWriteBackpressureThreshold();
      if (backpressure_threshold) {
        while (repo->GetMemBacklogDepth() > backpressure_threshold) {
          Indy::Fiber::YieldSlow();
        }
      }
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
  } catch (const exception &ex) {
    syslog(LOG_ERR, "Error in Session::Try : [%s]", ex.what());
    throw;
  }
}

/* Batched write (#253). One method, resolved once, invoked against each of N
   argument closures on the SAME context; every call's effects accumulate into
   one effect set (TContext::AddEffect already Augments same-key changes -- the
   identical accumulation a single method doing several `+=` to one key relies
   on), which folds into ONE TUpdate committed ONCE. Mirrors Try() and reuses
   its exact deferred-entry fold; the only differences are the call loop, the
   one-entry-per-call list result, and a batch-shaped meta record. */
TMethodResult TSession::TryBatch(TServer *server, const TUuid &pov_id, const vector<string> &fq_name, const vector<TClosure> &closures) {
  assert(Indy::Fiber::TRunner::LocalRunner);
  assert(!closures.empty());  // grammar guarantees N >= 1
  size_t prev_assignment_count = std::atomic_fetch_add(&server->FastAssignmentCounter, 1UL);
  Indy::Fiber::TSwitchToRunner RunnerSwitcher(server->FastRunnerVec[prev_assignment_count % server->FastRunnerVec.size()].get());
  Base::TTimer timer;
  Base::TTimer call_timer;
  bool had_effects = false;
  std::optional<TTracker> tracker = std::optional<TTracker>();
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
      Rt::TOpt<Base::Chrono::TTimePnt>(), Rt::TOpt<uint32_t>());
    // Resolve the function ONCE (same method for every call in the batch).
    auto func = server->GetPackageManager().Get(Package::TName{fq_name})->GetFunctionInfo(AsPiece(closures.front().GetMethodName()));
    // Run each call against the same context. Each call reads the SAME pre-batch
    // snapshot (no read-your-writes within a batch -- this is a write-coalescing
    // primitive, not a transaction script); effects accumulate across calls.
    std::vector<Var::TVar> results;
    results.reserve(closures.size());
    call_timer.Start();
    for (const auto &closure: closures) {
      Spa::TArgs::TOrlyArg prog_args;
      auto arena = closure.GetArena().get();
      for (const auto &item: closure.GetCoreByName()) {
        prog_args.insert(make_pair(item.first, Indy::TKey(item.second, arena)));
      }
      TCore call_core = func->Call(indy_context, prog_args);
      results.push_back(Var::ToVar(*Sabot::State::TAny::TWrapper(
          Indy::TKey(call_core, indy_context.GetArena()).GetState(call_state_alloc))));
    }
    call_timer.Stop();
    Package::TContext::TEffects effects = indy_context.MoveEffects();
    if (!effects.empty()) {
      had_effects = true;
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
      /* One meta record for the whole batch. The method is shared; each call's
         args are recorded under an index prefix ("<i>.<name>") so all N arg sets
         are preserved losslessly in the flat TArgByName map. Predicate results
         span every call, in order. */
      TMetaRecord::TEntry::TArgByName meta_args_by_name;
      for (size_t i = 0; i < closures.size(); ++i) {
        auto closure_arena = closures[i].GetArena().get();
        std::string prefix = std::to_string(i) + ".";
        for (const auto &item: closures[i].GetCoreByName()) {
          auto arg = Var::ToVar(*Sabot::State::TAny::TWrapper(item.second.NewState(closure_arena, state_alloc_1)));
          meta_args_by_name.insert(make_pair(prefix + item.first, arg));
        }
      }

      uint32_t random_seed = 0;
      if(indy_context.GetOptRandomSeed().IsKnown()) {
        random_seed = indy_context.GetOptRandomSeed().GetVal();
      }
      Base::Chrono::TTimePnt run_time = Base::Chrono::CreateTimePnt(2013, 10, 23, 17, 47, 14, 0, 0);
      if(indy_context.GetOptNow().IsKnown()) {
        run_time = indy_context.GetOptNow().GetVal();
      }

      TMetaRecord meta_record(
          update_id,
          TMetaRecord::TEntry(
              GetId(), GetUserId(), fq_name, closures.front().GetMethodName(),
              TMetaRecord::TEntry::TArgByName(meta_args_by_name.begin(), meta_args_by_name.end()),
              TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
              run_time, random_seed)
      );
      auto update = Indy::TUpdate::NewUpdate(op_by_key, Indy::TKey(meta_record, &my_arena, state_alloc_1), Indy::TKey(update_id, &my_arena, state_alloc_2));
      std::ranges::sort(deferred_entries, {},
                        [](const auto &entry) -> const Indy::TKey & {
                          return std::get<0>(entry).GetKey();
                        });
      for (auto &entry : deferred_entries) {
        update->AddEntry(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
      }
      transaction->Push(repo, update);
      transaction->Prepare();
      transaction->CommitAction();
    }
    /* Write backpressure (#234), applied once per batch (one transaction). */
    if (had_effects) {
      const size_t backpressure_threshold = server->GetWriteBackpressureThreshold();
      if (backpressure_threshold) {
        while (repo->GetMemBacklogDepth() > backpressure_threshold) {
          Indy::Fiber::YieldSlow();
        }
      }
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
    // Aggregate the N per-call results into one list-typed core (one entry per
    // call, in order); the ws marshal renders it as a JSON array. Built in the
    // indy_context arena and deep-copied out by the TMethodResult ctor, exactly
    // as Try() returns its single result_core.
    Var::TVar list_var = Var::TVar::List(results, results.front().GetType());
    TCore list_core(indy_context.GetArena(),
        Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc_1, list_var)).get());
    return TMethodResult(indy_context.GetArena(), list_core, tracker);
  } catch (const exception &ex) {
    syslog(LOG_ERR, "Error in Session::TryBatch : [%s]", ex.what());
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
           pausing keeps them from being promoted to the global POV. */
        Base::TUuid spov = NewFastSharedPov(server, std::optional<Base::TUuid>(), std::chrono::seconds(1000));
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
      Rt::TOpt<Base::Chrono::TTimePnt>(), Rt::TOpt<uint32_t>());
  func(indy_context);
  Package::TContext::TEffects effects(indy_context.MoveEffects());

  if (effects.empty()) {
    return;
  }

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
  uint32_t random_seed = 0;
  if (indy_context.GetOptRandomSeed().IsKnown()) {
    random_seed = indy_context.GetOptRandomSeed().GetVal();
  }
  Base::Chrono::TTimePnt run_time = Base::Chrono::CreateTimePnt(2013, 10, 23, 17, 47, 14, 0, 0);
  if (indy_context.GetOptNow().IsKnown()) {
    run_time = indy_context.GetOptNow().GetVal();
  }
  TMetaRecord meta_record(
      update_id,
      TMetaRecord::TEntry(
          GetId(), GetUserId(), package_name, std::string(),
          TMetaRecord::TEntry::TArgByName(),
          TMetaRecord::TEntry::TExpectedPredicateResults(predicate_results.begin(), predicate_results.end()),
          run_time, random_seed));
  auto update = Indy::TUpdate::NewUpdate(op_by_key, Indy::TKey(meta_record, &my_arena, state_alloc_1), Indy::TKey(update_id, &my_arena, state_alloc_2));
  transaction->Push(repo, update);
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
    Base::TUuid case_pov = NewFastSharedPov(server, parent_pov_id, std::chrono::seconds(1000));
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
  std::lock_guard<std::mutex> lock(PovMutex);
  if (find(Povs.begin(), Povs.end(), pov) == Povs.end()) {
    Povs.push_back(pov);
  }
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
    // TODO: add the ability to open a durable w/o changing ttl
    shared_parents = durable_manager->Open<TPov>(*parent_pov_id)->GetSharedParents();
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
