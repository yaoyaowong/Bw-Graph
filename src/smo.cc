#include "bw_graph/part/smo.h"

#include "bw_graph/buf/csr_flusher.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/logger.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/external.h"
#include "bw_graph/io/builder.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_index.h"
#include "bw_graph/part/page_delta.h"
#include "bw_graph/part/partition.h"
#include "bw_graph/part/reorder.h"
#include "bw_graph/storage/disk_manager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

// Construct smo_ctl_t.
smo_ctl_t::smo_ctl_t(size_t max_threads, bool manual_version_switch)
    : max_threads_(max_threads),
      pending_consolidations_(0),
      manual_version_switch_(manual_version_switch) {
  // max_threads == 0 disables consolidation entirely: skip arena/task_group
  // allocation so that no background SMO worker exists.
  if (max_threads_ == 0) {
    return;
  }
  smo_arena_ = std::make_unique<tbb::task_arena>(max_threads_, 0);
  smo_arena_->execute([this] { task_group_ = std::make_unique<tbb::task_group>(); });
}

// Destroy smo_ctl_t.
smo_ctl_t::~smo_ctl_t() { publish_pending_versions(); }

// Wait all consolidations.
void smo_ctl_t::wait_all_consolidations() {
  if (task_group_) {
    task_group_->wait();
  }
}

void smo_ctl_t::publish_pending_versions() {
  wait_all_consolidations();

  std::vector<pending_version_switch_t> pending;
  {
    std::lock_guard<std::mutex> guard(pending_version_switches_mutex_);
    pending.swap(pending_version_switches_);
  }

  folly::F14FastSet<vertex_index_t*> vertex_indexes;
  for (auto& version_switch : pending) {
    vertex_indexes.insert(version_switch.vertex_index);
    publish_version_switch(version_switch);
  }
  for (vertex_index_t* vertex_index : vertex_indexes) {
    if (vertex_index != nullptr) {
      vertex_index->wait_for_version_gc();
    }
  }
}

// Get pending consolidation count.
size_t smo_ctl_t::get_pending_consolidation_count() const { return pending_consolidations_.load(); }

// Check pending consolidations.
bool smo_ctl_t::has_pending_consolidations() const { return pending_consolidations_.load() > 0; }

// Build merged graph.
adj_map_t smo_ctl_t::build_merged_graph(csr_page_t* page,
                                        const std::vector<delta_record_t>& deltas,
                                        vertex_index_t* vertex_index,
                                        giant_vertex_db_t* giant_db) {
  adj_map_t graph;

  // Snapshot base page.
  page->r_latch();
  vertex_span_t vertices = page->get_vertices();
  uint16_t idx = 0;
  for (const csr_vertex_t& v : vertices) {
    if (!v.is_deleted()) {
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
      auto range = page->get_neighbor_ptv_range(idx);
      auto& vec = graph[v.vertex_id];
      vec.reserve(v.degree);
      for (v_id_t n : range) {
        vec.push_back(n);
      }
#else
      neighbor_span_t nbrs = page->get_neighbors(idx);
      graph[v.vertex_id].assign(nbrs.begin(), nbrs.end());
#endif
    }
    ++idx;
  }
  page->r_unlatch();

  if (vertex_index != nullptr && giant_db != nullptr) {
    for (auto& [v_id, nbrs] : graph) {
      if (vertex_index->is_giant_vertex(v_id)) {
        nbrs = giant_db->read_neighbor_clone(v_id);
      }
    }
  }

  for (const delta_record_t& delta : deltas) {
    if (delta.target != EDGE) {
      continue;
    }
    if (delta.type == INSERT) {
      graph[delta.first].push_back(delta.second);
    } else { // DELETE
      auto it = graph.find(delta.first);
      if (it != graph.end()) {
        auto& nbrs = it->second;
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), delta.second), nbrs.end());
      }
    }
  }

  for (auto& [v, nbrs] : graph) {
    std::sort(nbrs.begin(), nbrs.end());
    nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
  }

  return graph;
}

#ifdef BW_GRAPH_ENABLE_TRANSACTION
// Build merged graph with ts.
std::pair<adj_map_t, edge_ts_map_t>
smo_ctl_t::build_merged_graph_with_ts(csr_page_t* page, const std::vector<delta_record_t>& deltas,
                                      vertex_index_t* vertex_index,
                                      giant_vertex_db_t* giant_db) {
  adj_map_t graph;
  // per-edge commit_ts: (src, dst) -> timestamp
  folly::F14FastMap<v_id_t, folly::F14FastMap<v_id_t, timestamp_t>> commit_map;

  // Snapshot base page - carry existing per-edge timestamps.
  page->r_latch();
  vertex_span_t vertices = page->get_vertices();
  uint16_t idx = 0;
  for (const csr_vertex_t& v : vertices) {
    if (!v.is_deleted()) {
      auto& nbr_vec = graph[v.vertex_id];
      auto& ts_map = commit_map[v.vertex_id];
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
      auto range = page->get_neighbor_ptv_range(idx);
      uint32_t j = 0;
      for (v_id_t n : range) {
        nbr_vec.push_back(n);
        ts_map[n] = page->get_edge_ts(idx, j++);
      }
#else
      neighbor_span_t nbrs = page->get_neighbors(idx);
      for (uint32_t j = 0; j < static_cast<uint32_t>(nbrs.size()); ++j) {
        nbr_vec.push_back(nbrs[j]);
        ts_map[nbrs[j]] = page->get_edge_ts(idx, j);
      }
#endif
    }
    ++idx;
  }
  page->r_unlatch();

  if (vertex_index != nullptr && giant_db != nullptr) {
    for (auto& [v_id, nbrs] : graph) {
      if (vertex_index->is_giant_vertex(v_id)) {
        nbrs = giant_db->read_neighbor_clone(v_id);
        auto& ts_for_vertex = commit_map[v_id];
        ts_for_vertex.clear();
        for (v_id_t nbr : nbrs) {
          ts_for_vertex[nbr] = INVALID_TS;
        }
      }
    }
  }

  // Apply deltas: INSERT carries commit_ts; DELETE removes edge+timestamp.
  for (const delta_record_t& delta : deltas) {
    if (delta.target != EDGE)
      continue;
    if (delta.type == INSERT) {
      graph[delta.first].push_back(delta.second);
      commit_map[delta.first][delta.second] = delta.commit_ts;
    } else {
      auto git = graph.find(delta.first);
      if (git != graph.end()) {
        auto& nbrs = git->second;
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), delta.second), nbrs.end());
        auto cit = commit_map.find(delta.first);
        if (cit != commit_map.end())
          cit->second.erase(delta.second);
      }
    }
  }

  // Sort + dedup neighbors; build parallel edge_ts_map_t.
  edge_ts_map_t ts_out;
  for (auto& [v, nbrs] : graph) {
    std::sort(nbrs.begin(), nbrs.end());
    nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());

    auto& ts_vec = ts_out[v];
    const auto& c_map = commit_map[v];
    ts_vec.reserve(nbrs.size());
    for (v_id_t n : nbrs) {
      auto it = c_map.find(n);
      ts_vec.push_back(it != c_map.end() ? it->second : INVALID_TS);
    }
  }

  return {std::move(graph), std::move(ts_out)};
}
#endif // BW_GRAPH_ENABLE_TRANSACTION

// Estimate page bytes.
size_t smo_ctl_t::estimate_page_bytes(const adj_map_t& graph) {
  constexpr size_t kMetadata =
      sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

  const size_t kNeighborBlock = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);

  size_t total = kMetadata + kNeighborBlock;
  for (const auto& [v, nbrs] : graph) {
    total += sizeof(csr_vertex_t);
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
    // Upper-bound: each neighbor needs at most 4 bytes in PTV encoding
    total += nbrs.size() * 4;
#else
    total += nbrs.size() * sizeof(v_id_t);
#endif
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    total += nbrs.size() * sizeof(timestamp_t);
#endif
  }
  return total;
}

bool smo_ctl_t::try_recycle_old_page(page_no_t old_page_no, disk_manager_t* disk_manager,
                                     buf_pool_t<csr_page_t>* buf_pool) {
  if (!buf_pool->retire_page(old_page_no)) {
    return false;
  }
  disk_manager->delete_page(old_page_no);
  return true;
}

void smo_ctl_t::defer_recycle_old_page(page_no_t old_page_no) {
  std::lock_guard<std::mutex> guard(retired_pages_mutex_);
  if (std::find(pending_retired_pages_.begin(), pending_retired_pages_.end(), old_page_no) ==
      pending_retired_pages_.end()) {
    pending_retired_pages_.push_back(old_page_no);
  }
}

void smo_ctl_t::retry_deferred_recycles(disk_manager_t* disk_manager,
                                        buf_pool_t<csr_page_t>* buf_pool) {
  std::lock_guard<std::mutex> guard(retired_pages_mutex_);
  auto out = pending_retired_pages_.begin();
  for (auto it = pending_retired_pages_.begin(); it != pending_retired_pages_.end(); ++it) {
    if (!try_recycle_old_page(*it, disk_manager, buf_pool)) {
      *out++ = *it;
    }
  }
  pending_retired_pages_.erase(out, pending_retired_pages_.end());
}

// Handle do no split.
page_no_t smo_ctl_t::do_no_split(page_no_t old_page_no, adj_map_t& merged_graph,
                                 disk_manager_t* disk_manager, buf_pool_t<csr_page_t>* buf_pool,
                                 vertex_update_sketch_t* vertex_update_sketch,
                                 vertex_index_t* vertex_index,
                                 vertex_location_map_t& new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                                 ,
                                 const edge_ts_map_t* ts_map
#endif
) {
  size_t new_byte_offset = disk_manager->allocate_page();
  page_no_t new_page_no = static_cast<page_no_t>(new_byte_offset / bw_graph::BW_GRAPH_PAGE_SIZE);

  // Per-vertex slot reservation: write-heavy vertices get more in-place
  // insertion slots than read-heavy neighbours sharing the same page.
  folly::F14FastMap<v_id_t, uint16_t> per_vertex_slots;
  per_vertex_slots.reserve(merged_graph.size());
  for (const auto& [v, _] : merged_graph) {
    uint16_t s = 0;
    if (vertex_update_sketch != nullptr) {
      s = static_cast<uint16_t>(vertex_update_sketch->get_per_vertex_reservation(v));
    } else {
      s = static_cast<uint16_t>(bw_graph::BW_GRAPH_EDGE_SLOT_COUNT);
    }
    per_vertex_slots.emplace(v, s);
  }

  adj_map_iter_t view = create_neighbor_span_view(merged_graph);
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  csr_page_t new_page(new_page_no, view, vertex_map, per_vertex_slots);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Write per-edge timestamps before copying into the pool.
  if (ts_map != nullptr) {
    new_page.write_edge_timestamps(*ts_map);
  }
#endif

  // Copy into pool and mark dirty for async flush (no synchronous write_page).
  csr_page_t* pool_page = buf_pool->buf_page_cp(new_page_no, new_page.get_data(), disk_manager);
  if (pool_page == nullptr) {
    throw std::runtime_error("SMO no-split: allocated page already exists in buffer pool");
  }
  pool_page->mark_dirty();

  // Publish locations to the caller; vertex_index update is deferred to
  // finalize_consolidation so writers reroute their concurrent deltas via
  // the forwarding table installed on the old page.
  for (const auto& [v_id, new_offset] : vertex_map) {
    new_locations[v_id] = vertex_index->prepare_vertex_version(
        v_id, new_page_no, static_cast<uint16_t>(new_offset));
  }

  (void) old_page_no;
  return new_page_no;
}

// Handle do split.
std::vector<page_no_t> smo_ctl_t::do_split(page_no_t old_page_no, adj_map_t& merged_graph,
                                           disk_manager_t* disk_manager,
                                           buf_pool_t<csr_page_t>* buf_pool,
                                           vertex_update_sketch_t* vertex_update_sketch,
                                           vertex_index_t* vertex_index,
                                           vertex_location_map_t& new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                                           ,
                                           const edge_ts_map_t* ts_map
#endif
) {
  (void) old_page_no;

  // Reorder vertices by Gorder before sequential packing so that vertices
  // with high locality end up on the same CSR page after the split.
  std::vector<v_id_t> sorted_vertices = gorder_reordering_optimized(merged_graph);

  // Builder default slot is the yaml baseline; we override per vertex via
  // the slot overload of add_vertex below so write-heavy vertices reserve
  // more in-place insertion slots than read-heavy neighbours sharing the
  // same page.
  uint16_t fallback_slot = static_cast<uint16_t>(bw_graph::BW_GRAPH_EDGE_SLOT_COUNT);
  csr_page_builder_t builder(bw_graph::BW_GRAPH_PAGE_SIZE, fallback_slot);

  new_locations.reserve(merged_graph.size());

  std::vector<page_no_t> created_page_nos;

  auto flush_current_page = [&]() {
    if (builder.is_empty()) {
      return;
    }
    auto [built_page, vertex_map] = builder.build();

    size_t new_byte_offset = disk_manager->allocate_page();
    page_no_t new_page_no = static_cast<page_no_t>(new_byte_offset / bw_graph::BW_GRAPH_PAGE_SIZE);
    built_page->set_page_no(new_page_no);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
    if (ts_map != nullptr) {
      built_page->write_edge_timestamps(*ts_map);
    }
#endif

    // Copy into pool and mark dirty for async flush (no synchronous write_page).
    csr_page_t* pool_page =
        buf_pool->buf_page_cp(new_page_no, built_page->get_data(), disk_manager);
    if (pool_page == nullptr) {
      delete built_page;
      throw std::runtime_error("SMO split: allocated page already exists in buffer pool");
    }
    pool_page->mark_dirty();

    for (const auto& [v_id, off] : vertex_map) {
      new_locations[v_id] =
          vertex_index->prepare_vertex_version(v_id, new_page_no, static_cast<uint16_t>(off));
    }
    created_page_nos.push_back(new_page_no);
    delete built_page;
  };

  for (v_id_t v_id : sorted_vertices) {
    auto it = merged_graph.find(v_id);
    if (it == merged_graph.end()) {
      continue;
    }
    std::vector<v_id_t>& neighbors = it->second;
    neighbor_span_t neighbor_span(neighbors.data(), neighbors.size());

    // Per-vertex slot reservation derived from the sketch.
    uint16_t this_slot = fallback_slot;
    if (vertex_update_sketch != nullptr) {
      this_slot = static_cast<uint16_t>(vertex_update_sketch->get_per_vertex_reservation(v_id));
    }

    if (!builder.add_vertex(v_id, neighbor_span, this_slot)) {
      flush_current_page();
      bool inserted = builder.add_vertex(v_id, neighbor_span, this_slot);
      if (!inserted) {
        throw std::runtime_error("Failed to pack vertex into empty page");
      }
    }
  }

  flush_current_page();

  // vertex_index / delta_heads_ are NOT updated here; finalize_consolidation
  // performs the atomic commit while concurrent writers can still observe a
  // forwarding table on the old page.
  return created_page_nos;
}

// Atomic finalize: publish new vertex locations under per-vertex write
// latch and truncate the chain at the boundary record so the new CSR page
// becomes the sole source of truth for every merged neighbor list.
void smo_ctl_t::finalize_consolidation(vertex_location_map_t& new_locations,
                                       page_map_t* page_map, vertex_index_t* vertex_index,
                                       page_delta_t* forwarder,
                                       const folly::F14FastSet<v_id_t>& giant_vertices,
                                       const std::shared_ptr<vertex_version_reclaim_batch_t>&
                                           reclaim_batch) {
  for (auto& [v_id, loc] : new_locations) {
    auto& item = vertex_index->items[v_id];
    item.latch.w_lock();

    delta_head_t new_head = delta_head_t::invalid();

    // If a writer prepended a delta during the consolidation window, the
    // boundary record (the FIRST one inserted on the new chain) still has
    // its next_* link pointing at the now-merged old chain.  Cut that link
    // so the new CSR page becomes the chain terminator.
    if (forwarder != nullptr) {
      delta_head_t boundary{};
      bool found = false;
      {
        std::lock_guard<std::mutex> guard(forwarder->rerouted_mutex_);
        auto it = forwarder->rerouted_tail_.find(v_id);
        if (it != forwarder->rerouted_tail_.end()) {
          boundary = it->second;
          found = true;
        }
      }
      if (found) {
        page_map->truncate_chain_at(boundary);
        // The writer-published head remains valid (still on the new chain).
        new_head = vertex_index->delta_heads_[v_id];
      }
    }

    vertex_index->delta_heads_[v_id] = new_head;
    item.vertex_type = giant_vertices.count(v_id) > 0 ? GIANT : NORMAL;
    if (!vertex_index->publish_prepared_version(v_id, loc, reclaim_batch)) {
      item.latch.w_unlock();
      throw std::runtime_error("SMO vertex version CAS failed");
    }
    item.latch.w_unlock();
  }
}

void smo_ctl_t::publish_version_switch(pending_version_switch_t& pending) {
  auto reclaim_batch = std::make_shared<vertex_version_reclaim_batch_t>(
      pending.new_locations.size(),
      [old_page_no = pending.old_page_no, disk_manager = pending.disk_manager,
       buf_pool = pending.buf_pool] {
        while (!buf_pool->retire_page(old_page_no)) {
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        disk_manager->delete_page(old_page_no);
      });

  finalize_consolidation(pending.new_locations, pending.page_map, pending.vertex_index,
                         pending.forwarder.get(), pending.giant_vertices, reclaim_batch);

  pending.page_map->detach_forwarder_for(pending.old_page_no);

  if (!pending.need_split) {
    if (index_page_map_ != nullptr && pending.old_parent_no != 0) {
      update_index_no_split(pending.old_page_no, pending.single_new_csr_no,
                            pending.old_parent_no);
    }
  } else if (index_page_map_ != nullptr && pending.old_parent_no != 0 &&
             !pending.new_page_nos.empty()) {
    update_index_split(pending.old_page_no, pending.old_parent_no, pending.new_page_nos,
                       pending.merged_graph, pending.vertex_index);
  }

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  if (pending.set_page_snapshot_ts) {
    for (page_no_t page_no : pending.new_page_nos) {
      csr_page_t* page = pending.buf_pool->buf_page_read(page_no, pending.disk_manager);
      page->set_page_snapshot_ts(pending.page_snapshot_ts);
      page->r_unlatch();
    }
  }
#endif

  pending.forwarder.reset();
}

void smo_ctl_t::update_index_no_split(page_no_t old_csr_page_no, page_no_t new_csr_page_no,
                                      page_no_t old_parent_no) {
  std::lock_guard<std::mutex> lock(index_mutex_);
  auto it = index_page_map_->find(old_parent_no);
  if (it == index_page_map_->end())
    return;

  index_page_t* old_idx = it->second;
  block_adj_map_t adj = old_idx->get_block_adj_map();

  // Rename old_csr_page_no -> new_csr_page_no in adj keys and neighbor lists.
  if (adj.count(old_csr_page_no)) {
    std::vector<page_no_t> edges = std::move(adj[old_csr_page_no]);
    adj.erase(old_csr_page_no);
    adj[new_csr_page_no] = std::move(edges);
  }
  for (auto& [k, nbrs] : adj) {
    for (auto& n : nbrs) {
      if (n == old_csr_page_no)
        n = new_csr_page_no;
    }
  }

  // COW: build new index page (same page_no, new content).
  index_page_t* new_idx = new index_page_t(old_parent_no, adj);
  new_idx->set_parent_page_no(old_idx->parent_page_no_);
  for (page_no_t np : old_idx->read_neighbor_block_clone()) {
    new_idx->add_neighbor_page_no(np);
  }
  (*index_page_map_)[old_parent_no] = new_idx;
  delete old_idx;
}

// Update index split.
void smo_ctl_t::update_index_split(page_no_t old_csr_page_no, page_no_t old_parent_no,
                                   const std::vector<page_no_t>& new_csr_page_nos,
                                   const adj_map_t& merged_graph, vertex_index_t* vertex_index) {
  std::lock_guard<std::mutex> lock(index_mutex_);
  auto it = index_page_map_->find(old_parent_no);
  if (it == index_page_map_->end())
    return;

  index_page_t* old_idx = it->second;
  block_adj_map_t adj = old_idx->get_block_adj_map();

  // Remove the old CSR page from adj (key + all neighbor references).
  adj.erase(old_csr_page_no);
  for (auto& [k, nbrs] : adj) {
    nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), old_csr_page_no), nbrs.end());
  }

  // Insert new CSR pages with empty neighbor lists.
  for (page_no_t p : new_csr_page_nos) {
    adj[p] = {};
  }

  // Compute cross-page edges via merged_graph + vertex_index (now updated).
  // vertex_index already reflects the post-split locations.
  std::set<page_no_t> new_pages_set(new_csr_page_nos.begin(), new_csr_page_nos.end());
  for (const auto& [v, nbrs] : merged_graph) {
    page_no_t v_page = vertex_index->get_vertex_location(v).first;
    for (v_id_t u : nbrs) {
      page_no_t u_page = vertex_index->get_vertex_location(u).first;
      if (u_page == v_page)
        continue;

      // Add edge v_page ↔ u_page in adj (dedup).
      if (adj.count(v_page)) {
        auto& v_edges = adj[v_page];
        if (std::find(v_edges.begin(), v_edges.end(), u_page) == v_edges.end()) {
          v_edges.push_back(u_page);
        }
      }
      if (adj.count(u_page)) {
        auto& u_edges = adj[u_page];
        if (std::find(u_edges.begin(), u_edges.end(), v_page) == u_edges.end()) {
          u_edges.push_back(v_page);
        }
      }
    }
  }

  // 40% slack threshold (R3): trigger index split when > 60% of max capacity.
  const size_t max_blocks = bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS;
  const size_t threshold = (max_blocks > 0) ? (max_blocks * 6 / 10) : max_blocks;

  if (threshold == 0 || adj.size() <= threshold) {
    // COW: replace old index page (same page_no, new content).
    index_page_t* new_idx = new index_page_t(old_parent_no, adj);
    new_idx->set_parent_page_no(old_idx->parent_page_no_);
    for (page_no_t np : old_idx->read_neighbor_block_clone()) {
      new_idx->add_neighbor_page_no(np);
    }
    (*index_page_map_)[old_parent_no] = new_idx;
    delete old_idx;
  } else {
    // Index page overflows -> split it (R1). do_index_split is called with
    // index_mutex_ already held (it must NOT re-acquire).
    do_index_split(old_parent_no, adj);
  }
}

// Handle do index split.
void smo_ctl_t::do_index_split(page_no_t old_idx_page_no, block_adj_map_t& new_adj) {
  // Called with index_mutex_ already held.
  BW_GRAPH_LOG_INFO("[SMO] index_split idx_page=%lu", (unsigned long)old_idx_page_no);
  auto it = index_page_map_->find(old_idx_page_no);
  index_page_t* old_idx = (it != index_page_map_->end()) ? it->second : nullptr;
  page_no_t grandparent_no = old_idx ? old_idx->parent_page_no_ : 0;

  // Remove the old index page from the map now.
  if (it != index_page_map_->end()) {
    index_page_map_->erase(it);
    delete old_idx;
    old_idx = nullptr;
  }

  // Partition the block adj_map using the same strategy as TAT construction (R1).
  // Use 60% fill = 40% slack (R3), reusing calculate_bw_capacity indirectly
  // through BW_GRAPH_INDEX_MAX_BLOCKS which was computed by calculate_bw_capacity.
  const size_t max_blocks = bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS;
  const size_t target_size = (max_blocks > 0) ? (max_blocks * 6 / 10) : 1;

  auto [block_partition, block_part_map] =
      absolute_weight_block_partition(new_adj, static_cast<kaminpar::shm::NodeWeight>(target_size));

  std::vector<page_no_t> new_idx_page_nos;
  for (const auto& sub_partition : block_partition) {
    if (sub_partition.empty())
      continue;

    page_no_t new_idx_no = next_index_page_no_.fetch_sub(1, std::memory_order_relaxed);

    block_adj_map_t subgraph = induced_block_subgraph(new_adj, sub_partition);

    index_page_t* new_idx_page = new index_page_t(new_idx_no, subgraph);
    new_idx_page->set_parent_page_no(grandparent_no);

    (*index_page_map_)[new_idx_no] = new_idx_page;
    new_idx_page_nos.push_back(new_idx_no);
  }

  if (new_idx_page_nos.empty())
    return;

  // Update grandparent to reference new index pages instead of old one.
  if (grandparent_no != 0) {
    auto gp_it = index_page_map_->find(grandparent_no);
    if (gp_it != index_page_map_->end()) {
      index_page_t* gp = gp_it->second;
      block_adj_map_t gp_adj = gp->get_block_adj_map();

      // Remove old index page from grandparent adj.
      gp_adj.erase(old_idx_page_no);
      for (auto& [k, nbrs] : gp_adj) {
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), old_idx_page_no), nbrs.end());
      }

      // Add new index pages; they are mutually adjacent (they cover adjacent
      // partitions of the same block set).
      for (page_no_t p : new_idx_page_nos)
        gp_adj[p] = {};
      for (size_t i = 0; i < new_idx_page_nos.size(); ++i) {
        for (size_t j = i + 1; j < new_idx_page_nos.size(); ++j) {
          gp_adj[new_idx_page_nos[i]].push_back(new_idx_page_nos[j]);
          gp_adj[new_idx_page_nos[j]].push_back(new_idx_page_nos[i]);
        }
      }

      index_page_t* new_gp = new index_page_t(grandparent_no, gp_adj);
      new_gp->set_parent_page_no(gp->parent_page_no_);
      for (page_no_t np : gp->read_neighbor_block_clone()) {
        new_gp->add_neighbor_page_no(np);
      }
      (*index_page_map_)[grandparent_no] = new_gp;
      delete gp;

      // Fix parent pointers on the freshly installed index pages.
      for (page_no_t p : new_idx_page_nos) {
        auto ni = index_page_map_->find(p);
        if (ni != index_page_map_->end()) {
          ni->second->set_parent_page_no(grandparent_no);
        }
      }
    }
  } else if (root_index_page_ != nullptr) {
    if (new_idx_page_nos.size() == 1) {
      // Old root didn't need further splitting: update root pointer.
      *root_index_page_ = (*index_page_map_)[new_idx_page_nos[0]];
    } else {
      // Old root split into 2+: build a new root page.
      block_adj_map_t root_adj;
      for (page_no_t p : new_idx_page_nos)
        root_adj[p] = {};
      for (size_t i = 0; i < new_idx_page_nos.size(); ++i) {
        for (size_t j = i + 1; j < new_idx_page_nos.size(); ++j) {
          root_adj[new_idx_page_nos[i]].push_back(new_idx_page_nos[j]);
          root_adj[new_idx_page_nos[j]].push_back(new_idx_page_nos[i]);
        }
      }
      page_no_t new_root_no = next_index_page_no_.fetch_sub(1, std::memory_order_relaxed);
      index_page_t* new_root = new index_page_t(new_root_no, root_adj);
      (*index_page_map_)[new_root_no] = new_root;
      *root_index_page_ = new_root;
      for (page_no_t p : new_idx_page_nos) {
        auto ni = index_page_map_->find(p);
        if (ni != index_page_map_->end()) {
          ni->second->set_parent_page_no(new_root_no);
        }
      }
    }
  }
}

void smo_ctl_t::consolidate_pages(csr_page_t* old_page, page_map_t* page_map,
                                  disk_manager_t* disk_manager, buf_pool_t<csr_page_t>* buf_pool,
                                  vertex_index_t* vertex_index, giant_vertex_db_t* giant_db,
                                  vertex_update_sketch_t* vertex_update_sketch,
                                  uint64_t requested_threshold, bool old_page_pinned,
                                  bool consolidation_claimed) {
  struct old_page_pin_guard_t {
    csr_page_t* page{nullptr};
    page_map_t* page_map{nullptr};
    bool pinned{false};
    bool claimed{false};

    ~old_page_pin_guard_t() {
      if (claimed && page != nullptr && page_map != nullptr) {
        page_map->end_consolidation_for(page->get_page_no());
      }
      if (pinned && page != nullptr) {
        page->dec_pin_count();
      }
    }
  } old_page_pin_guard{old_page, page_map, old_page_pinned, consolidation_claimed};

  // Consolidation disabled: keep the delta chain intact and bail out.
  if (max_threads_ == 0) {
    return;
  }
  page_no_t old_page_no = old_page->get_page_no();
  retry_deferred_recycles(disk_manager, buf_pool);

  // Safety check.  Legacy callers (requested_threshold == 0) fall back to
  // the yaml baseline so they preserve historical behaviour.  Workload-
  // driven callers from db_dyn pass the adaptive threshold (down to 1)
  // they used to decide to trigger so a read-heavy fast path can still
  // make progress here even when the chain is well below the yaml value.
  uint64_t safety_threshold =
      (requested_threshold == 0) ? bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH : requested_threshold;
  uint64_t _smo_chain_len_ = page_map->get_delta_chain_length(old_page_no);
  if (_smo_chain_len_ < safety_threshold) {
    return;
  }

  std::vector<delta_record_t> deltas = page_map->fetch_deltas_with_mark(old_page_no);
  if (deltas.empty()) {
    return;
  }

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Defer records that are still visible to active readers.
  timestamp_t txn_watermark = INVALID_TS;
  std::vector<delta_record_t> deferred;

  if (txn_manager_ != nullptr) {
    txn_watermark = txn_manager_->get_watermark();
    std::vector<delta_record_t> mergeable;
    mergeable.reserve(deltas.size());
    for (auto& rec : deltas) {
      if (rec.commit_ts != INVALID_TS && rec.commit_ts < txn_watermark) {
        mergeable.push_back(rec);
      } else {
        deferred.push_back(rec);
      }
    }
    deltas = std::move(mergeable);

    if (deltas.empty()) {
      // Nothing safe to consolidate yet; restore deferred records into the
      // page_map and update delta_heads_ so readers can still see them.
      for (delta_record_t& rec : deferred) {
        if (rec.target != EDGE) {
          continue;
        }
        vertex_loc_t loc = vertex_index->get_vertex_location(rec.first);
        vertex_index->v_w_lock(rec.first);
        delta_head_t cur_head = vertex_index->delta_heads_[rec.first];
        rec.next_page_no = cur_head.page_no;
        rec.next_record_idx = cur_head.record_idx;
        auto [dp, di] = page_map->insert_delta_and_get_loc(loc.first, rec);
        vertex_index->delta_heads_[rec.first] = {dp, di};
        vertex_index->v_w_unlock(rec.first);
      }
      return;
    }
  }
#endif // BW_GRAPH_ENABLE_TRANSACTION

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  edge_ts_map_t ts_map;
  adj_map_t merged_graph;
  if (txn_manager_ != nullptr) {
    auto [mg, tm] = build_merged_graph_with_ts(old_page, deltas, vertex_index, giant_db);
    merged_graph = std::move(mg);
    ts_map = std::move(tm);
  } else {
    merged_graph = build_merged_graph(old_page, deltas, vertex_index, giant_db);
  }
#else
  adj_map_t merged_graph = build_merged_graph(old_page, deltas, vertex_index, giant_db);
#endif
  if (merged_graph.empty()) {
    return;
  }

  BW_GRAPH_LOG_INFO("[SMO] start page=%lu vertices=%zu chain_len=%lu",
      (unsigned long)old_page_no, merged_graph.size(), (unsigned long)_smo_chain_len_);
  auto _smo_t0_ = std::chrono::steady_clock::now();

  folly::F14FastSet<v_id_t> giant_vertices;
  giant_vertices.reserve(merged_graph.size());
  if (giant_db != nullptr) {
    for (const auto& [v_id, neighbors] : merged_graph) {
      if (vertex_index->is_giant_vertex(v_id) ||
          neighbors.size() >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
        if (!giant_db->insert_vertex(v_id, neighbors)) {
          throw std::runtime_error("SMO: failed to persist promoted giant vertex");
        }
        giant_vertices.insert(v_id);
        BW_GRAPH_LOG_INFO("[GIANT_MOVING] vertex=%lu degree=%zu page=%lu",
            (unsigned long)v_id, neighbors.size(), (unsigned long)old_page_no);
      }
    }
    if (!giant_vertices.empty()) {
      BW_GRAPH_LOG_INFO("[GIANT_MOVING] done page=%lu promoted=%zu",
          (unsigned long)old_page_no, giant_vertices.size());
    }
  }

  // Save parent info BEFORE the split changes the vertex index.
  page_no_t old_parent_no = old_page->get_parent_page_no();

  size_t est = estimate_page_bytes(merged_graph);
  bool need_split = (est > bw_graph::BW_GRAPH_PAGE_SIZE);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  const edge_ts_map_t* ts_map_ptr = (txn_manager_ != nullptr) ? &ts_map : nullptr;
#endif

  vertex_location_map_t new_locations;
  std::vector<page_no_t> new_page_nos;
  page_no_t single_new_csr_no = 0;

  if (!need_split) {
    single_new_csr_no = do_no_split(old_page_no, merged_graph, disk_manager, buf_pool,
                                    vertex_update_sketch, vertex_index, new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                                    ,
                                    ts_map_ptr
#endif
    );
    new_page_nos.push_back(single_new_csr_no);
    BW_GRAPH_LOG_INFO("[SMO] no_split page=%lu -> new_page=%lu",
        (unsigned long)old_page_no, (unsigned long)single_new_csr_no);
  } else {
    new_page_nos = do_split(old_page_no, merged_graph, disk_manager, buf_pool,
                            vertex_update_sketch, vertex_index, new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                            ,
                            ts_map_ptr
#endif
    );
    BW_GRAPH_LOG_INFO("[SMO] split page=%lu parts=%zu",
        (unsigned long)old_page_no, new_page_nos.size());
  }

  // Install the forwarding table on the OLD page_map entry BEFORE we publish
  // the new vertex_index entries.  Putting it on the page_map_entry lets
  // concurrent writers observe the forwarder with the SAME ConcurrentHashMap
  // probe they already need for insert_delta. Shared ownership keeps the
  // table alive until every writer that observed it has finished.
  auto forwarder = std::make_shared<page_delta_t>();
  forwarder->forward_map_.reserve(new_locations.size());
  for (const auto& [v_id, loc] : new_locations) {
    forwarder->forward_map_.emplace(v_id, loc.first);
  }
  page_map->install_forwarder_for(old_page_no, forwarder);

  // Drain stray deltas and reroute them to the new CSR pages.  Strays are
  // records written between fetch_deltas_with_mark and the forwarder being
  // installed, OR records written by code paths that did not consult the
  // forwarder.  Each is rerouted to the matching new CSR page.
  std::vector<delta_record_t> stray = page_map->fetch_deltas_with_mark(old_page_no);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Deferred records are rerouted to the new pages alongside strays.
  stray.insert(stray.end(), deferred.begin(), deferred.end());
#endif
  for (delta_record_t& delta : stray) {
    if (delta.target != EDGE) {
      continue;
    }
    auto new_loc_it = new_locations.find(delta.first);
    if (new_loc_it == new_locations.end()) {
      throw std::runtime_error("SMO stray delta source is missing from replacement pages");
    }

    // Acquire the vertex write latch to safely prepend into delta_heads_.
    vertex_index->v_w_lock(delta.first);

    delta_head_t cur_head = vertex_index->delta_heads_[delta.first];
    delta.next_page_no = cur_head.page_no;
    delta.next_record_idx = cur_head.record_idx;

    auto [d_page_no, d_record_idx] =
        page_map->insert_delta_and_get_loc(new_loc_it->second.first, delta);

    vertex_index->delta_heads_[delta.first] = {d_page_no, d_record_idx};
    forwarder->note_rerouted_tail(delta.first, {d_page_no, d_record_idx});

    vertex_index->v_w_unlock(delta.first);
  }

  pending_version_switch_t pending;
  pending.new_locations = std::move(new_locations);
  pending.forwarder = std::move(forwarder);
  pending.giant_vertices = std::move(giant_vertices);
  pending.page_map = page_map;
  pending.disk_manager = disk_manager;
  pending.buf_pool = buf_pool;
  pending.vertex_index = vertex_index;
  pending.old_page_no = old_page_no;
  pending.old_parent_no = old_parent_no;
  pending.need_split = need_split;
  pending.single_new_csr_no = single_new_csr_no;
  pending.new_page_nos = std::move(new_page_nos);
  if (need_split && index_page_map_ != nullptr && old_parent_no != 0) {
    pending.merged_graph = std::move(merged_graph);
  }
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  pending.set_page_snapshot_ts = txn_manager_ != nullptr;
  pending.page_snapshot_ts = txn_watermark;
#endif

  if (manual_version_switch_) {
    std::lock_guard<std::mutex> guard(pending_version_switches_mutex_);
    pending_version_switches_.push_back(std::move(pending));
  } else {
    publish_version_switch(pending);
  }

  {
    auto _smo_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - _smo_t0_).count();
    BW_GRAPH_LOG_INFO("[SMO] done page=%lu elapsed_ms=%lld",
        (unsigned long)old_page_no, (long long)_smo_ms_);
  }

  // Trigger async flush for the new dirty CSR pages produced by this SMO.
  if (csr_flusher_ != nullptr) {
    csr_flusher_->trigger_async_flush();
  }
}

// Consolidate pages async.
std::future<void> smo_ctl_t::consolidate_pages_async(csr_page_t* old_page, page_map_t* page_map,
                                                     disk_manager_t* disk_manager,
                                                     buf_pool_t<csr_page_t>* buf_pool,
                                                     vertex_index_t* vertex_index,
                                                     giant_vertex_db_t* giant_db,
                                                     vertex_update_sketch_t* vertex_update_sketch,
                                                     uint64_t requested_threshold,
                                                     bool old_page_pinned,
                                                     bool consolidation_claimed) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();

  // Consolidation disabled: return an already-satisfied future without
  // touching the SMO arena (which is not allocated when max_threads_ == 0).
  if (max_threads_ == 0) {
    (void) old_page;
    (void) page_map;
    (void) disk_manager;
    (void) buf_pool;
    (void) vertex_index;
    (void) giant_db;
    (void) vertex_update_sketch;
    (void) requested_threshold;
    if (consolidation_claimed && old_page != nullptr && page_map != nullptr) {
      page_map->end_consolidation_for(old_page->get_page_no());
    }
    if (old_page_pinned && old_page != nullptr) {
      old_page->dec_pin_count();
    }
    promise->set_value();
    return future;
  }

  pending_consolidations_.fetch_add(1);

  smo_arena_->enqueue([this, old_page, page_map, disk_manager, buf_pool, vertex_index, giant_db,
                       vertex_update_sketch, requested_threshold, old_page_pinned,
                       consolidation_claimed, promise]() {
    task_group_->run([this, old_page, page_map, disk_manager, buf_pool, vertex_index, giant_db,
                      vertex_update_sketch, requested_threshold, old_page_pinned,
                      consolidation_claimed, promise]() {
      try {
        consolidate_pages(old_page, page_map, disk_manager, buf_pool, vertex_index, giant_db,
                          vertex_update_sketch, requested_threshold, old_page_pinned,
                          consolidation_claimed);
        promise->set_value();
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
      pending_consolidations_.fetch_sub(1);
    });
  });

  return future;
}
