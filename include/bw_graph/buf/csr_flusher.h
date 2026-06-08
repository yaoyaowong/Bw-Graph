#ifndef BW_GRAPH_BUF_CSR_FLUSHER_H
#define BW_GRAPH_BUF_CSR_FLUSHER_H

#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/storage/disk_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <thread>

/**
 * @brief Background flusher for dirty CSR pages, backed by a TBB task arena.
 *
 * Worker count semantics (mirrors smo_ctl_t):
 *   = 0 : flusher disabled. start()/stop()/flush_all()/trigger_async_flush()
 *         are no-ops, no dispatcher thread is spawned, and the controller
 *         never writes any CSR page to disk.
 *   > 0 : a dispatcher thread paces flush cycles and dispatches the actual
 *         page write-outs onto a tbb::task_arena holding exactly
 *         `worker_count` worker threads.
 */
class csr_flusher_t {
public:
  /**
   * @brief Construct a CSR flusher.
   * @param pool         CSR buffer pool to scan for dirty pages.
   * @param disk_mgr     Disk manager used to persist CSR pages.
   * @param interval     Background scan interval.
   * @param worker_count Number of TBB worker threads to dispatch flush work
   *                     onto. 0 disables the flusher entirely.
   * @return None.
   */
  explicit csr_flusher_t(buf_pool_t<csr_page_t>* pool, disk_manager_t* disk_mgr,
                         std::chrono::milliseconds interval = std::chrono::milliseconds(500),
                         size_t worker_count = 1);

  /**
   * @brief Destroy the flusher and stop background work if needed.
   * @param None.
   * @return None.
   */
  ~csr_flusher_t();

  /**
   * @brief Start the dispatcher thread. No-op when disabled or already on.
   * @param None.
   * @return None.
   */
  void start();

  /**
   * @brief Stop the dispatcher and run a final synchronous flush.
   * @param None.
   * @return None.
   */
  void stop();

  /**
   * @brief Flush all dirty CSR pages synchronously.
   * @param None.
   * @return Number of pages written in this call.
   */
  size_t flush_all();

  /**
   * @brief Wake the dispatcher early for an asynchronous flush.
   * @param None.
   * @return None.
   */
  void trigger_async_flush();

  /**
   * @brief Get cumulative flushed page count since construction.
   * @param None.
   * @return Total number of pages flushed.
   */
  uint64_t total_flushed() const { return total_flushed_.load(std::memory_order_relaxed); }

  /**
   * @brief Check whether the dispatcher thread is running.
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
  /* CSR buffer pool to scan for dirty pages. */
  buf_pool_t<csr_page_t>* pool_;

  /* Disk manager used to persist CSR pages. */
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

  /* Mutex protecting wake and stop coordination. */
  std::mutex cv_mutex_;

  /* Condition variable used for timed wait and wake-up. */
  std::condition_variable cv_;

  /* One-shot wake flag set by trigger_async_flush(). */
  bool wake_requested_{false};

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

#endif // BW_GRAPH_BUF_CSR_FLUSHER_H
