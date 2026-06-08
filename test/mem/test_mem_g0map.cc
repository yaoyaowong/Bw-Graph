#include "bw_graph/mem/mem_g0map.h"
#include "test.h"

#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

// mem_sub_map_t constructor cases.
TEST(MemSubMapConstructorTest, DefaultConstructorCreatesEmptyMap) {
  mem_sub_map_t map_graph;

  EXPECT_EQ(map_graph.vertex_count(), 0);
  EXPECT_EQ(map_graph.edge_count(), 0);
  EXPECT_TRUE(map_graph.vertex_list().empty());
}

// Test MemSubMapConstructorTest.constructor from adjacency map builds correct graph.
TEST(MemSubMapConstructorTest, ConstructorFromAdjacencyMapBuildsCorrectGraph) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  EXPECT_EQ(map_graph.vertex_count(), 13);
  EXPECT_EQ(map_graph.edge_count(), 20); // Total outgoing edges

  // Verify all vertices exist
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(map_graph.has_vertex(i));
  }
}

// mem_sub_map_t Neighbor Access Tests
TEST(MemSubMapNeighborAccessTest, GetNeighborsReturnsCorrectNeighborList) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  // Test vertex 0 neighbors (should be {2} based on edge e 0 2)
  auto neighbors_0 = map_graph.get_neighbors(0);
  EXPECT_EQ(neighbors_0.size(), 1);
  EXPECT_EQ(neighbors_0[0], 2);

  // Test vertex 1 neighbors (should be {0, 2, 3} based on edges e 1 0, e 1 2, e
  // 1 3)
  auto neighbors_1 = map_graph.get_neighbors(1);
  EXPECT_EQ(neighbors_1.size(), 3);
  std::unordered_set<v_id_t> expected_1 = {0, 2, 3};
  for (const auto& neighbor : neighbors_1) {
    EXPECT_EQ(expected_1.count(neighbor), 1);
  }

  // Test vertex 5 neighbors (should be {4} based on edge e 5 4)
  auto neighbors_5 = map_graph.get_neighbors(5);
  EXPECT_EQ(neighbors_5.size(), 1);
  EXPECT_EQ(neighbors_5[0], 4);

  // Test vertex 9 neighbors (should be empty - no outgoing edges)
  auto neighbors_9 = map_graph.get_neighbors(9);
  EXPECT_TRUE(neighbors_9.empty());
}

// Test MemSubMapNeighborAccessTest.get neighbors reference returns correct neighbor list.
TEST(MemSubMapNeighborAccessTest, GetNeighborsReferenceReturnsCorrectNeighborList) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  // Test vertex 0 neighbors reference
  const auto& neighbors_0 = map_graph.get_neighbors_ref(0);
  EXPECT_EQ(neighbors_0.size(), 1);
  EXPECT_EQ(neighbors_0[0], 2);

  // Test vertex 1 neighbors reference
  const auto& neighbors_1 = map_graph.get_neighbors_ref(1);
  EXPECT_EQ(neighbors_1.size(), 3);
  std::unordered_set<v_id_t> expected_1 = {0, 2, 3};
  for (const auto& neighbor : neighbors_1) {
    EXPECT_EQ(expected_1.count(neighbor), 1);
  }
}

// Test MemSubMapNeighborAccessTest.get neighbors for non existent vertex returns empty.
TEST(MemSubMapNeighborAccessTest, GetNeighborsForNonExistentVertexReturnsEmpty) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  EXPECT_EQ(map_graph.get_neighbors(999).size(), 0);
  EXPECT_EQ(map_graph.get_neighbors_ref(999).size(), 0);
}

// mem_sub_map_t Vertex Existence Tests
TEST(MemSubMapVertexExistenceTest, HasVertexReturnsCorrectValues) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  // Test existing vertices
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(map_graph.has_vertex(i));
  }

  // Test non-existing vertices
  EXPECT_FALSE(map_graph.has_vertex(13));
  EXPECT_FALSE(map_graph.has_vertex(999));
  EXPECT_FALSE(map_graph.has_vertex(100));
}

// Test MemSubMapVertexExistenceTest.vertex list returns all vertices.
TEST(MemSubMapVertexExistenceTest, VertexListReturnsAllVertices) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  auto vertices = map_graph.vertex_list();
  EXPECT_EQ(vertices.size(), 13);

  // Convert to set for easy checking
  std::unordered_set<v_id_t> vertex_set(vertices.begin(), vertices.end());

  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_EQ(vertex_set.count(i), 1);
  }
}

// mem_sub_map_t Edge Existence Tests
TEST(MemSubMapEdgeExistenceTest, HasEdgeReturnsCorrectValuesForExistingEdges) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  // Test some existing edges
  EXPECT_TRUE(map_graph.has_edge(0, 2));
  EXPECT_TRUE(map_graph.has_edge(1, 0));
  EXPECT_TRUE(map_graph.has_edge(1, 2));
  EXPECT_TRUE(map_graph.has_edge(1, 3));
  EXPECT_TRUE(map_graph.has_edge(3, 4));
  EXPECT_TRUE(map_graph.has_edge(11, 12));
}

// Test MemSubMapEdgeExistenceTest.has edge returns false for non existing edges.
TEST(MemSubMapEdgeExistenceTest, HasEdgeReturnsFalseForNonExistingEdges) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  // Test non-existing edges
  EXPECT_FALSE(map_graph.has_edge(0, 1));   // Reverse direction
  EXPECT_FALSE(map_graph.has_edge(2, 0));   // Reverse direction
  EXPECT_FALSE(map_graph.has_edge(9, 10));  // Non-existing edge
  EXPECT_FALSE(map_graph.has_edge(12, 11)); // Reverse direction
}

// Test MemSubMapEdgeExistenceTest.has edge returns false for non existent vertices.
TEST(MemSubMapEdgeExistenceTest, HasEdgeReturnsFalseForNonExistentVertices) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  EXPECT_FALSE(map_graph.has_edge(999, 0));
  EXPECT_FALSE(map_graph.has_edge(0, 999));
  EXPECT_FALSE(map_graph.has_edge(999, 1000));
}

// mem_sub_map_t to_map Conversion Tests
TEST(MemSubMapToMapConversionTest, ToMapReturnsCorrectAdjacencyRepresentation) {
  auto [graph_view, original_data] = create_test_graph();
  mem_sub_map_t map_graph(original_data);

  auto reconstructed_map = map_graph.to_map();

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

// Test MemSubMapToMapConversionTest.to map works for empty graph.
TEST(MemSubMapToMapConversionTest, ToMapWorksForEmptyGraph) {
  mem_sub_map_t map_graph;
  auto adj_map = map_graph.to_map();
  EXPECT_TRUE(adj_map.empty());
}

// mem_sub_map_t Count Tests
TEST(MemSubMapCountTest, VertexCountAndEdgeCountReturnCorrectValues) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  EXPECT_EQ(map_graph.vertex_count(), 13);
  EXPECT_EQ(map_graph.edge_count(), 20);
}

// Test MemSubMapCountTest.simple graph has correct counts.
TEST(MemSubMapCountTest, SimpleGraphHasCorrectCounts) {
  auto simple_data = create_simple_graph();
  mem_sub_map_t map_graph(simple_data);

  EXPECT_EQ(map_graph.vertex_count(), 4);
  EXPECT_EQ(map_graph.edge_count(), 4);
}

// Test MemSubMapCountTest.empty graph has zero counts.
TEST(MemSubMapCountTest, EmptyGraphHasZeroCounts) {
  mem_sub_map_t map_graph;

  EXPECT_EQ(map_graph.vertex_count(), 0);
  EXPECT_EQ(map_graph.edge_count(), 0);
}

// mem_sub_map_t Simple Graph Tests
TEST(MemSubMapSimpleGraphTest, SimpleGraphStructureIsCorrect) {
  auto graph_data = create_simple_graph();
  mem_sub_map_t map_graph(graph_data);

  // Verify vertex 0 neighbors
  auto neighbors_0 = map_graph.get_neighbors(0);
  EXPECT_EQ(neighbors_0.size(), 2);
  std::unordered_set<v_id_t> expected_0 = {1, 3};
  std::unordered_set<v_id_t> actual_0(neighbors_0.begin(), neighbors_0.end());
  EXPECT_EQ(expected_0, actual_0);

  // Verify vertex 1 neighbors
  auto neighbors_1 = map_graph.get_neighbors(1);
  EXPECT_EQ(neighbors_1.size(), 1);
  EXPECT_EQ(neighbors_1[0], 2);

  // Verify vertex 2 neighbors
  auto neighbors_2 = map_graph.get_neighbors(2);
  EXPECT_EQ(neighbors_2.size(), 1);
  EXPECT_EQ(neighbors_2[0], 3);

  // Verify vertex 3 neighbors (should be empty)
  auto neighbors_3 = map_graph.get_neighbors(3);
  EXPECT_TRUE(neighbors_3.empty());
}

// mem_sub_map_t Modification Tests
TEST(MemSubMapModificationTest, AddVertexWorksCorrectly) {
  mem_sub_map_t map_graph;

  EXPECT_TRUE(map_graph.add_vertex(0));
  EXPECT_TRUE(map_graph.add_vertex(1));
  EXPECT_FALSE(map_graph.add_vertex(0)); // Already exists

  EXPECT_EQ(map_graph.vertex_count(), 2);
  EXPECT_TRUE(map_graph.has_vertex(0));
  EXPECT_TRUE(map_graph.has_vertex(1));
}

// Test MemSubMapModificationTest.add edge works correctly.
TEST(MemSubMapModificationTest, AddEdgeWorksCorrectly) {
  mem_sub_map_t map_graph;

  EXPECT_TRUE(map_graph.add_edge(0, 1));
  EXPECT_TRUE(map_graph.add_edge(1, 2));
  EXPECT_FALSE(map_graph.add_edge(0, 1)); // Already exists

  EXPECT_EQ(map_graph.vertex_count(), 3);
  EXPECT_EQ(map_graph.edge_count(), 2);
  EXPECT_TRUE(map_graph.has_edge(0, 1));
  EXPECT_TRUE(map_graph.has_edge(1, 2));
}

// Test MemSubMapModificationTest.remove vertex works correctly.
TEST(MemSubMapModificationTest, RemoveVertexWorksCorrectly) {
  auto graph_data = create_simple_graph();
  mem_sub_map_t map_graph(graph_data);

  EXPECT_TRUE(map_graph.remove_vertex(1));
  EXPECT_FALSE(map_graph.remove_vertex(1)); // Already removed

  EXPECT_FALSE(map_graph.has_vertex(1));
  EXPECT_FALSE(map_graph.has_edge(0, 1));
  EXPECT_FALSE(map_graph.has_edge(1, 2));
}

// Test MemSubMapModificationTest.remove edge works correctly.
TEST(MemSubMapModificationTest, RemoveEdgeWorksCorrectly) {
  auto graph_data = create_simple_graph();
  mem_sub_map_t map_graph(graph_data);

  EXPECT_TRUE(map_graph.remove_edge(0, 1));
  EXPECT_FALSE(map_graph.remove_edge(0, 1)); // Already removed

  EXPECT_FALSE(map_graph.has_edge(0, 1));
  EXPECT_TRUE(map_graph.has_vertex(0));
  EXPECT_TRUE(map_graph.has_vertex(1));
}

// Test MemSubMapModificationTest.clear works correctly.
TEST(MemSubMapModificationTest, ClearWorksCorrectly) {
  auto [graph_view, graph_data] = create_test_graph();
  mem_sub_map_t map_graph(graph_data);

  map_graph.clear();

  EXPECT_EQ(map_graph.vertex_count(), 0);
  EXPECT_EQ(map_graph.edge_count(), 0);
  EXPECT_TRUE(map_graph.vertex_list().empty());
}

// mem_sub_map_t edge cases.
TEST(MemSubMapEdgeCasesTest, SingleVertexGraph) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> single_vertex;
  single_vertex[42] = {};

  mem_sub_map_t map_graph(single_vertex);

  EXPECT_EQ(map_graph.vertex_count(), 1);
  EXPECT_EQ(map_graph.edge_count(), 0);
  EXPECT_TRUE(map_graph.has_vertex(42));
  EXPECT_TRUE(map_graph.get_neighbors(42).empty());
}

// Test MemSubMapEdgeCasesTest.self loop graph.
TEST(MemSubMapEdgeCasesTest, SelfLoopGraph) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> self_loop;
  self_loop[0] = {0};

  mem_sub_map_t map_graph(self_loop);

  EXPECT_EQ(map_graph.vertex_count(), 1);
  EXPECT_EQ(map_graph.edge_count(), 1);
  EXPECT_TRUE(map_graph.has_vertex(0));
  EXPECT_TRUE(map_graph.has_edge(0, 0));

  auto neighbors = map_graph.get_neighbors(0);
  EXPECT_EQ(neighbors.size(), 1);
  EXPECT_EQ(neighbors[0], 0);
}

// Test MemSubMapEdgeCasesTest.complete graph on3 vertices.
TEST(MemSubMapEdgeCasesTest, CompleteGraphOn3Vertices) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> complete_3;
  complete_3[0] = {1, 2};
  complete_3[1] = {0, 2};
  complete_3[2] = {0, 1};

  mem_sub_map_t map_graph(complete_3);

  EXPECT_EQ(map_graph.vertex_count(), 3);
  EXPECT_EQ(map_graph.edge_count(), 6);

  // Each vertex should have 2 neighbors
  for (v_id_t i = 0; i < 3; ++i) {
    auto neighbors = map_graph.get_neighbors(i);
    EXPECT_EQ(neighbors.size(), 2);
  }

  // All edges should exist
  for (v_id_t i = 0; i < 3; ++i) {
    for (v_id_t j = 0; j < 3; ++j) {
      if (i != j) {
        EXPECT_TRUE(map_graph.has_edge(i, j));
      }
    }
  }
}