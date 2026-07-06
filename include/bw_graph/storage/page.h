#ifndef BW_GRAPH_STORAGE_PAGE_H
#define BW_GRAPH_STORAGE_PAGE_H

#include "bw_graph/common/config.h"
#include "bw_graph/common/latch.h"
#include "bw_graph/common/type.h"

#include <atomic>
#include <cstring>

/**
 * @brief Basic unit of storage within the database system
 *
 * Page provides a wrapper for actual data pages being held in main memory.
 * Page also contains book-keeping information that is used by the buffer pool
 * manager, such as pin count, dirty flag, page id, etc.
 */
class page_t {
public:
  /**
   * @brief Default constructor
   *
   * Initializes a new page with zeroed data and default values.
   */
  page_t() : data_(nullptr), page_no_(bw_graph::BW_GRAPH_DEFAULT_PAGE_NO) {
    data_ = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
    owns_data_ = true;
    pin_count_.store(0, std::memory_order_relaxed);
    is_dirty_.store(false, std::memory_order_relaxed);
    reset_memory();
  }

  /**
   * @brief Set the page ID (internal use only)
   *
   * @param page_no New page number to set
   */
  void set_page_no(page_no_t page_no) { page_no_ = page_no; }

  /**
   * @brief Move constructor
   *
   * Transfers ownership of resources from another page_t object.
   * The source page will be left in a valid but unspecified state.
   *
   * @param other The page_t object to move from
   */
  page_t(page_t&& other) noexcept
      : data_(other.data_), owns_data_(other.owns_data_), page_no_(other.page_no_),
        pin_count_(other.pin_count_.load(std::memory_order_acquire)),
        is_dirty_(other.is_dirty_.load(std::memory_order_acquire)) {
    // Clear the moved-from object
    other.data_ = nullptr;
    other.owns_data_ = false;
    other.page_no_ = bw_graph::BW_GRAPH_DEFAULT_PAGE_NO;
    other.pin_count_.store(0, std::memory_order_release);
    other.is_dirty_.store(false, std::memory_order_release);
  }

  /**
   * @brief Constructor with page number
   *
   * Initializes a new page with the specified page number and zeroed data.
   *
   * @param page_no The page number to assign to this page
   */
  page_t(page_no_t page_no, page_type_t page_type = BASE_PAGE) : data_(nullptr), page_no_(page_no) {
    size_t assign_size;
    if (page_type == INDEX_PAGE) {
      assign_size = bw_graph::BW_GRAPH_INDEX_PAGE_SIZE;
    } else if (page_type == DELTA_PAGE) {
      assign_size = bw_graph::BW_DELTA_PAGE_SIZE;
    } else {
      assign_size = bw_graph::BW_GRAPH_PAGE_SIZE;
    }
    data_ = new char[assign_size];
    owns_data_ = true;
    pin_count_.store(0, std::memory_order_relaxed);
    is_dirty_.store(false, std::memory_order_relaxed);
    reset_memory(page_type);
  }

  /**
   * @brief Default destructor
   */
  ~page_t() {
    if (owns_data_ && data_ != nullptr) {
      delete[] data_;
    }
    data_ = nullptr;
    owns_data_ = false;
  }

  /**
   * @brief Copy constructor (deleted to prevent copying)
   */
  page_t(const page_t&) = delete;

  /**
   * @brief Copy assignment operator (deleted to prevent copying)
   */
  page_t& operator=(const page_t&) = delete;

  /**
   * @brief Get the actual data contained within this page
   *
   * @return Pointer to the page data buffer
   */
  char* get_data() { return data_; }

  /**
   * @brief Get the page no of this page
   *
   * @return Page no
   */
  page_no_t get_page_no() const { return page_no_; }

  /**
   * @brief Get the pin count of this page
   *
   * @return Current pin count
   */
  int get_pin_count() const { return pin_count_.load(std::memory_order_acquire); }

  /**
   * @brief Check if the page has been modified
   *
   * @return true if the page in memory has been modified from the page on disk
   */
  bool is_dirty() const { return is_dirty_.load(std::memory_order_acquire); }

  /**
   * @brief Acquire the page write latch (exclusive access)
   */
  void w_latch() { rw_latch_.w_lock(); }

  /**
   * @brief Try to acquire the page write latch without blocking.
   */
  bool try_w_latch() { return rw_latch_.try_w_lock(); }

  /**
   * @brief Release the page write latch
   */
  void w_unlatch() { rw_latch_.w_unlock(); }

  /**
   * @brief Acquire the page read latch (shared access)
   */
  void r_latch() {
    inc_pin_count();
    rw_latch_.r_lock();
  }

  /**
   * @brief Release the page read latch
   */
  void r_unlatch() {
    rw_latch_.r_unlock();
    dec_pin_count();
  }

  /**
   * @brief Convert the page latch from write to read mode (internal use only)
   */
  void w_to_r_latch() {
    inc_pin_count();
    rw_latch_.w_to_r_convert_safe();
  }

  /**
   * @brief Mark the page as dirty (public interface for external components)
   */
  void mark_dirty() { set_dirty(true); }

  /**
   * @brief Mark the page as clean.
   */
  void mark_clean() { set_dirty(false); }

  /**
   * @brief Replace the backing data pointer (internal use only)
   *
   * @param data New data buffer pointer
   */
  void set_data(char* data) {
    if (owns_data_ && data_ != nullptr) {
      delete[] data_;
    }
    data_ = data;
    owns_data_ = false;
  }

  /**
   * @brief Get the referenced flag for page replacement algorithm
   *
   * @return true if the page has been recently referenced
   */
  bool get_referenced() const { return referenced_.load(std::memory_order_relaxed); }

  /**
   * @brief Set the referenced flag (internal use only)
   *
   * @param val New referenced state
   */
  void set_referenced(bool val) { referenced_.store(val, std::memory_order_relaxed); }

  /**
   * @brief Increment pin count — page will not be evicted while pinned
   *
   * @return New pin count after increment
   */
  int inc_pin_count() { return pin_count_.fetch_add(1, std::memory_order_acq_rel) + 1; }

  /**
   * @brief Decrement pin count — allows eviction when count reaches zero
   *
   * @return New pin count after decrement
   */
  int dec_pin_count() { return pin_count_.fetch_sub(1, std::memory_order_acq_rel) - 1; }

  /**
   * @brief Get the next page in the LRU/free list (base-typed)
   */
  page_t* get_next_page() const { return next_page_; }

  /**
   * @brief Set the next page in the LRU/free list (base-typed)
   */
  void set_next_page(page_t* n) { next_page_ = n; }

  /**
   * @brief Get the previous page in the LRU/free list (base-typed)
   */
  page_t* get_prev_page() const { return prev_page_; }

  /**
   * @brief Set the previous page in the LRU/free list (base-typed)
   */
  void set_prev_page(page_t* p) { prev_page_ = p; }

  /**
   * @brief Get the buffer pool state of this page
   */
  page_state_t get_page_state() const { return state_; }

  /**
   * @brief Set the buffer pool state of this page
   */
  void set_page_state(page_state_t s) { state_ = s; }

protected:
  /* The actual data that is stored within a page */
  char* data_;

  /* Whether this page owns data_ or only references a buffer-pool frame. */
  bool owns_data_{false};

  /* The ID of this page */
  page_no_t page_no_;

  /* The pin count of this page */
  std::atomic<size_t> pin_count_;

  /* True if the page is dirty, i.e. different from its corresponding page on
   * disk */
  std::atomic<bool> is_dirty_;

  /* The page latch protecting data access */
  rw_latch_t rw_latch_;

  /* Referenced bit for clock/LRU algorithm */
  std::atomic<bool> referenced_{false};

  /* Intrusive doubly-linked list pointers for LRU/free list management.
   * Subclasses expose typed wrappers (get_next / set_next) via static_cast. */
  page_t* next_page_{nullptr};
  page_t* prev_page_{nullptr};

  /* Buffer-pool state: PAGE_FREE or PAGE_IN_LRU */
  page_state_t state_{PAGE_FREE};

  /**
   * @brief Reset the page memory and metadata
   *
   * Zeroes out the data that is held within the page and sets all fields
   * to default values.
   */
  void reset_memory(page_type_t page_type = BASE_PAGE) {
    if (data_ != nullptr) {
      if (page_type == INDEX_PAGE) {
        std::memset(data_, 0, bw_graph::BW_GRAPH_INDEX_PAGE_SIZE);
      } else if (page_type == DELTA_PAGE) {
        std::memset(data_, 0, bw_graph::BW_DELTA_PAGE_SIZE);
      } else {
        std::memset(data_, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
      }
    }
    pin_count_.store(0, std::memory_order_release);
    is_dirty_.store(false, std::memory_order_release);
  }

  /**
   * @brief Mark the page as dirty (internal use only)
   *
   * @param is_dirty New dirty state
   */
  void set_dirty(bool is_dirty) { is_dirty_.store(is_dirty, std::memory_order_release); }
};

#endif // BW_GRAPH_STORAGE_PAGE_H
