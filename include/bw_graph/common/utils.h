#ifndef BW_GRAPH_COMMON_UTILS_H
#define BW_GRAPH_COMMON_UTILS_H

#include "bw_graph/common/parallel.h"
#include "bw_graph/common/type.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

/**
 * @brief Concurrent bitmap backed by atomic 64-bit words.
 */
class concurrent_bit_map {
private:
  std::unique_ptr<std::atomic<uint64_t>[]> bits_;
  size_t num_words_;
  size_t size_;

public:
  /**
   * @brief Construct a bitmap with the given number of bits.
   * @param size  Number of bits in the bitmap
   */
  explicit concurrent_bit_map(size_t size) : size_(size) {
    num_words_ = (size + 63) / 64;
    bits_ = std::make_unique<std::atomic<uint64_t>[]>(num_words_);
  }

  /**
   * @brief Set one bit atomically.
   * @param idx    Bit index to set
   * @param order  Memory ordering for the atomic update
   * @return Previous value of the bit
   */
  bool set(size_t idx, std::memory_order order = std::memory_order_seq_cst) {
    assert(idx < size_);
    size_t block_idx = idx / 64;
    size_t bit_idx = idx % 64;
    uint64_t mask = 1ULL << bit_idx;
    return bits_[block_idx].fetch_or(mask, order) & mask;
  }

  /**
   * @brief Test whether a bit is set.
   * @param idx  Bit index to read
   * @return True if the bit is set
   */
  bool test(size_t idx) const {
    assert(idx < size_);
    size_t block_idx = idx / 64;
    size_t bit_idx = idx % 64;
    uint64_t mask = 1ULL << bit_idx;
    return bits_[block_idx].load(std::memory_order_acquire) & mask;
  }

  /**
   * @brief Get the number of bits in the bitmap.
   * @return Number of bits
   */
  size_t size() const { return size_; }
};

/**
 * @brief Convert microseconds to a human-readable duration string.
 * @param microseconds  Time duration in microseconds
 * @return Duration string using s, ms, or microseconds
 */
inline std::string format_duration(uint64_t microseconds) {
  std::ostringstream oss;
  oss << std::fixed;

  if (microseconds < 1000) {
    // Less than 1ms: display in microseconds
    oss << microseconds << " μs";
  } else if (microseconds < 1000000) {
    // Less than 1s: display in milliseconds
    double ms = microseconds / 1000.0;
    oss << std::setprecision(2) << ms << " ms";
  } else {
    // 1s or more: display in seconds
    double s = microseconds / 1000000.0;
    oss << std::setprecision(3) << s << " s";
  }

  return oss.str();
}

/**
 * @brief Format a byte count using Bytes, KB, MB, or GB.
 * @param bytes  Size in bytes
 * @return Formatted size string
 */
inline std::string format_size(uint64_t bytes) {
  const uint64_t KB = 1024;
  const uint64_t MB = 1024 * KB;
  const uint64_t GB = 1024 * MB;

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);

  if (bytes >= GB) {
    oss << (static_cast<double>(bytes) / GB) << " GB";
  } else if (bytes >= MB) {
    oss << (static_cast<double>(bytes) / MB) << " MB";
  } else if (bytes >= KB) {
    oss << (static_cast<double>(bytes) / KB) << " KB";
  } else {
    oss << bytes << " Bytes";
  }

  return oss.str();
}

/**
 * @brief Generate a timestamp in microseconds since Unix epoch.
 * @return Timestamp in microseconds
 */
inline uint64_t generate_timestamp() {
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration);
  return static_cast<uint64_t>(microseconds.count());
}

/**
 * @brief Calculate the maximum bandwidth capacity that fits in one page.
 * @param page_size      Page size in bytes
 * @param max_neighbors  Maximum neighbor count
 * @return Estimated bandwidth capacity
 */
inline uint64_t calculate_bw_capacity(uint64_t page_size, uint64_t max_neighbors) {
  const uint64_t I = sizeof(page_no_t);
  const uint64_t P = page_size;
  const uint64_t N = max_neighbors;

  // Return 0 when the fixed metadata cannot fit.
  if (P <= (3 + N) * I) {
    return 0;
  }

  uint64_t left = 0;
  uint64_t right = P; // A reasonable upper bound
  uint64_t result = 0;

  // Binary search the maximum B satisfying the page-size inequality.
  while (left <= right) {
    uint64_t mid = left + (right - left) / 2;

    uint64_t left_side = (3 + mid + N) * I;
    uint64_t floor_term = (mid * mid + 7) / 8; // Integer floor division
    left_side += floor_term;

    if (left_side < P) {
      result = mid;
      left = mid + 1;
    } else {
      if (mid == 0)
        break;
      right = mid - 1;
    }
  }

  // Verify the selected value before returning it.
  uint64_t final_left_side = (3 + result + N) * I + (result * result + 7) / 8;

  if (final_left_side >= P) {
    return 100;
  }

  return result;
}

/**
 * @brief Get wall-clock time in seconds.
 * @return Current time in seconds
 */
inline double get_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + (tv.tv_usec / 1e6);
}

#if !defined __APPLE__ && !defined LOWMEM
#include <malloc.h>
// comment out the following two lines if running out of memory
static int __ii = mallopt(M_MMAP_MAX, 0);
static int __jj = mallopt(M_TRIM_THRESHOLD, -1);
#endif

#define newA(__E, __n) (__E*) malloc((__n) * sizeof(__E))

#define _SCAN_LOG_BSIZE 10
#define _SCAN_BSIZE (1 << _SCAN_LOG_BSIZE)

#define nblocks(_n, _bsize) (1 + ((_n) - 1) / (_bsize))

#define _F_BSIZE (2 * _SCAN_BSIZE)

#define granular_for(_i, _start, _end, _cond, _body)                                               \
  {                                                                                                \
    if (_cond) {                                                                                   \
      {                                                                                            \
        bw_parallel_for(size_t _i = _start; _i < _end; _i++) { _body }                             \
      }                                                                                            \
    } else {                                                                                       \
      {                                                                                            \
        for (size_t _i = _start; _i < _end; _i++) {                                                \
          _body                                                                                    \
        }                                                                                          \
      }                                                                                            \
    }                                                                                              \
  }

namespace pbbs {

/**
 * @brief Empty marker type.
 */
struct empty {};

typedef uint32_t flags;
const flags no_flag = 0;
const flags fl_sequential = 1;
const flags fl_debug = 2;
const flags fl_time = 4;

/**
 * @brief Copy-construct into uninitialized storage.
 * @param a  Destination storage
 * @param b  Source value
 */
template <typename T>
inline void assign_uninitialized(T& a, const T& b) {
  new (static_cast<void*>(std::addressof(a))) T(b);
}

/**
 * @brief Allocate cache-line-aligned array storage without initialization.
 * @param n            Number of elements
 * @param touch_pages  Whether to touch huge-page-sized intervals
 * @return Allocated array pointer
 */
template <typename E>
E* new_array_no_init(size_t n, bool touch_pages = false) {
  // Pad allocation size to a cache-line boundary.
  size_t line_size = 64;
  size_t bytes = ((n * sizeof(E)) / line_size + 1) * line_size;
#ifndef __APPLE__
  E* r = (E*) aligned_alloc(line_size, bytes);
#else
  E* r;
  if (posix_memalign((void**) &r, line_size, bytes) != 0) {
    fprintf(stderr, "Cannot allocate space");
    exit(1);
  }
#endif
  if (r == NULL) {
    fprintf(stderr, "Cannot allocate space");
    exit(1);
  }
  // Touch one byte per huge page to populate page tables.
  if (touch_pages)
    bw_parallel_for(size_t i = 0; i < bytes; i = i + (1 << 21))((bool*) r)[i] = 0;
  return r;
}

} // namespace pbbs

#endif // BW_GRAPH_COMMON_UTILS_H
