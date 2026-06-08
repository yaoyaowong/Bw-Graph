#include "bw_graph/buf/buf_pool.h"

#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

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

  if (chunk_count != 0 &&
      page_count_per_chunk > std::numeric_limits<size_t>::max() / chunk_count) {
    delete pool;
    throw std::overflow_error("buf_pool: resident page count overflow");
  }
  const size_t resident_page_count = chunk_count * page_count_per_chunk;
  const size_t page_map_capacity = std::max<size_t>(8, resident_page_count);
  pool->page_map = folly::ConcurrentHashMap<page_no_t, PageType*>(page_map_capacity);

  pool->chunks.resize(chunk_count);
  for (size_t i = 0; i < chunk_count; ++i) {
    pool->chunks[i] = buf_chunk_t<PageType>::buf_chunk_init(page_count_per_chunk, page_size);
  }
  pool->free_list_init();
  return pool;
}

// Read a page and return it with a read latch.
template <typename PageType>
// Handle buf page read.
PageType* buf_pool_t<PageType>::buf_page_read(page_no_t page_no, disk_manager_t* disk_manager) {
  // Return directly on cache hit.
  auto it = page_map.find(page_no);
  if (it != page_map.end()) {
    it->second->set_referenced(true);
    it->second->r_latch();
    return it->second;
  }

  // Allocate a frame on cache miss.
  PageType* free_page = nullptr;

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
    if (free_page->is_dirty()) {
      disk_manager->write_page(free_page->get_page_no(), free_page->get_data());
    }
    remove_from_lru_list(free_page);
    page_map.erase(free_page->get_page_no());
    free_page->disable_parse();
  } else {
    free_page = free_head_;
    remove_from_free_list(free_page);
  }

  add_to_lru_head(free_page);
  free_page->w_latch();
  this->free_latch_.w_unlock();
  this->lru_latch_.w_unlock();

  // Load page data into the selected frame.
  disk_manager->read_page(page_no, free_page->get_data());
  free_page->set_page_no(page_no);
  free_page->disable_parse();
  free_page->self_parse();
  free_page->set_page_state(PAGE_IN_LRU);
  free_page->set_referenced(true);

  page_map.insert(page_no, free_page);
  free_page->w_to_r_latch();
  return free_page;
}

// Read a page and return it for write access.
template <typename PageType>
// Handle buf page read for slot write.
PageType* buf_pool_t<PageType>::buf_page_read_for_slot_write(page_no_t page_no,
                                                             disk_manager_t* disk_manager) {
  // Return directly on cache hit.
  auto it = page_map.find(page_no);
  if (it != page_map.end()) {
    it->second->set_referenced(true);
    it->second->r_latch();
    return it->second;
  }

  // Allocate a frame on cache miss.
  PageType* free_page = nullptr;

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
    if (free_page->is_dirty()) {
      disk_manager->write_page(free_page->get_page_no(), free_page->get_data());
    }
    remove_from_lru_list(free_page);
    page_map.erase(free_page->get_page_no());
    free_page->disable_parse();
  } else {
    free_page = free_head_;
    remove_from_free_list(free_page);
  }

  add_to_lru_head(free_page);
  free_page->w_latch();
  this->free_latch_.w_unlock();
  this->lru_latch_.w_unlock();

  // Load page data into the selected frame.
  disk_manager->read_page(page_no, free_page->get_data());
  free_page->set_page_no(page_no);
  free_page->disable_parse();
  free_page->self_parse();
  free_page->set_page_state(PAGE_IN_LRU);
  free_page->set_referenced(true);

  page_map.insert(page_no, free_page);
  return free_page;
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

  PageType* free_page = nullptr;

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
    if (free_page->is_dirty()) {
      disk_manager->write_page(free_page->get_page_no(), free_page->get_data());
    }
    remove_from_lru_list(free_page);
    page_map.erase(free_page->get_page_no());
    free_page->disable_parse();
  } else {
    free_page = free_head_;
    remove_from_free_list(free_page);
  }

  add_to_lru_head(free_page);
  free_page->w_latch();
  this->free_latch_.w_unlock();
  this->lru_latch_.w_unlock();

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
          if (p->get_pin_count() == 0)
            return p;
        }
        return nullptr;
      }
      continue;
    }

    if (!clock_hand_->get_referenced()) {
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
        if (p->get_pin_count() == 0)
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
  auto it = page_map.find(page_no);
  if (it != page_map.end()) {
    it->second->set_referenced(true);
    it->second->w_latch();
    return it->second;
  }

  PageType* free_page = nullptr;

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
    if (free_page->is_dirty()) {
      disk_manager->write_page(free_page->get_page_no(), free_page->get_data());
    }
    remove_from_lru_list(free_page);
    page_map.erase(free_page->get_page_no());
    free_page->disable_parse();
  } else {
    free_page = free_head_;
    remove_from_free_list(free_page);
  }

  add_to_lru_head(free_page);
  free_page->w_latch();
  this->free_latch_.w_unlock();
  this->lru_latch_.w_unlock();

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
