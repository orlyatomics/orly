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
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <syslog.h>
#include <thread>
#include <vector>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <base/as_str.h>
#include <base/fd.h>
#include <base/json.h>
#include <base/tmp_copy_to_file.h>
#include <base/tmp_dir_maker.h>
#include <orly/compiler.h>
#include <orly/error.h>
#include <orly/client/program/parse_stmt.h>
#include <orly/client/program/translate_expr.h>
#include <orly/indy/key.h>
#include <orly/orly.package.cst.h>
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

/* The implementation of the TWs interface declared in the header.

   Each connection runs on its own asio strand, so per-connection handlers
   serialize across the io_context's thread pool. The Conns set is guarded
   by Mutex; the strands cover everything else. */
class TWsImpl final
    : public TWs {
  public:

  /* Starts up the server. */
  TWsImpl(
      TSessionManager *session_mngr, size_t thread_count,
      in_port_t port_number)
      : SessionManager(session_mngr),
        TmpDirMaker(MakePath({ P_tmpdir, "orly_ws_compile" }, {})),
        IoCtx(thread_count ? static_cast<int>(thread_count) : 1),
        Acceptor(IoCtx) {
    assert(session_mngr);
    syslog(LOG_INFO, "ws compile tmp dir = \"%s\"", TmpDirMaker.GetPath().c_str());

    tcp::endpoint endpoint(tcp::v4(), port_number);
    Acceptor.open(endpoint.protocol());
    Acceptor.set_option(net::socket_base::reuse_address(true));
    Acceptor.bind(endpoint);
    Acceptor.listen(8192);

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

  /* Stops the io_context, joins the worker threads, and clears the
     connection set. Idempotent so the dtor can call it after a partially-
     constructed start. */
  void Shutdown() {
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
          WsStream(beast::tcp_stream(std::move(sock))) {}

    /* Perform the WS handshake then start the read loop. */
    void Run() {
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
        //TODO: Push TJson down into Var::Jsonify
        Result = TJson::Parse(AsStrFunc(&Var::Jsonify, Var::ToVar(*TWrapper(NewStateSabot(stmt->GetExpr(), alloc)))));
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
        TMethodResult result = GetSession()->Try(TMethodRequest(pov_id, fq_name, closure));
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        Result = TJson::Parse(AsStrFunc(
            &Var::Jsonify,
            Var::ToVar(*TWrapper(Indy::TKey(result.GetValue(), result.GetArena().get()).GetState(state_alloc)))));
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
        ostringstream out_strm;
        Result = TJson::Object;
        try {
          TTmpCopyToFile tmp_file(
              Conn->Ws->TmpDirMaker.GetPath(), Translate(stmt->GetStrExpr()),
              "tmp_pkg_", ".orly");
          auto pkg = Compiler::Compile(TPath(tmp_file.GetPath()),
                                       Conn->Ws->SessionManager->GetPackageManager().GetPackageDir(),
                                       true,
                                       false,
                                       false,
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
                  // TODO: Change to jsonify
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
        if (!Conn->Session) {
          throw invalid_argument("session not yet established");
        }
        return Conn->Session.get();
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
       it back. */
    void OnMsg() {
      const string payload = beast::buffers_to_string(ReadBuf.data());
      TJson reply = TJson::Object;
      try {
        TJson result;
        ParseStmtStr(
            payload.c_str(),
            [this, &result](const TStmt *stmt) {
              stmt->Accept(TStmtVisitor(this, result));
            });
        reply["result"] = std::move(result);
        reply["status"] = "ok";
      } catch (const TSourceError &src_error) {
        reply["result"] = src_error.what();
        reply["pos"] = AsStr(src_error.GetPosRange());
        reply["status"] = "source_error";
      } catch (const exception &ex) {
        reply["result"] = ex.what();
        reply["status"] = "exception";
      }
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
              self->WsStream.async_close(
                  websocket::close_code::normal,
                  [self2 = self](beast::error_code) { self2->Close(); });
              return;
            }
            self->DoRead();
          });
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
      lock_guard<mutex> lock(Ws->Mutex);
      Ws->Conns.erase(shared_from_this());
    }

    TWsImpl *Ws;
    websocket::stream<beast::tcp_stream> WsStream;
    beast::flat_buffer ReadBuf;
    string ReplyBuf;
    bool Exiting = false;
    unique_ptr<TSessionPin> Session;

  };  // TWsImpl::TConn

  /* The session manager interface passed to us at construction time. */
  TSessionManager *SessionManager;

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
    in_port_t port_number) {
  return new TWsImpl(session_mngr, thread_count, port_number);
}
