/* <orly/balancer/balancer.h>

   `Balancer::TBalancer` -- abstract base for orly's TCP load
   balancer. Holds command-line parsing (`TCmd`: port + connection
   backlog) and the abstract host-selection hook. Concrete subclass
   is `TFailoverTestBalancer`.

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

#pragma once

#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <base/class_traits.h>
#include <base/cmd.h>
#include <base/event_counter.h>
#include <base/fd.h>
#include <base/log.h>
#include <base/scheduler.h>
#include <base/socket/address.h>

namespace Orly {

  namespace Balancer {

    class TBalancer {
      NO_COPY(TBalancer);
      public:

      /* Command-line arguments. */
      class TCmd : public Base::TLog::TCmd {
        public:

        /* Construct with defaults. */
        TCmd() : PortNumber(19380), ConnectionBacklog(5000) {}

        /* Construct from argc/argv. */
        TCmd(int argc, char *argv[])
            : TCmd() {
          Parse(argc, argv, TMeta());
        }

        /* The port on which we respond to TCP. */
        in_port_t PortNumber;

        /* The maximum number of connection requests to backlog against MainSocket. */
        int ConnectionBacklog;

        private:

        /* Our meta-type. */
        class TMeta
            : public Base::TCmd::TMeta {
          public:

          /* Registers our fields. */
          TMeta()
              : Base::TCmd::TMeta("The TCP Traffic Balancer.") {
            Param(
                &TCmd::PortNumber, "port", Optional, "port\0",
                "The port on which we listen for incoming traffic."
            );
            Param(
                &TCmd::ConnectionBacklog, "connection_backlog", Optional, "connection_backlog\0cb\0",
                "The maximum number of client connection requests to backlog."
            );
          }

        };  // TCmd::TMeta

      };  // TCmd

      virtual ~TBalancer();

      protected:

      TBalancer(Base::TScheduler *scheduler, const TCmd &cmd);

      /* Stops accepting and serving: refuses jobs that haven't started yet, pokes the term fd so
         every job parked in an epoll unwinds, and blocks until all started jobs have exited.
         Idempotent.  A concrete subclass must call this at the top of its destructor -- before the
         state (and vtable slots) that ServeClient() reaches through ChooseHost()/OnError() start
         going away; ~TBalancer() calls it again harmlessly. */
      void StopServing();

      /* Accepts connections from clients on our main socket.  Scheduled as a job by the constructor. */
      void AcceptClientConnections();

      /* Serves a client on the given fd.  Scheduled as a job by AcceptClientConnections() when a client connects. */
      void ServeClient(Base::TFd &fd, const Socket::TAddress &client_address);

      virtual const Socket::TAddress &ChooseHost() = 0;

      virtual void OnError(const std::exception &ex) = 0;

      private:

      /* The teardown handshake shared with every job we schedule.  It lives behind a shared_ptr so
         a job the scheduler runs late -- or drops unrun -- touches only this block, never a balancer
         that may already be destroyed. */
      class THandshake {
        public:

        /* Called by a job before it touches the balancer.  False means StopServing() has begun and
           the job must return immediately without touching anything else. */
        bool Enter() {
          std::lock_guard<std::mutex> lock(Mutex);
          if (!Serving) {
            return false;
          }
          ++InFlightCount;
          return true;
        }

        /* Called by a job as it unwinds, however it unwinds. */
        void Exit() {
          std::lock_guard<std::mutex> lock(Mutex);
          --InFlightCount;
          AllDone.notify_all();
        }

        /* Covers Serving and InFlightCount. */
        std::mutex Mutex;

        /* Notified each time InFlightCount drops. */
        std::condition_variable AllDone;

        /* Set false by StopServing(). */
        bool Serving = true;

        /* The number of jobs currently inside the balancer. */
        size_t InFlightCount = 0;

        /* In every job's epoll set; pushed by StopServing() to wake them all. */
        Base::TEventCounter TermFd;

      };  // TBalancer::THandshake

      /* Schedule a job that participates in the shutdown handshake. */
      void ScheduleJob(std::function<void ()> &&job);

      /* The scheduler we use to launch jobs.  Set by the constructor and never changed. */
      Base::TScheduler *const Scheduler;

      /* Our teardown handshake.  Set by the constructor and never changed. */
      const std::shared_ptr<THandshake> Handshake;

      /* The socket on which AcceptClientConnections() listens. */
      Base::TFd MainSocket;

    };  // TBalancer

  }   // Balancer

}  // Orly