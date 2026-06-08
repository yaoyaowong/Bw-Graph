#ifndef BW_GRAPH_MEM_SUB_MAP_H
#define BW_GRAPH_MEM_SUB_MAP_H

#include "bw_graph/io/io_csr.h"
#include "bw_graph/mem/graph.h"

#include <vector>

class csr_page_t;

/**
 * @brief HashMap-based implementation of the graph interface with vector
 * neighbors
 *
 * This class implements a graph representation using unordered_map for storing
 * adjacency lists. This format is particularly suitable for dynamic graphs
 * where vertices and edges are frequently added or removed. The implementation
 * consists of:
 * - adjacency_map: Maps vertex IDs to their neighbor lists stored as vector
 * for fast iteration and better cache locality
 * - total_edges: Cached count of total edges for efficient edge_count()
 * operation
 *
 * Trade-offs:
 * - Faster neighbor iteration due to vector's contiguous memory layout
 * - Edge insertion/deletion is O(n) where n is the degree of the vertex
 * - Better cache performance for graph traversal algorithms
 */
class mem_sub_map_t : public mem_graph_i {
private:
  /**
   * @brief Adjacency map storing vertex ID to neighbor vector mapping
   */
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> adjacency_map;

  /**
   * @brief Cached total number of edges in the graph
   */
  mutable size_t total_edges;

  /**
   * @brief Flag indicating if edge count cache is valid
   */
  mutable bool edge_count_valid;

  /**
   * @brief Recalculate and cache the total number of edges
   */
  void update_edge_count() const;

  /**
   * @brief Helper function to check if a neighbor exists in a vertex's neighbor
   * list
   * @param neighbors The neighbor vector to search in
   * @param target_id The vertex ID to search for
   * @return True if the neighbor exists, false otherwise
   */
  bool has_neighbor(const std::vector<v_id_t>& neighbors, v_id_t target_id) const;

  /**
   * @brief Helper function to remove a neighbor from a vertex's neighbor list
   * @param neighbors The neighbor vector to remove from
   * @param target_id The vertex ID to remove
   * @return True if the neighbor was found and removed, false otherwise
   */
  bool remove_neighbor(std::vector<v_id_t>& neighbors, v_id_t target_id);

public:
  /**
   * @brief Default constructor creates an empty map-based graph
   */
  mem_sub_map_t();

  /**
   * @brief Compute degree distribution of the graph
   * @param step The step size for grouping degrees (default: 10)
   * @return A vector of pairs where each pair contains (degree_range_start,
   * percentage)
   *
   * This method computes the degree distribution by grouping vertices into
   * degree ranges. For example, with step=10, it returns percentages for
   * degrees [0-9], [10-19], [20-29], etc. The percentage indicates what
   * fraction of vertices fall into each degree range.
   */
  std::vector<std::pair<uint64_t, double>> compute_degree_distribution(uint64_t step = 10) const;

  /**
   * @brief Constructor that builds map-based graph from an adjacency map
   * @param adj_map Adjacency map where keys are vertex IDs and values are
   * neighbor lists
   */
  explicit mem_sub_map_t(const folly::F14FastMap<v_id_t, std::vector<v_id_t>>& adj_map);

  /**
   * @brief Destructor
   */
  virtual ~mem_sub_map_t() = default;

  /**
   * @brief Create a mem_sub_map_t instance from a csr_page_t object
   * @param page The csr_page_t object to convert
   * @return A mem_sub_map_t instance representing the same graph data
   */
  static mem_sub_map_t from_csr_page(csr_page_t& page);

  /**
   * @brief Get the neighbor list of a given vertex
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
   * @return An unordered_map where keys are vertex IDs and values are neighbor
   * lists
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

  /**
   * @brief Add a vertex to the graph
   * @param vertex_id The ID of the vertex to add
   * @return True if the vertex was added, false if it already existed
   */
  bool add_vertex(v_id_t vertex_id);

  /**
   * @brief Add an edge between two vertices
   * @param src_id The ID of the source vertex
   * @param dst_id The ID of the destination vertex
   * @return True if the edge was added, false if it already existed
   * @note This method will automatically add vertices if they don't exist
   */
  bool add_edge(v_id_t src_id, v_id_t dst_id);

  /**
   * @brief Remove a vertex and all its associated edges
   * @param vertex_id The ID of the vertex to remove
   * @return True if the vertex was removed, false if it didn't exist
   */
  bool remove_vertex(v_id_t vertex_id);

  /**
   * @brief Remove an edge between two vertices
   * @param src_id The ID of the source vertex
   * @param dst_id The ID of the destination vertex
   * @return True if the edge was removed, false if it didn't exist
   */
  bool remove_edge(v_id_t src_id, v_id_t dst_id);

  /**
   * @brief Clear all vertices and edges from the graph
   */
  void clear();

  /**
   * @brief Get direct reference to neighbor vector (for performance-critical
   * operations)
   * @param vertex_id The ID of the vertex whose neighbors to retrieve
   * @return Const reference to the neighbor vector
   * @throws std::invalid_argument if the vertex does not exist
   */
  const std::vector<v_id_t>& get_neighbors_ref(v_id_t vertex_id) const;
};

#endif // BW_GRAPH_MEM_SUB_MAP_H