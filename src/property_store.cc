#include "bw_graph/db/property_store.h"

#include <cstring>
#include <iostream>

property_store_t::property_store_t(const std::string& db_path) : db_path_(db_path) {
  initialize_options();
  // RocksDB >= 9.x changed DB::Open to take std::unique_ptr<DB>*.
  // Adapt by opening into a unique_ptr then releasing the ownership to db_.
  std::unique_ptr<rocksdb::DB> tmp_db;
  rocksdb::Status status = rocksdb::DB::Open(options_, db_path_, &tmp_db);
  if (!status.ok()) {
    throw std::runtime_error("property_store_t: failed to open RocksDB at " + db_path_ + ": " +
                             status.ToString());
  }
  db_ = tmp_db.release();
  std::cout << "Property store opened at: " << db_path_ << std::endl;
}

// Destroy property_store_t.
property_store_t::~property_store_t() {
  if (db_ != nullptr) {
    delete db_;
    db_ = nullptr;
  }
}

void property_store_t::initialize_options() {
  options_.create_if_missing = true;
  options_.error_if_exists = false;

  // Optimise for SSD mixed read/write workload
  options_.use_direct_reads = true;
  options_.use_direct_io_for_flush_and_compaction = true;
  options_.allow_mmap_reads = false;

  options_.write_buffer_size = 64 * 1024 * 1024; // 64 MB
  options_.max_write_buffer_number = 3;
  options_.min_write_buffer_number_to_merge = 1;
  options_.target_file_size_base = 64 * 1024 * 1024; // 64 MB
  options_.max_open_files = 1000;
  options_.max_background_jobs = 4;

  options_.level0_file_num_compaction_trigger = 4;
  options_.level0_slowdown_writes_trigger = 20;
  options_.level0_stop_writes_trigger = 36;

  rocksdb::BlockBasedTableOptions tbl;
  tbl.block_cache = rocksdb::NewLRUCache(16 * 1024 * 1024);
  tbl.cache_index_and_filter_blocks = true;
  tbl.pin_l0_filter_and_index_blocks_in_cache = true;
  tbl.block_size = 8 * 1024; // 8 KB
  options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tbl));
  options_.row_cache = nullptr;
}

void property_store_t::write_be32(char* buf, uint32_t v) {
  buf[0] = static_cast<char>((v >> 24) & 0xFF);
  buf[1] = static_cast<char>((v >> 16) & 0xFF);
  buf[2] = static_cast<char>((v >> 8) & 0xFF);
  buf[3] = static_cast<char>(v & 0xFF);
}

// Write be16.
void property_store_t::write_be16(char* buf, uint16_t v) {
  buf[0] = static_cast<char>((v >> 8) & 0xFF);
  buf[1] = static_cast<char>(v & 0xFF);
}

std::string property_store_t::make_vertex_prop_key(v_id_t vertex_id,
                                                   const std::string& prop_key) const {
  // [tag:1][v_id BE:4][prop_key_len BE:2][prop_key:N]
  const uint16_t key_len = static_cast<uint16_t>(prop_key.size());
  std::string key(1 + 4 + 2 + prop_key.size(), '\0');
  char* p = key.data();
  p[0] = static_cast<char>(VERTEX_PROP_TAG);
  write_be32(p + 1, vertex_id);
  write_be16(p + 5, key_len);
  std::memcpy(p + 7, prop_key.data(), prop_key.size());
  return key;
}

// Handle make vertex prefix.
std::string property_store_t::make_vertex_prefix(v_id_t vertex_id) const {
  // [tag:1][v_id BE:4]  ->  5 bytes
  std::string prefix(5, '\0');
  prefix[0] = static_cast<char>(VERTEX_PROP_TAG);
  write_be32(prefix.data() + 1, vertex_id);
  return prefix;
}

// Handle make edge prop key.
std::string property_store_t::make_edge_prop_key(v_id_t src_id, v_id_t dst_id,
                                                 const std::string& prop_key) const {
  // [tag:1][src BE:4][dst BE:4][prop_key_len BE:2][prop_key:N]
  const uint16_t key_len = static_cast<uint16_t>(prop_key.size());
  std::string key(1 + 4 + 4 + 2 + prop_key.size(), '\0');
  char* p = key.data();
  p[0] = static_cast<char>(EDGE_PROP_TAG);
  write_be32(p + 1, src_id);
  write_be32(p + 5, dst_id);
  write_be16(p + 9, key_len);
  std::memcpy(p + 11, prop_key.data(), prop_key.size());
  return key;
}

// Handle make edge prefix.
std::string property_store_t::make_edge_prefix(v_id_t src_id, v_id_t dst_id) const {
  // [tag:1][src BE:4][dst BE:4]  ->  9 bytes
  std::string prefix(9, '\0');
  prefix[0] = static_cast<char>(EDGE_PROP_TAG);
  write_be32(prefix.data() + 1, src_id);
  write_be32(prefix.data() + 5, dst_id);
  return prefix;
}

// Handle extract prop name.
std::string property_store_t::extract_prop_name(const rocksdb::Slice& raw_key,
                                                size_t prefix_len) const {
  // After the prefix [tag+IDs], the next 2 bytes are prop_key_len (BE),
  // then the property name follows.
  const size_t fixed_len = prefix_len + 2; // prefix + 2-byte length field
  if (raw_key.size() <= fixed_len) {
    return {};
  }
  const char* data = raw_key.data();
  const uint16_t key_len = static_cast<uint16_t>((static_cast<uint8_t>(data[prefix_len]) << 8) |
                                                 static_cast<uint8_t>(data[prefix_len + 1]));
  if (raw_key.size() < fixed_len + key_len) {
    return {};
  }
  return std::string(data + fixed_len, key_len);
}

bool property_store_t::set_vertex_property(v_id_t vertex_id, const std::string& prop_key,
                                           const std::string& prop_value) {
  const std::string key = make_vertex_prop_key(vertex_id, prop_key);
  rocksdb::WriteOptions wo;
  wo.sync = false;
  rocksdb::Status s = db_->Put(wo, key, prop_value);
  if (!s.ok()) {
    std::cerr << "property_store: set_vertex_property(" << vertex_id << ", " << prop_key
              << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

// Get vertex property.
std::optional<std::string> property_store_t::get_vertex_property(v_id_t vertex_id,
                                                                 const std::string& prop_key) {
  const std::string key = make_vertex_prop_key(vertex_id, prop_key);
  std::string value;
  rocksdb::ReadOptions ro;
  ro.verify_checksums = false;
  ro.fill_cache = true;
  rocksdb::Status s = db_->Get(ro, key, &value);
  if (s.IsNotFound()) {
    return std::nullopt;
  }
  if (!s.ok()) {
    std::cerr << "property_store: get_vertex_property(" << vertex_id << ", " << prop_key
              << ") failed: " << s.ToString() << std::endl;
    return std::nullopt;
  }
  return value;
}

// Delete vertex property.
bool property_store_t::delete_vertex_property(v_id_t vertex_id, const std::string& prop_key) {
  const std::string key = make_vertex_prop_key(vertex_id, prop_key);
  rocksdb::Status s = db_->Delete(rocksdb::WriteOptions(), key);
  if (!s.ok() && !s.IsNotFound()) {
    std::cerr << "property_store: delete_vertex_property(" << vertex_id << ", " << prop_key
              << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

// Get all vertex properties.
std::unordered_map<std::string, std::string>
property_store_t::get_all_vertex_properties(v_id_t vertex_id) {
  std::unordered_map<std::string, std::string> result;
  const std::string prefix = make_vertex_prefix(vertex_id);

  rocksdb::ReadOptions ro;
  ro.verify_checksums = false;
  ro.fill_cache = true;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));

  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    std::string name = extract_prop_name(it->key(), 5 /* vertex prefix len */);
    if (!name.empty()) {
      result.emplace(std::move(name), it->value().ToString());
    }
  }
  return result;
}

// Delete all vertex properties.
bool property_store_t::delete_all_vertex_properties(v_id_t vertex_id) {
  const std::string prefix = make_vertex_prefix(vertex_id);
  rocksdb::WriteBatch batch;

  rocksdb::ReadOptions ro;
  ro.verify_checksums = false;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));

  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    batch.Delete(it->key());
  }

  if (batch.Count() == 0) {
    return true;
  }
  rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
  if (!s.ok()) {
    std::cerr << "property_store: delete_all_vertex_properties(" << vertex_id
              << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

// Set vertex properties.
bool property_store_t::set_vertex_properties(
    v_id_t vertex_id, const std::unordered_map<std::string, std::string>& properties) {
  rocksdb::WriteBatch batch;
  for (const auto& [prop_key, prop_value] : properties) {
    const std::string key = make_vertex_prop_key(vertex_id, prop_key);
    batch.Put(key, prop_value);
  }
  rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
  if (!s.ok()) {
    std::cerr << "property_store: set_vertex_properties(" << vertex_id
              << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

bool property_store_t::set_edge_property(v_id_t src_id, v_id_t dst_id, const std::string& prop_key,
                                         const std::string& prop_value) {
  const std::string key = make_edge_prop_key(src_id, dst_id, prop_key);
  rocksdb::WriteOptions wo;
  wo.sync = false;
  rocksdb::Status s = db_->Put(wo, key, prop_value);
  if (!s.ok()) {
    std::cerr << "property_store: set_edge_property(" << src_id << "->" << dst_id << ", "
              << prop_key << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

// Get edge property.
std::optional<std::string> property_store_t::get_edge_property(v_id_t src_id, v_id_t dst_id,
                                                               const std::string& prop_key) {
  const std::string key = make_edge_prop_key(src_id, dst_id, prop_key);
  std::string value;
  rocksdb::ReadOptions ro;
  ro.verify_checksums = false;
  ro.fill_cache = true;
  rocksdb::Status s = db_->Get(ro, key, &value);
  if (s.IsNotFound()) {
    return std::nullopt;
  }
  if (!s.ok()) {
    std::cerr << "property_store: get_edge_property(" << src_id << "->" << dst_id << ", "
              << prop_key << ") failed: " << s.ToString() << std::endl;
    return std::nullopt;
  }
  return value;
}

// Delete edge property.
bool property_store_t::delete_edge_property(v_id_t src_id, v_id_t dst_id,
                                            const std::string& prop_key) {
  const std::string key = make_edge_prop_key(src_id, dst_id, prop_key);
  rocksdb::Status s = db_->Delete(rocksdb::WriteOptions(), key);
  if (!s.ok() && !s.IsNotFound()) {
    std::cerr << "property_store: delete_edge_property(" << src_id << "->" << dst_id << ", "
              << prop_key << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

// Get all edge properties.
std::unordered_map<std::string, std::string>
property_store_t::get_all_edge_properties(v_id_t src_id, v_id_t dst_id) {
  std::unordered_map<std::string, std::string> result;
  const std::string prefix = make_edge_prefix(src_id, dst_id);

  rocksdb::ReadOptions ro;
  ro.verify_checksums = false;
  ro.fill_cache = true;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));

  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    std::string name = extract_prop_name(it->key(), 9 /* edge prefix len */);
    if (!name.empty()) {
      result.emplace(std::move(name), it->value().ToString());
    }
  }
  return result;
}

// Delete all edge properties.
bool property_store_t::delete_all_edge_properties(v_id_t src_id, v_id_t dst_id) {
  const std::string prefix = make_edge_prefix(src_id, dst_id);
  rocksdb::WriteBatch batch;

  rocksdb::ReadOptions ro;
  ro.verify_checksums = false;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));

  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    batch.Delete(it->key());
  }

  if (batch.Count() == 0) {
    return true;
  }
  rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
  if (!s.ok()) {
    std::cerr << "property_store: delete_all_edge_properties(" << src_id << "->" << dst_id
              << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

// Set edge properties.
bool property_store_t::set_edge_properties(
    v_id_t src_id, v_id_t dst_id, const std::unordered_map<std::string, std::string>& properties) {
  rocksdb::WriteBatch batch;
  for (const auto& [prop_key, prop_value] : properties) {
    const std::string key = make_edge_prop_key(src_id, dst_id, prop_key);
    batch.Put(key, prop_value);
  }
  rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
  if (!s.ok()) {
    std::cerr << "property_store: set_edge_properties(" << src_id << "->" << dst_id
              << ") failed: " << s.ToString() << std::endl;
    return false;
  }
  return true;
}

void property_store_t::flush_wal() {
  rocksdb::Status s = db_->FlushWAL(true);
  if (!s.ok()) {
    std::cerr << "property_store: flush_wal failed: " << s.ToString() << std::endl;
  }
}

// Compact compact.
void property_store_t::compact() {
  rocksdb::Status s = db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
  if (!s.ok()) {
    std::cerr << "property_store: compact failed: " << s.ToString() << std::endl;
  }
}
