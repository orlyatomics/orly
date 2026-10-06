/* <orly/rt/shortest_path.h>

   `BidirShortestPath<...>`: bidirectional BFS over a key-cursor
   graph in an orly index. Returns shortest paths between `src` and
   `target`, each a vector of edge keys, or the limit the search hit
   (see <orly/rt/bidir_bfs.h>). The graph is taken as undirected: an
   edge keyed <[a, ..., b]> is assumed to have its twin <[b, ..., a]>.
   No orlyscript builtin emits a call to it yet.

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
#include <vector>

#include <base/class_traits.h>
#include <base/uuid.h>
#include <orly/indy/fiber/extern_fiber.h>
#include <orly/package/api.h>
#include <orly/package/rt.h>
#include <orly/rt/bidir_bfs.h>

namespace Orly {

  namespace Rt {

    /* `num_parallel` fibers read the cursors of a frontier's nodes at once,
       so one blocked on I/O doesn't stall the others. Each search level
       expands the smaller frontier, and the search stops at `limits` (#695). */
    template <typename TKeyType, typename TSearchType, size_t SrcPos, size_t TargetPos>
    TShortestPathResult<TKeyType> BidirShortestPath(Orly::Package::TContext &ctx,
                                                    const size_t num_parallel,
                                                    const Base::TUuid &idx_id,
                                                    const TSearchType &val_args,
                                                    const std::tuple_element_t<SrcPos, TKeyType> &src,
                                                    const std::tuple_element_t<TargetPos, TKeyType> &target,
                                                    const TShortestPathLimits &limits = TShortestPathLimits()) {
      using match_t = std::tuple_element_t<SrcPos, TKeyType>;
      static_assert(std::is_same<match_t, std::tuple_element_t<TargetPos, TKeyType>>::value, "Cannot follow links of differing types");
      Atom::TSuprena arena;
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      TSearchType search_key_tuple = val_args;
      /* Emit every edge of every node in `frontier`. On the target side, an
         edge <[node, ..., neighbor]> is reported as its twin <[neighbor, ...,
         node]>, which is the way a path from `src` crosses it. */
      auto expand = [&](const std::vector<match_t> &frontier, bool from_target, auto &emit) {
        Indy::ExternFiber::TSync extern_sync(num_parallel);
        auto iter = frontier.begin();
        bool keep_going = true;
        auto sub_scanner_func = [&]() {
          TKeyType k;
          while (keep_going && iter != frontier.end()) {
            const match_t &node = *iter;
            ++iter; // increment the iterator before we have a chance to block
            std::get<SrcPos>(search_key_tuple) = node;
            const Indy::TIndexKey search_key(idx_id, Indy::TKey(search_key_tuple, &arena, state_alloc));
            std::unique_ptr<TKeyCursor> kcp(ctx.NewKeyCursor(&ctx.GetFlux(), search_key));
            for (auto &kc = *kcp; keep_going && kc; ++kc) {
              Sabot::ToNative(*Sabot::State::TAny::TWrapper(kc->GetState(state_alloc)), k);
              const match_t neighbor = std::get<TargetPos>(k);
              if (from_target) {
                std::swap(std::get<SrcPos>(k), std::get<TargetPos>(k));
              }
              keep_going = emit(node, neighbor, k);
            }
          }
          extern_sync.Complete();
        };
        for (size_t i = 0; i < num_parallel; ++i) {
          Indy::ExternFiber::SchedTaskLocally(sub_scanner_func);
        }
        extern_sync.Sync();
      };
      return BidirBfs<match_t, TKeyType>(src, target, limits, expand);
    }

  }  // Rt

}  // Orly
