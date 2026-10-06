/* <orly/rt/bidir_bfs.h>

   `BidirBfs<...>`: the search behind `BidirShortestPath`, kept apart from
   the index so it can be tested on an in-memory graph. A bidirectional
   breadth-first search over an undirected graph, which always expands the
   smaller of its two frontiers, remembers one parent edge per node instead of
   a whole path, and stops at a depth or node limit with a status that says
   which one it hit (#695).

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

#include <algorithm>
#include <cstddef>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Orly {

  namespace Rt {

    /* How a shortest-path search ended. */
    enum class TShortestPathStatus {

      /* At least one shortest path was found. */
      Found,

      /* One side ran out of nodes to visit: there is no path. */
      NoPath,

      /* Any path would be longer than `MaxDepth` edges. */
      DepthLimit,

      /* More than `MaxNodes` nodes were visited before a path was found. */
      NodeLimit

    };  // TShortestPathStatus

    /* Bounds on a shortest-path search. The defaults are the server's: a
       query that hits one gets a `DepthLimit` or `NodeLimit` result rather
       than walking the whole connected component. */
    struct TShortestPathLimits {

      /* The longest path, in edges, the search will look for. */
      static constexpr size_t DefaultMaxDepth = 16;

      /* How many nodes, counting both sides, the search may visit. */
      static constexpr size_t DefaultMaxNodes = 100000;

      size_t MaxDepth = DefaultMaxDepth;

      size_t MaxNodes = DefaultMaxNodes;

    };  // TShortestPathLimits

    template <typename TEdge>
    struct TShortestPathResult {

      TShortestPathStatus Status = TShortestPathStatus::NoPath;

      /* When `Found`: shortest paths from source to target, all the same
         length, each a sequence of edges in path order. One per edge on
         which the two searches met, so two paths may share a prefix. Empty
         otherwise. */
      std::vector<std::vector<TEdge>> Paths;

      /* Nodes visited, counting both sides. */
      size_t NodesVisited = 0;

      /* Edges scanned, counting both sides. */
      size_t EdgesScanned = 0;

    };  // TShortestPathResult

    /* Searches from `src` and `target` toward each other. `expand(frontier,
       from_target, emit)` must call `emit(node, neighbor, edge)` for each edge
       of each node in `frontier`, with `edge` oriented in path order: from
       `node` to `neighbor` on the source side (`from_target` false), from
       `neighbor` to `node` on the target side. `emit` returns false once the
       search has seen enough, and `expand` may then stop early. */
    template <typename TNode, typename TEdge, typename TExpand>
    TShortestPathResult<TEdge> BidirBfs(const TNode &src,
                                        const TNode &target,
                                        const TShortestPathLimits &limits,
                                        TExpand &&expand) {
      using edge_vec_t = std::vector<TEdge>;
      /* Each visited node maps to the edge it was reached by and the node at
         the other end of it; the roots map to nothing. */
      using parent_t = std::optional<std::pair<TNode, TEdge>>;
      using visited_t = std::unordered_map<TNode, parent_t>;
      TShortestPathResult<TEdge> result;
      if (src == target) {
        result.Status = TShortestPathStatus::Found;
        result.Paths.emplace_back();
        result.NodesVisited = 1;
        return result;
      }
      visited_t from_src, from_target;
      from_src.emplace(src, std::nullopt);
      from_target.emplace(target, std::nullopt);
      std::vector<TNode> src_frontier{src}, target_frontier{target};
      size_t src_depth = 0, target_depth = 0;
      result.NodesVisited = 2;
      /* The edges from `node` back to its root, in path order. */
      auto path_to_src = [&](const TNode &node, edge_vec_t &out) {
        const size_t start = out.size();
        for (const parent_t *p = &from_src.at(node); *p; p = &from_src.at((*p)->first)) {
          out.push_back((*p)->second);
        }
        std::reverse(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
      };
      auto path_to_target = [&](const TNode &node, edge_vec_t &out) {
        for (const parent_t *p = &from_target.at(node); *p; p = &from_target.at((*p)->first)) {
          out.push_back((*p)->second);
        }
      };
      for (;;) {
        if (src_frontier.empty() || target_frontier.empty()) {
          result.Status = TShortestPathStatus::NoPath;
          return result;
        }
        if (src_depth + target_depth + 1 > limits.MaxDepth) {
          result.Status = TShortestPathStatus::DepthLimit;
          return result;
        }
        /* Expand the smaller frontier; on a tie, the side that has visited
           fewer nodes, and the target side if that ties too. A target with
           no edges therefore ends the search before the source's are read. */
        const bool from_target_side =
            target_frontier.size() < src_frontier.size() ||
            (target_frontier.size() == src_frontier.size() && from_target.size() <= from_src.size());
        visited_t &mine = from_target_side ? from_target : from_src;
        const visited_t &other = from_target_side ? from_src : from_target;
        std::vector<TNode> &frontier = from_target_side ? target_frontier : src_frontier;
        std::vector<TNode> next;
        /* The edges on which the two sides met: (source-side node, target-side
           node, edge between them in path order). */
        std::vector<std::tuple<TNode, TNode, TEdge>> meetings;
        bool over_limit = false;
        auto emit = [&](const TNode &node, const TNode &neighbor, const TEdge &edge) -> bool {
          if (over_limit) {
            return false;
          }
          ++result.EdgesScanned;
          if (other.find(neighbor) != other.end()) {
            if (from_target_side) {
              meetings.emplace_back(neighbor, node, edge);
            } else {
              meetings.emplace_back(node, neighbor, edge);
            }
          }
          if (mine.find(neighbor) == mine.end()) {
            mine.emplace(neighbor, parent_t(std::in_place, node, edge));
            next.push_back(neighbor);
            if (++result.NodesVisited > limits.MaxNodes) {
              over_limit = true;
              return false;
            }
          }
          return true;
        };
        expand(static_cast<const std::vector<TNode> &>(frontier), from_target_side, emit);
        ++(from_target_side ? target_depth : src_depth);
        if (!meetings.empty()) {
          /* A meeting through a node either side reached earlier would have
             been seen when that node was first reached, so every meeting in
             this level gives a path of the same, shortest, length. */
          for (const auto &[src_side, target_side, edge] : meetings) {
            edge_vec_t path;
            path_to_src(src_side, path);
            path.push_back(edge);
            path_to_target(target_side, path);
            result.Paths.push_back(std::move(path));
          }
          result.Status = TShortestPathStatus::Found;
          return result;
        }
        if (over_limit) {
          result.Status = TShortestPathStatus::NodeLimit;
          return result;
        }
        frontier = std::move(next);
      }
    }

  }  // Rt

}  // Orly
