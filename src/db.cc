#include "bw_graph/db/db.h"

#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/db.h"
#include "bw_graph/db/property_store.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_index.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <folly/ConcurrentBitSet.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <fstream>
#include <indicators/progress_bar.hpp>
#include <iostream>
#include <kaminpar.h>
#include <math.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_reduce.h>
#include <tbb/spin_mutex.h>
#include <utility>
#include <vector>

/**
 * @brief Synchronously flush all dirty delta pages.
 */
size_t bw_graph_db_t::flush_delta_pages() {
  if (delta_flusher_ != nullptr) {
    return delta_flusher_->flush_all();
  }
  // Flusher not active (interval == 0 or not yet started): flush inline.
  if (delta_buffer_pool == nullptr || delta_disk_manager == nullptr) {
    return 0;
  }
  size_t count = 0;
  delta_buffer_pool->traverse_page_map([&](page_no_t /*pno*/, delta_page_t* page) -> bool {
    if (page->is_dirty()) {
      try {
        if (page->flush_to_disk(delta_disk_manager)) {
          ++count;
        }
      } catch (...) {
      }
    }
    return true;
  });
  return count;
}

/**
 * @brief Synchronously flush all dirty CSR pages.
 */
size_t bw_graph_db_t::flush_csr_pages() {
  if (csr_flusher_ != nullptr) {
    return csr_flusher_->flush_all();
  }
  // Flusher not active (interval == 0 or not yet started): flush inline.
  if (buffer_pool == nullptr || disk_manager == nullptr) {
    return 0;
  }
  size_t count = 0;
  buffer_pool->traverse_page_map([&](page_no_t /*pno*/, csr_page_t* page) -> bool {
    if (page->is_dirty()) {
      try {
        if (page->flush_to_disk(disk_manager)) {
          ++count;
        }
      } catch (...) {
      }
    }
    return true;
  });
  return count;
}

#ifdef BW_GRAPH_ENABLE_TRANSACTION

// Initialize txn manager.
void bw_graph_db_t::init_txn_manager(uint64_t initial_ts) {
  if (txn_manager_) {
    std::cerr << "Warning: transaction manager already initialized" << std::endl;
    return;
  }
  txn_manager_ = std::make_unique<txn_manager_t>(initial_ts);
  smo_ctl->set_txn_manager(txn_manager_.get());
}

// Begin txn.
std::unique_ptr<txn_t> bw_graph_db_t::begin_txn(bool serializable) {
  if (!txn_manager_) {
    throw std::runtime_error("Transaction manager not initialized! Call init_txn_manager() first.");
  }
  uint64_t read_ts = txn_manager_->latest_commit_ts();
  return std::make_unique<txn_t>(this, txn_manager_.get(), read_ts, serializable);
}

#endif // BW_GRAPH_ENABLE_TRANSACTION

// Get vertex degree (including giant vertex)
uint64_t bw_graph_db_t::get_vertex_degree(v_id_t vertex_id) {
  if (this->vertex_index->is_giant_vertex(vertex_id)) {
    // If it's a giant vertex, read from the giant vertex database
    return this->giant_db->get_vertex_degree(vertex_id);
  } else {
    // If it's a regular vertex, read from the CSR pages
    // Look up which page contains this vertex
    auto [neighbor_span, current_page] = read_neighbor(vertex_id);
    uint64_t degree = neighbor_span.size();
    current_page->r_unlatch();
    return degree;
  }
}

// Read neighbor zero copy;
std::pair<neighbor_span_t, csr_page_t*> bw_graph_db_t::read_neighbor(v_id_t vertex_id) {
  this->vertex_update_sketch_->record_read(vertex_id);

  this->vertex_index->v_r_lock(vertex_id);
  protected_vertex_version_t version = this->vertex_index->protect_vertex_version(vertex_id);
  page_no_t page_no = version.page_no();
  vertex_type_t vertex_type = this->vertex_index->items[vertex_id].vertex_type;
  uint16_t offset = version.offset();
  csr_page_t* current_page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);
  this->vertex_index->v_r_unlock(vertex_id);
  version.reset();

  // Process giant vertex separately;
  if (vertex_type == GIANT) {
    neighbor_span_t neighbors_span = this->giant_db->read_neighbor(vertex_id);
    return {neighbors_span, current_page};
  }

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  // Decode PTV bytes into a thread-local buffer; safe because callers fully
  // iterate the returned span before making the next read_neighbor call.
  thread_local std::vector<v_id_t> tl_decode_buf;
  tl_decode_buf.clear();
  auto range = current_page->get_neighbor_ptv_range(offset);
  tl_decode_buf.reserve(range.size());
  for (v_id_t id : range) {
    tl_decode_buf.push_back(id);
  }
  return {neighbor_span_t(tl_decode_buf.data(), tl_decode_buf.size()), current_page};
#else
  auto neighbors_span = current_page->get_neighbors(offset);
  // Need to hold this read latch, prevent this page from being evicted;
  return {neighbors_span, current_page};
#endif
}

read_neighbor_with_delta_result_t bw_graph_db_t::read_neighbor_with_delta(v_id_t vertex_id) {
  this->vertex_update_sketch_->record_read(vertex_id);
  this->vertex_index->v_r_lock(vertex_id);
  protected_vertex_version_t version = this->vertex_index->protect_vertex_version(vertex_id);
  const vertex_type_t vertex_type = this->vertex_index->items[vertex_id].vertex_type;
  const uint16_t offset = version.offset();
  csr_page_t* current_page =
      this->buffer_pool->buf_page_read(version.page_no(), this->disk_manager);
  delta_record_ptr_t delta_head_snapshot(this->vertex_index->delta_heads_[vertex_id]);
  this->vertex_index->v_r_unlock(vertex_id);
  version.reset();

  if (vertex_type == GIANT) {
    return {this->giant_db->read_neighbor(vertex_id), delta_record_ptr_t::invalid(), current_page};
  }

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  thread_local std::vector<v_id_t> tl_decode_buf;
  tl_decode_buf.clear();
  auto range = current_page->get_neighbor_ptv_range(offset);
  tl_decode_buf.reserve(range.size());
  for (v_id_t id : range) {
    tl_decode_buf.push_back(id);
  }
  neighbor_span_t neighbors_span(tl_decode_buf.data(), tl_decode_buf.size());
#else
  neighbor_span_t neighbors_span = current_page->get_neighbors(offset);
#endif

  return {neighbors_span, delta_head_snapshot, current_page};
}

// Read neighbor clone.
std::vector<v_id_t> bw_graph_db_t::read_neighbor_clone(v_id_t vertex_id) {
  this->vertex_index->v_r_lock(vertex_id);
  const bool is_giant = this->vertex_index->items[vertex_id].vertex_type == GIANT;
  delta_record_ptr_t cur =
      is_giant ? delta_record_ptr_t(this->vertex_index->delta_heads_[vertex_id])
               : delta_record_ptr_t::invalid();
  this->vertex_index->v_r_unlock(vertex_id);

  neighbor_span_t neighbors_span;
  csr_page_t* csr_page = nullptr;
  std::vector<v_id_t> giant_base;

  if (is_giant) {
    giant_base = this->giant_db->read_neighbor_clone(vertex_id);
  } else {
    std::tie(neighbors_span, cur, csr_page) = this->read_neighbor_with_delta(vertex_id);
  }

  std::vector<v_id_t> delta_inserts;
  folly::F14FastSet<v_id_t> delta_deletes;
  folly::F14FastSet<v_id_t> delta_seen;

  while (cur.is_valid()) {
    delta_page_t* d_page =
        this->delta_buffer_pool->buf_page_read(cur.page_no, this->delta_disk_manager);
    const delta_record_t& rec = d_page->get_record(cur.record_idx);

    if (rec.target == EDGE && delta_seen.insert(rec.second).second) {
      if (rec.type == INSERT) {
        delta_inserts.push_back(rec.second);
      } else { // DELETE
        delta_deletes.insert(rec.second);
      }
    }

    // Follow chain link before releasing the page latch
    delta_record_ptr_t next{rec.next_page_no, rec.next_record_idx};
    d_page->r_unlatch();
    cur = next;
  }

  std::vector<v_id_t> result;
  result.reserve(delta_inserts.size() + 16);

  auto push_csr_nbr = [&](v_id_t nbr) {
    if (delta_deletes.find(nbr) == delta_deletes.end()) {
      result.push_back(nbr);
    }
  };

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  if (is_giant) {
    for (v_id_t nbr : giant_base)
      push_csr_nbr(nbr);
  } else {
    for (v_id_t nbr : neighbors_span)
      push_csr_nbr(nbr);
  }
#else
  for (v_id_t nbr : (is_giant ? neighbor_span_t{giant_base} : neighbors_span)) {
    push_csr_nbr(nbr);
  }
#endif
  if (csr_page != nullptr) {
    csr_page->r_unlatch();
  }

  for (v_id_t nbr : delta_inserts) {
    if (delta_deletes.find(nbr) == delta_deletes.end()) {
      result.push_back(nbr);
    }
  }

  return result;
}

#ifdef BW_GRAPH_ENABLE_TRANSACTION

// Read neighbor clone txn.
std::vector<v_id_t> bw_graph_db_t::read_neighbor_clone_txn(v_id_t vertex_id, timestamp_t read_ts) {
  if (this->vertex_index->items[vertex_id].vertex_type == GIANT) {
    // Base giant-db neighbors: filter by per-edge commit_ts stored in property_store.
    // Edges without a property entry were loaded at startup -> INVALID_TS -> always visible.
    std::vector<v_id_t> base = this->giant_db->read_neighbor_clone(vertex_id);
    std::vector<v_id_t> result;
    result.reserve(base.size());
    for (v_id_t nbr : base) {
      auto ts_opt = this->property_store->get_edge_property(vertex_id, nbr, "__ts");
      timestamp_t ets = INVALID_TS;
      if (ts_opt.has_value() && ts_opt->size() == sizeof(timestamp_t)) {
        std::memcpy(&ets, ts_opt->data(), sizeof(timestamp_t));
      }
      if (ets == INVALID_TS || ets <= read_ts) {
        result.push_back(nbr);
      }
    }

    // MVCC-filtered delta traversal for any inserts/deletes recorded in vertex delta chain.
    this->vertex_index->v_r_lock(vertex_id);
    delta_head_t cur = this->vertex_index->delta_heads_[vertex_id];
    this->vertex_index->v_r_unlock(vertex_id);

    std::vector<v_id_t> delta_inserts;
    folly::F14FastSet<v_id_t> delta_deletes;
    while (cur.is_valid()) {
      delta_page_t* d_page =
          this->delta_buffer_pool->buf_page_read(cur.page_no, this->delta_disk_manager);
      const delta_record_t& rec = d_page->get_record(cur.record_idx);
      if (rec.target == EDGE && rec.commit_ts != INVALID_TS && rec.commit_ts <= read_ts) {
        if (rec.type == INSERT)
          delta_inserts.push_back(rec.second);
        else
          delta_deletes.insert(rec.second);
      }
      delta_head_t next{rec.next_page_no, rec.next_record_idx};
      d_page->r_unlatch();
      cur = next;
    }

    // Remove deleted edges, then append visible inserts.
    if (!delta_deletes.empty()) {
      result.erase(std::remove_if(result.begin(), result.end(),
                                  [&](v_id_t n) { return delta_deletes.count(n) > 0; }),
                   result.end());
    }
    for (v_id_t nbr : delta_inserts) {
      if (delta_deletes.find(nbr) == delta_deletes.end())
        result.push_back(nbr);
    }
    return result;
  }

  this->vertex_index->v_r_lock(vertex_id);
  protected_vertex_version_t version = this->vertex_index->protect_vertex_version(vertex_id);
  page_no_t page_no = version.page_no();
  uint16_t offset = version.offset();

  csr_page_t* csr_page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);
  delta_head_t cur = this->vertex_index->delta_heads_[vertex_id];
  this->vertex_index->v_r_unlock(vertex_id);
  version.reset();

  std::vector<v_id_t> delta_inserts;
  folly::F14FastSet<v_id_t> delta_deletes;

  while (cur.is_valid()) {
    delta_page_t* d_page =
        this->delta_buffer_pool->buf_page_read(cur.page_no, this->delta_disk_manager);
    const delta_record_t& rec = d_page->get_record(cur.record_idx);

    // MVCC visibility: only include records committed at or before read_ts.
    if (rec.target == EDGE && rec.commit_ts != INVALID_TS && rec.commit_ts <= read_ts) {
      if (rec.type == INSERT) {
        delta_inserts.push_back(rec.second);
      } else {
        delta_deletes.insert(rec.second);
      }
    }

    delta_head_t next{rec.next_page_no, rec.next_record_idx};
    d_page->r_unlatch();
    cur = next;
  }

  std::vector<v_id_t> result;
  result.reserve(delta_inserts.size() + 16);

  auto push_csr_nbr = [&](v_id_t nbr) {
    if (delta_deletes.find(nbr) == delta_deletes.end()) {
      result.push_back(nbr);
    }
  };

  // Filter CSR edges by per-edge commit_ts: INVALID_TS (0) = always visible.
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  {
    uint32_t j = 0;
    for (v_id_t nbr : csr_page->get_neighbor_ptv_range(offset)) {
      timestamp_t ets = csr_page->get_edge_ts(offset, j++);
      if (ets == INVALID_TS || ets <= read_ts) {
        push_csr_nbr(nbr);
      }
    }
  }
#else
  {
    neighbor_span_t nbrs = csr_page->get_neighbors(offset);
    for (uint32_t j = 0; j < static_cast<uint32_t>(nbrs.size()); ++j) {
      timestamp_t ets = csr_page->get_edge_ts(offset, j);
      if (ets == INVALID_TS || ets <= read_ts) {
        push_csr_nbr(nbrs[j]);
      }
    }
  }
#endif
  csr_page->r_unlatch();

  for (v_id_t nbr : delta_inserts) {
    if (delta_deletes.find(nbr) == delta_deletes.end()) {
      result.push_back(nbr);
    }
  }

  return result;
}

#endif // BW_GRAPH_ENABLE_TRANSACTION

// Get vertex count.
uint32_t bw_graph_db_t::get_vertex_count() const { return this->vertex_index->items.size(); }

// Read page vertex.
csr_page_t* bw_graph_db_t::read_page_vertex(v_id_t vertex_id) {
  protected_vertex_version_t version = this->vertex_index->protect_vertex_version(vertex_id);
  csr_page_t* page = this->buffer_pool->buf_page_read(version.page_no(), this->disk_manager);
  version.reset();
  return page;
}

// Output a json file with arbitrary levels;
void bw_graph_db_t::create_db_json() {
  if (this->root_index_page == nullptr) {
    std::cout << "Error: Cannot create JSON file, root index page is missing." << std::endl;
    return;
  }

  // Build the json object to visualize the graph structure
  nlohmann::json graph_json;

  // The top level;
  auto root_desc = this->root_index_page->generate_desc();
  graph_json["top_level"] = {{"page_number", root_desc["page_number"]},
                             {"num_blocks", root_desc["num_blocks"]},
                             {"blocks", root_desc["blocks"]},
                             {"level", this->level_count_},
                             {"children", nlohmann::json::array()}};

  if (this->level_count_ == 2) {
    // Special handling for 2-level structure: Root -> CSR pages directly
    std::cout << "Processing 2-level structure: Root -> CSR pages" << std::endl;

    for (auto& child_page_no : this->root_index_page->get_blocks()) {
      csr_page_t* leaf_page = this->buffer_pool->buf_page_read(child_page_no, this->disk_manager);

      auto leaf_desc = leaf_page->generate_desc();
      leaf_page->r_unlatch();

      nlohmann::json leaf_node = {{"type", leaf_desc["type"]},
                                  {"page_number", leaf_desc["page_number"]},
                                  {"vertex_count", leaf_desc["vertex_count"]},
                                  {"edge_count", leaf_desc["edge_count"]},
                                  {"vertices", leaf_desc["vertices"]},
                                  {"parent_page", leaf_desc["parent_page_no"]},
                                  {"neighbor_page_count", leaf_desc["neighbor_page_count"]},
                                  {"neighbor_pages", leaf_desc["neighbor_pages"]},
                                  {"level", 1}};

      graph_json["top_level"]["children"].push_back(leaf_node);
    }
  } else {
    // Multi-level structure processing with queue
    std::cout << "Processing multi-level structure with " << this->level_count_ << " levels"
              << std::endl;

    // Use a queue to process all levels iteratively
    std::queue<std::pair<nlohmann::json*, index_page_t*>> processing_queue;
    processing_queue.push({&graph_json["top_level"]["children"], this->root_index_page});

    int current_level = this->level_count_;

    while (!processing_queue.empty() && current_level > 1) {
      int queue_size = processing_queue.size();

      // Process all nodes at current level
      for (int i = 0; i < queue_size; i++) {
        auto [parent_array, current_page] = processing_queue.front();
        processing_queue.pop();

        for (auto& child_page_no : current_page->get_blocks()) {
          if (current_level > 2) {
            // This should be an index page (multi-level case)
            auto index_it = this->index_page_map.find(child_page_no);

            if (index_it != this->index_page_map.end()) {
              index_page_t* child_index_page = index_it->second;
              auto child_desc = child_index_page->generate_desc();

              nlohmann::json child_node = {
                  {"page_number", child_desc["page_number"]},
                  {"num_blocks", child_desc["num_blocks"]},
                  {"blocks", child_desc["blocks"]},
                  {"level", current_level - 1},
                  {"parent_page", child_desc["parent_page_no"]},
                  {"neighbor_page_count", child_desc["neighbor_page_count"]},
                  {"neighbor_pages", child_desc["neighbor_pages"]},
                  {"children", nlohmann::json::array()}};

              parent_array->push_back(child_node);

              // Add to queue for next level processing
              processing_queue.push({&parent_array->back()["children"], child_index_page});
            }
          } else {
            // This is the leaf level (csr_page_t) - current_level == 2
            csr_page_t* leaf_page =
                this->buffer_pool->buf_page_read(child_page_no, this->disk_manager);

            auto leaf_desc = leaf_page->generate_desc();
            leaf_page->r_unlatch();

            nlohmann::json leaf_node = {{"type", leaf_desc["type"]},
                                        {"page_number", leaf_desc["page_number"]},
                                        {"vertex_count", leaf_desc["vertex_count"]},
                                        {"edge_count", leaf_desc["edge_count"]},
                                        {"vertices", leaf_desc["vertices"]},
                                        {"parent_page", leaf_desc["parent_page_no"]},
                                        {"neighbor_page_count", leaf_desc["neighbor_page_count"]},
                                        {"neighbor_pages", leaf_desc["neighbor_pages"]},
                                        {"level", 1}};

            parent_array->push_back(leaf_node);
          }
        }
      }
      current_level--;
    }
  }

  std::cout << "Multi-level Tree (" << this->level_count_ << " levels) for " << this->graph_name_
            << " saving;" << std::endl;
  std::cout << "Index Page Capacity: " << bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS
            << " blocks per index page." << std::endl;
  std::filesystem::create_directories("./output");

  std::ofstream j_file("./output/" + this->graph_name_ + ".json");
  if (j_file.is_open()) {
    j_file << graph_json.dump(4);
    j_file.close();
    std::cout << "JSON saved to ./output/" + this->graph_name_ + ".json" << std::endl;
  } else {
    std::cerr << "Error: Cannot create file ./output/" + this->graph_name_ + ".json" << std::endl;
  }
}

// Save index pages to file.
bool bw_graph_db_t::save_index_pages_to_file(const std::string& file_path, size_t buffer_size) {
  std::ofstream ofs(file_path, std::ios::binary);
  if (!ofs.is_open()) {
    return false;
  }

  // Step 1. Count valid pages first
  uint64_t page_count = 0;
  for (const auto& [page_no, index_page_ptr] : this->index_page_map) {
    if (index_page_ptr != nullptr) {
      page_count++;
    }
  }

  // Step 2. Write file header
  page_no_t root_page_no = (root_index_page != nullptr) ? root_index_page->get_page_no()
                                                        : bw_graph::BW_GRAPH_DEFAULT_PAGE_NO;

  ofs.write(reinterpret_cast<const char*>(&page_count), sizeof(uint64_t));
  ofs.write(reinterpret_cast<const char*>(&root_page_no), sizeof(page_no_t));
  ofs.write(reinterpret_cast<const char*>(&this->level_count_), sizeof(uint64_t));
  std::cout << "Saving " << page_count << " index pages to " << file_path << std::endl;

  // Step 3. Calculate buffer capacity
  const size_t page_entry_size = sizeof(page_no_t) + bw_graph::BW_GRAPH_INDEX_PAGE_SIZE;
  const size_t pages_per_buffer = buffer_size / page_entry_size;

  if (pages_per_buffer == 0) {
    return false; // Buffer too small
  }

  // Step 4. Allocate write buffer
  std::vector<char> write_buffer;
  write_buffer.reserve(buffer_size);

  // Step 5. Write index pages in batches
  uint64_t pages_written = 0;
  for (const auto& [page_no, index_page_ptr] : this->index_page_map) {
    if (index_page_ptr == nullptr) {
      continue;
    }

    // Ensure the page is parsed
    if (!index_page_ptr->is_parsed_) {
      index_page_ptr->self_parse();
    }

    // Append page_no to buffer
    const char* page_no_ptr = reinterpret_cast<const char*>(&page_no);
    write_buffer.insert(write_buffer.end(), page_no_ptr, page_no_ptr + sizeof(page_no_t));

    // Append page data to buffer
    char* page_data = index_page_ptr->get_data();
    write_buffer.insert(write_buffer.end(), page_data,
                        page_data + bw_graph::BW_GRAPH_INDEX_PAGE_SIZE);

    pages_written++;

    // Flush buffer when full or at last page
    if (write_buffer.size() >= buffer_size - page_entry_size || pages_written == page_count) {
      ofs.write(write_buffer.data(), write_buffer.size());
      write_buffer.clear();
    }
  }

  ofs.close();

  // Step 6. Verify written count matches expected count
  if (pages_written != page_count) {
    std::cout << "Warning: Expected to write " << page_count << " pages but wrote " << pages_written
              << std::endl;
    return false;
  }

  return true;
}

/**
 * @brief Load index pages from a binary file with buffered I/O
 *
 * @param file_path The path to the file from which index pages will be loaded
 * @param index_page_map The map to populate with loaded index pages
 * @param root_index_page Pointer to store the root index page
 * @param level_count Variable to store the level count
 * @param buffer_size The size of the I/O buffer (default: 1MB)
 * @return True if load is successful, false otherwise
 */
bool bw_graph_db_t::load_index_pages_from_file(const std::string& file_path, size_t buffer_size) {
  std::ifstream ifs(file_path, std::ios::binary);
  if (!ifs.is_open()) {
    std::cout << "[In Function] Error: Failed to open file for reading: " << file_path << std::endl;
    return false;
  }

  // Step 1. Read file header
  uint64_t page_count = 0;
  page_no_t root_page_no = bw_graph::BW_GRAPH_DEFAULT_PAGE_NO;

  ifs.read(reinterpret_cast<char*>(&page_count), sizeof(uint64_t));
  ifs.read(reinterpret_cast<char*>(&root_page_no), sizeof(page_no_t));
  ifs.read(reinterpret_cast<char*>(&this->level_count_), sizeof(uint64_t));

  if (ifs.gcount() != sizeof(uint64_t)) {
    std::cout << "Invalid file format: " << file_path << std::endl;
    return false; // Invalid file format
  }

  // Step 2. Clear existing map
  for (auto& [page_no, page_ptr] : index_page_map) {
    if (page_ptr != nullptr) {
      delete page_ptr;
    }
  }
  index_page_map.clear();
  root_index_page = nullptr;

  // Step 3. Calculate buffer capacity
  const size_t page_entry_size = sizeof(page_no_t) + bw_graph::BW_GRAPH_INDEX_PAGE_SIZE;
  const size_t pages_per_buffer = buffer_size / page_entry_size;

  if (pages_per_buffer == 0) {
    std::cout << "Error: Buffer size too small to hold even one page entry." << std::endl;
    return false; // Buffer too small
  }

  // Step 4. Allocate read buffer
  std::vector<char> read_buffer(buffer_size);

  // Step 5. Read and process pages in batches
  uint64_t pages_processed = 0;
  while (pages_processed < page_count) {
    // Calculate how many pages to read in this batch
    uint64_t pages_to_read = (pages_per_buffer < page_count - pages_processed)
                                 ? pages_per_buffer
                                 : (page_count - pages_processed);
    size_t bytes_to_read = pages_to_read * page_entry_size;

    // Read batch from disk
    ifs.read(read_buffer.data(), bytes_to_read);

    if (ifs.gcount() != static_cast<std::streamsize>(bytes_to_read)) {
      // Clean up on error
      for (auto& [page_no, page_ptr] : index_page_map) {
        delete page_ptr;
      }
      index_page_map.clear();
      std::cout << "Error: Unexpected end of file or read error." << std::endl;
      std::cout << "Page Processed: " << pages_processed << ", Expected: " << page_count
                << std::endl;
      return false;
    }

    // Process each page in the buffer
    size_t buffer_offset = 0;
    for (uint64_t i = 0; i < pages_to_read; ++i) {
      // Read page_no
      page_no_t page_no;
      std::memcpy(&page_no, read_buffer.data() + buffer_offset, sizeof(page_no_t));
      buffer_offset += sizeof(page_no_t);

      // Create new index page
      index_page_t* new_page = new index_page_t();
      new_page->set_page_no(page_no);

      // Copy page data
      char* page_data = new_page->get_data();
      std::memcpy(page_data, read_buffer.data() + buffer_offset,
                  bw_graph::BW_GRAPH_INDEX_PAGE_SIZE);
      buffer_offset += bw_graph::BW_GRAPH_INDEX_PAGE_SIZE;

      // Parse the page to restore internal structures
      new_page->self_parse();

      // Insert into map
      index_page_map[page_no] = new_page;

      // Set root page if this is it
      if (page_no == root_page_no) {
        root_index_page = new_page;
      }
    }

    pages_processed += pages_to_read;
  }

  ifs.close();
  return true;
}

// Warm up the buffer by reading valid pages in contiguous ranges.
void bw_graph_db_t::warm_up() {
  std::cout << "Warming Up Database..." << std::endl;

  uint64_t page_count = this->disk_manager->get_page_count();
  std::cout << "Loading " << page_count << " pages..." << std::endl;

  std::atomic<uint64_t> loaded_count{0};

  // Load bar.
  indicators::ProgressBar load_bar{
      indicators::option::BarWidth{50},
      // Start start.
      indicators::option::Start{"["}, indicators::option::Fill{"="}, indicators::option::Lead{">"},
      indicators::option::Remainder{" "}, indicators::option::End{"]"},
      indicators::option::PostfixText{"Loading Pages"},
      indicators::option::ForegroundColor{indicators::Color::green},
      indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
      indicators::option::ShowRemainingTime{true}, indicators::option::MaxProgress{page_count}};

  // Each thread handles a contiguous slice of the page-id range.
  tbb::parallel_for(uint64_t(0), page_count, [&](uint64_t page_no) {
    csr_page_t* page =
        this->buffer_pool->buf_page_read(static_cast<page_no_t>(page_no), this->disk_manager);
    page->r_unlatch();

    uint64_t current = loaded_count.fetch_add(1) + 1;
    if (current % 1000 == 0 || current == page_count) {
      load_bar.set_progress(current);
    }
  });

  load_bar.mark_as_completed();
  std::cout << std::endl;
  std::cout << "Loading pages...[Ok]" << std::endl;

  // Collect giant vertex IDs via a single O(vertex_count) scan.
  std::vector<v_id_t> giant_vertices;
  v_id_t vertex_count = this->get_vertex_count();
  for (v_id_t vid = 0; vid < vertex_count; ++vid) {
    if (this->vertex_index->is_giant_vertex(vid)) {
      giant_vertices.push_back(vid);
    }
  }

  if (!giant_vertices.empty()) {
    std::cout << "Pre-loading " << giant_vertices.size() << " giant vertices..." << std::endl;

    std::atomic<uint64_t> giant_loaded{0};
    uint64_t giant_total = giant_vertices.size();

    indicators::ProgressBar giant_bar{
        indicators::option::BarWidth{50},
        // Start start.
        indicators::option::Start{"["}, indicators::option::Fill{"="},
        indicators::option::Lead{">"}, indicators::option::Remainder{" "},
        indicators::option::End{"]"}, indicators::option::PostfixText{"Loading Giant Vertices"},
        indicators::option::ForegroundColor{indicators::Color::yellow},
        indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
        indicators::option::ShowRemainingTime{true}, indicators::option::MaxProgress{giant_total}};

    tbb::parallel_for(size_t(0), giant_vertices.size(), [&](size_t i) {
      this->giant_db->read_neighbor(giant_vertices[i]);
      uint64_t current = giant_loaded.fetch_add(1) + 1;
      if (current % 10 == 0 || current == giant_total) {
        giant_bar.set_progress(current);
      }
    });

    giant_bar.mark_as_completed();
    std::cout << std::endl;
    std::cout << "Pre-loading giant vertices...[Ok]" << std::endl;
  }

  std::cout << "Database Warmup...[Ok]" << std::endl;
}

// Initialize edge count.
void bw_graph_db_t::init_edge_count() {
  uint64_t page_count = this->disk_manager->get_page_count();

  uint64_t count = tbb::parallel_reduce(
      tbb::blocked_range<uint64_t>(0, page_count), uint64_t(0),
      [&](const tbb::blocked_range<uint64_t>& range, uint64_t local) -> uint64_t {
        for (uint64_t pno = range.begin(); pno != range.end(); ++pno) {
          csr_page_t* page =
              this->buffer_pool->buf_page_read(static_cast<page_no_t>(pno), this->disk_manager);
          vertex_span_t verts = page->get_vertices();
          for (const csr_vertex_t& v : verts) {
            if (!v.is_deleted()) {
              local += v.degree;
            }
          }
          page->r_unlatch();
        }
        return local;
      },
      [](uint64_t a, uint64_t b) -> uint64_t { return a + b; });

  total_edge_count_.store(count, std::memory_order_relaxed);
}

bool bw_graph_db_t::set_vertex_property(v_id_t vertex_id, const std::string& prop_key,
                                        const std::string& prop_value) {
  return property_store->set_vertex_property(vertex_id, prop_key, prop_value);
}

// Get vertex property.
std::optional<std::string> bw_graph_db_t::get_vertex_property(v_id_t vertex_id,
                                                              const std::string& prop_key) {
  return property_store->get_vertex_property(vertex_id, prop_key);
}

// Delete vertex property.
bool bw_graph_db_t::delete_vertex_property(v_id_t vertex_id, const std::string& prop_key) {
  return property_store->delete_vertex_property(vertex_id, prop_key);
}

// Get all vertex properties.
std::unordered_map<std::string, std::string>
bw_graph_db_t::get_all_vertex_properties(v_id_t vertex_id) {
  return property_store->get_all_vertex_properties(vertex_id);
}

// Delete all vertex properties.
bool bw_graph_db_t::delete_all_vertex_properties(v_id_t vertex_id) {
  return property_store->delete_all_vertex_properties(vertex_id);
}

// Set vertex properties.
bool bw_graph_db_t::set_vertex_properties(
    v_id_t vertex_id, const std::unordered_map<std::string, std::string>& properties) {
  return property_store->set_vertex_properties(vertex_id, properties);
}

// Set edge property.
bool bw_graph_db_t::set_edge_property(v_id_t src_id, v_id_t dst_id, const std::string& prop_key,
                                      const std::string& prop_value) {
  return property_store->set_edge_property(src_id, dst_id, prop_key, prop_value);
}

// Get edge property.
std::optional<std::string> bw_graph_db_t::get_edge_property(v_id_t src_id, v_id_t dst_id,
                                                            const std::string& prop_key) {
  return property_store->get_edge_property(src_id, dst_id, prop_key);
}

// Delete edge property.
bool bw_graph_db_t::delete_edge_property(v_id_t src_id, v_id_t dst_id,
                                         const std::string& prop_key) {
  return property_store->delete_edge_property(src_id, dst_id, prop_key);
}

// Get all edge properties.
std::unordered_map<std::string, std::string> bw_graph_db_t::get_all_edge_properties(v_id_t src_id,
                                                                                    v_id_t dst_id) {
  return property_store->get_all_edge_properties(src_id, dst_id);
}

// Delete all edge properties.
bool bw_graph_db_t::delete_all_edge_properties(v_id_t src_id, v_id_t dst_id) {
  return property_store->delete_all_edge_properties(src_id, dst_id);
}

// Set edge properties.
bool bw_graph_db_t::set_edge_properties(
    v_id_t src_id, v_id_t dst_id, const std::unordered_map<std::string, std::string>& properties) {
  return property_store->set_edge_properties(src_id, dst_id, properties);
}
