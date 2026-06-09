#ifndef BW_GRAPH_PART_SMO_H
#define BW_GRAPH_PART_SMO_H

#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/buf/sketch.h"
#include "bw_graph/common/type.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/io/io_index.h"
#include "bw_graph/storage/disk_manager.h"

#if defined(BW_GRAPH_ENABLE_TRANSACTION)
#include "bw_graph/db/txn.h"
#endif // BW_GRAPH_ENABLE_TRANSACTION

#include <atomic>
#include <cstddef>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <future>
#include <mutex>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <utility>
#include <vector>

class giant_vertex_db_t;
class csr_flusher_t;
struct page_delta_t;

/**
 * @brief Consolidation controller implementing CoW (Copy-on-Write) SMO.
 */
struct smo_ctl_t {
private:
  /* Task group executing SMO jobs. */
  std::unique_ptr<tbb::task_group> task_group_;
  /* Dedicated task arena limiting SMO concurrency. */
  std::unique_ptr<tbb::task_arena> smo_arena_;
  /* Maximum number of SMO worker threads. */
  size_t max_threads_;
  /* Number of queued or running consolidations. */
  std::atomic<size_t> pending_consolidations_{0};

  /** Defer vertex-version publication until explicitly requested. */
  bool manual_version_switch_{false};

  /** Optional CSR flusher to trigger after each successful SMO. */
  csr_flusher_t* csr_flusher_{nullptr};

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  /** Optional transaction manager for watermark-aware consolidation. */
  txn_manager_t* txn_manager_{nullptr};
#endif // BW_GRAPH_ENABLE_TRANSACTION

  /** Pointer to the database's index page map (null = TAT disabled). */
  folly::F14FastMap<page_no_t, index_page_t*>* index_page_map_{nullptr};

  /** Pointer to the database's root index page pointer. */
  index_page_t** root_index_page_{nullptr};

  /** Mutex serialising all index-page COW updates. */
  std::mutex index_mutex_;

  /** Atomic counter for allocating new index page numbers (decrements). */
  std::atomic<page_no_t> next_index_page_no_{0};

  /** CSR pages that were made unreachable but could not be recycled yet. */
  std::vector<page_no_t> pending_retired_pages_;

  /** Protects pending_retired_pages_. */
  std::mutex retired_pages_mutex_;

  /** Vertex -> prepared immutable version for one SMO publication. */
  using vertex_location_map_t = folly::F14FastMap<v_id_t, prepared_vertex_version_t>;

  struct pending_version_switch_t {
    vertex_location_map_t new_locations;
    std::shared_ptr<page_delta_t> forwarder;
    folly::F14FastSet<v_id_t> giant_vertices;
    page_map_t* page_map{nullptr};
    disk_manager_t* disk_manager{nullptr};
    buf_pool_t<csr_page_t>* buf_pool{nullptr};
    vertex_index_t* vertex_index{nullptr};
    page_no_t old_page_no{0};
    page_no_t old_parent_no{0};
    bool need_split{false};
    page_no_t single_new_csr_no{0};
    std::vector<page_no_t> new_page_nos;
    adj_map_t merged_graph;
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    bool set_page_snapshot_ts{false};
    timestamp_t page_snapshot_ts{INVALID_TS};
#endif
  };

  /** Consolidations built but not yet published in manual mode. */
  std::vector<pending_version_switch_t> pending_version_switches_;

  /** Protects pending_version_switches_. */
  std::mutex pending_version_switches_mutex_;

  /**
   * @brief Submit a void task into the SMO runtime.
   * @param func Task body to execute.
   * @return Future that becomes ready when task finishes.
   */
  std::future<void> submit_void_task(std::function<void()> func);

  // Private helper methods.

  /**
   * @brief Snapshot CSR page + apply deltas → merged adj_map_t.
   *
   * Acquires/releases r_latch internally. All neighbor data is copied into
   * std::vector so callers are safe to use the result after r_unlatch.
   * @param page Base CSR page to merge from.
   * @param deltas Delta records to apply.
   * @return Merged adjacency map.
   */
  adj_map_t build_merged_graph(csr_page_t* page, const std::vector<delta_record_t>& deltas,
                               vertex_index_t* vertex_index = nullptr,
                               giant_vertex_db_t* giant_db = nullptr);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  /**
   * @brief Txn-aware variant: also builds a parallel edge_ts_map_t where each
   *        entry's timestamp vector is aligned with the sorted neighbor list.
   *
   * Old CSR edges inherit per-edge timestamps already stored on the page
   * (INVALID_TS = always visible for initial/legacy edges).
   * Delta INSERT edges carry their commit_ts.
   *
   * @param page   Base CSR page (r_latch acquired/released internally).
   * @param deltas Delta records (only those with commit_ts < watermark).
   * @return Pair of merged adjacency map and parallel timestamp map.
   */
  std::pair<adj_map_t, edge_ts_map_t>
  build_merged_graph_with_ts(csr_page_t* page, const std::vector<delta_record_t>& deltas,
                             vertex_index_t* vertex_index = nullptr,
                             giant_vertex_db_t* giant_db = nullptr);
#endif

  /**
   * @brief Conservatively estimate bytes required to store the graph in a
   *        single CSR page (matches the csr_page_t constructor layout).
   * @param graph Merged adjacency map.
   * @return Estimated byte size.
   */
  static size_t estimate_page_bytes(const adj_map_t& graph);

  /**
   * @brief Try to recycle one old CSR page after CoW publication.
   */
  bool try_recycle_old_page(page_no_t old_page_no, disk_manager_t* disk_manager,
                            buf_pool_t<csr_page_t>* buf_pool);

  /**
   * @brief Queue an unreachable CSR page for later recycle.
   */
  void defer_recycle_old_page(page_no_t old_page_no);

  /**
   * @brief Retry all deferred CSR page recycle candidates.
   */
  void retry_deferred_recycles(disk_manager_t* disk_manager, buf_pool_t<csr_page_t>* buf_pool);

  /**
   * @brief No-split CoW: build ONE new CSR page for merged_graph and copy it
   *        into the buffer pool.  Returns the new page_no and the vertex ->
   *        (new_page_no, offset) map but does NOT update the vertex index or
   *        per-vertex delta heads; finalize_consolidation() does that.
   *
   * A fresh page_no is always allocated (old_page_no is never reused).
   * @param old_page_no Old page number before consolidation.
   * @param merged_graph Merged graph content to materialize.
   * @param disk_manager Disk allocator and writer.
   * @param buf_pool CSR buffer pool.
   * @param vertex_update_sketch Slot reservation sketch.
   * @param[out] new_locations vertex -> (new_page_no, offset) for every
   *             vertex placed on the new page.
   * @return New page number allocated by this operation.
   */
  page_no_t do_no_split(page_no_t old_page_no, adj_map_t& merged_graph,
                        disk_manager_t* disk_manager, buf_pool_t<csr_page_t>* buf_pool,
                        vertex_update_sketch_t* vertex_update_sketch, vertex_index_t* vertex_index,
                        vertex_location_map_t& new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                        ,
                        const edge_ts_map_t* ts_map = nullptr
#endif
  );

  /**
   * @brief Split CoW: partition merged_graph, build N new CSR pages (fresh
   *        page_nos), copy them into the pool.  Like do_no_split this does
   *        NOT touch the vertex index / delta heads.
   *
   * @param old_page_no Old page number before consolidation.
   * @param merged_graph Merged graph content to materialize.
   * @param disk_manager Disk allocator and writer.
   * @param buf_pool CSR buffer pool.
   * @param vertex_update_sketch Slot reservation sketch.
   * @param[out] new_locations vertex -> (new_page_no, offset) for every
   *             vertex placed on any of the new pages.
   * @return Vector of all new CSR page numbers created.
   */
  std::vector<page_no_t> do_split(page_no_t old_page_no, adj_map_t& merged_graph,
                                  disk_manager_t* disk_manager, buf_pool_t<csr_page_t>* buf_pool,
                                  vertex_update_sketch_t* vertex_update_sketch,
                                  vertex_index_t* vertex_index,
                                  vertex_location_map_t& new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                                  ,
                                  const edge_ts_map_t* ts_map = nullptr
#endif
  );

  /**
   * @brief Atomically commit the consolidation: for every vertex in
   *        new_locations, take its write latch, fix the next-link of the
   *        boundary delta record published by writers via the forwarding
   *        table (if any), then publish the new (page_no, offset) and the
   *        appropriate delta head.
   *
   * Must be called AFTER do_no_split / do_split and AFTER a page_delta_t has
   * been installed on old_page so concurrent writers reroute their deltas.
   *
   * @param new_locations vertex -> (new_page_no, offset).
   * @param page_map      Delta-page map (used to look up boundary records).
   * @param vertex_index  Vertex location index to update.
   * @param forwarder     Forwarding table currently installed on old_page;
   *                      may be nullptr when consolidation runs in legacy mode.
   */
  void finalize_consolidation(vertex_location_map_t& new_locations, page_map_t* page_map,
                              vertex_index_t* vertex_index, page_delta_t* forwarder,
                              const folly::F14FastSet<v_id_t>& giant_vertices,
                              const std::shared_ptr<vertex_version_reclaim_batch_t>& reclaim_batch);

  /**
   * @brief Publish one staged consolidation and recycle its old CSR page.
   */
  void publish_version_switch(pending_version_switch_t& pending);

  /**
   * @brief COW update: replace old_csr_page_no with new_csr_page_no in parent
   *        index page. Called after do_no_split.
   */
  void update_index_no_split(page_no_t old_csr_page_no, page_no_t new_csr_page_no,
                             page_no_t old_parent_no);

  /**
   * @brief COW update: replace old CSR page with multiple new CSR pages in
   *        parent index page. May trigger do_index_split if index overflows.
   */
  void update_index_split(page_no_t old_csr_page_no, page_no_t old_parent_no,
                          const std::vector<page_no_t>& new_csr_page_nos,
                          const adj_map_t& merged_graph, vertex_index_t* vertex_index);

  /**
   * @brief Split an overfull index page into 2+ pages using COW + partition.
   *        Updates index_page_map and sets parent pointers on new pages.
   *        MUST be called with index_mutex_ already held.
   * @param old_idx_page_no  Page number of the index page being replaced.
   * @param new_adj          Updated block adjacency map for the split page.
   */
  void do_index_split(page_no_t old_idx_page_no, block_adj_map_t& new_adj);

public:
  // Test hooks: the consolidation pipeline below is exposed so unit tests can
  // drive the forwarder/finalize handshake step by step. Regular call sites
  // should use consolidate_pages / consolidate_pages_async instead.
  using vertex_location_map_pub_t = vertex_location_map_t;

  /** Phase B (no-split): build one new CSR page and emit new_locations. */
  page_no_t test_do_no_split(page_no_t old_page_no, adj_map_t& merged_graph,
                             disk_manager_t* disk_manager, buf_pool_t<csr_page_t>* buf_pool,
                             vertex_update_sketch_t* vertex_update_sketch,
                             vertex_index_t* vertex_index, vertex_location_map_t& new_locations) {
    return do_no_split(old_page_no, merged_graph, disk_manager, buf_pool, vertex_update_sketch,
                       vertex_index, new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                       ,
                       nullptr
#endif
    );
  }

  /** Phase B (split): same as test_do_no_split but emits multiple pages. */
  std::vector<page_no_t> test_do_split(page_no_t old_page_no, adj_map_t& merged_graph,
                                       disk_manager_t* disk_manager,
                                       buf_pool_t<csr_page_t>* buf_pool,
                                       vertex_update_sketch_t* vertex_update_sketch,
                                       vertex_index_t* vertex_index,
                                       vertex_location_map_t& new_locations) {
    return do_split(old_page_no, merged_graph, disk_manager, buf_pool, vertex_update_sketch,
                    vertex_index, new_locations
#ifdef BW_GRAPH_ENABLE_TRANSACTION
                    ,
                    nullptr
#endif
    );
  }

  /** Phase C: atomic finalize. */
  void test_finalize_consolidation(vertex_location_map_t& new_locations,
                                   page_map_t* page_map, vertex_index_t* vertex_index,
                                   page_delta_t* forwarder) {
    folly::F14FastSet<v_id_t> giant_vertices;
    finalize_consolidation(new_locations, page_map, vertex_index, forwarder, giant_vertices,
                           nullptr);
  }

  /**
   * @brief Construct SMO controller.
   * @param max_threads Maximum SMO worker count.
   * @return None.
   */
  explicit smo_ctl_t(size_t max_threads = 4, bool manual_version_switch = false);

  /**
   * @brief Destroy SMO controller and wait all pending tasks.
   * @param None.
   * @return None.
   */
  ~smo_ctl_t();

  /**
   * @brief Register CSR flusher used by post-SMO async flush trigger.
   * @param f CSR flusher instance, nullable.
   * @return None.
   */
  void set_csr_flusher(csr_flusher_t* f) { csr_flusher_ = f; }

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  void set_txn_manager(txn_manager_t* tm) { txn_manager_ = tm; }
#endif // BW_GRAPH_ENABLE_TRANSACTION

  /**
   * @brief Wire up the database's index-page map so SMO can maintain the TAT.
   * @param ipm             Pointer to bw_graph_db_t::index_page_map.
   * @param root_idx_page   Pointer to bw_graph_db_t::root_index_page.
   * @param start_page_no   First free index page_no
   *                        (from tat_build_result_t::next_index_page_no).
   */
  void set_index_page_map(folly::F14FastMap<page_no_t, index_page_t*>* ipm,
                          index_page_t** root_idx_page, page_no_t start_page_no) {
    index_page_map_ = ipm;
    root_index_page_ = root_idx_page;
    next_index_page_no_.store(start_page_no, std::memory_order_relaxed);
  }

  /**
   * @brief Run the five-phase CoW consolidation for old_page synchronously.
   * @param old_page Source CSR page.
   * @param page_map Delta page map table.
   * @param disk_manager CSR disk manager.
   * @param buf_pool CSR buffer pool.
   * @param vertex_index Vertex index table.
   * @param giant_db Giant-vertex store.
   * @param vertex_update_sketch Slot reservation sketch.
   * @param requested_threshold Minimum chain length the caller requires
   *                            before doing the work; defaults to the
   *                            yaml baseline so legacy call sites behave
   *                            unchanged.  Workload-driven callers may
   *                            pass a smaller value (down to 1) so the
   *                            adaptive trigger can fire on read-heavy
   *                            vertices without being short-circuited
   *                            here.
   * @return None.
   */
  void consolidate_pages(csr_page_t* old_page, page_map_t* page_map, disk_manager_t* disk_manager,
                         buf_pool_t<csr_page_t>* buf_pool, vertex_index_t* vertex_index,
                         giant_vertex_db_t* giant_db, vertex_update_sketch_t* vertex_update_sketch,
                         uint64_t requested_threshold = 0, bool old_page_pinned = false,
                         bool consolidation_claimed = false);

  /**
   * @brief Submit consolidation as a background TBB task.
   * @param old_page Source CSR page.
   * @param page_map Delta page map table.
   * @param disk_manager CSR disk manager.
   * @param buf_pool CSR buffer pool.
   * @param vertex_index Vertex index table.
   * @param giant_db Giant-vertex store.
   * @param vertex_update_sketch Slot reservation sketch.
   * @param requested_threshold Same semantics as the sync variant.
   * @return Future that becomes ready when consolidation ends.
   */
  std::future<void> consolidate_pages_async(csr_page_t* old_page, page_map_t* page_map,
                                            disk_manager_t* disk_manager,
                                            buf_pool_t<csr_page_t>* buf_pool,
                                            vertex_index_t* vertex_index,
                                            giant_vertex_db_t* giant_db,
                                            vertex_update_sketch_t* vertex_update_sketch,
                                            uint64_t requested_threshold = 0,
                                            bool old_page_pinned = false,
                                            bool consolidation_claimed = false);

  /**
   * @brief Block until all pending consolidations finish.
   * @param None.
   * @return None.
   */
  void wait_all_consolidations();

  /**
   * @brief Publish all replacement pages staged by manual-version SMO.
   *
   * Callers must stop graph writers before invoking this method.
   */
  void publish_pending_versions();

  /** Return whether this controller defers vertex-version publication. */
  bool manual_version_switch_enabled() const { return manual_version_switch_; }

  /**
   * @brief Get current pending consolidation count.
   * @param None.
   * @return Number of pending consolidations.
   */
  size_t get_pending_consolidation_count() const;

  /**
   * @brief Check whether there is any pending consolidation.
   * @param None.
   * @return true if pending count is non-zero, false otherwise.
   */
  bool has_pending_consolidations() const;

  /**
   * @brief Report whether consolidation is enabled.
   *
   * Controlled by yaml field `thread.consolidation_workers`: a value of 0
   * disables the SMO worker arena and turns every consolidation entry point
   * into a no-op.
   * @param None.
   * @return true if at least one SMO worker is configured.
   */
  bool is_enabled() const { return max_threads_ > 0; }

  /**
   * @brief Return the configured SMO worker count.
   * @param None.
   * @return Maximum number of consolidation worker threads.
   */
  size_t get_worker_count() const { return max_threads_; }
};

#endif // BW_GRAPH_PART_SMO_H
