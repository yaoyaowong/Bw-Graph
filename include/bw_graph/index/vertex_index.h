#ifndef BW_GRAPH_INDEX_VERTEX_INDEX_H
#define BW_GRAPH_INDEX_VERTEX_INDEX_H

#include "bw_graph/common/latch.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/mem/mem_g0com.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <folly/synchronization/Hazptr.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief Head of the per-vertex delta linked list in the vertex index.
 *
 * Stores a (delta_page_no, record_idx) pair pointing to the most-recently
 * inserted delta record for a vertex.  Records are chained via
 * delta_record_t::next_page_no / next_record_idx.
 *
 * An invalid head (no pending deltas) is represented by
 * page_no == INVALID_DELTA_PAGE_NO.
 */
struct delta_head_t {
  page_no_t page_no{INVALID_DELTA_PAGE_NO};
  uint32_t record_idx{INVALID_DELTA_RECORD_IDX};

  bool is_valid() const { return page_no != INVALID_DELTA_PAGE_NO; }

  static delta_head_t invalid() { return delta_head_t{}; }
};

struct vertex_version_t;

struct vertex_version_deleter_t {
  std::function<void(vertex_version_t*)> reclaim;

  void operator()(vertex_version_t* version) const;
};

/**
 * @brief Immutable CSR location published through v_item_t::cur.
 *
 * A version never points to a delta page. Delta records remain reachable only
 * through vertex_index_t::delta_heads_.
 */
struct vertex_version_t
    : public folly::hazptr_obj_base<vertex_version_t, std::atomic, vertex_version_deleter_t> {
  const page_no_t page_no;
  const uint16_t offset;

  /**
   * Transient construction link used to model new -> old while an SMO is in
   * progress. It is detached immediately after cur publishes this version.
   */
  vertex_version_t* previous;

  vertex_version_t(page_no_t page_no, uint16_t offset, vertex_version_t* previous = nullptr)
      : page_no(page_no), offset(offset), previous(previous) {}
};

struct vertex_version_reclaim_batch_t {
  explicit vertex_version_reclaim_batch_t(size_t version_count,
                                           std::function<void()> on_complete = {})
      : remaining(version_count), on_complete(std::move(on_complete)) {}

  std::atomic<size_t> remaining;
  std::function<void()> on_complete;
};

struct prepared_vertex_version_t {
  page_no_t first{0};
  uint16_t second{0};
  vertex_version_t* expected{nullptr};
  vertex_version_t* desired{nullptr};

  prepared_vertex_version_t() = default;
  prepared_vertex_version_t(page_no_t page_no, uint16_t offset, vertex_version_t* expected,
                            vertex_version_t* desired)
      : first(page_no), second(offset), expected(expected), desired(desired) {}

  prepared_vertex_version_t(prepared_vertex_version_t&& other) noexcept
      : first(other.first),
        second(other.second),
        expected(std::exchange(other.expected, nullptr)),
        desired(std::exchange(other.desired, nullptr)) {}

  prepared_vertex_version_t& operator=(prepared_vertex_version_t&& other) noexcept {
    if (this != &other) {
      delete desired;
      first = other.first;
      second = other.second;
      expected = std::exchange(other.expected, nullptr);
      desired = std::exchange(other.desired, nullptr);
    }
    return *this;
  }

  prepared_vertex_version_t(const prepared_vertex_version_t&) = delete;
  prepared_vertex_version_t& operator=(const prepared_vertex_version_t&) = delete;

  ~prepared_vertex_version_t() { delete desired; }
};

/**
 * @brief RAII hazard-pointer guard for one immutable vertex version.
 */
class protected_vertex_version_t {
public:
  protected_vertex_version_t() = default;
  explicit protected_vertex_version_t(const std::atomic<vertex_version_t*>& source);

  protected_vertex_version_t(protected_vertex_version_t&&) noexcept = default;
  protected_vertex_version_t& operator=(protected_vertex_version_t&&) noexcept = default;

  protected_vertex_version_t(const protected_vertex_version_t&) = delete;
  protected_vertex_version_t& operator=(const protected_vertex_version_t&) = delete;

  page_no_t page_no() const { return version_->page_no; }
  uint16_t offset() const { return version_->offset; }
  vertex_loc_t location() const { return {version_->page_no, version_->offset}; }
  const vertex_version_t* get() const { return version_; }

  void reset();

private:
  folly::hazptr_holder<> holder_;
  vertex_version_t* version_{nullptr};
};

/**
 * @brief Vertex metadata plus the atomic current CSR-location version.
 */
struct v_item_t {
  /**
   * @brief Current immutable CSR location.
   */
  std::atomic<vertex_version_t*> cur;

  /**
   * @brief Vertex type (NORMAL or GIANT)
   */
  vertex_type_t vertex_type;

  /**
   * @brief Vertex latch for concurrency control
   */
  rw_latch_t latch;

  /**
   * @brief Deletion mark: 0 = exists, non-zero = deleted
   */
  uint8_t is_delete;

  /**
   * @brief Reserved padding
   */
  uint8_t padding;
  /**
   * @brief Default constructor
   */
  v_item_t()
      : cur(new vertex_version_t(0, 0)), vertex_type(NORMAL), latch(), is_delete(0), padding(0) {}

  /**
   * @brief Parameterized constructor
   */
  v_item_t(page_no_t pno, uint16_t off, vertex_type_t type = NORMAL, uint8_t del = 0)
      : cur(new vertex_version_t(pno, off)), vertex_type(type), latch(), is_delete(del), padding(0) {}

  v_item_t(v_item_t&& other) noexcept
      : cur(other.cur.exchange(nullptr, std::memory_order_acq_rel)),
        vertex_type(other.vertex_type),
        latch(std::move(other.latch)),
        is_delete(other.is_delete),
        padding(other.padding) {}

  v_item_t& operator=(v_item_t&& other) noexcept {
    if (this != &other) {
      delete cur.exchange(other.cur.exchange(nullptr, std::memory_order_acq_rel),
                          std::memory_order_acq_rel);
      vertex_type = other.vertex_type;
      latch = std::move(other.latch);
      is_delete = other.is_delete;
      padding = other.padding;
    }
    return *this;
  }

  v_item_t(const v_item_t&) = delete;
  v_item_t& operator=(const v_item_t&) = delete;

  ~v_item_t() { delete cur.load(std::memory_order_relaxed); }
  /**
   * @brief Check if this vertex is deleted
   */
  inline bool deleted() const { return is_delete != 0; }

  /**
   * @brief Mark this vertex as deleted
   */
  inline void mark_deleted() { is_delete = 1; }

  /**
   * @brief Mark this vertex as existing
   */
  inline void mark_existing() { is_delete = 0; }

  /**
   * @brief Check if this is a giant vertex
   */
  inline bool is_giant() const { return vertex_type == GIANT; }

};

/**
 * @brief Vertex index structure
 */
struct vertex_index_t {
public:
  /**
   * @brief Vector of vertex items
   */
  std::vector<v_item_t> items;

  /**
   * @brief Global latch for the vertex index
   */
  rw_latch_t global_latch;

  /**
   * @brief Per-vertex delta linked-list heads.
   *
   * delta_heads_[v] is the head of the per-vertex delta chain for vertex v.
   * Always kept in sync with items (same size).  Protected per-vertex by the
   * corresponding items[v].latch (read latch for read, write latch for write).
   */
  std::vector<delta_head_t> delta_heads_;

  /**
   * @brief Default constructor
   */
  vertex_index_t();

  ~vertex_index_t();

  /**
   * @brief Default constructor
   * @param vertex_count The vertex count in this graph.
   */
  vertex_index_t(uint64_t vertex_count);

  /**
   * @brief Constructor from raw graph and partition
   *
   * Constructs a vertex index from the given raw graph data and partition
   * information. This constructor processes the raw graph structure, applies
   * the specified partitioning scheme, and generates the corresponding page
   * structures that are populated into the provided mutable pages vector.
   *
   * @param raw_graph The raw graph data structure containing vertex and edge
   * information
   * @param
   * @param mut_raw_pages Mutable reference to vector of pages where the
   * constructed page structures will be stored. This vector will be populated
   *                      with the generated pages during the construction
   * process.
   *
   */
  vertex_index_t(mem_sub_com_t raw_graph, const graph_partition_t& partition,
                 std::vector<csr_page_t*>& mut_raw_pages, std::vector<v_id_t>& giant_vertex_ids);

  /**
   * @brief Get the vertex location by ID
   *
   * @param vertex_id The ID of the vertex
   * @return The location of the vertex as a pair (page number, offset)
   */
  vertex_loc_t get_vertex_location(v_id_t vertex_id) const;

  /**
   * @brief Protect the current version until the caller has latched its CSR page.
   */
  protected_vertex_version_t protect_vertex_version(v_id_t vertex_id) const;

  /**
   * @brief Update the vertex location by ID
   *
   * @param vertex_id The ID of the vertex
   * @param page_no The new page number of the vertex
   * @param offset The new offset of the vertex
   */
  void update_vertex_loc(v_id_t vertex_id, page_no_t page_no, uint64_t offset);

  /**
   * @brief Prepare a new version without publishing it.
   */
  prepared_vertex_version_t prepare_vertex_version(v_id_t vertex_id, page_no_t page_no,
                                                   uint16_t offset) const;

  /**
   * @brief CAS-publish a prepared version and enqueue the replaced version for GC.
   */
  bool publish_prepared_version(
      v_id_t vertex_id, prepared_vertex_version_t& prepared,
      const std::shared_ptr<vertex_version_reclaim_batch_t>& reclaim_batch = nullptr);

  /**
   * @brief Delete an unpublished prepared version.
   */
  static void discard_prepared_version(prepared_vertex_version_t& prepared);

  size_t get_version_gc_worker_count() const { return version_gc_threads_.size(); }
  uint64_t get_reclaimed_version_count() const {
    return reclaimed_version_count_.load(std::memory_order_acquire);
  }
  void wait_for_version_gc();
  void shutdown_version_gc();

  /**
   * @brief Check if a vertex is a giant vertex
   *
   * @param vertex_id The ID of the vertex
   * @return True if the vertex is a giant vertex, false otherwise
   */
  bool is_giant_vertex(v_id_t vertex_id);

  /**
   * @brief Update the vertex type by ID
   *
   * @param vertex_id The ID of the vertex
   * @param vertex_type The new type of the vertex
   */
  void update_vertex_type(v_id_t vertex_id, vertex_type_t vertex_type);

  /**
   * @brief Acquire a read lock on a vertex
   *
   * @param vertex_id The ID of the vertex
   */
  void v_w_lock(v_id_t vertex_id);

  /**
   * @brief Release a write lock on a vertex
   *
   * @param vertex_id The ID of the vertex
   */
  void v_w_unlock(v_id_t vertex_id);

  /**
   * @brief Acquire a read lock on a vertex
   *
   * @param vertex_id The ID of the vertex
   */
  void v_r_lock(v_id_t vertex_id);

  /**
   * @brief Release a read lock on a vertex
   *
   * @param vertex_id The ID of the vertex
   */
  void v_r_unlock(v_id_t vertex_id);

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
  bool save_to_file(const std::string& file_path, bool use_compression = true,
                    size_t buffer_size = 8 * 1024 * 1024) const;

  /**
   * @brief Load the vertex index from a binary file
   *
   * @param file_path The path to the file from which the index will be loaded
   * @return True if the load operation is successful, false otherwise
   */
  bool load_from_file(const std::string& file_path, size_t buffer_size = 8 * 1024 * 1024);

  /**
   * @brief Allocate a new vertex ID
   * @return The allocated vertex ID
   */
  v_id_t allocate_new_vertex();

private:
  struct retired_version_t {
    vertex_version_t* version;
    std::shared_ptr<vertex_version_reclaim_batch_t> batch;
  };

  std::vector<std::thread> version_gc_threads_;
  std::deque<retired_version_t> retired_versions_;
  std::mutex version_gc_mutex_;
  std::condition_variable version_gc_cv_;
  std::condition_variable version_gc_idle_cv_;
  std::atomic<bool> version_gc_stop_{false};
  std::atomic<uint64_t> pending_version_count_{0};
  std::atomic<uint64_t> reclaimed_version_count_{0};

  void start_version_gc();
  void version_gc_loop();
  void retire_version(vertex_version_t* version,
                      std::shared_ptr<vertex_version_reclaim_batch_t> reclaim_batch);
  void submit_version_to_hazptr(retired_version_t retired);

  /**
   * @brief Buffered write helper
   */
  static bool write_buffered(std::ofstream& ofs, const char* data, size_t size, size_t buffer_size);

  /**
   * @brief Buffered read helper
   */
  static bool read_buffered(std::ifstream& ifs, char* data, size_t size, size_t buffer_size);
};

#endif // BW_GRAPH_INDEX_VERTEX_INDEX_H
