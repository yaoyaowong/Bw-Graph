#include "bw_graph/db/external.h"
#if defined(__linux__) && defined(USE_PFORDELTA)
#include <SIMDCompressionAndIntersection/codecfactory.h>
#else
#include <folly/GroupVarint.h>
#endif

// Construct giant_vertex_db_t.
giant_vertex_db_t::giant_vertex_db_t(const std::string& graph_name, const std::string& db_path)
    : db_path_(db_path), graph_name_(graph_name) {
  initialize_options();

  // Open the database
  // RocksDB >= 9.x changed DB::Open to take std::unique_ptr<DB>*.
  // Adapt by opening into a unique_ptr then releasing the ownership to db_.
  std::unique_ptr<rocksdb::DB> tmp_db;
  rocksdb::Status status = rocksdb::DB::Open(options_, db_path_, &tmp_db);
  if (!status.ok()) {
    throw std::runtime_error("Failed to open RocksDB at " + db_path_ + ": " + status.ToString());
  }
  db_ = tmp_db.release();

  std::cout << "Giant vertex database '" << graph_name_ << "' opened successfully at: " << db_path_
            << std::endl;
}

// Destroy giant_vertex_db_t.
giant_vertex_db_t::~giant_vertex_db_t() {
  if (db_ != nullptr) {
    delete db_;
    db_ = nullptr;
  }
}

// Initialize options.
void giant_vertex_db_t::initialize_options() {
  // Basic database options
  options_.create_if_missing = true;
  options_.error_if_exists = false;

  // Performance optimizations for SSD
  options_.use_direct_reads = true;
  options_.use_direct_io_for_flush_and_compaction = true;
  options_.allow_mmap_reads = false;

  // Memory and write optimizations
  options_.write_buffer_size = 128 * 1024 * 1024;     // 128MB write buffer (increased)
  options_.max_write_buffer_number = 4;               // Allow 4 write buffers
  options_.min_write_buffer_number_to_merge = 2;      // Merge when 2 buffers available
  options_.target_file_size_base = 128 * 1024 * 1024; // 128MB SST files

  // File and background job optimization
  options_.max_open_files = 2000;   // Increased file handles
  options_.max_background_jobs = 6; // More background threads

  // Compaction optimization
  options_.level0_file_num_compaction_trigger = 4;
  options_.level0_slowdown_writes_trigger = 20;
  options_.level0_stop_writes_trigger = 36;

  // Block-based table optimizations with selective caching
  rocksdb::BlockBasedTableOptions table_options;

  // Enable block cache for better read performance (32MB)
  table_options.block_cache = rocksdb::NewLRUCache(32 * 1024 * 1024);
  table_options.cache_index_and_filter_blocks = true;
  table_options.pin_l0_filter_and_index_blocks_in_cache = true;
  table_options.pin_top_level_index_and_filter = true;

  // Block size optimization
  table_options.block_size = 16 * 1024; // 16KB blocks

  // Apply table configuration
  options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

  // Disable row cache to prevent double caching
  options_.row_cache = nullptr;
}

// Handle make vertex key.
std::string giant_vertex_db_t::make_vertex_key(v_id_t vertex_id) const {
  return "v:" + std::to_string(vertex_id);
}

// Serialize neighbors.
std::string giant_vertex_db_t::serialize_neighbors(const std::vector<v_id_t>& neighbors) const {
  if (neighbors.empty()) {
    return "";
  }

  // COMPRESSED MODE: Use Delta+Group VarInt encoding

  // Sort neighbors for better delta compression
  std::vector<v_id_t> sorted_neighbors = neighbors;
  std::sort(sorted_neighbors.begin(), sorted_neighbors.end());

  // Compress using Delta+Group VarInt
  std::vector<uint8_t> compressed = compress_neighbors(sorted_neighbors);

  // Create result with compression marker
  // Format: [compressed_flag:1byte][degree:8bytes][compressed_data:N bytes]
  std::string value;
  value.resize(1 + sizeof(uint64_t) + compressed.size());

  char* ptr = value.data();

  // Write compression flag (1 = compressed with Group VarInt)
  *ptr = 1;
  ptr += 1;

  // Write original degree
  uint64_t degree = sorted_neighbors.size();
  std::memcpy(ptr, &degree, sizeof(uint64_t));
  ptr += sizeof(uint64_t);

  // Write compressed data
  std::memcpy(ptr, compressed.data(), compressed.size());

  return value;
}

// Deserialize neighbors.
std::vector<v_id_t> giant_vertex_db_t::deserialize_neighbors(const std::string& data) const {
  if (data.empty()) {
    return {};
  }

  // Check minimum size (at least compression flag)
  if (data.size() < 1) {
    throw std::runtime_error("Invalid serialized data: too small");
  }

  const char* ptr = data.data();

  // Read compression flag
  uint8_t compression_flag = static_cast<uint8_t>(*ptr);
  ptr += 1;

  if (compression_flag == 1) {
    // COMPRESSED MODE: Decompress Delta+Group VarInt data

    // Read original degree
    if (data.size() < 1 + sizeof(uint64_t)) {
      throw std::runtime_error("Invalid compressed data: missing degree");
    }

    uint64_t degree;
    std::memcpy(&degree, ptr, sizeof(uint64_t));
    ptr += sizeof(uint64_t);

    // Get compressed data pointer
    const uint8_t* compressed_data = reinterpret_cast<const uint8_t*>(ptr);

    // Decompress
    std::vector<v_id_t> neighbors = decompress_neighbors(compressed_data, degree);

    return neighbors;

  } else if (compression_flag == 0) {
    // UNCOMPRESSED MODE: Direct read
    size_t neighbor_count = (data.size() - 1) / sizeof(v_id_t);
    std::vector<v_id_t> neighbors;
    neighbors.resize(neighbor_count);
    std::memcpy(neighbors.data(), ptr, neighbor_count * sizeof(v_id_t));

    return neighbors;

  } else {
    throw std::runtime_error("Invalid compression flag: " + std::to_string(compression_flag));
  }
}

// Insert vertex.
bool giant_vertex_db_t::insert_vertex(v_id_t vertex_id, const std::vector<v_id_t>& neighbors) {
  std::string key = make_vertex_key(vertex_id);
  std::string value = serialize_neighbors(neighbors);

  rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
  if (!status.ok()) {
    std::cerr << "Failed to insert vertex " << vertex_id << ": " << status.ToString() << std::endl;
    return false;
  }

  return true;
}
// Giant vertex operations
neighbor_span_t giant_vertex_db_t::read_neighbor(v_id_t vertex_id) {
  if (this->neighbor_cache.find(vertex_id) == this->neighbor_cache.end()) {
    // Cache miss;
    std::string key = make_vertex_key(vertex_id);
    std::string value;

    // Optimize read performance
    rocksdb::ReadOptions read_options;
    read_options.verify_checksums = false;
    read_options.fill_cache = true; // Use block cache for better performance

    rocksdb::Status status = db_->Get(read_options, key, &value);

    if (!status.ok()) {
      if (status.IsNotFound()) {
        return {}; // Return empty list for non-existent vertices
      }
      std::cerr << "Failed to read neighbors for vertex " << vertex_id << ": " << status.ToString()
                << std::endl;
      return {};
    }

    std::vector<v_id_t> neighbors = deserialize_neighbors(value);
    // Put it into cache;
    std::shared_ptr<std::vector<v_id_t>> neighbors_ptr =
        std::make_shared<std::vector<v_id_t>>(neighbors);
    neighbor_cache.insert_or_assign(vertex_id, neighbors_ptr);
  }
  // Cache hit;
  auto neighbors_ptr = neighbor_cache.at(vertex_id);
  return neighbor_span_t(neighbors_ptr->data(), neighbors_ptr->size());
}

// Read neighbor clone.
std::vector<v_id_t> giant_vertex_db_t::read_neighbor_clone(v_id_t vertex_id) {
  std::string key = make_vertex_key(vertex_id);
  std::string value;

  // Optimize read performance
  rocksdb::ReadOptions read_options;
  read_options.verify_checksums = false;
  read_options.fill_cache = true; // Use block cache for better performance

  rocksdb::Status status = db_->Get(read_options, key, &value);

  if (!status.ok()) {
    if (status.IsNotFound()) {
      return {}; // Return empty list for non-existent vertices
    }
    std::cerr << "Failed to read neighbors for vertex " << vertex_id << ": " << status.ToString()
              << std::endl;
    return {};
  }

  std::vector<v_id_t> neighbors = deserialize_neighbors(value);
  // Put it into cache;
  std::shared_ptr<std::vector<v_id_t>> neighbors_ptr =
      std::make_shared<std::vector<v_id_t>>(neighbors);
  neighbor_cache.insert_or_assign(vertex_id, neighbors_ptr);
  return neighbors;
}

// Add neighbor.
bool giant_vertex_db_t::add_neighbor(v_id_t vertex_id, v_id_t neighbor_id) {
  // Read current neighbors
  std::vector<v_id_t> neighbors = read_neighbor_clone(vertex_id);

  // Check if neighbor already exists
  if (std::find(neighbors.begin(), neighbors.end(), neighbor_id) != neighbors.end()) {
    return true; // Neighbor already exists, consider it success
  }

  // Add new neighbor
  neighbors.push_back(neighbor_id);

  // Store updated neighbor list
  return insert_vertex(vertex_id, neighbors);
}

// Remove neighbor.
bool giant_vertex_db_t::remove_neighbor(v_id_t vertex_id, v_id_t neighbor_id) {
  // Read current neighbors
  std::vector<v_id_t> neighbors = read_neighbor_clone(vertex_id);

  // Find and remove the neighbor
  auto it = std::find(neighbors.begin(), neighbors.end(), neighbor_id);
  if (it == neighbors.end()) {
    return true; // Neighbor doesn't exist, consider it success
  }

  neighbors.erase(it);

  // Store updated neighbor list
  return insert_vertex(vertex_id, neighbors);
}

// Handle batch insert vertices.
bool giant_vertex_db_t::batch_insert_vertices(
    const std::vector<std::pair<v_id_t, std::vector<v_id_t>>>& vertices) {
  rocksdb::WriteBatch batch;
  const size_t batch_size = 1000; // Process in batches of 1000

  std::cout << "Batch inserting " << vertices.size() << " vertices..." << std::endl;

  for (size_t i = 0; i < vertices.size(); ++i) {
    const auto& vertex_data = vertices[i];
    std::string key = make_vertex_key(vertex_data.first);
    std::string value = serialize_neighbors(vertex_data.second);

    batch.Put(key, value);

    // Commit batch when reaching batch size or at the end
    if ((i + 1) % batch_size == 0 || i == vertices.size() - 1) {
      rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
      if (!status.ok()) {
        std::cerr << "Failed to write batch at index " << i << ": " << status.ToString()
                  << std::endl;
        return false;
      }
      batch.Clear();

      // Progress reporting every 100,000 vertices
      if ((i + 1) % 100000 == 0) {
        std::cout << "Inserted " << (i + 1) << " vertices..." << std::endl;
      }
    }
  }

  std::cout << "Batch insertion completed successfully!" << std::endl;
  return true;
}

// Handle vertex exists.
bool giant_vertex_db_t::vertex_exists(v_id_t vertex_id) {
  std::string key = make_vertex_key(vertex_id);
  std::string value;

  rocksdb::ReadOptions read_options;
  read_options.verify_checksums = false;

  rocksdb::Status status = db_->Get(read_options, key, &value);
  return status.ok();
}

// Get vertex degree.
uint64_t giant_vertex_db_t::get_vertex_degree(v_id_t vertex_id) {
  neighbor_span_t neighbors = read_neighbor(vertex_id);
  return neighbors.size();
}

// Get vertex count.
uint64_t giant_vertex_db_t::get_vertex_count() const { return vertex_count_; }

// Set vertex count.
void giant_vertex_db_t::set_vertex_count(uint64_t count) { vertex_count_ = count; }

// Print stats.
void giant_vertex_db_t::print_stats() {
  std::string stats;
  if (db_->GetProperty("rocksdb.stats", &stats)) {
    std::cout << "=== RocksDB Statistics for " << graph_name_ << " ===" << std::endl;
    std::cout << stats << std::endl;
  } else {
    std::cout << "Failed to retrieve RocksDB statistics" << std::endl;
  }

  // Additional useful properties
  std::string num_keys;
  if (db_->GetProperty("rocksdb.estimate-num-keys", &num_keys)) {
    std::cout << "Estimated number of keys: " << num_keys << std::endl;
  }

  std::string total_sst_size;
  if (db_->GetProperty("rocksdb.total-sst-files-size", &total_sst_size)) {
    std::cout << "Total SST files size: " << total_sst_size << " bytes" << std::endl;
  }
}

// Flush wal.
void giant_vertex_db_t::flush_wal() {
  rocksdb::Status status = db_->FlushWAL(true);
  if (!status.ok()) {
    std::cerr << "Failed to flush WAL: " << status.ToString() << std::endl;
  }
}

// Compact database.
void giant_vertex_db_t::compact_database() {
  std::cout << "Starting database compaction..." << std::endl;
  rocksdb::Status status = db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
  if (!status.ok()) {
    std::cerr << "Failed to compact database: " << status.ToString() << std::endl;
  } else {
    std::cout << "Database compaction completed successfully!" << std::endl;
  }
}

// Handle make delta key.
std::string giant_vertex_db_t::make_delta_key(v_id_t vertex_id, uint64_t timestamp) const {
  // Use mark 'd:' to distinguish delta entries
  return "d:" + std::to_string(vertex_id) + ":" + std::to_string(timestamp);
}

// Serialize delta.
std::string giant_vertex_db_t::serialize_delta(const neighbor_delta_t& delta) const {
  std::string value;
  value.resize(sizeof(delta_operation_t) + sizeof(v_id_t) + sizeof(uint64_t));

  char* ptr = value.data();
  std::memcpy(ptr, &delta.operation, sizeof(delta_operation_t));
  ptr += sizeof(delta_operation_t);
  std::memcpy(ptr, &delta.neighbor_id, sizeof(v_id_t));
  ptr += sizeof(v_id_t);
  std::memcpy(ptr, &delta.timestamp, sizeof(uint64_t));

  return value;
}

// Deserialize delta.
neighbor_delta_t giant_vertex_db_t::deserialize_delta(const std::string& data) const {
  if (data.size() < sizeof(delta_operation_t) + sizeof(v_id_t) + sizeof(uint64_t)) {
    throw std::runtime_error("Invalid delta data size");
  }

  const char* ptr = data.data();
  delta_operation_t operation;
  v_id_t neighbor_id;
  uint64_t timestamp;

  std::memcpy(&operation, ptr, sizeof(delta_operation_t));
  ptr += sizeof(delta_operation_t);
  std::memcpy(&neighbor_id, ptr, sizeof(v_id_t));
  ptr += sizeof(v_id_t);
  std::memcpy(&timestamp, ptr, sizeof(uint64_t));

  return neighbor_delta_t(operation, neighbor_id, timestamp);
}

// Get current timestamp.
uint64_t giant_vertex_db_t::get_current_timestamp() const {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Add neighbor delta.
bool giant_vertex_db_t::add_neighbor_delta(v_id_t vertex_id, v_id_t neighbor_id) {
  uint64_t timestamp = get_current_timestamp();
  std::string key = make_delta_key(vertex_id, timestamp);
  neighbor_delta_t delta(delta_operation_t::ADD_NEIGHBOR, neighbor_id, timestamp);
  std::string value = serialize_delta(delta);

  rocksdb::WriteOptions write_options;
  write_options.sync = false;
  write_options.disableWAL = false;

  rocksdb::Status status = db_->Put(write_options, key, value);
  if (!status.ok()) {
    std::cerr << "Failed to add neighbor delta for vertex " << vertex_id << ": "
              << status.ToString() << std::endl;
    return false;
  }

  return true;
}

// Remove neighbor delta.
bool giant_vertex_db_t::remove_neighbor_delta(v_id_t vertex_id, v_id_t neighbor_id) {
  uint64_t timestamp = get_current_timestamp();
  std::string key = make_delta_key(vertex_id, timestamp);
  neighbor_delta_t delta(delta_operation_t::REMOVE_NEIGHBOR, neighbor_id, timestamp);
  std::string value = serialize_delta(delta);

  rocksdb::WriteOptions write_options;
  write_options.sync = false;
  write_options.disableWAL = false;

  rocksdb::Status status = db_->Put(write_options, key, value);
  if (!status.ok()) {
    std::cerr << "Failed to remove neighbor delta for vertex " << vertex_id << ": "
              << status.ToString() << std::endl;
    return false;
  }

  return true;
}

// Handle batch apply deltas.
bool giant_vertex_db_t::batch_apply_deltas(
    const std::vector<std::pair<v_id_t, std::vector<neighbor_delta_t>>>& vertex_deltas) {
  rocksdb::WriteBatch batch;
  const size_t batch_size = 1000;

  size_t total_deltas = 0;
  for (const auto& vertex_delta : vertex_deltas) {
    total_deltas += vertex_delta.second.size();
  }

  std::cout << "Batch applying " << total_deltas << " delta operations for " << vertex_deltas.size()
            << " vertices..." << std::endl;

  size_t processed = 0;
  for (const auto& vertex_delta : vertex_deltas) {
    v_id_t vertex_id = vertex_delta.first;
    const auto& deltas = vertex_delta.second;

    for (const auto& delta : deltas) {
      std::string key = make_delta_key(vertex_id, delta.timestamp);
      std::string value = serialize_delta(delta);
      batch.Put(key, value);

      ++processed;

      // Batch commit
      if (processed % batch_size == 0) {
        rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!status.ok()) {
          std::cerr << "Failed to write delta batch: " << status.ToString() << std::endl;
          return false;
        }
        batch.Clear();

        if (processed % 10000 == 0) {
          std::cout << "Applied " << processed << " delta operations..." << std::endl;
        }
      }
    }
  }

  if (batch.Count() != 0) {
    rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!status.ok()) {
      std::cerr << "Failed to write final delta batch: " << status.ToString() << std::endl;
      return false;
    }
  }

  std::cout << "Delta batch application completed successfully!" << std::endl;
  return true;
}

#if defined(__linux__) && defined(USE_PFORDELTA)

// Extra uint32_t elements appended to every encode/decode buffer so that
// SIMD instructions in the codec cannot read/write past the allocation.
static constexpr size_t SIMD_OVERREAD = 32;

// Compress neighbors.
std::vector<uint8_t>
giant_vertex_db_t::compress_neighbors(const std::vector<v_id_t>& neighbors) const {
  if (neighbors.empty()) {
    return {};
  }

  // s4-bp128-d1 has integrated delta-1 encoding for sorted inputs; no manual
  // delta step needed.  The codec requires a multiple of 128 elements, so we
  // pad the end with copies of the last value (delta = 0, compresses to 0 bits).
  size_t original_size = neighbors.size();
  size_t padded_size = ((original_size + 127) / 128) * 128;

  std::vector<uint32_t> in_buf(padded_size + SIMD_OVERREAD, neighbors.back());
  std::copy(neighbors.begin(), neighbors.end(), in_buf.begin());

  thread_local auto codec_ptr = SIMDCompressionLib::CODECFactory::getFromName("s4-bp128-d1");
  SIMDCompressionLib::IntegerCODEC& codec = *codec_ptr;

  std::vector<uint32_t> buf(padded_size * 2 + 1024 + SIMD_OVERREAD);
  size_t nvalue = buf.size();
  codec.encodeArray(in_buf.data(), padded_size, buf.data(), nvalue);

  // Output layout: [4B: nvalue (uint32_t count)][nvalue * 4B: compressed data]
  std::vector<uint8_t> output(sizeof(uint32_t) + nvalue * sizeof(uint32_t));
  uint32_t nv32 = static_cast<uint32_t>(nvalue);
  std::memcpy(output.data(), &nv32, sizeof(uint32_t));
  std::memcpy(output.data() + sizeof(uint32_t), buf.data(), nvalue * sizeof(uint32_t));
  return output;
}

#else

// Compress neighbors.
std::vector<uint8_t>
giant_vertex_db_t::compress_neighbors(const std::vector<v_id_t>& neighbors) const {
  if (neighbors.empty()) {
    return {};
  }

  std::vector<uint8_t> compressed;
  // Group VarInt encodes 4 uint32_t at a time
  // Worst case: 1 byte descriptor + 4*5 bytes data per group
  compressed.reserve(neighbors.size() * 5 / 4 + 100);

  // Step 1: Delta encode - compute deltas from sorted neighbors
  std::vector<v_id_t> deltas;
  deltas.reserve(neighbors.size());
  deltas.push_back(neighbors[0]); // First value stored directly

  for (size_t i = 1; i < neighbors.size(); ++i) {
    deltas.push_back(neighbors[i] - neighbors[i - 1]); // Store differences
  }

  // Step 2: Encode using Folly Group VarInt (processes 4 values at a time)
  size_t pos = 0;
  char buffer[17]; // Max size for 4 uint32_t: 1 descriptor + 4*4 bytes

  while (pos + 4 <= deltas.size()) {
    char* start = buffer;
    char* end = folly::GroupVarint32::encode(start, deltas[pos], deltas[pos + 1], deltas[pos + 2],
                                             deltas[pos + 3]);
    // Calculate encoded size from pointer difference
    size_t encoded_size = end - start;
    compressed.insert(compressed.end(), buffer, buffer + encoded_size);
    pos += 4;
  }

  // Step 3: Handle remaining values (less than 4) with standard varint
  while (pos < deltas.size()) {
    v_id_t value = deltas[pos];
    while (value >= 0x80) {
      compressed.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
      value >>= 7;
    }
    compressed.push_back(static_cast<uint8_t>(value & 0x7F));
    pos++;
  }

  compressed.shrink_to_fit();
  return compressed;
}

#endif

#if defined(__linux__) && defined(USE_PFORDELTA)

// Decompress neighbors.
std::vector<v_id_t> giant_vertex_db_t::decompress_neighbors(const uint8_t* data,
                                                            uint64_t degree) const {
  if (data == nullptr || degree == 0) {
    return {};
  }

  // Read compressed uint32_t count from header
  uint32_t nvalue;
  std::memcpy(&nvalue, data, sizeof(uint32_t));

  // Copy compressed bytes into an aligned vector so the SIMD loads inside the
  // codec see a properly aligned, padded buffer (the raw `data` pointer is
  // derived from a RocksDB string at offset +9 and is typically misaligned).
  std::vector<uint32_t> cbuf(nvalue + SIMD_OVERREAD);
  std::memcpy(cbuf.data(), data + sizeof(uint32_t), nvalue * sizeof(uint32_t));

  // s4-bp128-d1 reconstructs sorted values directly (integrated delta-1 decode).
  // Output buffer must be a multiple of 128; add SIMD overread slack.
  size_t padded_size = ((degree + 127) / 128) * 128;

  thread_local auto codec_ptr = SIMDCompressionLib::CODECFactory::getFromName("s4-bp128-d1");
  SIMDCompressionLib::IntegerCODEC& codec = *codec_ptr;

  std::vector<uint32_t> recovered(padded_size + SIMD_OVERREAD, 0);
  size_t nrecovered = padded_size;
  codec.decodeArray(cbuf.data(), nvalue, recovered.data(), nrecovered);

  // Trim to the original degree and return; s4-bp128-d1 already produced the
  // sorted neighbor IDs - no manual delta reconstruction needed.
  recovered.resize(degree);
  return recovered;
}

#else

// Decompress neighbors.
std::vector<v_id_t> giant_vertex_db_t::decompress_neighbors(const uint8_t* data,
                                                            uint64_t degree) const {
  if (data == nullptr || degree == 0) {
    return {};
  }

  std::vector<v_id_t> deltas;
  deltas.reserve(degree);

  const char* ptr = reinterpret_cast<const char*>(data);
  size_t decoded = 0;

  // Step 1: Decode groups of 4 using Folly Group VarInt
  while (decoded + 4 <= degree) {
    uint32_t vals[4];
    ptr = folly::GroupVarint32::decode(ptr, &vals[0], &vals[1], &vals[2], &vals[3]);
    deltas.insert(deltas.end(), vals, vals + 4);
    decoded += 4;
  }

  // Step 2: Decode remaining values (less than 4) with standard varint
  const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(ptr);
  while (decoded < degree) {
    v_id_t value = 0;
    int shift = 0;

    while (*byte_ptr & 0x80) {
      value |= static_cast<v_id_t>(*byte_ptr & 0x7F) << shift;
      shift += 7;
      byte_ptr++;
    }
    value |= static_cast<v_id_t>(*byte_ptr & 0x7F) << shift;
    byte_ptr++;

    deltas.push_back(value);
    decoded++;
  }

  // Step 3: Reconstruct original values from deltas (reverse delta encoding)
  std::vector<v_id_t> neighbors;
  neighbors.reserve(degree);

  v_id_t current = deltas[0]; // First value
  neighbors.push_back(current);

  for (size_t i = 1; i < deltas.size(); ++i) {
    current += deltas[i]; // Accumulate deltas
    neighbors.push_back(current);
  }

  return neighbors;
}

#endif

// Handle encode vbyte.
void giant_vertex_db_t::encode_vbyte(uint64_t value, std::vector<uint8_t>& output) const {
  while (value >= 128) {
    output.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  output.push_back(static_cast<uint8_t>(value & 0x7F));
}

// Handle decode vbyte.
uint64_t giant_vertex_db_t::decode_vbyte(const uint8_t*& ptr) const {
  uint64_t value = 0;
  int shift = 0;

  while (*ptr & 0x80) {
    value |= (static_cast<uint64_t>(*ptr & 0x7F) << shift);
    shift += 7;
    ptr++;
  }

  value |= (static_cast<uint64_t>(*ptr & 0x7F) << shift);
  ptr++;

  return value;
}

// Read neighbors with deltas applied.
std::vector<v_id_t> giant_vertex_db_t::read_neighbors_with_deltas_applied(v_id_t vertex_id) {
  // Read base neighbors
  std::vector<v_id_t> neighbors = read_neighbor_clone(vertex_id);
  std::set<v_id_t> neighbor_set(neighbors.begin(), neighbors.end());

  // Read all delta operations
  std::string prefix = "d:" + std::to_string(vertex_id) + ":";
  rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());

  std::vector<neighbor_delta_t> deltas;

  // Iterate through all relevant delta entries
  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    std::string value = it->value().ToString();
    try {
      neighbor_delta_t delta = deserialize_delta(value);
      deltas.push_back(delta);
    } catch (const std::exception& e) {
      std::cerr << "Failed to deserialize delta: " << e.what() << std::endl;
      continue;
    }
  }

  delete it;

  // Sort delta operations by timestamp
  std::sort(deltas.begin(), deltas.end(), [](const neighbor_delta_t& a, const neighbor_delta_t& b) {
    return a.timestamp < b.timestamp;
  });

  // Apply delta operations
  for (const auto& delta : deltas) {
    if (delta.operation == delta_operation_t::ADD_NEIGHBOR) {
      neighbor_set.insert(delta.neighbor_id);
    } else if (delta.operation == delta_operation_t::REMOVE_NEIGHBOR) {
      neighbor_set.erase(delta.neighbor_id);
    }
  }

  // Convert back to vector
  return std::vector<v_id_t>(neighbor_set.begin(), neighbor_set.end());
}

// Consolidate vertex deltas.
bool giant_vertex_db_t::consolidate_vertex_deltas(v_id_t vertex_id) {
  // Read final neighbor list after applying deltas
  std::vector<v_id_t> final_neighbors = read_neighbors_with_deltas_applied(vertex_id);

  // Update main record
  bool success = insert_vertex(vertex_id, final_neighbors);
  if (!success) {
    return false;
  }

  // Delete all delta records
  std::string prefix = "d:" + std::to_string(vertex_id) + ":";
  rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());
  rocksdb::WriteBatch batch;

  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    batch.Delete(it->key());
  }

  delete it;

  if (batch.Count() > 0) {
    rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!status.ok()) {
      std::cerr << "Failed to delete delta records: " << status.ToString() << std::endl;
      return false;
    }
  }

  return true;
}