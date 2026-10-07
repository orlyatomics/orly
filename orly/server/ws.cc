/* <orly/server/ws.cc>

   Implements <orly/server/ws.h>.

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

#include <orly/server/ws.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <syslog.h>
#include <thread>
#include <vector>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <base/as_str.h>
#include <base/fd.h>
#include <base/json.h>
#include <base/tmp_copy_to_file.h>
#include <base/tmp_dir_maker.h>
#include <orly/auth.h>
#include <orly/compiler.h>
#include <orly/error.h>
#include <orly/client/program/parse_stmt.h>
#include <orly/client/program/translate_expr.h>
#include <orly/indy/key.h>
#include <orly/orly.package.cst.h>
#include <orly/server/insufficient_memory.h>
#include <orly/server/insufficient_storage.h>
#include <orly/server/read_too_large.h>
#include <orly/server/write_too_large.h>
#include <orly/sabot/state_dumper.h>
#include <orly/sabot/type_dumper.h>
#include <orly/synth/cst_utils.h>
#include <orly/type/orlyify.h>
#include <orly/var/jsonify.h>
#include <orly/var/sabot_to_var.h>

using namespace std;

using namespace Base;
using namespace Util;

using namespace Orly;
using namespace Orly::Client::Program;
using namespace Orly::Sabot;
using namespace Orly::Server;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

  /* The error the `compile` statement gets when the server was started without
     --allow_remote_compile (#705). Reported as "status": "remote_compile_disabled". */
  class TRemoteCompileDisabled
      : public std::runtime_error {
    public:

    TRemoteCompileDisabled()
        : std::runtime_error(
              "remote compile disabled: this server does not accept the compile statement; "
              "compile packages with orlyc and install them, or start orlyi with --allow_remote_compile") {}

  };  // TRemoteCompileDisabled

  /* Fill in a reply's "result" and "status" for the exception being handled.  Call only from
     inside a catch block. */
  void SetErrorReply(TJson &reply) {
    try {
      throw;
    } catch (const TSourceError &src_error) {
      reply["result"] = src_error.what();
      reply["pos"] = AsStr(src_error.GetPosRange());
      /* Kept out of "result" so a client shows a clean message; the
         compiler line is there for whoever is reporting a bug (#557). */
      reply["compiler_loc"] = AsStr(src_error.GetCodeLocation());
      reply["status"] = "source_error";
    } catch (const Orly::Server::TInsufficientStorage &ex) {
      /* A write refused for lack of disk space (#590): its own status, so a client can tell
         it from a failed statement and keep reading. */
      reply["result"] = ex.what();
      reply["status"] = "insufficient_storage";
    } catch (const Orly::Server::TInsufficientMemory &ex) {
      /* A write refused because the update pools are down to the merges' reserve (#607). */
      reply["result"] = ex.what();
      reply["status"] = "insufficient_memory";
    } catch (const Orly::Server::TWriteTooLarge &ex) {
      /* A write too big ever to be promoted (#687). Not retryable: the client must split it. */
      reply["result"] = ex.what();
      reply["status"] = "write_too_large";
    } catch (const Orly::Server::TReadTooLarge &ex) {
      /* A read that walked or built more than the per-read budget (#694). Not retryable as
         sent: the client must read less. */
      reply["result"] = ex.what();
      reply["status"] = "read_too_large";
    } catch (const TRemoteCompileDisabled &ex) {
      /* `compile` on a server started without --allow_remote_compile (#705). */
      reply["result"] = ex.what();
      reply["status"] = "remote_compile_disabled";
    } catch (const exception &ex) {
      reply["result"] = ex.what();
      reply["status"] = "exception";
    } catch (...) {
      reply["result"] = "unknown exception";
      reply["status"] = "exception";
    }
  }

  /* The statements that run off the I/O threads (#761): try, batch and multi.  Each is a work
     function, run by TWs::TSessionManager::RunStatement() (on the server, a fiber on a fast
     runner), that calls the session and returns a finisher; the finisher turns the result into
     the reply's JSON back on the connection's strand, so result rendering keeps the I/O
     thread's stack and the fiber does only the statement.  A done callback carries the finisher,
     or the exception the work threw, back to the connection.

     At most MaxInFlight work functions run at once; the rest wait here in arrival order.  Each
     in-flight statement holds a fiber frame for as long as it runs, plus whatever frames the
     read itself fans out to, so an unbounded number of them would empty the frame pool (#762).
     Since a connection has one statement in flight at most, the wait queue is never longer
     than the number of connections. */
  class TStmtQueue final
      : public std::enable_shared_from_this<TStmtQueue> {
    NO_COPY(TStmtQueue);
    public:

    using TFinish = std::function<TJson ()>;
    using TWork = std::function<TFinish ()>;
    using TDone = std::function<void (TFinish &&, std::exception_ptr)>;

    TStmtQueue(TWs::TSessionManager *session_mngr, size_t max_in_flight)
        : SessionManager(session_mngr), MaxInFlight(max_in_flight) {}

    /* Run the work now if there's room, or once there is.  done is called exactly once, from
       whichever thread finished the work (or refused it), never from inside this call while
       the caller holds anything of ours. */
    void Submit(TWork &&work, TDone &&done) {
      /* lock */ {
        std::unique_lock<std::mutex> lock(Mutex);
        if (Stopping) {
          lock.unlock();
          done(TFinish(), std::make_exception_ptr(std::runtime_error("server shutting down")));
          return;
        }
        if (MaxInFlight && InFlight >= MaxInFlight) {
          Waiting.emplace_back(std::move(work), std::move(done));
          return;
        }
        ++InFlight;
      }
      Launch(std::move(work), std::move(done));
    }

    /* Refuse what's waiting and wait for what's running.  After this returns, nothing of ours
       runs or calls a done callback again.  The wait has no deadline, as the I/O threads'
       join had before #761: a statement in flight is a client's write or read the server
       already took, and the rest of the shutdown (TServer::Shutdown()) relies on the runners
       still being up while it finishes. */
    void Stop() {
      std::deque<std::pair<TWork, TDone>> refused;
      /* lock */ {
        std::lock_guard<std::mutex> lock(Mutex);
        Stopping = true;
        refused.swap(Waiting);
      }
      for (auto &item: refused) {
        item.second(TFinish(), std::make_exception_ptr(std::runtime_error("server shutting down")));
      }
      refused.clear();
      std::unique_lock<std::mutex> lock(Mutex);
      while (InFlight) {
        if (Idle.wait_for(lock, std::chrono::seconds(10)) == std::cv_status::timeout && InFlight) {
          syslog(LOG_WARNING, "ws: shutdown waiting for [%zu] statement(s) in flight (#761)", InFlight);
        }
      }
    }

    private:

    /* Hand one admitted work function to the session manager.  If it can't take it (no fiber
       frame, say), report the failure and give the slot to the next waiting statement. */
    void Launch(TWork &&work, TDone &&done) {
      for (;;) {
        /* Shared so the failure paths below still have them after the closure is gone. */
        auto shared_done = std::make_shared<TDone>(std::move(done));
        TWork work_for_retry = work;
        /* Run the work, report it, then free its slot.  The order matters for Stop(): the
           done callback must have run before InFlight can reach zero. */
        std::function<void ()> run =
            [self = shared_from_this(), work = std::move(work), shared_done]() mutable {
          TFinish finish;
          std::exception_ptr error;
          try {
            finish = work();
          } catch (...) {
            error = std::current_exception();
          }
          /* Drop the work's captures (the session pin, the request) here, before replying. */
          work = nullptr;
          /* Moved, not copied: the exception object must have one owner at a time, or this
             thread's copy dies after the strand has read it, through a refcount TSan can't see
             (it lives in uninstrumented libstdc++). */
          (*shared_done)(std::move(finish), std::move(error));
          *shared_done = nullptr;
          self->Finished();
        };
        try {
          SessionManager->RunStatement(std::move(run));
          return;
        } catch (const Orly::Server::TInsufficientMemory &ex) {
          /* No fiber frame free (#762). If another statement is in flight, its end frees one and
             starts the next waiting statement, so put this one back at the head of the queue and
             give up its slot. With nothing else in flight, nothing would start it again: refuse
             it, typed and retryable. A fresh exception, as below. */
          {
            std::lock_guard<std::mutex> lock(Mutex);
            if (!Stopping && InFlight > 1) {
              Waiting.emplace_front(std::move(work_for_retry), std::move(*shared_done));
              --InFlight;
              Idle.notify_all();
              return;
            }
          }
          (*shared_done)(TFinish(), std::make_exception_ptr(Orly::Server::TInsufficientMemory(ex.what())));
        } catch (const std::exception &ex) {
          /* RunStatement() promises the work didn't run and never will.  A fresh exception, so
             the one this handler holds isn't shared with the strand (see above). */
          syslog(LOG_ERR, "ws: could not start a statement: %s (#761)", ex.what());
          (*shared_done)(TFinish(), std::make_exception_ptr(std::runtime_error(
              std::string("could not start the statement: ") + ex.what())));
        } catch (...) {
          syslog(LOG_ERR, "ws: could not start a statement (#761)");
          (*shared_done)(TFinish(), std::make_exception_ptr(std::runtime_error("could not start the statement")));
        }
        /* That slot is free again; give it to the next waiting statement, if any. */
        std::lock_guard<std::mutex> lock(Mutex);
        if (Stopping || Waiting.empty()) {
          --InFlight;
          Idle.notify_all();
          return;
        }
        work = std::move(Waiting.front().first);
        done = std::move(Waiting.front().second);
        Waiting.pop_front();
      }
    }

    /* A statement finished; start the next waiting one in its slot, if any. */
    void Finished() {
      TWork work;
      TDone done;
      /* lock */ {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Stopping || Waiting.empty()) {
          --InFlight;
          Idle.notify_all();
          return;
        }
        work = std::move(Waiting.front().first);
        done = std::move(Waiting.front().second);
        Waiting.pop_front();
      }
      Launch(std::move(work), std::move(done));
    }

    TWs::TSessionManager *const SessionManager;

    const size_t MaxInFlight;

    /* Covers everything below. */
    std::mutex Mutex;

    /* Notified when InFlight drops. */
    std::condition_variable Idle;

    /* Admitted and not yet finished. */
    size_t InFlight = 0;

    /* Statements waiting for a slot, in arrival order. */
    std::deque<std::pair<TWork, TDone>> Waiting;

    /* Set by Stop(). */
    bool Stopping = false;

  };  // TStmtQueue

}  // namespace

/* The implementation of the TWs interface declared in the header.

   Each connection runs on its own asio strand, so per-connection handlers
   serialize across the io_context's thread pool. The Conns set is guarded
   by Mutex; the strands cover everything else. */
/* The compile scratch dir must be unique per server instance: a fixed
   machine-global path breaks concurrent runs (two checkouts, or make test
   racing a live orlyi) and cross-user runs (a root-invoked orlyi leaves a
   root-owned dir that aborts every later non-root ws.test at teardown),
   #444. */
static std::string MakeCompileTmpDir() {
  std::string path = Util::MakePath({ P_tmpdir }, { "orly_ws_compile.XXXXXX" });
  if (!mkdtemp(path.data())) {
    throw std::system_error(errno, std::generic_category(), "mkdtemp for ws compile dir");
  }
  return path;
}

class TWsImpl final
    : public TWs {
  public:

  /* Starts up the server. */
  TWsImpl(
      TSessionManager *session_mngr, size_t thread_count,
      in_port_t port_number, const std::string &bind_address, bool allow_remote_compile,
      const std::string &auth_token, size_t max_in_flight)
      : SessionManager(session_mngr),
        AllowRemoteCompile(allow_remote_compile),
        AuthToken(auth_token),
        StmtQueue(std::make_shared<TStmtQueue>(session_mngr, max_in_flight)),
        TmpDirMaker(MakeCompileTmpDir()),
        IoCtx(thread_count ? static_cast<int>(thread_count) : 1),
        Acceptor(IoCtx) {
    assert(session_mngr);
    syslog(LOG_INFO, "ws compile tmp dir = \"%s\"", TmpDirMaker.GetPath().c_str());

    boost::system::error_code ec;
    const auto address = net::ip::make_address(bind_address, ec);
    if (ec) {
      throw std::invalid_argument("ws bind address \"" + bind_address + "\" is not an IPv4 or IPv6 address");
    }
    tcp::endpoint endpoint(address, port_number);
    Acceptor.open(endpoint.protocol());
    Acceptor.set_option(net::socket_base::reuse_address(true));
    Acceptor.bind(endpoint);
    Acceptor.listen(8192);
    /* Log what the kernel gave us: a port of 0 asks for an ephemeral one. */ {
      const auto bound = Acceptor.local_endpoint();
      std::ostringstream strm;
      strm << bound;
      syslog(LOG_INFO, "websocket listener bound to %s; remote compile %s%s; %zu I/O thread(s), "
             "%zu statement(s) in flight at most (0: no limit)",
             strm.str().c_str(), allow_remote_compile ? "allowed" : "disabled",
             auth_token.empty() ? "" : "; token required", thread_count ? thread_count : 1, max_in_flight);
      /* LOG_ERR, not LOG_WARNING: the default log mask shows only errors, and an operator must
         see this one without asking for more. */
      if (allow_remote_compile && auth_token.empty() && !bound.address().is_loopback()) {
        syslog(LOG_ERR,
               "WARNING: websocket listener on %s accepts compile from any client that reaches it; "
               "Orly has no authentication",
               strm.str().c_str());
      }
    }

    DoAccept();

    const size_t n = thread_count ? thread_count : 1;
    BgThreads.reserve(n);
    try {
      for (size_t i = 0; i < n; ++i) {
        BgThreads.emplace_back([this] { IoCtx.run(); });
      }
    } catch (...) {
      Shutdown();
      throw;
    }
  }

  /* Shuts down the server. */
  ~TWsImpl() override {
    Shutdown();
  }

  private:

  class TConn;

  /* Refuses queued statements and waits for those in flight (#761), then
     stops the io_context, joins the worker threads, and clears the
     connection set. Idempotent so the dtor can call it after a partially-
     constructed start.  The I/O threads keep running through the wait, so
     the replies of the statements that finish are sent. */
  void Shutdown() {
    StmtQueue->Stop();
    if (!IoCtx.stopped()) {
      IoCtx.stop();
    }
    for (auto &t : BgThreads) {
      if (t.joinable()) {
        t.join();
      }
    }
    lock_guard<mutex> lock(Mutex);
    Conns.clear();
  }

  /* Queue the next accept. Self-perpetuating chain. */
  void DoAccept();

  /* Per-connection state.  Constructed by DoAccept(), held in Conns until
     the connection closes or errors out.  Pinned by shared_ptr inside its
     own async handlers so it survives until the last in-flight op
     completes. */
  class TConn final : public std::enable_shared_from_this<TConn> {
    NO_COPY(TConn);
    public:

    TConn(TWsImpl *ws, tcp::socket sock)
        : Ws(ws),
          WsStream(beast::tcp_stream(std::move(sock))),
          AuthTimer(WsStream.get_executor()),
          Authenticated(ws->AuthToken.empty()) {}

    /* Perform the WS handshake then start the read loop. */
    void Run() {
      if (!Authenticated) {
        /* With a token (#710), a connection that hasn't authenticated in time is closed, so an
           unauthenticated client can't hold a connection open. */
        AuthTimer.expires_after(AuthDeadline);
        AuthTimer.async_wait([self = shared_from_this()](beast::error_code ec) {
          if (!ec && !self->Authenticated) {
            beast::error_code ignored;
            beast::get_lowest_layer(self->WsStream).socket().shutdown(tcp::socket::shutdown_both, ignored);
            beast::get_lowest_layer(self->WsStream).socket().close(ignored);
          }
        });
      }
      WsStream.async_accept(
          [self = shared_from_this()](beast::error_code ec) {
            if (ec) {
              self->OnError(ec, "handshake");
              return;
            }
            self->DoRead();
          });
    }

    private:

    /* Interpret a statement.  Identical to the previous implementation --
       this is the Orly business logic; the only thing that changed is
       what's sitting between it and the wire. */
    class TStmtVisitor final
        : public TStmt::TVisitor {
      public:

      TStmtVisitor(TConn *conn, TJson &result)
          : Conn(conn), Result(result) {}

      virtual void operator()(const TEchoStmt *stmt) const override {
        assert(stmt);
        void *alloc = alloca(SabotStateSize);
        Result = Var::ToJson(Var::ToVar(*TWrapper(NewStateSabot(stmt->GetExpr(), alloc))));
      }

      virtual void operator()(const TExitStmt *) const override {
        Conn->Exiting = true;
      }

      virtual void operator()(const TNewSessionStmt *) const override {
        if (Conn->Session) {
          throw invalid_argument("session already established");
        }
        Conn->Session.reset(Conn->Ws->SessionManager->NewSession());
        Result = AsStr(Conn->Session->GetId());
      }

      virtual void operator()(const TResumeSessionStmt *stmt) const override {
        assert(stmt);
        if (Conn->Session) {
          throw invalid_argument("session already established");
        }
        Conn->Session.reset(Conn->Ws->SessionManager->ResumeSession(Translate(stmt->GetIdExpr())));
        Result = AsStr(Conn->Session->GetId());
      }

      virtual void operator()(const TSetUserIdStmt *stmt) const override {
        assert(stmt);
        TUuid user_id = Translate(stmt->GetIdExpr());
        GetSession()->SetUserId(user_id);
      }

      virtual void operator()(const TSetTtlStmt *stmt) const override {
        assert(stmt);
        TUuid durable_id = Translate(stmt->GetIdExpr());
        chrono::seconds ttl(stmt->GetIntExpr()->GetLexeme().AsInt());
        GetSession()->SetTtl(durable_id, ttl);
      }

      virtual void operator()(const TInstallStmt *stmt) const override {
        assert(stmt);
        vector<string> package_name;
        uint64_t version;
        TranslatePackage(package_name, version, stmt->GetPackageName());
        GetSession()->InstallPackage(package_name, version);
      }

      virtual void operator()(const TUninstallStmt *stmt) const override {
        assert(stmt);
        vector<string> package_name;
        uint64_t version;
        TranslatePackage(package_name, version, stmt->GetPackageName());
        GetSession()->UninstallPackage(package_name, version);
      }

      virtual void operator()(const TPovConsStmt *stmt) const override {
        assert(stmt);
        bool is_safe = dynamic_cast<const TSafeGuarantee *>(stmt->GetPovGuarantee()) != nullptr;
        bool is_shared = dynamic_cast<const TSharedKind *>(stmt->GetPovKind()) != nullptr;
        std::optional<TUuid> parent_id;
        auto parent = dynamic_cast<const TParent *>(stmt->GetOptParent());
        if (parent) {
          parent_id = Translate(parent->GetIdExpr());
        }
        Result = AsStr(GetSession()->NewPov(is_safe, is_shared, parent_id));
      }

      virtual void operator()(const TTryStmt *stmt) const override {
        assert(stmt);
        TUuid pov_id = Translate(stmt->GetPovId());
        vector<string> fq_name;
        TranslatePathName(fq_name, stmt->GetPackage());
        TClosure closure(stmt->GetMethodName()->GetLexeme().GetText());
        auto list = dynamic_cast<const TObjMemberList *>(stmt->GetArgs()->GetOptObjMemberList());
        void *alloc = alloca(SabotStateSize);
        while (list) {
          auto member = list->GetObjMember();
          TWrapper state(NewStateSabot(member->GetExpr(), alloc));
          closure.AddArgBySabot(member->GetName()->GetLexeme().GetText(), state);
          auto tail = dynamic_cast<const TObjMemberListTail *>(list->GetOptObjMemberListTail());
          list = tail ? tail->GetObjMemberList() : nullptr;
        }
        /* Runs off the I/O thread (#761); see TStmtQueue. */
        Conn->Deferred = [session = GetSharedSession(),
                          request = std::make_shared<const TMethodRequest>(pov_id, fq_name, closure)] {
          auto result = std::make_shared<TMethodResult>(session->Try(*request));
          return TStmtQueue::TFinish([result] { return ToJson(*result); });
        };
      }

      virtual void operator()(const TTryBatchStmt *stmt) const override {
        assert(stmt);
        TUuid pov_id = Translate(stmt->GetPovId());
        vector<string> fq_name;
        TranslatePathName(fq_name, stmt->GetPackage());
        string method_name = stmt->GetMethodName()->GetLexeme().GetText();
        void *alloc = alloca(SabotStateSize);
        /* Build one TClosure per argument record in the bracketed list, reusing
           the same per-record member walk as TTryStmt. The server folds all N
           calls into a single transaction (#253). */
        std::vector<TClosure> closures;
        auto list = stmt->GetObjExprList();
        while (list) {
          TClosure closure(method_name);
          auto members = dynamic_cast<const TObjMemberList *>(list->GetObjExpr()->GetOptObjMemberList());
          while (members) {
            auto member = members->GetObjMember();
            TWrapper state(NewStateSabot(member->GetExpr(), alloc));
            closure.AddArgBySabot(member->GetName()->GetLexeme().GetText(), state);
            auto member_tail = dynamic_cast<const TObjMemberListTail *>(members->GetOptObjMemberListTail());
            members = member_tail ? member_tail->GetObjMemberList() : nullptr;
          }
          closures.push_back(std::move(closure));
          auto list_tail = dynamic_cast<const TObjExprListTail *>(list->GetOptObjExprListTail());
          list = list_tail ? list_tail->GetObjExprList() : nullptr;
        }
        /* Runs off the I/O thread (#761); see TStmtQueue. */
        Conn->Deferred = [session = GetSharedSession(), pov_id,
                          fq_name = std::make_shared<const vector<string>>(std::move(fq_name)),
                          closures = std::make_shared<const std::vector<TClosure>>(std::move(closures))] {
          auto result = std::make_shared<TMethodResult>(session->TryBatch(pov_id, *fq_name, *closures));
          return TStmtQueue::TFinish([result] { return ToJson(*result); });
        };
      }

      virtual void operator()(const TTryMultiStmt *stmt) const override {
        assert(stmt);
        TUuid pov_id = Translate(stmt->GetPovId());
        void *alloc = alloca(SabotStateSize);
        /* One call per element, each naming its own package and method; the server folds
           them all into a single transaction (#255). */
        std::vector<Orly::Server::TBatchCall> calls;
        auto list = stmt->GetBatchCallList();
        while (list) {
          auto call = list->GetBatchCall();
          Orly::Server::TBatchCall batch_call{{}, TClosure(call->GetMethodName()->GetLexeme().GetText())};
          TranslatePathName(batch_call.FqName, call->GetPackage());
          auto members = dynamic_cast<const TObjMemberList *>(call->GetArgs()->GetOptObjMemberList());
          while (members) {
            auto member = members->GetObjMember();
            TWrapper state(NewStateSabot(member->GetExpr(), alloc));
            batch_call.Closure.AddArgBySabot(member->GetName()->GetLexeme().GetText(), state);
            auto member_tail = dynamic_cast<const TObjMemberListTail *>(members->GetOptObjMemberListTail());
            members = member_tail ? member_tail->GetObjMemberList() : nullptr;
          }
          calls.push_back(std::move(batch_call));
          auto list_tail = dynamic_cast<const TBatchCallListTail *>(list->GetOptBatchCallListTail());
          list = list_tail ? list_tail->GetBatchCallList() : nullptr;
        }
        /* Runs off the I/O thread (#761); see TStmtQueue. */
        Conn->Deferred = [session = GetSharedSession(), pov_id,
                          calls = std::make_shared<const std::vector<Orly::Server::TBatchCall>>(std::move(calls))] {
          auto vars = std::make_shared<std::vector<Var::TVar>>(session->TryMulti(pov_id, *calls));
          /* The results may differ in type, so they come back as separate values: one JSON
             array element per call, in order. */
          return TStmtQueue::TFinish([vars] {
            TJson::TArray results;
            for (const auto &var: *vars) {
              results.push_back(Var::ToJson(var));
            }
            return TJson(std::move(results));
          });
        };
      }

      virtual void operator()(const TPovStatusStmt *stmt) const override {
        assert(stmt);
        bool is_pause = dynamic_cast<const TPauseKind *>(stmt->GetStatusKind()) != nullptr;
        TUuid pov_id = Translate(stmt->GetIdExpr());
        if (is_pause) {
          GetSession()->PausePov(pov_id);
          Result = "paused";
        } else {
          GetSession()->UnpausePov(pov_id);
          Result = "unpaused";
        }
      }

      virtual void operator()(const TTailStmt *) const override {
        GetSession()->Tail();
      }

      virtual void operator()(const TBeginImportStmt *) const override {
        GetSession()->BeginImport();
      }

      virtual void operator()(const TEndImportStmt *) const override {
        GetSession()->EndImport();
      }

      virtual void operator()(const TImportStmt *stmt) const override {
        assert(stmt);
        string path = Translate(stmt->GetFile());
        int64_t
            load_threads = Translate(stmt->GetLoadThreads()),
            merge_threads = Translate(stmt->GetMergeThreads()),
            merge_sim = Translate(stmt->GetMergeSim());
        GetSession()->Import(path, Translate(stmt->GetPkgName()), load_threads, merge_threads, merge_sim);
      }

      virtual void operator()(const TCompileStmt *stmt) const override {
        assert(stmt);
        if (!Conn->Ws->AllowRemoteCompile) {
          throw TRemoteCompileDisabled();
        }
        ostringstream out_strm;
        Result = TJson::Object;
        try {
          TTmpCopyToFile tmp_file(
              Conn->Ws->TmpDirMaker.GetPath(), Translate(stmt->GetStrExpr()),
              "tmp_pkg_", ".orly");
          auto pkg = Compiler::Compile(TPath(tmp_file.GetPath()),
                                       Conn->Ws->SessionManager->GetPackageManager().GetPackageDir(),
                                       {.DebugCc = true},
                                       out_strm);
          Result["name"] = AsStr(pkg.Name);
          Result["version"] = pkg.Version;
        } catch (const Compiler::TCompileFailure &) {
          THROW << out_strm.str();
        }
      }

      virtual void operator()(const TListPackageStmt *) const override {
        TJson::TArray packages;
        auto &package_manager = Conn->Ws->SessionManager->GetPackageManager();
        package_manager.YieldInstalled([&packages, &package_manager](const Package::TVersionedName &versioned_name) {
          TJson::TObject package_info;
          package_info["name"] = AsStr(versioned_name.Name);
          package_info["version"] = versioned_name.Version;

          /* get each function's info */ {
            TJson::TObject functions;
            package_manager.Get(versioned_name.Name)
                ->ForEachFunction([&functions](const string &name, auto func) {
              TJson::TObject func_info;
              /* params */ {
                TJson::TObject parameters;
                for (const auto &param: func->GetParameters()) {
                  /* The orly-syntax type name IS the JSON representation of
                     a type (a plain string); rendering it once is not a
                     round-trip, and a structured type encoding would change
                     the wire contract for no consumer that wants it (#377's
                     round-trip half is fixed in Var::ToJson). */
                  ostringstream oss;
                  Orly::Type::Orlyify(oss, param.second);
                  parameters[param.first] = oss.str();
                }

                func_info["parameters"] = TJson(move(parameters));
              }
              /* oss for return */ {
                ostringstream oss;
                Orly::Type::Orlyify(oss, func->GetReturnType());
                func_info["return"] = oss.str();
              }
              functions[name] = TJson(move(func_info));
              return true;
                  });

            package_info["functions"] = TJson(move(functions));
          }

          packages.emplace_back(move(package_info));
          return true;
        });
        Result = TJson::Object;
        Result["packages"] = TJson(move(packages));
      }

      virtual void operator()(const TGetSourceStmt *stmt) const override {
        assert(stmt);
        vector<string> package_name;
        TranslatePathName(package_name, stmt->GetNameList());
        TPath filename(package_name, {"orly"});
        string src_filename = AsStr(Conn->Ws->SessionManager->GetPackageManager().GetPackageDir().GetAbsPath(filename));
        Result = TJson::Object;
        Result["code"] = ReadAll(TFd(open(src_filename.c_str(), O_RDONLY)));
        Result["filename"] = AsStr(filename);
        // The "line_nums" field is set if the the source is parsable.
        auto cst = Package::Syntax::TPackage::ParseFile(src_filename.data());
        if (!cst.HasErrors()) {
          TJson line_nums(TJson::Object);
          Synth::ForEach<Package::Syntax::TDef>(
              cst.Get()->GetOptDefSeq(),
              [&line_nums](const Package::Syntax::TDef *def) {
                auto *func_def =
                    dynamic_cast<const Package::Syntax::TFuncDef *>(def);
                if (func_def) {
                  auto lexeme = func_def->GetName()->GetLexeme();
                  auto name = lexeme.GetText();
                  auto line_num = lexeme.GetPosRange().GetStart().GetLineNumber();
                  line_nums[name] = line_num;
                }
                return true;
              });
          Result["line_nums"] = std::move(line_nums);
        }
      }

      virtual void operator()(const TListSchemaStmt *stmt) const override {
        assert(stmt);
        Result = TJson::Object;
        Conn->Ws->SessionManager->ForEachIndex([this](const std::string &pkg,
                                                  const std::string &key,
                                                  const std::string &val) {
          if (!Result.Contains(pkg)) {
            Result[pkg] = TJson::Object;
          }
          Result[pkg][key] = val;
          return true;
        });
      }

      private:

      static constexpr auto SabotStateSize = Orly::Sabot::State::GetMaxStateSize();

      using TStateDumper = Orly::Sabot::TStateDumper;
      using TWrapper = Orly::Sabot::State::TAny::TWrapper;

      TSessionPin *GetSession() const {
        return GetSharedSession().get();
      }

      /* For a statement that runs after this visitor is gone (#761). */
      const std::shared_ptr<TSessionPin> &GetSharedSession() const {
        if (!Conn->Session) {
          throw invalid_argument("session not yet established");
        }
        return Conn->Session;
      }

      /* A method result as the reply's JSON. */
      static TJson ToJson(const TMethodResult &result) {
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        return Var::ToJson(
            Var::ToVar(*TWrapper(Indy::TKey(result.GetValue(), result.GetArena().get()).GetState(state_alloc))));
      }

      static TUuid Translate(const TIdExpr *id_expr) {
        assert(id_expr);
        return TUuid(id_expr->GetLexeme().GetText().substr(1, id_expr->GetLexeme().GetText().size() - 2).c_str());
      }

      static int64_t Translate(const TIntExpr *int_expr) {
        assert(int_expr);
        return int_expr->GetLexeme().AsInt();
      }

      static string Translate(const TStrExpr *str_expr) {
        assert(str_expr);
        struct visitor_t final : public TStrExpr::TVisitor {
          string &Result;
          visitor_t(string &result) : Result(result) {}
          virtual void operator()(const TDoubleQuotedRawStrExpr *that) const override {
            Result = that->GetLexeme().AsDoubleQuotedRawString();
          }
          virtual void operator()(const TDoubleQuotedStrExpr *that) const override {
            Result = that->GetLexeme().AsDoubleQuotedString();
          }
          virtual void operator()(const TSingleQuotedRawStrExpr *that) const override {
            Result = that->GetLexeme().AsSingleQuotedRawString();
          }
          virtual void operator()(const TSingleQuotedStrExpr *that) const override {
            Result = that->GetLexeme().AsSingleQuotedString();
          }
        };
        string result;
        str_expr->Accept(visitor_t(result));
        return move(result);
      }

      TConn *Conn;
      TJson &Result;

    };  // TConn::TStmtVisitor

    /* Queue a read. */
    void DoRead() {
      ReadBuf.consume(ReadBuf.size());
      WsStream.async_read(
          ReadBuf,
          [self = shared_from_this()](beast::error_code ec, std::size_t /*n*/) {
            if (ec == websocket::error::closed ||
                ec == net::error::operation_aborted ||
                ec == net::error::eof ||
                ec == net::error::connection_reset) {
              self->Close();
              return;
            }
            if (ec) {
              self->OnError(ec, "read");
              return;
            }
            self->OnMsg();
          });
    }

    /* Parse + run the incoming statement, format the JSON reply, write
       it back.  A try, batch or multi doesn't run here: it goes to the
       statement queue (#761), and its reply is sent by Finish(), on this
       strand, when it's done.  Either way the next message isn't read
       until this one's reply is written, so a session's statements run
       one at a time, in order. */
    void OnMsg() {
      const string payload = beast::buffers_to_string(ReadBuf.data());
      TJson reply = TJson::Object;
      if (!Authenticated) {
        Authenticate(payload, reply);
      } else try {
        TJson result;
        Deferred = nullptr;
        ParseStmtStr(
            payload.c_str(),
            [this, &result](const TStmt *stmt) {
              stmt->Accept(TStmtVisitor(this, result));
            });
        if (Deferred) {
          RunDeferred();
          return;
        }
        reply["result"] = std::move(result);
        reply["status"] = "ok";
      } catch (...) {
        SetErrorReply(reply);
      }
      SendReply(std::move(reply));
    }

    /* Hand the statement the visitor left in Deferred to the queue.  Its done callback runs on
       whichever thread finished it and posts the rest back to this connection's strand. */
    void RunDeferred() {
      auto work = std::move(Deferred);
      Deferred = nullptr;
      Ws->StmtQueue->Submit(
          std::move(work),
          [weak = weak_from_this()](TStmtQueue::TFinish &&finish, std::exception_ptr error) {
            /* The connection lives in Ws->Conns until it closes, and it can't close while a
               statement is in flight (no read is pending), so this only fails at shutdown,
               when nobody is left to reply to. */
            if (auto self = weak.lock()) {
              auto executor = self->WsStream.get_executor();
              net::post(executor, [self = std::move(self), finish = std::move(finish),
                                   error = std::move(error)]() mutable {
                self->Finish(std::move(finish), std::move(error));
              });
            }
          });
    }

    /* On the strand: the deferred statement is done; build its reply and send it. */
    void Finish(TStmtQueue::TFinish &&finish, std::exception_ptr error) {
      TJson reply = TJson::Object;
      try {
        if (error) {
          std::rethrow_exception(error);
        }
        reply["result"] = finish();
        reply["status"] = "ok";
      } catch (...) {
        SetErrorReply(reply);
      }
      SendReply(std::move(reply));
    }

    /* Write a reply, then read the next message (or close, after an exit or a refused token). */
    void SendReply(TJson &&reply) {
      ReplyBuf = AsStr(reply);
      WsStream.text(true);
      WsStream.async_write(
          net::buffer(ReplyBuf),
          [self = shared_from_this()](beast::error_code ec, std::size_t /*n*/) {
            if (ec) {
              self->OnError(ec, "write");
              return;
            }
            if (self->Exiting) {
              /* A refused token (#710) closes with 1008 (policy violation), which a browser's
                 close event shows. */
              self->WsStream.async_close(
                  self->Authenticated ? websocket::close_code::normal : websocket::close_code::policy_error,
                  [self2 = self](beast::error_code) { self2->Close(); });
              return;
            }
            self->DoRead();
          });
    }

    /* The first message on a connection to a server with a token (#710): {"auth": "<token>"}.
       Accepted, the reply is "status": "ok" and statements follow. Anything else, or a different
       token, is answered "status": "unauthorized" and the connection closes after the reply. The
       token is never logged or echoed. */
    void Authenticate(const string &payload, TJson &reply) {
      bool presented = false, accepted = false;
      try {
        const TJson msg = TJson::Parse(payload);
        if (msg.GetKind() == TJson::Object && msg.GetSize() == 1 && msg.Contains("auth") &&
            msg["auth"].GetKind() == TJson::String) {
          presented = true;
          accepted = Orly::Auth::TokensEqual(msg["auth"].GetString(), Ws->AuthToken);
        }
      } catch (const exception &) {
        /* Not JSON, so not an auth message. */
      }
      if (accepted) {
        Authenticated = true;
        AuthTimer.cancel();
        reply["result"] = TJson();
        reply["status"] = "ok";
        return;
      }
      const string remote = RemoteAddress();
      syslog(LOG_WARNING, "ws: refused a client from %s: %s", remote.c_str(), presented ? "wrong token" : "no token");
      reply["result"] = presented
          ? "unauthorized: wrong token"
          : "unauthorized: this server requires a token; send {\"auth\": \"<token>\"} as the first message";
      reply["status"] = "unauthorized";
      Exiting = true;
    }

    /* The peer's address, for the log. */
    string RemoteAddress() {
      beast::error_code ec;
      const auto endpoint = beast::get_lowest_layer(WsStream).socket().remote_endpoint(ec);
      if (ec) {
        return "(unknown)";
      }
      ostringstream strm;
      strm << endpoint;
      return strm.str();
    }

    void OnError(beast::error_code ec, const char *where) {
      syslog(LOG_WARNING, "ws: %s: %s", where, ec.message().c_str());
      Close();
    }

    /* Remove ourselves from the parent's Conns set.  Any in-flight async
       handlers still hold a shared_ptr to us via their capture, so the
       actual destruction happens when those handlers run (or are
       discarded when the io_context unwinds at shutdown). */
    void Close() {
      AuthTimer.cancel();
      lock_guard<mutex> lock(Ws->Mutex);
      Ws->Conns.erase(shared_from_this());
    }

    TWsImpl *Ws;
    websocket::stream<beast::tcp_stream> WsStream;
    beast::flat_buffer ReadBuf;
    string ReplyBuf;
    bool Exiting = false;

    /* Shared with a statement in flight (#761), which may finish after the connection closes. */
    shared_ptr<TSessionPin> Session;

    /* Set by the visitor to a statement that runs off the I/O thread (#761); see OnMsg(). */
    TStmtQueue::TWork Deferred;

    /* How long a connection to a server with a token has to authenticate (#710). */
    static constexpr std::chrono::seconds AuthDeadline{10};

    /* Closes the connection if it hasn't authenticated by AuthDeadline. */
    net::steady_timer AuthTimer;

    /* True once the connection has presented the token, or from the start without one. */
    bool Authenticated;

  };  // TWsImpl::TConn

  /* The session manager interface passed to us at construction time. */
  TSessionManager *SessionManager;

  /* Whether the compile statement is accepted (#705). */
  const bool AllowRemoteCompile;

  /* The token every connection must present first (#710); empty, none is needed. */
  const std::string AuthToken;

  /* Runs try, batch and multi statements off the I/O threads (#761).  Shared with the
     statements in flight; Shutdown() waits for them. */
  const std::shared_ptr<TStmtQueue> StmtQueue;

  /* Creates and destroys the tmp dir used by the compile stmt. */
  TTmpDirMaker TmpDirMaker;

  /* I/O event loop; runs on BgThreads. */
  net::io_context IoCtx;

  /* Accepts new TCP connections. */
  tcp::acceptor Acceptor;

  /* Worker threads pumping IoCtx.run(). */
  vector<thread> BgThreads;

  /* Guards Conns.  Per-connection state is otherwise protected by each
     connection's strand. */
  mutex Mutex;

  /* The currently-live connections.  We hold shared_ptrs to keep them
     alive at least as long as the server; handlers also hold their own
     shared_ptrs so a connection survives in-flight ops after removal. */
  set<shared_ptr<TConn>> Conns;

};  // TWsImpl

void TWsImpl::DoAccept() {
  /* Each accepted socket is bound to its own strand, serializing the
     per-connection async chain across the io_context thread pool. */
  Acceptor.async_accept(
      net::make_strand(IoCtx),
      [this](beast::error_code ec, tcp::socket sock) {
        if (!ec) {
          try {
            sock.set_option(tcp::no_delay(true));
            auto conn = make_shared<TConn>(this, std::move(sock));
            {
              lock_guard<mutex> lock(Mutex);
              Conns.insert(conn);
            }
            conn->Run();
          } catch (const std::exception &ex) {
            syslog(LOG_ERR, "ws: accept handler: %s", ex.what());
          }
        }
        /* operation_aborted means the acceptor was closed (shutdown).
           Other errors are transient and we want to keep listening. */
        if (ec != net::error::operation_aborted) {
          DoAccept();
        }
      });
}

TWs *TWs::New(
    TSessionManager *session_mngr, size_t thread_count,
    in_port_t port_number, const std::string &bind_address, bool allow_remote_compile,
    const std::string &auth_token, size_t max_in_flight) {
  return new TWsImpl(session_mngr, thread_count, port_number, bind_address, allow_remote_compile, auth_token,
                     max_in_flight);
}
