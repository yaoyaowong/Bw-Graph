#ifndef TEST_H
#define TEST_H
#include "bw_graph/common/type.h"

#include <iostream>
#include <queue>
#include <set>
#include <vector>

/**
 * @brief Create test graph data based on the provided graph format
 *
 * Graph data:
 * t 13 20 (13 vertices, 20 edges)
 * Vertices: 0-12 with various properties
 * Edges form a connected graph with cycles
 *
 * @return Pair of adjacency map (with spans) and the underlying adj_map_t
 * storage
 */
inline std::pair<adj_map_iter_t, adj_map_t> create_test_graph() {
  // Step 1: Build adjacency list with vectors
  adj_map_t adjacency_map;

  // Initialize all vertices (0-12)
  for (v_id_t i = 0; i <= 12; ++i) {
    adjacency_map[i] = std::vector<v_id_t>();
  }

  // Add edges as specified in the test data
  std::vector<std::pair<v_id_t, v_id_t>> edges = {
      {0, 2}, {1, 0}, {1, 2}, {1, 3}, {2, 3}, {3, 0}, {3, 4},  {3, 11}, {4, 6},  {4, 7},
      {5, 4}, {6, 5}, {7, 3}, {7, 8}, {7, 9}, {8, 9}, {8, 10}, {10, 7}, {10, 9}, {11, 12}};

  // Build adjacency list
  for (const auto& edge : edges) {
    adjacency_map[edge.first].push_back(edge.second);
  }

  // Step 2: Convert to adj_map_iter_t (with stable storage from adj_map_t)
  adj_map_iter_t adjacency_map_spans;
  adjacency_map_spans.reserve(adjacency_map.size());

  for (auto& [vertex_id, neighbors] : adjacency_map) {
    // Create span pointing to the vector in adjacency_map
    adjacency_map_spans[vertex_id] = std::span<v_id_t>(neighbors);
  }

  // Return both the spans and the storage (adjacency_map must outlive the
  // spans!)
  return {std::move(adjacency_map_spans), std::move(adjacency_map)};
}

/**
 * @brief Create test graph data based on the provided graph format
 *
 * Graph data:
 * t 13 20 (13 vertices, 20 edges)
 * Vertices: 0-12 with various properties
 * Edges form a connected graph with cycles
 *
 * @return Map representing adjacency list of the test graph
 */
inline block_adj_map_t create_test_block_adjmap() {
  block_adj_map_t block_adjacency_map;

  // Initialize all blocks (0-12)
  for (page_no_t i = 0; i <= 12; ++i) {
    block_adjacency_map[i] = std::vector<page_no_t>();
  }

  // Add edges as specified in the test data
  std::vector<std::pair<page_no_t, page_no_t>> edges = {
      {0, 2}, {1, 0}, {1, 2}, {1, 3}, {2, 3}, {3, 0}, {3, 4},  {3, 11}, {4, 6},  {4, 7},
      {5, 4}, {6, 5}, {7, 3}, {7, 8}, {7, 9}, {8, 9}, {8, 10}, {10, 7}, {10, 9}, {11, 12}};

  // Build adjacency list
  for (const auto& edge : edges) {
    block_adjacency_map[edge.first].push_back(edge.second);
  }

  return block_adjacency_map;
}

/**
 * @brief Create a simple test graph for basic functionality testing
 *
 * Simple 4-vertex graph: 0->1->2->3, 0->3
 *
 * @return Map representing simple adjacency list
 */
inline adj_map_t create_simple_graph() {
  adj_map_t adjacency_map;

  adjacency_map[0] = {1, 3};
  adjacency_map[1] = {2};
  adjacency_map[2] = {3};
  adjacency_map[3] = {};

  return adjacency_map;
}

// Helper function to check if graph is connected
inline bool is_connected(const block_adj_map_t& graph) {
  if (graph.empty())
    return true;

  std::set<page_no_t> visited;
  std::queue<page_no_t> queue;

  // Start BFS from first node
  auto start_node = graph.begin()->first;
  queue.push(start_node);
  visited.insert(start_node);

  while (!queue.empty()) {
    page_no_t current = queue.front();
    queue.pop();

    auto it = graph.find(current);
    if (it != graph.end()) {
      for (page_no_t neighbor : it->second) {
        if (visited.find(neighbor) == visited.end()) {
          visited.insert(neighbor);
          queue.push(neighbor);
        }
      }
    }
  }

  return visited.size() == graph.size();
}

// Helper function to check degree constraints
inline bool check_degree_constraints(const block_adj_map_t& graph, uint64_t max_degree) {
  for (const auto& [node, neighbors] : graph) {
    if (neighbors.size() > max_degree) {
      std::cout << "Node " << node << " has degree " << neighbors.size()
                << " which exceeds max_degree " << max_degree << std::endl;
      return false;
    }
  }
  return true;
}

// Helper function to validate graph structure (bidirectional edges)
inline bool validate_graph_structure(const block_adj_map_t& graph) {
  for (const auto& [u, neighbors] : graph) {
    for (page_no_t v : neighbors) {
      auto it = graph.find(v);
      if (it == graph.end()) {
        std::cout << "Node " << v << " not found in graph but referenced by " << u << std::endl;
        return false;
      }

      bool found_reverse = false;
      for (page_no_t w : it->second) {
        if (w == u) {
          found_reverse = true;
          break;
        }
      }

      if (!found_reverse) {
        std::cout << "Edge (" << u << "," << v << ") exists but reverse edge not found"
                  << std::endl;
        return false;
      }
    }
  }
  return true;
}

inline void print_connectivity_info(const block_adj_map_t& original,
                                    const block_adj_map_t& sparsified) {
  bool original_connected = is_connected(original);
  bool sparsified_connected = is_connected(sparsified);

  std::cout << "Original graph connected: " << (original_connected ? "YES" : "NO") << std::endl;
  std::cout << "Sparsified graph connected: " << (sparsified_connected ? "YES" : "NO") << std::endl;

  size_t original_edges = 0;
  size_t sparsified_edges = 0;

  for (const auto& [_, neighbors] : original) {
    original_edges += neighbors.size();
  }
  for (const auto& [_, neighbors] : sparsified) {
    sparsified_edges += neighbors.size();
  }

  // Each edge is counted twice (once for each endpoint)
  original_edges /= 2;
  sparsified_edges /= 2;

  std::cout << "Original edges: " << original_edges << ", Sparsified edges: " << sparsified_edges
            << std::endl;
  std::cout << "Edge reduction: "
            << (original_edges > 0
                    ? (double) (original_edges - sparsified_edges) / original_edges * 100
                    : 0)
            << "%" << std::endl;
}

#endif // TEST_H