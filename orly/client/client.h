/* <orly/client/client.h>

   The client end of the full-duplex RPC connection.

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
#include <chrono>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

#include <base/class_traits.h>
#include <base/event_semaphore.h>
#include <base/fd.h>
#include <optional>
#include <base/uuid.h>
#include <base/io/device.h>
#include <base/rpc/rpc.h>
#include <base/socket/address.h>
#include <orly/closure.h>
#include <orly/method_result.h>

namespace Orly {

  namespace Client {

    /* The client end of the full-duplex RPC connection. */
    class TClient
        : public Rpc::TContext {
      NO_COPY(TClient);
      public:

      virtual ~TClient();

      std::shared_ptr<Rpc::TFuture<std::string>> Echo(const std::string &msg);

      std::shared_ptr<Rpc::TFuture<void>> InstallPackage(const std::vector<std::string> &package_name, uint64_t version);

      std::shared_ptr<Rpc::TFuture<void>> UnInstallPackage(const std::vector<std::string> &package_name, uint64_t version);

      std::shared_ptr<Rpc::TFuture<Base::TUuid>> NewFastPrivatePov(const std::optional<Base::TUuid> &parent_pov_id,
                                                                   const std::chrono::seconds &ttl = std::chrono::seconds(600));

      std::shared_ptr<Rpc::TFuture<Base::TUuid>> NewSafePrivatePov(const std::optional<Base::TUuid> &parent_pov_id,
                                                                   const std::chrono::seconds &ttl = std::chrono::seconds(600));

      std::shared_ptr<Rpc::TFuture<Base::TUuid>> NewFastSharedPov(const std::optional<Base::TUuid> &parent_pov_id,
                                                                  const std::chrono::seconds &ttl = std::chrono::seconds(600));

      std::shared_ptr<Rpc::TFuture<Base::TUuid>> NewSafeSharedPov(const std::optional<Base::TUuid> &parent_pov_id,
                                                                  const std::chrono::seconds &ttl = std::chrono::seconds(600));

      std::shared_ptr<Rpc::TFuture<void>> PausePov(const Base::TUuid &pov_id);

      std::shared_ptr<Rpc::TFuture<void>> UnpausePov(const Base::TUuid &pov_id);

      std::shared_ptr<Rpc::TFuture<void>> SetUserId(const Base::TUuid &user_id);

      std::shared_ptr<Rpc::TFuture<void>> SetTimeToLive(const Base::TUuid &durable_id, const std::chrono::seconds &time_to_live);

      std::shared_ptr<Rpc::TFuture<TMethodResult>> Try(const Base::TUuid &pov_id, const std::vector<std::string> &fq_name, const TClosure &closure);

      std::shared_ptr<Rpc::TFuture<void>> BeginImport();

      std::shared_ptr<Rpc::TFuture<void>> EndImport();

      std::shared_ptr<Rpc::TFuture<void>> TailGlobalPov();

      std::shared_ptr<Rpc::TFuture<std::string>> ImportCoreVector(const std::string &file_pattern, const std::string &pkg_name, int64_t num_load_threads, int64_t num_merge_threads, int64_t merge_simultaneous);

      const std::optional<Base::TUuid> &GetSessionId() const {
        return SessionId;
      }

      const std::chrono::seconds &GetTimeToLive() const {
        return TimeToLive;
      }

      protected:

      TClient(const Socket::TAddress &server_address, const std::optional<Base::TUuid> &session_id, const std::chrono::seconds &time_to_live);

      virtual void OnPovFailed(const Base::TUuid &repo_id) = 0;

      virtual void OnUpdateAccepted(const Base::TUuid &repo_id, const Base::TUuid &tracking_id) = 0;

      virtual void OnUpdateReplicated(const Base::TUuid &repo_id, const Base::TUuid &tracking_id) = 0;

      virtual void OnUpdateDurable(const Base::TUuid &repo_id, const Base::TUuid &tracking_id) = 0;

      virtual void OnUpdateSemiDurable(const Base::TUuid &repo_id, const Base::TUuid &tracking_id) = 0;

      private:

      class TProtocol
          : public Rpc::TProtocol {
        NO_COPY(TProtocol);
        public:

        static const TProtocol Protocol;

        private:

        TProtocol();

      };  // TClient::TProtocol

      void DispatchMain();

      /* Handles background I/O with the server. */
      void IoMain();

      Socket::TAddress ServerAddress;

      std::optional<Base::TUuid> SessionId;

      std::chrono::seconds TimeToLive;

      std::shared_ptr<Io::TDevice> Device;

      Base::TFd InternalSocket;

      /* Runs IoMain(). */
      std::thread DispatchThread, IoThread;

      /* Set (before Destructing is pushed) so the service threads can tell
         an expected teardown-window exception from a real one (#503). */
      std::atomic<bool> DestructionUnderway = false;

      Base::TEventSemaphore Destructing;

    };  // TClient

  }  // Client

}  // Orly
