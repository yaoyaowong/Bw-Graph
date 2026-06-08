#ifndef BW_GRAPH_BUF_DELTA_FLUSHER_H
#define BW_GRAPH_BUF_DELTA_FLUSHER_H

#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <thread>

/**
 * @brief Background delta-page flusher backed by a TBB task arena.
 *
 * Worker count semantics (mirrors smo_ctl_t):
 *   = 0 : flusher disabled. start()/stop()/flush_all() are no-ops, no
 *         dispatcher thread is spawned, and the controller never writes any
 *         delta page to disk.
 *   > 0 : a dispatcher thread wakes up every `interval` and submits the
 *         dirty-page scan to a dedicated tbb::task_arena holding exactly
 *         `worker_count` worker threads.
 */
class delta_flusher_t {
public:
  /**
   * @brief Construct the flusher (does not start the background thread yet).
   * @param pool         Delta-page buffer pool to scan for dirty pages.
   * @param disk_mgr     Delta-page disk manager used for write_page().
   * @param interval     Period between automatic flush cycles.
   * @param worker_count Number of TBB worker threads to dispatch flush work
   *                     onto. 0 disables the flusher entirely.
   * @return None.
   */
  explicit delta_flusher_t(buf_pool_t<delta_page_t>* pool, disk_manager_t* disk_mgr,
                           std::chrono::milliseconds interval = std::chrono::milliseconds(200),
                           size_t worker_count = 1);

  /**
   * @brief Destructor stops the dispatcher if still running.
   * @param None.
   * @return None.
   */
  ~delta_flusher_t();

  delta_flusher_t(const delta_flusher_t&) = delete;
  delta_flusher_t& operator=(const delta_flusher_t&) = delete;

  /**
   * @brief Start the dispatcher thread. No-op when worker_count == 0 or
   *        already running.
   * @param None.
   * @return None.
   */
  void start();

  /**
   * @brief Stop the dispatcher thread and run a final synchronous flush.
   *        No-op when the flusher is disabled.
   * @param None.
   * @return None.
   */
  void stop();

  /**
   * @brief Synchronously flush every dirty delta page once. Returns 0 when
   *        the flusher is disabled.
   * @param None.
   * @return Number of pages actually written in this call.
   */
  size_t flush_all();

  /**
   * @brief Total pages flushed since construction.
   * @param None.
   * @return Cumulative flushed-page counter.
   */
  uint64_t total_flushed() const { return total_flushed_.load(std::memory_order_relaxed); }

  /**
   * @brief Whether the dispatcher thread is currently running.
   * @param None.
   * @return true if running, false otherwise.
   */
  bool is_running() const { return running_.load(std::memory_order_acquire); }

  /**
   * @brief Whether the flusher is enabled (worker_count > 0).
   * @param None.
   * @return true if at least one worker is configured.
   */
  bool is_enabled() const { return worker_count_ > 0; }

  /**
   * @brief Configured worker thread count.
   * @param None.
   * @return Number of TBB workers in the flush arena.
   */
  size_t get_worker_count() const { return worker_count_; }

private:
  /* Delta buffer pool to scan for dirty pages. */
  buf_pool_t<delta_page_t>* pool_;

  /* Disk manager used to persist delta pages. */
  disk_manager_t* disk_mgr_;

  /* Period between two automatic flush cycles. */
  std::chrono::milliseconds interval_;

  /* Number of TBB workers used for parallel page write-out. */
  size_t worker_count_;

  /* Dispatcher thread that paces flush cycles. */
  std::thread flush_thread_;

  /* Stop flag observed by the dispatcher. */
  std::atomic<bool> stop_{false};

  /* True while the dispatcher thread is alive. */
  std::atomic<bool> running_{false};

  /* Mutex protecting condition-variable state. */
  std::mutex cv_mutex_;

  /* Condition variable used for interval sleep and stop wake-up. */
  std::condition_variable cv_;

  /* Total number of pages flushed by this flusher. */
  std::atomic<uint64_t> total_flushed_{0};

  /* TBB arena that limits flush concurrency to worker_count_. */
  std::unique_ptr<tbb::task_arena> flush_arena_;

  /* Task group used to dispatch and join parallel write tasks. */
  std::unique_ptr<tbb::task_group> task_group_;

  /**
   * @brief Dispatcher main loop.
   * @param None.
   * @return None.
   */
  void flush_loop();

  /**
   * @brief One flush cycle: scan the pool, dispatch dirty-page writes onto
   *        the arena, and join the resulting tasks.
   * @param None.
   * @return Number of pages written this cycle.
   */
  size_t flush_dirty_pages();
};

#endif // BW_GRAPH_BUF_DELTA_FLUSHER_H
