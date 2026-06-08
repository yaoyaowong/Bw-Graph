#ifndef BW_GRAPH_COMMON_BITMAP_H
#define BW_GRAPH_COMMON_BITMAP_H

#include <cstddef>

#define WORD_OFFSET(i) (i >> 6)
#define BIT_OFFSET(i) (i & 0x3f)

/**
 * @brief A simple bitmap implementation.
 */
class bit_map_t {
public:
  size_t size;
  unsigned long* data;
  bit_map_t() {
    size = 0;
    data = nullptr;
  }
  bit_map_t(size_t size) { init(size); }
  void init(size_t size) {
    this->size = size;
    data = new unsigned long[WORD_OFFSET(size) + 1];
  }

  /**
   * @brief Clear this bitmap.
   */
  void clear() {
    size_t bm_size = WORD_OFFSET(size);
#pragma omp parallel for
    for (size_t i = 0; i <= bm_size; i++) {
      data[i] = 0;
    }
#pragma omp barrier
  }

  /**
   * @brief Fill this bitmap.
   *
   */
  void fill() {
    size_t bm_size = WORD_OFFSET(size);
#pragma omp parallel for
    for (size_t i = 0; i < bm_size; i++) {
      data[i] = 0xffffffffffffffff;
    }
#pragma omp barrier
    data[bm_size] = 0;
    for (size_t i = (bm_size << 6); i < size; i++) {
      data[bm_size] |= 1ul << BIT_OFFSET(i);
    }
  }

  /**
   * @brief Get the bit object.
   *
   * @param i The retrieval key.
   * @return unsigned long Result.
   */
  unsigned long get_bit(size_t i) { return data[WORD_OFFSET(i)] & (1ul << BIT_OFFSET(i)); }

  /**
   * @brief Set the bit object
   *
   * @param i The insert value.
   */
  void set_bit(size_t i) { __sync_fetch_and_or(data + WORD_OFFSET(i), 1ul << BIT_OFFSET(i)); }
};

#endif // BW_GRAPH_COMMON_BITMAP_H