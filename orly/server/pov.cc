/* <orly/server/pov.cc>

   Implements <orly/server/pov.h>.

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

#include <orly/server/pov.h>
#include <optional>
#include <sstream>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Server;

const Indy::L0::TManager::TPtr<Indy::TRepo> &TPov::GetRepo(const TServer *server) const {
  std::lock_guard lock(RepoLock);
  if (!Repo) {
    assert(server);
    auto repo_manager = server->GetRepoManager();
    if (Reloaded) {
      /* This pov came back from a durable disk layer, so its repo predates
         this object.  ATTACH if the repo is alive (on a promoted slave,
         replication built it); NEVER create.  Creating would resurrect the
         pov as an empty shell parented to global, silently swallowing every
         update tetris had not yet promoted at shutdown (#439). */
      Repo = repo_manager->TryGetLiveRepo(GetId());
      if (!Repo) {
        std::ostringstream strm;
        strm << "pov {" << GetId() << "} cannot be revived: point-of-view state does not survive a server restart"
                " (povs are ephemeral, #439); create a new pov";
        throw std::runtime_error(strm.str());
      }
    } else {
      std::optional<Indy::L0::TManager::TPtr<Indy::L0::TManager::TRepo>> parent_repo;
      if (SharedParents.empty()) {
        parent_repo = server->GetGlobalRepo();
      } else {
        /* The parent's repo must be live.  After a restart the parent pov comes back from disk
           without it (#439), and opening it by force built an empty repo with no parent: this pov
           then read and wrote through a parent that went nowhere, instead of failing as the
           parent itself does (#671). */
        parent_repo = repo_manager->TryGetLiveRepo(SharedParents.back());
        if (!*parent_repo) {
          std::ostringstream strm;
          strm << "pov {" << GetId() << "} cannot be made under pov {" << SharedParents.back()
               << "}: that pov's state is gone, and point-of-view state does not survive a server restart"
                  " (povs are ephemeral, #439); create a new pov";
          throw std::runtime_error(strm.str());
        }
      }
      Repo = repo_manager->GetRepo(GetId(), GetTtl(), parent_repo, Policy == TPolicy::Safe, true);
    }
  }
  return Repo;
}

TPov::TPov(Durable::TManager *manager,
           const Base::TUuid &id,
           const Durable::TTtl &ttl,
           const TUuid &session_id,
           TAudience audience,
           TPolicy policy,
           const TSharedParents &shared_parents,
           bool reloaded)
    : TObj(manager, id, ttl),
      SessionId(session_id),
      Audience(audience),
      Policy(policy),
      SharedParents(shared_parents),
      Reloaded(reloaded) {}

TPov::TPov(Durable::TManager *manager, const Base::TUuid &id, Io::TBinaryInputStream &strm)
    : TObj(manager, id, strm), Reloaded(true) {
  char audience_char{};
  char policy_char{};
  strm >> SessionId >> audience_char >> policy_char >> SharedParents;
  Audience = static_cast<TAudience>(audience_char);
  Policy = static_cast<TPolicy>(policy_char);
}

TPov::~TPov() {
  /* Past our deadline, nothing can open this pov again, so nothing can open its repo either.
     Without this the repo, closed by our letting go, would sit in the repo cache for another
     ttl of its own, holding its pool blocks and its WAL pin (#914).  Before the deadline (cache
     pressure, or shutdown) the pov can be reloaded and attach to its repo, so that stays cached. */
  const auto &deadline = GetDeadline();
  if (Repo && deadline && *deadline <= Durable::TDeadline::clock::now()) {
    Repo->DiscardWhenClosed();
  }
}

void TPov::Write(Io::TBinaryOutputStream &strm) const {
  TObj::Write(strm);
  strm << SessionId << static_cast<char>(Audience) << static_cast<char>(Policy);
  strm << SharedParents.size();
  for (const auto &id : SharedParents) {
    strm << id;
  }
}
