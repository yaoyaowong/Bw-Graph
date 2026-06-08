#ifndef BW_GRAPH_MEM_GRAPH_H
#define BW_GRAPH_MEM_GRAPH_H

#include "bw_graph/common/type.h"

#include <vector>

/**
 * @brief Abstract base class defining the graph interface
 *
 * This class serves as a trait-like interface for different graph
 * implementations, providing common operations that all graph types must
 * support.
 */
class mem_graph_i {
public:
  virtual ~mem_graph_i() = default;

  /**
   * @brief Get the neighbor list of a given vertex
   * @param vertex_id The ID of the vertex whose neighbors to retrieve
   * @return A vector containing the IDs of all neighboring vertices
   * @throws std::invalid_argument if the vertex does not exist
   */
  virtual std::vector<v_id_t> get_neighbors(v_id_t vertex_id) const = 0;

  /**
   * @brief Check if a vertex exists in the graph
   * @param vertex_id The ID of the vertex to check
   * @return True if the vertex exists, false otherwise
   */
  virtual bool has_vertex(v_id_t vertex_id) const = 0;

  /**
   * @brief Check if an edge exists between two vertices
   * @param src_id The ID of the source vertex
   * @param dst_id The ID of the destination vertex
   * @return True if the edge exists, false otherwise
   */
  virtual bool has_edge(v_id_t src_id, v_id_t dst_id) const = 0;

  /**
   * @brief Get a list of all vertices in the graph
   * @return A vector containing the IDs of all vertices
   */
  virtual std::vector<v_id_t> vertex_list() const = 0;

  /**
   * @brief Convert the graph to an adjacency map representation
   * @return An unordered_map where keys are vertex IDs and values are neighbor
   * lists
   */
  virtual folly::F14FastMap<v_id_t, std::vector<v_id_t>> to_map() const = 0;

  /**
   * @brief Get the number of vertices in the graph
   * @return The total number of vertices
   */
  virtual size_t vertex_count() const = 0;

  /**
   * @brief Get the number of edges in the graph
   * @return The total number of edges
   */
  virtual size_t edge_count() const = 0;
};

#endif // BW_GRAPH_MEM_GRAPH_H