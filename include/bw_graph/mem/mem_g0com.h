#ifndef BW_GRAPH_MEM_SUB_COM_H
#define BW_GRAPH_MEM_SUB_COM_H

#include "bw_graph/common/type.h"
#include "bw_graph/mem/graph.h"
#include "bw_graph/mem/mem_g0map.h"

#include <cstddef>
#include <cstdint>
#include <folly/container/F14Set.h>
#include <memory>
#include <string>
#include <vector>

// Forward declaration
class mem_sub_csr_t;

/**
 * @brief CSR (Compressed Sparse Row) implementation with contiguous vertex IDs
 *
 * This class implements a memory-efficient graph representation using the CSR
 * format optimized for graphs with contiguous vertex IDs (0, 1, 2, ..., n-1).
 * The CSR format consists of:
 * - offsets: Array containing the starting position of each vertex's neighbors
 * in the neighbor_list array
 * - neighbor_list: Array containing all neighbors of all vertices in a
 * contiguous layout
 * - communities: Array containing the community ID for each vertex
 *
 * Since vertex IDs are contiguous, no vertex_index mapping is needed, allowing
 * direct array access for O(1) vertex operations.
 */
class mem_sub_com_t : public mem_graph_i {
public:
  /**
   * @brief Starting positions of neighbor lists in the neighbor_list array
   */
  std::vector<size_t> offsets;

  /**
   * @brief Array containing all neighbors of all vertices
   */
  std::vector<v_id_t> neighbor_list;

  /**
   * @brief Community assignment for each vertex
   */
  std::vector<v_id_t> communities;

  /**
   * @brief Community split of the graph
   */
  community_structure_t community_structure;

  /**
   * @brief Number of vertices in the graph
   */
  size_t num_vertices;

  /**
   * @brief Validate that a vertex ID is within the valid range
   * @param vertex_id The vertex ID to validate
   * @return True if the vertex ID is valid, false otherwise
   */
  bool is_valid_vertex(v_id_t vertex_id) const;

public:
  /**
   * @brief Default constructor creates an empty CSR graph
   */
  mem_sub_com_t();

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
   * @brief Sample a random walk from the graph
   * @param start_vertex The starting vertex for the random walk
   * @param vertex_count The number of vertices to sample
   * @return A map containing the sampled vertices and their neighbors
   */
  std::shared_ptr<mem_sub_map_t> graph_sample_random_walk(v_id_t start_vertex,
                                                          uint64_t vertex_count = 0);

  /**
   * @brief Sample a BFS from the graph
   * @param start_vertex The starting vertex for the BFS
   * @param vertex_count The number of vertices to sample
   * @return A map containing the sampled vertices and their neighbors
   */
  std::shared_ptr<mem_sub_map_t> graph_sample_bfs(v_id_t start_vertex, uint64_t vertex_count);

  /**
   * @brief Constructor that builds CSR from an adjacency map
   * @param adj_map Adjacency map where keys are vertex IDs and values are
   * neighbor lists
   * @param community_map Map from vertex ID to community ID
   */
  explicit mem_sub_com_t(const folly::F14FastMap<v_id_t, std::vector<v_id_t>>& adj_map,
                         const folly::F14FastMap<v_id_t, v_id_t>& community_map = {});

  /**
   * @brief Constructor that builds CSR from vectors directly
   * @param offsets_vec Offset array for CSR format
   * @param neighbor_list_vec Neighbor list array for CSR format
   * @param communities_vec Community assignment for each vertex
   */
  mem_sub_com_t(const std::vector<size_t>& offsets_vec,
                const std::vector<v_id_t>& neighbor_list_vec,
                const std::vector<v_id_t>& communities_vec);

  /**
   * @brief Constructor that loads CSR from a file
   * @param filename Path to the input file
   * @param has_communities Whether the file contains community information
   * @param buffer_size Buffer size for file I/O
   * @param deduplication_handle Whether to handle deduplication of edges
   */
  explicit mem_sub_com_t(const std::string& filename, bool has_communities = true,
                         size_t buffer_size = 64 * 1024, bool deduplication_handle = true);

  /**
   * @brief Destructor
   */
  virtual ~mem_sub_com_t() = default;

  /**
   * @brief Get the neighbor list of a given vertex
   * @param vertex_id The ID of the vertex whose neighbors to retrieve
   * @return A vector containing the IDs of all neighboring vertices
   * @throws std::invalid_argument if the vertex does not exist
   */
  std::vector<v_id_t> get_neighbors(v_id_t vertex_id) const override;

  /**
   * @brief Get the neighbor list of a given vertex as a span
   * @param vertex_id The ID of the vertex whose neighbors to retrieve
   * @return A span containing the IDs of all neighboring vertices
   */
  neighbor_span_t get_neighbors_span(v_id_t vertex_id);

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
   * @brief Generate the zero copy version vertex
   * @param induced_vertex Induced vertex list
   * @return A view of subgraph
   */
  folly::F14FastMap<v_id_t, neighbor_span_t>
  induce_subgraph_iter(std::vector<v_id_t> induced_vertex);

  /**
   * @brief Generate the zero copy version vertex
   * @param vertex_set Induced vertex list
   * @return A view of subgraph
   */
  folly::F14FastMap<v_id_t, std::vector<v_id_t>>
  induce_subgraph_filter(folly::F14FastSet<v_id_t>& vertex_set);

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
   * @brief Get the community ID of a given vertex
   * @param vertex_id The ID of the vertex whose community to retrieve
   * @return The community ID of the vertex
   * @throws std::invalid_argument if the vertex does not exist
   */
  v_id_t get_community_id(v_id_t vertex_id) const;

  /**
   * @brief Get all vertices belonging to a specific community
   * @param community_id The ID of the community
   * @return A vector containing the IDs of all vertices in the community
   */
  std::vector<v_id_t> get_vertices_in_community(comm_id_t community_id) const;

  /**
   * @brief Get the community assignment for all vertices
   * @return A vector where index i contains the community ID of vertex i
   */
  const std::vector<comm_id_t>& get_communities() const;

  /**
   * @brief Compute induced subgraph from given vertex list
   * @param vertex_subset List of vertex IDs to include in the subgraph
   * @return A shared pointer to the induced subgraph in original CSR format
   * @throws std::invalid_argument if any vertex in the subset does not exist
   *
   * The induced subgraph contains all vertices from vertex_subset and all edges
   * between vertices in the subset. The resulting graph uses the original
   * CSR format with vertex_index mapping since vertex IDs may not be
   * contiguous.
   */
  std::shared_ptr<mem_sub_csr_t>
  compute_induced_subgraph(const std::vector<v_id_t>& vertex_subset) const;

  /**
   * @brief Compute induced subgraph from given vertex list
   * @param vertex_subset List of vertex IDs to include in the subgraph
   * @return A shared pointer to the induced subgraph in original CSR format
   * @throws std::invalid_argument if any vertex in the subset does not exist
   *
   * The induced subgraph contains all vertices from vertex_subset and all edges
   * between vertices in the subset. The resulting graph uses the original
   * CSR format with vertex_index mapping since vertex IDs may not be
   * contiguous.
   */
  adj_map_t compute_induced_subgraph_map(std::vector<v_id_t>& vertex_subset,
                                         bool is_pruning = false) const;

  /**
   * @brief Compute induced subgraph for the entire graph
   * @return An adjacency map representing the induced subgraph
   */
  adj_map_t compute_induced_graph_map() const;

  /**
   * @brief Compute induced subgraph for a specific community
   * @param community_id The ID of the community
   * @return A shared pointer to the induced subgraph containing all vertices in
   * the community
   */
  std::shared_ptr<mem_sub_csr_t> compute_community_subgraph(comm_id_t community_id) const;

  /**
   * @brief Get a reference to the community structure
   * @return A reference to the community_structure_t object
   */
  community_structure_t& get_community_structure_ref() { return community_structure; }

  /**
   * @brief Get statistics about the graph structure
   * @return A string containing various graph statistics
   */
  std::string get_graph_statistics() const;

  /**
   * @brief Get statistics about community structure
   * @return A string containing community-related statistics
   */
  std::string get_community_statistics() const;

  /**
   * @brief Perform BFS starting from a given vertex
   * @param start_vertex The ID of the vertex to start the BFS from
   * @return A vector containing the IDs of all vertices reached during the BFS
   */
  std::vector<std::pair<v_id_t, uint64_t>> bfs(v_id_t start_vertex);

  /**
   * @brief Perform parallel BFS starting from a given vertex
   * @param start_vertex The ID of the vertex to start the BFS from
   * @return A vector containing the IDs of all vertices reached during the BFS
   */
  std::vector<std::pair<v_id_t, uint64_t>> bfs_parallel(v_id_t start_vertex);
};

#endif // BW_GRAPH_MEM_SUB_COM_H