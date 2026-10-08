/* <orly/server/ws.test.cc>

   Unit test for <orly/server/ws.h>.

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

#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <orly/server/ws_test_server.h>
#include <base/fd.h>
#include <base/test/kit.h>
#include <orly/type/type_czar.h>

using namespace std;
using namespace Orly::Server;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

FIXTURE(Typical) {
  TWsTestServer ws_test_server(8080, 100);

  net::io_context ioc;
  tcp::resolver resolver(ioc);
  websocket::stream<tcp::socket> ws(ioc);

  auto const results = resolver.resolve(
      "127.0.0.1", to_string(ws_test_server.GetPortNumber()));
  net::connect(ws.next_layer(), results.begin(), results.end());
  ws.handshake("127.0.0.1", "/");

  ws.write(net::buffer(string("echo 'hello';")));

  beast::flat_buffer buffer;
  ws.read(buffer);
  const string reply = beast::buffers_to_string(buffer.data());

  ws.close(websocket::close_code::going_away);

  EXPECT_EQ(Base::TJson::Parse(reply),
            Base::TJson::Parse(R"({"status":"ok","result":"hello"})"));
}

/* Sends one statement to the test server and returns the parsed reply. */
static Base::TJson SendOne(in_port_t port, const string &stmt) {
  net::io_context ioc;
  tcp::resolver resolver(ioc);
  websocket::stream<tcp::socket> ws(ioc);
  auto const results = resolver.resolve("127.0.0.1", to_string(port));
  net::connect(ws.next_layer(), results.begin(), results.end());
  ws.handshake("127.0.0.1", "/");
  ws.write(net::buffer(stmt));
  beast::flat_buffer buffer;
  ws.read(buffer);
  const string reply = beast::buffers_to_string(buffer.data());
  ws.close(websocket::close_code::going_away);
  return Base::TJson::Parse(reply);
}

/* By default the compile statement is refused with its own status (#705). */
FIXTURE(RemoteCompileDisabledByDefault) {
  TWsTestServer ws_test_server(8080, 100);
  const auto reply = SendOne(ws_test_server.GetPortNumber(), "compile \"x = 42;\";");
  EXPECT_EQ(reply["status"], Base::TJson("remote_compile_disabled"));
  /* Other statements on the same server are unaffected. */
  EXPECT_EQ(SendOne(ws_test_server.GetPortNumber(), "echo 'hello';"),
            Base::TJson::Parse(R"({"status":"ok","result":"hello"})"));
}

/* The first IPv4 address of this host that is not loopback, if it has one. */
static optional<in_addr> FindNonLoopbackIPv4() {
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) < 0) {
    return nullopt;
  }
  optional<in_addr> result;
  for (const ifaddrs *ifa = list; ifa && !result; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
      const in_addr addr = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr)->sin_addr;
      if ((ntohl(addr.s_addr) >> 24) != 127) {
        result = addr;
      }
    }
  }
  freeifaddrs(list);
  return result;
}

/* By default the listener binds loopback only (#705): it answers on 127.0.0.1 and refuses a
   connection to this host's other addresses. Skipped on a host with no other IPv4 address. */
FIXTURE(LoopbackOnlyByDefault) {
  TWsTestServer ws_test_server(8080, 100);
  const auto port = ws_test_server.GetPortNumber();
  EXPECT_EQ(SendOne(port, "echo 'hello';")["status"], Base::TJson("ok"));
  const auto other = FindNonLoopbackIPv4();
  if (!other) {
    std::cout << "no non-loopback IPv4 address on this host; skipping the refusal check" << std::endl;
    return;
  }
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr = *other;
  Base::TFd sock(socket(AF_INET, SOCK_STREAM, 0));
  const int rc = connect(sock, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa));
  const int err = errno;
  char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &*other, buf, sizeof(buf));
  std::cout << "connect to " << buf << ":" << port << " -> " << (rc < 0 ? strerror(err) : "connected") << std::endl;
  EXPECT_TRUE(rc < 0);
  EXPECT_EQ(err, ECONNREFUSED);
}

/* Sends the messages in order on one connection, without waiting between them, and returns
   every reply the server sends before it closes the connection or `msgs.size()` replies have
   arrived. `closed` says whether the server closed the connection. */
static vector<Base::TJson> SendPipelined(in_port_t port, const vector<string> &msgs, bool &closed) {
  net::io_context ioc;
  tcp::resolver resolver(ioc);
  websocket::stream<tcp::socket> ws(ioc);
  auto const results = resolver.resolve("127.0.0.1", to_string(port));
  net::connect(ws.next_layer(), results.begin(), results.end());
  ws.handshake("127.0.0.1", "/");
  for (const auto &msg: msgs) {
    ws.write(net::buffer(msg));
  }
  vector<Base::TJson> replies;
  closed = false;
  while (replies.size() < msgs.size()) {
    beast::flat_buffer buffer;
    beast::error_code ec;
    ws.read(buffer, ec);
    if (ec) {
      closed = true;
      break;
    }
    replies.push_back(Base::TJson::Parse(beast::buffers_to_string(buffer.data())));
  }
  if (!closed) {
    beast::error_code ec;
    ws.close(websocket::close_code::going_away, ec);
  }
  return replies;
}

static const string TestToken = "0123456789abcdef-test-token";

/* Without a token, an auth message is just a statement that doesn't parse ("exception", as before
   #710), and the connection carries on. */
FIXTURE(AuthNoTokenUnchanged) {
  TWsTestServer ws_test_server(8080, 100);
  bool closed;
  const auto replies = SendPipelined(
      ws_test_server.GetPortNumber(), {R"({"auth": ")" + TestToken + R"("})", "echo 'hello';"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 2U)) {
    EXPECT_EQ(replies[0]["status"], Base::TJson("exception"));
    EXPECT_EQ(replies[1], Base::TJson::Parse(R"({"status":"ok","result":"hello"})"));
  }
}

/* With a token, a connection whose first message isn't the auth message is refused with
   "unauthorized" and closed; the statement never runs. */
FIXTURE(AuthTokenMissing) {
  TWsTestServer ws_test_server(8080, 100, TestToken);
  bool closed;
  const auto replies = SendPipelined(ws_test_server.GetPortNumber(), {"echo 'hello';", "echo 'again';"}, closed);
  EXPECT_TRUE(closed);
  if (EXPECT_EQ(replies.size(), 1U)) {
    EXPECT_EQ(replies[0]["status"], Base::TJson("unauthorized"));
  }
}

/* A wrong token is refused the same way, and a statement pipelined behind it never runs. */
FIXTURE(AuthTokenWrong) {
  TWsTestServer ws_test_server(8080, 100, TestToken);
  bool closed;
  const auto replies = SendPipelined(
      ws_test_server.GetPortNumber(), {R"({"auth": "0123456789abcdef-wrong-token"})", "echo 'hello';"}, closed);
  EXPECT_TRUE(closed);
  if (EXPECT_EQ(replies.size(), 1U)) {
    EXPECT_EQ(replies[0]["status"], Base::TJson("unauthorized"));
    /* The reply never echoes a token. */
    EXPECT_EQ(replies[0]["result"].GetString().find("wrong-token"), string::npos);
  }
}

/* A token that is a prefix of the right one, or the right one with more after it, is wrong. */
FIXTURE(AuthTokenPrefix) {
  TWsTestServer ws_test_server(8080, 100, TestToken);
  for (const string &presented: {TestToken.substr(0, TestToken.size() - 1), TestToken + "x"}) {
    bool closed;
    const auto replies = SendPipelined(
        ws_test_server.GetPortNumber(), {R"({"auth": ")" + presented + R"("})"}, closed);
    if (EXPECT_EQ(replies.size(), 1U)) {
      EXPECT_EQ(replies[0]["status"], Base::TJson("unauthorized"));
    }
  }
}

/* The right token is accepted, and statements follow on the same connection. */
FIXTURE(AuthTokenRight) {
  TWsTestServer ws_test_server(8080, 100, TestToken);
  bool closed;
  const auto replies = SendPipelined(
      ws_test_server.GetPortNumber(), {R"({"auth": ")" + TestToken + R"("})", "echo 'hello';"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 2U)) {
    EXPECT_EQ(replies[0]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[1], Base::TJson::Parse(R"({"status":"ok","result":"hello"})"));
  }
}

/* #746: the POV review statements parse, take their options as a record, and answer in their JSON
   shapes (the test server's session fakes empty results).  `.from` can't name an option: `from` is
   a keyword of the statement grammar, which is why the range options are `.start` and `.stop`. */
FIXTURE(PovReviewStatements) {
  TWsTestServer ws_test_server(8080, 100);
  const string pov = "{00000000-0000-0000-0000-000000000001}";
  bool closed;
  auto replies = SendPipelined(ws_test_server.GetPortNumber(), {
      "new session;",
      "new fast private pov <{.conflicts: \"refuse\"}>;",
      "diff_pov " + pov + " <{.start: <['a', 1]>, .stop: <['b']>, .after: <['a', 2]>, .limit: 5}>;",
      "discard_pov " + pov + ";",
      "promote_pov " + pov + " <{.force: true}>;",
      "review_pov " + pov + " <{.after: 3}>;",
      "diff_pov " + pov + ";",
      "diff_pov " + pov + " <{.from: <['a']>}>;",
      "diff_pov " + pov + " <{.limit: 0}>;",
      "new fast private pov <{.conflicts: \"maybe\"}>;"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 10U)) {
    for (size_t i = 0; i < 7; ++i) {
      EXPECT_EQ(replies[i]["status"], Base::TJson("ok"));
    }
    EXPECT_EQ(replies[2]["result"]["changes"], Base::TJson(Base::TJson::Array));
    EXPECT_EQ(replies[2]["result"]["next"], Base::TJson());
    EXPECT_EQ(replies[3]["result"]["discarded_updates"], Base::TJson(0));
    EXPECT_EQ(replies[4]["result"]["status"], Base::TJson("promoting"));
    EXPECT_EQ(replies[5]["result"]["conflict_mode"], Base::TJson("none"));
    for (size_t i = 7; i < 10; ++i) {
      EXPECT_EQ(replies[i]["status"], Base::TJson("exception"));
    }
  }
}

/* A `try` that asks for a receipt gets one, with a version that rises with each write; one that
   doesn't gets the reply it always did (#750). */
FIXTURE(TryReceipt) {
  Orly::Type::TTypeCzar type_czar;
  TWsTestServer ws_test_server(8080, 100);
  const string pov = "{00000000-0000-0000-0000-000000000001}";
  const string call = "try " + pov + " pkg/v1 f <{}>";
  bool closed;
  auto replies = SendPipelined(ws_test_server.GetPortNumber(), {
      "new session;",
      call + ";",
      call + " <{.receipt: true}>;",
      call + " <{.receipt: true}>;",
      call + " <{.receipt: false}>;",
      call + " <{.receipt: \"yes\"}>;",
      call + " <{.durable: true}>;"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 7U)) {
    EXPECT_EQ(replies[1]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[1]["result"], Base::TJson(98.6));
    EXPECT_EQ(replies[1].TryFind("receipt"), nullptr);
    EXPECT_EQ(replies[2]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[2]["receipt"]["pov"], Base::TJson("00000000-0000-0000-0000-000000000001"));
    EXPECT_EQ(replies[2]["receipt"]["durability"], Base::TJson("memory"));
    EXPECT_EQ(replies[3]["status"], Base::TJson("ok"));
    EXPECT_GT(replies[3]["receipt"]["version"].GetNumber(), replies[2]["receipt"]["version"].GetNumber());
    EXPECT_EQ(replies[4]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[4].TryFind("receipt"), nullptr);
    EXPECT_EQ(replies[5]["status"], Base::TJson("exception"));
    EXPECT_EQ(replies[6]["status"], Base::TJson("exception"));
  }
}

/* `.wait_durable_ms` holds the reply until the write is durable, or gives a typed timeout whose
   receipt still names the write (#750). */
FIXTURE(TryWaitDurable) {
  Orly::Type::TTypeCzar type_czar;
  TWsTestServer ws_test_server(8080, 100);
  const string durable_call = "try {00000000-0000-0000-0000-000000000002} pkg/v1 f <{}>";
  const string slow_call = "try {00000000-0000-0000-0000-000000000001} pkg/v1 f <{}>";
  bool closed;
  auto replies = SendPipelined(ws_test_server.GetPortNumber(), {
      "new session;",
      durable_call + " <{.wait_durable_ms: 1000}>;",
      slow_call + " <{.wait_durable_ms: 30}>;",
      slow_call + " <{.wait_durable_ms: 0}>;",
      slow_call + " <{.wait_durable_ms: \"soon\"}>;"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 5U)) {
    EXPECT_EQ(replies[1]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[1]["result"], Base::TJson(98.6));
    EXPECT_EQ(replies[1]["receipt"]["durability"], Base::TJson("durable"));
    EXPECT_EQ(replies[2]["status"], Base::TJson("durable_timeout"));
    EXPECT_EQ(replies[2]["receipt"]["durability"], Base::TJson("memory"));
    EXPECT_TRUE(replies[2]["receipt"]["version"].GetNumber() > 100);
    EXPECT_EQ(replies[3]["status"], Base::TJson("exception"));
    EXPECT_EQ(replies[4]["status"], Base::TJson("exception"));
  }
}

/* The batch form takes the same options: a receipt, and a wait for durable (#750). */
FIXTURE(TryBatchReceipt) {
  Orly::Type::TTypeCzar type_czar;
  TWsTestServer ws_test_server(8080, 100);
  const string call = "try {00000000-0000-0000-0000-000000000002} pkg/v1 f [<{}>, <{}>]";
  const string slow = "try {00000000-0000-0000-0000-000000000001} pkg/v1 f [<{}>, <{}>]";
  bool closed;
  auto replies = SendPipelined(ws_test_server.GetPortNumber(), {
      "new session;",
      call + ";",
      call + " <{.receipt: true}>;",
      call + " <{.wait_durable_ms: 1000}>;",
      slow + " <{.wait_durable_ms: 30}>;",
      call + " <{.nope: 1}>;"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 6U)) {
    EXPECT_EQ(replies[1]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[1].TryFind("receipt"), nullptr);
    EXPECT_EQ(replies[2]["receipt"]["durability"], Base::TJson("memory"));
    EXPECT_TRUE(replies[2]["receipt"]["version"].GetNumber() > 500);
    EXPECT_EQ(replies[3]["receipt"]["durability"], Base::TJson("durable"));
    EXPECT_EQ(replies[4]["status"], Base::TJson("durable_timeout"));
    EXPECT_EQ(replies[4]["receipt"]["durability"], Base::TJson("memory"));
    EXPECT_EQ(replies[5]["status"], Base::TJson("exception"));
  }
}

/* The durable version of a POV is its own statement (#750). */
FIXTURE(DurableVersion) {
  TWsTestServer ws_test_server(8080, 100);
  bool closed;
  auto replies = SendPipelined(ws_test_server.GetPortNumber(), {
      "new session;",
      "durable_version {00000000-0000-0000-0000-000000000001};"}, closed);
  EXPECT_FALSE(closed);
  if (EXPECT_EQ(replies.size(), 2U)) {
    EXPECT_EQ(replies[1]["status"], Base::TJson("ok"));
    EXPECT_EQ(replies[1]["result"]["pov"], Base::TJson("00000000-0000-0000-0000-000000000001"));
    EXPECT_EQ(replies[1]["result"]["durable_version"], Base::TJson(77));
  }
}
