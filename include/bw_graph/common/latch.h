#ifndef BW_GRAPH_COMMON_LATCH_H
#define BW_GRAPH_COMMON_LATCH_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

// Ubuntu/Linux specific optimizations
#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

/**
 * @brief 32-bit atomic reader-writer latch implementation (Ubuntu optimized)
 *
 * This class provides a high-performance reader-writer latch using only 32
 * bits. Layout: [1 bit write flag][31 bits reader count]
 * - Bit 31: Write lock flag (1 = write locked, 0 = not write locked)
 * - Bits 0-30: Reader count (max 2^31-1 concurrent readers)
 *
 * Multiple readers can acquire the latch simultaneously, but writers have
 * exclusive access. This version includes Ubuntu/Linux specific optimizations.
 */
class rw_latch_t {
public:
  /**
   * @brief Default constructor
   *
   * Initializes a new reader-writer latch in an unlocked state.
   */
  rw_latch_t() : lock_state_(0) {}

  /**
   * @brief Copy constructor (deleted)
   */
  rw_latch_t(const rw_latch_t&) = delete;

  /**
   * @brief Copy assignment (deleted)
   */
  rw_latch_t& operator=(const rw_latch_t&) = delete;

  /**
   * @brief Move constructor
   */
  rw_latch_t(rw_latch_t&& other) noexcept
      : lock_state_(other.lock_state_.load(std::memory_order_relaxed)) {
    other.lock_state_.store(0, std::memory_order_relaxed);
  }

  /**
   * @brief Move assignment
   */
  rw_latch_t& operator=(rw_latch_t&& other) noexcept {
    if (this != &other) {
      lock_state_.store(other.lock_state_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
      other.lock_state_.store(0, std::memory_order_relaxed);
    }
    return *this;
  }

  /**
   * @brief Acquire write latch (exclusive access)
   *
   * Blocks until the latch can be acquired for writing. Only one writer
   * can hold the latch at a time, and no readers can access while a writer
   * holds the latch.
   */
  void w_lock() {
    while (!try_w_lock()) {
      spin_wait();
    }
  }

  /**
   * @brief Release write latch
   *
   * Releases the exclusive write lock, allowing other readers or writers
   * to acquire the latch.
   */
  void w_unlock() { lock_state_.store(0, std::memory_order_release); }

  /**
   * @brief Acquire read latch (shared access)
   *
   * Blocks until the latch can be acquired for reading. Multiple readers
   * can hold the latch simultaneously, but readers cannot acquire the latch
   * while a writer holds it.
   */
  void r_lock() {
    while (!try_r_lock()) {
      spin_wait();
    }
  }

  /**
   * @brief Release read latch
   *
   * Releases the shared read lock. When all readers have released their
   * locks, writers can acquire the latch.
   */
  void r_unlock() { lock_state_.fetch_sub(1, std::memory_order_release); }

  /**
   * @brief Try to acquire write latch without blocking
   *
   * @return true if write latch was successfully acquired, false otherwise
   */
  bool try_w_lock() {
    uint32_t expected = 0;
    return lock_state_.compare_exchange_strong(expected, WRITE_LOCK_FLAG,
                                               std::memory_order_acquire);
  }

  /**
   * @brief Try to acquire read latch without blocking
   *
   * @return true if read latch was successfully acquired, false otherwise
   */
  bool try_r_lock() {
    uint32_t expected = lock_state_.load(std::memory_order_acquire);
    while (true) {
      // Check if write locked or reader count would overflow
      if ((expected & WRITE_LOCK_FLAG) || (expected & READ_COUNT_MASK) == READ_COUNT_MASK) {
        return false;
      }

      // Try to increment reader count
      uint32_t desired = expected + 1;
      if (lock_state_.compare_exchange_weak(expected, desired, std::memory_order_acquire)) {
        return true;
      }
      // expected has been updated by compare_exchange_weak on failure
    }
  }

  /**
   * @brief Get current reader count (for debugging/monitoring)
   *
   * @return Number of current readers
   */
  uint32_t get_reader_count() const {
    return lock_state_.load(std::memory_order_relaxed) & READ_COUNT_MASK;
  }

  /**
   * @brief Convert write lock to read lock atomically (conservative version)
   */
  bool w_to_r_convert() {
    uint32_t expected = WRITE_LOCK_FLAG;
    uint32_t desired = 1;

    // Use seq_cst for maximum compatibility
    return lock_state_.compare_exchange_strong(expected, desired, std::memory_order_seq_cst);
  }

  /**
   * @brief Convert write lock to read lock atomically (safe conservative
   * version)
   */
  void w_to_r_convert_safe() {
    uint32_t current_state = lock_state_.load(std::memory_order_seq_cst);

    // Validation
    if ((current_state & WRITE_LOCK_FLAG) == 0) {
      throw std::logic_error("w_to_r_convert called without holding write lock");
    }

    if ((current_state & READ_COUNT_MASK) != 0) {
      throw std::logic_error("Invalid state: readers present during write lock");
    }

    // Atomic store with seq_cst
    lock_state_.store(1, std::memory_order_seq_cst);
  }

  /**
   * @brief Check if write locked (for debugging/monitoring)
   *
   * @return true if currently write locked
   */
  bool is_write_locked() const {
    return (lock_state_.load(std::memory_order_relaxed) & WRITE_LOCK_FLAG) != 0;
  }

  /**
   * @brief Get the size of this latch in bytes
   *
   * @return Size in bytes (should be 4)
   */
  static constexpr size_t size() { return sizeof(uint32_t); }

private:
  /* Lock state constants */
  static constexpr uint32_t WRITE_LOCK_FLAG = 0x80000000U; // Bit 31
  static constexpr uint32_t READ_COUNT_MASK = 0x7FFFFFFFU; // Bits 0-30

  /* Atomic lock state: [1 bit write flag][31 bits reader count] */
  mutable std::atomic<uint32_t> lock_state_;

  /**
   * @brief Ubuntu/Linux optimized spin waiting strategy
   *
   * Uses progressive backoff with platform-specific optimizations to reduce CPU
   * usage during contention
   */
  void spin_wait() const {
    static thread_local uint32_t spin_count = 0;

    if (spin_count < 8) {
      // Very short busy-wait with CPU pause instruction
      for (int i = 0; i < (1 << spin_count); ++i) {
#ifdef __x86_64__
        __builtin_ia32_pause(); // x86 PAUSE instruction
#elif defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory"); // ARM yield
#else
        std::this_thread::yield();
#endif
      }
      ++spin_count;
    } else if (spin_count < 16) {
      // Medium wait with thread yield
      std::this_thread::yield();
      ++spin_count;
    } else if (spin_count < 32) {
      // Longer wait with short sleep
      std::this_thread::sleep_for(std::chrono::nanoseconds(100));
      ++spin_count;
    } else {
      // Very long wait for heavy contention
#ifdef __linux__
      // Linux-specific: use sched_yield() which may be more efficient
      sched_yield();
#else
      std::this_thread::sleep_for(std::chrono::microseconds(1));
#endif
      spin_count = 0; // Reset spin count
    }
  }

  /**
   * @brief CPU pause hint for busy-wait loops
   */
  static inline void cpu_pause() {
#ifdef __x86_64__
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }
};

/**
 * @brief Vertex latch type
 */
using vertex_latch_t = rw_latch_t;

/**
 * @brief RAII wrapper for automatic write lock management
 *
 * Automatically acquires a write lock on construction and releases it
 * on destruction, ensuring proper lock cleanup even in the presence
 * of exceptions.
 */
struct scoped_w_lock_t {
  /**
   * @brief Constructor that acquires write lock
   *
   * @param latch Reference to the reader-writer latch to lock
   */
  explicit scoped_w_lock_t(rw_latch_t& latch) : latch_(latch), locked_(true) { latch_.w_lock(); }

  /**
   * @brief Destructor that releases write lock
   */
  ~scoped_w_lock_t() {
    if (locked_) {
      latch_.w_unlock();
    }
  }

  /* Disable copy constructor and assignment operator */
  scoped_w_lock_t(const scoped_w_lock_t&) = delete;
  scoped_w_lock_t& operator=(const scoped_w_lock_t&) = delete;

  /* Enable move constructor */
  scoped_w_lock_t(scoped_w_lock_t&& other) noexcept : latch_(other.latch_), locked_(other.locked_) {
    other.locked_ = false;
  }

  /**
   * @brief Manually release the lock
   */
  void unlock() {
    if (locked_) {
      latch_.w_unlock();
      locked_ = false;
    }
  }

private:
  rw_latch_t& latch_;
  bool locked_;
};

/**
 * @brief RAII wrapper for automatic read lock management
 *
 * Automatically acquires a read lock on construction and releases it
 * on destruction, ensuring proper lock cleanup even in the presence
 * of exceptions.
 */
struct scoped_r_lock_t {
  /**
   * @brief Constructor that acquires read lock
   *
   * @param latch Reference to the reader-writer latch to lock
   */
  explicit scoped_r_lock_t(rw_latch_t& latch) : latch_(latch), locked_(true) { latch_.r_lock(); }

  /**
   * @brief Destructor that releases read lock
   */
  ~scoped_r_lock_t() {
    if (locked_) {
      latch_.r_unlock();
    }
  }

  /* Disable copy constructor and assignment operator */
  scoped_r_lock_t(const scoped_r_lock_t&) = delete;
  scoped_r_lock_t& operator=(const scoped_r_lock_t&) = delete;

  /* Enable move constructor */
  scoped_r_lock_t(scoped_r_lock_t&& other) noexcept : latch_(other.latch_), locked_(other.locked_) {
    other.locked_ = false;
  }

  /**
   * @brief Manually release the lock
   */
  void unlock() {
    if (locked_) {
      latch_.r_unlock();
      locked_ = false;
    }
  }

private:
  rw_latch_t& latch_;
  bool locked_;
};

// Ubuntu specific performance tuning macros
#ifdef __linux__
/**
 * @brief Prefetch hint for better cache performance on Ubuntu/Linux
 */
#define LATCH_PREFETCH_READ(addr) __builtin_prefetch((addr), 0, 3)
#define LATCH_PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)

/**
 * @brief Branch prediction hints for Ubuntu/Linux
 */
#define LATCH_LIKELY(x) __builtin_expect(!!(x), 1)
#define LATCH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LATCH_PREFETCH_READ(addr)
#define LATCH_PREFETCH_WRITE(addr)
#define LATCH_LIKELY(x) (x)
#define LATCH_UNLIKELY(x) (x)
#endif

#endif // BW_GRAPH_COMMON_LATCH_H