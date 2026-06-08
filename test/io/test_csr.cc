#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "test.h"

#include <cstdint>
#include <gtest/gtest.h>

// Constructor cases.

#ifdef BW_GRAPH_ENABLE_TRANSACTION
// Per-edge timestamp tests (txn mode only)
TEST(CSREdgeTimestampTest, InitialTimestampsAreInvalidTs) {
  // All edges built from an adj map start with INVALID_TS (always visible).
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  auto [graph_view, graph] = create_test_graph();
  csr_page_t csr_page(0, graph_view, vertex_map);

  vertex_span_t vertices = csr_page.get_vertices();
  for (uint16_t vi = 0; vi < static_cast<uint16_t>(vertices.size()); ++vi) {
    uint32_t degree = vertices[vi].degree;
    if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND)
      continue;
    for (uint32_t j = 0; j < degree; ++j) {
      EXPECT_EQ(csr_page.get_edge_ts(vi, j), INVALID_TS)
          << "vertex offset " << vi << " neighbor " << j << " should be INVALID_TS";
    }
  }
}

// Test CSREdgeTimestampTest.write and read timestamps.
TEST(CSREdgeTimestampTest, WriteAndReadTimestamps) {
  // Build a simple graph, write specific timestamps, read them back.
  adj_map_t graph = {{1u, {2u, 3u}}, {2u, {1u}}, {3u, {1u, 4u}}, {4u, {3u}}};
  adj_map_iter_t view = create_neighbor_span_view(graph);
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  csr_page_t csr(0, view, vertex_map);

  // Assign commit timestamps: vertex 1 -> neighbors sorted {2,3} -> ts {10,20}
  edge_ts_map_t ts_map;
  ts_map[1u] = {10u, 20u}; // neighbors are sorted: 2,3
  ts_map[2u] = {30u};      // neighbor: 1
  ts_map[3u] = {40u, 50u}; // neighbors sorted: 1,4
  ts_map[4u] = {60u};      // neighbor: 3

  csr.write_edge_timestamps(ts_map);

  // Build offset->vertex_id reverse map so we can look up by offset
  std::unordered_map<v_id_t, uint16_t> vid_to_offset;
  for (auto& [vid, off] : vertex_map)
    vid_to_offset[vid] = off;

  // Verify
  auto check = [&](v_id_t vid, uint32_t j, timestamp_t expected) {
    uint16_t off = vid_to_offset.at(vid);
    EXPECT_EQ(csr.get_edge_ts(off, j), expected) << "v=" << vid << " j=" << j;
  };

  check(1u, 0u, 10u);
  check(1u, 1u, 20u);
  check(2u, 0u, 30u);
  check(3u, 0u, 40u);
  check(3u, 1u, 50u);
  check(4u, 0u, 60u);
}

// Test CSREdgeTimestampTest.timestamps survive move parse.
TEST(CSREdgeTimestampTest, TimestampsSurviveMoveParse) {
  // Build CSR, write timestamps, copy raw data to a new page and re-parse it.
  adj_map_t graph = {{10u, {20u, 30u}}, {20u, {10u}}, {30u, {10u}}};
  adj_map_iter_t view = create_neighbor_span_view(graph);
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  csr_page_t original(42, view, vertex_map);

  edge_ts_map_t ts_map;
  ts_map[10u] = {100u, 200u};
  ts_map[20u] = {300u};
  ts_map[30u] = {400u};
  original.write_edge_timestamps(ts_map);

  // Copy raw page bytes and parse into a fresh csr_page_t.
  page_t raw(42);
  std::memcpy(raw.get_data(), original.get_data(), bw_graph::BW_GRAPH_PAGE_SIZE);
  csr_page_t parsed(std::move(raw));

  // Re-build offset map from parsed page
  std::unordered_map<v_id_t, uint16_t> off_map;
  for (auto& [vid, off] : vertex_map)
    off_map[vid] = off;

  EXPECT_EQ(parsed.get_edge_ts(off_map[10u], 0u), 100u);
  EXPECT_EQ(parsed.get_edge_ts(off_map[10u], 1u), 200u);
  EXPECT_EQ(parsed.get_edge_ts(off_map[20u], 0u), 300u);
  EXPECT_EQ(parsed.get_edge_ts(off_map[30u], 0u), 400u);
}

// Test CSREdgeTimestampTest.out of bounds return invalid ts.
TEST(CSREdgeTimestampTest, OutOfBoundsReturnInvalidTs) {
  adj_map_t graph = {{1u, {2u}}};
  adj_map_iter_t view = create_neighbor_span_view(graph);
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  csr_page_t csr(0, view, vertex_map);

  // Out-of-bounds vertex offset -> INVALID_TS
  EXPECT_EQ(csr.get_edge_ts(999u, 0u), INVALID_TS);
  // Out-of-bounds neighbor index has undefined but safe behaviour (no crash).
}
#endif // BW_GRAPH_ENABLE_TRANSACTION

// Test CSRConstructorTest.default constructor creates empty csr.
TEST(CSRConstructorTest, DefaultConstructorCreatesEmptyCSR) {
  csr_page_t csr_page;

  EXPECT_EQ(csr_page.get_num_vertices(), 0);
  EXPECT_EQ(csr_page.get_num_edges(), 0);

  // Test that get_vertices returns empty span
  vertex_span_t vertices = csr_page.get_vertices();
  EXPECT_EQ(vertices.size(), 0);
}

// Test CSRConstructorTest.default constructor page no check.
TEST(CSRConstructorTest, DefaultConstructorPageNoCheck) {
  csr_page_t csr_page;

  // Default constructor should use default page number
  EXPECT_EQ(csr_page.get_page_no(), bw_graph::BW_GRAPH_DEFAULT_PAGE_NO);

  // Verify data is properly initialized
  EXPECT_NE(csr_page.get_data(), nullptr);
}

// Test CSRConstructorTest.map constructor creates csr.
TEST(CSRConstructorTest, MapConstructorCreatesCSR) {
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  auto [graph_view, graph] = create_test_graph();
  csr_page_t csr_page(0, graph_view, vertex_map);

  EXPECT_EQ(csr_page.get_num_vertices(), 13);
  EXPECT_EQ(csr_page.get_num_edges(), 20);
  EXPECT_EQ(csr_page.get_page_no(), 0);
}

// Test CSRConstructorTest.move constructor from page.
TEST(CSRConstructorTest, MoveConstructorFromPage) {
  // Create a page with known page number
  page_t original_page(123);

  // Move construct CSR page
  csr_page_t csr_page(std::move(original_page));

  // Should inherit page number and have empty CSR data initially
  EXPECT_EQ(csr_page.get_page_no(), 123);
  EXPECT_EQ(csr_page.get_num_vertices(), 0);
  EXPECT_EQ(csr_page.get_num_edges(), 0);
}

// Test CSRConstructorTest.move constructor with data.
TEST(CSRConstructorTest, MoveConstructorWithData) {
  // First create a CSR page with data
  auto [graph_view, _] = create_test_graph();
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  csr_page_t original_csr(456, graph_view, vertex_map);

  // Create a page and copy CSR data to it
  page_t page_with_data(456);
  std::memcpy(page_with_data.get_data(), original_csr.get_data(), bw_graph::BW_GRAPH_PAGE_SIZE);

  // Move construct from the page
  csr_page_t moved_csr(std::move(page_with_data));

  // Should parse the data correctly
  EXPECT_EQ(moved_csr.get_page_no(), 456);
  EXPECT_EQ(moved_csr.get_num_vertices(), 13);
  EXPECT_EQ(moved_csr.get_num_edges(), 20);
}
