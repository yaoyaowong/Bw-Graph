#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/part/page_delta.h"
#include "bw_graph/part/smo.h"
#include "bw_graph/storage/disk_manager.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

// Mirrors the SmoTest fixture but keeps a public access point to the new
// finalize/forwarder pipeline so we can drive the consolidation handshake
// step by step.
class PageDeltaTest : public ::testing::Test {
protected:
  size_t saved_page_size{};
  size_t saved_delta_page_size{};
  uint64_t saved_max_delta_chain{};

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
    saved_page_size = bw_graph::BW_GRAPH_PAGE_SIZE;
    saved_delta_page_size = bw_graph::BW_DELTA_PAGE_SIZE;
    saved_max_delta_chain = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
    bw_graph::BW_GRAPH_PAGE_SIZE = 1024;
    bw_graph::BW_DELTA_PAGE_SIZE = 4096;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;

    tmp_dir = std::filesystem::temp_directory_path() /
              ("test_page_delta_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp_dir);

    csr_disk_mgr = new disk_manager_t(tmp_dir / "csr.db", bw_graph::BW_GRAPH_PAGE_SIZE);
    delta_disk_mgr = new disk_manager_t(tmp_dir / "delta.db", bw_graph::BW_DELTA_PAGE_SIZE);

    csr_pool = buf_pool_t<csr_page_t>::buf_pool_init(4, 256, bw_graph::BW_GRAPH_PAGE_SIZE);
    delta_pool = buf_pool_t<delta_page_t>::buf_pool_init(4, 256, bw_graph::BW_DELTA_PAGE_SIZE);

    pmt = new page_map_t(delta_pool, delta_disk_mgr);
    vertex_index = new vertex_index_t(512);
    sketch = new vertex_update_sketch_t(512);
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

    bw_graph::BW_GRAPH_PAGE_SIZE = saved_page_size;
    bw_graph::BW_DELTA_PAGE_SIZE = saved_delta_page_size;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = saved_max_delta_chain;
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
    EXPECT_NE(frame, nullptr);

    for (auto& [v_id, off] : vertex_map) {
      vertex_index->update_vertex_loc(v_id, pno, off);
    }
    return {frame, pno};
  }
};

// Basic data-structure semantics: lookup_forward + note_rerouted_tail.
TEST_F(PageDeltaTest, ForwardingTable_BasicSemantics) {
  page_delta_t pd;
  pd.forward_map_.emplace(7, 100);
  pd.forward_map_.emplace(8, 200);

  page_no_t out = 0;
  EXPECT_TRUE(pd.lookup_forward(7, out));
  EXPECT_EQ(out, 100u);
  EXPECT_TRUE(pd.lookup_forward(8, out));
  EXPECT_EQ(out, 200u);
  EXPECT_FALSE(pd.lookup_forward(9, out));

  // First note wins; subsequent notes are ignored (idempotent).
  pd.note_rerouted_tail(7, delta_head_t{50, 3});
  pd.note_rerouted_tail(7, delta_head_t{51, 4});

  auto it = pd.rerouted_tail_.find(7);
  ASSERT_NE(it, pd.rerouted_tail_.end());
  EXPECT_EQ(it->second.page_no, 50u);
  EXPECT_EQ(it->second.record_idx, 3u);
}

// install / detach forwarder on a page_map_entry via the page_map_t API.
// This is the steady-state hot path that writers consult on every insert.
TEST_F(PageDeltaTest, PageMap_InstallGetDetachForwarder) {
  adj_map_t g;
  g[0] = {1};
  g[1] = {};
  auto [frame, pno] = make_csr_page(g);
  (void) frame;

  // The page has no delta entry yet; install_forwarder_for must materialise
  // an empty entry on the fly so concurrent writers can observe it.
  auto pd = std::make_shared<page_delta_t>();
  pd->forward_map_.emplace(0, 42);
  pmt->install_forwarder_for(pno, pd);

  // A writer would discover the forwarder via insert_delta_with_forward:
  // rerouted == true and the record lands on the forwarded target.
  delta_record_t rec(EDGE, INSERT, 0, 7);
  bool rerouted = false;
  auto [d_pno, d_idx] = pmt->insert_delta_with_forward(pno, 0, rec, rerouted);
  EXPECT_TRUE(rerouted);
  (void) d_pno;
  (void) d_idx;

  std::shared_ptr<page_delta_t> taken = pmt->detach_forwarder_for(pno);
  EXPECT_EQ(taken, pd);

  // After detach, a second writer must take the regular (non-rerouted) path.
  bool rerouted2 = false;
  pmt->insert_delta_with_forward(pno, 0, rec, rerouted2);
  EXPECT_FALSE(rerouted2);
}

TEST_F(PageDeltaTest, PageMap_DetachKeepsInFlightForwarderAlive) {
  adj_map_t g;
  g[0] = {1};
  g[1] = {};
  auto [frame, pno] = make_csr_page(g);
  (void) frame;

  auto forwarder = std::make_shared<page_delta_t>();
  forwarder->forward_map_.emplace(0, 42);
  std::weak_ptr<page_delta_t> lifetime = forwarder;
  pmt->install_forwarder_for(pno, forwarder);

  std::shared_ptr<page_delta_t> in_flight = pmt->acquire_forwarder_for(pno);
  ASSERT_EQ(in_flight, forwarder);

  EXPECT_EQ(pmt->detach_forwarder_for(pno), forwarder);
  forwarder.reset();
  EXPECT_FALSE(lifetime.expired());

  page_no_t target = 0;
  EXPECT_TRUE(in_flight->lookup_forward(0, target));
  EXPECT_EQ(target, 42u);

  in_flight.reset();
  EXPECT_TRUE(lifetime.expired());
}

// Drive the full Phase A -> reroute -> Phase C handshake by hand and assert
// that finalize_consolidation truncates the boundary record so the chain
// stops at exactly the writer-published delta.
TEST_F(PageDeltaTest, Finalize_TruncatesBoundaryRecord) {
  adj_map_t g;
  g[0] = {1, 2};
  g[1] = {0};
  g[2] = {0};
  auto [old_frame, old_pno] = make_csr_page(g);
  (void) old_frame;

  // Phase B (build new CSR page) but skip vertex_index update.
  adj_map_t merged_graph = g;
  smo_ctl_t::vertex_location_map_pub_t new_locations;
  page_no_t new_pno = smo->test_do_no_split(old_pno, merged_graph, csr_disk_mgr, csr_pool, sketch,
                                            vertex_index, new_locations);
  ASSERT_FALSE(new_locations.empty());

  // Phase A (install forwarder) AFTER new_pno is known. Until this point,
  // vertex_index still points at old_pno; the forwarder maps every merged
  // vertex onto new_pno.
  auto forwarder = std::make_shared<page_delta_t>();
  for (const auto& [v_id, loc] : new_locations) {
    forwarder->forward_map_.emplace(v_id, loc.first);
  }
  pmt->install_forwarder_for(old_pno, forwarder);

  // Simulate a concurrent writer that observes the forwarder via the
  // page_map and prepends a delta onto the new chain.  Inject a fake
  // non-invalid head so the boundary truncation is observable.
  vertex_index->v_w_lock(0);
  vertex_index->delta_heads_[0] = delta_head_t{12345, 7}; // pretend old chain
  delta_head_t cur = vertex_index->delta_heads_[0];

  delta_record_t pending(EDGE, INSERT, 0, 99);
  pending.next_page_no = cur.page_no;
  pending.next_record_idx = cur.record_idx;
  bool rerouted = false;
  auto [boundary_pno, boundary_idx] =
      pmt->insert_delta_with_forward(old_pno, 0, pending, rerouted);
  vertex_index->delta_heads_[0] = delta_head_t{boundary_pno, boundary_idx};
  vertex_index->v_w_unlock(0);

  EXPECT_TRUE(rerouted);

  // Sanity: the just-written record carries the fake old head as next_*.
  delta_page_t* dp = delta_pool->buf_page_read(boundary_pno, delta_disk_mgr);
  const delta_record_t& rec_before = dp->get_record(boundary_idx);
  EXPECT_EQ(rec_before.next_page_no, 12345u);
  EXPECT_EQ(rec_before.next_record_idx, 7u);
  dp->r_unlatch();

  // Phase C: atomic finalize.
  smo->test_finalize_consolidation(new_locations, pmt, vertex_index, forwarder.get());

  // vertex_index now points at the new page.
  EXPECT_EQ(vertex_index->get_vertex_location(0).first, new_pno);
  EXPECT_NE(vertex_index->get_vertex_location(0).first, old_pno);

  // delta_heads_[0] keeps pointing at the writer-published record because a
  // delta was prepended after Phase A.
  delta_head_t head_after = vertex_index->delta_heads_[0];
  EXPECT_EQ(head_after.page_no, boundary_pno);
  EXPECT_EQ(head_after.record_idx, boundary_idx);

  // The boundary record's next_* must now be INVALID so chain traversal
  // stops here instead of dereferencing the (already-merged) old chain.
  dp = delta_pool->buf_page_read(boundary_pno, delta_disk_mgr);
  const delta_record_t& rec_after = dp->get_record(boundary_idx);
  EXPECT_EQ(rec_after.next_page_no, INVALID_DELTA_PAGE_NO);
  EXPECT_EQ(rec_after.next_record_idx, INVALID_DELTA_RECORD_IDX);
  dp->r_unlatch();

  // For vertices that received no writes during the window, finalize must
  // leave the head invalid (the new CSR page already embeds everything).
  EXPECT_FALSE(vertex_index->delta_heads_[1].is_valid());
  EXPECT_FALSE(vertex_index->delta_heads_[2].is_valid());

  pmt->detach_forwarder_for(old_pno);
}

// No writer during the window: finalize must reset every delta_head to
// invalid and not call truncate_chain_at (no boundary recorded).
TEST_F(PageDeltaTest, Finalize_NoWriter_ResetsHeads) {
  adj_map_t g;
  g[10] = {11};
  g[11] = {10};
  auto [old_frame, old_pno] = make_csr_page(g);
  (void) old_frame;

  adj_map_t merged_graph = g;
  smo_ctl_t::vertex_location_map_pub_t new_locations;
  page_no_t new_pno = smo->test_do_no_split(old_pno, merged_graph, csr_disk_mgr, csr_pool, sketch,
                                            vertex_index, new_locations);

  auto forwarder = std::make_shared<page_delta_t>();
  for (const auto& [v_id, loc] : new_locations) {
    forwarder->forward_map_.emplace(v_id, loc.first);
  }
  pmt->install_forwarder_for(old_pno, forwarder);

  smo->test_finalize_consolidation(new_locations, pmt, vertex_index, forwarder.get());

  EXPECT_EQ(vertex_index->get_vertex_location(10).first, new_pno);
  EXPECT_EQ(vertex_index->get_vertex_location(11).first, new_pno);
  EXPECT_FALSE(vertex_index->delta_heads_[10].is_valid());
  EXPECT_FALSE(vertex_index->delta_heads_[11].is_valid());

  pmt->detach_forwarder_for(old_pno);
}

// truncate_chain_at on an arbitrary record must persist next_* = INVALID and
// keep the rest of the page intact (regression for page_map_t helper).
TEST_F(PageDeltaTest, TruncateChainAt_OnlyClearsNextLink) {
  delta_record_t rec(EDGE, INSERT, 5, 6);
  rec.next_page_no = 17;
  rec.next_record_idx = 3;
  auto [pno, idx] = pmt->insert_delta_and_get_loc(/*csr_pno=*/1, rec);

  delta_page_t* dp = delta_pool->buf_page_read(pno, delta_disk_mgr);
  EXPECT_EQ(dp->get_record(idx).next_page_no, 17u);
  EXPECT_EQ(dp->get_record(idx).next_record_idx, 3u);
  dp->r_unlatch();

  pmt->truncate_chain_at(delta_head_t{pno, idx});

  dp = delta_pool->buf_page_read(pno, delta_disk_mgr);
  EXPECT_EQ(dp->get_record(idx).next_page_no, INVALID_DELTA_PAGE_NO);
  EXPECT_EQ(dp->get_record(idx).next_record_idx, INVALID_DELTA_RECORD_IDX);
  // The payload columns must remain untouched.
  EXPECT_EQ(dp->get_record(idx).first, 5u);
  EXPECT_EQ(dp->get_record(idx).second, 6u);
  EXPECT_EQ(dp->get_record(idx).target, EDGE);
  EXPECT_EQ(dp->get_record(idx).type, INSERT);
  dp->r_unlatch();
}
