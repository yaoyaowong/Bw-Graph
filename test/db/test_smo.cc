#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/common/utils.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/io/io_index.h"
#include "bw_graph/part/page_delta.h"
#include "bw_graph/part/smo.h"
#include "bw_graph/storage/disk_manager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <set>
#include <string>

class SmoTest : public ::testing::Test {
protected:
  size_t saved_page_size{};
  size_t saved_delta_page_size{};
  uint64_t saved_max_delta_chain{};
  uint64_t saved_max_block_weight{};
  uint64_t saved_giant_degree{};
  uint64_t saved_index_page_size{};
  uint64_t saved_max_block_neighbor{};
  uint64_t saved_index_max_blocks{};

  std::filesystem::path tmp_dir;

  disk_manager_t* csr_disk_mgr{nullptr};
  disk_manager_t* delta_disk_mgr{nullptr};
  buf_pool_t<csr_page_t>* csr_pool{nullptr};
  buf_pool_t<delta_page_t>* delta_pool{nullptr};
  page_map_t* pmt{nullptr};
  vertex_index_t* vertex_index{nullptr};
  vertex_update_sketch_t* sketch{nullptr};
  smo_ctl_t* smo{nullptr};

  void SetUp() override {
    // Save global config.
    saved_page_size = bw_graph::BW_GRAPH_PAGE_SIZE;
    saved_delta_page_size = bw_graph::BW_DELTA_PAGE_SIZE;
    saved_max_delta_chain = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
    saved_max_block_weight = bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT;
    saved_giant_degree = bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND;
    saved_index_page_size = bw_graph::BW_GRAPH_INDEX_PAGE_SIZE;
    saved_max_block_neighbor = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR;
    saved_index_max_blocks = bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS;

    // Tune for deterministic tests.
    bw_graph::BW_GRAPH_PAGE_SIZE = 1024;
    bw_graph::BW_DELTA_PAGE_SIZE = 4096;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;
    bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT = 60;
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = 256;
    bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR = 16;

    // Temp directory.
    tmp_dir =
        std::filesystem::temp_directory_path() /
        ("test_smo_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp_dir);

    // Disk managers.
    csr_disk_mgr = new disk_manager_t(tmp_dir / "csr.db", bw_graph::BW_GRAPH_PAGE_SIZE);
    delta_disk_mgr = new disk_manager_t(tmp_dir / "delta.db", bw_graph::BW_DELTA_PAGE_SIZE);

    // Buffer pools.
    csr_pool = buf_pool_t<csr_page_t>::buf_pool_init(4, 256, bw_graph::BW_GRAPH_PAGE_SIZE);
    delta_pool = buf_pool_t<delta_page_t>::buf_pool_init(4, 256, bw_graph::BW_DELTA_PAGE_SIZE);

    pmt = new page_map_t(delta_pool, delta_disk_mgr);
    vertex_index = new vertex_index_t(512);
    sketch = new vertex_update_sketch_t(512);
    // Single worker for deterministic behavior.
    smo = new smo_ctl_t(1);
  }

  void TearDown() override {
    delete smo;
    delete sketch;
    delete vertex_index;
    delete pmt;
    delete csr_pool;
    delete delta_pool;
    delete csr_disk_mgr;
    delete delta_disk_mgr;
    std::filesystem::remove_all(tmp_dir);

    // Restore global config.
    bw_graph::BW_GRAPH_PAGE_SIZE = saved_page_size;
    bw_graph::BW_DELTA_PAGE_SIZE = saved_delta_page_size;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = saved_max_delta_chain;
    bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT = saved_max_block_weight;
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = saved_giant_degree;
    bw_graph::BW_GRAPH_INDEX_PAGE_SIZE = saved_index_page_size;
    bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR = saved_max_block_neighbor;
    bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS = saved_index_max_blocks;
  }

  // Build and install one CSR page.
  std::pair<csr_page_t*, page_no_t> make_csr_page(adj_map_t graph, uint16_t slot_count = 0) {
    size_t byte_off = csr_disk_mgr->allocate_page();
    page_no_t pno = static_cast<page_no_t>(byte_off / bw_graph::BW_GRAPH_PAGE_SIZE);

    adj_map_iter_t view = create_neighbor_span_view(graph);
    std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
    csr_page_t tmp(pno, view, vertex_map, slot_count);

    csr_disk_mgr->write_page(pno, tmp.get_data());
    csr_page_t* frame = csr_pool->buf_page_cp(pno, tmp.get_data(), csr_disk_mgr);
    EXPECT_NE(frame, nullptr) << "buf_page_cp returned nullptr for page_no=" << pno;

    for (auto& [v_id, off] : vertex_map) {
      vertex_index->update_vertex_loc(v_id, pno, off);
    }
    return {frame, pno};
  }

  // Read and sort neighbors for one vertex.
  std::vector<v_id_t> read_neighbors(v_id_t v_id) {
    vertex_loc_t loc = vertex_index->get_vertex_location(v_id);
    csr_page_t* page = csr_pool->buf_page_read(loc.first, csr_disk_mgr);
    neighbor_span_t span = page->get_neighbors(static_cast<uint16_t>(loc.second));
    std::vector<v_id_t> result(span.begin(), span.end());
    page->r_unlatch();
    std::sort(result.begin(), result.end());
    return result;
  }

  // Append one edge delta for src_page.
  void push_edge_delta(page_no_t src_page, v_id_t src, v_id_t dst, delta_type_t type = INSERT) {
    delta_record_t d(EDGE, type, src, dst);
    pmt->insert_delta(src_page, {d});
  }
};

static index_page_t* make_index_page(page_no_t idx_page_no,
                                     const std::vector<page_no_t>& csr_page_nos) {
  block_adj_map_t adj;
  for (page_no_t p : csr_page_nos) {
    adj[p] = {};
  }
  return new index_page_t(idx_page_no, adj);
}

// Test SmoTest.no split vertex index updated.
TEST_F(SmoTest, NoSplit_VertexIndexUpdated) {
  adj_map_t initial;
  initial[0] = {1, 2};
  initial[1] = {0, 2};
  initial[2] = {0, 3};
  initial[3] = {2, 4};
  initial[4] = {3};
  auto [old_frame, old_pno] = make_csr_page(initial);

  for (v_id_t v = 0; v <= 4; ++v) {
    EXPECT_EQ(vertex_index->get_vertex_location(v).first, old_pno)
        << "vertex " << v << " should start on page " << old_pno;
  }

  push_edge_delta(old_pno, 0, 3);
  push_edge_delta(old_pno, 1, 4);
  push_edge_delta(old_pno, 2, 4);

  ASSERT_GE(pmt->get_delta_chain_length(old_pno), 1u);

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  for (v_id_t v = 0; v <= 4; ++v) {
    EXPECT_NE(vertex_index->get_vertex_location(v).first, old_pno)
        << "CoW violated: vertex " << v << " still points to old page " << old_pno;
  }

  page_no_t new_pno = vertex_index->get_vertex_location(0).first;
  for (v_id_t v = 1; v <= 4; ++v) {
    EXPECT_EQ(vertex_index->get_vertex_location(v).first, new_pno)
        << "no-split should keep all vertices on one new page";
  }

  auto nbrs0 = read_neighbors(0);
  EXPECT_NE(std::find(nbrs0.begin(), nbrs0.end(), 1u), nbrs0.end())
      << "original edge v0→v1 must survive consolidation";
  EXPECT_NE(std::find(nbrs0.begin(), nbrs0.end(), 2u), nbrs0.end())
      << "original edge v0→v2 must survive consolidation";

  EXPECT_NE(std::find(nbrs0.begin(), nbrs0.end(), 3u), nbrs0.end())
      << "delta edge v0→v3 must be visible after consolidation";

  auto nbrs1 = read_neighbors(1);
  EXPECT_NE(std::find(nbrs1.begin(), nbrs1.end(), 4u), nbrs1.end())
      << "delta edge v1→v4 must be visible after consolidation";

  auto nbrs2 = read_neighbors(2);
  EXPECT_NE(std::find(nbrs2.begin(), nbrs2.end(), 4u), nbrs2.end())
      << "delta edge v2→v4 must be visible after consolidation";
}

TEST_F(SmoTest, ManualVersionSwitchPublishesOnlyOnDemand) {
  delete smo;
  smo = new smo_ctl_t(1, true);

  adj_map_t initial;
  initial[0] = {1};
  initial[1] = {0};
  auto [old_frame, old_pno] = make_csr_page(initial);

  push_edge_delta(old_pno, 0, 2);
  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  EXPECT_EQ(vertex_index->get_vertex_location(0).first, old_pno);
  EXPECT_EQ(read_neighbors(0), std::vector<v_id_t>({1}));

  auto forwarder = pmt->acquire_forwarder_for(old_pno);
  ASSERT_NE(forwarder, nullptr);
  page_no_t staged_pno = old_pno;
  ASSERT_TRUE(forwarder->lookup_forward(0, staged_pno));
  EXPECT_NE(staged_pno, old_pno);

  smo->publish_pending_versions();

  EXPECT_EQ(vertex_index->get_vertex_location(0).first, staged_pno);
  EXPECT_EQ(read_neighbors(0), std::vector<v_id_t>({1, 2}));
  EXPECT_EQ(pmt->acquire_forwarder_for(old_pno), nullptr);

  page_no_t recycled_pno =
      static_cast<page_no_t>(csr_disk_mgr->allocate_page() / bw_graph::BW_GRAPH_PAGE_SIZE);
  EXPECT_EQ(recycled_pno, old_pno);
}

// Test SmoTest.no split edge delete applied.
TEST_F(SmoTest, NoSplit_EdgeDeleteApplied) {
  adj_map_t g;
  g[10] = {11, 12, 13};
  g[11] = {};
  g[12] = {};
  g[13] = {};

  auto [old_frame, old_pno] = make_csr_page(g);

  // Add one delete and one insert to trigger consolidation.
  push_edge_delta(old_pno, 10, 12, DELETE);
  push_edge_delta(old_pno, 10, 14, INSERT);

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  auto nbrs10 = read_neighbors(10);
  EXPECT_EQ(std::find(nbrs10.begin(), nbrs10.end(), 12u), nbrs10.end())
      << "deleted edge v10→v12 must not appear after consolidation";
  EXPECT_NE(std::find(nbrs10.begin(), nbrs10.end(), 11u), nbrs10.end())
      << "v10→v11 must still exist";
  EXPECT_NE(std::find(nbrs10.begin(), nbrs10.end(), 13u), nbrs10.end())
      << "v10→v13 must still exist";
  EXPECT_NE(std::find(nbrs10.begin(), nbrs10.end(), 14u), nbrs10.end())
      << "inserted edge v10→v14 must be visible after consolidation";
}

// Test SmoTest.guard skips when no delta.
TEST_F(SmoTest, Guard_SkipsWhenNoDelta) {
  adj_map_t g;
  g[20] = {21};
  g[21] = {};

  auto [old_frame, old_pno] = make_csr_page(g);

  EXPECT_EQ(pmt->get_delta_chain_length(old_pno), 0u) << "sanity: no delta pushed yet";

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  EXPECT_EQ(vertex_index->get_vertex_location(20).first, old_pno)
      << "guard must skip consolidation; vertex 20 still on old page";
  EXPECT_EQ(vertex_index->get_vertex_location(21).first, old_pno)
      << "guard must skip consolidation; vertex 21 still on old page";
}

// Force split by pushing many new neighbors.
TEST_F(SmoTest, Split_VertexIndexRedirected) {
  adj_map_t initial;
  for (v_id_t v = 100; v < 105; ++v) {
    for (v_id_t nb = 200; nb < 205; ++nb) {
      initial[v].push_back(nb);
    }
  }
  auto [old_frame, old_pno] = make_csr_page(initial);

  for (v_id_t v = 100; v < 105; ++v) {
    for (v_id_t nb = 205; nb < 250; ++nb) {
      push_edge_delta(old_pno, v, nb);
    }
  }

  ASSERT_GE(pmt->get_delta_chain_length(old_pno), 1u)
      << "delta chain must be non-empty before consolidation";

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  for (v_id_t v = 100; v < 105; ++v) {
    page_no_t new_pno = vertex_index->get_vertex_location(v).first;
    EXPECT_NE(new_pno, old_pno) << "CoW violated: vertex " << v << " still on old page " << old_pno;
  }

  for (v_id_t v = 100; v < 105; ++v) {
    auto nbrs = read_neighbors(v);
    for (v_id_t nb = 200; nb < 205; ++nb) {
      EXPECT_NE(std::find(nbrs.begin(), nbrs.end(), nb), nbrs.end())
          << "original neighbour " << nb << " of vertex " << v << " missing after consolidation";
    }
  }

  std::set<page_no_t> page_nos;
  for (v_id_t v = 100; v < 105; ++v) {
    page_nos.insert(vertex_index->get_vertex_location(v).first);
  }
  std::cout << "[split] vertices 100-104 distributed across " << page_nos.size()
            << " distinct page(s)\n";
}

// Test SmoTest.no split idempotent after consolidation.
TEST_F(SmoTest, NoSplit_IdempotentAfterConsolidation) {
  adj_map_t g;
  g[30] = {31, 32};
  g[31] = {30};
  g[32] = {30};

  auto [old_frame, old_pno] = make_csr_page(g);

  push_edge_delta(old_pno, 30, 33);
  push_edge_delta(old_pno, 31, 33);

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  page_no_t first_new_pno = vertex_index->get_vertex_location(30).first;
  EXPECT_NE(first_new_pno, old_pno) << "first consolidation must redirect vertex 30 (CoW)";

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  EXPECT_EQ(vertex_index->get_vertex_location(30).first, first_new_pno)
      << "second (skipped) consolidation must not change vertex index";
}

// Test SmoTest.no split delta chain reset after consolidation.
TEST_F(SmoTest, NoSplit_DeltaChainResetAfterConsolidation) {
  adj_map_t g;
  g[40] = {41};
  g[41] = {};

  auto [old_frame, old_pno] = make_csr_page(g);

  push_edge_delta(old_pno, 40, 42);
  push_edge_delta(old_pno, 40, 43);

  EXPECT_GE(pmt->get_delta_chain_length(old_pno), 1u)
      << "delta chain should be non-empty before consolidation";

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  EXPECT_EQ(pmt->get_delta_chain_length(old_pno), 0u)
      << "delta chain for old page must be reset to 0 after consolidation";
}

// Test SmoTest.index page updated after csrsplit.
TEST_F(SmoTest, IndexPageUpdatedAfterCSRSplit) {
  adj_map_t initial;
  for (v_id_t v = 0; v < 5; ++v) {
    for (v_id_t nb = 100; nb < 105; ++nb) {
      initial[v].push_back(nb);
    }
  }
  auto [old_frame, old_pno] = make_csr_page(initial);

  const page_no_t idx_pno = 9000;
  folly::F14FastMap<page_no_t, index_page_t*> index_pages;
  index_pages[idx_pno] = make_index_page(idx_pno, {old_pno});

  old_frame->set_parent_page_no(idx_pno);
  index_page_t* root_index_page = index_pages[idx_pno];
  smo->set_index_page_map(&index_pages, &root_index_page, 8999);

  for (v_id_t v = 0; v < 5; ++v) {
    for (v_id_t nb = 105; nb < 150; ++nb) {
      push_edge_delta(old_pno, v, nb);
    }
  }
  ASSERT_GE(pmt->get_delta_chain_length(old_pno), 1u);

  smo->consolidate_pages(old_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  std::set<page_no_t> new_pnos;
  for (v_id_t v = 0; v < 5; ++v) {
    new_pnos.insert(vertex_index->get_vertex_location(v).first);
  }
  ASSERT_GT(new_pnos.size(), 1u) << "test setup must force a CSR split into multiple pages";
  EXPECT_EQ(new_pnos.count(old_pno), 0u)
      << "CoW violated: old CSR page is still referenced by vertex_index";

  ASSERT_EQ(index_pages.count(idx_pno), 1u) << "parent index page entry disappeared";
  block_adj_map_t updated_adj = index_pages[idx_pno]->get_block_adj_map();
  EXPECT_EQ(updated_adj.count(old_pno), 0u) << "old CSR page still referenced in parent index page";
  for (page_no_t p : new_pnos) {
    EXPECT_EQ(updated_adj.count(p), 1u)
        << "new CSR page " << p << " missing from parent index page";
  }

  for (auto& [_, page] : index_pages) {
    delete page;
  }
}

// Test SmoTest.index page split on overflow.
TEST_F(SmoTest, IndexPageSplitOnOverflow) {
  bw_graph::BW_GRAPH_INDEX_PAGE_SIZE = 256;
  bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS = calculate_bw_capacity(
      bw_graph::BW_GRAPH_INDEX_PAGE_SIZE, bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);

  const size_t threshold = bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS * 6 / 10;
  ASSERT_GE(threshold, 2u) << "test setup needs room for existing pages plus the split page";
  ASSERT_LT(threshold, bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS)
      << "60% split threshold must leave index-page slack";

  const page_no_t idx_pno = 9000;
  folly::F14FastMap<page_no_t, index_page_t*> index_pages;
  std::vector<page_no_t> indexed_csr_pages;

  for (size_t i = 0; i < threshold - 1; ++i) {
    v_id_t base = static_cast<v_id_t>(300 + i * 2);
    adj_map_t g;
    g[base] = {base + 1};
    g[base + 1] = {};
    auto [frame, pno] = make_csr_page(g);
    frame->set_parent_page_no(idx_pno);
    indexed_csr_pages.push_back(pno);
  }

  adj_map_t split_graph;
  for (v_id_t v = 200; v < 205; ++v) {
    for (v_id_t nb = 400; nb < 405; ++nb) {
      split_graph[v].push_back(nb);
    }
  }
  auto [split_frame, split_pno] = make_csr_page(split_graph);
  split_frame->set_parent_page_no(idx_pno);
  indexed_csr_pages.push_back(split_pno);

  index_pages[idx_pno] = make_index_page(idx_pno, indexed_csr_pages);
  index_page_t* root_index_page = index_pages[idx_pno];
  smo->set_index_page_map(&index_pages, &root_index_page, 8999);

  for (v_id_t v = 200; v < 205; ++v) {
    for (v_id_t nb = 405; nb < 450; ++nb) {
      push_edge_delta(split_pno, v, nb);
    }
  }
  ASSERT_GE(pmt->get_delta_chain_length(split_pno), 1u);

  smo->consolidate_pages(split_frame, pmt, csr_disk_mgr, csr_pool, vertex_index, nullptr, sketch);

  std::set<page_no_t> new_pnos;
  for (v_id_t v = 200; v < 205; ++v) {
    new_pnos.insert(vertex_index->get_vertex_location(v).first);
  }
  ASSERT_GT(new_pnos.size(), 1u) << "test setup must force a CSR split into multiple pages";

  EXPECT_EQ(index_pages.count(idx_pno), 0u)
      << "overflowing index page should be replaced by split pages";

  bool found_old_csr = false;
  std::set<page_no_t> indexed_new_pnos;
  for (auto& [_, page] : index_pages) {
    block_adj_map_t adj = page->get_block_adj_map();
    found_old_csr = found_old_csr || adj.count(split_pno) > 0;
    for (page_no_t p : new_pnos) {
      if (adj.count(p) > 0) {
        indexed_new_pnos.insert(p);
      }
    }
  }

  EXPECT_FALSE(found_old_csr) << "old split CSR page still referenced after index-page split";
  EXPECT_EQ(indexed_new_pnos, new_pnos)
      << "index-page split did not preserve all new CSR page references";
  ASSERT_NE(root_index_page, nullptr);
  EXPECT_NE(root_index_page->get_page_no(), idx_pno)
      << "root pointer should move away from the replaced index page";

  vertex_index->wait_for_version_gc();
  size_t reused_offset = csr_disk_mgr->allocate_page();
  page_no_t reused_pno = static_cast<page_no_t>(reused_offset / bw_graph::BW_GRAPH_PAGE_SIZE);
  EXPECT_EQ(reused_pno, split_pno)
      << "old split CSR page should be retired into the disk free-slot list";

  for (auto& [_, page] : index_pages) {
    delete page;
  }
}
