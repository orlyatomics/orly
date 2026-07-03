/* <orly/balancer/balancer.test.cc>

   Unit test for <orly/balancer/balancer.h>.

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
#include <optional>

#include <base/epoll.h>
#include <base/event_counter.h>
#include <base/timer_fd.h>
#include <base/uuid.h>
#include <base/io/device.h>
#include <base/rpc/rpc.h>
#include <base/util/io.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Io;
using namespace Socket;
using namespace Orly::Balancer;
using namespace Util;

class TRouter : public TBalancer {
  NO_COPY(TRouter);
  public:

  TRouter(TScheduler *scheduler, const TBalancer::TCmd &cmd, chrono::milliseconds interval) : TBalancer(scheduler, cmd), Running(true), CheckHostsDone(false) {
    scheduler->Schedule(bind(&TRouter::CheckHosts, this, interval));
  }

  virtual ~TRouter() {
    /* Stop the accept/serve jobs first, while the state (and vtable slots) they
       reach through ChooseHost()/OnError() are still fully alive. */
    StopServing();
    /* Now stop the host-checking job and wait until it has fully exited before
       we destroy ourselves.  We wait on a predicate (rather than a bare wait) so
       this is correct regardless of whether CheckHosts() exited gracefully or
       was interrupted by a scheduler shutdown. */
    std::unique_lock<std::mutex> lock(HostMutex);
    Running = false;
    while (!CheckHostsDone) {
      HostCond.wait(lock);
    }
  }

  virtual const Socket::TAddress &ChooseHost() {
    std::lock_guard<std::mutex> lock(HostMutex);
    if (MasterHost) {
    } else {
      std::cerr << "No master host available to connect to." << endl;
      throw std::runtime_error("No master host available to connect to.");
    }
    return *MasterHost;
  }

  void AddHost(const Socket::TAddress &address) {
    std::lock_guard<std::mutex> lock(HostMutex);
    auto ret = HostSet.insert(address);
    if (ret.second) {
    } else {
      throw std::runtime_error("Host already exists in HostSet.");
    }
  }

  void RemoveHost(const Socket::TAddress &address) {
    std::lock_guard<std::mutex> lock(HostMutex);
    auto ret = HostSet.erase(address);
    assert(ret == 1);
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(HostMutex);
    while (!MasterHost) {
      HostCond.wait(lock);
    }
  }

  virtual void OnError(const exception &) {}

  private:

  class TConnection
      : public Rpc::TContext {
    NO_COPY(TConnection);
    public:

    TConnection(Base::TFd &&fd)
        : TContext(TProtocol::Protocol), Device(make_shared<TDevice>(move(fd))) {
      BinaryIoStream = make_shared<TBinaryIoStream>(Device);
    }

    private:

    class TProtocol
        : public Rpc::TProtocol {
      NO_COPY(TProtocol);
      public:

      static const TProtocol Protocol;

      private:

      TProtocol() {}

    };  // TProtocol

    shared_ptr<TDevice> Device;

  };

  void CheckHosts(chrono::milliseconds interval) {
    try {
      Base::TTimerFd check_hosts(interval);
      for (;Running;) {
        check_hosts.Pop();
        std::lock_guard<std::mutex> lock(HostMutex);
        MasterHost.reset();
        for (const auto &addr : HostSet) {
          bool is_master = CheckHost(addr);
          if (MasterHost && is_master) {
            std::cerr << "There is more than 1 master" << std::endl;
            MasterHost.reset();
            throw std::runtime_error("There is more than 1 master");
          } else if (is_master) {
            MasterHost = addr;
          }
        }
        if (MasterHost) {
          HostCond.notify_all();
        }
      }
    } catch (...) {
      /* Fall through to signal completion below.  Whether we exited the loop
         normally or via an interrupt/exception, the destructor must be told the
         job is finished so it doesn't block forever. */
    }
    std::lock_guard<std::mutex> lock(HostMutex);
    CheckHostsDone = true;
    HostCond.notify_all();
  }

  bool CheckHost(const Socket::TAddress &addr) const;

  std::mutex HostMutex;
  std::condition_variable HostCond;

  std::unordered_set<Socket::TAddress> HostSet;

  std::optional<Socket::TAddress> MasterHost;

  bool Running;

  /* Set true by CheckHosts() once it has exited (gracefully or via interrupt). */
  bool CheckHostsDone;

};

class TTestServer {
  NO_COPY(TTestServer);
  public:

  static const Rpc::TEntryId
    AddId         = 1001,
    GetPortId     = 1002,
    HealthCheckId = 1003;

  TTestServer(TScheduler *scheduler, in_port_t port_num, char status, int connection_backlog = 5000)
      : Running(true), Done(false), Scheduler(scheduler), PortNum(port_num), Status(status) {
    /* open the main socket */ {
      TAddress address(TAddress::IPv4Any, port_num);
      MainSocket = TFd(socket(address.GetFamily(), SOCK_STREAM, 0));
      int flag = true;
      IfLt0(setsockopt(MainSocket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)));
      Bind(MainSocket, address);
      IfLt0(listen(MainSocket, connection_backlog));
    }
    scheduler->Schedule(bind(&TTestServer::AcceptClientConnections, this));
  }

  ~TTestServer() {
    /* Stop the accept job and wait until it (and therefore the ServeClient
       jobs it would spawn) has finished before we destroy ourselves.  We wait
       on a predicate so this is correct whether AcceptClientConnections()
       exited on its own or had already been drained by a scheduler shutdown
       before this destructor ran. */
    Running = false;
    TermFd.Push();
    std::unique_lock<std::mutex> lock(StatusLock);
    while (!Done) {
      Finish.wait(lock);
    }
  }

  void ChangeStatus(char status) {
    std::lock_guard<std::mutex> lock(StatusLock);
    Status = status;
  }

  private:

  class TConnection
      : public Rpc::TContext {
    NO_COPY(TConnection);
    public:

    TConnection(TTestServer *server, Base::TFd &&fd)
        : TContext(TProtocol::Protocol), Server(server), Device(make_shared<TDevice>(move(fd))) {
      BinaryIoStream = make_shared<TBinaryIoStream>(Device);
    }

    private:

    int Add(int a, int b) {
      return a + b;
    }

    int GetPort() {
      std::lock_guard<std::mutex> lock(Server->StatusLock);
      if (Server->Status == 'M') {
        return Server->PortNum;
      } else {
        throw std::runtime_error("Slave should not respond");
      }
    }

    char HealthCheck() {
      std::lock_guard<std::mutex> lock(Server->StatusLock);
      return Server->Status;
    }

    class TProtocol
        : public Rpc::TProtocol {
      NO_COPY(TProtocol);
      public:

      static const TProtocol Protocol;

      private:

      TProtocol() {
        Register<TConnection, int, int, int>(AddId, &TConnection::Add);
        Register<TConnection, int>(GetPortId, &TConnection::GetPort);
        Register<TConnection, char>(HealthCheckId, &TConnection::HealthCheck);
      }

    };  // TProtocol

    TTestServer *const Server;

    shared_ptr<TDevice> Device;

  };

  void AcceptClientConnections() {
    try {
      TEpoll poll;
      poll.Add(MainSocket);
      poll.Add(TermFd.GetFd());
      for (;Running;) {
        TAddress client_address;
        int ready_fd = poll.WaitForOne();
        if (ready_fd == MainSocket) {
          TFd client_socket(Accept(MainSocket, client_address));
          Scheduler->Schedule(bind(&TTestServer::ServeClient, this, move(client_socket), client_address));
        } else {
          /* we're exiting now */
        }
      }
    } catch (const std::exception &/*ex*/) {
      /* do nothing */
    }
    std::lock_guard<std::mutex> lock(StatusLock);
    Done = true;
    Finish.notify_all();
  }

  void ServeClient(TFd &fd, const TAddress &) {
    std::shared_ptr<TConnection> connection = make_shared<TConnection>(this, std::move(fd));
    try {
      /* Run the server thru one cycle. */ {
        /* Read the request. */
        auto request = connection->Read();
        /* Service the request. */
        (*request)();
      }
    } catch (const std::exception &ex) {
    }
  }

  bool Running;

  /* Set true by AcceptClientConnections() once it has exited. */
  bool Done;

  std::mutex StatusLock;
  std::condition_variable Finish;

  Base::TScheduler *const Scheduler;

  Base::TFd MainSocket;

  Base::TEventCounter TermFd;

  in_port_t PortNum;

  char Status;

};

std::mutex ExpectedHostLock;
int ExpectedHost = 0;

class TTestClient {
  NO_COPY(TTestClient);
  public:

  TTestClient(TScheduler *scheduler, const TAddress &server_addr)
      : ServerAddr(server_addr), Running(true), Done(false) {
    scheduler->Schedule(bind(&TTestClient::Runner, this));
  }

  ~TTestClient() {
    /* Tell the runner to stop and wait until it has actually exited before we
       tear ourselves (and our connection objects) down.  Without this handshake
       the runner's job can still be calling virtual methods on this object (and
       on the TConnection it owns) after the object's vtable has begun
       transitioning during destruction, which manifests as a
       'pure virtual method called' abort. */
    std::unique_lock<std::mutex> lock(DoneMutex);
    Running = false;
    while (!Done) {
      DoneCond.wait(lock);
    }
  }

  void Runner() {
    for (;Running;) {
      try {
        TFd new_server_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        /* Bound every read and write on this socket.  We hold ExpectedHostLock
           across the RPC below, and the fixture's switch-hosts block takes the
           same lock; if a reply is ever lost (say, the balancer dropped the
           proxied connection mid-flight), an unbounded read here would freeze
           the whole test.  With the timeout, a lost reply throws, we drop the
           lock, and the loop simply retries. */ {
          timeval timeout{.tv_sec = 1, .tv_usec = 0};
          IfLt0(setsockopt(new_server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
          IfLt0(setsockopt(new_server_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
        }
        Connect(new_server_socket, ServerAddr);
        std::shared_ptr<TConnection> connection = make_shared<TConnection>(std::move(new_server_socket));
        /* get port number */ {
          std::lock_guard<std::mutex> lock(ExpectedHostLock);
          auto future = connection->Write<int>(TTestServer::GetPortId);
          connection->Read();
          int from_host = **future;
          EXPECT_EQ(from_host, ExpectedHost);
        }
      } catch (const Io::TInputConsumer::TPastEndError &ex) {
        /* do nothing */
      } catch (const std::exception &ex) {
      }
    }
    /* Signal the destructor that the runner has fully unwound. */
    std::lock_guard<std::mutex> lock(DoneMutex);
    Done = true;
    DoneCond.notify_all();
  }

  private:

  class TConnection
      : public Rpc::TContext {
    NO_COPY(TConnection);
    public:

    TConnection(Base::TFd &&fd)
        : TContext(TProtocol::Protocol), Device(make_shared<TDevice>(move(fd))) {
      BinaryIoStream = make_shared<TBinaryIoStream>(Device);
    }

    private:

    class TProtocol
        : public Rpc::TProtocol {
      NO_COPY(TProtocol);
      public:

      static const TProtocol Protocol;

      private:

      TProtocol() {
      }

    };  // TProtocol

    shared_ptr<TDevice> Device;

  };

  const TAddress ServerAddr;

  bool Running;

  /* Covers Done. */
  std::mutex DoneMutex;
  std::condition_variable DoneCond;

  /* Set true by Runner() once it has exited its loop. */
  bool Done;

};

bool TRouter::CheckHost(const Socket::TAddress &addr) const {
  TFd new_server_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  try {
    Connect(new_server_socket, addr);
    std::shared_ptr<TConnection> connection = make_shared<TConnection>(std::move(new_server_socket));
    auto future = connection->Write<char>(TTestServer::HealthCheckId);
    connection->Read();
    return **future == 'M';
  } catch (const std::exception &ex) {
    return false;
  }
  return false;
}

const TRouter::TConnection::TProtocol TRouter::TConnection::TProtocol::Protocol;
const TTestServer::TConnection::TProtocol TTestServer::TConnection::TProtocol::Protocol;
const TTestClient::TConnection::TProtocol TTestClient::TConnection::TProtocol::Protocol;

FIXTURE(Typical) {
  /* Dedicated test ports, deliberately away from DefaultPortNumber (19380) and
     DefaultSlavePortNumber (19381).  On the default service ports, any stray Orly
     activity on the machine (an interactive orlyi, a retrying failover slave, a
     client) lands foreign connections on the balancer's listener mid-test.
     tests/restart_test.sh dodges the same trap with its own dedicated range. */
  const in_port_t router_port = 19480, server_1_port = 19481, server_2_port = 19482;
  const auto check_interval = 500ms;
  TBalancer::TCmd cmd;
  cmd.PortNumber = router_port;
  const TScheduler::TPolicy scheduler_policy(4, 1000, milliseconds(1000));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  TAddress router_address(TAddress::IPv4Loopback, router_port);
  TAddress server_1_address(TAddress::IPv4Loopback, server_1_port);
  TAddress server_2_address(TAddress::IPv4Loopback, server_2_port);
  TRouter router(&scheduler, cmd, check_interval);
  auto test_server_1 = make_unique<TTestServer>(&scheduler, server_1_port, 'M');
  router.AddHost(server_1_address);
  auto test_server_2 = make_unique<TTestServer>(&scheduler, server_2_port, 'S');
  router.AddHost(server_2_address);
  router.Wait();
  /* set master */ {
    std::lock_guard<std::mutex> lock(ExpectedHostLock);
    ExpectedHost = server_1_port;
  }
  /* The client runs inside its own scope so that its runner job is fully
     stopped (its destructor blocks until the runner has exited) before we
     start tearing the scheduler and the remaining objects down. */ {
    TTestClient client_1(&scheduler, router_address);
    sleep(3);
    router.RemoveHost(server_1_address);
    /* switch hosts */ {
      std::lock_guard<std::mutex> lock(ExpectedHostLock);
      test_server_1->ChangeStatus('S');
      ExpectedHost = server_2_port;
      test_server_2->ChangeStatus('M');
      test_server_1.reset();
    }
    sleep(3);
  }  // client_1's runner is now stopped.
  /* Ordered teardown.  Drain every worker job out of the scheduler while the
     objects those jobs dispatch into (the router and the remaining test
     server) are still fully alive.  Until this point the scheduler's worker
     threads may still be running jobs -- the router's accept/serve loop (which
     calls the virtual ChooseHost()/OnError() hooks) and the servers' accept
     loop -- and if those objects were destroyed first, a worker could invoke a
     virtual method on an object whose vtable is mid-destruction, aborting with
     'pure virtual method called'.  Shutting the scheduler down here joins all
     of that work to a stop before any of these objects begins destructing. */
  scheduler.Shutdown(milliseconds(1000));
  test_server_2.reset();
}