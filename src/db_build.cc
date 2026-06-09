#include "bw_graph/buf/page_map.h"
#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/logger.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/db.h"
#include "bw_graph/db/property_store.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/builder.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_index.h"
#include "bw_graph/part/partition.h"
#include "bw_graph/part/reorder.h"
#include "bw_graph/storage/disk_manager.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <folly/ConcurrentBitSet.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <fstream>
#include <indicators/progress_bar.hpp>
#include <iomanip>
#include <iostream>
#include <kaminpar.h>
#include <math.h>
#include <nlohmann/json.hpp>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/spin_mutex.h>
#include <utility>
#include <vector>

// Construct bw_graph_db_t.
bw_graph_db_t::bw_graph_db_t(const std::string& graph_name, bool rebuild, bool re_part,
                             bool compressed, bool id_aware, bool coarsening, bool reordered,
                             bool manual_version_switch) {
  // Step 1. Initialize basic components
  this->graph_name_ = graph_name;
  bw_graph::ensure_storage_prefixes();
  bw_graph::logger::init(bw_graph::LOG_FILE_PATH);

  const std::filesystem::path db_file = bw_graph::csr_db_path(graph_name);
  this->disk_manager = new disk_manager_t(db_file);

  // Initialise delta-page storage (separate file, 4 KB pages)
  const std::filesystem::path delta_db_file = bw_graph::delta_db_path(graph_name);
  this->delta_disk_manager = new disk_manager_t(delta_db_file, bw_graph::BW_DELTA_PAGE_SIZE);
  this->delta_buffer_pool = buf_pool_t<delta_page_t>::buf_pool_init(
      bw_graph::BW_DELTA_BUFFER_CHUNK_COUNT, bw_graph::BW_DELTA_BUFFER_CHUNK_SIZE,
      bw_graph::BW_DELTA_PAGE_SIZE);

  this->page_map_table = new page_map_t(this->delta_buffer_pool, this->delta_disk_manager);
  this->smo_ctl =
      new smo_ctl_t(bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT, manual_version_switch);

  // Start the background delta-page flusher.
  // Both the interval and worker count must be positive; either set to 0
  // disables the flusher (no dispatcher thread, no disk I/O).
  if (bw_graph::BW_DELTA_FLUSH_INTERVAL_MS > 0 && bw_graph::BW_DELTA_FLUSH_WORKER_COUNT > 0) {
    this->delta_flusher_ = std::make_unique<delta_flusher_t>(
        this->delta_buffer_pool, this->delta_disk_manager,
        std::chrono::milliseconds(bw_graph::BW_DELTA_FLUSH_INTERVAL_MS),
        bw_graph::BW_DELTA_FLUSH_WORKER_COUNT);
    this->delta_flusher_->start();
  }

  // Step 2. Attempt to load existing index (if not rebuilding)
  bool vertex_index_loaded = false;
  bool index_pages_loaded = false;

  if (!rebuild) {
    vertex_index_loaded = try_load_vertex_index(graph_name);
    if (vertex_index_loaded) {
      index_pages_loaded = try_load_index_pages(graph_name);
    }
  }

  // Step 3. Build or rebuild the database
  if (!vertex_index_loaded) {
    // Vertex index load failed, rebuild from scratch
    std::cout << "Building index for graph '" << graph_name << "' from scratch..." << std::endl;

    // Initialize buffer pool and giant vertex database
    this->buffer_pool = buf_pool_t<csr_page_t>::buf_pool_init(bw_graph::BW_BUFFER_CHUNK_COUNT,
                                                              bw_graph::BW_BUFFER_CHUNK_SIZE,
                                                              bw_graph::BW_GRAPH_PAGE_SIZE);
    const std::string rocksdb_path = bw_graph::giant_db_path(graph_name).string();
    this->giant_db = new giant_vertex_db_t(graph_name, rocksdb_path);
    this->property_store = new property_store_t(bw_graph::property_db_path(graph_name).string());

    // Route to different build strategies
    if (compressed) {
      build_compressed_db();
    } else if (id_aware) {
      build_seq_db();
    } else if (coarsening) {
      build_reorder_tat_db(bw_graph::BW_GRAPH_COARSENING_WEIGHT, reordered);
    } else {
      build_standard_db(re_part);
    }

  } else {
    // Vertex index loaded successfully
    std::cout << "Index loaded successfully for graph '" << graph_name << "'." << std::endl;

    // Initialize buffer pool and giant vertex database
    this->buffer_pool = buf_pool_t<csr_page_t>::buf_pool_init(bw_graph::BW_BUFFER_CHUNK_COUNT,
                                                              bw_graph::BW_BUFFER_CHUNK_SIZE,
                                                              bw_graph::BW_GRAPH_PAGE_SIZE);
    const std::string rocksdb_path = bw_graph::giant_db_path(graph_name).string();
    this->giant_db = new giant_vertex_db_t(graph_name, rocksdb_path);
    this->property_store = new property_store_t(bw_graph::property_db_path(graph_name).string());

    // If index pages failed to load, rebuild them if leveled mode is enabled
    if (!index_pages_loaded && bw_graph::BW_GRAPH_LEVELED) {
      std::cout << "Index pages not found, rebuilding TAT structure..." << std::endl;
      rebuild_tat_from_disk();
    }
  }

  // Start the background CSR-page flusher and wire it into SMO.
  // Both the interval and worker count must be positive; either set to 0
  // disables the flusher (no dispatcher thread, no disk I/O).
  if (bw_graph::BW_CSR_FLUSH_INTERVAL_MS > 0 && bw_graph::BW_CSR_FLUSH_WORKER_COUNT > 0) {
    this->csr_flusher_ = std::make_unique<csr_flusher_t>(
        this->buffer_pool, this->disk_manager,
        std::chrono::milliseconds(bw_graph::BW_CSR_FLUSH_INTERVAL_MS),
        bw_graph::BW_CSR_FLUSH_WORKER_COUNT);
    this->csr_flusher_->start();
  }
  this->smo_ctl->set_csr_flusher(this->csr_flusher_.get());
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  init_txn_manager(0);
#endif

  // Step 4. Initialize vertex update sketch
  this->vertex_update_sketch_ = new vertex_update_sketch_t(get_vertex_count());
}

// Open an existing graph database.
std::unique_ptr<bw_graph_db_t> bw_graph_db_t::open(const std::string& graph_name) {
  const std::filesystem::path db_file = bw_graph::csr_db_path(graph_name);
  const std::filesystem::path vertex_index_file = bw_graph::vertex_index_path(graph_name);
  if (!std::filesystem::is_regular_file(db_file) ||
      !std::filesystem::is_regular_file(vertex_index_file)) {
    throw std::runtime_error("Cannot open graph '" + graph_name +
                             "': persisted database files are missing");
  }
  return std::make_unique<bw_graph_db_t>(graph_name, false);
}

// Destroy bw_graph_db_t.
bw_graph_db_t::~bw_graph_db_t() {
  try {
    close();
  } catch (...) {
  }
}

// Close bw_graph_db_t.
void bw_graph_db_t::close() {
  if (closed_) {
    return;
  }
  closed_ = true;

  if (smo_ctl != nullptr) {
    smo_ctl->publish_pending_versions();
    smo_ctl->set_csr_flusher(nullptr);
  }

  csr_flusher_.reset();
  delta_flusher_.reset();
  flush_csr_pages();
  flush_delta_pages();

  if (vertex_index != nullptr) {
    save_vertex_index(graph_name_);
  }
  if (bw_graph::BW_GRAPH_LEVELED && root_index_page != nullptr) {
    save_index_pages(graph_name_);
  }
  if (giant_db != nullptr) {
    giant_db->flush_wal();
  }
  if (property_store != nullptr) {
    property_store->flush_wal();
  }

  if (vertex_index != nullptr) {
    vertex_index->shutdown_version_gc();
  }

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  txn_manager_.reset();
#endif

  delete smo_ctl;
  smo_ctl = nullptr;
  delete page_map_table;
  page_map_table = nullptr;
  delete vertex_update_sketch_;
  vertex_update_sketch_ = nullptr;
  delete giant_db;
  giant_db = nullptr;
  delete property_store;
  property_store = nullptr;

  for (auto& [page_no, page] : index_page_map) {
    (void) page_no;
    delete page;
  }
  index_page_map.clear();
  root_index_page = nullptr;

  delete buffer_pool;
  buffer_pool = nullptr;
  delete delta_buffer_pool;
  delta_buffer_pool = nullptr;
  delete vertex_index;
  vertex_index = nullptr;

  if (disk_manager != nullptr) {
    disk_manager->shutdown();
    delete disk_manager;
    disk_manager = nullptr;
  }
  if (delta_disk_manager != nullptr) {
    delta_disk_manager->shutdown();
    delete delta_disk_manager;
    delta_disk_manager = nullptr;
  }
  bw_graph::logger::shutdown();
}

// Handle try load vertex index.
bool bw_graph_db_t::try_load_vertex_index(const std::string& graph_name) {
  const std::string index_file = bw_graph::vertex_index_path(graph_name).string();

  std::ifstream ifs(index_file, std::ios::binary);
  if (!ifs.good()) {
    std::cout << "Vertex index file does not exist: " << index_file << std::endl;
    return false;
  }
  ifs.close();

  this->vertex_index = new vertex_index_t();
  if (this->vertex_index->load_from_file(index_file)) {
    std::cout << "Loaded vertex index from " << index_file << std::endl;
    return true;
  } else {
    std::cout << "[LOGIC BUG] Failed to load vertex index from file: " << index_file << std::endl;
    delete this->vertex_index;
    this->vertex_index = nullptr;
    return false;
  }
}

// Handle try load index pages.
bool bw_graph_db_t::try_load_index_pages(const std::string& graph_name) {
  const std::string index_page_file = bw_graph::index_page_path(graph_name).string();

  std::ifstream ifs(index_page_file, std::ios::binary);
  if (!ifs.good()) {
    std::cout << "Index page file does not exist: " << index_page_file << std::endl;
    return false;
  }
  ifs.close();

  if (this->load_index_pages_from_file(index_page_file)) {
    std::cout << "Loaded index pages from " << index_page_file << std::endl;
    return true;
  } else {
    std::cout << "[LOGIC BUG] Failed to load index pages from file: " << index_page_file
              << std::endl;
    return false;
  }
}

// Build standard db.
void bw_graph_db_t::build_standard_db(bool re_part) {
  // Load graph from file
  std::string graph_file = "data/" + this->graph_name_ + ".graph";
  mem_sub_com_t com_graph(graph_file, true, bw_graph::LOAD_BUFFER_SIZE);
  std::cout << "Graph loaded: " << com_graph.vertex_count() << " vertices, "
            << com_graph.edge_count() << " edges" << std::endl;

  // Step 1. Load or compute graph partitioning
  graph_partition_t graph_partition =
      load_or_compute_partition(com_graph, this->graph_name_, re_part);

  // Step 2. Build CSR pages and vertex index
  std::vector<csr_page_t*> page_list(graph_partition.size());
  std::vector<v_id_t> giant_vertex_ids;
  this->vertex_index = new vertex_index_t(com_graph, graph_partition, page_list, giant_vertex_ids);

  // Step 3. Process giant vertices
  for (auto giant_vertex_id : giant_vertex_ids) {
    std::vector<v_id_t> neighbors = com_graph.get_neighbors(giant_vertex_id);
    this->giant_db->insert_vertex(giant_vertex_id, neighbors);
  }
  std::cout << giant_vertex_ids.size() << " Giant vertices processed successfully!" << std::endl;

  // Step 4. Build TAT hierarchy if enabled
  if (bw_graph::BW_GRAPH_LEVELED) {
    build_tat_hierarchy(page_list, com_graph);
  }

  // Step 5. Write CSR pages to disk
  page_no_t current_page_no = 0;
  for (auto csr_page : page_list) {
    this->disk_manager->write_page(current_page_no++, csr_page->get_data());
  }
  std::cout << "CSR pages written to disk successfully!" << std::endl;

  // Step 6. Save vertex index and index pages
  save_vertex_index(this->graph_name_);
  if (bw_graph::BW_GRAPH_LEVELED) {
    save_index_pages(this->graph_name_);
  }
}

// Load or compute partition.
graph_partition_t bw_graph_db_t::load_or_compute_partition(mem_sub_com_t& com_graph,
                                                           const std::string& graph_name,
                                                           bool re_part) {
  graph_partition_t graph_partition;

  if (!re_part) {
    const std::filesystem::path part_file = bw_graph::partition_path(graph_name);

    if (std::filesystem::exists(part_file)) {
      std::ifstream in(part_file, std::ios::binary);
      if (in && load_partition_from_stream(in, graph_partition)) {
        std::cout << "Loaded partition from " << part_file << std::endl;
        return graph_partition;
      }
    }
  }

  // Compute new partition
  std::cout << "Computing new partition..." << std::endl;
  graph_partition =
      compute_absolute_weight_graph_partition(com_graph, bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT);

  // Save partition to disk
  if (!re_part) {
    save_partition_to_file(graph_partition, graph_name);
  }

  return graph_partition;
}

// Load partition from stream.
bool bw_graph_db_t::load_partition_from_stream(std::ifstream& in, graph_partition_t& partition) {
  uint64_t parts = 0;
  in.read(reinterpret_cast<char*>(&parts), sizeof(parts));
  if (!in || parts > (1ull << 40)) {
    return false;
  }

  partition.resize(static_cast<size_t>(parts));
  for (uint64_t i = 0; i < parts; ++i) {
    uint64_t sz = 0;
    in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
    if (!in || sz > (1ull << 50)) {
      partition.clear();
      return false;
    }

    partition[i].resize(static_cast<size_t>(sz));
    in.read(reinterpret_cast<char*>(partition[i].data()), sizeof(v_id_t) * static_cast<size_t>(sz));
    if (!in) {
      partition.clear();
      return false;
    }
  }

  return true;
}

// Save partition to file.
void bw_graph_db_t::save_partition_to_file(const graph_partition_t& partition,
                                           const std::string& graph_name) {
  const std::filesystem::path part_file = bw_graph::partition_path(graph_name);

  std::ofstream out(part_file, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "Warning: failed to open partition file for writing: " << part_file << std::endl;
    return;
  }

  uint64_t parts = static_cast<uint64_t>(partition.size());
  out.write(reinterpret_cast<const char*>(&parts), sizeof(parts));

  for (const auto& bucket : partition) {
    uint64_t sz = static_cast<uint64_t>(bucket.size());
    out.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
    if (sz) {
      out.write(reinterpret_cast<const char*>(bucket.data()),
                sizeof(v_id_t) * static_cast<size_t>(sz));
    }
  }

  out.flush();
  std::cout << "Computed and saved partition to " << part_file << std::endl;
}

// Build tat hierarchy.
void bw_graph_db_t::build_tat_hierarchy(std::vector<csr_page_t*>& page_list,
                                        mem_sub_com_t& com_graph) {
  std::cout << "Building TAT hierarchy..." << std::endl;

  // Find max page number in L0
  page_no_t max_page_no_l0 = 0;
  for (auto& page : page_list) {
    max_page_no_l0 = std::max(max_page_no_l0, page->get_page_no());
  }

  // Build TAT starting from (max_page_no_l0 + 1)
  page_no_t start_page_no = max_page_no_l0 + 1;
  tat_build_result_t result =
      build_tat_from_csr_pages(page_list, start_page_no, this->vertex_index, com_graph);

  // Update database state
  this->root_index_page = result.root_page;
  this->level_count_ = result.level_count;
  this->index_page_map = std::move(result.index_pages);

  // Wire SMO controller to the index page map so it can maintain the TAT
  // after CSR page splits.
  this->smo_ctl->set_index_page_map(&this->index_page_map, &this->root_index_page,
                                    result.next_index_page_no);

  std::cout << "TAT hierarchy built successfully!" << std::endl;
  std::cout << "Total " << this->level_count_ << " levels in the index." << std::endl;
  std::cout << "Root index page number: " << result.root_page_no << std::endl;
}

// Rebuild tat from disk.
void bw_graph_db_t::rebuild_tat_from_disk() {
  // Load graph to rebuild adjacency information
  std::string graph_file = "data/" + this->graph_name_ + ".graph";
  mem_sub_com_t com_graph(graph_file, true, bw_graph::LOAD_BUFFER_SIZE);

  // Reconstruct CSR page objects from vertex index
  std::vector<csr_page_t*> page_list;
  reconstruct_page_list_from_index(page_list);

  // Build TAT hierarchy
  build_tat_hierarchy(page_list, com_graph);

  // Save the rebuilt index pages
  save_index_pages(this->graph_name_);

  // Clean up temporary page objects
  for (auto page : page_list) {
    delete page;
  }
}

// Reconstruct page list from index.
void bw_graph_db_t::reconstruct_page_list_from_index(std::vector<csr_page_t*>& page_list) {
  // Build a map of page_no -> vertices
  folly::F14FastMap<page_no_t, std::vector<v_id_t>> page_vertices_map;

  for (v_id_t vid = 0; vid < this->vertex_index->items.size(); ++vid) {
    const auto& item = this->vertex_index->items[vid];
    if (!item.deleted() && !item.is_giant()) {
      page_vertices_map[this->vertex_index->get_vertex_location(vid).first].push_back(vid);
    }
  }

  // Create CSR page objects for each page
  for (auto& [page_no, vertices] : page_vertices_map) {
    // Read page data from disk
    auto page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);
    page_list.push_back(page);
  }

  std::cout << "Reconstructed " << page_list.size() << " pages from vertex index." << std::endl;
}

// Save vertex index.
void bw_graph_db_t::save_vertex_index(const std::string& graph_name) {
  const std::string index_file = bw_graph::vertex_index_path(graph_name).string();

  // Use compression by default
  if (this->vertex_index->save_to_file(index_file, true)) {
    std::cout << "Vertex index saved to " << index_file << std::endl;
  } else {
    std::cerr << "Failed to save vertex index to " << index_file << std::endl;
  }
}

// Save index pages.
void bw_graph_db_t::save_index_pages(const std::string& graph_name) {
  const std::string index_page_file = bw_graph::index_page_path(graph_name).string();

  if (this->save_index_pages_to_file(index_page_file)) {
    std::cout << "Index page saved to " << index_page_file << std::endl;
  } else {
    std::cerr << "Failed to save index page to " << index_page_file << std::endl;
  }
}

/**
 * @brief Build the ID-aware sequential database with parallel processing
 *
 * This function builds a database where vertices are stored in ID order,
 * with optional parallelization across ID ranges. Giant vertices are
 * stored in external storage (RocksDB).
 */
void bw_graph_db_t::build_seq_db() {
  std::cout << "=== Building Sequential ID-Aware Database ===" << std::endl;
  std::cout << "Graph: " << this->graph_name_ << std::endl;
  std::cout << "Edge slots per vertex: " << bw_graph::BW_GRAPH_EDGE_SLOT_COUNT << std::endl;

  // Step 1 - Load Graph
  std::string graph_file = "data/" + this->graph_name_ + ".graph";
  mem_sub_com_t com_graph(graph_file, true, bw_graph::LOAD_BUFFER_SIZE);
  auto start = std::chrono::high_resolution_clock::now();

  std::cout << "Total vertices: " << com_graph.vertex_count() << std::endl;
  std::cout << "Total edges: " << com_graph.edge_count() << std::endl;

  // Initialize the vertex index
  this->vertex_index = new vertex_index_t(com_graph.vertex_count());

  uint64_t total_vertices = com_graph.vertex_count();

  // Step 2 - Determine Parallelization Strategy
  // Get number of hardware threads
  uint64_t num_threads = std::thread::hardware_concurrency();
  if (num_threads == 0) {
    num_threads = 4;
  }

  // Calculate vertices per thread (segment size)
  uint64_t vertices_per_segment = (total_vertices + num_threads - 1) / num_threads;

  std::cout << "Using " << num_threads << " threads for parallel building" << std::endl;
  std::cout << "Vertices per segment: " << vertices_per_segment << std::endl;

  // Step 3: Parallel Paged CSR Packing
  std::cout << "Building CSR pages in parallel..." << std::endl;

  std::vector<segment_result_t> segment_results(num_threads);

  // Build bar.
  indicators::ProgressBar build_bar{
      indicators::option::BarWidth{50},
      // Start start.
      indicators::option::Start{"["}, indicators::option::Fill{"="}, indicators::option::Lead{">"},
      indicators::option::Remainder{" "}, indicators::option::End{"]"},
      indicators::option::PostfixText{"Building Pages"},
      indicators::option::ForegroundColor{indicators::Color::green},
      indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
      indicators::option::ShowRemainingTime{true}, indicators::option::MaxProgress{total_vertices}};

  std::mutex bar_mutex;
  std::atomic<size_t> global_processed_vertices{0};
  const size_t progress_update_interval = total_vertices / 100 > 0 ? total_vertices / 10000 : 1;

  // Parallel processing - each thread handles a range of vertex IDs
  tbb::parallel_for(
      tbb::blocked_range<size_t>(0, num_threads), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t thread_idx = range.begin(); thread_idx != range.end(); ++thread_idx) {
          // Calculate vertex ID range for this segment
          v_id_t start_vertex = thread_idx * vertices_per_segment;
          v_id_t end_vertex = std::min(start_vertex + vertices_per_segment, total_vertices);

          // Thread-local page builder
          csr_page_builder_t paged_csr_builder(bw_graph::BW_GRAPH_PAGE_SIZE,
                                               bw_graph::BW_GRAPH_EDGE_SLOT_COUNT);

          // Thread-local page number (relative to this segment)
          page_no_t local_page_no = 0;

          auto& result = segment_results[thread_idx];

          // Thread-local counter for batch updates
          size_t local_processed = 0;
          const size_t batch_size = 10000;

          // Process vertices in this segment
          for (v_id_t vertex_id = start_vertex; vertex_id < end_vertex; ++vertex_id) {
            // Get neighbors for this vertex
            neighbor_span_t neighbors = com_graph.get_neighbors_span(vertex_id);
            uint64_t degree = neighbors.size();

            // Check if this is a giant vertex
            if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
              result.giant_vertices.push_back(vertex_id);
              local_processed++;

              // Batch update progress
              if (local_processed >= batch_size) {
                size_t new_global = global_processed_vertices.fetch_add(local_processed);
                if ((new_global / progress_update_interval) !=
                    ((new_global + local_processed) / progress_update_interval)) {
                  std::lock_guard<std::mutex> lock(bar_mutex);
                  build_bar.set_progress(new_global + local_processed);
                }
                local_processed = 0;
              }
              continue;
            }

            // Try to add vertex to current page
            if (!paged_csr_builder.add_vertex(vertex_id, neighbors)) {
              // Current page is full, build it
              auto [new_csr_page, v_offset_map] = paged_csr_builder.build();

              new_csr_page->set_page_no(local_page_no);
              result.pages.push_back(new_csr_page);

              for (const auto& [inner_vertex_id, offset] : v_offset_map) {
                result.normal_vertices.emplace_back(inner_vertex_id, local_page_no, offset);
              }

              local_page_no++;

              bool must_success = paged_csr_builder.add_vertex(vertex_id, neighbors);
              assert(must_success && "Failed to add vertex after page flush");
            }

            local_processed++;

            // Batch update progress
            if (local_processed >= batch_size) {
              size_t new_global = global_processed_vertices.fetch_add(local_processed);
              if ((new_global / progress_update_interval) !=
                  ((new_global + local_processed) / progress_update_interval)) {
                std::lock_guard<std::mutex> lock(bar_mutex);
                build_bar.set_progress(new_global + local_processed);
              }
              local_processed = 0;
            }
          }

          // Build the last page if not empty
          if (!paged_csr_builder.is_empty()) {
            auto [new_csr_page, v_offset_map] = paged_csr_builder.build();

            new_csr_page->set_page_no(local_page_no);
            result.pages.push_back(new_csr_page);

            for (const auto& [inner_vertex_id, offset] : v_offset_map) {
              result.normal_vertices.emplace_back(inner_vertex_id, local_page_no, offset);
            }
          }

          // Flush remaining local progress
          if (local_processed > 0) {
            size_t new_global = global_processed_vertices.fetch_add(local_processed);
            std::lock_guard<std::mutex> lock(bar_mutex);
            build_bar.set_progress(new_global + local_processed);
          }
        }
      });

  build_bar.mark_as_completed();
  std::cout << std::endl;
  std::cout << "Building Pages...[OK]" << std::endl;

  // Step 4 - Calculate Segment Starting Page Numbers
  std::cout << "Calculating global page numbers... " << std::endl;

  std::vector<page_no_t> segment_start_page_no(num_threads);
  page_no_t current_global_page_no = 0;

  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    segment_start_page_no[thread_idx] = current_global_page_no;
    current_global_page_no += segment_results[thread_idx].pages.size();
  }

  uint64_t total_pages = current_global_page_no;
  std::cout << "Total pages created: " << total_pages << std::endl;

  // Step 5 - Update Vertex Index with Global Page Numbers
  std::cout << "Updating vertex index..." << std::endl;

  uint64_t total_normal_vertices = 0;
  uint64_t total_giant_vertices = 0;

  // Update vertex index for normal vertices
  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    const auto& result = segment_results[thread_idx];
    page_no_t segment_base_page_no = segment_start_page_no[thread_idx];

    // For each normal vertex in this segment
    for (const auto& [vertex_id, local_page_no, offset] : result.normal_vertices) {
      // Calculate global page number: segment_base + local_page_no
      page_no_t global_page_no = segment_base_page_no + local_page_no;

      // This avoids copy assignment and uses the constructor
      this->vertex_index->items[vertex_id] = v_item_t(global_page_no, offset, NORMAL, 0);
    }

    total_normal_vertices += result.normal_vertices.size();
    total_giant_vertices += result.giant_vertices.size();
  }

  // Update vertex index for giant vertices
  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    const auto& result = segment_results[thread_idx];

    for (v_id_t giant_vertex_id : result.giant_vertices) {
      // Giant vertices don't have page_no or offset
      this->vertex_index->items[giant_vertex_id] = v_item_t(0, 0, GIANT, 0);
    }
  }

  std::cout << "Normal vertices: " << total_normal_vertices << std::endl;
  std::cout << "Giant vertices: " << total_giant_vertices << std::endl;

  // Step 6 - Update Page Numbers and Collect All Pages
  std::cout << "Finalizing page numbers..." << std::endl;

  std::vector<csr_page_t*> all_pages;
  all_pages.reserve(total_pages);

  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    auto& result = segment_results[thread_idx];
    page_no_t segment_base_page_no = segment_start_page_no[thread_idx];

    // Update each page's page number to global number
    for (auto* page : result.pages) {
      page_no_t local_page_no = page->get_page_no();
      page_no_t global_page_no = segment_base_page_no + local_page_no;
      page->set_page_no(global_page_no);
      all_pages.push_back(page);
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "[DB BUILDING] DB Building Time: " << elapsed_ms << " ms" << std::endl;

  // Step 7 - Insert Giant Vertices into External Storage
  std::cout << "Parallel Giant Vertex Insertion..." << std::endl;

  if (total_giant_vertices > 0) {
    indicators::ProgressBar giant_bar{
        indicators::option::BarWidth{50},
        // Start start.
        indicators::option::Start{"["}, indicators::option::Fill{"="},
        indicators::option::Lead{">"}, indicators::option::Remainder{" "},
        indicators::option::End{"]"}, indicators::option::PostfixText{"Inserting Giant Vertices"},
        indicators::option::ForegroundColor{indicators::Color::yellow},
        indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
        indicators::option::MaxProgress{total_giant_vertices}};

    uint64_t giant_count = 0;
    for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
      const auto& result = segment_results[thread_idx];

      for (v_id_t giant_vertex_id : result.giant_vertices) {
        std::vector<v_id_t> neighbors = com_graph.get_neighbors(giant_vertex_id);
        this->giant_db->insert_vertex(giant_vertex_id, neighbors);

        giant_count++;
        if (giant_count % 100 == 0 || giant_count == total_giant_vertices) {
          giant_bar.set_progress(giant_count);
        }
      }
    }

    giant_bar.mark_as_completed();
    std::cout << std::endl;
  }
  std::cout << "Inserting Giant Vertices...[OK]" << std::endl;

  // Step 8 - Write All Pages to Disk
  std::cout << "Writing pages to disk..." << std::endl;

  indicators::ProgressBar disk_bar{
      indicators::option::BarWidth{50},
      // Start start.
      indicators::option::Start{"["}, indicators::option::Fill{"="}, indicators::option::Lead{">"},
      indicators::option::Remainder{" "}, indicators::option::End{"]"},
      indicators::option::PostfixText{"Writing Pages"},
      indicators::option::ForegroundColor{indicators::Color::blue},
      indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
      indicators::option::MaxProgress{all_pages.size()}};

  std::atomic<size_t> progress_counter{0};

  tbb::parallel_for(size_t(0), all_pages.size(), [&](size_t i) {
    auto* page = all_pages[i];
    this->disk_manager->write_page(page->get_page_no(), page->get_data());

    // Update progress
    size_t current = progress_counter.fetch_add(1) + 1;
    if (current % 100 == 0 || current == all_pages.size()) {
      disk_bar.set_progress(current);
    }
  });

  disk_bar.mark_as_completed();
  std::cout << std::endl;
  std::cout << "Writing Pages...[OK]" << std::endl;

  // Step 9 - Calculate and Print Statistics
  uint64_t total_bytes = all_pages.size() * bw_graph::BW_GRAPH_PAGE_SIZE;

  std::cout << "=== Build Statistics ===" << std::endl;
  std::cout << "Total pages: " << all_pages.size() << std::endl;
  std::cout << "Storage footprint: " << format_bytes(total_bytes) << std::endl;

  // Step 10 - Save Vertex Index
  save_vertex_index(this->graph_name_);

  // Step 11: Cleanup
  // Clean up CSR page data after writing to disk
  for (auto* page : all_pages) {
    delete[] page->get_data();
  }

  // Create the vertex update sketch
  this->vertex_update_sketch_ = new vertex_update_sketch_t(get_vertex_count());

  std::cout << "=== Build Complete ===" << std::endl;
}

/**
 * @brief Build compressed database using PTV (Prefixed-Tag Vertex) encoding.
 *
 * Replaces the old hand-rolled vbyte builder that used an incompatible 48-byte
 * metadata header. This version delegates neighbor storage to
 * csr_page_t::build_from_adjacency_map(), which applies PTV encoding when
 * BW_GRAPH_NEIGHBOR_COMPRESS is true, producing pages fully compatible with
 * parse_from_page_data().
 *
 * Build strategy: sequential id-aware packing (same as build_seq_db) so that
 * vertex IDs are localised on pages, maximising cache efficiency.
 */
void bw_graph_db_t::build_compressed_db() {
  std::cout << "=== Building Compressed Database (PTV Encoding) ===" << std::endl;
  std::cout << "Graph: " << this->graph_name_ << std::endl;
  std::cout << "Compression: "
            << (bw_graph::BW_GRAPH_NEIGHBOR_COMPRESS ? "ENABLED (PTV)" : "DISABLED") << std::endl;

  // Load graph
  std::string graph_file = "data/" + this->graph_name_ + ".graph";
  mem_sub_com_t com_graph(graph_file, true, bw_graph::LOAD_BUFFER_SIZE);

  std::cout << "Total vertices: " << com_graph.vertex_count() << std::endl;
  std::cout << "Total edges: " << com_graph.edge_count() << std::endl;

  this->vertex_index = new vertex_index_t(com_graph.vertex_count());

  const size_t PAGE_SIZE = bw_graph::BW_GRAPH_PAGE_SIZE;
  // Fixed per-page overhead: 16-byte compact metadata + neighbor block
  const size_t kFixed = (sizeof(uint16_t) * 2 + sizeof(uint32_t) * 3) +
                        bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);

  uint64_t total_vertices = com_graph.vertex_count();
  uint64_t normal_vertex_count = 0;
  uint64_t total_pages = 0;
  uint64_t total_edges_stored = 0;

  // Accumulate vertices for the current page
  adj_map_t current_batch;
  size_t current_batch_bytes = kFixed;
  page_no_t current_page_no = 0;

  // Flush the accumulated batch as one CSR page via the standard constructor,
  // which calls build_from_adjacency_map() and applies PTV when compression
  // is enabled.
  auto flush_page = [&]() {
    if (current_batch.empty()) {
      return;
    }

    adj_map_iter_t view = create_neighbor_span_view(current_batch);
    std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
    csr_page_t page(current_page_no, view, vertex_map, 0);

    this->disk_manager->write_page(current_page_no, page.get_data());

    // Update vertex index with the offsets returned by the constructor
    for (const auto& [vid, offset] : vertex_map) {
      this->vertex_index->update_vertex_loc(vid, current_page_no,
                                            static_cast<uint16_t>(offset));
    }

    std::cout << "Page " << current_page_no << ": " << current_batch.size() << " vertices, "
              << page.get_num_edges() << " edges, " << page.get_used_bytes() << " / " << PAGE_SIZE
              << " bytes (" << std::fixed << std::setprecision(1)
              << (100.0 * page.get_used_bytes() / PAGE_SIZE) << "%)" << std::endl;

    total_edges_stored += page.get_num_edges();
    total_pages++;
    current_page_no++;

    current_batch.clear();
    current_batch_bytes = kFixed;
  };

  for (v_id_t vertex_id = 0; vertex_id < total_vertices; ++vertex_id) {
    std::vector<v_id_t> neighbors = com_graph.get_neighbors(vertex_id);
    uint64_t degree = neighbors.size();

    if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      this->giant_db->insert_vertex(vertex_id, neighbors);
      this->vertex_index->items[vertex_id].vertex_type = GIANT;
      continue;
    }

    std::sort(neighbors.begin(), neighbors.end());

    // Estimate bytes for this vertex (PTV upper-bound = 4 bytes per neighbor)
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
    size_t nbr_bytes = degree * 4;
#else
    size_t nbr_bytes = degree * sizeof(v_id_t);
#endif
    size_t vertex_bytes = sizeof(csr_vertex_t) + nbr_bytes;

    if (!current_batch.empty() && current_batch_bytes + vertex_bytes > PAGE_SIZE) {
      flush_page();
    }

    current_batch[vertex_id] = std::move(neighbors);
    current_batch_bytes += vertex_bytes;
    normal_vertex_count++;
  }

  flush_page();

  if (total_pages > 0) {
    std::cout << "Average vertices per page: " << std::fixed << std::setprecision(2)
              << static_cast<double>(normal_vertex_count) / total_pages << std::endl;
    std::cout << "Average edges per page: " << std::fixed << std::setprecision(2)
              << static_cast<double>(total_edges_stored) / total_pages << std::endl;
  }

  std::cout << "CSR construction complete: " << total_edges_stored << " edges stored" << std::endl;
  std::cout << "Memory optimization complete. Temporary structures released." << std::endl;

  save_vertex_index(this->graph_name_);
  this->vertex_update_sketch_ = new vertex_update_sketch_t(get_vertex_count());
}

// Constants for buffered I/O
constexpr size_t PARTITION_WRITE_BUFFER_SIZE = 64 * 1024 * 1024; // 64MB
constexpr size_t PARTITION_READ_BUFFER_SIZE = 64 * 1024 * 1024;  // 64MB

// Save partition result to disk with buffering
bool save_partition_to_disk(const graph_partition_t& partition, const std::string& filepath) {
  std::ofstream ofs(filepath, std::ios::binary);
  if (!ofs.is_open()) {
    std::cerr << "Failed to open file for writing: " << filepath << std::endl;
    return false;
  }

  // Set up write buffer
  std::vector<char> write_buffer(PARTITION_WRITE_BUFFER_SIZE);
  ofs.rdbuf()->pubsetbuf(write_buffer.data(), write_buffer.size());

  try {
    // Write number of partitions
    uint64_t num_partitions = partition.size();
    ofs.write(reinterpret_cast<const char*>(&num_partitions), sizeof(uint64_t));

    // Write each partition
    for (const auto& part : partition) {
      // Write partition size
      uint64_t part_size = part.size();
      ofs.write(reinterpret_cast<const char*>(&part_size), sizeof(uint64_t));

      // Write vertex IDs in chunks to avoid large memory copy
      const size_t chunk_size = 1024 * 1024; // 1M vertices per chunk
      for (size_t i = 0; i < part.size(); i += chunk_size) {
        size_t write_count = std::min<size_t>(chunk_size, part.size() - i);
        ofs.write(reinterpret_cast<const char*>(part.data() + i), write_count * sizeof(v_id_t));
      }
    }

    ofs.flush();
    if (!ofs.good()) {
      std::cerr << "Error occurred during partition write" << std::endl;
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    std::cerr << "Exception during partition save: " << e.what() << std::endl;
    return false;
  }
}

// Load partition result from disk with buffering
bool load_partition_from_disk(graph_partition_t& partition, const std::string& filepath) {
  std::ifstream ifs(filepath, std::ios::binary);
  if (!ifs.is_open()) {
    return false; // File doesn't exist
  }

  // Set up read buffer
  std::vector<char> read_buffer(PARTITION_READ_BUFFER_SIZE);
  ifs.rdbuf()->pubsetbuf(read_buffer.data(), read_buffer.size());

  try {
    // Read number of partitions
    uint64_t num_partitions = 0;
    ifs.read(reinterpret_cast<char*>(&num_partitions), sizeof(uint64_t));
    if (!ifs.good()) {
      std::cerr << "Failed to read partition count" << std::endl;
      return false;
    }

    partition.clear();
    partition.resize(num_partitions);

    // Read each partition
    for (uint64_t i = 0; i < num_partitions; ++i) {
      // Read partition size
      uint64_t part_size = 0;
      ifs.read(reinterpret_cast<char*>(&part_size), sizeof(uint64_t));
      if (!ifs.good()) {
        std::cerr << "Failed to read partition " << i << " size" << std::endl;
        return false;
      }

      // Allocate space
      partition[i].resize(part_size);

      // Read vertex IDs in chunks
      const size_t chunk_size = 1024 * 1024; // 1M vertices per chunk
      for (size_t j = 0; j < part_size; j += chunk_size) {
        size_t read_count = std::min<size_t>(chunk_size, part_size - j);
        ifs.read(reinterpret_cast<char*>(partition[i].data() + j), read_count * sizeof(v_id_t));
        if (!ifs.good()) {
          std::cerr << "Failed to read partition " << i << " data at offset " << j << std::endl;
          return false;
        }
      }
    }

    return true;
  } catch (const std::exception& e) {
    std::cerr << "Exception during partition load: " << e.what() << std::endl;
    return false;
  }
}

// Modified build_reorder_tat_db function
void bw_graph_db_t::build_reorder_tat_db(uint64_t coarsening_weight, bool reorder) {
  // Load graph
  std::string graph_file = "data/" + this->graph_name_ + ".graph";
  mem_sub_com_t com_graph(graph_file, true, bw_graph::LOAD_BUFFER_SIZE);

  // Initialize the vertex index
  this->vertex_index = new vertex_index_t(com_graph.vertex_count());

  // Step 1 - Load or Compute Partition
  const std::string partition_file = bw_graph::coarsening_path(this->graph_name_).string();

  graph_partition_t g_coar_part;
  bool partition_loaded = false;

  // Try to load partition if BW_GRAPH_REDO_PARTITION is false
  if (bw_graph::BW_GRAPH_USING_NATIVE_PARTITION) {
    // Try to load partition result from file;
    std::cout << "Loading partition from file..." << std::endl;
    uint32_t vertex_count = 0;
    for (const auto& partition : com_graph.get_community_structure_ref()) {
      g_coar_part.push_back(partition);
      vertex_count += partition.size();
    }
    // Key path check.
    assert(vertex_count == com_graph.vertex_count());

    std::cout << "Loading partition from file...[Ok]" << std::endl;
  } else {
    if (!bw_graph::BW_GRAPH_REDO_PARTITION) {
      std::cout << "Attempting to load partition from: " << partition_file << std::endl;

      if (load_partition_from_disk(g_coar_part, partition_file)) {
        std::cout << "Successfully loaded partition with " << g_coar_part.size() << " partitions"
                  << std::endl;

        // Verify partition integrity
        uint64_t total_vertices = 0;
        for (const auto& part : g_coar_part) {
          total_vertices += part.size();
        }

        if (total_vertices == com_graph.vertex_count()) {
          std::cout << "Partition verification passed: " << total_vertices << " vertices"
                    << std::endl;
          partition_loaded = true;
        } else {
          std::cerr << "Partition verification failed: expected " << com_graph.vertex_count()
                    << " vertices, got " << total_vertices << std::endl;
          std::cerr << "Will recompute partition..." << std::endl;
        }
      } else {
        std::cout << "Partition file not found or corrupted, will compute new "
                     "partition"
                  << std::endl;
      }
    } else {
      std::cout << "BW_GRAPH_REDO_PARTITION is true, will compute new partition" << std::endl;
    }

    // Compute partition if not loaded
    if (!partition_loaded) {
      std::cout << "Computing graph partition with coarsening weight: " << coarsening_weight
                << std::endl;

      g_coar_part = compute_absolute_weight_graph_partition_non_giant(com_graph, coarsening_weight);

      std::cout << "Partition computed: " << g_coar_part.size() << " partitions" << std::endl;

      uint64_t total_vertices_in_partition = 0;
      for (const auto& part : g_coar_part) {
        total_vertices_in_partition += part.size();
      }
      uint64_t expected_vertex_count = com_graph.vertex_count();

      assert(total_vertices_in_partition == expected_vertex_count);

      // Save partition to disk
      std::cout << "Saving partition to: " << partition_file << std::endl;
      if (save_partition_to_disk(g_coar_part, partition_file)) {
        std::cout << "Partition saved successfully" << std::endl;
      } else {
        std::cerr << "Warning: Failed to save partition to disk" << std::endl;
      }
    }
  }

  // Step 2 - Build Pages
  // Create CSR pages and vertex index for efficient access
  std::vector<csr_page_t*> page_list;
  std::vector<v_id_t> giant_vertex_ids;
  csr_page_builder_t paged_csr_builder(bw_graph::BW_GRAPH_PAGE_SIZE,
                                       bw_graph::BW_GRAPH_EDGE_SLOT_COUNT);
  page_no_t current_page_id = 0;

  std::vector<std::vector<v_id_t>> reordered_partitions;
  reordered_partitions.resize(g_coar_part.size());

  // Parallel Reordering
  if (reorder) {
    std::cout << "Segmented Graph Reordering..." << std::endl;

    indicators::ProgressBar reorder_bar{
        indicators::option::BarWidth{50},
        // Start start.
        indicators::option::Start{"["}, indicators::option::Fill{"="},
        indicators::option::Lead{">"}, indicators::option::Remainder{" "},
        indicators::option::End{"]"}, indicators::option::PostfixText{"Segmented Graph Reordering"},
        indicators::option::ForegroundColor{indicators::Color::cyan},
        indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
        indicators::option::ShowRemainingTime{true},
        indicators::option::MaxProgress{g_coar_part.size()}};

    std::mutex bar_mutex;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, g_coar_part.size()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i != range.end(); ++i) {
                          // Build vertex set
                          folly::F14FastSet<v_id_t> vertex_set;
                          vertex_set.reserve(g_coar_part[i].size());
                          vertex_set.insert(g_coar_part[i].begin(), g_coar_part[i].end());

                          // Induce subgraph
                          adj_map_t induced_graph = com_graph.induce_subgraph_filter(vertex_set);

                          // Perform Gorder reordering
                          reordered_partitions[i] = gorder_reordering_optimized(induced_graph);

                          {
                            std::lock_guard<std::mutex> lock(bar_mutex);
                            reorder_bar.tick();
                          }
                        }
                      });

    reorder_bar.mark_as_completed();
    std::cout << std::endl;
    std::cout << "Segmented Graph Reordering...[OK]" << std::endl;

  } else {
    std::cout << "No reordering, using original order..." << std::endl;

    // Copy original partitions without reordering
    for (size_t i = 0; i < g_coar_part.size(); ++i) {
      reordered_partitions[i] = g_coar_part[i];
    }
  }

  // Phase 2 - Sequential Vertex Packing
  std::cout << "Paged CSR Building..." << std::endl;

  // Build bar.
  indicators::ProgressBar build_bar{
      indicators::option::BarWidth{50},
      // Start start.
      indicators::option::Start{"["}, indicators::option::Fill{"="}, indicators::option::Lead{">"},
      indicators::option::Remainder{" "}, indicators::option::End{"]"},
      indicators::option::PostfixText{"Paged CSR Building"},
      indicators::option::ForegroundColor{indicators::Color::green},
      indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
      indicators::option::ShowRemainingTime{true},
      indicators::option::MaxProgress{reordered_partitions.size()}};

  // Track partition boundaries for TAT construction
  std::vector<std::vector<csr_page_t*>> partition_pages;
  partition_pages.resize(reordered_partitions.size());
  std::vector<csr_page_t*> current_partition_pages;

  folly::F14FastSet<v_id_t> giant_vertices;

  for (size_t part_idx = 0; part_idx < reordered_partitions.size(); ++part_idx) {
    const auto& g_part_red = reordered_partitions[part_idx];

    // Build the pages one by one
    for (auto vertex_id : g_part_red) {
      // Retrieve the neighbor
      neighbor_span_t neighbors = com_graph.get_neighbors_span(vertex_id);
      uint64_t degree = neighbors.size();

      // If giant, push it to rocksdb
      if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
        giant_vertices.insert(vertex_id);
      }

      // Try to insert to a page
      if (!paged_csr_builder.add_vertex(vertex_id, neighbors)) {
        // A page is full
        auto [new_csr_page, v_offset_map] = paged_csr_builder.build();
        // Collect it to current page list
        new_csr_page->set_page_no(current_page_id);
        page_list.push_back(new_csr_page);
        current_partition_pages.push_back(new_csr_page);

        // Before updating current page id, maintain the vertex index info
        for (auto [inner_vertex_id, offset] : v_offset_map) {
          this->vertex_index->update_vertex_loc(inner_vertex_id, current_page_id, offset);
          this->vertex_index->items[inner_vertex_id].vertex_type = NORMAL;
        }
        current_page_id++;

        // Insert the failed vertex again
        bool must_success = paged_csr_builder.add_vertex(vertex_id, neighbors);
        // Must success, else logic error!
        assert(must_success);
      }
    }

    build_bar.tick();

    // Force build the last page if not empty
    if (!paged_csr_builder.is_empty()) {
      auto [new_csr_page, v_offset_map] = paged_csr_builder.build();
      // Collect it to current page list
      new_csr_page->set_page_no(current_page_id);
      page_list.push_back(new_csr_page);
      current_partition_pages.push_back(new_csr_page);

      // Before updating current page id, maintain the vertex index info
      for (auto [inner_vertex_id, offset] : v_offset_map) {
        this->vertex_index->update_vertex_loc(inner_vertex_id, current_page_id, offset);
        this->vertex_index->items[inner_vertex_id].vertex_type = NORMAL;
      }

      current_page_id++;
    }

    // Save this partition's pages
    partition_pages[part_idx] = current_partition_pages;
    current_partition_pages.clear();
  }

  build_bar.mark_as_completed();
  std::cout << std::endl;
  std::cout << "Paged CSR Building...[OK]" << std::endl;

  // Print Storage Footprint Info.
  std::cout << "Total Pages: " << page_list.size() << std::endl;
  uint64_t total_bytes = page_list.size() * bw_graph::BW_GRAPH_PAGE_SIZE;
  std::cout << "Storage Footprint: " << format_bytes(total_bytes) << std::endl;

  // After processing all parts, perform flush for all paged csr;
  for (auto csr_page : page_list) {
    this->disk_manager->write_page(csr_page->get_page_no(), csr_page->get_data());
  }

  // Process the giant vertices;
  for (auto giant_vertex_id : giant_vertices) {
    neighbor_span_t neighbors = com_graph.get_neighbors_span(giant_vertex_id);
    // Store in RocksDB
    std::vector<v_id_t> neighbors_copyed(neighbors.begin(), neighbors.end());
    this->giant_db->insert_vertex(giant_vertex_id, neighbors_copyed);
    this->vertex_index->items[giant_vertex_id].vertex_type = GIANT;
  }

  // Step 6. Save vertex index and index pages
  save_vertex_index(this->graph_name_);
  if (bw_graph::BW_GRAPH_LEVELED) {
    save_index_pages(this->graph_name_);
  }

  // Phase 3 - Build Hierarchical TAT
  if (bw_graph::BW_GRAPH_LEVELED) {
    std::cout << "Building Hierarchical TAT..." << std::endl;

    // Initialize index page allocation from high to low
    page_no_t next_index_page_no = std::numeric_limits<page_no_t>::max();
    std::vector<index_page_t*> small_tat_roots;

    indicators::ProgressBar tat_bar{
        indicators::option::BarWidth{50},
        // Start start.
        indicators::option::Start{"["}, indicators::option::Fill{"="},
        indicators::option::Lead{">"}, indicators::option::Remainder{" "},
        indicators::option::End{"]"}, indicators::option::PostfixText{"Building Small TATs"},
        indicators::option::ForegroundColor{indicators::Color::magenta},
        indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
        indicators::option::ShowRemainingTime{true},
        indicators::option::MaxProgress{partition_pages.size()}};

    // Build small TAT for each partition
    for (size_t i = 0; i < partition_pages.size(); ++i) {
      auto& partition_page_list = partition_pages[i];

      if (partition_page_list.empty()) {
        tat_bar.tick();
        continue;
      }

      // Build TAT for this partition
      auto small_tat = build_tat_from_csr_pages(partition_page_list, next_index_page_no,
                                                this->vertex_index, com_graph);

      // Store the root for later merging
      small_tat_roots.push_back(small_tat.root_page);

      // Merge index pages into global map
      for (auto& [page_no, index_page] : small_tat.index_pages) {
        this->index_page_map[page_no] = index_page;
      }

      // Update next available page number
      next_index_page_no = small_tat.next_index_page_no;

      tat_bar.tick();
    }

    tat_bar.mark_as_completed();
    std::cout << std::endl;
    std::cout << "Building Small TATs...[OK]" << std::endl;
    std::cout << "Built " << small_tat_roots.size() << " small TATs" << std::endl;

    // Build the global TAT from small TAT roots
    std::cout << "Building Global TAT..." << std::endl;
    auto global_tat = build_tat_from_index_pages(small_tat_roots, next_index_page_no);

    this->root_index_page = global_tat.root_page;
    this->level_count_ = global_tat.level_count;

    // Merge global index pages
    for (auto& [page_no, index_page] : global_tat.index_pages) {
      this->index_page_map[page_no] = index_page;
    }

    std::cout << "Building Global TAT...[OK]" << std::endl;
    std::cout << "Total TAT levels: " << this->level_count_ << std::endl;
    std::cout << "Root index page number: " << this->root_index_page->get_page_no() << std::endl;
    std::cout << "Total index pages: " << this->index_page_map.size() << std::endl;
  }

  // Cleanup CSR page data after writing to disk
  for (auto csr_page : page_list) {
    delete[] csr_page->get_data();
  }

  // Create the vertex update sketch.
  this->vertex_update_sketch_ = new vertex_update_sketch_t(get_vertex_count());

  std::cout << "=== Build Complete ===" << std::endl;
}

// Build from base pages.
tat_build_result_t bw_graph_db_t::build_tat_from_csr_pages(std::vector<csr_page_t*>& pages,
                                                           page_no_t start_page_no,
                                                           vertex_index_t* vertex_index,
                                                           mem_sub_com_t& com_graph) {
  tat_build_result_t result;
  result.index_pages.clear();

  // Step 1: Build L0 block adjacency map
  folly::F14FastMap<page_no_t, folly::F14FastSet<page_no_t>> block_adj_map_set;
  block_adj_map_set.reserve(pages.size());

  // Reserve space for each page
  for (auto& page : pages) {
    block_adj_map_set.insert({page->get_page_no(), {}});
  }

  // Traverse the base pages to compute block adjacency relationships
  for (auto page : pages) {
    vertex_span_t v_list = page->get_vertices();
    for (auto& v : v_list) {
      // Grab the location of v and v's neighbors
      auto neighbor_list = com_graph.get_neighbors(v.vertex_id);
      // For each neighbor, calculate the neighbor block ID
      for (auto& neighbor : neighbor_list) {
        auto neighbor_loc = vertex_index->get_vertex_location(neighbor);
        if (neighbor_loc.first != page->get_page_no()) {
          // A neighbor in a different block
          block_adj_map_set[page->get_page_no()].insert(neighbor_loc.first);
          block_adj_map_set[neighbor_loc.first].insert(page->get_page_no());
        }
      }
    }
  }

  // Eliminate duplicate edges and generate the block adj list
  block_adj_map_t block_adj_map;
  for (auto& [page_no, neighbor_set] : block_adj_map_set) {
    block_adj_map[page_no] = std::vector<page_no_t>(neighbor_set.begin(), neighbor_set.end());
  }

  // Compute the reduced sparse graph
  auto sparse_block_adj_map =
      graph_sparsification(block_adj_map, bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);

  // Step 2: Build the L1 - Ln-1 index pages
  // Index pages allocate from high to low (starting from start_page_no,
  // decrementing)
  page_no_t current_index_page_no = start_page_no;
  uint64_t current_level = 1;
  std::vector<page_no_t> level_page_no_list;

  while (block_adj_map.size() > bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS) {
    page_no_t level_start_page_no = current_index_page_no;

    // Subprocess 1: Perform graph partition for block adj map
    auto [block_partition, block_part_map] = absolute_weight_block_partition(
        block_adj_map,
        static_cast<kaminpar::shm::NodeWeight>(bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS * 6 / 10));

    // Subprocess 2: Update the parent_page_no for previous level
    if (current_level == 1) {
      // Previous level is L0, update the parent_page_no for base pages
      for (auto& page : pages) {
        // Parent page no = level_start_page_no - block_part_map[page_no]
        page->set_parent_page_no(level_start_page_no - block_part_map.at(page->get_page_no()));
        // Retrieve the sampled neighbor
        auto sampled_neighbor = sparse_block_adj_map.at(page->get_page_no());
        uint64_t bound = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR < sampled_neighbor.size()
                             ? bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR
                             : sampled_neighbor.size();
        for (size_t i = 0; i < bound; i++) {
          page->add_neighbor_page_no(sampled_neighbor[i]);
        }
      }
    } else {
      // Previous level is L1 - Ln-2, update the parent_page_no for index pages
      for (auto& page_no : level_page_no_list) {
        auto index_page_ptr = result.index_pages.at(page_no);
        // Parent page no = level_start_page_no - block_part_map[page_no]
        index_page_ptr->set_parent_page_no(level_start_page_no - block_part_map.at(page_no));
        // Retrieve the sampled neighbor
        auto sampled_neighbor = sparse_block_adj_map.at(page_no);
        uint64_t bound = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR < sampled_neighbor.size()
                             ? bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR
                             : sampled_neighbor.size();
        for (size_t i = 0; i < bound; i++) {
          index_page_ptr->add_neighbor_page_no(sampled_neighbor[i]);
        }
      }
      // Clear this list for next iteration
      level_page_no_list.clear();
    }

    // Subprocess 3: Build Index pages for each block partition
    folly::F14FastMap<page_no_t, folly::F14FastSet<page_no_t>> next_level_block_adj_map(
        block_partition.size());

    for (auto& sub_partition : block_partition) {
      // Process each graph in the block partition
      auto induced_subgraph = induced_block_subgraph(block_adj_map, sub_partition);

      // Build index page for each subgraph
      index_page_t* index_page = new index_page_t(current_index_page_no, induced_subgraph);

      result.index_pages[current_index_page_no] = index_page;

      // Record the page no in this level
      level_page_no_list.push_back(current_index_page_no);

      // Subprocess 4: Process the block adj map for next level
      for (auto& block_id : sub_partition) {
        auto neighbor_list = get_neighbor_pages(block_adj_map, block_id);
        for (auto& neighbor : neighbor_list) {
          auto neighbor_block_id = block_part_map.at(neighbor);
          // Calculate the global block ID (decrementing allocation)
          page_no_t global_block_id = level_start_page_no - neighbor_block_id;
          if (global_block_id != current_index_page_no) {
            next_level_block_adj_map[current_index_page_no].insert(global_block_id);
            next_level_block_adj_map[global_block_id].insert(current_index_page_no);
          }
        }
      }
      // Decrement page number for next index page
      current_index_page_no--;
    }

    // Subprocess 5: Update the block adj map for next iteration
    block_adj_map.clear();
    for (auto& [page_no, neighbor_set] : next_level_block_adj_map) {
      block_adj_map[page_no] = std::vector<page_no_t>(neighbor_set.begin(), neighbor_set.end());
    }

    // Subprocess 6: Sparse the block adj map for recording the neighbors
    sparse_block_adj_map.clear();
    sparse_block_adj_map =
        graph_sparsification(block_adj_map, bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);
    current_level++;
  }

  // Step 3: Build the Ln index page (root)
  page_no_t root_single_page_no = current_index_page_no;
  result.root_page = new index_page_t(root_single_page_no, block_adj_map);
  result.root_page_no = root_single_page_no;
  result.level_count = current_level + 1;
  result.next_index_page_no = current_index_page_no - 1;
  result.index_pages[root_single_page_no] = result.root_page;

  // Record the parent_page_no for previous level
  if (current_level == 1) {
    // Previous level is L0, update the parent_page_no for base pages
    for (auto& page : pages) {
      page->set_parent_page_no(root_single_page_no);
      // Retrieve the sampled neighbor
      auto sampled_neighbor = sparse_block_adj_map.at(page->get_page_no());
      uint64_t bound = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR < sampled_neighbor.size()
                           ? bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR
                           : sampled_neighbor.size();
      for (size_t i = 0; i < bound; i++) {
        page->add_neighbor_page_no(sampled_neighbor[i]);
      }
    }
  } else {
    for (auto& page_no : level_page_no_list) {
      auto index_page_ptr = result.index_pages.at(page_no);
      index_page_ptr->set_parent_page_no(root_single_page_no);
      // Retrieve the sampled neighbor
      auto it = sparse_block_adj_map.find(page_no);
      if (it != sparse_block_adj_map.end()) {
        const auto& sampled_neighbor = it->second;
        uint64_t bound = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR < sampled_neighbor.size()
                             ? bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR
                             : sampled_neighbor.size();
        for (size_t i = 0; i < bound; i++) {
          index_page_ptr->add_neighbor_page_no(sampled_neighbor[i]);
        }
      }
    }
  }

  return result;
}

// Build from index pages (merge small TATs into a larger TAT).
tat_build_result_t bw_graph_db_t::build_tat_from_index_pages(std::vector<index_page_t*>& root_pages,
                                                             page_no_t start_page_no) {
  tat_build_result_t result;
  result.index_pages.clear();

  // Step 1: Build L0 block adjacency map from index pages
  // Each root_page represents a block in the new TAT
  folly::F14FastMap<page_no_t, folly::F14FastSet<page_no_t>> block_adj_map_set;
  block_adj_map_set.reserve(root_pages.size());

  // Reserve space for each index page
  for (auto& page : root_pages) {
    block_adj_map_set.insert({page->get_page_no(), {}});
  }

  // Build adjacency relationships between index pages
  // We need to extract the block adjacency from each index page
  for (auto root_page : root_pages) {
    page_no_t page_no = root_page->get_page_no();

    // Get the neighbor page list from this index page
    // Assuming index_page_t has a method to get its neighbor pages
    auto neighbor_page_list = root_page->read_neighbor_block_clone();

    for (auto& neighbor_page_no : neighbor_page_list) {
      // Check if this neighbor is also in our root_pages list
      // (we only care about edges between blocks in this level)
      if (block_adj_map_set.find(neighbor_page_no) != block_adj_map_set.end()) {
        block_adj_map_set[page_no].insert(neighbor_page_no);
        block_adj_map_set[neighbor_page_no].insert(page_no);
      }
    }
  }

  // Eliminate duplicate edges and generate the block adj list
  block_adj_map_t block_adj_map;
  for (auto& [page_no, neighbor_set] : block_adj_map_set) {
    block_adj_map[page_no] = std::vector<page_no_t>(neighbor_set.begin(), neighbor_set.end());
  }

  // Compute the reduced sparse graph
  auto sparse_block_adj_map =
      graph_sparsification(block_adj_map, bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);

  // Step 2: Build the L1 - Ln-1 index pages
  // Index pages allocate from high to low (starting from start_page_no,
  // decrementing)
  page_no_t current_index_page_no = start_page_no;
  uint64_t current_level = 1;
  std::vector<page_no_t> level_page_no_list;

  // Track current level pages (initially the root_pages)
  std::vector<index_page_t*> current_level_pages;
  for (auto& page : root_pages) {
    current_level_pages.push_back(page);
  }

  while (block_adj_map.size() > bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS) {
    page_no_t level_start_page_no = current_index_page_no;

    // Subprocess 1: Perform graph partition for block adj map
    auto [block_partition, block_part_map] = absolute_weight_block_partition(
        block_adj_map,
        static_cast<kaminpar::shm::NodeWeight>(bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS * 6 / 10));

    // Subprocess 2: Update the parent_page_no for previous level
    for (auto& page : current_level_pages) {
      // Parent page no = level_start_page_no - block_part_map[page_no]
      page->set_parent_page_no(level_start_page_no - block_part_map.at(page->get_page_no()));

      // Retrieve the sampled neighbor
      auto it = sparse_block_adj_map.find(page->get_page_no());
      if (it != sparse_block_adj_map.end()) {
        const auto& sampled_neighbor = it->second;
        uint64_t bound = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR < sampled_neighbor.size()
                             ? bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR
                             : sampled_neighbor.size();
        for (size_t i = 0; i < bound; i++) {
          page->add_neighbor_page_no(sampled_neighbor[i]);
        }
      }
    }

    // Clear for next iteration
    current_level_pages.clear();
    level_page_no_list.clear();

    // Subprocess 3: Build Index pages for each block partition
    folly::F14FastMap<page_no_t, folly::F14FastSet<page_no_t>> next_level_block_adj_map(
        block_partition.size());

    for (auto& sub_partition : block_partition) {
      // Process each graph in the block partition
      auto induced_subgraph = induced_block_subgraph(block_adj_map, sub_partition);

      // Build index page for each subgraph
      index_page_t* index_page = new index_page_t(current_index_page_no, induced_subgraph);

      result.index_pages[current_index_page_no] = index_page;

      // Track this page for next level
      current_level_pages.push_back(index_page);
      level_page_no_list.push_back(current_index_page_no);

      // Subprocess 4: Process the block adj map for next level
      for (auto& block_id : sub_partition) {
        auto neighbor_list = get_neighbor_pages(block_adj_map, block_id);
        for (auto& neighbor : neighbor_list) {
          auto neighbor_block_id = block_part_map.at(neighbor);
          // Calculate the global block ID (decrementing allocation)
          page_no_t global_block_id = level_start_page_no - neighbor_block_id;
          if (global_block_id != current_index_page_no) {
            next_level_block_adj_map[current_index_page_no].insert(global_block_id);
            next_level_block_adj_map[global_block_id].insert(current_index_page_no);
          }
        }
      }
      // Decrement page number for next index page
      current_index_page_no--;
    }

    // Subprocess 5: Update the block adj map for next iteration
    block_adj_map.clear();
    for (auto& [page_no, neighbor_set] : next_level_block_adj_map) {
      block_adj_map[page_no] = std::vector<page_no_t>(neighbor_set.begin(), neighbor_set.end());
    }

    // Subprocess 6: Sparse the block adj map for recording the neighbors
    sparse_block_adj_map.clear();
    sparse_block_adj_map =
        graph_sparsification(block_adj_map, bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);
    current_level++;
  }

  // Step 3: Build the Ln index page (root)
  page_no_t root_single_page_no = current_index_page_no;
  result.root_page = new index_page_t(root_single_page_no, block_adj_map);
  result.root_page_no = root_single_page_no;
  result.level_count = current_level + 1;
  result.index_pages[root_single_page_no] = result.root_page;

  // Record the parent_page_no for previous level
  for (auto& page : current_level_pages) {
    page->set_parent_page_no(root_single_page_no);

    // Retrieve the sampled neighbor
    auto it = sparse_block_adj_map.find(page->get_page_no());
    if (it != sparse_block_adj_map.end()) {
      const auto& sampled_neighbor = it->second;
      uint64_t bound = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR < sampled_neighbor.size()
                           ? bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR
                           : sampled_neighbor.size();
      for (size_t i = 0; i < bound; i++) {
        page->add_neighbor_page_no(sampled_neighbor[i]);
      }
    }
  }

  // Record the next available page no
  result.next_index_page_no = root_single_page_no - 1;

  std::cout << "TAT built successfully from index pages!" << std::endl;
  std::cout << "Total " << result.level_count << " levels in the merged TAT." << std::endl;
  std::cout << "Root index page number: " << result.root_page_no << std::endl;
  std::cout << "Index page range: [" << result.next_index_page_no + 1 << ", " << start_page_no
            << "]" << std::endl;

  return result;
}
