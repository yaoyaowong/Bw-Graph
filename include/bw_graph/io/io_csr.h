#ifndef BW_GRAPH_CSR_PAGE_H
#define BW_GRAPH_CSR_PAGE_H

#include "bw_graph/common/ptv_codec.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/page.h"

class disk_manager_t;
#include <cstdint>
#include <utility>
#include <vector>

/**
 * @brief Compressed Sparse Row (CSR) graph storage page
 *
 * This page provides an efficient way to store and manipulate sparse graphs
 * using the CSR format. It inherits from page_t and supports disk-based
 * storage for handling large graphs that may not fit entirely in memory.
 */
class csr_page_t : public page_t {
private:
  /* Is parse. */
  bool is_parse_{false};

  /* Total number of vertices in the page */
  uint16_t num_vertices_{0};

  /* Total number of edges in the graph */
  uint16_t num_edges_{0};

  /* Used bytes in this page */
  uint32_t used_bytes_{0};

  /* Parent page number in the hierarchy */
  page_no_t parent_page_no_{0};

  /* Number of neighbor pages in the hierarchy */
  page_no_t neighbor_page_count_{0};

  /* Span view of neighbor pages */
  block_span_t neighbor_page_span_;

  /* Span view of vertex structures */
  vertex_span_t vertex_span_;

  /* Pointer to neighbor vertex IDs in page data */
  v_id_t* neighbor_ptr_;

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  /* Watermark at the time of last txn-aware consolidation (in-memory only, not on disk) */
  timestamp_t page_snapshot_ts{INVALID_TS};

  /* Points into page data at the per-edge timestamp section (after neighbor data).
   * Parallel to the neighbor array: same length (includes slot entries).
   * Timestamp for vertex vi's j-th edge: edge_ts_ptr_[vi.offset/sizeof(v_id_t) + j]. */
  timestamp_t* edge_ts_ptr_{nullptr};
#endif // BW_GRAPH_ENABLE_TRANSACTION

public:
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  void set_page_snapshot_ts(timestamp_t ts) { page_snapshot_ts = ts; }
  timestamp_t get_page_snapshot_ts() const { return page_snapshot_ts; }

  /**
   * @brief Return the commit timestamp of the j-th neighbor of the vertex at
   *        vertex_offset.  INVALID_TS (0) means unconditionally visible.
   * @param vertex_offset Position of the vertex in vertex_span_ (= item.offset).
   * @param j             0-based neighbor index within the vertex's degree.
   */
  timestamp_t get_edge_ts(uint16_t vertex_offset, uint32_t j) const;

  /**
   * @brief Overwrite per-edge timestamps from ts_map.
   *
   * ts_map[v] must be a vector of length degree(v), in the same sorted order
   * as the neighbors stored on the page.  Marks the page dirty.
   * @param ts_map src → vector<timestamp_t> parallel to sorted neighbor list.
   */
  void write_edge_timestamps(const edge_ts_map_t& ts_map);
#endif // BW_GRAPH_ENABLE_TRANSACTION

  /**
   * @brief Default constructor
   *
   * Initializes an empty CSR page with zero vertices and edges
   */
  csr_page_t();

  /**
   * @brief Constructor from adjacency map
   *
   * Builds the CSR structure from a map where keys are vertex IDs and
   * values are vectors of neighbor IDs. This constructor automatically
   * calculates offsets and constructs the compressed representation.
   * Note that this constructor does not read from disk, it builds the
   * CSR structure in memory.
   * @param page_no The page number for this CSR page
   * @param adjacency_map Map containing vertex ID as key and neighbor list as
   *value
   * @param vertex_map Vector to be filled with (vertex_id, offset)
   *                 pairs for quick lookup
   */
  explicit csr_page_t(const page_no_t& page_no, const adj_map_iter_t& adjacency_map,
                      std::vector<std::pair<v_id_t, uint16_t>>& vertex_map, uint16_t edge_slot = 0);

  /**
   * @brief Constructor with per-vertex slot reservation.
   *
   * Identical to the page-uniform constructor except that per_vertex_slots
   * is consulted per vertex_id, giving the number of trailing free slots
   * reserved for that vertex.  Used by the workload-driven SMO path so
   * that write-heavy vertices can reserve more in-place slots than their
   * read-only neighbours sharing the same page. Vertices absent from the
   * map fall back to 0 slots.
   * @param page_no Allocated page number.
   * @param adjacency_map Materialised neighbour map.
   * @param vertex_map Output: vertex_id -> page-local offset.
   * @param per_vertex_slots vertex_id -> reserved slot count.
   */
  explicit csr_page_t(const page_no_t& page_no, const adj_map_iter_t& adjacency_map,
                      std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                      const folly::F14FastMap<v_id_t, uint16_t>& per_vertex_slots);

  /**
   * @brief Get current used bytes in the page
   * @return Used bytes count
   */
  uint64_t get_used_bytes() const { return used_bytes_; }

  /**
   * @brief Move constructor from page_t object
   *
   * Takes ownership of the page_t object's resources and initializes
   * the CSR page from the page data. The original page_t object will
   * be destroyed/invalidated after this operation.
   *
   * @param page The page_t object to move from and parse CSR data
   */
  explicit csr_page_t(page_t&& page);

  /**
   * @brief Get all neighbors of a specific vertex by offset
   *
   * @param vertex_offset Offset of the vertex in the vertex array
   * @return Span containing all neighbor IDs, empty if vertex not found
   * @note Only valid when BW_GRAPH_NEIGHBOR_COMPRESS == false.
   */
  neighbor_span_t get_neighbors(uint16_t vertex_offset);

  /**
   * @brief Get a lazy PTV-decoded neighbor range.
   *
   * Only valid when BW_GRAPH_NEIGHBOR_COMPRESS == true.
   * The returned range decodes on demand; no copy is made.
   *
   * Pass vertex_offset to read degree-many actual neighbors.
   * To also expose trailing slot entries pass the slot count separately
   * to the underlying ptv_neighbor_range_t (count = degree + slot_count).
   *
   * @param vertex_offset Offset of the vertex in the vertex array
   * @return PTV range for lazy, zero-copy neighbour iteration
   */
  bw_graph::ptv_neighbor_range_t get_neighbor_ptv_range(uint16_t vertex_offset);

  /**
   * @brief Get the degree of a specific vertex by offset
   *
   * @param vertex_offset Offset of the vertex in the vertex array
   * @return Degree of the vertex, or 0 if not found
   */
  uint64_t get_vertex_degree(uint16_t vertex_offset);

  /**
   * @brief Get all vertices as a span
   *
   * @return Span of all vertex structures
   */
  vertex_span_t get_vertices();

  /**
   * @brief Get total number of vertices
   *
   * @return Number of vertices in this CSR page
   */
  uint64_t get_num_vertices();

  /**
   * @brief Get total number of edges
   *
   * @return Number of edges in this CSR page
   */
  uint64_t get_num_edges();

  /**
   * @brief Self-parse the CSR page
   *
   * This function attempts to parse the CSR page data and initialize
   * the internal structures. It should be called after the page data
   * has been loaded into memory.
   */
  void self_parse();

  /**
   * @brief Re-initialise this frame as a brand-new empty CSR page.
   *
   * Required by buf_pool_t::buf_page_new().  CSR pages are normally loaded
   * from disk, so this is a no-op that just resets the page number and
   * clears the parse flag; the caller is responsible for writing real data.
   *
   * @param page_no Page number to assign
   */
  void reinit(page_no_t page_no);

  /**
   * @brief Disable further parsing of the CSR page
   *
   * This function sets a flag to prevent the CSR page from being
   * parsed again. Useful in scenarios where the page data is known
   * to be valid and re-parsing is unnecessary.
   */
  void disable_parse();

  /**
   * @brief Get the next page in the LRU list (typed wrapper over page_t)
   */
  csr_page_t* get_next() const { return static_cast<csr_page_t*>(get_next_page()); }

  /**
   * @brief Get the previous page in the LRU list (typed wrapper over page_t)
   */
  csr_page_t* get_prev() const { return static_cast<csr_page_t*>(get_prev_page()); }

  /**
   * @brief Set the next page in the LRU list (typed wrapper over page_t)
   */
  void set_next(csr_page_t* next) { set_next_page(next); }

  /**
   * @brief Set the previous page in the LRU list (typed wrapper over page_t)
   */
  void set_prev(csr_page_t* prev) { set_prev_page(prev); }

  /**
   * @brief Insert a new edge into the CSR structure
   * @param vertex_offset Offset of the vertex in the vertex array
   * @param src Source vertex ID
   * @param dest Destination vertex ID
   * @return true if the edge was successfully inserted, false if it already
   * exists
   */
  insert_status_t insert_edge(uint16_t vertex_offset, v_id_t src, v_id_t dest);

  /**
   * @brief Insert new edges to reconstruct the CSR structure
   * @param deltas Span of delta records containing edge modifications
   * @param vertex_map Vector containing (vertex_id, offset) pairs for lookup
   * @return true if the edge was successfully inserted, false if it already
   * exists
   */
  insert_status_t
  apply_deltas(std::span<delta_record_t> deltas,
               std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
               std::vector<std::pair<v_id_t, std::vector<v_id_t>>>& giant_vertex_ids);

  /**
   * @brief Set the parent page number
   * @param parent_page_no The parent page number to set
   */
  void set_parent_page_no(page_no_t parent_page_no);

  /**
   * @brief Get the parent page number
   * @return The parent page number
   */
  page_no_t get_parent_page_no() const { return parent_page_no_; }

  /**
   * @brief: Generate a description of the index page
   * @return: A string description of the index page
   */
  std::unordered_map<std::string, std::string> generate_desc();

  /**
   * @brief Add a neighbor page number to the list
   * @param page_no The neighbor page number to add
   * @return true if success, false if fail, meaning the block is full
   */
  bool add_neighbor_page_no(page_no_t page_no);

  /**
   * @brief Read the neighbor block and return a clone
   * @return A vector containing the neighbor page numbers
   */
  std::vector<page_no_t> read_neighbor_block_clone();

  /**
   * @brief Read the neighbor block and return a span
   * @return A span containing the neighbor page numbers
   */
  block_span_t read_neighbor_block();

  /**
   * @brief Insert a neighbor to the slot.
   * @param src_vertex_offset Offset of the source vertex in the vertex array
   * @param src_vertex_id Source vertex ID
   * @param dest_vertex_id Destination vertex ID
   * @return The edge insert status.
   */
  insert_status_t insert_edge_in_slot(uint16_t src_vertex_offset, v_id_t src_vertex_id,
                                      v_id_t dest_vertex_id);

  /**
   * @brief Try to flush this page to disk if dirty.
   *
   * Uses try_w_lock() so it never blocks the caller.
   * @param disk_mgr CSR disk manager used for write_page().
   * @return true if page is clean or successfully written.
   * @return false if latch is busy and this round is skipped.
   */
  bool flush_to_disk(disk_manager_t* disk_mgr);

private:
  /**
   * @brief Calculate space needed for a compressed vertex
   * @param neighbors The neighbor list
   * @return Bytes needed (vertex struct + compressed neighbors)
   */
  uint64_t calculate_compressed_size(std::vector<v_id_t>& neighbors);

  /**
   * @brief Build CSR structure from adjacency map
   *
   * @param adjacency_map Map containing vertex adjacency information
   * @param slot_count Number of slots to use for the CSR structure
   */
  void build_from_adjacency_map(const adj_map_iter_t& adjacency_map,
                                std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                                uint16_t edge_slot_count = 0);

  /**
   * @brief Per-vertex slot variant of build_from_adjacency_map.
   *
   * Each vertex looks up its reservation count in per_vertex_slots by
   * vertex_id; vertices absent from the map use 0 slots. Because
   * adj_map_iter_t (a folly::F14FastMap) has an unspecified iteration
   * order we cannot use a positional vector here.
   */
  void build_from_adjacency_map(const adj_map_iter_t& adjacency_map,
                                std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                                const folly::F14FastMap<v_id_t, uint16_t>& per_vertex_slots);

  /**
   * @brief Parse and initialize CSR structure from page data
   */
  void parse_from_page_data();

  /**
   * @brief Validate if the page contains valid CSR data
   *
   * @return true if the page data appears to contain valid CSR structure
   */
  bool is_valid_csr_data() const;
};

#endif // BW_GRAPH_CSR_PAGE_H
