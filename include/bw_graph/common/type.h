#ifndef BW_GRAPH_COMMON_TYPE_H
#define BW_GRAPH_COMMON_TYPE_H

#include <cstdint>
#include <folly/container/F14Map.h>
#include <limits>
#include <span>
#include <vector>

#ifdef BW_GRAPH_ENABLE_TRANSACTION
/**
 * @brief Logical-clock timestamp for MVCC. Only defined in transaction builds.
 */
using timestamp_t = uint64_t;
static constexpr timestamp_t INVALID_TS = 0;
static constexpr timestamp_t INF_TS = std::numeric_limits<uint64_t>::max();

#endif // BW_GRAPH_ENABLE_TRANSACTION

/**
 * @brief Vertex ID type definition using 32-bit unsigned integer
 */
using v_id_t = uint32_t;

/**
 * @brief Vertex Count type definition using 32-bit unsigned integer
 */
using v_cnt_t = v_id_t;

/**
 * @brief Community structure type definition using vector.
 */
using community_structure_t = std::vector<std::vector<v_id_t>>;

constexpr v_id_t INVALID_VID = std::numeric_limits<v_id_t>::max(); // 4294967295

/**
 * @brief Community ID.
 */
using comm_id_t = uint32_t;

/**
 * @brief Page number type definition using 64-bit unsigned integer.
 */
using page_no_t = uint32_t;

/**
 * @brief Neighbor structure type definition using a pair of vertex IDs.
 *
 * This structure represents the neighbors in the graph.
 */
using neighbor_span_t = std::span<v_id_t>;

/**
 * @brief Block structure type definition using a span of page numbers.
 */
using block_span_t = std::span<page_no_t>;

/**
 * @brief Adjacency list map type definition
 */
using adj_map_t = folly::F14FastMap<v_id_t, std::vector<v_id_t>>;

/**
 * @brief Adjacency list map (Zero copy version)
 */
using adj_map_iter_t = folly::F14FastMap<v_id_t, neighbor_span_t>;

/**
 * @brief Trasform adj_map_t to adj_map_iter_t;
 * @param adj_map source map
 * @return The view
 */
inline adj_map_iter_t create_neighbor_span_view(adj_map_t& adj_map) {
  adj_map_iter_t result;
  result.reserve(adj_map.size());

  for (auto& [vid, neighbors] : adj_map) {
    result[vid] = neighbor_span_t{neighbors};
  }

  return result;
}

/**
 * @brief Block adjacency map type definition
 */
using block_adj_map_t = folly::F14FastMap<page_no_t, std::vector<page_no_t>>;

#ifdef BW_GRAPH_ENABLE_TRANSACTION
/**
 * @brief Per-edge timestamp map: src vertex → parallel vector of commit
 *        timestamps, one entry per neighbor in sorted neighbor order.
 *        INVALID_TS (0) means always visible (initial graph edges).
 */
using edge_ts_map_t = folly::F14FastMap<v_id_t, std::vector<timestamp_t>>;
#endif // BW_GRAPH_ENABLE_TRANSACTION

/**
 * @brief Vertex location type definition
 */
using vertex_loc_t = std::pair<page_no_t, uint64_t>;

/**
 * @brief Vertex type enumeration
 *
 * This enum defines the types of vertices in the graph.
 * - NORMAL: Regular vertex
 * - GIANT: High degree vertex that may have different handling in the graph
 */
enum vertex_type_t : uint32_t {

  /* Regular vertex */
  NORMAL = 0,

  /* High degree vertex */
  GIANT = 1
};

/**
 * @brief Delta operation type enumeration
 *
 * This enum defines the types of delta operations that can be performed on the
 * graph.
 * - ADD_NEIGHBOR: Add a neighbor to a vertex
 * - REMOVE_NEIGHBOR: Remove a neighbor from a vertex
 */
enum class delta_operation_t : uint8_t {
  /* Add a neighbor to a vertex */
  ADD_NEIGHBOR = 1,

  /* Remove a neighbor from a vertex */
  REMOVE_NEIGHBOR = 2
};

/**
 * @brief Page type enumeration
 * - BASE_PAGE: Standard data page
 * - INDEX_PAGE: Page used for indexing purposes
 * - DELTA_PAGE: Page used for storing changes or updates
 */
enum page_type_t : uint8_t { BASE_PAGE = 0, INDEX_PAGE = 1, DELTA_PAGE = 2 };

/**
 * @brief Vertex latch type definition using 32-bit unsigned integer
 * - INSERTED: Vertex was successfully inserted
 * - EXISTS: Vertex already exists
 * - SLOT_FULL: Vertex insertion failed
 * - FAILED: Vertex insertion failed
 */
enum insert_status_t : uint8_t { INSERTED = 0, EXISTS = 1, SLOT_FULL = 2, FAILED = 3 };

/**
 * @brief Delta page status enumeration
 * - ACCUMULATE: Page is accumulating changes
 * - CONSOLIDATE: Page is being consolidated
 * - OUTOFDATE: Page is out of date
 */
enum delta_page_status_t : uint8_t { ACCUMULATE = 0, CONSOLIDATE = 1, OUTOFDATE = 2 };

/**
 * @brief Delta target type definition using 8-bit unsigned integer
 * - VERTEX: The delta operation targets a vertex
 * - EDGE: The delta operation targets an edge
 */
enum delta_target_t : uint16_t { VERTEX = 0, EDGE = 1 };

/**
 * @brief Delta operation type definition using 8-bit unsigned integer
 * - INSERT: Represents an insertion operation
 * - DELETE: Represents a deletion operation
 */
enum delta_type_t : uint16_t { INSERT = 0, DELETE = 1 };

/**
 * @brief Page state enumeration
 *
 * This enum defines the possible states of a page in the graph storage system.
 * - PAGE_FREE: The page is free and not currently used.
 * - PAGE_IN_LRU: The page is in the Least Recently Used (LRU) cache.
 */
enum page_state_t { PAGE_FREE = 0, PAGE_IN_LRU = 1 };

/**
 * @brief Graph partitioning structure
 */
using graph_partition_t = std::vector<std::vector<v_id_t>>;

/**
 * @brief Block partitioning structure
 */
using block_partition_t = std::vector<std::vector<page_no_t>>;

/**
 * @brief CSR vertex structure optimized for storage efficiency
 *
 * Memory layout (12 bytes, 4-byte aligned):
 * [0-3]   vertex_id    (uint32_t)
 * [4-7]   degree       (uint32_t)
 * [8-9]   offset       (uint16_t)
 * [10-11] delete_mark  (uint16_t)
 * Total: 12 bytes
 */
struct alignas(4) csr_vertex_t {
  /* Unique identifier for the vertex */
  v_id_t vertex_id;

  /* Number of neighbors (degree) of the vertex */
  uint32_t degree;

  /* Starting offset of neighbors in the neighbors array */
  uint16_t offset;

  /* Deletion mark: 0 = exists, 1 = deleted */
  uint16_t delete_mark;

  /**
   * @brief Default constructor
   *
   * Initializes all fields to zero (vertex exists by default)
   */
  csr_vertex_t() : vertex_id(0), degree(0), offset(0), delete_mark(0) {}

  /**
   * @brief Parameterized constructor
   *
   * @param id Vertex ID
   * @param deg Degree of the vertex
   * @param off Offset in the neighbors array
   * @param del Deletion mark (default: 0 = exists)
   */
  csr_vertex_t(v_id_t id, uint32_t deg, uint16_t off, uint16_t del = 0)
      : vertex_id(id), degree(deg), offset(off), delete_mark(del) {}

  /**
   * @brief Check if this vertex is deleted
   *
   * @return true if the vertex is marked as deleted
   */
  inline bool is_deleted() const { return delete_mark != 0; }

  /**
   * @brief Mark this vertex as deleted
   */
  inline void mark_deleted() { delete_mark = 1; }

  /**
   * @brief Mark this vertex as existing (undelete)
   */
  inline void mark_existing() { delete_mark = 0; }

  /**
   * @brief Get the size of this structure in bytes
   *
   * @return Size in bytes (12 bytes)
   */
  static constexpr size_t size_in_bytes() { return sizeof(csr_vertex_t); }

  /**
   * @brief Check if the structure is properly aligned for span access
   *
   * @return true if alignment is correct for efficient access
   */
  static constexpr bool is_properly_aligned() {
    return sizeof(csr_vertex_t) == 12 && alignof(csr_vertex_t) == 4;
  }

  /**
   * @brief Equality comparison operator
   *
   * Compares all semantic fields
   */
  bool operator==(const csr_vertex_t& other) const {
    return vertex_id == other.vertex_id && degree == other.degree && offset == other.offset &&
           delete_mark == other.delete_mark;
  }

  /**
   * @brief Inequality comparison operator
   */
  bool operator!=(const csr_vertex_t& other) const { return !(*this == other); }

  /**
   * @brief Less-than comparison for sorting (by vertex_id)
   */
  bool operator<(const csr_vertex_t& other) const { return vertex_id < other.vertex_id; }
} __attribute__((packed));

/**
 * @brief Span type for csr_vertex_t
 *
 * This type is used to represent a contiguous sequence of csr_vertex_t objects.
 */
using vertex_span_t = std::span<csr_vertex_t>;

#endif // BW_GRAPH_COMMON_TYPE_H