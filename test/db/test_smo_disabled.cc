#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/part/smo.h"
#include "bw_graph/storage/disk_manager.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <string>

// Fixture mirroring test_smo.cc but parameterised on the SMO worker count
// so we can exercise both the disabled path (workers == 0) and the regular
// path (workers > 0) from a single suite.
class SmoToggleTest : public ::testing::Test {
protected:
  size_t saved_page_size{};
  size_t saved_delta_page_size{};
  uint64_t saved_max_delta_chain{};
  uint64_t saved_max_block_weight{};
  uint64_t saved_giant_degree{};
  uint64_t saved_max_block_neighbor{};

  std::filesystem::path tmp_dir;

  disk_manager_t* csr_disk_mgr{nullptr};
  disk_manager_t* delta_disk_mgr{nullptr};
  buf_pool_t<csr_page_t>* csr_pool{nullptr};
  buf_pool_t<delta_page_t>* delta_pool{nullptr};
  page_map_t* pmt{nullptr};
  vertex_index_t* vertex_index{nullptr};
  vertex_update_sketch_t* sketch{nullptr};

  void SetUp() override {
    saved_page_size = bw_graph::BW_GRAPH_PAGE_SIZE;
    saved_delta_page_size = bw_graph::BW_DELTA_PAGE_SIZE;
    saved_max_delta_chain = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
    saved_max_block_weight = bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT;
    saved_giant_degree = bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND;
    saved_max_block_neighbor = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR;

    bw_graph::BW_GRAPH_PAGE_SIZE = 1024;
    bw_graph::BW_DELTA_PAGE_SIZE = 4096;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;
    bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT = 60;
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = 256;
    bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR = 16;

    tmp_dir = std::filesystem::temp_directory_path() /
              ("test_smo_toggle_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp_dir);

    csr_disk_mgr = new disk_manager_t(tmp_dir / "csr.db", bw_graph::BW_GRAPH_PAGE_SIZE);
    delta_disk_mgr = new disk_manager_t(tmp_dir / "delta.db", bw_graph::BW_DELTA_PAGE_SIZE);

    csr_pool = buf_pool_t<csr_page_t>::buf_pool_init(4, 256, bw_graph::BW_GRAPH_PAGE_SIZE);
    delta_pool = buf_pool_t<delta_page_t>::buf_pool_init(4, 256, bw_graph::BW_DELTA_PAGE_SIZE);

    pmt = new page_map_t(delta_pool, delta_disk_mgr);
    vertex_index = new vertex_index_t(512);
    sketch = new vertex_update_sketch_t(512);
  }

  void TearDown() override {
    delete sketch;
    delete vertex_index;
    delete pmt;
    delete csr_pool;
    delete delta_pool;
    delete csr_disk_mgr;
    delete delta_disk_mgr;
    std::filesystem::remove_all(tmp_dir);

    bw_graph::BW_GRAPH_PAGE_SIZE = saved_page_size;
    bw_graph::BW_DELTA_PAGE_SIZE = saved_delta_page_size;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = saved_max_delta_chain;
    bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT = saved_max_block_weight;
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = saved_giant_degree;
    bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR = saved_max_block_neighbor;
  }

  // Build and install one CSR page; returns (frame, page_no).
  std::pair<csr_page_t*, page_no_t> make_csr_page(adj_map_t graph) {
    size_t byte_off = csr_disk_mgr->allocate_page();
    page_no_t pno = static_cast<page_no_t>(byte_off / bw_graph::BW_GRAPH_PAGE_SIZE);

    adj_map_iter_t view = create_neighbor_span_view(graph);
    std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
    csr_page_t tmp(pno, view, vertex_map, 0);

    csr_disk_mgr->write_page(pno, tmp.get_data());
    csr_page_t* frame = csr_pool->buf_page_cp(pno, tmp.get_data(), csr_disk_mgr);
    EXPECT_NE(frame, nullptr) << "buf_page_cp returned nullptr for page_no=" << pno;

    for (auto& [v_id, off] : vertex_map) {
      vertex_index->update_vertex_loc(v_id, pno, off);
    }
    return {frame, pno};
  }

  // Append one edge delta against src_page.
  void push_edge_delta(page_no_t src_page, v_id_t src, v_id_t dst,
                       delta_type_t type = INSERT) {
    delta_record_t d(EDGE, type, src, dst);
    pmt->insert_delta(src_page, {d});
  }
};

// Worker count == 0 must report the controller as disabled.
TEST_F(SmoToggleTest, DisabledFlag_WhenWorkerCountIsZero) {
  smo_ctl_t smo(0);
  EXPECT_FALSE(smo.is_enabled());
  EXPECT_EQ(smo.get_worker_count(), 0u);
  EXPECT_FALSE(smo.has_pending_consolidations());
}

// Positive worker count must report the controller as enabled.
TEST_F(SmoToggleTest, EnabledFlag_WhenWorkerCountIsPositive) {
  smo_ctl_t smo(2);
  EXPECT_TRUE(smo.is_enabled());
  EXPECT_EQ(smo.get_worker_count(), 2u);
}

// Disabled controller must leave the vertex index and delta chain untouched.
TEST_F(SmoToggleTest, Disabled_SyncConsolidateIsNoOp) {
  smo_ctl_t smo(0);

  adj_map_t g;
  g[0] = {1};
  g[1] = {};
  auto [frame, old_pno] = make_csr_page(g);

  push_edge_delta(old_pno, 0, 2);
  push_edge_delta(old_pno, 0, 3);
  ASSERT_GE(pmt->get_delta_chain_length(old_pno), 1u);

  smo.consolidate_pages(frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  // Vertex location must not have moved.
  EXPECT_EQ(vertex_index->get_vertex_location(0).first, old_pno);
  EXPECT_EQ(vertex_index->get_vertex_location(1).first, old_pno);
  // Delta chain must still hold the queued records.
  EXPECT_GE(pmt->get_delta_chain_length(old_pno), 1u);
}

// Disabled controller must return an immediately-ready future from the async
// entry point and never enqueue work onto a (non-existent) arena.
TEST_F(SmoToggleTest, Disabled_AsyncConsolidateReturnsReadyFuture) {
  smo_ctl_t smo(0);

  adj_map_t g;
  g[10] = {11};
  g[11] = {};
  auto [frame, old_pno] = make_csr_page(g);

  push_edge_delta(old_pno, 10, 12);
  push_edge_delta(old_pno, 10, 13);
  ASSERT_GE(pmt->get_delta_chain_length(old_pno), 1u);

  std::future<void> fut = smo.consolidate_pages_async(frame, pmt, csr_disk_mgr, csr_pool,
                                                     vertex_index, nullptr, sketch);

  ASSERT_EQ(fut.wait_for(std::chrono::seconds(0)), std::future_status::ready)
      << "disabled async path must return a ready future without scheduling work";
  fut.get();

  EXPECT_FALSE(smo.has_pending_consolidations());
  EXPECT_EQ(vertex_index->get_vertex_location(10).first, old_pno);
  EXPECT_GE(pmt->get_delta_chain_length(old_pno), 1u);
}

// wait_all_consolidations must be safe even when no arena/task_group exists.
TEST_F(SmoToggleTest, Disabled_WaitAllIsSafe) {
  smo_ctl_t smo(0);
  smo.wait_all_consolidations();
  smo.wait_all_consolidations();
  SUCCEED();
}

// Sanity: when enabled, the controller still consolidates (regression guard
// for the toggle change).
TEST_F(SmoToggleTest, Enabled_StillConsolidates) {
  smo_ctl_t smo(1);

  adj_map_t g;
  g[20] = {21, 22};
  g[21] = {20};
  g[22] = {20};
  auto [frame, old_pno] = make_csr_page(g);

  push_edge_delta(old_pno, 20, 23);
  push_edge_delta(old_pno, 21, 24);
  ASSERT_GE(pmt->get_delta_chain_length(old_pno), 1u);

  smo.consolidate_pages(frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  EXPECT_NE(vertex_index->get_vertex_location(20).first, old_pno)
      << "enabled controller must redirect vertex 20 (CoW)";
  EXPECT_EQ(pmt->get_delta_chain_length(old_pno), 0u)
      << "enabled controller must drain the old page's delta chain";
}
