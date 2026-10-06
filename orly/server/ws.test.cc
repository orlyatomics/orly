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
