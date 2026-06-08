#include "bw_graph/mem/mem_g0csr.h"
#include "test.h"

#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

// mem_sub_csr_t constructor cases.
TEST(MemSubCSRConstructorTest, DefaultConstructorCreatesEmptyCSR) {
  mem_sub_csr_t csr;

  EXPECT_EQ(csr.vertex_count(), 0);
  EXPECT_EQ(csr.edge_count(), 0);
  EXPECT_TRUE(csr.vertex_list().empty());
}

// Test MemSubCSRConstructorTest.constructor from adjacency map builds correct csr.
TEST(MemSubCSRConstructorTest, ConstructorFromAdjacencyMapBuildsCorrectCSR) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  EXPECT_EQ(csr.vertex_count(), 13);
  EXPECT_EQ(csr.edge_count(), 20); // Total outgoing edges

  // Verify all vertices exist
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(csr.has_vertex(i));
  }
}

// mem_sub_csr_t Neighbor Access Tests
TEST(MemSubCSRNeighborAccessTest, GetNeighborsReturnsCorrectNeighborList) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  // Test vertex 0 neighbors (should be {2} based on edge e 0 2)
  auto neighbors_0 = csr.get_neighbors(0);
  EXPECT_EQ(neighbors_0.size(), 1);
  EXPECT_EQ(neighbors_0[0], 2);

  // Test vertex 1 neighbors (should be {0, 2, 3} based on edges e 1 0, e 1 2, e
  // 1 3)
  auto neighbors_1 = csr.get_neighbors(1);
  EXPECT_EQ(neighbors_1.size(), 3);
  std::unordered_set<v_id_t> expected_1 = {0, 2, 3};
  for (const auto& neighbor : neighbors_1) {
    EXPECT_EQ(expected_1.count(neighbor), 1);
  }

  // Test vertex 5 neighbors (should be {4} based on edge e 5 4)
  auto neighbors_5 = csr.get_neighbors(5);
  EXPECT_EQ(neighbors_5.size(), 1);
  EXPECT_EQ(neighbors_5[0], 4);

  // Test vertex 9 neighbors (should be empty - no outgoing edges)
  auto neighbors_9 = csr.get_neighbors(9);
  EXPECT_TRUE(neighbors_9.empty());
}

// Test MemSubCSRNeighborAccessTest.get neighbors for non existent vertex returns empty vector.
TEST(MemSubCSRNeighborAccessTest, GetNeighborsForNonExistentVertexReturnsEmptyVector) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  auto neighbors = csr.get_neighbors(999);
  EXPECT_TRUE(neighbors.empty());
}

// mem_sub_csr_t Vertex Existence Tests
TEST(MemSubCSRVertexExistenceTest, HasVertexReturnsCorrectValues) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  // Test existing vertices
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(csr.has_vertex(i));
  }

  // Test non-existing vertices
  EXPECT_FALSE(csr.has_vertex(13));
  EXPECT_FALSE(csr.has_vertex(999));
  EXPECT_FALSE(csr.has_vertex(100));
}

// Test MemSubCSRVertexExistenceTest.vertex list returns all vertices.
TEST(MemSubCSRVertexExistenceTest, VertexListReturnsAllVertices) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  auto vertices = csr.vertex_list();
  EXPECT_EQ(vertices.size(), 13);

  // Convert to set for easy checking
  std::unordered_set<v_id_t> vertex_set(vertices.begin(), vertices.end());

  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_EQ(vertex_set.count(i), 1);
  }
}

// mem_sub_csr_t Edge Existence Tests
TEST(MemSubCSREdgeExistenceTest, HasEdgeReturnsCorrectValuesForExistingEdges) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  // Test some existing edges
  EXPECT_TRUE(csr.has_edge(0, 2));
  EXPECT_TRUE(csr.has_edge(1, 0));
  EXPECT_TRUE(csr.has_edge(1, 2));
  EXPECT_TRUE(csr.has_edge(1, 3));
  EXPECT_TRUE(csr.has_edge(3, 4));
  EXPECT_TRUE(csr.has_edge(11, 12));
}

// Test MemSubCSREdgeExistenceTest.has edge returns false for non existing edges.
TEST(MemSubCSREdgeExistenceTest, HasEdgeReturnsFalseForNonExistingEdges) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  // Test non-existing edges
  EXPECT_FALSE(csr.has_edge(0, 1));   // Reverse direction
  EXPECT_FALSE(csr.has_edge(2, 0));   // Reverse direction
  EXPECT_FALSE(csr.has_edge(9, 10));  // Non-existing edge
  EXPECT_FALSE(csr.has_edge(12, 11)); // Reverse direction
}

// Test MemSubCSREdgeExistenceTest.has edge returns false for non existent vertices.
TEST(MemSubCSREdgeExistenceTest, HasEdgeReturnsFalseForNonExistentVertices) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  EXPECT_FALSE(csr.has_edge(999, 0));
  EXPECT_FALSE(csr.has_edge(0, 999));
  EXPECT_FALSE(csr.has_edge(999, 1000));
}

// mem_sub_csr_t to_map Conversion Tests
TEST(MemSubCSRToMapConversionTest, ToMapReturnsCorrectAdjacencyRepresentation) {
  auto [graph_view, original_data] = create_test_graph();
  mem_sub_csr_t csr(original_data);

  auto reconstructed_map = csr.to_map();

  EXPECT_EQ(reconstructed_map.size(), original_data.size());

  // Verify each vertex's neighbors match
  for (const auto& pair : original_data) {
    v_id_t vertex_id = pair.first;
    const auto& original_neighbors = pair.second;

    EXPECT_EQ(reconstructed_map.count(vertex_id), 1);
    auto reconstructed_neighbors = reconstructed_map[vertex_id];

    EXPECT_EQ(original_neighbors.size(), reconstructed_neighbors.size());

    // Convert to sets for order-independent comparison
    std::unordered_set<v_id_t> original_set(original_neighbors.begin(), original_neighbors.end());
    std::unordered_set<v_id_t> reconstructed_set(reconstructed_neighbors.begin(),
                                                 reconstructed_neighbors.end());

    EXPECT_EQ(original_set, reconstructed_set);
  }
}

// Test MemSubCSRToMapConversionTest.to map works for empty graph.
TEST(MemSubCSRToMapConversionTest, ToMapWorksForEmptyGraph) {
  mem_sub_csr_t csr;
  auto adj_map = csr.to_map();
  EXPECT_TRUE(adj_map.empty());
}

// mem_sub_csr_t Count Tests
TEST(MemSubCSRCountTest, VertexCountAndEdgeCountReturnCorrectValues) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_csr_t csr(graph_data);

  EXPECT_EQ(csr.vertex_count(), 13);
  EXPECT_EQ(csr.edge_count(), 20);
}

// Test MemSubCSRCountTest.simple graph has correct counts.
TEST(MemSubCSRCountTest, SimpleGraphHasCorrectCounts) {
  auto simple_data = create_simple_graph();
  mem_sub_csr_t csr(simple_data);

  EXPECT_EQ(csr.vertex_count(), 4);
  EXPECT_EQ(csr.edge_count(), 4);
}

// Test MemSubCSRCountTest.empty graph has zero counts.
TEST(MemSubCSRCountTest, EmptyGraphHasZeroCounts) {
  mem_sub_csr_t csr;

  EXPECT_EQ(csr.vertex_count(), 0);
  EXPECT_EQ(csr.edge_count(), 0);
}

// mem_sub_csr_t Simple Graph Tests
TEST(MemSubCSRSimpleGraphTest, SimpleGraphStructureIsCorrect) {
  auto graph_data = create_simple_graph();
  mem_sub_csr_t csr(graph_data);

  // Verify vertex 0 neighbors
  auto neighbors_0 = csr.get_neighbors(0);
  EXPECT_EQ(neighbors_0.size(), 2);
  std::unordered_set<v_id_t> expected_0 = {1, 3};
  std::unordered_set<v_id_t> actual_0(neighbors_0.begin(), neighbors_0.end());
  EXPECT_EQ(expected_0, actual_0);

  // Verify vertex 1 neighbors
  auto neighbors_1 = csr.get_neighbors(1);
  EXPECT_EQ(neighbors_1.size(), 1);
  EXPECT_EQ(neighbors_1[0], 2);

  // Verify vertex 2 neighbors
  auto neighbors_2 = csr.get_neighbors(2);
  EXPECT_EQ(neighbors_2.size(), 1);
  EXPECT_EQ(neighbors_2[0], 3);

  // Verify vertex 3 neighbors (should be empty)
  auto neighbors_3 = csr.get_neighbors(3);
  EXPECT_TRUE(neighbors_3.empty());
}

// mem_sub_csr_t edge cases.
TEST(MemSubCSREdgeCasesTest, SingleVertexGraph) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> single_vertex;
  single_vertex[42] = {};

  mem_sub_csr_t csr(single_vertex);

  EXPECT_EQ(csr.vertex_count(), 1);
  EXPECT_EQ(csr.edge_count(), 0);
  EXPECT_TRUE(csr.has_vertex(42));
  EXPECT_TRUE(csr.get_neighbors(42).empty());
}

// Test MemSubCSREdgeCasesTest.self loop graph.
TEST(MemSubCSREdgeCasesTest, SelfLoopGraph) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> self_loop;
  self_loop[0] = {0};

  mem_sub_csr_t csr(self_loop);

  EXPECT_EQ(csr.vertex_count(), 1);
  EXPECT_EQ(csr.edge_count(), 1);
  EXPECT_TRUE(csr.has_vertex(0));
  EXPECT_TRUE(csr.has_edge(0, 0));

  auto neighbors = csr.get_neighbors(0);
  EXPECT_EQ(neighbors.size(), 1);
  EXPECT_EQ(neighbors[0], 0);
}

// Test MemSubCSREdgeCasesTest.complete graph on3 vertices.
TEST(MemSubCSREdgeCasesTest, CompleteGraphOn3Vertices) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> complete_3;
  complete_3[0] = {1, 2};
  complete_3[1] = {0, 2};
  complete_3[2] = {0, 1};

  mem_sub_csr_t csr(complete_3);

  EXPECT_EQ(csr.vertex_count(), 3);
  EXPECT_EQ(csr.edge_count(), 6);

  // Each vertex should have 2 neighbors
  for (v_id_t i = 0; i < 3; ++i) {
    auto neighbors = csr.get_neighbors(i);
    EXPECT_EQ(neighbors.size(), 2);
  }

  // All edges should exist
  for (v_id_t i = 0; i < 3; ++i) {
    for (v_id_t j = 0; j < 3; ++j) {
      if (i != j) {
        EXPECT_TRUE(csr.has_edge(i, j));
      }
    }
  }
}