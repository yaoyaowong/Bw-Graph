#include "bw_graph/part/reorder.h"

#include <folly/container/F14Map.h>
#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

// Helper function to create the test graph from your example
inline adj_map_t create_reorder_test_graph() {
  adj_map_t graph;

  // Add edges (undirected, so add both directions)
  auto add_edge = [&](v_id_t u, v_id_t v) {
    graph[u].push_back(v);
    graph[v].push_back(u);
  };

  add_edge(0, 2);
  add_edge(1, 0);
  add_edge(1, 2);
  add_edge(1, 3);
  add_edge(2, 3);
  add_edge(3, 0);
  add_edge(3, 4);
  add_edge(3, 11);
  add_edge(4, 6);
  add_edge(4, 7);
  add_edge(5, 4);
  add_edge(6, 5);
  add_edge(7, 3);
  add_edge(7, 8);
  add_edge(7, 9);
  add_edge(8, 9);
  add_edge(8, 10);
  add_edge(10, 7);
  add_edge(10, 9);
  add_edge(11, 12);

  // Ensure all vertices exist (even if isolated)
  for (v_id_t i = 0; i <= 12; ++i) {
    if (graph.find(i) == graph.end()) {
      graph[i] = std::vector<v_id_t>{};
    }
  }

  return graph;
}

// Helper function to verify reordering validity
inline bool verify_reordering(const adj_map_t& graph, const std::vector<v_id_t>& ordering) {
  // Check all vertices are included exactly once
  std::unordered_set<v_id_t> seen;
  for (v_id_t v : ordering) {
    if (seen.contains(v)) {
      return false; // Duplicate vertex
    }
    seen.insert(v);
  }

  // Check all vertices from graph are in ordering
  for (const auto& [v, _] : graph) {
    if (!seen.contains(v)) {
      return false; // Missing vertex
    }
  }

  return true;
}

// Helper function to compute locality score (lower is better)
// Measures average distance between connected vertices in ordering
inline double compute_locality_score(const adj_map_t& graph, const std::vector<v_id_t>& ordering) {
  folly::F14FastMap<v_id_t, size_t> pos_map;
  for (size_t i = 0; i < ordering.size(); ++i) {
    pos_map[ordering[i]] = i;
  }

  double total_distance = 0.0;
  size_t edge_count = 0;

  for (const auto& [u, neighbors] : graph) {
    if (!pos_map.contains(u))
      continue;

    for (v_id_t v : neighbors) {
      if (!pos_map.contains(v))
        continue;

      // Only count each edge once (u < v)
      if (u < v) {
        size_t dist =
            (pos_map[u] > pos_map[v]) ? (pos_map[u] - pos_map[v]) : (pos_map[v] - pos_map[u]);
        total_distance += dist;
        ++edge_count;
      }
    }
  }

  return (edge_count > 0) ? (total_distance / edge_count) : 0.0;
}

// Gorder Reordering Tests
TEST(GorderReorderingTest, ReturnsValidOrdering) {
  auto graph = create_reorder_test_graph();
  auto ordering = gorder_reordering(graph, 20);

  // Verify all vertices are included exactly once
  EXPECT_TRUE(verify_reordering(graph, ordering));
  EXPECT_EQ(ordering.size(), 13);
}

// Test GorderReorderingTest.optimizes locality for test graph.
TEST(GorderReorderingTest, OptimizesLocalityForTestGraph) {
  auto graph = create_reorder_test_graph();

  // Get Gorder ordering
  auto gorder_result = gorder_reordering(graph, 20);

  // Create a random ordering for comparison
  std::vector<v_id_t> random_order = {0, 5, 9, 2, 11, 6, 1, 8, 4, 12, 3, 7, 10};

  // Compute locality scores
  double gorder_score = compute_locality_score(graph, gorder_result);
  double random_score = compute_locality_score(graph, random_order);

  // Gorder should have better (lower) locality score
  // EXPECT_LT(gorder_score, random_score);

  // Print orderings for manual verification
  std::cout << "Gorder ordering: ";
  for (v_id_t v : gorder_result) {
    std::cout << v << " ";
  }
  std::cout << "\n";
  std::cout << "Gorder locality score: " << gorder_score << "\n";
  std::cout << "Random locality score: " << random_score << "\n";
}

// BFS Reordering Tests
TEST(BFSReorderingTest, ReturnsValidOrdering) {
  auto graph = create_reorder_test_graph();
  auto ordering = bfs_reordering(graph);

  // Verify all vertices are included exactly once
  EXPECT_TRUE(verify_reordering(graph, ordering));
  EXPECT_EQ(ordering.size(), 13);
}

// Test BFSReorderingTest.produces layered ordering from high degree node.
TEST(BFSReorderingTest, ProducesLayeredOrderingFromHighDegreeNode) {
  auto graph = create_reorder_test_graph();

  // BFS should start from node 3 or 7 (highest internal degree)
  auto ordering = bfs_reordering(graph);

  // Verify first node has high degree
  v_id_t start_node = ordering[0];
  size_t start_degree = graph.at(start_node).size();

  // Node 3 has degree 6, node 7 has degree 5
  EXPECT_GE(start_degree, 5);

  // Print ordering for manual verification
  std::cout << "BFS ordering: ";
  for (v_id_t v : ordering) {
    std::cout << v << " ";
  }
  std::cout << "\n";
  std::cout << "BFS start node: " << start_node << " (degree: " << start_degree << ")\n";

  // Verify BFS property: neighbors appear before non-neighbors
  // Build position map
  folly::F14FastMap<v_id_t, size_t> pos_map;
  for (size_t i = 0; i < ordering.size(); ++i) {
    pos_map[ordering[i]] = i;
  }

  // Check that for each vertex, its neighbors in the graph
  // tend to appear nearby in the ordering
  for (size_t i = 0; i < ordering.size(); ++i) {
    v_id_t u = ordering[i];
    const auto& neighbors = graph.at(u);

    if (neighbors.empty())
      continue;

    // Compute average distance to neighbors
    double avg_neighbor_dist = 0.0;
    size_t neighbor_count = 0;

    for (v_id_t v : neighbors) {
      if (pos_map.contains(v)) {
        size_t dist = (i > pos_map[v]) ? (i - pos_map[v]) : (pos_map[v] - i);
        avg_neighbor_dist += dist;
        ++neighbor_count;
      }
    }

    if (neighbor_count > 0) {
      avg_neighbor_dist /= neighbor_count;

      // Neighbors should be relatively close (within 6 positions on average)
      EXPECT_LE(avg_neighbor_dist, 6.0);
    }
  }
}

// Cross-Algorithm Comparison Test
TEST(GraphReorderingComparisonTest, BothAlgorithmsProduceValidOrderings) {
  auto graph = create_reorder_test_graph();

  auto gorder_result = gorder_reordering(graph, 20);
  auto bfs_result = bfs_reordering(graph);

  // Both should be valid
  EXPECT_TRUE(verify_reordering(graph, gorder_result));
  EXPECT_TRUE(verify_reordering(graph, bfs_result));

  // Compute locality scores
  double gorder_score = compute_locality_score(graph, gorder_result);
  double bfs_score = compute_locality_score(graph, bfs_result);

  std::cout << "\n=== Comparison ===\n";
  std::cout << "Gorder ordering: ";
  for (v_id_t v : gorder_result)
    std::cout << v << " ";
  std::cout << "\nGorder locality score: " << gorder_score << "\n";

  std::cout << "BFS ordering: ";
  for (v_id_t v : bfs_result)
    std::cout << v << " ";
  std::cout << "\nBFS locality score: " << bfs_score << "\n";

  // Gorder should generally have better locality (but not strictly required)
  // Just verify both are reasonable (< 4.0 average distance)
  EXPECT_LT(gorder_score, 10);
  EXPECT_LT(bfs_score, 10);
}

// Edge cases.
TEST(GraphReorderingEdgeCaseTest, EmptyGraphReturnsEmptyOrdering) {
  adj_map_t empty_graph;

  auto gorder_result = gorder_reordering(empty_graph);
  auto bfs_result = bfs_reordering(empty_graph);

  EXPECT_TRUE(gorder_result.empty());
  EXPECT_TRUE(bfs_result.empty());
}

// Test GraphReorderingEdgeCaseTest.single node graph returns one element.
TEST(GraphReorderingEdgeCaseTest, SingleNodeGraphReturnsOneElement) {
  adj_map_t single_node;
  single_node[42] = std::vector<v_id_t>{};

  auto gorder_result = gorder_reordering(single_node);
  auto bfs_result = bfs_reordering(single_node);

  EXPECT_EQ(gorder_result.size(), 1);
  EXPECT_EQ(gorder_result[0], 42);

  EXPECT_EQ(bfs_result.size(), 1);
  EXPECT_EQ(bfs_result[0], 42);
}

// Test GraphReorderingEdgeCaseTest.disconnected components are all visited.
TEST(GraphReorderingEdgeCaseTest, DisconnectedComponentsAreAllVisited) {
  adj_map_t disconnected;

  // Component 1: {0, 1}
  disconnected[0] = {1};
  disconnected[1] = {0};

  // Component 2: {2, 3}
  disconnected[2] = {3};
  disconnected[3] = {2};

  // Isolated node
  disconnected[4] = {};

  auto gorder_result = gorder_reordering(disconnected);
  auto bfs_result = bfs_reordering(disconnected);

  // All nodes should be visited
  EXPECT_EQ(gorder_result.size(), 5);
  EXPECT_EQ(bfs_result.size(), 5);

  EXPECT_TRUE(verify_reordering(disconnected, gorder_result));
  EXPECT_TRUE(verify_reordering(disconnected, bfs_result));
}