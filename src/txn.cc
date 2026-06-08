#include "bw_graph/db/txn.h"

#ifdef BW_GRAPH_ENABLE_TRANSACTION

#include "bw_graph/db/db.h"

#include <iostream>

// FNV-1a hash function for simple key hashing
static uint32_t fnv1a_hash(const void* data, size_t len) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

// water_mark_t Implementation
void water_mark_t::add_reader(uint64_t ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  readers_[ts]++;
}

// Remove reader.
void water_mark_t::remove_reader(uint64_t ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = readers_.find(ts);
  if (it != readers_.end()) {
    it->second--;
    if (it->second == 0) {
      readers_.erase(it);
    }
  }
}

// Handle watermark.
std::optional<uint64_t> water_mark_t::watermark() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (readers_.empty()) {
    return std::nullopt;
  }
  return readers_.begin()->first;
}

// Handle num retained snapshots.
size_t water_mark_t::num_retained_snapshots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return readers_.size();
}

// Construct txn_manager_t.
txn_manager_t::txn_manager_t(uint64_t initial_ts)
    : current_ts_(initial_ts), watermark_(std::make_unique<water_mark_t>()) {}

// Handle latest commit ts.
uint64_t txn_manager_t::latest_commit_ts() const {
  return current_ts_.load(std::memory_order_acquire);
}

// Handle alloc commit ts.
uint64_t txn_manager_t::alloc_commit_ts() {
  return current_ts_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// Get watermark.
uint64_t txn_manager_t::get_watermark() const {
  std::lock_guard<std::mutex> lock(ts_mutex_);
  auto wm = watermark_->watermark();
  return wm.value_or(current_ts_.load(std::memory_order_acquire));
}

// Handle register reader.
void txn_manager_t::register_reader(uint64_t ts) {
  std::lock_guard<std::mutex> lock(ts_mutex_);
  watermark_->add_reader(ts);
}

// Handle unregister reader.
void txn_manager_t::unregister_reader(uint64_t ts) {
  std::lock_guard<std::mutex> lock(ts_mutex_);
  watermark_->remove_reader(ts);
}

// Add committed txn.
void txn_manager_t::add_committed_txn(uint64_t commit_ts, committed_txn_data_t data) {
  std::lock_guard<std::mutex> lock(committed_txns_mutex_);
  committed_txns_.emplace(commit_ts, std::move(data));
}

// Check serializable conflict.
bool txn_manager_t::check_serializable_conflict(
    uint64_t read_ts, const std::unordered_set<uint32_t>& read_set) const {
  std::lock_guard<std::mutex> lock(committed_txns_mutex_);

  // Check all committed transactions after our read timestamp
  auto it = committed_txns_.upper_bound(read_ts);
  for (; it != committed_txns_.end(); ++it) {
    const auto& committed_write_set = it->second.key_hashes;

    // Check if any of our reads conflict with committed writes
    for (uint32_t read_hash : read_set) {
      if (committed_write_set.find(read_hash) != committed_write_set.end()) {
        return true; // Conflict detected
      }
    }
  }

  return false; // No conflict
}

// Handle cleanup old txns.
void txn_manager_t::cleanup_old_txns() {
  std::lock_guard<std::mutex> lock(committed_txns_mutex_);
  uint64_t watermark = get_watermark();

  // Remove all transactions committed before watermark
  auto it = committed_txns_.begin();
  while (it != committed_txns_.end() && it->first < watermark) {
    it = committed_txns_.erase(it);
  }
}

// Handle acquire commit lock.
std::unique_lock<std::mutex> txn_manager_t::acquire_commit_lock() {
  return std::unique_lock<std::mutex>(commit_lock_);
}

// Handle acquire write lock.
std::unique_lock<std::mutex> txn_manager_t::acquire_write_lock() {
  return std::unique_lock<std::mutex>(write_lock_);
}

// Construct txn_t.
txn_t::txn_t(bw_graph_db_t* db, txn_manager_t* txn_manager, uint64_t read_ts, bool serializable)
    : db_(db), txn_manager_(txn_manager), read_ts_(read_ts), committed_(false),
      serializable_(serializable) {
  // Register this transaction as an active reader
  txn_manager_->register_reader(read_ts_);
}

// Destroy txn_t.
txn_t::~txn_t() {
  // Unregister reader on destruction
  txn_manager_->unregister_reader(read_ts_);
}

// Compute key hash.
uint32_t txn_t::compute_key_hash(v_id_t src, v_id_t dst) {
  if (dst == 0) {
    // Vertex key: just hash the vertex ID
    return fnv1a_hash(&src, sizeof(v_id_t));
  } else {
    // Edge key: hash both source and destination
    struct edge_key_t {
      v_id_t src;
      v_id_t dst;
    } key{src, dst};
    return fnv1a_hash(&key, sizeof(edge_key_t));
  }
}

// Get neighbors.
std::vector<v_id_t> txn_t::get_neighbors(v_id_t vertex_id) {
  if (committed_.load(std::memory_order_acquire)) {
    throw std::runtime_error("cannot operate on committed transaction!");
  }

  if (serializable_) {
    std::lock_guard<std::mutex> lock(sets_mutex_);
    uint32_t hash = compute_key_hash(vertex_id, 0);
    read_set_hashes_.insert(hash);
  }

  // MVCC snapshot read: only see delta records committed at or before read_ts_
  return db_->read_neighbor_clone_txn(vertex_id, read_ts_);
}

// Insert edge.
void txn_t::insert_edge(v_id_t src, v_id_t dst) {
  if (committed_.load(std::memory_order_acquire)) {
    throw std::runtime_error("cannot operate on committed transaction!");
  }

  write_ops_.emplace_back(write_ops_type_t::INSERT_EDGE, src, dst);

  if (serializable_) {
    std::lock_guard<std::mutex> lock(sets_mutex_);
    uint32_t hash = compute_key_hash(src, dst);
    write_set_hashes_.insert(hash);
  }
}

// Delete edge.
void txn_t::delete_edge(v_id_t src, v_id_t dst) {
  if (committed_.load(std::memory_order_acquire)) {
    throw std::runtime_error("cannot operate on committed transaction!");
  }

  write_ops_.emplace_back(write_ops_type_t::DELETE_EDGE, src, dst);

  if (serializable_) {
    std::lock_guard<std::mutex> lock(sets_mutex_);
    uint32_t hash = compute_key_hash(src, dst);
    write_set_hashes_.insert(hash);
  }
}

// Insert vertex.
void txn_t::insert_vertex(v_id_t vertex_id) {
  if (committed_.load(std::memory_order_acquire)) {
    throw std::runtime_error("cannot operate on committed transaction!");
  }

  write_ops_.emplace_back(write_ops_type_t::INSERT_VERTEX, vertex_id, 0);

  if (serializable_) {
    std::lock_guard<std::mutex> lock(sets_mutex_);
    uint32_t hash = compute_key_hash(vertex_id, 0);
    write_set_hashes_.insert(hash);
  }
}

// Delete vertex.
void txn_t::delete_vertex(v_id_t vertex_id) {
  if (committed_.load(std::memory_order_acquire)) {
    throw std::runtime_error("cannot operate on committed transaction!");
  }

  write_ops_.emplace_back(write_ops_type_t::DELETE_VERTEX, vertex_id, 0);

  if (serializable_) {
    std::lock_guard<std::mutex> lock(sets_mutex_);
    uint32_t hash = compute_key_hash(vertex_id, 0);
    write_set_hashes_.insert(hash);
  }
}

// Apply writes.
bool txn_t::apply_writes(timestamp_t begin_ts, timestamp_t commit_ts) {
  for (const auto& op : write_ops_) {
    bool success = false;

    switch (op.type) {
    case write_ops_type_t::INSERT_EDGE:
      success = db_->insert_edge_txn(op.src, op.dst, begin_ts, commit_ts);
      break;
    case write_ops_type_t::DELETE_EDGE:
      success = db_->delete_edge_txn(op.src, op.dst, begin_ts, commit_ts);
      break;
    case write_ops_type_t::INSERT_VERTEX:
      success = db_->insert_single_vertex(op.src);
      break;
    case write_ops_type_t::DELETE_VERTEX:
      success = db_->delete_vertex(op.src, false);
      break;
    }

    if (!success) {
      std::cerr << "Warning: write operation failed during commit" << std::endl;
    }
  }

  return true;
}

// Commit commit.
bool txn_t::commit() {
  // Use compare-exchange to ensure we only commit once
  bool expected = false;
  if (!committed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    throw std::runtime_error("cannot operate on committed transaction!");
  }

  // If no writes, just return success
  if (write_ops_.empty()) {
    return true;
  }

  // Acquire commit lock to serialize commits
  auto commit_lock = txn_manager_->acquire_commit_lock();

  // Check for serialization conflicts if in serializable mode
  if (serializable_ && !write_set_hashes_.empty()) {
    std::lock_guard<std::mutex> lock(sets_mutex_);

    if (txn_manager_->check_serializable_conflict(read_ts_, read_set_hashes_)) {
      std::cerr << "serializable check failed - conflict detected" << std::endl;
      return false;
    }
  }

  // Allocate commit timestamp BEFORE applying writes so every delta record
  // carries a valid commit_ts atomically (Option B: atomic-at-commit).
  timestamp_t commit_ts = txn_manager_->alloc_commit_ts();

  // Apply writes with both logical timestamps stamped on each delta record.
  if (!apply_writes(static_cast<timestamp_t>(read_ts_), commit_ts)) {
    return false;
  }

  // Record committed write set for serializability checking.
  if (serializable_) {
    std::lock_guard<std::mutex> lock(sets_mutex_);

    committed_txn_data_t txn_data(std::move(write_set_hashes_), read_ts_, commit_ts);

    txn_manager_->add_committed_txn(commit_ts, std::move(txn_data));
    txn_manager_->cleanup_old_txns();
  }

  return true;
}

// Abort abort.
void txn_t::abort() {
  bool expected = false;
  if (committed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    // Clear write operations without applying them
    write_ops_.clear();
  }
}
#endif // BW_GRAPH_ENABLE_TRANSACTION
