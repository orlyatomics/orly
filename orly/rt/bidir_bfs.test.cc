/* <orly/rt/bidir_bfs.test.cc>

   Unit test for <orly/rt/bidir_bfs.h>.

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

#include <orly/rt/bidir_bfs.h>

#include <unordered_map>
#include <utility>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Rt;

using edge_t = pair<int, int>;
using path_t = vector<edge_t>;

/* An undirected graph held as adjacency lists, standing in for an index of
   edge keys and their twins. */
class TGraph {
  public:

  void Link(int a, int b) {
    Adj[a].push_back(b);
    Adj[b].push_back(a);
  }

  TShortestPathResult<edge_t> Search(int src, int target, const TShortestPathLimits &limits = TShortestPathLimits()) const {
    return BidirBfs<int, edge_t>(src, target, limits, [this](const vector<int> &frontier, bool from_target, auto &emit) {
      for (int node : frontier) {
        auto iter = Adj.find(node);
        if (iter == Adj.end()) {
          continue;
        }
        for (int neighbor : iter->second) {
          if (!emit(node, neighbor, from_target ? edge_t(neighbor, node) : edge_t(node, neighbor))) {
            return;
          }
        }
      }
    });
  }

  private:

  unordered_map<int, vector<int>> Adj;

};  // TGraph

/* True iff `path` is a chain of edges from `src` to `target`. */
static bool IsPath(const path_t &path, int src, int target) {
  int at = src;
  for (const auto &edge : path) {
    if (edge.first != at) {
      return false;
    }
    at = edge.second;
  }
  return at == target;
}

/* A path of `len` edges, 0 - 1 - ... - len. */
static TGraph Chain(int len) {
  TGraph g;
  for (int i = 0; i < len; ++i) {
    g.Link(i, i + 1);
  }
  return g;
}

/* A hub, 0, with `spokes` spokes, each with `leaves` leaves of its own. Node
   ids start at `base`. */
static void AddHub(TGraph &g, int base, int spokes, int leaves) {
  int next = base + 1;
  for (int s = 0; s < spokes; ++s) {
    const int spoke = next++;
    g.Link(base, spoke);
    for (int l = 0; l < leaves; ++l) {
      g.Link(spoke, next++);
    }
  }
}

FIXTURE(SameNode) {
  const auto res = Chain(3).Search(2, 2);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths.size(), 1u);
  EXPECT_TRUE(res.Paths[0].empty());
}

FIXTURE(DirectEdge) {
  const auto res = Chain(1).Search(0, 1);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths.size(), 1u);
  EXPECT_TRUE(res.Paths[0] == path_t({{0, 1}}));
}

FIXTURE(TwoHops) {
  const auto res = Chain(2).Search(0, 2);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths.size(), 1u);
  EXPECT_TRUE(res.Paths[0] == path_t({{0, 1}, {1, 2}}));
}

FIXTURE(LongChainBothDirections) {
  const auto g = Chain(12);
  for (auto [src, target] : {pair(0, 12), pair(12, 0), pair(3, 10)}) {
    const auto res = g.Search(src, target);
    EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
    EXPECT_EQ(res.Paths.size(), 1u);
    EXPECT_EQ(res.Paths[0].size(), static_cast<size_t>(abs(target - src)));
    EXPECT_TRUE(IsPath(res.Paths[0], src, target));
  }
}

FIXTURE(ShortestOfSeveral) {
  /* 1 - 2 - 3 - 9 and 1 - 4 - 5 - 6 - 7 - 9: only the 3-edge path. */
  TGraph g;
  g.Link(1, 2); g.Link(2, 3); g.Link(3, 9);
  g.Link(1, 4); g.Link(4, 5); g.Link(5, 6); g.Link(6, 7); g.Link(7, 9);
  const auto res = g.Search(1, 9);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths.size(), 1u);
  EXPECT_TRUE(res.Paths[0] == path_t({{1, 2}, {2, 3}, {3, 9}}));
}

FIXTURE(TiedShortestPaths) {
  /* A square: 1 - 2 - 4 and 1 - 3 - 4 are both shortest. */
  TGraph g;
  g.Link(1, 2); g.Link(1, 3); g.Link(2, 4); g.Link(3, 4);
  const auto res = g.Search(1, 4);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths.size(), 2u);
  for (const auto &path : res.Paths) {
    EXPECT_EQ(path.size(), 2u);
    EXPECT_TRUE(IsPath(path, 1, 4));
  }
  EXPECT_TRUE(res.Paths[0] != res.Paths[1]);
}

FIXTURE(PathThroughHub) {
  /* Leaf to leaf through two spokes and the hub: 4 edges. */
  TGraph g;
  AddHub(g, 0, 100, 20);
  const int leaf_a = 2, leaf_b = 2 + 21 * 99;  // first leaf of the first and last spoke
  const auto res = g.Search(leaf_a, leaf_b);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths.size(), 1u);
  EXPECT_EQ(res.Paths[0].size(), 4u);
  EXPECT_TRUE(IsPath(res.Paths[0], leaf_a, leaf_b));
}

/* The case in #695: a query from a hub to a target outside its component.
   The old search expanded only the source side past depth 2, so it read
   every edge of the hub's component (here 1,000 spokes with 20 leaves each:
   21,000 edges, 42,000 edge reads counting both ends) before giving up. */
FIXTURE(HubToDisconnectedTarget) {
  TGraph g;
  AddHub(g, 0, 1000, 20);
  /* An isolated target: its side is empty at once. */
  auto res = g.Search(0, 1000000);
  EXPECT_TRUE(res.Status == TShortestPathStatus::NoPath);
  EXPECT_TRUE(res.Paths.empty());
  EXPECT_EQ(res.EdgesScanned, 0u);
  /* A target in a small component of its own: that side runs out first. */
  g.Link(1000000, 1000001);
  g.Link(1000001, 1000002);
  g.Link(1000002, 1000000);
  /* From the hub, its own 1,000 edges are read once (its frontier of one is
     the smaller), and then the target's side runs out. From a spoke or a
     leaf, the hub is never reached. */
  for (auto [src, max_edges] : {pair(0, 1010), pair(1, 50), pair(2, 50)}) {
    res = g.Search(src, 1000000);
    EXPECT_TRUE(res.Status == TShortestPathStatus::NoPath);
    EXPECT_LE(res.EdgesScanned, static_cast<size_t>(max_edges));
  }
}

FIXTURE(NodeLimit) {
  /* Two hubs, disconnected: neither side runs out, so the node limit ends it. */
  TGraph g;
  AddHub(g, 0, 1000, 20);
  AddHub(g, 1000000, 1000, 20);
  TShortestPathLimits limits;
  limits.MaxNodes = 500;
  const auto res = g.Search(2, 1000002, limits);
  EXPECT_TRUE(res.Status == TShortestPathStatus::NodeLimit);
  EXPECT_TRUE(res.Paths.empty());
  EXPECT_EQ(res.NodesVisited, 501u);
  EXPECT_LE(res.EdgesScanned, 1000u);
  /* The default limit holds too, with the same graph grown past it. */
  AddHub(g, 2000000, 3000, 20);
  AddHub(g, 3000000, 3000, 20);
  const auto res2 = g.Search(2000000, 3000000);
  EXPECT_TRUE(res2.Status == TShortestPathStatus::NodeLimit);
  EXPECT_EQ(res2.NodesVisited, TShortestPathLimits::DefaultMaxNodes + 1);
}

FIXTURE(DepthLimit) {
  const auto g = Chain(20);
  TShortestPathLimits limits;
  limits.MaxDepth = 19;
  auto res = g.Search(0, 20, limits);
  EXPECT_TRUE(res.Status == TShortestPathStatus::DepthLimit);
  EXPECT_TRUE(res.Paths.empty());
  limits.MaxDepth = 20;
  res = g.Search(0, 20, limits);
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
  EXPECT_EQ(res.Paths[0].size(), 20u);
  /* The default depth limit. */
  const auto long_chain = Chain(40);
  res = long_chain.Search(0, 40);
  EXPECT_TRUE(res.Status == TShortestPathStatus::DepthLimit);
  res = long_chain.Search(0, static_cast<int>(TShortestPathLimits::DefaultMaxDepth));
  EXPECT_TRUE(res.Status == TShortestPathStatus::Found);
}
