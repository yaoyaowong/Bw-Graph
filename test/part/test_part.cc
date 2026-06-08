#include "bw_graph/common/type.h"
#include "bw_graph/part/partition.h"
#include "test.h"

#include <folly/container/F14Map.h>
#include <gtest/gtest.h>
#include <iostream>

class GraphSparsificationTest : public ::testing::Test {
protected:
};

// Test GraphSparsificationTest.empty graph.
TEST_F(GraphSparsificationTest, EmptyGraph) {
  block_adj_map_t empty_graph;
  auto result = graph_sparsification(empty_graph, 3);

  EXPECT_TRUE(result.empty());
  EXPECT_TRUE(check_degree_constraints(result, 3));
}

// Test GraphSparsificationTest.single node.
TEST_F(GraphSparsificationTest, SingleNode) {
  block_adj_map_t graph;
  graph[1] = {};

  auto result = graph_sparsification(graph, 3);

  EXPECT_EQ(result.size(), 1);
  EXPECT_TRUE(result[1].empty());
  EXPECT_TRUE(check_degree_constraints(result, 3));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.two nodes.
TEST_F(GraphSparsificationTest, TwoNodes) {
  block_adj_map_t graph;
  graph[1] = {2};
  graph[2] = {1};

  auto result = graph_sparsification(graph, 1);

  EXPECT_TRUE(check_degree_constraints(result, 1));
  EXPECT_TRUE(validate_graph_structure(result));
  EXPECT_TRUE(is_connected(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.linear chain.
TEST_F(GraphSparsificationTest, LinearChain) {
  block_adj_map_t graph;
  // Create linear chain: 1-2-3-4-5
  for (page_no_t i = 1; i <= 5; ++i) {
    graph[i] = {};
    if (i > 1) {
      graph[i].push_back(i - 1);
      graph[i - 1].push_back(i);
    }
  }

  auto result = graph_sparsification(graph, 2);

  EXPECT_TRUE(check_degree_constraints(result, 2));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.complete graph.
TEST_F(GraphSparsificationTest, CompleteGraph) {
  block_adj_map_t graph;
  // Create complete graph with 6 nodes
  for (page_no_t i = 1; i <= 6; ++i) {
    graph[i] = {};
    for (page_no_t j = 1; j <= 6; ++j) {
      if (i != j) {
        graph[i].push_back(j);
      }
    }
  }

  auto result = graph_sparsification(graph, 3);

  EXPECT_TRUE(check_degree_constraints(result, 3));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.star graph.
TEST_F(GraphSparsificationTest, StarGraph) {
  block_adj_map_t graph;
  // Create star graph: center node 1 connected to nodes 2,3,4,5,6
  graph[1] = {2, 3, 4, 5, 6};
  for (page_no_t i = 2; i <= 6; ++i) {
    graph[i] = {1};
  }

  auto result = graph_sparsification(graph, 3);

  EXPECT_TRUE(check_degree_constraints(result, 3));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.disconnected graph.
TEST_F(GraphSparsificationTest, DisconnectedGraph) {
  block_adj_map_t graph;
  // Two disconnected components: {1,2,3} and {4,5,6}
  graph[1] = {2, 3};
  graph[2] = {1, 3};
  graph[3] = {1, 2};
  graph[4] = {5, 6};
  graph[5] = {4, 6};
  graph[6] = {4, 5};

  auto result = graph_sparsification(graph, 2);

  EXPECT_TRUE(check_degree_constraints(result, 2));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.strict degree constraint.
TEST_F(GraphSparsificationTest, StrictDegreeConstraint) {
  block_adj_map_t graph;
  // Create a graph where strict constraint forces significant sparsification
  for (page_no_t i = 1; i <= 8; ++i) {
    graph[i] = {};
    for (page_no_t j = 1; j <= 8; ++j) {
      if (i != j) {
        graph[i].push_back(j);
      }
    }
  }

  auto result = graph_sparsification(graph, 1);

  EXPECT_TRUE(check_degree_constraints(result, 1));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Test GraphSparsificationTest.zero max degree.
TEST_F(GraphSparsificationTest, ZeroMaxDegree) {
  block_adj_map_t graph;
  graph[1] = {2};
  graph[2] = {1};

  auto result = graph_sparsification(graph, 0);

  EXPECT_TRUE(result.empty());
  EXPECT_TRUE(check_degree_constraints(result, 0));
}

// Test GraphSparsificationTest.large graph.
TEST_F(GraphSparsificationTest, LargeGraph) {
  block_adj_map_t graph;
  // Create a grid-like structure
  const page_no_t grid_size = 10;

  for (page_no_t i = 0; i < grid_size; ++i) {
    for (page_no_t j = 0; j < grid_size; ++j) {
      page_no_t node_id = i * grid_size + j + 1;
      graph[node_id] = {};

      // Connect to right neighbor
      if (j < grid_size - 1) {
        page_no_t right = i * grid_size + (j + 1) + 1;
        graph[node_id].push_back(right);
        if (graph.find(right) == graph.end())
          graph[right] = {};
        graph[right].push_back(node_id);
      }

      // Connect to bottom neighbor
      if (i < grid_size - 1) {
        page_no_t bottom = (i + 1) * grid_size + j + 1;
        graph[node_id].push_back(bottom);
        if (graph.find(bottom) == graph.end())
          graph[bottom] = {};
        graph[bottom].push_back(node_id);
      }
    }
  }

  auto result = graph_sparsification(graph, 3);

  EXPECT_TRUE(check_degree_constraints(result, 3));
  EXPECT_TRUE(validate_graph_structure(result));

  print_connectivity_info(graph, result);
}

// Performance case.
TEST_F(GraphSparsificationTest, StressDegreeConstraints) {
  // Test with various max_degree values to ensure constraints are always
  // respected
  block_adj_map_t graph;

  // Create a highly connected graph
  for (page_no_t i = 1; i <= 20; ++i) {
    graph[i] = {};
    for (page_no_t j = 1; j <= 20; ++j) {
      if (i != j && (i + j) % 3 == 0) { // Add some pattern to connections
        graph[i].push_back(j);
      }
    }
  }

  for (uint64_t max_deg = 1; max_deg <= 10; ++max_deg) {
    auto result = graph_sparsification(graph, max_deg);

    EXPECT_TRUE(check_degree_constraints(result, max_deg))
        << "Degree constraint violated for max_degree = " << max_deg;
    EXPECT_TRUE(validate_graph_structure(result))
        << "Graph structure invalid for max_degree = " << max_deg;

    std::cout << "Max degree " << max_deg << ": ";
    print_connectivity_info(graph, result);
    std::cout << std::endl;
  }
}