/* <orly/balancer/failover_test_balancer.h>

   `TFailoverTestBalancer` -- a `TBalancer` impl that periodically
   probes registered backend hosts and routes new connections to
   whichever is currently reachable. Used in failover-test rigs to
   verify the balancer's host-selection behaviour under simulated
   outages.

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

#include <optional>

#include <base/event_semaphore.h>
#include <orly/balancer/balancer.h>

namespace Orly {

  namespace Balancer {

    class TFailoverTestBalancer
        : public TBalancer {
      NO_COPY(TFailoverTestBalancer);
      public:

      TFailoverTestBalancer(Base::TScheduler *scheduler,
                            const TBalancer::TCmd &cmd,
                            std::chrono::milliseconds interval);

      virtual ~TFailoverTestBalancer();

      virtual const Socket::TAddress &ChooseHost();

      void RegisterHost(const Socket::TAddress &addr);

      private:

      void CheckHosts();

      bool CheckHost(const Socket::TAddress &addr) const;

      virtual void OnError(const std::exception &ex);

      std::unordered_set<Socket::TAddress> HostSet;
      std::optional<Socket::TAddress> MasterHost;
      std::mutex HostMutex;
      std::condition_variable HostCond;

      std::chrono::milliseconds Interval;

      bool Running;

      Base::TEventSemaphore ErrorSem;

    };  // TFailoverTestBalancer

  }  // Balancer

}  // Orly