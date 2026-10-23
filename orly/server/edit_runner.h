/* <orly/server/edit_runner.h>

   Where a statement's write is committed (--edit_runner).

   Every statement runs on a fast runner (#761, #908): the server can't tell a read from a write
   until the method has run, since only then does it know whether the call left effects. Most of
   a write's cost comes after that point, though: resolving its effects, encoding its meta record,
   building the update and pushing it into the repo's memory layer. Under a heavy ingest that
   commit work takes a share of the fast cores the reads need.

   So once a statement knows it has effects, it may move to a slow runner for the commit and come
   back to its fast runner when the commit is done (TEditRunnerSwitch):

     fast   the commit stays on the fast runner, as before.
     slow   the commit always moves to the least-loaded slow runner.
     spill  the commit moves to the least-loaded slow runner while that runner has fewer than
            --edit_spill_load commits on it; otherwise it stays on the fast runner.

   Nothing about the commit changes but the thread it runs on. A fiber may run on any runner; it
   already hops between a slow and a fast runner for every binary-protocol statement
   (TServer::TFastRunnerSwitch). The order in which writes commit comes from the transaction
   (sequence numbers, the repo's locks), not from the runner; the statement still finishes, and
   frees its frame, on the runner it started on.

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

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <base/class_traits.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/server/runner_balancer.h>

namespace Orly {

  namespace Server {

    /* See above. */
    enum class TEditRunner { Fast, Slow, Spill };

    /* The --edit_runner value for 'mode'. */
    [[nodiscard]] constexpr const char *ToString(TEditRunner mode) noexcept {
      switch (mode) {
        case TEditRunner::Fast: return "fast";
        case TEditRunner::Slow: return "slow";
        case TEditRunner::Spill: return "spill";
      }
      return "?";
    }

    /* The mode an --edit_runner value names; throws std::invalid_argument for anything else. */
    [[nodiscard]] inline TEditRunner ParseEditRunner(std::string_view text) {
      for (auto mode: {TEditRunner::Fast, TEditRunner::Slow, TEditRunner::Spill}) {
        if (text == ToString(mode)) {
          return mode;
        }
      }
      throw std::invalid_argument("--edit_runner must be fast, slow or spill, not \"" + std::string(text) + '"');
    }

    /* The slow runner a commit goes to, as a lease on it; an empty lease means the commit stays
       where it is. 'balancer' may be null (no slow runners), and then the commit stays too. */
    [[nodiscard]] inline TRunnerBalancer::TLease PickEditRunner(
        TEditRunner mode, TRunnerBalancer *balancer, size_t spill_load) noexcept {
      if (!balancer) {
        return {};
      }
      switch (mode) {
        case TEditRunner::Fast: return {};
        case TEditRunner::Slow: return balancer->Acquire();
        case TEditRunner::Spill: return balancer->AcquireBelow(spill_load);
      }
      return {};
    }

    /* For its lifetime, puts the calling fiber on the slow runner PickEditRunner() chooses, if it
       chooses one, and counts it in that runner's load; then switches back to where it came from.
       Must be made on a fiber. */
    class TEditRunnerSwitch final {
      NO_COPY(TEditRunnerSwitch);
      public:

      TEditRunnerSwitch(TEditRunner mode, TRunnerBalancer *balancer,
          const std::vector<std::unique_ptr<Indy::Fiber::TRunner>> &runners, size_t spill_load)
          : Lease(PickEditRunner(mode, balancer, spill_load)) {
        if (Lease) {
          Switcher.emplace(runners[Lease.GetIndex()].get());
        }
      }

      /* True iff the fiber moved. */
      [[nodiscard]] bool Moved() const noexcept {
        return Switcher.has_value();
      }

      private:

      /* Declared in this order so that we switch back before the lease is released. */
      TRunnerBalancer::TLease Lease;
      std::optional<Indy::Fiber::TSwitchToRunner> Switcher;

    };  // TEditRunnerSwitch

  }  // Server

}  // Orly
