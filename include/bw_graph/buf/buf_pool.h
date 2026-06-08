#ifndef BW_GRAPH_BUF_BUF_POOL_H
#define BW_GRAPH_BUF_BUF_POOL_H

#include "bw_graph/common/config.h"
#include "bw_graph/common/latch.h"
#include "bw_graph/common/type.h"
#include "bw_graph/storage/disk_manager.h"
#include "bw_graph/storage/page.h"

#include <cstddef>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <functional>
#include <type_traits>
#include <vector>

/**
 * @brief Buffer chunk holding a fixed number of pages of type PageType.
 *
 * PageType must be a subclass of page_t.
 */
template <typename PageType>
struct buf_chunk_t {
  static_assert(std::is_base_of_v<page_t, PageType>, "PageType must derive from page_t");

public:
  /* The number of pages in this chunk. */
  size_t size;

  /* The page status in this chunk. */
  /* False represents an invalid slot, true represents a valid page. */
  std::vector<bool> valid;

  /* The page headers of those frames. */
  PageType* pages;

  /* The page frames in this chunk. */
  char* frames;

public:
  /**
   * @brief Default constructor
   */
  buf_chunk_t() : size(0), pages(nullptr), frames(nullptr) {}

  ~buf_chunk_t() {
    delete[] pages;
    delete[] frames;
  }

  /**
   * @brief Get the number of pages in this chunk
   *
   * @return Number of pages in this chunk
   */
  size_t get_size() const { return size; }

  /**
   * @brief Initialize a buffer chunk with the given number of pages
   *
   * @param page_count  Number of pages in this chunk
   * @param page_size   Size of each page frame in bytes
   * @return A pointer to the initialized buf_chunk_t
   */
  static buf_chunk_t<PageType>* buf_chunk_init(size_t page_count, size_t page_size);
};

/**
 * @brief Buffer pool managing a pool of PageType pages backed by a
 * disk_manager_t.
 *
 * PageType must be a subclass of page_t and must provide:
 *   - PageType* get_next() const
 *   - PageType* get_prev() const
 *   - void set_next(PageType*)
 *   - void set_prev(PageType*)
 *   - void self_parse()
 *   - void disable_parse()
 *
 * page_state_t get_page_state() / set_page_state() are inherited from page_t.
 */
template <typename PageType>
struct buf_pool_t {
  static_assert(std::is_base_of_v<page_t, PageType>, "PageType must derive from page_t");

private:
  /* The number of chunks in the pool. */
  size_t chunk_num;

  /* The buffer chunks in the pool. */
  std::vector<buf_chunk_t<PageType>*> chunks;

  /* The fast page index for allocation. */
  folly::ConcurrentHashMap<page_no_t, PageType*> page_map;

  /* Head of the Least Recently Used (LRU) list of pages */
  PageType* lru_head_{nullptr};

  /* Head of the Free list of pages */
  PageType* free_head_{nullptr};

  /* Tail of the Least Recently Used (LRU) list of pages */
  PageType* lru_tail_{nullptr};

  /* Tail of the Free list of pages */
  PageType* free_tail_{nullptr};

  /* Latch for the free list */
  rw_latch_t free_latch_;

  /* Latch for the LRU list */
  rw_latch_t lru_latch_;

  /* Clock hand for Clock/Second-Chance page replacement algorithm */
  PageType* clock_hand_{nullptr};

  /* Page frame size in bytes (determines I/O and memcpy size) */
  size_t page_size_{bw_graph::BW_GRAPH_PAGE_SIZE};

public:
  /**
   * @brief Default constructor
   */
  buf_pool_t() : chunk_num(0) {}

  ~buf_pool_t() {
    for (buf_chunk_t<PageType>* chunk : chunks) {
      delete chunk;
    }
  }

  /**
   * @brief Get the number of chunks in the pool
   *
   * @return Number of chunks in the pool
   */
  size_t get_chunk_num() const { return chunk_num; }

  /**
   * @brief Initialize a buffer pool
   *
   * @param chunk_count           Number of chunks
   * @param page_count_per_chunk  Pages per chunk
   * @param page_size             Frame size in bytes (default:
   * BW_GRAPH_PAGE_SIZE)
   * @return Pointer to the initialized buf_pool_t
   */
  static buf_pool_t<PageType>* buf_pool_init(size_t chunk_count, size_t page_count_per_chunk,
                                             size_t page_size = bw_graph::BW_GRAPH_PAGE_SIZE);

  /**
   * @brief Read a page from the buffer pool (acquires read latch on return)
   *
   * @param page_no       Page number to read
   * @param disk_manager  Disk manager used for page I/O
   * @return Pointer to the requested page
   */
  PageType* buf_page_read(page_no_t page_no, disk_manager_t* disk_manager);

  /**
   * @brief Read a page from the buffer pool for slot-write operations
   *        (returns with write latch held)
   *
   * @param page_no       Page number to read
   * @param disk_manager  Disk manager used for page I/O
   * @return Pointer to the requested page
   */
  PageType* buf_page_read_for_slot_write(page_no_t page_no, disk_manager_t* disk_manager);

  /**
   * @brief Copy external page data into the buffer pool
   *
   * @param page_no       Page number to copy into
   * @param page_data     Source page data
   * @param disk_manager  Disk manager used for page I/O
   * @return Pointer to the copied page
   */
  PageType* buf_page_cp(page_no_t page_no, char* page_data, disk_manager_t* disk_manager);

  /**
   * @brief Allocate a brand-new page frame without reading from disk.
   *
   * @param page_no      Page number to assign to the new frame
   * @param disk_manager Disk manager used for write-back of evicted dirty pages
   * @return Pointer to the newly initialised page (write latch held on return)
   */
  PageType* buf_page_new(page_no_t page_no, disk_manager_t* disk_manager);

  /**
   * @brief Remove a retired page from the buffer pool if no thread is reading it.
   *
   * @param page_no Page number to retire
   * @return true if the page is absent or removed, false if it is still in use
   */
  bool retire_page(page_no_t page_no);

  /**
   * @brief Traverse the free list with a lambda function
   *
   * @param func  Returns true to continue traversal, false to stop
   */
  void traverse_free_list(const std::function<bool(PageType*)>& func);

  /**
   * @brief Traverse all pages currently resident in the pool (page_map).
   *
   * Iterates the internal ConcurrentHashMap without holding the LRU latch,
   * so disk writes inside @p func do NOT stall the allocation path.
   * The ConcurrentHashMap guarantees safe concurrent iteration.
   *
   * @param func  Called for each (page_no, page*) pair.
   *              Returns true to continue, false to stop early.
   */
  void traverse_page_map(const std::function<bool(page_no_t, PageType*)>& func);

private:
  /**
   * @brief Initialize the free list with all buffer pages
   */
  void free_list_init();

  /**
   * @brief Add a page to the tail of the free list
   *
   * @param page  Page to add
   */
  void add_to_free_list(PageType* page);

  /**
   * @brief Remove a page from the free list
   *
   * @param page  Page to remove
   */
  void remove_from_free_list(PageType* page);

  /**
   * @brief Add a page to the head of the LRU list
   *
   * @param page  Page to add
   */
  void add_to_lru_head(PageType* page);

  /**
   * @brief Remove a page from the LRU list
   *
   * @param page  Page to remove
   */
  void remove_from_lru_list(PageType* page);

  /**
   * @brief Move a page to the head of the LRU list
   *
   * @param page  Page to move
   */
  void move_to_lru_head(PageType* page);

  /**
   * @brief Find a victim page for replacement
   *
   * @return Pointer to the selected victim page
   */
  PageType* find_victim_page();
};

#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"

extern template struct buf_chunk_t<csr_page_t>;
extern template struct buf_pool_t<csr_page_t>;
extern template struct buf_chunk_t<delta_page_t>;
extern template struct buf_pool_t<delta_page_t>;

#endif // BW_GRAPH_BUF_BUF_POOL_H
