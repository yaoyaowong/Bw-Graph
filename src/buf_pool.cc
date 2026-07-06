#include "bw_graph/buf/buf_pool.h"

#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

// Initialize a buffer chunk.
template <typename PageType>
// Handle buf chunk init.
buf_chunk_t<PageType>* buf_chunk_t<PageType>::buf_chunk_init(size_t page_count, size_t page_size) {
  buf_chunk_t<PageType>* chunk = new buf_chunk_t<PageType>();
  chunk->size = page_count;
  chunk->valid.resize(page_count, false);
  chunk->pages = new PageType[page_count];
  chunk->frames = new char[page_count * page_size];
  for (size_t i = 0; i < page_count; ++i) {
    chunk->pages[i].set_data(chunk->frames + i * page_size);
    chunk->pages[i].set_page_state(PAGE_FREE);
    chunk->pages[i].set_next(nullptr);
    chunk->pages[i].set_prev(nullptr);
    chunk->pages[i].set_referenced(false);
  }
  return chunk;
}

// Initialize a buffer pool.
template <typename PageType>
// Handle buf pool init.
buf_pool_t<PageType>* buf_pool_t<PageType>::buf_pool_init(size_t chunk_count,
                                                          size_t page_count_per_chunk,
                                                          size_t page_size) {
  buf_pool_t<PageType>* pool = new buf_pool_t<PageType>();
  pool->lru_head_ = nullptr;
  pool->lru_tail_ = nullptr;
  pool->clock_hand_ = nullptr;
  pool->chunk_num = chunk_count;
  pool->page_size_ = page_size;

  if (chunk_count != 0 && page_count_per_chunk > std::numeric_limits<size_t>::max() / chunk_count) {
    delete pool;
    throw std::overflow_error("buf_pool: resident page count overflow");
  }
  const size_t resident_page_count = chunk_count * page_count_per_chunk;
  pool->resident_page_count_ = resident_page_count;
  const size_t page_map_capacity = std::max<size_t>(8, resident_page_count);
  pool->page_map = folly::ConcurrentHashMap<page_no_t, PageType*>(page_map_capacity);

  pool->chunks.resize(chunk_count);
  for (size_t i = 0; i < chunk_count; ++i) {
    pool->chunks[i] = buf_chunk_t<PageType>::buf_chunk_init(page_count_per_chunk, page_size);
  }
  pool->free_list_init();
  return pool;
}

// Try to latch a cached page and verify the frame was not evicted/reused.
template <typename PageType>
PageType* buf_pool_t<PageType>::try_latch_cached_page(page_no_t page_no, bool write_latch) {
  auto it = page_map.find(page_no);
  if (it != page_map.end()) {
    PageType* page = it->second;
    if (write_latch) {
      page->w_latch();
    } else {
      page->r_latch();
    }
    auto verify = page_map.find(page_no);
    if (verify != page_map.end() && verify->second == page && page->get_page_no() == page_no) {
      page->set_referenced(true);
      return page;
    }
    if (write_latch) {
      page->w_unlatch();
    } else {
      page->r_unlatch();
    }
  }
  return nullptr;
}

// Register this thread as the cold-page loader, or wait for the active loader.
template <typename PageType>
bool buf_pool_t<PageType>::register_page_load(page_no_t page_no) {
  std::unique_lock<std::mutex> lk(loading_mutex_);
  auto inserted = loading_pages_.insert(page_no);
  if (inserted.second) {
    return true;
  }

  duplicate_miss_waits_.fetch_add(1, std::memory_order_relaxed);
  loading_cv_.wait(lk, [&] { return loading_pages_.find(page_no) == loading_pages_.end(); });
  return false;
}

// Finish a cold-page load and wake waiters.
template <typename PageType>
void buf_pool_t<PageType>::finish_page_load(page_no_t page_no) {
  {
    std::lock_guard<std::mutex> lk(loading_mutex_);
    loading_pages_.erase(page_no);
  }
  loading_cv_.notify_all();
}

// Claim a free frame or evict a victim. Dirty write-back runs outside LRU/free locks.
template <typename PageType>
PageType* buf_pool_t<PageType>::claim_frame_for_reuse(disk_manager_t* disk_manager) {
  PageType* free_page = nullptr;
  bool victim_locked = false;
  bool write_dirty_victim = false;
  page_no_t victim_page_no = bw_graph::BW_GRAPH_DEFAULT_PAGE_NO;

  this->lru_latch_.w_lock();
  this->free_latch_.w_lock();
  if (free_head_ == nullptr) {
    // Evict one page when no free frame exists.
    free_page = find_victim_page();
    if (free_page == nullptr) {
      this->free_latch_.w_unlock();
      this->lru_latch_.w_unlock();
      throw std::runtime_error("buf_pool: all pages are pinned, cannot allocate new page");
    }
    victim_locked = true;
    victim_page_no = free_page->get_page_no();
    if (free_page->is_dirty()) {
      write_dirty_victim = true;
      dirty_writebacks_.fetch_add(1, std::memory_order_relaxed);
    }
    evictions_.fetch_add(1, std::memory_order_relaxed);
    remove_from_lru_list(free_page);
    page_map.erase(victim_page_no);
    free_page->disable_parse();
  } else {
    free_page = free_head_;
    remove_from_free_list(free_page);
  }

  add_to_lru_head(free_page);
  if (!victim_locked) {
    free_page->w_latch();
  }
  this->free_latch_.w_unlock();
  this->lru_latch_.w_unlock();

  if (write_dirty_victim) {
    disk_manager->write_page(victim_page_no, free_page->get_data());
    free_page->mark_clean();
  }

  return free_page;
}

// Shared page read implementation.
template <typename PageType>
PageType* buf_pool_t<PageType>::read_page_internal(page_no_t page_no, disk_manager_t* disk_manager,
                                                   bool write_latch) {
  while (true) {
    PageType* cached_page = try_latch_cached_page(page_no, write_latch);
    if (cached_page != nullptr) {
      cache_hits_.fetch_add(1, std::memory_order_relaxed);
      return cached_page;
    }

    if (register_page_load(page_no)) {
      break;
    }
  }

  cache_misses_.fetch_add(1, std::memory_order_relaxed);

  PageType* free_page = nullptr;
  try {
    // A page may have been installed by a non-read path while this thread was
    // becoming the loader.
    PageType* cached_page = try_latch_cached_page(page_no, write_latch);
    if (cached_page != nullptr) {
      cache_hits_.fetch_add(1, std::memory_order_relaxed);
      finish_page_load(page_no);
      return cached_page;
    }

    free_page = claim_frame_for_reuse(disk_manager);

    // Load page data into the selected frame.
    disk_manager->read_page(page_no, free_page->get_data());
    free_page->set_page_no(page_no);
    free_page->disable_parse();
    free_page->self_parse();
    free_page->set_page_state(PAGE_IN_LRU);
    free_page->set_referenced(true);

    page_map.insert(page_no, free_page);
    finish_page_load(page_no);
  } catch (...) {
    finish_page_load(page_no);
    if (free_page != nullptr) {
      free_page->w_unlatch();
    }
    throw;
  }

  if (write_latch) {
    return free_page;
  }
  free_page->w_to_r_latch();
  return free_page;
}

// Read a page and return it with a read latch.
template <typename PageType>
// Handle buf page read.
PageType* buf_pool_t<PageType>::buf_page_read(page_no_t page_no, disk_manager_t* disk_manager) {
  return read_page_internal(page_no, disk_manager, false);
}

// Read a page and return it for write access.
template <typename PageType>
// Handle buf page read for slot write.
PageType* buf_pool_t<PageType>::buf_page_read_for_slot_write(page_no_t page_no,
                                                             disk_manager_t* disk_manager) {
  return read_page_internal(page_no, disk_manager, true);
}

template <typename PageType>
bool buf_pool_t<PageType>::is_page_cached_or_loading(page_no_t page_no) {
  if (page_map.find(page_no) != page_map.end()) {
    return true;
  }
  std::lock_guard<std::mutex> lk(loading_mutex_);
  return loading_pages_.find(page_no) != loading_pages_.end();
}

template <typename PageType>
void buf_pool_t<PageType>::ensure_prefetch_workers() {
  const uint64_t configured_workers = bw_graph::BW_BUFFER_PREFETCH_WORKER_COUNT;
  if (configured_workers == 0) {
    return;
  }

  std::lock_guard<std::mutex> lk(prefetch_mutex_);
  if (!prefetch_workers_.empty() || prefetch_stop_) {
    return;
  }

  const unsigned int hardware_threads = std::thread::hardware_concurrency();
  const size_t worker_count =
      std::max<size_t>(1, std::min<size_t>(configured_workers,
                                           hardware_threads == 0 ? 4 : hardware_threads));
  const size_t resident_bound = std::max<size_t>(1, resident_page_count_);
  prefetch_queue_capacity_ =
      std::max<size_t>(worker_count * 4, std::min<size_t>(resident_bound, 64));
  prefetch_workers_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    prefetch_workers_.emplace_back([this] { prefetch_worker_loop(); });
  }
}

template <typename PageType>
void buf_pool_t<PageType>::stop_prefetch_workers() {
  {
    std::lock_guard<std::mutex> lk(prefetch_mutex_);
    prefetch_stop_ = true;
    if (prefetch_workers_.empty()) {
      prefetch_queue_.clear();
      queued_prefetch_pages_.clear();
      return;
    }
  }
  prefetch_cv_.notify_all();
  for (std::thread& worker : prefetch_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  prefetch_workers_.clear();
}

template <typename PageType>
void buf_pool_t<PageType>::prefetch_worker_loop() {
  while (true) {
    prefetch_job_t job{};
    {
      std::unique_lock<std::mutex> lk(prefetch_mutex_);
      prefetch_cv_.wait(lk, [&] { return prefetch_stop_ || !prefetch_queue_.empty(); });
      if (prefetch_stop_ && prefetch_queue_.empty()) {
        return;
      }
      job = prefetch_queue_.front();
      prefetch_queue_.pop_front();
      ++prefetch_active_count_;
    }

    prefetch_page_sync(job.page_no, job.disk_manager);

    {
      std::lock_guard<std::mutex> lk(prefetch_mutex_);
      queued_prefetch_pages_.erase(job.page_no);
      --prefetch_active_count_;
      if (prefetch_queue_.empty() && prefetch_active_count_ == 0) {
        prefetch_idle_cv_.notify_all();
      }
    }
  }
}

template <typename PageType>
void buf_pool_t<PageType>::prefetch_page_sync(page_no_t page_no, disk_manager_t* disk_manager) {
  try {
    PageType* page = buf_page_read(page_no, disk_manager);
    page->r_unlatch();
    prefetch_completed_.fetch_add(1, std::memory_order_relaxed);
  } catch (...) {
    // Prefetch is only a performance hint. Demand reads preserve correctness.
  }
}

// Best-effort prefetch: warm the page and release it immediately.
template <typename PageType>
void buf_pool_t<PageType>::buf_page_prefetch(page_no_t page_no, disk_manager_t* disk_manager) {
  prefetches_.fetch_add(1, std::memory_order_relaxed);
  if (disk_manager == nullptr) {
    prefetch_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (is_page_cached_or_loading(page_no)) {
    prefetch_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (bw_graph::BW_BUFFER_PREFETCH_WORKER_COUNT == 0) {
    prefetch_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  ensure_prefetch_workers();
  {
    std::lock_guard<std::mutex> lk(prefetch_mutex_);
    if (prefetch_workers_.empty() || prefetch_stop_ ||
        queued_prefetch_pages_.find(page_no) != queued_prefetch_pages_.end() ||
        prefetch_queue_.size() >= prefetch_queue_capacity_) {
      prefetch_dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (is_page_cached_or_loading(page_no)) {
      prefetch_dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    queued_prefetch_pages_.insert(page_no);
    prefetch_queue_.push_back({page_no, disk_manager});
    prefetch_enqueued_.fetch_add(1, std::memory_order_relaxed);
  }
  prefetch_cv_.notify_one();
}

template <typename PageType>
void buf_pool_t<PageType>::wait_for_prefetch_idle() {
  std::unique_lock<std::mutex> lk(prefetch_mutex_);
  prefetch_idle_cv_.wait(
      lk, [&] { return prefetch_queue_.empty() && prefetch_active_count_ == 0; });
}

// Get buffer pool statistics.
template <typename PageType>
typename buf_pool_t<PageType>::stats_t buf_pool_t<PageType>::get_stats() const {
  stats_t stats;
  stats.cache_hits = cache_hits_.load(std::memory_order_relaxed);
  stats.cache_misses = cache_misses_.load(std::memory_order_relaxed);
  stats.duplicate_miss_waits = duplicate_miss_waits_.load(std::memory_order_relaxed);
  stats.evictions = evictions_.load(std::memory_order_relaxed);
  stats.dirty_writebacks = dirty_writebacks_.load(std::memory_order_relaxed);
  stats.prefetches = prefetches_.load(std::memory_order_relaxed);
  stats.prefetch_enqueued = prefetch_enqueued_.load(std::memory_order_relaxed);
  stats.prefetch_completed = prefetch_completed_.load(std::memory_order_relaxed);
  stats.prefetch_dropped = prefetch_dropped_.load(std::memory_order_relaxed);
  return stats;
}

// Reset buffer pool statistics.
template <typename PageType>
void buf_pool_t<PageType>::reset_stats() {
  cache_hits_.store(0, std::memory_order_relaxed);
  cache_misses_.store(0, std::memory_order_relaxed);
  duplicate_miss_waits_.store(0, std::memory_order_relaxed);
  evictions_.store(0, std::memory_order_relaxed);
  dirty_writebacks_.store(0, std::memory_order_relaxed);
  prefetches_.store(0, std::memory_order_relaxed);
  prefetch_enqueued_.store(0, std::memory_order_relaxed);
  prefetch_completed_.store(0, std::memory_order_relaxed);
  prefetch_dropped_.store(0, std::memory_order_relaxed);
}

// Copy external page data into the buffer pool.
template <typename PageType>
// Handle buf page cp.
PageType* buf_pool_t<PageType>::buf_page_cp(page_no_t page_no, char* page_data,
                                            disk_manager_t* disk_manager) {
  // Skip if the page is already cached.
  auto it = page_map.find(page_no);
  if (it != page_map.end()) {
    return nullptr;
  }

  PageType* free_page = claim_frame_for_reuse(disk_manager);

  // Copy page data into the selected frame.
  memcpy(free_page->get_data(), page_data, page_size_);
  free_page->set_page_no(page_no);
  free_page->disable_parse();
  free_page->self_parse();
  free_page->set_page_state(PAGE_IN_LRU);
  free_page->set_referenced(true);

  page_map.insert(page_no, free_page);
  free_page->w_unlatch();
  return free_page;
}

// Initialize the free list from all chunks.
template <typename PageType>
// Free list init.
void buf_pool_t<PageType>::free_list_init() {
  free_head_ = nullptr;
  free_tail_ = nullptr;

  for (size_t chunk_idx = 0; chunk_idx < chunk_num; ++chunk_idx) {
    buf_chunk_t<PageType>* chunk = chunks[chunk_idx];

    for (size_t page_idx = 0; page_idx < chunk->get_size(); ++page_idx) {
      PageType* page = &chunk->pages[page_idx];

      page->set_page_state(PAGE_FREE);
      page->set_prev(nullptr);
      page->set_next(nullptr);
      page->set_referenced(false);

      if (free_head_ == nullptr) {
        free_head_ = page;
        free_tail_ = page;
      } else {
        free_tail_->set_next(page);
        page->set_prev(free_tail_);
        free_tail_ = page;
      }
    }
  }
}

// Traverse all pages in the free list.
template <typename PageType>
// Traverse free list.
void buf_pool_t<PageType>::traverse_free_list(const std::function<bool(PageType*)>& func) {
  this->free_latch_.r_lock();
  PageType* current = free_head_;

  while (current != nullptr) {
    PageType* next = current->get_next();
    if (!func(current)) {
      break;
    }
    current = next;
  }
  this->free_latch_.r_unlock();
}

// Traverse all resident pages via the ConcurrentHashMap (no LRU latch held).
template <typename PageType>
// Traverse page map.
void buf_pool_t<PageType>::traverse_page_map(
    const std::function<bool(page_no_t, PageType*)>& func) {
  // ConcurrentHashMap supports safe concurrent iteration; no extra latch needed.
  for (auto it = page_map.cbegin(); it != page_map.cend(); ++it) {
    if (!func(it->first, it->second)) {
      break;
    }
  }
}

// Find a victim page with the clock policy.
template <typename PageType>
// Find victim page.
PageType* buf_pool_t<PageType>::find_victim_page() {
  if (clock_hand_ == nullptr) {
    clock_hand_ = lru_head_;
  }
  if (clock_hand_ == nullptr) {
    return nullptr;
  }

  PageType* start = clock_hand_;

  while (true) {
    // Skip pinned pages.
    if (clock_hand_->get_pin_count() > 0) {
      clock_hand_ = clock_hand_->get_next();
      if (clock_hand_ == nullptr)
        clock_hand_ = lru_head_;
      if (clock_hand_ == start) {
        // Fall back to a full scan.
        for (PageType* p = lru_head_; p != nullptr; p = p->get_next()) {
          if (p->get_pin_count() == 0 && p->try_w_latch())
            return p;
        }
        return nullptr;
      }
      continue;
    }

    if (!clock_hand_->get_referenced()) {
      if (!clock_hand_->try_w_latch()) {
        clock_hand_ = clock_hand_->get_next();
        if (clock_hand_ == nullptr)
          clock_hand_ = lru_head_;
        if (clock_hand_ == start) {
          for (PageType* p = lru_head_; p != nullptr; p = p->get_next()) {
            if (p->get_pin_count() == 0 && p->try_w_latch())
              return p;
          }
          return nullptr;
        }
        continue;
      }
      PageType* victim = clock_hand_;
      clock_hand_ = clock_hand_->get_next();
      if (clock_hand_ == nullptr)
        clock_hand_ = lru_head_;
      return victim;
    }

    // Clear the reference bit once.
    clock_hand_->set_referenced(false);
    clock_hand_ = clock_hand_->get_next();
    if (clock_hand_ == nullptr)
      clock_hand_ = lru_head_;

    if (clock_hand_ == start) {
      // Fall back to a full scan.
      for (PageType* p = lru_head_; p != nullptr; p = p->get_next()) {
        if (p->get_pin_count() == 0 && p->try_w_latch())
          return p;
      }
      return nullptr;
    }
  }
}

// Allocate a new page frame without reading from disk.
template <typename PageType>
// Handle buf page new.
PageType* buf_pool_t<PageType>::buf_page_new(page_no_t page_no, disk_manager_t* disk_manager) {
  // Return directly on cache hit.
  PageType* cached_page = try_latch_cached_page(page_no, true);
  if (cached_page != nullptr) {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
    return cached_page;
  }

  PageType* free_page = claim_frame_for_reuse(disk_manager);

  // Reinitialize the selected frame.
  free_page->reinit(page_no);
  free_page->set_page_state(PAGE_IN_LRU);
  free_page->set_referenced(true);
  free_page->inc_pin_count();

  page_map.insert(page_no, free_page);
  return free_page;
}

// Retire one page from the buffer pool if it is not in use.
template <typename PageType>
bool buf_pool_t<PageType>::retire_page(page_no_t page_no) {
  auto it = page_map.find(page_no);
  if (it == page_map.end()) {
    return true;
  }

  PageType* page = it->second;
  this->lru_latch_.w_lock();
  this->free_latch_.w_lock();

  auto locked_it = page_map.find(page_no);
  if (locked_it == page_map.end() || locked_it->second != page) {
    this->free_latch_.w_unlock();
    this->lru_latch_.w_unlock();
    return true;
  }
  if (page->get_pin_count() > 0 || !page->try_w_latch()) {
    this->free_latch_.w_unlock();
    this->lru_latch_.w_unlock();
    return false;
  }

  page_map.erase(page_no);
  remove_from_lru_list(page);
  page->disable_parse();
  page->mark_clean();
  page->set_referenced(false);
  page->set_page_state(PAGE_FREE);
  add_to_free_list(page);

  this->free_latch_.w_unlock();
  this->lru_latch_.w_unlock();
  page->w_unlatch();
  return true;
}

// Remove a page from the free list.
template <typename PageType>
// Remove from free list.
void buf_pool_t<PageType>::remove_from_free_list(PageType* page) {
  if (page == nullptr)
    return;

  PageType* prev = page->get_prev();
  PageType* next = page->get_next();

  if (prev) {
    prev->set_next(next);
  } else {
    free_head_ = next;
  }

  if (next) {
    next->set_prev(prev);
  } else {
    free_tail_ = prev;
  }

  page->set_next(nullptr);
  page->set_prev(nullptr);
}

// Add a page to the head of the LRU list.
template <typename PageType>
// Add to lru head.
void buf_pool_t<PageType>::add_to_lru_head(PageType* page) {
  if (page == nullptr) {
    return;
  }

  page->set_prev(nullptr);
  page->set_next(nullptr);

  if (lru_head_ == nullptr) {
    lru_head_ = page;
    lru_tail_ = page;
    clock_hand_ = page;
  } else {
    page->set_next(lru_head_);
    lru_head_->set_prev(page);
    lru_head_ = page;
  }
}

// Remove a page from the LRU list.
template <typename PageType>
// Remove from lru list.
void buf_pool_t<PageType>::remove_from_lru_list(PageType* page) {
  if (page == nullptr) {
    return;
  }

  PageType* prev = page->get_prev();
  PageType* next = page->get_next();

  // Move the clock hand away from the removed page.
  if (clock_hand_ == page) {
    clock_hand_ = next;
    if (clock_hand_ == nullptr) {
      clock_hand_ = lru_head_;
    }
  }

  if (prev != nullptr) {
    prev->set_next(next);
  } else {
    lru_head_ = next;
  }

  if (next != nullptr) {
    next->set_prev(prev);
  } else {
    lru_tail_ = prev;
  }

  page->set_next(nullptr);
  page->set_prev(nullptr);
}

// Add a page to the tail of the free list.
template <typename PageType>
// Add to free list.
void buf_pool_t<PageType>::add_to_free_list(PageType* page) {
  if (page == nullptr) {
    return;
  }

  page->set_page_state(PAGE_FREE);
  page->set_prev(nullptr);
  page->set_next(nullptr);

  if (free_head_ == nullptr) {
    free_head_ = page;
    free_tail_ = page;
  } else {
    free_tail_->set_next(page);
    page->set_prev(free_tail_);
    free_tail_ = page;
  }
}

// Move a page to the head of the LRU list.
template <typename PageType>
// Move to lru head.
void buf_pool_t<PageType>::move_to_lru_head(PageType* page) {
  if (page == nullptr || page == lru_head_) {
    return;
  }

  remove_from_lru_list(page);
  add_to_lru_head(page);
}

// Explicit template instantiations.
template struct buf_chunk_t<csr_page_t>;
template struct buf_pool_t<csr_page_t>;
template struct buf_chunk_t<delta_page_t>;
template struct buf_pool_t<delta_page_t>;
