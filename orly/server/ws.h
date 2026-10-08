/* <orly/server/ws.h>

   The websockets server object used by the main server.

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

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <netinet/in.h>

#include <base/class_traits.h>
#include <optional>
#include <base/uuid.h>
#include <orly/pov_review.h>
#include <orly/method_request.h>
#include <orly/method_result.h>
#include <orly/package/manager.h>
#include <orly/server/batch_call.h>
#include <orly/var.h>

namespace Orly {

  namespace Server {

     /* An interface to a websocket server.  We use an interface-only definition
        here to avoid bloating up the build with a lot of websocket-specific
        headers. */
      class TWs {
        NO_COPY(TWs);
        public:

        /* Inherit and finalize this class to handle per-session operations.
           The websockets subsystem will destroy this interface object when the connection
           goes away, so, in the Orly sense, it's not the session (which is durable), but
           a pin in the session to indicate it is in use. */
        class TSessionPin {
          NO_COPY(TSessionPin);
          public:

          /* Do-little. */
          virtual ~TSessionPin() {}

          /* Override to perform the request. */
          virtual void BeginImport() const = 0;

          /* Override to perform the request. */
          virtual void EndImport() const = 0;

          /* The id used to resume the session later. */
          virtual const Base::TUuid &GetId() const = 0;

          /* Override to perform the request. */
          virtual void Import(
              const std::string &file_pattern, const std::string &pkg_name, int64_t num_load_threads,
              int64_t num_merge_threads, int64_t merge_simultaneous) const = 0;

          /* Override to perform the request. */
          virtual void InstallPackage(const std::vector<std::string> &name, uint64_t version) const = 0;

          /* Override to perform the request. */
          virtual Base::TUuid NewPov(bool is_safe, bool is_shared, const std::optional<Base::TUuid> &parent_id) const = 0;

          /* Override to perform the request. */
          virtual void PausePov(const Base::TUuid &pov_id) const = 0;

          /* Override to perform the request. */
          virtual void SetTtl(const Base::TUuid &durable_id, const std::chrono::seconds &ttl) const = 0;

          /* Override to perform the request. */
          virtual void SetUserId(const Base::TUuid &user_id) const = 0;

          /* Override to perform the request. */
          virtual void Tail() const = 0;

          /* Override to perform the request. */
          virtual TMethodResult Try(const TMethodRequest &method_request) const = 0;

          /* Override to perform the POV review requests (#746; see <orly/pov_review.h>). */
          virtual Base::TUuid NewReviewPov(bool is_safe, bool is_shared, const std::optional<Base::TUuid> &parent_id,
                                           TConflictMode mode) const = 0;
          virtual TPovDiff DiffPov(const Base::TUuid &pov_id, const TPovDiffOptions &options) const = 0;
          virtual TPovDiscard DiscardPov(const Base::TUuid &pov_id) const = 0;
          virtual TPovPromote PromotePov(const Base::TUuid &pov_id, bool force) const = 0;
          virtual TPovReview ReviewPov(const Base::TUuid &pov_id, uint64_t after) const = 0;

          /* Override to perform a batch request: one method against N argument
             records, folded into a single transaction (#253). Returns a
             list-typed result with one entry per call, in statement order. */
          virtual TMethodResult TryBatch(
              const Base::TUuid &pov_id, const std::vector<std::string> &fq_name,
              const std::vector<TClosure> &closures) const = 0;

          /* Override to perform a mixed batch: N calls, each naming its own package and
             method, folded into a single transaction (#255). Returns one result per call,
             in statement order; they may differ in type. */
          virtual std::vector<Var::TVar> TryMulti(
              const Base::TUuid &pov_id, const std::vector<Server::TBatchCall> &calls) const = 0;

          /* Override to perform the request. */
          virtual void UninstallPackage(const std::vector<std::string> &name, uint64_t version) const = 0;

          /* Override to perform the request. */
          virtual void UnpausePov(const Base::TUuid &pov_id) const = 0;

          protected:

          /* Do-little. */
          TSessionPin() {}

        };  // TWs::TSessionPin

        /* Inherit and finalize this class to handle per-connection.
           To the websockets subsystem, this interface represents the rest of the server. */
        class TSessionManager {
          NO_COPY(TSessionManager);
          public:

          /* For the convenience of those who inherit from us. */
          using TSessionPin = TWs::TSessionPin;

          /* Get the package manager. Useful for things like iterating over all installed packages. */
          virtual const Package::TManager &GetPackageManager() const = 0;

          /* Called when the connection wishes to create a new session. */
          virtual TSessionPin *NewSession() = 0;

          /* Called when the connection wishes to resume an old session. */
          virtual TSessionPin *ResumeSession(const Base::TUuid &id) = 0;

          virtual bool ForEachIndex(const std::function<
              bool(const std::string &pkg, const std::string &key_type, const std::string &val_type)> &cb) const = 0;

          /* Run a statement's work (a try, a batch or a multi; #761) somewhere other than the
             websocket I/O thread that read it, and return without waiting for it.  The work calls
             the session pin's methods, catches everything itself and reports its own completion,
             so an implementation only has to run it once.  If this throws, the work has not run
             and never will.

             The server runs it on a fiber on its fast runners, so as many statements can be in
             flight as admission allows (TWs::New's max_in_flight), not one per I/O thread.  This
             default runs it on the calling thread before returning, which is what a session
             manager whose pins don't need fiber context (the test server) wants. */
          virtual void RunStatement(std::function<void ()> &&work) {
            work();
          }

          protected:

          /* Do-little. */
          TSessionManager() {}

          /* Do-little. */
          virtual ~TSessionManager() {}

        };  // TWs::TSessionManager

        /* Shut down the server. */
        virtual ~TWs() {}

        /* Use this factory to construct an instance of this class.
           The server will be open for business by the time this function
           returns.  To shut down the server, destroy the object.

           The listener binds bind_address, an IPv4 or IPv6 literal; loopback
           by default, because the protocol has no authentication (#705).
           Unless allow_remote_compile is true, the `compile` statement is
           refused with "status": "remote_compile_disabled".

           A non-empty auth_token (#710) must be the first message on every
           connection, as {"auth": "<token>"}; anything else is answered with
           "status": "unauthorized" and the connection is closed, before any
           statement runs.  Empty, connections are unchanged.

           thread_count is the number of I/O threads.  They read, parse and
           reply; try, batch and multi statements run through
           TSessionManager::RunStatement() and don't hold an I/O thread while
           they execute (#761).  At most max_in_flight of those run at once
           (0: no limit); the rest wait in arrival order.  A connection has at
           most one statement in flight, since its next message isn't read
           until the reply to the last one is sent, so each session's
           statements still run in order. */
        static TWs *New(
            TSessionManager *session_mngr, size_t thread_count,
            in_port_t port_number = 8080,
            const std::string &bind_address = "127.0.0.1",
            bool allow_remote_compile = false,
            const std::string &auth_token = std::string(),
            size_t max_in_flight = 0);

        protected:

        /* Start up the server. */
        TWs() {}

    };  // TWs

  }  // Server

}  // Orly
