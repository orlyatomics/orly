/* <orly/balancer/balancer.cc>

   Implements <orly/balancer/balancer.h>.

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

#include <orly/balancer/balancer.h>

#include <base/epoll.h>
#include <base/util/io.h>

using namespace std;
using namespace Base;
using namespace Socket;
using namespace Orly::Balancer;
using namespace Util;

TBalancer::TBalancer(TScheduler *scheduler, const TCmd &cmd)
    : Scheduler(scheduler), Handshake(make_shared<THandshake>()) {
  /* open the main socket */ {
    TAddress address(TAddress::IPv4Any, cmd.PortNumber);
    MainSocket = TFd(socket(address.GetFamily(), SOCK_STREAM, 0));
    int flag = true;
    IfLt0(setsockopt(MainSocket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)));
    Bind(MainSocket, address);
    IfLt0(listen(MainSocket, cmd.ConnectionBacklog));
  }
  ScheduleJob([this] { AcceptClientConnections(); });
}

TBalancer::~TBalancer() {
  StopServing();
}

void TBalancer::StopServing() {
  unique_lock<mutex> lock(Handshake->Mutex);
  if (Handshake->Serving) {
    Handshake->Serving = false;
    Handshake->TermFd.Push();
  }
  Handshake->AllDone.wait(lock, [this] { return Handshake->InFlightCount == 0; });
}

void TBalancer::AcceptClientConnections() {
  TEpoll poll;
  poll.Add(MainSocket);
  poll.Add(Handshake->TermFd.GetFd());
  for (;;) {
    if (poll.WaitForOne() != MainSocket) {
      break;  /* the term fd fired: we're shutting down */
    }
    TAddress client_address;
    TFd client_socket(Accept(MainSocket, client_address));
    ScheduleJob([this, client_socket, client_address]() mutable { ServeClient(client_socket, client_address); });
  }
}

void TBalancer::ServeClient(TFd &fd, const TAddress &) {
  const size_t buf_size = 4096;
  /* Figure out which host to route to. */
  const Socket::TAddress &server_address = ChooseHost();
  TFd new_server_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  try {
    Connect(new_server_socket, server_address);
    char buf[buf_size];
    TEpoll poll;
    poll.Add(fd);
    poll.Add(new_server_socket);
    const int term_fd = Handshake->TermFd.GetFd();
    poll.Add(term_fd);
    for (;;) {
      int ready_fd = poll.WaitForOne();
      if (ready_fd == term_fd) {
        return;  /* we're shutting down: drop the connection */
      }
      size_t amt_read = ReadAtMost(ready_fd, &buf, buf_size);
      if (amt_read) {
        WriteExactly(ready_fd == fd ? new_server_socket : fd, &buf, amt_read);
      } else {
        return;
      }
    }
  } catch (const std::exception &ex) {
    OnError(ex);
  }
}

void TBalancer::ScheduleJob(function<void ()> &&job) {
  Scheduler->Schedule([handshake = Handshake, job = move(job)] {
    if (!handshake->Enter()) {
      return;
    }
    try {
      job();
    } catch (...) {
      handshake->Exit();
      throw;
    }
    handshake->Exit();
  });
}
