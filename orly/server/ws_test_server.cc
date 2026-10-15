/* <orly/server/ws_test_server.cc>

   Implements <orly/server/ws_test_server.h>.

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

#include <orly/server/ws_test_server.h>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <syslog.h>

#include <orly/atom/suprena.h>
#include <orly/indy/disk/wal.h>
#include <orly/server/version_unavailable.h>
#include <orly/var/int.h>
#include <orly/var/sabot_to_var.h>
#include <orly/var/util.h>
#include <base/signal/handler_installer.h>
#include <base/signal/masker.h>
#include <base/signal/set.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Server;
using namespace Signal;

/* Our finalization of TWs::TSessionManager. */
/* POV ids whose pause stands for a write the WAL refused (#891); see TSession::PausePov. */
static const TUuid WalRingFullPovId("00000000-0000-0000-0000-0000000008a1");
static const TUuid WalRecordTooLargePovId("00000000-0000-0000-0000-0000000008a2");

class TWsTestServer::TSessionManager
    : public TWs::TSessionManager {
  public:

  /* Do-little. */
  TSessionManager() {}

  virtual const Package::TManager &GetPackageManager() const final {
    static const Package::TManager package_manager({""});
    return package_manager;
  }

  /* Construct a new session and pin it. */
  virtual TWs::TSessionPin *NewSession() override {
    TUuid id(TUuid::Best);
    return (Sessions[id] = unique_ptr<TSession>(new TSession(id)))->NewPin();
  }

  /* Look up an existing session and pin it. */
  virtual TWs::TSessionPin *ResumeSession(const Base::TUuid &id) {
    auto iter = Sessions.find(id);
    if (iter == Sessions.end()) {
      throw invalid_argument("unknown session");
    }
    return iter->second->NewPin();
  }

  virtual bool ForEachIndex(const std::function<
      bool(const std::string &pkg, const std::string &key_type, const std::string &val_type)> &) const final {
    return true;
  }

  private:

  /* A session object, which survives multiple connect/disconnect events.
     In a real server, this would have a time-to-live and be managed as part of an
     in-memory cache.  But we're a fake server so we just let them hang around
     forever. */
  class TSession final {
    NO_COPY(TSession);
    public:

    /* Cache the id as our own and start out without a pin. */
    explicit TSession(const TUuid &id)
        : Id(id), Pin(nullptr) {}

    /* Do-little. */
    void BeginImport() const {}

    /* Do-little. */
    void EndImport() const {}

    /* The id by which we may be looked up and resumed later. */
    const Base::TUuid &GetId() const {
      return Id;
    }

    /* Do-little. */
    void Import(const string &, const string &, int64_t, int64_t, int64_t) const {}

    /* Do-little. */
    void InstallPackage(const vector<string> &/*name*/, uint64_t /*version*/) {}

    /* Create a new pin in this session.  If it's already pinned, throw. */
    TWs::TSessionPin *NewPin() {
      return new TPin(this);
    }

    /* We fake this by just generating a random id. */
    TUuid NewPov(bool /*is_safe*/, bool /*is_shared*/, const std::optional<TUuid> &/*parent_id*/) {
      return TUuid(TUuid::Best);
    }

    /* Do-little, except that the POV ids WalRingFullPovId and WalRecordTooLargePovId stand for a
       write the WAL refused (#891), so a test can see how the reply maps those refusals. */
    void PausePov(const Base::TUuid &pov_id) const {
      if (pov_id == WalRingFullPovId) {
        throw Indy::Disk::TWalRingFull("test: the WAL ring is full");
      }
      if (pov_id == WalRecordTooLargePovId) {
        throw Indy::Disk::TWalRecordTooLarge("test: the WAL record is too large");
      }
    }

    /* Do-little. */
    void SetTtl(const TUuid &/*durable_id*/, const chrono::seconds &/*ttl*/) const {}

    /* Do-little. */
    void SetUserId(const TUuid &/*user_id*/) const {}

    /* Do-little. */
    void Tail() const {}

    /* We fake this by just always returning 98.6, as a write committed at the next version (#750). A read at a
       version (#745) past 1000 is refused as not committed yet, with the oldest readable 10. */
    TMethodResult Try(const TMethodRequest &method_request) {
      static std::atomic<uint64_t> version(100);
      void *alloc = alloca(Sabot::State::GetMaxStateSize());
      const auto &closure = method_request.GetClosure();
      if (const auto at = closure.GetCoreByName().find("$at"); at != closure.GetCoreByName().end()) {
        const auto var = Var::ToVar(*Sabot::State::TAny::TWrapper(at->second.NewState(closure.GetArena().get(), alloc)));
        if (const auto *val = var.TryAs<Var::TInt>(); val && val->GetVal() > 1000) {
          throw TVersionUnavailable(static_cast<uint64_t>(val->GetVal()), 10UL, 1000UL);
        }
      }
      TSuprena arena;
      TMethodResult result(&arena, TCore(98.6, &arena, alloc), std::optional<TTracker>());
      result.SetCommitSequenceNumber(++version);
      return result;
    }

    /* Fake mixed batch: one 98.6 marker per call (#255). */
    std::vector<Var::TVar> TryMulti(const TUuid &/*pov_id*/, const vector<Server::TBatchCall> &calls) {
      return std::vector<Var::TVar>(calls.size(), Var::TVar(98.6));
    }

    /* Fake batch: same 98.6 marker regardless of N (#253). */
    TMethodResult TryBatch(const TUuid &/*pov_id*/, const vector<string> &/*fq_name*/, const vector<TClosure> &/*closures*/) {
      static std::atomic<uint64_t> version(500);
      void *alloc = alloca(Sabot::State::GetMaxStateSize());
      TSuprena arena;
      TMethodResult result(&arena, TCore(98.6, &arena, alloc), std::optional<TTracker>());
      result.SetCommitSequenceNumber(++version);
      return result;
    }

    /* Do-little. */
    void UninstallPackage(const vector<string> &/*name*/, uint64_t /*version*/) {}

    /* Do-little. */
    void UnpausePov(const Base::TUuid &/*pov_id*/) const {}

    private:

    /* Our finalization of TWs::TSessionPin.
       It redirects request to the session object it is pinning. */
    class TPin final
        : public TWs::TSessionPin {
      public:

      /* Pin the given session. */
      TPin(TSession *session)
          : Session(session) {
        if (session->Pin) {
          throw logic_error("session already pinned");
        }
        session->Pin = this;
      }

      /* Unpin the given session.  In a real server, the session would now
            (1) be available to be removed from memory and
            (2) start running its time-to live clock, at the end of which it
                would be available to be removed from disk. */
      virtual ~TPin() {
        if (Session->Pin != this) {
          /* A destructor is noexcept, so throwing here would terminate anyway; fail fast explicitly. */
          syslog(LOG_ERR, "session pin is messed up");
          cerr << "session pin is messed up" << endl;
          abort();
        }
        Session->Pin = nullptr;
      }

      /* Pass-throughs to the session object we have pinned. */
      virtual void BeginImport() const override { Session->BeginImport(); }
      virtual void EndImport() const override { Session->EndImport(); }
      virtual const TUuid &GetId() const override { return Session->GetId(); }
      virtual void Import(
              const string &file_pattern, const string &pkg_name, int64_t num_load_threads,
              int64_t num_merge_threads, int64_t merge_simultaneous) const override {
        Session->Import(file_pattern, pkg_name, num_load_threads, num_merge_threads, merge_simultaneous);
      }
      virtual void InstallPackage(const vector<string> &name, uint64_t version) const override { Session->InstallPackage(name, version); }
      virtual TUuid NewPov(bool is_safe, bool is_shared, const std::optional<TUuid> &parent_id) const override { return Session->NewPov(is_safe, is_shared, parent_id); }
      virtual void PausePov(const TUuid &pov_id) const override { Session->PausePov(pov_id); }
      virtual void SetTtl(const TUuid &durable_id, const chrono::seconds &ttl) const override { Session->SetTtl(durable_id, ttl); }
      virtual void SetUserId(const TUuid &user_id) const override { Session->SetUserId(user_id); }
      virtual void Tail() const override { Session->Tail(); }
      virtual TMethodResult Try(const TMethodRequest &method_request) const override { return Session->Try(method_request); }
      virtual TMethodResult TryBatch(const TUuid &pov_id, const vector<string> &fq_name, const vector<TClosure> &closures) const override { return Session->TryBatch(pov_id, fq_name, closures); }
      virtual std::vector<Var::TVar> TryMulti(const TUuid &pov_id, const vector<Server::TBatchCall> &calls) const override { return Session->TryMulti(pov_id, calls); }
      virtual void UninstallPackage(const vector<string> &name, uint64_t version) const override { Session->UninstallPackage(name, version); }
      virtual void UnpausePov(const TUuid &pov_id) const override { Session->UnpausePov(pov_id); }
      /* Fakes: a random id, and empty results (#746). */
      virtual TUuid NewReviewPov(bool, bool, const std::optional<TUuid> &, TConflictMode) const override { return TUuid(TUuid::Best); }
      virtual TPovDiff DiffPov(const TUuid &, const TPovDiffOptions &) const override { return TPovDiff(); }
      virtual TPovDiscard DiscardPov(const TUuid &) const override { return TPovDiscard(); }
      virtual std::optional<uint64_t> GetDurableVersion(const TUuid &pov_id) const override {
        /* POV ...0002 is always durable up to any version a fake write gets (#750); the rest stay at 77. */
        return pov_id == TUuid("00000000-0000-0000-0000-000000000002") ? 1000000UL : 77UL;
      }
      /* Fakes for the versioned reads' statements (#889). POV ...0001 is the global one, with version 1000 now, which
         has a save point `known` at 42 and one `gone` below the horizon. Any time from 2000 to now is version 500;
         an earlier one is older than the window. */
      virtual uint64_t ResolveTime(std::chrono::system_clock::time_point when) const override {
        if (when > std::chrono::system_clock::now()) {
          throw Server::TTimeUnavailable("that time is in the future", 10UL, 1000UL);
        }
        if (when < std::chrono::system_clock::time_point(std::chrono::hours(24 * 10957))) {
          throw Server::TTimeUnavailable("that time is older than the oldest the retention window has samples for", 10UL, 1000UL);
        }
        return 500UL;
      }
      virtual uint64_t ResolveSavePoint(const string &name) const override {
        if (name == "known") {
          return 42UL;
        }
        if (name == "gone") {
          throw Server::TVersionUnavailable(5UL, 10UL, 1000UL);
        }
        throw invalid_argument("no save point named \"" + name + "\"");
      }
      virtual Server::TVersionOf GetVersionOf(const TUuid &pov_id) const override {
        Server::TVersionOf result;
        if (pov_id == TUuid("00000000-0000-0000-0000-000000000001")) {
          result.Number = 1000UL;
        } else {
          result.Vector = "(3, 1000)";
        }
        return result;
      }
      virtual void SetSavePoint(const TUuid &pov_id, const string &name, const optional<uint64_t> &at) const override {
        if (pov_id != TUuid("00000000-0000-0000-0000-000000000001") && at) {
          throw invalid_argument("save point: a POV other than the global one is saved at its newest version");
        }
        if (at && (*at < 10UL || *at > 1000UL)) {
          throw Server::TVersionUnavailable(*at, 10UL, 1000UL);
        }
        if (name.empty()) {
          throw invalid_argument("save point: the name must not be empty");
        }
      }
      virtual bool DropSavePoint(const TUuid &, const string &name) const override { return name == "known"; }
      virtual TUuid HoldVersion(const TUuid &pov_id, uint64_t at, const chrono::seconds &) const override {
        if (pov_id != TUuid("00000000-0000-0000-0000-000000000001")) {
          throw invalid_argument("hold version: only the global POV's versions can be held");
        }
        if (at < 10UL || at > 1000UL) {
          throw Server::TVersionUnavailable(at, 10UL, 1000UL);
        }
        return TUuid("00000000-0000-0000-0000-0000000000aa");
      }
      virtual bool ReleaseHold(const TUuid &hold_id) const override { return hold_id == TUuid("00000000-0000-0000-0000-0000000000aa"); }
      virtual TPovPromote PromotePov(const TUuid &, bool) const override { return TPovPromote(); }
      virtual TPovReview ReviewPov(const TUuid &, uint64_t) const override { return TPovReview(); }

      private:

      /* The session object we have pinned. */
      TSession *Session;

    };  // TwsTestServer::TSessionManager::TSession::TPin

    /* See accessor. */
    TUuid Id;

    /* Our current pin, if any. */
    TPin *Pin;

  };  // TwsTestServer::TSessionManager::TSession

  /* All the sessions currently in memory. */
  unordered_map<TUuid, unique_ptr<TSession>> Sessions;

};  // TSessionManager

TWsTestServer::TWsTestServer(in_port_t port_start, size_t probe_size, const std::string &auth_token)
    : SessionManager(nullptr), PortNumber(port_start), Ws(nullptr) {
  assert(probe_size);
  try {
    SessionManager = new TSessionManager;
    for (;;) {
      try {
        Ws = TWs::New(SessionManager, 4, PortNumber, "127.0.0.1", false, auth_token);
        break;
      } catch (...) {
        --probe_size;
        if (!probe_size) {
          throw;
        }
        ++PortNumber;
      }
    }  // for
  } catch (...) {
    this->~TWsTestServer();
    throw;
  }
}

TWsTestServer::~TWsTestServer() {
  delete Ws;
  delete SessionManager;
}