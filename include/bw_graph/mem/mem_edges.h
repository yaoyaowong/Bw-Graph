#ifndef BW_GRAPH_MEM_EDGES_H
#define BW_GRAPH_MEM_EDGES_H

#include "bw_graph/common/type.h"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Insertion mode for edge operations
 */
enum class insert_mode_t {
  SEQUENTIAL = 0, // Insert edges ordered by source vertex ID
  RANDOM = 1,     // Insert edges in random order
  DEFAULT = 2,    // Insert edges in original order
};

/**
 * @brief Edge list management structure for storing and processing graph edges
 *
 * This class provides efficient storage and parallel processing capabilities
 * for edge lists. It supports loading edges from files, storing them in memory,
 * and applying custom operations in parallel.
 */
class mem_edge_list_t {
private:
  /**
   * @brief Name of the graph
   */
  std::string graph_name;

  /**
   * @brief Path to the edge list file
   */
  std::string file_path;

  /**
   * @brief Vector storing all edges as (source, destination) pairs
   */
  std::vector<std::pair<v_id_t, v_id_t>> edges;

  /**
   * @brief Buffer size for file I/O operations
   */
  size_t buffer_size;

public:
  /**
   * @brief Default constructor creates an empty edge list
   */
  mem_edge_list_t();

  /**
   * @brief Constructor with graph name
   * @param graph_name Name of the graph (file will be at
   * data/{graph_name}.delta)
   * @param buffer_size Buffer size for file I/O (default: 64KB)
   */
  explicit mem_edge_list_t(const std::string& graph_name, size_t buffer_size = 64 * 1024);

  /**
   * @brief Constructor with custom file path
   * @param graph_name Name of the graph
   * @param file_path Custom path to the edge list file
   * @param buffer_size Buffer size for file I/O (default: 64KB)
   */
  mem_edge_list_t(const std::string& graph_name, const std::string& file_path,
                  size_t buffer_size = 64 * 1024);

  /**
   * @brief Destructor
   */
  ~mem_edge_list_t() = default;

  /**
   * @brief Generate a workload for SMO testing
   * @param num_vertices Number of vertices in the graph
   * @param edges_per_vertex Number of edges to generate per vertex
   * @param seed Random seed for reproducibility (default: random)
   * @return A mem_edge_list_t instance containing the generated edges
   *
   * This function generates a synthetic workload where each vertex has
   * a fixed number of randomly generated outgoing edges. Useful for
   * testing SMO (Structure Modification Operations) behavior under
   * controlled conditions.
   */
  static mem_edge_list_t generate_workload_for_smo(std::string graph_name, v_id_t num_vertices,
                                                   size_t edges_per_vertex, uint64_t seed = 0);

  /**
   * @brief Load edges from file into memory
   * @return True if loading succeeded, false otherwise
   */
  bool load_from_file();

  /**
   * @brief Load edges from file with custom file path
   * @param custom_path Custom path to the edge list file
   * @return True if loading succeeded, false otherwise
   */
  bool load_from_file(const std::string& custom_path);

  /**
   * @brief Get the number of edges stored
   * @return Number of edges
   */
  size_t edge_count() const;

  /**
   * @brief Get the graph name
   * @return Graph name string
   */
  const std::string& get_graph_name() const;

  /**
   * @brief Get the file path
   * @return File path string
   */
  const std::string& get_file_path() const;

  /**
   * @brief Get reference to the edge list
   * @return Const reference to the vector of edges
   */
  const std::vector<std::pair<v_id_t, v_id_t>>& get_edges() const;

  /**
   * @brief Clear all stored edges
   */
  void clear();

  /**
   * @brief Add a single edge to the list
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   */
  void add_edge(v_id_t src, v_id_t dst);

  /**
   * @brief Apply a custom operation to all edges sequentially
   * @param operation Function to apply to each edge (src, dst)
   * @param mode Insertion mode (SEQUENTIAL or RANDOM)
   *
   * Example usage:
   * edge_list.apply_sequential([&](v_id_t src, v_id_t dst) {
   *     db.insert(src, dst);
   * }, InsertionMode::SEQUENTIAL);
   */
  void apply_sequential(const std::function<void(v_id_t, v_id_t)>& operation,
                        insert_mode_t mode = insert_mode_t::RANDOM, bool is_shuffle = false) const;

  /**
   * @brief Apply a custom operation to all edges in parallel
   * @param operation Function to apply to each edge (src, dst)
   * @param num_threads Number of threads to use (default: hardware concurrency)
   * @param mode Insertion mode (SEQUENTIAL or RANDOM)
   *
   * For SEQUENTIAL mode, edges are sorted by source ID and partitioned among
   * threads, ensuring each thread processes edges in order.
   *
   * Example usage:
   * edge_list.apply_parallel([&](v_id_t src, v_id_t dst) {
   *     db.insert(src, dst);
   * }, 8, InsertionMode::SEQUENTIAL);
   */
  void apply_parallel(const std::function<void(v_id_t, v_id_t)>& operation, int num_threads = 0,
                      insert_mode_t mode = insert_mode_t::RANDOM) const;

  /**
   * @brief Apply a custom operation to a range of edges
   * @param operation Function to apply to each edge (src, dst)
   * @param start_idx Starting index (inclusive)
   * @param end_idx Ending index (exclusive)
   *
   * Example usage:
   * edge_list.apply_range([&](v_id_t src, v_id_t dst) {
   *     db.insert(src, dst);
   * }, 0, 1000);
   */
  void apply_range(const std::function<void(v_id_t, v_id_t)>& operation, size_t start_idx,
                   size_t end_idx) const;

  /**
   * @brief Apply a custom operation to all edges at a constant rate
   * (single-threaded)
   * @param operation Function to apply to each edge (src, dst)
   * @param edges_per_second Target processing rate (edges per second, default:
   * 1000)
   *
   * This method processes edges sequentially at a constant rate, sleeping
   * between edges to maintain the target throughput. Useful for simulating
   * real-time edge arrival or rate-limited processing.
   *
   * Example usage:
   * edge_list.apply_at_rate([&](v_id_t src, v_id_t dst) {
   *     db.insert(src, dst);
   * }, 5000); // Process 5000 edges per second
   */
  void apply_at_rate(const std::function<void(v_id_t, v_id_t)>& operation,
                     size_t edges_per_second = 1000) const;

  /**
   * @brief Transform edge list to follow Zipfian distribution
   * @param alpha Zipfian parameter (skewness factor, typical range: 0.5-2.0)
   *              Higher values = more skewed (more edges concentrated on fewer
   * sources) alpha = 0: uniform distribution alpha = 1: standard Zipfian alpha
   * > 1: highly skewed
   * @param seed Random seed for reproducibility (default: random)
   * @param max_edges_per_source Per-source update cap. 0 means uncapped.
   *
   * This transforms the edge list so that source vertices follow a Zipfian
   * distribution, meaning some source vertices will have many more edges than
   * others. The transformation:
   * 1. Identifies all unique source vertices
   * 2. Ranks them according to Zipfian distribution
   * 3. Redistributes edges to follow the skewed pattern
   * 4. Randomizes the order of edges
   *
   * Example usage:
   * edge_list.to_zipfian(0.8, 0, 10000); // Skewed distribution with capped hot sources
   */
  void to_zipfian(double alpha, uint64_t seed = 0, size_t max_edges_per_source = 0);

  /**
   * @brief Get statistics about the edge list
   * @return String containing edge list statistics
   */
  std::string get_statistics() const;
};

#endif // BW_GRAPH_MEM_EDGES_H
