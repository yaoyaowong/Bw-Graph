#ifndef BW_GRAPH_IO_BUILDER_H
#define BW_GRAPH_IO_BUILDER_H

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"

#include <cstddef>
#include <utility>
#include <vector>

// The builder of the CSR page.
// Used for TAT building.
struct csr_page_builder_t {
private:
  /**
   * The size of the page.
   */
  size_t page_size_{bw_graph::BW_GRAPH_PAGE_SIZE};

  /**
   * The number of edge slots in the page.
   */
  uint16_t edge_slot_count_{static_cast<uint16_t>(bw_graph::BW_GRAPH_EDGE_SLOT_COUNT)};

  /**
   * The vertices in this page.
   */
  std::vector<v_id_t> vertices_;

  /**
   * The offsets in this page (byte offsets for neighbor data).
   */
  std::vector<uint16_t> offsets_;

  /**
   * The degrees in this page.
   */
  std::vector<uint32_t> degrees_;

  /**
   * The edges in this page.
   */
  std::vector<v_id_t> edges_;

  /**
   * The current edge offset (byte offset).
   */
  uint32_t current_edge_offset_{0};

  /**
   * The Giant Vertices (stored as pairs of vertex_id and neighbors).
   */
  std::vector<std::pair<v_id_t, std::vector<v_id_t>>> giant_vertices_;

public:
  /**
   * @brief Constructor for csr_page_builder_t
   *
   * @param page_size The size of the page in bytes
   * @param edge_slot_count The number of edge slots in the page
   * @param is_compressed Whether to use compression for neighbor storage
   * (deprecated, kept for compatibility)
   */
  csr_page_builder_t(size_t page_size, uint16_t edge_slot_count);

  /**
   * @brief Add a vertex to the page.
   * @param vertex_id The ID of the vertex to add
   * @param neighbors The span of neighbor IDs for the vertex
   * @return true if the vertex was added successfully, false if there was
   *         not enough space in the page
   */
  bool add_vertex(v_id_t vertex_id, neighbor_span_t& neighbors);

  /**
   * @brief Add a vertex with an explicit per-vertex slot reservation.
   *
   * Used by the workload-driven SMO path so that write-heavy vertices can
   * reserve more in-place insertion slots than read-heavy neighbours
   * sharing the same page.  When per_vertex_slots is 0 this overload is
   * equivalent to the page-uniform path. Passing a value here overrides
   * the builder's default edge_slot_count_ for this single vertex only.
   * @param vertex_id The vertex being added.
   * @param neighbors Neighbour list span.
   * @param per_vertex_slots Slots to reserve for this vertex only.
   * @return true on success, false if the page is full.
   */
  bool add_vertex(v_id_t vertex_id, neighbor_span_t& neighbors, uint16_t per_vertex_slots);

  /**
   * @brief Check if the builder is empty.
   * @return true if no vertices have been added, false otherwise
   */
  bool is_empty();

  /**
   * @brief Estimate the size needed for the current page data.
   * @return Estimated size in bytes
   */
  size_t estimated_size();

  /**
   * @brief Build the CSR page with current vertices and edges
   * @return A pair containing the built csr_page_t and the vertex map
   */
  std::pair<csr_page_t*, std::vector<std::pair<v_id_t, uint16_t>>> build();

  /**
   * @brief Clear all data in the builder.
   */
  void clear_all();
};

#endif // BW_GRAPH_IO_BUILDER_H