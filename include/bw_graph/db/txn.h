#ifndef BW_GRAPH_DB_TXN
#define BW_GRAPH_DB_TXN

#ifdef BW_GRAPH_ENABLE_TRANSACTION

#include "bw_graph/common/type.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

// Forward declarations
class bw_graph_db_t;

/**
 * @brief: Watermark for tracking active read timestamps
 * Manages reader timestamps to determine safe garbage collection points
 */
class water_mark_t {
private:
  std::map<uint64_t, size_t> readers_; // timestamp -> count
  mutable std::mutex mutex_;

public:
  water_mark_t() = default;

  /**
   * @brief: Add a reader at given timestamp
   */
  void add_reader(uint64_t ts);

  /**
   * @brief: Remove a reader at given timestamp
   */
  void remove_reader(uint64_t ts);

  /**
   * @brief: Get the lowest active read timestamp (watermark)
   * @return: Watermark timestamp, or nullopt if no readers
   */
  std::optional<uint64_t> watermark() const;

  /**
   * @brief: Get number of retained snapshots
   */
  size_t num_retained_snapshots() const;
};

/**
 * @brief: Data structure for committed transaction metadata
 * Used for serializable isolation level conflict detection
 */
struct committed_txn_data_t {
  std::unordered_set<uint32_t> key_hashes; // Write set hashes
  uint64_t read_ts;                        // Read timestamp
  uint64_t commit_ts;                      // Commit timestamp

  committed_txn_data_t(std::unordered_set<uint32_t>&& hashes, uint64_t r_ts, uint64_t c_ts)
      : key_hashes(std::move(hashes)), read_ts(r_ts), commit_ts(c_ts) {}
};

/**
 * @brief: Write operation types for transaction log
 */
enum class write_ops_type_t { INSERT_EDGE, DELETE_EDGE, INSERT_VERTEX, DELETE_VERTEX };

/**
 * @brief: Write operation record
 */
struct write_opt_t {
  write_ops_type_t type;
  v_id_t src; // Source vertex (or vertex_id for vertex operations)
  v_id_t dst; // Destination vertex (only for edge operations)

  write_opt_t(write_ops_type_t t, v_id_t s, v_id_t d = 0) : type(t), src(s), dst(d) {}
};

/**
 * @brief: Transaction manager for LSM-style MVCC
 * Manages timestamps, watermarks, and committed transaction metadata
 */
class txn_manager_t {
private:
  std::atomic<uint64_t> current_ts_;        // Current commit timestamp
  std::unique_ptr<water_mark_t> watermark_; // Active readers tracking
  mutable std::mutex ts_mutex_;             // Protects watermark operations

  std::mutex commit_lock_; // Serializes commits
  std::mutex write_lock_;  // Protects write operations

  // Committed transaction list for serializability checking
  std::map<uint64_t, committed_txn_data_t> committed_txns_;
  mutable std::mutex committed_txns_mutex_;

public:
  txn_manager_t(uint64_t initial_ts = 0);
  ~txn_manager_t() = default;

  /**
   * @brief: Get current commit timestamp
   */
  uint64_t latest_commit_ts() const;

  /**
   * @brief: Allocate and increment commit timestamp
   */
  uint64_t alloc_commit_ts();

  /**
   * @brief: Get watermark (all timestamps below can be GC'd)
   */
  uint64_t get_watermark() const;

  /**
   * @brief: Register a new reader at given timestamp
   */
  void register_reader(uint64_t ts);

  /**
   * @brief: Unregister a reader
   */
  void unregister_reader(uint64_t ts);

  /**
   * @brief: Add committed transaction for serializability checking
   */
  void add_committed_txn(uint64_t commit_ts, committed_txn_data_t data);

  /**
   * @brief: Check for serialization conflicts
   * @return: true if conflict detected, false otherwise
   */
  bool check_serializable_conflict(uint64_t read_ts,
                                   const std::unordered_set<uint32_t>& read_set) const;

  /**
   * @brief: Cleanup old committed transaction records below watermark
   */
  void cleanup_old_txns();

  /**
   * @brief: Get commit lock for transaction commit
   */
  std::unique_lock<std::mutex> acquire_commit_lock();

  /**
   * @brief: Get write lock for write operations
   */
  std::unique_lock<std::mutex> acquire_write_lock();
};

/**
 * @brief: Transaction class implementing snapshot isolation with optional
 * serializability
 */
class txn_t {
private:
  bw_graph_db_t* db_;          // Pointer to database
  txn_manager_t* txn_manager_; // Pointer to transaction manager
  uint64_t read_ts_;           // Read timestamp (snapshot)

  // Local write buffer
  std::vector<write_opt_t> write_ops_;

  // Committed flag
  std::atomic<bool> committed_;

  // For serializable isolation: track read/write sets
  bool serializable_;
  std::unordered_set<uint32_t> write_set_hashes_;
  std::unordered_set<uint32_t> read_set_hashes_;
  mutable std::mutex sets_mutex_;

  /**
   * @brief: Compute hash for vertex/edge key
   */
  static uint32_t compute_key_hash(v_id_t src, v_id_t dst = 0);

  /**
   * @brief: Apply write operations to database with logical timestamps
   */
  bool apply_writes(timestamp_t begin_ts, timestamp_t commit_ts);

public:
  /**
   * @brief: Constructor
   * @param db: Pointer to the graph database
   * @param txn_manager: Pointer to transaction manager
   * @param read_ts: Read timestamp for this transaction
   * @param serializable: Enable serializability checking
   */
  txn_t(bw_graph_db_t* db, txn_manager_t* txn_manager, uint64_t read_ts, bool serializable = false);

  /**
   * @brief: Destructor - unregister reader
   */
  ~txn_t();

  // Disable copy
  txn_t(const txn_t&) = delete;
  txn_t& operator=(const txn_t&) = delete;

  /**
   * @brief: Get neighbors of a vertex (read operation)
   * @param vertex_id: The vertex ID to read
   * @return: Vector of neighbor vertex IDs
   */
  std::vector<v_id_t> get_neighbors(v_id_t vertex_id);

  /**
   * @brief: Insert an edge (write operation)
   * @param src: Source vertex ID
   * @param dst: Destination vertex ID
   */
  void insert_edge(v_id_t src, v_id_t dst);

  /**
   * @brief: Delete an edge (write operation)
   * @param src: Source vertex ID
   * @param dst: Destination vertex ID
   */
  void delete_edge(v_id_t src, v_id_t dst);

  /**
   * @brief: Insert a vertex (write operation)
   * @param vertex_id: Vertex ID to insert
   */
  void insert_vertex(v_id_t vertex_id);

  /**
   * @brief: Delete a vertex (write operation)
   * @param vertex_id: Vertex ID to delete
   */
  void delete_vertex(v_id_t vertex_id);

  /**
   * @brief: Commit the transaction
   * @return: true if commit succeeds, false if conflict detected
   */
  bool commit();

  /**
   * @brief: Abort the transaction (explicit)
   */
  void abort();

  /**
   * @brief: Check if transaction is committed
   */
  bool is_committed() const { return committed_.load(std::memory_order_acquire); }

  /**
   * @brief: Get read timestamp
   */
  uint64_t get_read_ts() const { return read_ts_; }
};

/**
 * @brief: RAII wrapper for transaction
 */
class txn_guard_t {
private:
  std::unique_ptr<txn_t> txn_;

public:
  explicit txn_guard_t(std::unique_ptr<txn_t> txn) : txn_(std::move(txn)) {}

  ~txn_guard_t() {
    if (txn_ && !txn_->is_committed()) {
      txn_->abort();
    }
  }

  txn_t* operator->() { return txn_.get(); }
  txn_t& operator*() { return *txn_; }
  txn_t* get() { return txn_.get(); }

  // Release ownership (after successful commit)
  void release() { txn_.reset(); }
};

#endif // BW_GRAPH_ENABLE_TRANSACTION
#endif // BW_GRAPH_DB_TXN