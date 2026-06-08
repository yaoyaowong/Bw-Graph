#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/builder.h"
#include "bw_graph/io/io_csr.h"

#include <cstdint>
#include <folly/container/F14Map.h>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

// Exercise the per-vertex slot reservation path on csr_page_t directly so
// that we can verify the physical layout regardless of the higher-level
// SMO machinery. Works under default / --compress / --txn builds.
class PerVertexSlotTest : public ::testing::Test {
protected:
  size_t saved_page_size{};

  void SetUp() override {
    saved_page_size = bw_graph::BW_GRAPH_PAGE_SIZE;
    bw_graph::BW_GRAPH_PAGE_SIZE = 4096;
  }

  void TearDown() override { bw_graph::BW_GRAPH_PAGE_SIZE = saved_page_size; }

  // Build a small graph: 4 vertices, each with degree 3.
  static adj_map_t build_small_graph() {
    adj_map_t graph;
    graph.emplace(1u, std::vector<v_id_t>{10, 11, 12});
    graph.emplace(2u, std::vector<v_id_t>{20, 21, 22});
    graph.emplace(3u, std::vector<v_id_t>{30, 31, 32});
    graph.emplace(4u, std::vector<v_id_t>{40, 41, 42});
    return graph;
  }
};

// Empty per-vertex map must behave identically to slot == 0.
TEST_F(PerVertexSlotTest, EmptyMapEqualsZeroSlots) {
  adj_map_t graph_a = build_small_graph();
  adj_map_t graph_b = build_small_graph();

  adj_map_iter_t view_a = create_neighbor_span_view(graph_a);
  adj_map_iter_t view_b = create_neighbor_span_view(graph_b);

  std::vector<std::pair<v_id_t, uint16_t>> vm_a;
  std::vector<std::pair<v_id_t, uint16_t>> vm_b;

  csr_page_t page_uniform(1u, view_a, vm_a, static_cast<uint16_t>(0));

  folly::F14FastMap<v_id_t, uint16_t> empty;
  csr_page_t page_per_vertex(2u, view_b, vm_b, empty);

  EXPECT_EQ(page_uniform.get_used_bytes(), page_per_vertex.get_used_bytes());
  EXPECT_EQ(page_uniform.get_num_vertices(), page_per_vertex.get_num_vertices());
  EXPECT_EQ(page_uniform.get_num_edges(), page_per_vertex.get_num_edges());
}

// A page with one write-heavy vertex (slot=4) and three read-only vertices
// (slot=0) must use strictly more bytes than the all-zero-slot page, but
// strictly less than a page where every vertex gets slot=4.
TEST_F(PerVertexSlotTest, MixedReservationFitsBetweenUniformExtremes) {
  adj_map_t graph_a = build_small_graph();
  adj_map_t graph_b = build_small_graph();
  adj_map_t graph_c = build_small_graph();

  adj_map_iter_t view_a = create_neighbor_span_view(graph_a);
  adj_map_iter_t view_b = create_neighbor_span_view(graph_b);
  adj_map_iter_t view_c = create_neighbor_span_view(graph_c);

  std::vector<std::pair<v_id_t, uint16_t>> vm_a, vm_b, vm_c;

  // Uniform 0 slots.
  csr_page_t page_zero(10u, view_a, vm_a, static_cast<uint16_t>(0));

  // Mixed: vertex 1 reserves 4, the rest 0.
  folly::F14FastMap<v_id_t, uint16_t> mixed;
  mixed.emplace(1u, 4);
  csr_page_t page_mixed(11u, view_b, vm_b, mixed);

  // Uniform 4 slots.
  csr_page_t page_full(12u, view_c, vm_c, static_cast<uint16_t>(4));

  EXPECT_LT(page_zero.get_used_bytes(), page_mixed.get_used_bytes());
  EXPECT_LT(page_mixed.get_used_bytes(), page_full.get_used_bytes());

  // Vertex count and edge count are independent of slot reservation.
  EXPECT_EQ(page_zero.get_num_vertices(), page_mixed.get_num_vertices());
  EXPECT_EQ(page_zero.get_num_edges(), page_mixed.get_num_edges());
}

// Verify per-vertex slot reservation through csr_page_builder_t's overload
// (covers the SMO split path).
TEST_F(PerVertexSlotTest, BuilderPerVertexSlotOverloadIsHonoured) {
  // Pick a generous fallback so add_vertex() without explicit slot count
  // still goes through cleanly.
  csr_page_builder_t builder(bw_graph::BW_GRAPH_PAGE_SIZE, 0);

  std::vector<v_id_t> n1{10, 11, 12};
  std::vector<v_id_t> n2{20, 21, 22};
  std::vector<v_id_t> n3{30, 31, 32};

  neighbor_span_t s1(n1);
  neighbor_span_t s2(n2);
  neighbor_span_t s3(n3);

  ASSERT_TRUE(builder.add_vertex(1u, s1, /*per_vertex_slots=*/0));
  ASSERT_TRUE(builder.add_vertex(2u, s2, /*per_vertex_slots=*/4));
  ASSERT_TRUE(builder.add_vertex(3u, s3, /*per_vertex_slots=*/2));

  auto [page, vertex_map] = builder.build();
  ASSERT_NE(page, nullptr);

  // Used bytes should equal: metadata + neighbor_block + 3 * sizeof(csr_vertex_t)
  // + neighbor data section.  We do not pin down the exact value here -
  // the cross-mode invariants we verify below are enough.
  EXPECT_EQ(page->get_num_vertices(), 3u);
  EXPECT_EQ(page->get_num_edges(), 9u);

  // Compare against a builder where everybody has 0 slots: the mixed page
  // must use strictly more bytes.
  csr_page_builder_t baseline(bw_graph::BW_GRAPH_PAGE_SIZE, 0);
  std::vector<v_id_t> b1 = n1, b2 = n2, b3 = n3;
  neighbor_span_t bs1(b1), bs2(b2), bs3(b3);
  ASSERT_TRUE(baseline.add_vertex(1u, bs1, 0));
  ASSERT_TRUE(baseline.add_vertex(2u, bs2, 0));
  ASSERT_TRUE(baseline.add_vertex(3u, bs3, 0));
  auto [baseline_page, _bmap] = baseline.build();

  EXPECT_GT(page->get_used_bytes(), baseline_page->get_used_bytes());

  delete page;
  delete baseline_page;
}

// When a single vertex gets slot=N, the per-vertex variant must allocate
// exactly the same number of trailing slot bytes as the uniform N path
// would have allocated for that one vertex (4 bytes per slot in compress
// mode, sizeof(v_id_t) per slot otherwise). We test the parity by:
//   - building page X with one vertex with per-vertex slot N
//   - building page Y with the same single vertex with uniform slot N
// They must be byte-identical.
TEST_F(PerVertexSlotTest, SingleVertexPerVertexEqualsSingleVertexUniform) {
  adj_map_t graph_a;
  adj_map_t graph_b;
  graph_a.emplace(7u, std::vector<v_id_t>{100, 101, 102, 103, 104});
  graph_b.emplace(7u, std::vector<v_id_t>{100, 101, 102, 103, 104});

  adj_map_iter_t view_a = create_neighbor_span_view(graph_a);
  adj_map_iter_t view_b = create_neighbor_span_view(graph_b);

  std::vector<std::pair<v_id_t, uint16_t>> vm_a;
  std::vector<std::pair<v_id_t, uint16_t>> vm_b;

  csr_page_t page_uniform(0u, view_a, vm_a, static_cast<uint16_t>(3));

  folly::F14FastMap<v_id_t, uint16_t> per_vertex;
  per_vertex.emplace(7u, 3);
  csr_page_t page_per_vertex(0u, view_b, vm_b, per_vertex);

  EXPECT_EQ(page_uniform.get_used_bytes(), page_per_vertex.get_used_bytes());
  EXPECT_EQ(page_uniform.get_num_vertices(), page_per_vertex.get_num_vertices());
  EXPECT_EQ(page_uniform.get_num_edges(), page_per_vertex.get_num_edges());
}
