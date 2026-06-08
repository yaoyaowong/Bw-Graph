#include "bw_graph/buf/delta_flusher.h"
#include "bw_graph/common/logger.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

// Constructor and destructor.
delta_flusher_t::delta_flusher_t(buf_pool_t<delta_page_t>* pool, disk_manager_t* disk_mgr,
                                 std::chrono::milliseconds interval, size_t worker_count)
    : pool_(pool), disk_mgr_(disk_mgr), interval_(interval), worker_count_(worker_count) {
  // Disabled flusher: skip arena/task_group allocation entirely.
  if (worker_count_ == 0) {
    return;
  }
  flush_arena_ = std::make_unique<tbb::task_arena>(static_cast<int>(worker_count_), 0);
  flush_arena_->execute([this] { task_group_ = std::make_unique<tbb::task_group>(); });
}

// Destroy delta_flusher_t.
delta_flusher_t::~delta_flusher_t() {
  if (running_.load(std::memory_order_acquire)) {
    stop();
  }
}

// Start and stop.
void delta_flusher_t::start() {
  if (worker_count_ == 0) {
    return; // Flusher disabled.
  }
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return; // Already running.
  }

  stop_.store(false, std::memory_order_release);
  flush_thread_ = std::thread([this] { flush_loop(); });
}

// Stop stop.
void delta_flusher_t::stop() {
  if (worker_count_ == 0) {
    return;
  }
  // Signal the dispatcher thread to exit.
  {
    std::lock_guard<std::mutex> lk(cv_mutex_);
    stop_.store(true, std::memory_order_release);
  }
  cv_.notify_all();

  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }

  running_.store(false, std::memory_order_release);

  // One final synchronous flush to drain any remaining dirty pages.
  flush_all();
}

// Background flush loop.
void delta_flusher_t::flush_loop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lk(cv_mutex_);
      // Sleep for interval_ or wake up early when stop_ is set.
      cv_.wait_for(lk, interval_, [this] { return stop_.load(std::memory_order_acquire); });
    }

    if (stop_.load(std::memory_order_acquire)) {
      break;
    }

    flush_dirty_pages();
  }
}

// Public synchronous flush.
size_t delta_flusher_t::flush_all() {
  if (worker_count_ == 0) {
    return 0;
  }
  return flush_dirty_pages();
}

// Scan the pool and flush dirty pages in parallel through the arena.
size_t delta_flusher_t::flush_dirty_pages() {
  if (pool_ == nullptr || disk_mgr_ == nullptr || worker_count_ == 0) {
    return 0;
  }

  // Snapshot the dirty frame pointers under traverse_page_map() so the actual
  // disk I/O happens outside the page-map iterator and can be parallelised.
  std::vector<delta_page_t*> dirty_pages;
  pool_->traverse_page_map([&](page_no_t /*pno*/, delta_page_t* page) -> bool {
    if (page->is_dirty()) {
      dirty_pages.push_back(page);
    }
    return true;
  });

  if (dirty_pages.empty()) {
    return 0;
  }

  BW_GRAPH_LOG_INFO("[DELTA_FLUSH] start pages=%zu", dirty_pages.size());
  auto _df_t0_ = std::chrono::steady_clock::now();

  std::atomic<size_t> count{0};

  flush_arena_->execute([&] {
    task_group_->run_and_wait([&] {
      // Each iteration is enqueued as an independent task; tbb's arena keeps
      // the concurrency bounded by worker_count_.
      for (delta_page_t* page : dirty_pages) {
        task_group_->run([page, &count, this] {
          try {
            if (page->flush_to_disk(disk_mgr_)) {
              count.fetch_add(1, std::memory_order_relaxed);
            }
          } catch (const std::exception& e) {
            std::cerr << "[delta_flusher] IO error flushing page " << page->get_page_no() << ": "
                      << e.what() << std::endl;
          } catch (...) {
            std::cerr << "[delta_flusher] Unknown error flushing page " << page->get_page_no()
                      << std::endl;
          }
        });
      }
    });
  });

  size_t written = count.load(std::memory_order_relaxed);
  total_flushed_.fetch_add(written, std::memory_order_relaxed);
  {
    auto _df_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - _df_t0_).count();
    BW_GRAPH_LOG_INFO("[DELTA_FLUSH] done pages=%zu elapsed_ms=%lld",
        written, (long long)_df_ms_);
  }
  return written;
}
