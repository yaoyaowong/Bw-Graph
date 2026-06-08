#ifndef BW_GRAPH_MEM_SUB_CSR_H
#define BW_GRAPH_MEM_SUB_CSR_H

#include "bw_graph/mem/graph.h"

#include <vector>

/**
 * @brief CSR (Compressed Sparse Row) implementation of the graph interface
 *
 * This class implements a memory-efficient graph representation using the CSR
 * format, which is particularly suitable for sparse graphs. The CSR format
 * consists of:
 * - vertex_index: Maps vertex IDs to their positions in the offsets array
 * - offsets: Array containing the starting position of each vertex's neighbors
 * in the neighbor_list array
 * - neighbor_list: Array containing all neighbors of all vertices in a
 * contiguous layout
 */
class mem_sub_csr_t : public mem_graph_i {
private:
  /**
   * @brief Maps vertex ID to offsets array index
   */
  folly::F14FastMap<v_id_t, size_t> vertex_index;

  /**
   * @brief Starting positions of neighbor_list lists
   */
  std::vector<size_t> offsets;

  /**
   * @brief Total number of edges in the graph
   */
  std::vector<v_id_t> neighbor_list;

public:
  /**
   * @brief Default constructor creates an empty CSR graph
   */
  mem_sub_csr_t();

  /**
   * @brief Constructor that builds CSR from an adjacency map
   * @param adj_map Adjacency map where keys are vertex IDs and values are
   * neighbor lists
   */
  explicit mem_sub_csr_t(const folly::F14FastMap<v_id_t, std::vector<v_id_t>>& adj_map);

  /**
   * @brief Destructor
   */
  virtual ~mem_sub_csr_t() = default;

  /**
   * @brief Get the neighbor_list list of a given vertex
   * @param vertex_id The ID of the vertex whose neighbors to retrieve
   * @return A vector containing the IDs of all neighboring vertices
   * @throws std::invalid_argument if the vertex does not exist
   */
  std::vector<v_id_t> get_neighbors(v_id_t vertex_id) const override;

  /**
   * @brief Check if a vertex exists in the graph
   * @param vertex_id The ID of the vertex to check
   * @return True if the vertex exists, false otherwise
   */
  bool has_vertex(v_id_t vertex_id) const override;

  /**
   * @brief Check if an edge exists between two vertices
   * @param src_id The ID of the source vertex
   * @param dst_id The ID of the destination vertex
   * @return True if the edge exists, false otherwise
   */
  bool has_edge(v_id_t src_id, v_id_t dst_id) const override;

  /**
   * @brief Get a list of all vertices in the graph
   * @return A vector containing the IDs of all vertices
   */
  std::vector<v_id_t> vertex_list() const override;

  /**
   * @brief Convert the graph to an adjacency map representation
   * @return An unordered_map where keys are vertex IDs and values are
   * neighbor_list lists
   */
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> to_map() const override;

  /**
   * @brief Get the number of vertices in the graph
   * @return The total number of vertices
   */
  size_t vertex_count() const override;

  /**
   * @brief Get the number of edges in the graph
   * @return The total number of edges
   */
  size_t edge_count() const override;
};

#endif // BW_GRAPH_MEM_SUB_CSR_H