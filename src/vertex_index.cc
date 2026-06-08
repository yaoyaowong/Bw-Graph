#include "bw_graph/index/vertex_index.h"

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <math.h>
#include <stdexcept>
#include <utility>
#include <vector>

void vertex_version_deleter_t::operator()(vertex_version_t* version) const {
  if (reclaim) {
    reclaim(version);
  } else {
    delete version;
  }
}

protected_vertex_version_t::protected_vertex_version_t(
    const std::atomic<vertex_version_t*>& source)
    : holder_(folly::make_hazard_pointer()), version_(holder_.protect(source)) {
  if (version_ == nullptr) {
    throw std::runtime_error("vertex index contains a null current version");
  }
}

void protected_vertex_version_t::reset() {
  if (version_ != nullptr) {
    holder_.reset_protection();
    version_ = nullptr;
  }
}

vertex_index_t::vertex_index_t() { start_version_gc(); }

// Construct vertex_index_t.
vertex_index_t::vertex_index_t(uint64_t vertex_count) {
  start_version_gc();
  std::vector<v_item_t> items(vertex_count);
  this->items = std::move(items);
  this->delta_heads_.resize(vertex_count);
  this->global_latch = rw_latch_t(); // Initialize global latch
}

// Construct vertex_index_t.
vertex_index_t::vertex_index_t(mem_sub_com_t raw_graph, const graph_partition_t& partition,
                               std::vector<csr_page_t*>& mut_raw_pages,
                               std::vector<v_id_t>& giant_vertex_ids) {
  start_version_gc();
  // For each partition;
  page_no_t current_page_no = 0;
  std::vector<v_item_t> items;
  items.reserve(raw_graph.vertex_count() + raw_graph.vertex_count() / 5);
  items.resize(raw_graph.vertex_count());

  for (auto part : partition) {
    // Step 1. Compute the induced graph;
    adj_map_iter_t induced_graph = raw_graph.induce_subgraph_iter(part);

    std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
    // Step 2. Encode this induced graph into a csr_page_t;
    csr_page_t* csr_page_ptr = new csr_page_t(current_page_no, induced_graph, vertex_map);
    mut_raw_pages[current_page_no] = csr_page_ptr;

    // Step 3. Maintain the vertex index.
    for (auto v_id : part) {
      items[v_id].vertex_type = NORMAL;      // Set vertex type
      items[v_id].latch = vertex_latch_t();  // Initialize latch
      delete items[v_id].cur.exchange(new vertex_version_t(current_page_no, 0),
                                      std::memory_order_acq_rel);
      uint64_t degree = induced_graph[v_id].size();
      if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
        items[v_id].vertex_type = GIANT;
        giant_vertex_ids.push_back(v_id);
      }
    }

    for (auto vertex_offset_item : vertex_map) {
      v_id_t vertex_id = vertex_offset_item.first;
      uint64_t offset = vertex_offset_item.second;
      vertex_version_t* current = items[vertex_id].cur.load(std::memory_order_relaxed);
      delete items[vertex_id].cur.exchange(
          new vertex_version_t(current->page_no, static_cast<uint16_t>(offset)),
          std::memory_order_acq_rel);
    }

    current_page_no++;
  }
  this->items = std::move(items);
  this->delta_heads_.resize(this->items.size());
  this->global_latch = rw_latch_t(); // Initialize global latch
}

vertex_index_t::~vertex_index_t() { shutdown_version_gc(); }

void vertex_index_t::start_version_gc() {
  if (bw_graph::version_gc_thd_count == 0) {
    throw std::invalid_argument("version_gc_thd_count must be at least 1");
  }
  version_gc_stop_.store(false, std::memory_order_release);
  version_gc_threads_.reserve(bw_graph::version_gc_thd_count);
  for (size_t i = 0; i < bw_graph::version_gc_thd_count; ++i) {
    version_gc_threads_.emplace_back([this] { version_gc_loop(); });
  }
}

void vertex_index_t::submit_version_to_hazptr(retired_version_t retired) {
  auto batch = std::move(retired.batch);
  retired.version->retire(vertex_version_deleter_t{
      [this, batch = std::move(batch)](vertex_version_t* version) mutable {
        vertex_index_t* owner = this;
        auto retained_batch = batch;
        delete version;
        owner->reclaimed_version_count_.fetch_add(1, std::memory_order_acq_rel);

        if (retained_batch != nullptr) {
          size_t before = retained_batch->remaining.fetch_sub(1, std::memory_order_acq_rel);
          if (before == 1 && retained_batch->on_complete) {
            retained_batch->on_complete();
          }
        }

        owner->pending_version_count_.fetch_sub(1, std::memory_order_acq_rel);
        owner->version_gc_idle_cv_.notify_all();
      }});
  folly::hazptr_cleanup();
}

void vertex_index_t::version_gc_loop() {
  while (true) {
    retired_version_t retired{nullptr, nullptr};
    {
      std::unique_lock<std::mutex> lock(version_gc_mutex_);
      version_gc_cv_.wait_for(lock, std::chrono::milliseconds(2), [this] {
        return version_gc_stop_.load(std::memory_order_acquire) || !retired_versions_.empty();
      });
      if (!retired_versions_.empty()) {
        retired = std::move(retired_versions_.front());
        retired_versions_.pop_front();
      } else if (version_gc_stop_.load(std::memory_order_acquire) &&
                 pending_version_count_.load(std::memory_order_acquire) == 0) {
        break;
      }
    }

    if (retired.version != nullptr) {
      submit_version_to_hazptr(std::move(retired));
    } else {
      folly::hazptr_cleanup();
    }
  }
}

void vertex_index_t::retire_version(
    vertex_version_t* version,
    std::shared_ptr<vertex_version_reclaim_batch_t> reclaim_batch) {
  if (version == nullptr) {
    return;
  }

  pending_version_count_.fetch_add(1, std::memory_order_acq_rel);
  retired_version_t retired{version, std::move(reclaim_batch)};
  if (version_gc_threads_.empty()) {
    submit_version_to_hazptr(std::move(retired));
    return;
  }

  {
    std::lock_guard<std::mutex> lock(version_gc_mutex_);
    retired_versions_.push_back(std::move(retired));
  }
  version_gc_cv_.notify_one();
}

void vertex_index_t::wait_for_version_gc() {
  while (pending_version_count_.load(std::memory_order_acquire) != 0) {
    folly::hazptr_cleanup();
    std::unique_lock<std::mutex> lock(version_gc_mutex_);
    version_gc_idle_cv_.wait_for(lock, std::chrono::milliseconds(2), [this] {
      return pending_version_count_.load(std::memory_order_acquire) == 0;
    });
  }
}

void vertex_index_t::shutdown_version_gc() {
  wait_for_version_gc();
  version_gc_stop_.store(true, std::memory_order_release);
  version_gc_cv_.notify_all();
  for (auto& worker : version_gc_threads_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  version_gc_threads_.clear();
}

// Allocate a new vertex ID
v_id_t vertex_index_t::allocate_new_vertex() {
  // Step 1 - Lock the global latch for writing
  this->global_latch.w_lock();

  // Step 2 - Allocate new vertex ID;
  v_id_t new_vertex_id = static_cast<v_id_t>(this->items.size());
  // Step 3 - Temporarily set a location for this new vertex;
  this->items.emplace_back(0, 0, NORMAL, false);
  this->delta_heads_.emplace_back(); // invalid delta head by default
  this->global_latch.w_unlock();
  return new_vertex_id;
}

// Get vertex location.
vertex_loc_t vertex_index_t::get_vertex_location(v_id_t vertex_id) const {
  return protect_vertex_version(vertex_id).location();
}

protected_vertex_version_t vertex_index_t::protect_vertex_version(v_id_t vertex_id) const {
  return protected_vertex_version_t(items[vertex_id].cur);
}

// Update vertex location;
void vertex_index_t::update_vertex_loc(v_id_t vertex_id, page_no_t page_no, uint64_t offset) {
  auto& item = this->items[vertex_id];
  item.latch.w_lock();
  prepared_vertex_version_t prepared =
      prepare_vertex_version(vertex_id, page_no, static_cast<uint16_t>(offset));
  if (!publish_prepared_version(vertex_id, prepared)) {
    discard_prepared_version(prepared);
    item.latch.w_unlock();
    throw std::runtime_error("vertex location CAS failed");
  }
  item.latch.w_unlock();
}

prepared_vertex_version_t vertex_index_t::prepare_vertex_version(v_id_t vertex_id,
                                                                 page_no_t page_no,
                                                                 uint16_t offset) const {
  vertex_version_t* expected = items[vertex_id].cur.load(std::memory_order_acquire);
  return {page_no, offset, expected, new vertex_version_t(page_no, offset, expected)};
}

bool vertex_index_t::publish_prepared_version(
    v_id_t vertex_id, prepared_vertex_version_t& prepared,
    const std::shared_ptr<vertex_version_reclaim_batch_t>& reclaim_batch) {
  if (prepared.expected == nullptr || prepared.desired == nullptr) {
    return false;
  }

  vertex_version_t* expected = prepared.expected;
  if (!items[vertex_id].cur.compare_exchange_strong(expected, prepared.desired,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
    return false;
  }

  prepared.desired->previous = nullptr;
  vertex_version_t* replaced = prepared.expected;
  prepared.expected = nullptr;
  prepared.desired = nullptr;
  retire_version(replaced, reclaim_batch);
  return true;
}

void vertex_index_t::discard_prepared_version(prepared_vertex_version_t& prepared) {
  delete prepared.desired;
  prepared.expected = nullptr;
  prepared.desired = nullptr;
}

// Check if a vertex is a giant vertex;
bool vertex_index_t::is_giant_vertex(v_id_t vertex_id) {
  if (vertex_id >= this->items.size() || this->items[vertex_id].is_delete) {
    return false;
  }

  this->items[vertex_id].latch.r_lock();

  if (this->items[vertex_id].is_delete) {
    this->items[vertex_id].latch.r_unlock();
    return false;
  }

  bool res = this->items[vertex_id].vertex_type == GIANT;
  this->items[vertex_id].latch.r_unlock();
  return res;
}

// Update vertex type;
void vertex_index_t::update_vertex_type(v_id_t vertex_id, vertex_type_t vertex_type) {
  auto& item = this->items[vertex_id];
  item.vertex_type = vertex_type;
}

// Handle v w lock.
void vertex_index_t::v_w_lock(v_id_t vertex_id) { this->items[vertex_id].latch.w_lock(); }

// Handle v w unlock.
void vertex_index_t::v_w_unlock(v_id_t vertex_id) { this->items[vertex_id].latch.w_unlock(); }

// Handle v r lock.
void vertex_index_t::v_r_lock(v_id_t vertex_id) { this->items[vertex_id].latch.r_lock(); }

// Handle v r unlock.
void vertex_index_t::v_r_unlock(v_id_t vertex_id) { this->items[vertex_id].latch.r_unlock(); }

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

/**
 * @brief Save vertex index to file with optimizations
 *
 * Serialization format:
 * - File header (32 bytes)
 * - Deletion bitmap (ceil(vertex_count / 8) bytes)
 * - Core data: [page_no(4) + vertex_type(4) + offset(2)] * vertex_count
 *
 * NOT serialized: latch, vertex_cache, padding
 *
 * @param file_path Output file path
 * @param use_compression Enable zlib compression if available (default: true)
 * @param buffer_size Write buffer size (default: 8MB)
 * @return true if successful, false otherwise
 */
bool vertex_index_t::save_to_file(const std::string& file_path, bool use_compression,
                                  size_t buffer_size) const {
  std::ofstream ofs(file_path, std::ios::binary);
  if (!ofs.is_open()) {
    return false;
  }

  const uint64_t vertex_count = this->items.size();

  // Step 1. Write file header
  struct FileHeader {
    uint64_t magic = 0x5645525445585F49; // "VERTEX_I" in hex
    uint64_t version = 2;                // v2: includes delta_heads_ (8 bytes/vertex extra)
    uint64_t vertex_count;
    uint8_t compression; // 0=none, 1=zlib
    uint8_t reserved[7] = {0};
  } header;

  header.vertex_count = vertex_count;

  // Determine actual compression to use
#ifdef HAVE_ZLIB
  header.compression = use_compression ? 1 : 0;
#else
  header.compression = 0; // Force no compression if zlib not available
#endif

  ofs.write(reinterpret_cast<const char*>(&header), sizeof(FileHeader));

  if (vertex_count == 0) {
    ofs.close();
    return true;
  }

  // Step 2. Build and write deletion bitmap
  const size_t bitmap_bytes = (vertex_count + 7) / 8;
  std::vector<uint8_t> deletion_bitmap(bitmap_bytes, 0);

  for (uint64_t i = 0; i < vertex_count; ++i) {
    if (this->items[i].is_delete) {
      deletion_bitmap[i / 8] |= (1 << (i % 8));
    }
  }

  ofs.write(reinterpret_cast<const char*>(deletion_bitmap.data()), bitmap_bytes);

  // Step 3. Pack core data (exclude latch, is_delete, padding)
  // v2 layout per vertex:
  //   page_no(4) + vertex_type(4) + offset(2) + delta_head_page_no(4) + delta_head_record_idx(4) =
  //   18 bytes
  constexpr size_t core_item_size = 18;
  const size_t total_data_size = vertex_count * core_item_size;

  std::vector<char> data_buffer(total_data_size);

  char* write_ptr = data_buffer.data();
  for (uint64_t i = 0; i < vertex_count; ++i) {
    const auto& item = this->items[i];
    protected_vertex_version_t version(item.cur);
    const delta_head_t& dh =
        (i < this->delta_heads_.size()) ? this->delta_heads_[i] : delta_head_t::invalid();

    const page_no_t page_no = version.page_no();
    std::memcpy(write_ptr, &page_no, sizeof(page_no_t));
    write_ptr += sizeof(page_no_t);

    std::memcpy(write_ptr, &item.vertex_type, sizeof(vertex_type_t));
    write_ptr += sizeof(vertex_type_t);

    const uint16_t offset = version.offset();
    std::memcpy(write_ptr, &offset, sizeof(uint16_t));
    write_ptr += sizeof(uint16_t);

    std::memcpy(write_ptr, &dh.page_no, sizeof(page_no_t));
    write_ptr += sizeof(page_no_t);

    std::memcpy(write_ptr, &dh.record_idx, sizeof(uint32_t));
    write_ptr += sizeof(uint32_t);
  }

  // Step 4. Write data with optional compression
  bool write_success = false;

  if (header.compression == 0) {
    // No compression: direct buffered write
    write_success = write_buffered(ofs, data_buffer.data(), total_data_size, buffer_size);

#ifdef HAVE_ZLIB
  } else if (header.compression == 1) {
    // zlib compression (balanced speed/ratio)
    uLongf compressed_size = compressBound(total_data_size);
    std::vector<Bytef> compressed_buffer(compressed_size);

    int result = compress2(compressed_buffer.data(), &compressed_size,
                           reinterpret_cast<const Bytef*>(data_buffer.data()), total_data_size,
                           Z_DEFAULT_COMPRESSION // Level 6, balanced
    );

    if (result == Z_OK) {
      uint64_t comp_size = compressed_size;
      ofs.write(reinterpret_cast<const char*>(&comp_size), sizeof(uint64_t));
      write_success = write_buffered(ofs, reinterpret_cast<const char*>(compressed_buffer.data()),
                                     compressed_size, buffer_size);
    }
#endif
  }

  ofs.close();
  return write_success && ofs.good();
}

// Load from file.
bool vertex_index_t::load_from_file(const std::string& file_path, size_t buffer_size) {
  std::ifstream ifs(file_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  // Step 1. Read and validate file header
  struct FileHeader {
    uint64_t magic;
    uint64_t version;
    uint64_t vertex_count;
    uint8_t compression;
    uint8_t reserved[7];
  } header;

  ifs.read(reinterpret_cast<char*>(&header), sizeof(FileHeader));

  if (ifs.gcount() != sizeof(FileHeader) || header.magic != 0x5645525445585F49) {
    return false;
  }

  const uint64_t vertex_count = header.vertex_count;

  if (vertex_count == 0) {
    this->items.clear();
    this->global_latch = rw_latch_t();
    ifs.close();
    return true;
  }

  // Step 2. Read deletion bitmap
  const size_t bitmap_bytes = (vertex_count + 7) / 8;
  std::vector<uint8_t> deletion_bitmap(bitmap_bytes);

  ifs.read(reinterpret_cast<char*>(deletion_bitmap.data()), bitmap_bytes);

  if (ifs.gcount() != static_cast<std::streamsize>(bitmap_bytes)) {
    return false;
  }

  // Step 3. Read and decompress core data (auto-detect from header)
  // v1: 10 bytes/vertex; v2: 18 bytes/vertex (adds delta_head 8 bytes)
  const size_t core_item_size = (header.version >= 2) ? 18 : 10;
  const size_t total_data_size = vertex_count * core_item_size;
  std::vector<char> data_buffer(total_data_size);

  bool read_success = false;

  if (header.compression == 0) {
    // No compression: direct read
    read_success = read_buffered(ifs, data_buffer.data(), total_data_size, buffer_size);

#ifdef HAVE_ZLIB
  } else if (header.compression == 1) {
    // zlib decompression
    uint64_t compressed_size = 0;
    ifs.read(reinterpret_cast<char*>(&compressed_size), sizeof(uint64_t));

    if (ifs.gcount() != sizeof(uint64_t)) {
      return false;
    }

    std::vector<Bytef> compressed_buffer(compressed_size);
    if (read_buffered(ifs, reinterpret_cast<char*>(compressed_buffer.data()), compressed_size,
                      buffer_size)) {
      uLongf decompressed_size = total_data_size;
      int result = uncompress(reinterpret_cast<Bytef*>(data_buffer.data()), &decompressed_size,
                              compressed_buffer.data(), compressed_size);

      read_success = (result == Z_OK && decompressed_size == total_data_size);
    }
#endif
  } else {
    // Unsupported compression format
    return false;
  }

  if (!read_success) {
    return false;
  }

  // Step 4. Reconstruct items and delta_heads_ vectors
  this->items.clear();
  this->items.resize(vertex_count);
  this->delta_heads_.clear();
  this->delta_heads_.resize(vertex_count);

  const char* read_ptr = data_buffer.data();
  for (uint64_t i = 0; i < vertex_count; ++i) {
    auto& item = this->items[i];

    page_no_t page_no = 0;
    std::memcpy(&page_no, read_ptr, sizeof(page_no_t));
    read_ptr += sizeof(page_no_t);

    std::memcpy(&item.vertex_type, read_ptr, sizeof(vertex_type_t));
    read_ptr += sizeof(vertex_type_t);

    uint16_t offset = 0;
    std::memcpy(&offset, read_ptr, sizeof(uint16_t));
    read_ptr += sizeof(uint16_t);

    delete item.cur.exchange(new vertex_version_t(page_no, offset), std::memory_order_acq_rel);

    if (header.version >= 2) {
      // Restore delta head
      std::memcpy(&this->delta_heads_[i].page_no, read_ptr, sizeof(page_no_t));
      read_ptr += sizeof(page_no_t);
      std::memcpy(&this->delta_heads_[i].record_idx, read_ptr, sizeof(uint32_t));
      read_ptr += sizeof(uint32_t);
    }
    // v1 files: delta_heads_[i] stays at default (invalid)

    // Restore deletion mark from bitmap
    item.is_delete = (deletion_bitmap[i / 8] & (1 << (i % 8))) ? 1 : 0;

    // Initialize transient fields
    item.latch = rw_latch_t();
    item.padding = 0;
  }

  // Step 5. Initialize transient state
  this->global_latch = rw_latch_t();

  ifs.close();
  return ifs.good();
}

// Write buffered.
bool vertex_index_t::write_buffered(std::ofstream& ofs, const char* data, size_t size,
                                    size_t buffer_size) {
  size_t written = 0;
  while (written < size) {
    const size_t to_write = std::min(buffer_size, size - written);
    ofs.write(data + written, to_write);
    if (!ofs.good()) {
      return false;
    }
    written += to_write;
  }
  return true;
}

// Read in buffered manner
bool vertex_index_t::read_buffered(std::ifstream& ifs, char* data, size_t size,
                                   size_t buffer_size) {
  size_t read_total = 0;
  while (read_total < size) {
    const size_t to_read = std::min(buffer_size, size - read_total);
    ifs.read(data + read_total, to_read);
    if (ifs.gcount() != static_cast<std::streamsize>(to_read)) {
      return false;
    }
    read_total += to_read;
  }
  return true;
}
