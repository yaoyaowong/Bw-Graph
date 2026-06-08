#ifndef BW_GRAPH_COMMON_PTV_CODEC_H
#define BW_GRAPH_COMMON_PTV_CODEC_H

#include "bw_graph/common/type.h"

#include <cstdint>
#include <utility>

/**
 * @brief Prefixed-Tag Vertex (PTV) variable-length integer encoding.
 *
 * The top 2 bits of the first byte encode the total byte width:
 *   - 00: 1 byte  (6-bit value,  0 to         63)
 *   - 01: 2 bytes (14-bit value, 0 to      16 383)
 *   - 10: 3 bytes (22-bit value, 0 to   4 194 303)
 *   - 11: 4 bytes (30-bit value, 0 to 1 073 741 823)
 *
 * UK-2007 has fewer than 134 M vertices, so all IDs fit in 27 bits and use at
 * most 4 bytes.
 *
 * Slots (pre-allocated free entries) are always serialized in 4-byte form
 * (tag 11, value 0 = 0xC0 0x00 0x00 0x00), so their position in the byte
 * stream stays fixed regardless of what gets inserted later.
 */

namespace bw_graph {

/**
 * @brief Get the number of bytes required to encode a vertex ID.
 *
 * @param id  Vertex ID to encode
 * @return Encoded size in bytes
 */
inline uint8_t ptv_encoded_size(v_id_t id) noexcept {
  if (id < (1u << 6))
    return 1;
  if (id < (1u << 14))
    return 2;
  if (id < (1u << 22))
    return 3;
  return 4;
}

/**
 * @brief Write a PTV-encoded vertex ID to a byte buffer.
 * @param buf  Destination buffer
 * @param id   Vertex ID to encode
 * @return Number of bytes written
 */
inline uint32_t ptv_write(uint8_t* buf, v_id_t id) noexcept {
  const uint8_t sz = ptv_encoded_size(id);
  switch (sz) {
  case 1:
    buf[0] = static_cast<uint8_t>(id); // tag 00
    break;
  case 2:
    buf[0] = static_cast<uint8_t>(0x40u | (id >> 8)); // tag 01
    buf[1] = static_cast<uint8_t>(id);
    break;
  case 3:
    buf[0] = static_cast<uint8_t>(0x80u | (id >> 16)); // tag 10
    buf[1] = static_cast<uint8_t>(id >> 8);
    buf[2] = static_cast<uint8_t>(id);
    break;
  default:                                             // 4
    buf[0] = static_cast<uint8_t>(0xC0u | (id >> 24)); // tag 11
    buf[1] = static_cast<uint8_t>(id >> 16);
    buf[2] = static_cast<uint8_t>(id >> 8);
    buf[3] = static_cast<uint8_t>(id);
    break;
  }
  return sz;
}

/**
 * @brief Write a vertex ID in fixed-width slot format.
 * @param buf  Destination buffer
 * @param id   Vertex ID to encode
 */
inline void ptv_write_slot(uint8_t* buf, v_id_t id) noexcept {
  buf[0] = static_cast<uint8_t>(0xC0u | (id >> 24));
  buf[1] = static_cast<uint8_t>(id >> 16);
  buf[2] = static_cast<uint8_t>(id >> 8);
  buf[3] = static_cast<uint8_t>(id);
}

/**
 * @brief Decode one PTV entry from a byte buffer.
 * @param buf  Source buffer
 * @return Pair of vertex ID and bytes consumed
 */
inline std::pair<v_id_t, uint32_t> ptv_read(const uint8_t* buf) noexcept {
  const uint8_t tag = (buf[0] >> 6) & 0x3u;
  switch (tag) {
  case 0:
    return {static_cast<v_id_t>(buf[0] & 0x3Fu), 1u};
  case 1:
    return {static_cast<v_id_t>((static_cast<uint32_t>(buf[0] & 0x3Fu) << 8) |
                                static_cast<uint32_t>(buf[1])),
            2u};
  case 2:
    return {static_cast<v_id_t>((static_cast<uint32_t>(buf[0] & 0x3Fu) << 16) |
                                (static_cast<uint32_t>(buf[1]) << 8) |
                                static_cast<uint32_t>(buf[2])),
            3u};
  default: // tag 11
    return {static_cast<v_id_t>((static_cast<uint32_t>(buf[0] & 0x3Fu) << 24) |
                                (static_cast<uint32_t>(buf[1]) << 16) |
                                (static_cast<uint32_t>(buf[2]) << 8) |
                                static_cast<uint32_t>(buf[3])),
            4u};
  }
}

/**
 * @brief Check whether a slot entry holds the empty marker.
 * @param buf  Source buffer pointing to a 4-byte slot entry
 * @return True if the slot is encoded as tag 11 with value 0
 */
inline bool ptv_slot_is_empty(const uint8_t* buf) noexcept {
  return buf[0] == 0xC0u && buf[1] == 0 && buf[2] == 0 && buf[3] == 0;
}

/**
 * @brief Lazy zero-copy iterator over a PTV-encoded neighbor list.
 */
struct ptv_neighbor_iter_t {
  /* Current byte position in the encoded stream. */
  const uint8_t* ptr_;

  /* The number of items left to decode. */
  uint32_t remaining_;

  /**
   * @brief Construct an iterator over encoded neighbor data.
   * @param p      Pointer to the encoded data
   * @param count  Number of items to decode
   */
  constexpr ptv_neighbor_iter_t(const uint8_t* p, uint32_t count) noexcept
      : ptr_(p), remaining_(count) {}

  /**
   * @brief Check whether another item can be decoded.
   * @return True if the iterator has at least one remaining item
   */
  bool has_next() const noexcept { return remaining_ > 0; }

  /**
   * @brief Decode and return the next vertex ID.
   * @return Next decoded vertex ID
   */
  v_id_t next() noexcept {
    auto [id, sz] = ptv_read(ptr_);
    ptr_ += sz;
    --remaining_;
    return id;
  }

  /**
   * @brief Dereference the current iterator position.
   * @return Current decoded vertex ID
   */
  v_id_t operator*() const noexcept { return ptv_read(ptr_).first; }

  /**
   * @brief Advance to the next encoded entry.
   * @return Reference to this iterator after advancing
   */
  ptv_neighbor_iter_t& operator++() noexcept {
    ptr_ += ptv_read(ptr_).second;
    --remaining_;
    return *this;
  }

  /**
   * @brief Compare iterators by remaining item count.
   * @param o  Iterator to compare against
   * @return True if both iterators have the same remaining count
   */
  bool operator==(const ptv_neighbor_iter_t& o) const noexcept {
    return remaining_ == o.remaining_;
  }

  /**
   * @brief Compare iterators by remaining item count.
   *
   * @param o  Iterator to compare against
   * @return True if iterators have different remaining counts
   */
  bool operator!=(const ptv_neighbor_iter_t& o) const noexcept {
    return remaining_ != o.remaining_;
  }
};

/**
 * @brief Range wrapper for a PTV-encoded neighbor list.
 */
struct ptv_neighbor_range_t {
  /* Pointer to the encoded neighbor data. */
  const uint8_t* data_;

  /* The number of items to decode. */
  uint32_t count_;

  /**
   * @brief Construct a range from encoded neighbor data.
   * @param data   Pointer to the encoded data
   * @param count  Number of items to decode
   */
  constexpr ptv_neighbor_range_t(const uint8_t* data, uint32_t count) noexcept
      : data_(data), count_(count) {}

  /**
   * @brief Get an iterator to the beginning of the range.
   * @return Iterator positioned at the first encoded entry
   */
  ptv_neighbor_iter_t begin() const noexcept { return {data_, count_}; }

  /**
   * @brief Get an iterator to the end of the range.
   * @return End iterator
   */
  ptv_neighbor_iter_t end() const noexcept { return {data_, 0u}; }

  /**
   * @brief Get the number of encoded entries in the range.
   * @return Number of items to decode
   */
  uint32_t size() const noexcept { return count_; }

  /**
   * @brief Check whether the range is empty.
   * @return True if there are no encoded entries
   */
  bool empty() const noexcept { return count_ == 0; }
};

} // namespace bw_graph

#endif // BW_GRAPH_COMMON_PTV_CODEC_H
