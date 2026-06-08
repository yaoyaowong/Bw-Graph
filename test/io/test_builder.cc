
#include "bw_graph/common/type.h"
#include "bw_graph/io/builder.h"

#include <gtest/gtest.h>

// Constructor cases.
TEST(CSRPageBuilderTest, ConstructorInitialization) {
  csr_page_builder_t builder(4096, 2);

  EXPECT_TRUE(builder.is_empty());
  EXPECT_EQ(builder.estimated_size(),
            sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                sizeof(uint32_t) + bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t));
}

// is_empty() Tests
TEST(CSRPageBuilderTest, IsEmptyInitially) {
  csr_page_builder_t builder(4096, 2);

  EXPECT_TRUE(builder.is_empty());
}

// Test CSRPageBuilderTest.is not empty after adding vertex.
TEST(CSRPageBuilderTest, IsNotEmptyAfterAddingVertex) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors = {2, 3, 4};
  neighbor_span_t neighbor_span(neighbors);

  builder.add_vertex(1, neighbor_span);

  EXPECT_FALSE(builder.is_empty());
}

// add_vertex() Tests - Regular Vertices
TEST(CSRPageBuilderTest, AddSingleRegularVertex) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors = {2, 3, 4};
  neighbor_span_t neighbor_span(neighbors);

  bool result = builder.add_vertex(1, neighbor_span);

  EXPECT_TRUE(result);
  EXPECT_FALSE(builder.is_empty());
}

// Test CSRPageBuilderTest.add multiple regular vertices.
TEST(CSRPageBuilderTest, AddMultipleRegularVertices) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors1 = {2, 3, 4};
  std::vector<v_id_t> neighbors2 = {1, 5};
  std::vector<v_id_t> neighbors3 = {1};

  neighbor_span_t span1(neighbors1);
  neighbor_span_t span2(neighbors2);
  neighbor_span_t span3(neighbors3);

  EXPECT_TRUE(builder.add_vertex(1, span1));
  EXPECT_TRUE(builder.add_vertex(2, span2));
  EXPECT_TRUE(builder.add_vertex(5, span3));
}

// Test CSRPageBuilderTest.add vertex with no neighbors.
TEST(CSRPageBuilderTest, AddVertexWithNoNeighbors) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> empty_neighbors;
  neighbor_span_t neighbor_span(empty_neighbors);

  bool result = builder.add_vertex(10, neighbor_span);

  EXPECT_TRUE(result);
}

// Test CSRPageBuilderTest.add giant vertex.
TEST(CSRPageBuilderTest, AddGiantVertex) {
  csr_page_builder_t builder(4096, 2);

  // Create a giant vertex with degree >= BW_GRAPH_GIANT_DEGREE_BOUND
  std::vector<v_id_t> giant_neighbors;
  for (v_id_t i = 0; i < bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND; ++i) {
    giant_neighbors.push_back(i + 100);
  }
  neighbor_span_t neighbor_span(giant_neighbors);

  bool result = builder.add_vertex(1, neighbor_span);

  EXPECT_TRUE(result);
  EXPECT_FALSE(builder.is_empty());
}

// Test CSRPageBuilderTest.add mixed regular and giant vertices.
TEST(CSRPageBuilderTest, AddMixedRegularAndGiantVertices) {
  csr_page_builder_t builder(4096, 2);

  // Add regular vertex
  std::vector<v_id_t> regular_neighbors = {2, 3, 4};
  neighbor_span_t regular_span(regular_neighbors);
  EXPECT_TRUE(builder.add_vertex(1, regular_span));

  // Add giant vertex
  std::vector<v_id_t> giant_neighbors;
  for (v_id_t i = 0; i < bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND; ++i) {
    giant_neighbors.push_back(i + 100);
  }
  neighbor_span_t giant_span(giant_neighbors);
  EXPECT_TRUE(builder.add_vertex(2, giant_span));

  // Add another regular vertex
  std::vector<v_id_t> regular_neighbors2 = {1, 5};
  neighbor_span_t regular_span2(regular_neighbors2);
  EXPECT_TRUE(builder.add_vertex(3, regular_span2));
}

// Test CSRPageBuilderTest.reject vertex when page full.
TEST(CSRPageBuilderTest, RejectVertexWhenPageFull) {
  // Create a small page
  csr_page_builder_t builder(512, 2);

  // Add vertices until page is full
  std::vector<v_id_t> neighbors = {2, 3, 4, 5, 6};
  neighbor_span_t neighbor_span(neighbors);

  bool result = true;
  int vertex_count = 0;

  for (v_id_t vid = 1; vid <= 100 && result; ++vid) {
    result = builder.add_vertex(vid, neighbor_span);
    if (result) {
      vertex_count++;
    }
  }

  // Should have added at least 1 vertex
  EXPECT_GT(vertex_count, 0);
  // Should have rejected at least one vertex (page became full)
  EXPECT_FALSE(result);
}

// Test CSRPageBuilderTest.estimated size empty.
TEST(CSRPageBuilderTest, EstimatedSizeEmpty) {
  csr_page_builder_t builder(4096, 2);

  size_t expected_size = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                         sizeof(uint32_t) +
                         bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);

  EXPECT_EQ(builder.estimated_size(), expected_size);
}

// Test CSRPageBuilderTest.estimated size with regular vertices.
TEST(CSRPageBuilderTest, EstimatedSizeWithRegularVertices) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors1 = {2, 3, 4}; // 3 neighbors + 2 slots = 5
  std::vector<v_id_t> neighbors2 = {1, 5};    // 2 neighbors + 2 slots = 4 edges

  neighbor_span_t span1(neighbors1);
  neighbor_span_t span2(neighbors2);

  builder.add_vertex(1, span1);
  builder.add_vertex(2, span2);

  size_t expected_size =
      sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) +
      sizeof(uint32_t) +                                          // metadata: 16 bytes
      bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t) + // neighbor block
      2 * sizeof(csr_vertex_t) +                                  // 2 vertices (12 bytes each)
      (5 + 4) * sizeof(v_id_t);                                   // 9 edges total
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  expected_size += 9 * sizeof(timestamp_t); // per-edge timestamps for all 9 edge slots
#endif

  EXPECT_EQ(builder.estimated_size(), expected_size);
}

// Test CSRPageBuilderTest.estimated size with giant vertex.
TEST(CSRPageBuilderTest, EstimatedSizeWithGiantVertex) {
  csr_page_builder_t builder(4096, 2);

  // Add giant vertex
  std::vector<v_id_t> giant_neighbors;
  for (v_id_t i = 0; i < bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND; ++i) {
    giant_neighbors.push_back(i + 100);
  }
  neighbor_span_t giant_span(giant_neighbors);
  builder.add_vertex(1, giant_span);

  // Giant vertex: metadata + neighbor block + 1 vertex entry (no neighbor data
  // in page)
  size_t expected_size = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                         sizeof(uint32_t) + // metadata: 16 bytes
                         bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t) +
                         1 * sizeof(csr_vertex_t); // 1 vertex: 12 bytes

  EXPECT_EQ(builder.estimated_size(), expected_size);
}

// Test CSRPageBuilderTest.build empty throws exception.
TEST(CSRPageBuilderTest, BuildEmptyThrowsException) {
  csr_page_builder_t builder(4096, 2);

  EXPECT_THROW({ builder.build(); }, std::runtime_error);
}

// Test CSRPageBuilderTest.build single regular vertex.
TEST(CSRPageBuilderTest, BuildSingleRegularVertex) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors = {2, 3, 4};
  neighbor_span_t neighbor_span(neighbors);
  builder.add_vertex(1, neighbor_span);

  auto [page, vertex_map] = builder.build();

  // Check vertex map
  EXPECT_EQ(vertex_map.size(), 1);
  EXPECT_EQ(vertex_map[0].first, 1);  // vertex_id
  EXPECT_EQ(vertex_map[0].second, 0); // index in page

  // Check page metadata
  EXPECT_EQ(page->get_num_vertices(), 1);
  EXPECT_EQ(page->get_num_edges(), 3);
}

// Test CSRPageBuilderTest.build multiple regular vertices.
TEST(CSRPageBuilderTest, BuildMultipleRegularVertices) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors1 = {2, 3, 4};
  std::vector<v_id_t> neighbors2 = {1, 5};
  std::vector<v_id_t> neighbors3 = {1};

  neighbor_span_t span1(neighbors1);
  neighbor_span_t span2(neighbors2);
  neighbor_span_t span3(neighbors3);

  builder.add_vertex(1, span1);
  builder.add_vertex(2, span2);
  builder.add_vertex(5, span3);

  auto [page, vertex_map] = builder.build();

  // Check vertex map
  EXPECT_EQ(vertex_map.size(), 3);
  EXPECT_EQ(vertex_map[0].first, 1);
  EXPECT_EQ(vertex_map[1].first, 2);
  EXPECT_EQ(vertex_map[2].first, 5);

  // Check page metadata
  EXPECT_EQ(page->get_num_vertices(), 3);
  EXPECT_EQ(page->get_num_edges(), 6); // 3 + 2 + 1

  // Check vertex degrees
  vertex_span_t vertices = page->get_vertices();
  EXPECT_EQ(vertices[0].vertex_id, 1);
  EXPECT_EQ(vertices[0].degree, 3);
  EXPECT_EQ(vertices[1].vertex_id, 2);
  EXPECT_EQ(vertices[1].degree, 2);
  EXPECT_EQ(vertices[2].vertex_id, 5);
  EXPECT_EQ(vertices[2].degree, 1);
}

// Test CSRPageBuilderTest.build with giant vertex.
TEST(CSRPageBuilderTest, BuildWithGiantVertex) {
  csr_page_builder_t builder(4096, 2);

  // Add giant vertex
  std::vector<v_id_t> giant_neighbors;
  for (v_id_t i = 0; i < bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND; ++i) {
    giant_neighbors.push_back(i + 100);
  }
  neighbor_span_t giant_span(giant_neighbors);
  builder.add_vertex(1, giant_span);

  auto [page, vertex_map] = builder.build();

  // Check page metadata
  EXPECT_EQ(page->get_num_vertices(), 1);
  // Giant vertex edges not stored in page
  EXPECT_EQ(page->get_num_edges(), 0);

  // Check vertex structure
  vertex_span_t vertices = page->get_vertices();
  EXPECT_EQ(vertices[0].vertex_id, 1);
  EXPECT_EQ(vertices[0].degree, bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND);
  EXPECT_EQ(vertices[0].offset, 0); // Giant vertex offset is 0
}

// Test CSRPageBuilderTest.giant vertex has no paged neighbors.
TEST(CSRPageBuilderTest, GiantVertexHasNoPagedNeighbors) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> giant_neighbors;
  for (v_id_t i = 0; i < bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND; ++i) {
    giant_neighbors.push_back(i + 100);
  }
  neighbor_span_t giant_span(giant_neighbors);
  builder.add_vertex(1, giant_span);

  auto [page, vertex_map] = builder.build();

  ASSERT_EQ(vertex_map.size(), 1);
  EXPECT_EQ(vertex_map[0].second, 0);
  EXPECT_TRUE(page->get_neighbors(vertex_map[0].second).empty());
}

// Test CSRPageBuilderTest.build with mixed vertices.
TEST(CSRPageBuilderTest, BuildWithMixedVertices) {
  csr_page_builder_t builder(4096, 2);

  // Add regular vertex
  std::vector<v_id_t> regular_neighbors = {2, 3};
  neighbor_span_t regular_span(regular_neighbors);
  builder.add_vertex(1, regular_span);

  // Add giant vertex
  std::vector<v_id_t> giant_neighbors;
  for (v_id_t i = 0; i < bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND; ++i) {
    giant_neighbors.push_back(i + 100);
  }
  neighbor_span_t giant_span(giant_neighbors);
  builder.add_vertex(2, giant_span);

  // Add another regular vertex
  std::vector<v_id_t> regular_neighbors2 = {1, 5, 6};
  neighbor_span_t regular_span2(regular_neighbors2);
  builder.add_vertex(3, regular_span2);

  auto [page, vertex_map] = builder.build();
  std::cout << "Page build done" << std::endl;

  // Check page metadata
  EXPECT_EQ(page->get_num_vertices(), 3);
  EXPECT_EQ(page->get_num_edges(), 5); // Only count regular vertices: 2 + 3

  // Check vertex structures
  vertex_span_t vertices = page->get_vertices();

  // First vertex (regular)
  EXPECT_EQ(vertices[0].vertex_id, 1);
  EXPECT_EQ(vertices[0].degree, 2);
  EXPECT_EQ(vertices[0].offset, 0); // Has actual offset

  // Second vertex (giant)
  EXPECT_EQ(vertices[1].vertex_id, 2);
  EXPECT_EQ(vertices[1].degree, bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND);
  EXPECT_EQ(vertices[1].offset, 0); // Giant vertex offset is 0

  // Third vertex (regular)
  EXPECT_EQ(vertices[2].vertex_id, 3);
  EXPECT_EQ(vertices[2].degree, 3);
  EXPECT_GT(vertices[2].offset, 0);
}

// Test CSRPageBuilderTest.build vertex map correctness.
TEST(CSRPageBuilderTest, BuildVertexMapCorrectness) {
  csr_page_builder_t builder(4096, 2);

  std::vector<v_id_t> neighbors1 = {2, 3};
  std::vector<v_id_t> neighbors2 = {1, 4};
  std::vector<v_id_t> neighbors3 = {2, 5};

  neighbor_span_t span1(neighbors1);
  neighbor_span_t span2(neighbors2);
  neighbor_span_t span3(neighbors3);

  builder.add_vertex(10, span1);
  builder.add_vertex(20, span2);
  builder.add_vertex(30, span3);

  auto [page, vertex_map] = builder.build();

  // Vertex map should map vertex_id to its index in the page
  EXPECT_EQ(vertex_map[0].first, 10);
  EXPECT_EQ(vertex_map[0].second, 0);

  EXPECT_EQ(vertex_map[1].first, 20);
  EXPECT_EQ(vertex_map[1].second, 1);

  EXPECT_EQ(vertex_map[2].first, 30);
  EXPECT_EQ(vertex_map[2].second, 2);
}

// Test CSRPageBuilderTest.build with slots and insert edges.
TEST(CSRPageBuilderTest, BuildWithSlotsAndInsertEdges) {
  // Build a CSR page with 2 slots per vertex
  csr_page_builder_t builder(4096, 2);

  // Add vertex 1 with 2 neighbors
  std::vector<v_id_t> neighbors1 = {2, 3};
  neighbor_span_t span1(neighbors1);
  builder.add_vertex(1, span1);

  // Add vertex 2 with 1 neighbor
  std::vector<v_id_t> neighbors2 = {4};
  neighbor_span_t span2(neighbors2);
  builder.add_vertex(2, span2);

  // Add vertex 3 with 3 neighbors
  std::vector<v_id_t> neighbors3 = {1, 5, 6};
  neighbor_span_t span3(neighbors3);
  builder.add_vertex(3, span3);

  auto [page, vertex_map] = builder.build();
  std::cout << "Page build done with slots" << std::endl;

  // Check initial page metadata
  EXPECT_EQ(page->get_num_vertices(), 3);
  EXPECT_EQ(page->get_num_edges(), 6); // 2 + 1 + 3 = 6

  // Test 1: Insert edge to vertex 1 (offset 0) - first slot
  insert_status_t status1 = page->insert_edge_in_slot(0, 1, 7);
  EXPECT_EQ(status1, INSERTED);
  EXPECT_EQ(page->get_num_edges(), 7);

  vertex_span_t vertices = page->get_vertices();
  EXPECT_EQ(vertices[0].degree, 3); // Was 2, now 3

  // Verify the new neighbor was added
  neighbor_span_t vertex1_neighbors = page->get_neighbors(0);
  EXPECT_EQ(vertex1_neighbors.size(), 3);
  EXPECT_EQ(vertex1_neighbors[2], 7); // New neighbor at the end

  // Test 2: Insert edge to vertex 1 (offset 0) - second slot
  insert_status_t status2 = page->insert_edge_in_slot(0, 1, 8);
  EXPECT_EQ(status2, INSERTED);
  EXPECT_EQ(page->get_num_edges(), 8);
  EXPECT_EQ(vertices[0].degree, 4); // Was 3, now 4

  vertex1_neighbors = page->get_neighbors(0);
  EXPECT_EQ(vertex1_neighbors.size(), 4);
  EXPECT_EQ(vertex1_neighbors[3], 8); // Second new neighbor

  // Test 3: Try to insert beyond slots (should fail with SLOT_FULL)
  insert_status_t status3 = page->insert_edge_in_slot(0, 1, 9);
  EXPECT_EQ(status3, SLOT_FULL);
  EXPECT_EQ(page->get_num_edges(), 8); // No change
  EXPECT_EQ(vertices[0].degree, 4);    // No change

  // Test 4: Insert edge to vertex 2 (offset 1) - first slot
  insert_status_t status4 = page->insert_edge_in_slot(1, 2, 10);
  EXPECT_EQ(status4, INSERTED);
  EXPECT_EQ(page->get_num_edges(), 9);
  EXPECT_EQ(vertices[1].degree, 2); // Was 1, now 2

  neighbor_span_t vertex2_neighbors = page->get_neighbors(1);
  EXPECT_EQ(vertex2_neighbors.size(), 2);
  EXPECT_EQ(vertex2_neighbors[0], 4);  // Original neighbor
  EXPECT_EQ(vertex2_neighbors[1], 10); // New neighbor

  // Test 5: Insert edge to vertex 2 (offset 1) - second slot
  insert_status_t status5 = page->insert_edge_in_slot(1, 2, 11);
  EXPECT_EQ(status5, INSERTED);
  EXPECT_EQ(page->get_num_edges(), 10);
  EXPECT_EQ(vertices[1].degree, 3); // Was 2, now 3

  // Test 6: Insert edge to vertex 3 (offset 2) - first slot
  insert_status_t status6 = page->insert_edge_in_slot(2, 3, 12);
  EXPECT_EQ(status6, INSERTED);
  EXPECT_EQ(page->get_num_edges(), 11);
  EXPECT_EQ(vertices[2].degree, 4); // Was 3, now 4

  neighbor_span_t vertex3_neighbors = page->get_neighbors(2);
  EXPECT_EQ(vertex3_neighbors.size(), 4);
  EXPECT_EQ(vertex3_neighbors[3], 12); // New neighbor

  // Test 7: Insert edge to vertex 3 (offset 2) - second slot
  insert_status_t status7 = page->insert_edge_in_slot(2, 3, 13);
  EXPECT_EQ(status7, INSERTED);
  EXPECT_EQ(page->get_num_edges(), 12);
  EXPECT_EQ(vertices[2].degree, 5); // Was 4, now 5

  // Test 8: Try to insert beyond slots for vertex 3 (should fail)
  insert_status_t status8 = page->insert_edge_in_slot(2, 3, 14);
  EXPECT_EQ(status8, SLOT_FULL);
  EXPECT_EQ(page->get_num_edges(), 12); // No change
  EXPECT_EQ(vertices[2].degree, 5);     // No change

  // Test 9: Test with invalid vertex offset
  insert_status_t status9 = page->insert_edge_in_slot(10, 1, 15);
  EXPECT_EQ(status9, FAILED);

  // Test 10: Test with vertex ID mismatch
  insert_status_t status10 = page->insert_edge_in_slot(0, 999, 16);
  EXPECT_EQ(status10, FAILED);

  std::cout << "All slot insertion tests passed!" << std::endl;
  std::cout << "Final edge count: " << page->get_num_edges() << std::endl;
  std::cout << "Vertex 1 degree: " << vertices[0].degree << std::endl;
  std::cout << "Vertex 2 degree: " << vertices[1].degree << std::endl;
  std::cout << "Vertex 3 degree: " << vertices[2].degree << std::endl;
}
