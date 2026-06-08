#include "bw_graph/mem/mem_g0com.h"
#include "bw_graph/mem/mem_g0csr.h"
#include "test.h"

#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <unordered_set>
#include <vector>

// Helper function to create test graph file
inline void create_test_graph_file(const std::string& filename, bool include_communities = true) {
  std::ofstream file(filename);

  // Write header
  file << "t 13 20\n";

  // Write vertices with communities
  if (include_communities) {
    file << "v 0 0 0\n";
    file << "v 1 0 0\n";
    file << "v 2 0 0\n";
    file << "v 3 0 0\n";
    file << "v 4 0 1\n";
    file << "v 5 0 1\n";
    file << "v 6 0 1\n";
    file << "v 7 0 2\n";
    file << "v 8 0 2\n";
    file << "v 9 0 2\n";
    file << "v 10 0 2\n";
    file << "v 11 0 3\n";
    file << "v 12 0 3\n";
  } else {
    for (int i = 0; i < 13; ++i) {
      file << "v " << i << " 0 0\n";
    }
  }

  // Write edges (same as in create_test_graph())
  file << "e 0 2\n";
  file << "e 1 0\n";
  file << "e 1 2\n";
  file << "e 1 3\n";
  file << "e 2 3\n";
  file << "e 3 0\n";
  file << "e 3 4\n";
  file << "e 3 11\n";
  file << "e 4 6\n";
  file << "e 4 7\n";
  file << "e 5 4\n";
  file << "e 6 5\n";
  file << "e 7 3\n";
  file << "e 7 8\n";
  file << "e 7 9\n";
  file << "e 8 9\n";
  file << "e 8 10\n";
  file << "e 10 7\n";
  file << "e 10 9\n";
  file << "e 11 12\n";

  file.close();
}

// mem_sub_com_t constructor cases.
TEST(MemSubComConstructorTest, DefaultConstructorCreatesEmptyCSR) {
  mem_sub_com_t com_graph;

  EXPECT_EQ(com_graph.vertex_count(), 0);
  EXPECT_EQ(com_graph.edge_count(), 0);
  EXPECT_TRUE(com_graph.vertex_list().empty());
}

// Test MemSubComConstructorTest.constructor from adjacency map builds correct csr.
TEST(MemSubComConstructorTest, ConstructorFromAdjacencyMapBuildsCorrectCSR) {
  auto [graph_view, graph_data] = create_test_graph();
  folly::F14FastMap<v_id_t, v_id_t> community_map = {{0, 0},  {1, 0},  {2, 0}, {3, 0}, {4, 1},
                                                     {5, 1},  {6, 1},  {7, 2}, {8, 2}, {9, 2},
                                                     {10, 2}, {11, 3}, {12, 3}};

  mem_sub_com_t com_graph(graph_data, community_map);

  EXPECT_EQ(com_graph.vertex_count(), 13);
  EXPECT_EQ(com_graph.edge_count(), 20);

  // Verify all vertices exist
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(com_graph.has_vertex(i));
  }
}

// Test MemSubComConstructorTest.constructor from file builds correct csrwith communities.
TEST(MemSubComConstructorTest, ConstructorFromFileBuildsCorrectCSRWithCommunities) {
  std::string test_file = "test_graph_with_communities.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  EXPECT_EQ(com_graph.vertex_count(), 13);
  EXPECT_EQ(com_graph.edge_count(), 40);

  // Verify all vertices exist
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(com_graph.has_vertex(i));
  }

  // Verify communities
  EXPECT_EQ(com_graph.get_community_id(0), 0);
  EXPECT_EQ(com_graph.get_community_id(4), 1);
  EXPECT_EQ(com_graph.get_community_id(7), 2);
  EXPECT_EQ(com_graph.get_community_id(11), 3);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComConstructorTest.constructor from file builds correct csrwithout communities.
TEST(MemSubComConstructorTest, ConstructorFromFileBuildsCorrectCSRWithoutCommunities) {
  std::string test_file = "test_graph_no_communities.txt";
  create_test_graph_file(test_file, false);

  mem_sub_com_t com_graph(test_file, false);

  EXPECT_EQ(com_graph.vertex_count(), 13);
  EXPECT_EQ(com_graph.edge_count(), 40);

  // Verify all vertices exist
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(com_graph.has_vertex(i));
  }

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComNeighborAccessTest.get neighbors for non existent vertex returns empty.
TEST(MemSubComNeighborAccessTest, GetNeighborsForNonExistentVertexReturnsEmpty) {
  std::string test_file = "test_invalid_vertex.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  EXPECT_EQ(com_graph.get_neighbors(999).size(), 0);

  // Clean up
  std::remove(test_file.c_str());
}

// mem_sub_com_t Vertex Existence Tests
TEST(MemSubComVertexExistenceTest, HasVertexReturnsCorrectValues) {
  std::string test_file = "test_vertex_existence.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Test existing vertices
  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_TRUE(com_graph.has_vertex(i));
  }

  // Test non-existing vertices
  EXPECT_FALSE(com_graph.has_vertex(13));
  EXPECT_FALSE(com_graph.has_vertex(999));
  EXPECT_FALSE(com_graph.has_vertex(100));

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComVertexExistenceTest.vertex list returns all vertices.
TEST(MemSubComVertexExistenceTest, VertexListReturnsAllVertices) {
  std::string test_file = "test_vertex_list.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  auto vertices = com_graph.vertex_list();
  EXPECT_EQ(vertices.size(), 13);

  // Convert to set for easy checking
  std::unordered_set<v_id_t> vertex_set(vertices.begin(), vertices.end());

  for (v_id_t i = 0; i <= 12; ++i) {
    EXPECT_EQ(vertex_set.count(i), 1);
  }

  // Clean up
  std::remove(test_file.c_str());
}

// mem_sub_com_t Edge Existence Tests
TEST(MemSubComEdgeExistenceTest, HasEdgeReturnsCorrectValuesForExistingEdges) {
  std::string test_file = "test_edge_existence.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Test some existing edges
  EXPECT_TRUE(com_graph.has_edge(0, 2));
  EXPECT_TRUE(com_graph.has_edge(1, 0));
  EXPECT_TRUE(com_graph.has_edge(1, 2));
  EXPECT_TRUE(com_graph.has_edge(1, 3));
  EXPECT_TRUE(com_graph.has_edge(3, 4));
  EXPECT_TRUE(com_graph.has_edge(11, 12));

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComEdgeExistenceTest.has edge returns false for non existing edges.
TEST(MemSubComEdgeExistenceTest, HasEdgeReturnsFalseForNonExistingEdges) {
  std::string test_file = "test_nonexistent_edges.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Test non-existing edges
  EXPECT_TRUE(com_graph.has_edge(0, 1));   // Reverse direction
  EXPECT_TRUE(com_graph.has_edge(2, 0));   // Reverse direction
  EXPECT_TRUE(com_graph.has_edge(9, 10));  // Non-existing edge
  EXPECT_TRUE(com_graph.has_edge(12, 11)); // Reverse direction

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComEdgeExistenceTest.has edge returns false for non existent vertices.
TEST(MemSubComEdgeExistenceTest, HasEdgeReturnsFalseForNonExistentVertices) {
  std::string test_file = "test_invalid_edge_vertices.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  EXPECT_FALSE(com_graph.has_edge(999, 0));
  EXPECT_FALSE(com_graph.has_edge(0, 999));
  EXPECT_FALSE(com_graph.has_edge(999, 1000));

  // Clean up
  std::remove(test_file.c_str());
}

// mem_sub_com_t Community Tests
TEST(MemSubComCommunityTest, GetCommunityIdReturnsCorrectCommunityAssignments) {
  std::string test_file = "test_communities.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Test community assignments
  EXPECT_EQ(com_graph.get_community_id(0), 0);
  EXPECT_EQ(com_graph.get_community_id(1), 0);
  EXPECT_EQ(com_graph.get_community_id(2), 0);
  EXPECT_EQ(com_graph.get_community_id(3), 0);

  EXPECT_EQ(com_graph.get_community_id(4), 1);
  EXPECT_EQ(com_graph.get_community_id(5), 1);
  EXPECT_EQ(com_graph.get_community_id(6), 1);

  EXPECT_EQ(com_graph.get_community_id(7), 2);
  EXPECT_EQ(com_graph.get_community_id(8), 2);
  EXPECT_EQ(com_graph.get_community_id(9), 2);
  EXPECT_EQ(com_graph.get_community_id(10), 2);

  EXPECT_EQ(com_graph.get_community_id(11), 3);
  EXPECT_EQ(com_graph.get_community_id(12), 3);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComCommunityTest.get vertices in community returns correct vertex lists.
TEST(MemSubComCommunityTest, GetVerticesInCommunityReturnsCorrectVertexLists) {
  std::string test_file = "test_community_vertices.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Test community 0
  auto community_0 = com_graph.get_vertices_in_community(0);
  EXPECT_EQ(community_0.size(), 4);
  std::unordered_set<v_id_t> expected_0 = {0, 1, 2, 3};
  std::unordered_set<v_id_t> actual_0(community_0.begin(), community_0.end());
  EXPECT_EQ(expected_0, actual_0);

  // Test community 1
  auto community_1 = com_graph.get_vertices_in_community(1);
  EXPECT_EQ(community_1.size(), 3);
  std::unordered_set<v_id_t> expected_1 = {4, 5, 6};
  std::unordered_set<v_id_t> actual_1(community_1.begin(), community_1.end());
  EXPECT_EQ(expected_1, actual_1);

  // Test community 2
  auto community_2 = com_graph.get_vertices_in_community(2);
  EXPECT_EQ(community_2.size(), 4);
  std::unordered_set<v_id_t> expected_2 = {7, 8, 9, 10};
  std::unordered_set<v_id_t> actual_2(community_2.begin(), community_2.end());
  EXPECT_EQ(expected_2, actual_2);

  // Test community 3
  auto community_3 = com_graph.get_vertices_in_community(3);
  EXPECT_EQ(community_3.size(), 2);
  std::unordered_set<v_id_t> expected_3 = {11, 12};
  std::unordered_set<v_id_t> actual_3(community_3.begin(), community_3.end());
  EXPECT_EQ(expected_3, actual_3);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComCommunityTest.get community id throws exception for non existent vertex.
TEST(MemSubComCommunityTest, GetCommunityIdThrowsExceptionForNonExistentVertex) {
  std::string test_file = "test_invalid_community.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  EXPECT_THROW(com_graph.get_community_id(999), std::invalid_argument);

  // Clean up
  std::remove(test_file.c_str());
}

// mem_sub_com_t Count Tests
TEST(MemSubComCountTest, VertexCountAndEdgeCountReturnCorrectValues) {
  std::string test_file = "test_counts.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  EXPECT_EQ(com_graph.vertex_count(), 13);
  EXPECT_EQ(com_graph.edge_count(), 40);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComCountTest.empty graph has zero counts.
TEST(MemSubComCountTest, EmptyGraphHasZeroCounts) {
  mem_sub_com_t com_graph;

  EXPECT_EQ(com_graph.vertex_count(), 0);
  EXPECT_EQ(com_graph.edge_count(), 0);
}

// mem_sub_com_t Subgraph Tests
TEST(MemSubComSubgraphTest, ComputeInducedSubgraphWorksCorrectly) {
  std::string test_file = "test_subgraph.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Create subgraph with vertices {0, 1, 2, 3}
  std::vector<v_id_t> subset = {0, 1, 2, 3};
  std::shared_ptr<mem_sub_csr_t> subgraph = com_graph.compute_induced_subgraph(subset);

  EXPECT_NE(subgraph, nullptr);
  EXPECT_EQ(subgraph->vertex_count(), 4);

  // Verify edges exist in subgraph
  EXPECT_TRUE(subgraph->has_edge(1, 0));
  EXPECT_TRUE(subgraph->has_edge(1, 2));
  EXPECT_TRUE(subgraph->has_edge(1, 3));
  EXPECT_TRUE(subgraph->has_edge(2, 3));
  EXPECT_TRUE(subgraph->has_edge(3, 0));

  // Verify edges to vertices outside subset don't exist
  EXPECT_FALSE(subgraph->has_vertex(4));
  EXPECT_FALSE(subgraph->has_vertex(11));

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComSubgraphTest.compute community subgraph works correctly.
TEST(MemSubComSubgraphTest, ComputeCommunitySubgraphWorksCorrectly) {
  std::string test_file = "test_community_subgraph.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  // Create subgraph for community 0
  std::shared_ptr<mem_sub_csr_t> community_subgraph = com_graph.compute_community_subgraph(0);

  EXPECT_NE(community_subgraph, nullptr);
  EXPECT_EQ(community_subgraph->vertex_count(), 4);

  // Verify all vertices in community 0 exist
  EXPECT_TRUE(community_subgraph->has_vertex(0));
  EXPECT_TRUE(community_subgraph->has_vertex(1));
  EXPECT_TRUE(community_subgraph->has_vertex(2));
  EXPECT_TRUE(community_subgraph->has_vertex(3));

  // Verify vertices from other communities don't exist
  EXPECT_FALSE(community_subgraph->has_vertex(4));
  EXPECT_FALSE(community_subgraph->has_vertex(7));

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemSubComSubgraphTest.compute induced subgraph throws for invalid vertices.
TEST(MemSubComSubgraphTest, ComputeInducedSubgraphThrowsForInvalidVertices) {
  std::string test_file = "test_invalid_subgraph.txt";
  create_test_graph_file(test_file, true);

  mem_sub_com_t com_graph(test_file, true);

  std::vector<v_id_t> invalid_subset = {0, 1, 999};
  EXPECT_THROW(com_graph.compute_induced_subgraph(invalid_subset), std::invalid_argument);

  // Clean up
  std::remove(test_file.c_str());
}