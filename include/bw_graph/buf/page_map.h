#ifndef BW_GRAPH_BUF_PAGE_MAP_H
#define BW_GRAPH_BUF_PAGE_MAP_H

#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/common/type.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <atomic>
#include <cstdint>
#include <folly/concurrency/ConcurrentHashMap.h>

struct page_delta_t;

/**
 * @brief Page map entry — tracks the delta chain for one CSR page.
 */
struct page_map_entry_t {
private:
  /* The CSR page number this entry belongs to */
  page_no_t page_no;

  /* Head of the delta chain (singly-linked via next_delta_page_) */
  delta_page_t* delta_page;

  /* Number of delta pages currently in the chain */
  std::atomic<uint64_t> delta_page_count{0};

  /* Forwarding table installed for the duration of an in-flight
   * consolidation.  nullptr in steady state - writers then take the regular
   * path with a single atomic acquire-load on the hot path. */
  std::atomic<page_delta_t*> forwarder_{nullptr};

  /* True while an SMO task for this CSR page is queued or running. */
  std::atomic<bool> consolidating_{false};

public:
  /* Latch protecting this entry's chain pointer and count */
  rw_latch_t entry_latch;

  page_map_entry_t() = default;

  page_map_entry_t(page_no_t page_no, delta_page_t* delta_page, uint64_t delta_page_count);

  page_no_t get_page_no() const;

  uint64_t get_delta_page_count() const;

  delta_page_t* get_first_delta_page();

  std::vector<delta_page_t*> get_delta_pages();

  delta_page_t* get_first_delta_page() const;

  void set_delta_page_ptr(delta_page_t* new_delta_page);

  /** Increment chain length by one (called when a new page is prepended). */
  void inc_delta_page_count();

  /** Reset chain length to zero (called after consolidation). */
  void reset_delta_page_count();

  /** Atomically publish a forwarding table for an in-flight consolidation. */
  void install_forwarder(page_delta_t* fwd) {
    forwarder_.store(fwd, std::memory_order_release);
  }

  /** Atomically retrieve the currently installed forwarder (or nullptr). */
  page_delta_t* try_get_forwarder() const { return forwarder_.load(std::memory_order_acquire); }

  /** Atomically clear the forwarder, returning the previously installed one. */
  page_delta_t* detach_forwarder() {
    return forwarder_.exchange(nullptr, std::memory_order_acq_rel);
  }

  bool try_begin_consolidation() {
    bool expected = false;
    return consolidating_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }

  void end_consolidation() { consolidating_.store(false, std::memory_order_release); }
};

/**
 * @brief Concurrent hash map from CSR page_no → page_map_entry_t*
 */
using page_map_inner_t = folly::ConcurrentHashMap<page_no_t, page_map_entry_t*>;

/**
 * @brief Page map — manages the delta chains for all CSR pages.
 *
 * Delta pages are allocated from and managed by a buf_pool_t<delta_page_t>.
 * Allocated pages are pinned (pin_count > 0) while part of an active chain
 * and unpinned during fetch_deltas_with_mark so the pool can reuse them.
 */
struct page_map_t {
private:
  /* Inner concurrent map: CSR page_no → entry */
  page_map_inner_t page_map_inner;

  /* Buffer pool owning delta page frames */
  buf_pool_t<delta_page_t>* delta_pool_{nullptr};

  /* Disk manager for the delta page file */
  disk_manager_t* delta_disk_mgr_{nullptr};

public:
  /**
   * @brief Default constructor — for use before buffer pool is available.
   *        Must call set_pool() before inserting any deltas.
   */
  page_map_t() = default;

  /**
   * @brief Construct with a pre-initialised delta pool and disk manager.
   *
   * @param pool     Buffer pool managing delta page frames
   * @param disk_mgr Disk manager for the delta-page backing file
   */
  page_map_t(buf_pool_t<delta_page_t>* pool, disk_manager_t* disk_mgr);

  /**
   * @brief Late-bind the pool and disk manager (used when constructing
   *        page_map_t before the pool is ready).
   */
  void set_pool(buf_pool_t<delta_page_t>* pool, disk_manager_t* disk_mgr);

  /**
   * @brief Insert delta records for a given CSR page.
   * @param page_no       CSR page number
   * @param delta_records Delta records to append
   */
  void insert_delta(page_no_t page_no, std::vector<delta_record_t> delta_records);

  /**
   * @brief Insert a single delta record and return its stable location.
   * @param csr_page_no  CSR page that owns the source vertex
   * @param record       Delta record to insert (next fields already set)
   * @return             {delta_page_no, record_idx} — stable location of the
   *                     newly inserted record within the delta buffer pool.
   */
  std::pair<page_no_t, uint32_t> insert_delta_and_get_loc(page_no_t csr_page_no,
                                                          delta_record_t record);

  /**
   * @brief Forwarder-aware single record insert.
   *
   * Looks up the page_map_entry_t for @p csr_page_no with a single
   * ConcurrentHashMap probe; if the entry carries a forwarder, the record is
   * appended to the chain of the new CSR page named by
   * forwarder->forward_map_[src] instead.  The first rerouted record for
   * each vertex is registered on the forwarder so the consolidation
   * finaliser can later truncate the now-merged tail.
   *
   * @param csr_page_no Source-vertex's current CSR page (as observed by the
   *                    writer under v_w_lock).
   * @param src         Source vertex of the delta (used by forwarder lookup).
   * @param record      Delta record to insert; the caller must have already
   *                    populated record.next_page_no / next_record_idx with
   *                    the current per-vertex delta head.
   * @param[out] out_rerouted Set to true when the record was routed onto a
   *                          new CSR page via the forwarder.
   * @return            Stable {delta_page_no, record_idx} of the new record.
   */
  std::pair<page_no_t, uint32_t> insert_delta_with_forward(page_no_t csr_page_no, v_id_t src,
                                                           delta_record_t record,
                                                           bool& out_rerouted);

  /**
   * @brief Install / detach the forwarder on the entry for @p csr_page_no.
   *
   * install_forwarder_for() materialises the entry if it does not yet exist
   * so concurrent writers and the consolidation worker observe the same
   * page_map_entry_t object.
   */
  void install_forwarder_for(page_no_t csr_page_no, page_delta_t* forwarder);
  page_delta_t* detach_forwarder_for(page_no_t csr_page_no);

  /**
   * @brief Claim / release the right to run one SMO task for @p csr_page_no.
   *
   * This suppresses duplicate async consolidations while a task is queued or
   * running, including the early phase before a forwarding table is installed.
   */
  bool try_begin_consolidation_for(page_no_t csr_page_no);
  void end_consolidation_for(page_no_t csr_page_no);

  /**
   * @brief Collect all delta records for a CSR page and release the chain.
   * @param page_no CSR page number
   * @return All delta records in chain order (head → tail)
   */
  std::vector<delta_record_t> fetch_deltas_with_mark(page_no_t page_no);

  /**
   * @brief Return the number of delta pages in the chain for a CSR page.
   */
  uint64_t get_delta_chain_length(page_no_t page_no);

  /**
   * @brief Return a reference to the inner page map (for iteration).
   */
  page_map_inner_t& get_inner_page_map();

  /**
   * @brief Truncate the per-vertex delta chain at the given record location.
   *
   * Used by the consolidation finaliser to disconnect the new chain from the
   * already-merged old chain.  Loads (or re-uses) the delta page via the
   * buffer pool, takes its write latch, and overwrites the record's next-link
   * with INVALID.
   *
   * @param loc Stable location of the boundary record (delta_page_no, idx).
   */
  void truncate_chain_at(delta_head_t loc);

private:
  /**
   * @brief Allocate a fresh delta page frame from the pool.
   * @return Pinned delta_page_t* with w_latch held, or nullptr on failure
   */
  delta_page_t* alloc_delta_page();
};

#endif // BW_GRAPH_BUF_PAGE_MAP_H
