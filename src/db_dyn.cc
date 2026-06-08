#include "bw_graph/buf/page_map.h"
#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/db.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <folly/ConcurrentBitSet.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <iostream>
#include <kaminpar.h>
#include <math.h>
#include <nlohmann/json.hpp>
#include <sys/types.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for_each.h>
#include <tbb/spin_mutex.h>
#include <vector>

// Insert an edge directly into the buffer pool
bool bw_graph_db_t::insert_edge_direct(v_id_t src, v_id_t dst) {
  // TODO
  // Step 1 - Get the vertex location from vertex index
  protected_vertex_version_t src_version = this->vertex_index->protect_vertex_version(src);
  vertex_loc_t src_loc = src_version.location();

  // Step 2 - Fetch the page;
  csr_page_t* target_page =
      this->buffer_pool->buf_page_read_for_slot_write(src_loc.first, this->disk_manager);
  src_version.reset();
  insert_status_t status = target_page->insert_edge_in_slot(src_loc.second, src, dst);
  target_page->w_unlatch();
  if (status == INSERTED) {
    return true;
  } else if (status == SLOT_FULL) {
    // Call insert edge to delta pages;
    return this->insert_edge(src, dst);
  } else {
    // This branch are illegal;
    return false;
  }
}

// Insert an edge
bool bw_graph_db_t::insert_edge(v_id_t src, v_id_t dst) {
  // Step 1: Validate vertices.
  if (src >= this->vertex_index->items.size() || dst >= this->vertex_index->items.size() ||
      this->vertex_index->items[src].is_delete || this->vertex_index->items[dst].is_delete) {
    return false;
  }

  if (this->vertex_update_sketch_ != nullptr) {
    this->vertex_update_sketch_->record_write(src);
  }

  // Link the delta record and publish it as the new per-vertex head.
  delta_record_t new_delta_record(EDGE, INSERT, src, dst);

  this->vertex_index->v_w_lock(src);
  protected_vertex_version_t src_version = this->vertex_index->protect_vertex_version(src);
  vertex_loc_t src_loc = src_version.location();

  delta_head_t cur_head = this->vertex_index->delta_heads_[src];
  new_delta_record.next_page_no = cur_head.page_no;
  new_delta_record.next_record_idx = cur_head.record_idx;

  // Forwarder-aware insert: the page_map probe needed to locate the source
  // entry doubles as a fast-path check for an in-flight consolidation; on
  // hit the record is routed onto the new CSR page instead.
  bool rerouted = false;
  auto [d_page_no, d_record_idx] = this->page_map_table->insert_delta_with_forward(
      src_loc.first, src, new_delta_record, rerouted);
  this->vertex_index->delta_heads_[src] = {d_page_no, d_record_idx};
  this->vertex_index->v_w_unlock(src);

  // Step 4: Trigger SMO when delta chain exceeds the adaptive threshold.
  // The threshold is workload-driven (see vertex_update_sketch_t):
  //   - read-heavy vertices fire at chain length 1
  //   - write-heavy vertices wait longer than the yaml baseline
  // Falls back to BW_GRAPH_MAX_DELTA_CHAIN_LENGTH when sketch is unset or
  // when the vertex has too few samples to draw conclusions.
  uint64_t trigger_threshold = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
  if (this->vertex_update_sketch_ != nullptr) {
    trigger_threshold = this->vertex_update_sketch_->get_consolidation_threshold(
        src, bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH);
  }
  if (this->page_map_table->get_delta_chain_length(src_loc.first) < trigger_threshold) {
    return true;
  }

  // Skip consolidation entirely when SMO workers are disabled.
  if (!this->smo_ctl->is_enabled()) {
    return true;
  }

  // Forwarding active: another SMO is already consolidating this page.
  if (rerouted) {
    return true;
  }

  if (!this->page_map_table->try_begin_consolidation_for(src_loc.first)) {
    return true;
  }

  csr_page_t* old_page = nullptr;
  bool old_page_pinned = false;
  bool task_submitted = false;
  try {
    old_page = this->buffer_pool->buf_page_read(src_loc.first, this->disk_manager);
    old_page->inc_pin_count();
    old_page_pinned = true;
    old_page->r_unlatch();
    this->smo_ctl->consolidate_pages_async(old_page, this->page_map_table, this->disk_manager,
                                           this->buffer_pool, this->vertex_index, this->giant_db,
                                           this->vertex_update_sketch_, trigger_threshold, true,
                                           true);
    task_submitted = true;
  } catch (...) {
    if (!task_submitted) {
      if (old_page_pinned && old_page != nullptr) {
        old_page->dec_pin_count();
      }
      this->page_map_table->end_consolidation_for(src_loc.first);
    }
    throw;
  }
  return true;
}

// Delete an edge;
bool bw_graph_db_t::delete_edge(v_id_t src, v_id_t dst) {
  // Step 1: Validate vertices
  if (src >= this->vertex_index->items.size() || dst >= this->vertex_index->items.size()) {
    return false;
  }
  this->vertex_update_sketch_->record_write(src);

  // Step 3: Under per-vertex write latch, link DELETE record into delta chain.
  delta_record_t new_delta_record(EDGE, DELETE, src, dst);

  this->vertex_index->v_w_lock(src);
  protected_vertex_version_t src_version = this->vertex_index->protect_vertex_version(src);
  vertex_loc_t src_loc = src_version.location();

  delta_head_t cur_head = this->vertex_index->delta_heads_[src];
  new_delta_record.next_page_no = cur_head.page_no;
  new_delta_record.next_record_idx = cur_head.record_idx;

  // Forwarder-aware insert: see insert_edge for the rationale.
  bool rerouted = false;
  auto [d_page_no, d_record_idx] = this->page_map_table->insert_delta_with_forward(
      src_loc.first, src, new_delta_record, rerouted);
  (void) rerouted;
  this->vertex_index->delta_heads_[src] = {d_page_no, d_record_idx};
  this->vertex_index->v_w_unlock(src);

  return true;
}

#ifdef BW_GRAPH_ENABLE_TRANSACTION

// Insert edge txn.
bool bw_graph_db_t::insert_edge_txn(v_id_t src, v_id_t dst, timestamp_t begin_ts,
                                    timestamp_t commit_ts) {
  if (src >= this->vertex_index->items.size() || dst >= this->vertex_index->items.size() ||
      this->vertex_index->items[src].is_delete || this->vertex_index->items[dst].is_delete) {
    return false;
  }

  if (this->vertex_update_sketch_ != nullptr) {
    this->vertex_update_sketch_->record_write(src);
  }

  delta_record_t new_delta_record(EDGE, INSERT, src, dst);
  new_delta_record.begin_ts = begin_ts;
  new_delta_record.commit_ts = commit_ts;

  this->vertex_index->v_w_lock(src);
  protected_vertex_version_t src_version = this->vertex_index->protect_vertex_version(src);
  vertex_loc_t src_loc = src_version.location();

  delta_head_t cur_head = this->vertex_index->delta_heads_[src];
  new_delta_record.next_page_no = cur_head.page_no;
  new_delta_record.next_record_idx = cur_head.record_idx;

  // Forwarder-aware insert: see insert_edge for the rationale.
  bool rerouted = false;
  auto [d_page_no, d_record_idx] = this->page_map_table->insert_delta_with_forward(
      src_loc.first, src, new_delta_record, rerouted);
  this->vertex_index->delta_heads_[src] = {d_page_no, d_record_idx};
  this->vertex_index->v_w_unlock(src);

  // Workload-driven adaptive trigger; see insert_edge for the rationale.
  uint64_t trigger_threshold = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
  if (this->vertex_update_sketch_ != nullptr) {
    trigger_threshold = this->vertex_update_sketch_->get_consolidation_threshold(
        src, bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH);
  }
  if (this->page_map_table->get_delta_chain_length(src_loc.first) < trigger_threshold) {
    return true;
  }

  // Skip consolidation entirely when SMO workers are disabled.
  if (!this->smo_ctl->is_enabled()) {
    return true;
  }

  // Forwarding active: another SMO is already consolidating this page.
  if (rerouted) {
    return true;
  }

  if (!this->page_map_table->try_begin_consolidation_for(src_loc.first)) {
    return true;
  }

  csr_page_t* old_page = nullptr;
  bool old_page_pinned = false;
  bool task_submitted = false;
  try {
    old_page = this->buffer_pool->buf_page_read(src_loc.first, this->disk_manager);
    old_page->inc_pin_count();
    old_page_pinned = true;
    old_page->r_unlatch();
    this->smo_ctl->consolidate_pages_async(old_page, this->page_map_table, this->disk_manager,
                                           this->buffer_pool, this->vertex_index, this->giant_db,
                                           this->vertex_update_sketch_, trigger_threshold, true,
                                           true);
    task_submitted = true;
  } catch (...) {
    if (!task_submitted) {
      if (old_page_pinned && old_page != nullptr) {
        old_page->dec_pin_count();
      }
      this->page_map_table->end_consolidation_for(src_loc.first);
    }
    throw;
  }
  return true;
}

// Delete edge txn.
bool bw_graph_db_t::delete_edge_txn(v_id_t src, v_id_t dst, timestamp_t begin_ts,
                                    timestamp_t commit_ts) {
  if (src >= this->vertex_index->items.size() || dst >= this->vertex_index->items.size()) {
    return false;
  }

  if (this->vertex_update_sketch_ != nullptr) {
    this->vertex_update_sketch_->record_write(src);
  }

  delta_record_t new_delta_record(EDGE, DELETE, src, dst);
  new_delta_record.begin_ts = begin_ts;
  new_delta_record.commit_ts = commit_ts;

  this->vertex_index->v_w_lock(src);
  protected_vertex_version_t src_version = this->vertex_index->protect_vertex_version(src);
  vertex_loc_t src_loc = src_version.location();

  delta_head_t cur_head = this->vertex_index->delta_heads_[src];
  new_delta_record.next_page_no = cur_head.page_no;
  new_delta_record.next_record_idx = cur_head.record_idx;

  // Forwarder-aware insert: see insert_edge for the rationale.
  bool rerouted = false;
  auto [d_page_no, d_record_idx] = this->page_map_table->insert_delta_with_forward(
      src_loc.first, src, new_delta_record, rerouted);
  (void) rerouted;
  this->vertex_index->delta_heads_[src] = {d_page_no, d_record_idx};
  this->vertex_index->v_w_unlock(src);

  return true;
}

#endif // BW_GRAPH_ENABLE_TRANSACTION

// Consolidate all pages.
void bw_graph_db_t::consolidate_all_pages() {
  // Visit every page tracked by the page-map table.
  for (auto& [page_no, map_entry] : this->page_map_table->get_inner_page_map()) {
    // Read and release the current page frame.
    csr_page_t* old_page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);
    old_page->r_unlatch();
  }
}

// Wait for pending consolidations.
void bw_graph_db_t::wait_for_pending_consolidations() { this->smo_ctl->wait_all_consolidations(); }

// Delete vertex.
bool bw_graph_db_t::delete_vertex(v_id_t vertex_id, bool remove_neighbors) {
  // Step 1. Check vertex exists.
  if (vertex_id >= this->get_vertex_count() || this->vertex_index->items[vertex_id].is_delete) {
    return false;
  }

  // Step 2. Remove directly.
  this->vertex_index->v_w_lock(vertex_id);
  this->vertex_index->items[vertex_id].is_delete = true;
  this->vertex_index->v_w_unlock(vertex_id);
  if (remove_neighbors) {
    // Get neighbors.
    if (!this->vertex_index->is_giant_vertex(vertex_id)) {
      // Get neighbor
      auto [removed_neighbors, related_paged_csr] = this->read_neighbor(vertex_id);
      related_paged_csr->r_unlatch();
      // Remove neighbor one by one.
      for (v_id_t removed_neighbor : removed_neighbors) {
        this->delete_edge(vertex_id, removed_neighbor);
        this->delete_edge(removed_neighbor, vertex_id);
      }
    }
  }
  return true;
}

// Insert single vertex.
bool bw_graph_db_t::insert_single_vertex(v_id_t vertex_id) {
  // Step 1. Check whether this vertex exsits.
  if (vertex_id < this->get_vertex_count() && !this->vertex_index->items[vertex_id].is_delete) {
    return false;
  }
  // Step 2. If this vertex is deleted previously.
  if (vertex_id < this->get_vertex_count() && this->vertex_index->items[vertex_id].is_delete) {
    this->vertex_index->items[vertex_id].is_delete = false;
    return true;
  }

  // Step 3. This vertex never exists before.
  // Just push it into vertex buffer (keep delta_heads_ in sync).
  this->vertex_index->items.emplace_back(0, 0);
  this->vertex_index->delta_heads_.emplace_back();
  return true;
}

// Insert vertex with neighbors.
v_id_t bw_graph_db_t::insert_vertex_with_neighbors(std::vector<v_id_t>& neighbors) {
  // Step 1 - Allocate a new vertex ID;
  v_id_t new_vertex_id = this->vertex_index->allocate_new_vertex();
  // Step 2 - Arrange a new page id for this vertex ramdomly;
  folly::F14FastMap<page_no_t, uint32_t> page_count;
  page_no_t best_page = 0;
  uint32_t max_count = 0;
  for (v_id_t neighbor : neighbors) {
    vertex_loc_t neighbor_loc = this->vertex_index->get_vertex_location(neighbor);
    page_no_t neighbor_page_no = neighbor_loc.first;
    auto [it, inserted] = page_count.try_emplace(neighbor_page_no, 0);
    it->second++;
    if (it->second > max_count) {
      max_count = it->second;
      best_page = neighbor_page_no;
    }
  }
  this->vertex_index->update_vertex_loc(new_vertex_id, best_page, 0);
  // Step 3 - Insert edges;
  for (v_id_t neighbor : neighbors) {
    this->insert_edge(new_vertex_id, neighbor);
  }
  return new_vertex_id;
}

// Insert vertex batch.
bool bw_graph_db_t::insert_vertex_batch(
    folly::F14FastMap<v_id_t, std::vector<v_id_t>>& vertex_batch) {
  if (vertex_batch.empty()) {
    return true;
  }

  std::cout << "\n=== Batch Insert Vertices ===" << std::endl;
  std::cout << "Inserting " << vertex_batch.size() << " vertices" << std::endl;

  // Step 1: Validate vertices - check if they exist
  v_id_t current_vertex_count = this->vertex_index->items.size();
  for (const auto& [vertex_id, neighbors] : vertex_batch) {
    if (vertex_id < current_vertex_count) {
      std::cerr << "Error: Vertex " << vertex_id << " already exists!" << std::endl;
      return false;
    }
  }

  // Expand vertex index to accommodate new vertices
  v_id_t max_new_vertex_id = 0;
  for (const auto& [vertex_id, neighbors] : vertex_batch) {
    max_new_vertex_id = std::max(max_new_vertex_id, vertex_id);
  }

  if (max_new_vertex_id >= current_vertex_count) {
    size_t new_size = max_new_vertex_id + 1;
    this->vertex_index->items.resize(new_size);
    this->vertex_index->delta_heads_.resize(new_size);
    std::cout << "Expanded vertex index from " << current_vertex_count << " to " << new_size
              << std::endl;
  }

  // Step 2 & 3: Separate giant and normal vertices
  std::vector<std::pair<v_id_t, std::vector<v_id_t>>> normal_vertices;
  size_t giant_count = 0;

  for (auto& [vertex_id, neighbors] : vertex_batch) {
    uint64_t degree = neighbors.size();

    // Check if giant vertex
    if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      // Store in RocksDB
      this->giant_db->insert_vertex(vertex_id, neighbors);
      this->vertex_index->items[vertex_id].vertex_type = GIANT;
      giant_count++;
    } else {
      // Normal vertex: will be packed into pages
      normal_vertices.emplace_back(vertex_id, std::move(neighbors));
    }
  }

  std::cout << "Giant vertices: " << giant_count << std::endl;
  std::cout << "Normal vertices: " << normal_vertices.size() << std::endl;

  if (normal_vertices.empty()) {
    std::cout << "All vertices are giant, batch insert complete." << std::endl;
    return true;
  }

  // CSR Page Layout Constants
  const size_t METADATA_SIZE = 6 * sizeof(uint64_t);
  const size_t NEIGHBOR_BLOCK_SIZE = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);
  const size_t CSR_VERTEX_SIZE = 24;
  const size_t PAGE_SIZE = bw_graph::BW_GRAPH_PAGE_SIZE;

  // Determine neighbor ID size based on compression setting
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  const size_t NEIGHBOR_ID_SIZE = sizeof(uint32_t);
#else
  const size_t NEIGHBOR_ID_SIZE = sizeof(uint64_t);
#endif

  // Current page building buffers
  char* current_page_data = new char[PAGE_SIZE];
  std::vector<uint64_t> current_page_vertex_ids;
  std::vector<uint64_t> current_page_degrees;
  std::vector<std::vector<uint8_t>> current_page_neighbor_data;
  uint64_t current_page_used_bytes = METADATA_SIZE + NEIGHBOR_BLOCK_SIZE;

  page_no_t current_page_no = UINT32_MAX;
  size_t total_pages_created = 0;

  // Helper: Flush current page to disk
  auto flush_page = [&]() {
    if (current_page_vertex_ids.empty()) {
      return;
    }

    // Step 4: Allocate new page (returns byte offset, convert to page_no)
    if (current_page_no == UINT32_MAX) {
      size_t page_offset = this->disk_manager->allocate_page();
      current_page_no = page_offset / bw_graph::BW_GRAPH_PAGE_SIZE;
    }

    // Clear page buffer
    std::memset(current_page_data, 0, PAGE_SIZE);

    uint64_t num_vertices = current_page_vertex_ids.size();
    uint64_t total_edges = 0;
    for (auto degree : current_page_degrees) {
      total_edges += degree;
    }

    // Write Metadata (48 bytes)
    char* ptr = current_page_data;
    *reinterpret_cast<uint64_t*>(ptr) = num_vertices;
    ptr += 8;
    *reinterpret_cast<uint64_t*>(ptr) = total_edges;
    ptr += 8;
    *reinterpret_cast<uint64_t*>(ptr) = 0; // available_slots
    ptr += 8;
    *reinterpret_cast<uint64_t*>(ptr) = current_page_used_bytes;
    ptr += 8;
    *reinterpret_cast<uint64_t*>(ptr) = 0; // parent_page_no
    ptr += 8;
    *reinterpret_cast<uint64_t*>(ptr) = 0; // neighbor_page_count
    ptr += 8;

    // Skip Neighbor Block (256 bytes, already zeroed)
    ptr += NEIGHBOR_BLOCK_SIZE;

    // Write Vertex Array
    uint64_t neighbor_data_offset = 0;
    for (size_t i = 0; i < num_vertices; ++i) {
      *reinterpret_cast<uint64_t*>(ptr) = current_page_vertex_ids[i];
      ptr += 8;
      *reinterpret_cast<uint64_t*>(ptr) = current_page_degrees[i];
      ptr += 8;
      *reinterpret_cast<uint64_t*>(ptr) = neighbor_data_offset;
      ptr += 8;

      neighbor_data_offset += current_page_neighbor_data[i].size();
    }

    // Write Neighbor Data
    for (size_t i = 0; i < num_vertices; ++i) {
      const auto& neighbor_bytes = current_page_neighbor_data[i];
      std::memcpy(ptr, neighbor_bytes.data(), neighbor_bytes.size());
      ptr += neighbor_bytes.size();
    }

    // Step 5: Flush to disk (write_page expects page_no)
    this->disk_manager->write_page(current_page_no, current_page_data);

    std::cout << "Created Page " << current_page_no << ": " << num_vertices << " vertices, "
              << total_edges << " edges, " << current_page_used_bytes << " / " << PAGE_SIZE
              << " bytes (" << std::fixed << std::setprecision(1)
              << (100.0 * current_page_used_bytes / PAGE_SIZE) << "%)" << std::endl;

    // Reset for next page
    current_page_vertex_ids.clear();
    current_page_degrees.clear();
    current_page_neighbor_data.clear();
    current_page_used_bytes = METADATA_SIZE + NEIGHBOR_BLOCK_SIZE;
    current_page_no = UINT32_MAX;
    total_pages_created++;
  };

  // Step 3: Process normal vertices and pack into pages
  for (auto& [vertex_id, neighbors] : normal_vertices) {
    uint64_t degree = neighbors.size();

    // Prepare neighbor data - no compression, just raw bytes
    std::vector<uint8_t> neighbor_bytes;
    neighbor_bytes.resize(degree * NEIGHBOR_ID_SIZE);

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
    // Store as uint32_t
    uint32_t* neighbor_ptr = reinterpret_cast<uint32_t*>(neighbor_bytes.data());
    for (size_t i = 0; i < degree; ++i) {
      neighbor_ptr[i] = static_cast<uint32_t>(neighbors[i]);
    }
#else
    // Store as uint64_t
    uint64_t* neighbor_ptr = reinterpret_cast<uint64_t*>(neighbor_bytes.data());
    for (size_t i = 0; i < degree; ++i) {
      neighbor_ptr[i] = neighbors[i];
    }
#endif

    // Calculate space needed
    uint64_t space_needed = CSR_VERTEX_SIZE + neighbor_bytes.size();

    // Check if page has space
    if (current_page_used_bytes + space_needed > PAGE_SIZE) {
      flush_page();
    }

    // Allocate page number if needed (convert offset to page_no)
    if (current_page_no == UINT32_MAX) {
      size_t page_offset = this->disk_manager->allocate_page();
      current_page_no = page_offset / bw_graph::BW_GRAPH_PAGE_SIZE;
    }

    // Add to current page
    uint64_t vertex_offset_in_page = current_page_vertex_ids.size();
    current_page_vertex_ids.push_back(vertex_id);
    current_page_degrees.push_back(degree);
    current_page_neighbor_data.push_back(std::move(neighbor_bytes));
    current_page_used_bytes += space_needed;

    // Update vertex index
    this->vertex_index->items[vertex_id].vertex_type = NORMAL;
    this->vertex_index->update_vertex_loc(vertex_id, current_page_no, vertex_offset_in_page);
  }

  // Flush last page
  flush_page();

  // Cleanup
  delete[] current_page_data;

  std::cout << "Batch insert complete: " << total_pages_created << " pages created" << std::endl;

  return true;
}
