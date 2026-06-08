#ifndef IO_IO_DELTA_H
#define IO_IO_DELTA_H

#include "bw_graph/common/type.h"
#include "bw_graph/common/utils.h"
#include "bw_graph/storage/page.h"

#include <cstdint>

class csr_page_t;
class disk_manager_t;

/**
 * @brief Structure representing a single delta record for graph modifications.
 *
 * This structure encapsulates the details of a single change (delta) to the
 * graph, including the target of the change (vertex or edge) and the type of
 * operation (insertion or deletion).
 */
/**
 * @brief Sentinel value indicating no next record in the per-vertex delta chain.
 */
static constexpr page_no_t INVALID_DELTA_PAGE_NO = std::numeric_limits<page_no_t>::max();
static constexpr uint32_t INVALID_DELTA_RECORD_IDX = std::numeric_limits<uint32_t>::max();

struct delta_record_t {
public:
  /* The target of the delta operation (vertex or edge) */
  delta_target_t target{VERTEX};

  /* The type of the delta operation (insert or delete) */
  delta_type_t type{INSERT};

  /* The first vertex ID involved in the delta operation */
  v_id_t first{0};

  /* The second vertex ID involved in the delta operation (if applicable) */
  v_id_t second{0};

  /* The timestamp of the delta operation */
  uint64_t time{0};

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  /* Logical begin timestamp: the read_ts of the transaction that created this record */
  timestamp_t begin_ts{INVALID_TS};

  /* Logical commit timestamp: assigned atomically at commit; INVALID_TS if uncommitted */
  timestamp_t commit_ts{INVALID_TS};
#endif // BW_GRAPH_ENABLE_TRANSACTION

  /**
   * @brief Per-vertex delta linked list: page number of the next delta record
   *        for the same source vertex.  INVALID_DELTA_PAGE_NO if no next.
   *
   * Stored as stable page-id + record-index rather than a raw pointer so the
   * chain survives buffer-pool eviction and process restarts without rebuild.
   */
  page_no_t next_page_no{INVALID_DELTA_PAGE_NO};

  /**
   * @brief Index of the next delta record within next_page_no.
   *        INVALID_DELTA_RECORD_IDX if no next.
   */
  uint32_t next_record_idx{INVALID_DELTA_RECORD_IDX};

  /**
   * @brief Default constructor
   */
  delta_record_t() = default;

  /**
   * @brief Constructs a delta record.
   */
  delta_record_t(delta_target_t target, delta_type_t type, v_id_t first, v_id_t second)
      : target(target), type(type), first(first), second(second) {
    this->time = generate_timestamp();
  };

  /**
   * @brief Returns true when next_page_no/next_record_idx form a valid link.
   */
  bool has_next() const { return next_page_no != INVALID_DELTA_PAGE_NO; }
};

/**
 * @brief Span type for a sequence of delta records.
 *
 * This type is used to represent a contiguous sequence of delta_record_t
 * objects, facilitating efficient access and manipulation of delta records in
 * memory.
 */
using delta_span_t = std::span<delta_record_t>;

class delta_page_t : public page_t {
private:
  /* Indicates whether the page has been parsed */
  bool is_parse_{false};

  /* Total number of delta records in this page */
  uint64_t num_delta_records_;

  /* Span representing the delta records stored in this page */
  delta_span_t delta_records_span_;

  /* Used bytes in this page */
  uint64_t used_bytes_{0};

  /* The parent page number for this delta page */
  uint64_t parent_page_no_{0};

  /* The pointer to the next delta page */
  delta_page_t* next_delta_page_{nullptr};

  /* The status of this delta page */
  delta_page_status_t status_{ACCUMULATE};

public:
  /**
   * @brief Get the next page in the LRU/free list (typed wrapper over page_t)
   *
   * NOTE: this is distinct from next_delta_page_, which links the delta chain.
   */
  delta_page_t* get_next() const { return static_cast<delta_page_t*>(get_next_page()); }

  /**
   * @brief Get the previous page in the LRU/free list (typed wrapper over
   * page_t)
   */
  delta_page_t* get_prev() const { return static_cast<delta_page_t*>(get_prev_page()); }

  /**
   * @brief Set the next page in the LRU/free list (typed wrapper over page_t)
   */
  void set_next(delta_page_t* next) { set_next_page(next); }

  /**
   * @brief Set the previous page in the LRU/free list (typed wrapper over
   * page_t)
   */
  void set_prev(delta_page_t* prev) { set_prev_page(prev); }

  /**
   * @brief Disable further parsing of the delta page
   *
   * Resets the parse flag so the page will be re-parsed on next access.
   * Mirrors csr_page_t::disable_parse() for buffer-pool compatibility.
   */
  void disable_parse();

  /**
   * @brief Re-initialise this frame as a brand-new empty delta page.
   *
   * Called by buf_pool_t::buf_page_new() when reusing a pool frame for a
   * freshly allocated delta page.  Equivalent to running the
   * delta_page_t(page_no) constructor on an already-constructed object.
   *
   * @param page_no Page number to assign to this frame
   */
  void reinit(page_no_t page_no);

  /**
   * @brief Default constructor
   *
   * Initializes an empty delta page with no delta records
   */
  delta_page_t();

  /**
   * @brief Parameterized constructor
   * @param page_no The page number to initialize.
   * Initializes a delta page with the specified page number.
   */
  delta_page_t(const page_no_t page_no);

  /**
   * @brief Parses the delta page to extract metadata and records.
   *
   * This function reads the metadata from the page and sets up the internal
   * structures to access the delta records.
   */
  void self_parse();

  /**
   * @brief Inserts a new delta record into the page.
   *
   * @param delta_record The delta record to insert.
   * @return true if the insertion was successful,
   *         false if there was not enough space.
   */
  bool insert_delta(delta_record_t delta_record);

  /**
   * @brief Retrieves all delta records related to a specific vertex.
   *
   * @param vertex_id The ID of the vertex for which to retrieve related delta
   * records.
   * @return A vector of delta records related to the specified vertex.
   */
  std::vector<delta_record_t> get_related_records(v_id_t vertex_id);

  /**
   * @brief Get the parent page number
   * @return The parent page number
   */
  void set_parent_page_no(uint64_t parent_page_no);

  /**
   * @brief Get the next delta page
   * @return Pointer to the next delta page, or nullptr if none
   */
  void set_next_ptr(delta_page_t* next);

  /**
   * @brief Get the next delta page
   * @return Pointer to the next delta page, or nullptr if none
   */
  delta_page_t* get_next_delta_page() const;

  /**
   * @brief Set the status of the delta page
   * @param status The new status to set
   */
  void set_status(delta_page_status_t status);

  /**
   * @brief Get the status of the delta page
   * @return The current status of the delta page
   */
  delta_page_status_t get_status() const;

  /**
   * @brief Get the span of delta records in the page
   * @return The span of delta records
   */
  delta_span_t get_delta_span() const;

  /**
   * @brief Return the number of records currently stored in this page.
   */
  uint64_t get_num_records() const;

  /**
   * @brief Return a const reference to the record at the given index.
   *
   * Parses the page on first access.  Callers must ensure the page latch is
   * held (at minimum r_latch) for the duration of the reference.
   *
   * @param idx  Zero-based record index (must be < get_num_records())
   */
  const delta_record_t& get_record(uint32_t idx);

  /**
   * @brief Overwrite the per-vertex next-link of the record at `idx` so the
   *        chain terminates at this record.
   *
   * Used by the consolidation finaliser to detach an already-merged section
   * of the chain: every record placed on the old delta pages now lives
   * behind a freshly inserted record, but the new CSR page already embeds
   * the effects of those old records.  Clearing the boundary record's
   * next-link prunes the now-redundant tail in O(1).
   *
   * Caller must hold this page's write latch.
   * @param idx Zero-based record index whose next-link is being cleared.
   */
  void clear_record_next(uint32_t idx);

  /**
   * @brief Flush this page to disk if it is dirty.
   *
   * Uses try_w_lock() so it is completely non-blocking: if the page latch
   * is already held (e.g. during buf_pool reinit), the call returns
   * immediately without writing.  The page stays dirty and will be picked
   * up on the next flush cycle.
   *
   * @param disk_mgr  The delta-page disk manager
   * @return true if the page was actually written, false if skipped
   */
  bool flush_to_disk(disk_manager_t* disk_mgr);
};

#endif // IO_IO_DELTA_H