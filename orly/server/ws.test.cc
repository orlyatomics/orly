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

#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <orly/server/ws_test_server.h>
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
