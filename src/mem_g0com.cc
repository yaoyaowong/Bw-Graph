#include "bw_graph/mem/mem_g0com.h"

#include "bw_graph/common/type.h"
#include "bw_graph/mem/mem_g0csr.h"

#include <algorithm>
#include <cstddef>
#include <fcntl.h>
#include <folly/container/F14Set.h>
#include <fstream>
#include <indicators/progress_bar.hpp>
#include <iostream>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_vector.h>
#include <tbb/info.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <unistd.h>
#include <unordered_set>

// Constructor
mem_sub_com_t::mem_sub_com_t() : num_vertices(0) {}

// Constructor from adjacency map
mem_sub_com_t::mem_sub_com_t(const folly::F14FastMap<v_id_t, std::vector<v_id_t>>& adj_map,
                             const folly::F14FastMap<v_id_t, v_id_t>& community_map) {
  if (adj_map.empty()) {
    num_vertices = 0;
    return;
  }

  // Find the maximum vertex ID to determine the number of vertices
  v_id_t max_vertex = 0;
  for (const auto& pair : adj_map) {
    max_vertex = std::max(max_vertex, pair.first);
    for (v_id_t neighbor : pair.second) {
      max_vertex = std::max(max_vertex, neighbor);
    }
  }

  num_vertices = max_vertex + 1;

  // Initialize communities array (default to vertex ID if not specified)
  communities.resize(num_vertices);
  for (size_t i = 0; i < num_vertices; ++i) {
    auto it = community_map.find(static_cast<v_id_t>(i));
    communities[i] = (it != community_map.end()) ? it->second : static_cast<v_id_t>(i);
  }

  // Build CSR format
  offsets.resize(num_vertices + 1, 0);

  // First pass: count neighbors for each vertex
  for (const auto& pair : adj_map) {
    offsets[pair.first + 1] = pair.second.size();
  }

  // Convert counts to offsets
  for (size_t i = 1; i <= num_vertices; ++i) {
    offsets[i] += offsets[i - 1];
  }

  // Allocate neighbor list
  neighbor_list.resize(offsets[num_vertices]);

  // Second pass: fill neighbor list
  for (const auto& pair : adj_map) {
    v_id_t vertex = pair.first;
    const auto& neighbors = pair.second;

    // Copy and sort neighbors
    std::copy(neighbors.begin(), neighbors.end(), neighbor_list.begin() + offsets[vertex]);
    std::sort(neighbor_list.begin() + offsets[vertex], neighbor_list.begin() + offsets[vertex + 1]);
  }
}

// Constructor from vectors
mem_sub_com_t::mem_sub_com_t(const std::vector<size_t>& offsets_vec,
                             const std::vector<v_id_t>& neighbor_list_vec,
                             const std::vector<v_id_t>& communities_vec)
    : offsets(offsets_vec), neighbor_list(neighbor_list_vec), communities(communities_vec) {
  num_vertices = offsets.empty() ? 0 : offsets.size() - 1;
}

/**
 * @brief Constructor that builds CSR from a file with minimal memory usage
 * (Undirected Graph) - Single-pass construction with oneTBB arena
 * @param filename Path to the input file
 * @param has_communities Whether the file contains community information
 * @param buffer_size Buffer size for file I/O
 * @throws std::runtime_error if the file cannot be opened or parsed
 *
 * @note Assumes vertex IDs are consecutive: [0, max_vertex_id]
 */
mem_sub_com_t::mem_sub_com_t(const std::string& filename, bool has_communities, size_t buffer_size,
                             bool deduplication_handle) {
  // Configuration Parameters
  const size_t BUFFER_SIZE = buffer_size;
  const size_t MAX_VERTICES = 1000000000;

  // Single Pass: Build Graph Directly
  {
    std::ifstream file(filename);
    if (!file.is_open()) {
      throw std::runtime_error("Cannot open file: " + filename);
    }

// Platform-Specific Optimizations
#if defined(__linux__)
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd != -1) {
      std::cout << "Enabling readahead on Linux for file: " << filename << std::endl;
      posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
      posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
      close(fd);
    }
#elif defined(__APPLE__)
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd != -1) {
      std::cout << "Enabling readahead on macOS for file: " << filename << std::endl;
      fcntl(fd, F_RDAHEAD, 1);

      struct radvisory {
        off_t ra_offset;
        int ra_count;
      } ra;
      ra.ra_offset = 0;
      ra.ra_count = buffer_size;
      fcntl(fd, F_RDADVISE, &ra);

      close(fd);
    }
#endif

    // Set large buffer for batched disk reads
    std::vector<char> io_buffer(BUFFER_SIZE);
    file.rdbuf()->pubsetbuf(io_buffer.data(), BUFFER_SIZE);

    // Use vector<vector> for maximum insertion speed
    std::vector<std::vector<v_id_t>> adj_list;
    std::vector<comm_id_t> temp_communities;

    v_id_t max_vertex_id = 0;
    comm_id_t max_community_id = 0;
    size_t num_v_from_header = 0;
    bool header_parsed = false;
    bool graph_allocated = false;
    std::string line;

    // Progress Bar for File Loading

    // Get file size for progress calculation
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    indicators::ProgressBar loading_bar{
        indicators::option::BarWidth{50},
        // Start start.
        indicators::option::Start{"["}, indicators::option::Fill{"="},
        indicators::option::Lead{">"}, indicators::option::Remainder{" "},
        indicators::option::End{"]"}, indicators::option::PostfixText{"Loading graph from file"},
        indicators::option::ForegroundColor{indicators::Color::cyan},
        indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
        indicators::option::ShowRemainingTime{true}, indicators::option::MaxProgress{file_size}};

    size_t bytes_read = 0;
    size_t last_update = 0;
    const size_t UPDATE_INTERVAL = file_size / 1000; // Update every 0.1%
    std::cout << "Graph Loading..." << std::endl;

    while (std::getline(file, line)) {
      bytes_read += line.size() + 1; // +1 for newline

      // Update progress bar periodically
      if (bytes_read - last_update > UPDATE_INTERVAL) {
        loading_bar.set_progress(bytes_read);
        last_update = bytes_read;
      }

      // Skip empty lines and comments
      if (line.empty() || line[0] == '#')
        continue;

      const char* ptr = line.c_str();
      char type = *ptr++;

      // Skip whitespace after type
      while (*ptr == ' ' || *ptr == '\t')
        ++ptr;

      if (type == 't') {
        // Parse header: t num_vertices num_edges
        size_t num_v = 0, num_e = 0;
        while (*ptr >= '0' && *ptr <= '9')
          num_v = num_v * 10 + (*ptr++ - '0');
        while (*ptr == ' ' || *ptr == '\t')
          ++ptr;
        while (*ptr >= '0' && *ptr <= '9')
          num_e = num_e * 10 + (*ptr++ - '0');

        if (num_v > MAX_VERTICES) {
          throw std::runtime_error("Graph size exceeds limits");
        }

        num_v_from_header = num_v;
        header_parsed = true;

        // Allocate exactly once based on header
        adj_list.resize(num_v);
        temp_communities.resize(num_v, 0);
        graph_allocated = true;

        // Pre-reserve space for each adjacency list based on average degree
        // Assume average degree of 20 (adjust based on your graph)
        size_t avg_degree = 20;
        for (auto& neighbors : adj_list) {
          neighbors.reserve(avg_degree);
        }

        std::cout << "\nHeader parsed: " << num_v << " vertices, " << num_e << " edges expected"
                  << std::endl;

      } else if (type == 'v') {
        // Parse vertex: v vertex_id label community_id
        v_id_t vertex_id = 0;
        while (*ptr >= '0' && *ptr <= '9')
          vertex_id = vertex_id * 10 + (*ptr++ - '0');

        while (*ptr == ' ' || *ptr == '\t')
          ++ptr;
        while (*ptr >= '0' && *ptr <= '9')
          ++ptr; // Skip label

        while (*ptr == ' ' || *ptr == '\t')
          ++ptr;
        comm_id_t community_id = 0;
        while (*ptr >= '0' && *ptr <= '9')
          community_id = community_id * 10 + (*ptr++ - '0');

        if (vertex_id > MAX_VERTICES) {
          throw std::runtime_error("Vertex ID exceeds limits");
        }

        // Track max_vertex_id for allocation (if no header)
        if (!graph_allocated) {
          max_vertex_id = std::max(max_vertex_id, vertex_id);
        }

        max_community_id = std::max(max_community_id, community_id);

        // Dynamic allocation if no header
        if (!graph_allocated && vertex_id >= adj_list.size()) {
          size_t estimated_size = vertex_id + 1000000;
          estimated_size = std::min(estimated_size, MAX_VERTICES);
          adj_list.resize(estimated_size);
          temp_communities.resize(estimated_size, 0);
        }

        if (has_communities && vertex_id < adj_list.size()) {
          temp_communities[vertex_id] = community_id;
        }

      } else if (type == 'e') {
        // Parse edge: e src_vertex dst_vertex
        v_id_t src = 0, dst = 0;
        while (*ptr >= '0' && *ptr <= '9')
          src = src * 10 + (*ptr++ - '0');
        while (*ptr == ' ' || *ptr == '\t')
          ++ptr;
        while (*ptr >= '0' && *ptr <= '9')
          dst = dst * 10 + (*ptr++ - '0');

        if (src > MAX_VERTICES || dst > MAX_VERTICES) {
          throw std::runtime_error("Edge vertex ID exceeds limits");
        }

        // Track max_vertex_id for final allocation
        if (!graph_allocated) {
          max_vertex_id = std::max(max_vertex_id, std::max(src, dst));
        }

        // Dynamic allocation if no header
        size_t required_size = std::max(src, dst) + 1;
        if (!graph_allocated && required_size > adj_list.size()) {
          size_t estimated_size = required_size + 1000000;
          estimated_size = std::min(estimated_size, MAX_VERTICES);
          adj_list.resize(estimated_size);
          temp_communities.resize(estimated_size, 0);
        }

        // Direct push_back - fastest insertion method
        // Duplicates will be removed later
        if (src < adj_list.size() && dst < adj_list.size()) {
          if (src != dst) {
            // Undirected graph: add both directions
            adj_list[src].push_back(dst);
            adj_list[dst].push_back(src);
          }
        }
      }
    }

    // Complete the loading progress bar
    loading_bar.set_progress(file_size);
    loading_bar.mark_as_completed();
    std::cout << "Graph Loading...[OK]" << std::endl;

    file.close();

    // Finalize vertex count and resize to exact size
    if (header_parsed) {
      num_vertices = num_v_from_header;
    } else {
      num_vertices = max_vertex_id + 1;
    }

    adj_list.resize(num_vertices);
    temp_communities.resize(num_vertices, 0);

    std::cout << "Graph loaded: " << num_vertices << " vertices" << std::endl;
    std::cout << "Sorting and removing duplicates..." << std::endl;

    // Sort and Remove Duplicates with TBB Arena
    size_t total_edges_before = 0;
    for (const auto& neighbors : adj_list) {
      total_edges_before += neighbors.size();
    }
    std::cout << "Total edges before dedup: " << total_edges_before << std::endl;

    // Create arena with maximum available threads
    int max_threads = tbb::info::default_concurrency();
    std::cout << "Using " << max_threads << " threads for deduplication" << std::endl;
    if (deduplication_handle) {
      // Handle deduplication
      {
        std::atomic<size_t> processed_count{0};

        // Progress Bar for Deduplication
        indicators::ProgressBar dedup_bar{
            indicators::option::BarWidth{50},
            // Start start.
            indicators::option::Start{"["}, indicators::option::Fill{"="},
            indicators::option::Lead{">"}, indicators::option::Remainder{" "},
            indicators::option::End{"]"}, indicators::option::PostfixText{"Deduplicating edges"},
            indicators::option::ForegroundColor{indicators::Color::green},
            indicators::option::ShowPercentage{true}, indicators::option::ShowElapsedTime{true},
            indicators::option::ShowRemainingTime{true},
            indicators::option::MaxProgress{num_vertices}};

        std::mutex bar_mutex; // Protect progress bar updates

        // Create task_arena with max threads - threads released when arena
        // destroyed
        tbb::task_arena arena(max_threads);

        arena.execute([&] {
          tbb::parallel_for(
              tbb::blocked_range<size_t>(0, num_vertices, 50),
              [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                  if (!adj_list[i].empty()) {
                    // Sort neighbors
                    std::sort(adj_list[i].begin(), adj_list[i].end());

                    // Remove duplicates
                    auto last = std::unique(adj_list[i].begin(), adj_list[i].end());
                    adj_list[i].erase(last, adj_list[i].end());

                    // Shrink to fit to release excess memory
                    adj_list[i].shrink_to_fit();
                  }

                  // Update progress bar (thread-safe)
                  size_t count = processed_count.fetch_add(1) + 1;

                  // Update progress bar every 1000 vertices for performance
                  if (count % 1000 == 0 || count == num_vertices) {
                    std::lock_guard<std::mutex> lock(bar_mutex);
                    dedup_bar.set_progress(count);
                  }
                }
              },
              tbb::auto_partitioner());
        });

        // Ensure final update
        dedup_bar.set_progress(num_vertices);
        dedup_bar.mark_as_completed();
        std::cout << std::endl;

        // Arena destroyed here, threads released immediately
      }
    }

    std::cout << "Sorting and removing duplicates...[OK]" << std::endl;

    // Convert to CSR Format
    offsets.resize(num_vertices + 1, 0);
    size_t total_edges = 0;

    for (size_t i = 0; i < num_vertices; ++i) {
      offsets[i] = total_edges;
      total_edges += adj_list[i].size();
    }
    offsets[num_vertices] = total_edges;

    // Allocate neighbor list
    neighbor_list.resize(total_edges);

    // Copy from adjacency list to CSR format
    for (size_t i = 0; i < num_vertices; ++i) {
      std::copy(adj_list[i].begin(), adj_list[i].end(), neighbor_list.begin() + offsets[i]);
    }

    // Move communities data
    communities = std::move(temp_communities);

    // Build community structure
    community_structure_t community_structure_tmp(max_community_id + 1);
    for (size_t i = 0; i < num_vertices; ++i) {
      community_structure_tmp[communities[i]].push_back(i);
    }
    community_structure = std::move(community_structure_tmp);

    std::cout << "CSR construction complete: " << total_edges << " edges stored" << std::endl;

    // adj_list is automatically destroyed here, but release memory explicitly
    adj_list.clear();
    adj_list.shrink_to_fit();
  }

  std::cout << "Memory optimization complete. Temporary structures released." << std::endl;
}

// Validate vertex ID
bool mem_sub_com_t::is_valid_vertex(v_id_t vertex_id) const { return vertex_id < num_vertices; }

// Get neighbors of a vertex
std::vector<v_id_t> mem_sub_com_t::get_neighbors(v_id_t vertex_id) const {
  if (!is_valid_vertex(vertex_id)) {
    // Invalid vertex ID
    return {};
  }

  size_t start = offsets[vertex_id];
  size_t end = offsets[vertex_id + 1];

  return std::vector<v_id_t>(neighbor_list.begin() + start, neighbor_list.begin() + end);
}

// Get neighbors span.
neighbor_span_t mem_sub_com_t::get_neighbors_span(v_id_t vertex_id) {
  if (!is_valid_vertex(vertex_id)) {
    return neighbor_span_t(neighbor_list.data(), 0);
  }

  const size_t start = offsets[vertex_id];
  const size_t end = offsets[vertex_id + 1];

  return std::span(neighbor_list).subspan(start, end - start);
}

// Check if vertex exists
bool mem_sub_com_t::has_vertex(v_id_t vertex_id) const { return is_valid_vertex(vertex_id); }

// Check if edge exists (binary search since neighbors are sorted)
bool mem_sub_com_t::has_edge(v_id_t src_id, v_id_t dst_id) const {
  if (!is_valid_vertex(src_id)) {
    return false;
  }

  size_t start = offsets[src_id];
  size_t end = offsets[src_id + 1];

  return std::binary_search(neighbor_list.begin() + start, neighbor_list.begin() + end, dst_id);
}

// Get list of all vertices
std::vector<v_id_t> mem_sub_com_t::vertex_list() const {
  std::vector<v_id_t> vertices(num_vertices);
  for (size_t i = 0; i < num_vertices; ++i) {
    vertices[i] = static_cast<v_id_t>(i);
  }
  return vertices;
}

// Zero-Copy version induced graph
folly::F14FastMap<v_id_t, neighbor_span_t>
mem_sub_com_t::induce_subgraph_iter(std::vector<v_id_t> induced_vertex) {
  folly::F14FastMap<v_id_t, neighbor_span_t> res;
  for (v_id_t vertex : induced_vertex) {
    auto neighbors = this->get_neighbors_span(vertex);
    res.emplace(vertex, neighbors);
  }
  return res;
}

// induced graph
folly::F14FastMap<v_id_t, std::vector<v_id_t>>
mem_sub_com_t::induce_subgraph_filter(folly::F14FastSet<v_id_t>& vertex_set) {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> res;
  res.reserve(vertex_set.size());

  for (v_id_t vertex : vertex_set) {
    res[vertex] = std::vector<v_id_t>();
  }

  for (v_id_t vertex : vertex_set) {
    neighbor_span_t neighbor_span = this->get_neighbors_span(vertex);

    if (neighbor_span.empty()) {
      continue;
    }

    std::vector<v_id_t>& filtered_neighbors = res[vertex];
    filtered_neighbors.reserve(neighbor_span.size());

    for (v_id_t neighbor : neighbor_span) {
      if (vertex_set.contains(neighbor)) {
        filtered_neighbors.push_back(neighbor);
      }
    }

    filtered_neighbors.shrink_to_fit();
  }

  return res;
}

// Convert to adjacency map
folly::F14FastMap<v_id_t, std::vector<v_id_t>> mem_sub_com_t::to_map() const {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> adj_map;

  for (size_t i = 0; i < num_vertices; ++i) {
    v_id_t vertex = static_cast<v_id_t>(i);
    adj_map[vertex] = get_neighbors(vertex);
  }

  return adj_map;
}

// Get vertex count
size_t mem_sub_com_t::vertex_count() const { return num_vertices; }

// Get edge count
size_t mem_sub_com_t::edge_count() const { return neighbor_list.size(); }

// Get community of a vertex
v_id_t mem_sub_com_t::get_community_id(v_id_t vertex_id) const {
  if (!is_valid_vertex(vertex_id)) {
    throw std::invalid_argument("Vertex does not exist");
  }
  return communities[vertex_id];
}

// Get vertices in a community
std::vector<v_id_t> mem_sub_com_t::get_vertices_in_community(comm_id_t community_id) const {
  if (community_id >= community_structure.size()) {
    throw std::invalid_argument("Community ID does not exist");
  }
  return community_structure[community_id];
}

// Get communities array
const std::vector<comm_id_t>& mem_sub_com_t::get_communities() const { return communities; }

// Compute induced subgraph in adjacency map format
adj_map_t mem_sub_com_t::compute_induced_subgraph_map(std::vector<v_id_t>& vertex_subset,
                                                      bool is_pruning) const {
  // Validate all vertices exist
  for (v_id_t vertex : vertex_subset) {
    if (!is_valid_vertex(vertex)) {
      throw std::invalid_argument("Vertex " + std::to_string(vertex) + " does not exist");
    }
  }

  // Convert vertex subset to set for fast lookup
  std::unordered_set<v_id_t> vertex_set(vertex_subset.begin(), vertex_subset.end());

  // Build adjacency map for the subgraph
  adj_map_t subgraph_adj_map;

  for (v_id_t vertex : vertex_subset) {
    std::vector<v_id_t> filtered_neighbors;

    // Get all neighbors of this vertex
    size_t start = offsets[vertex];
    size_t end = offsets[vertex + 1];

    // Filter neighbors: only keep those in the vertex subset
    for (size_t i = start; i < end; ++i) {
      v_id_t neighbor = neighbor_list[i];
      if (!is_pruning || vertex_set.contains(neighbor)) {
        filtered_neighbors.push_back(neighbor);
      }
    }

    // Sort the filtered neighbors to ensure they are ordered
    std::sort(filtered_neighbors.begin(), filtered_neighbors.end());

    subgraph_adj_map[vertex] = std::move(filtered_neighbors);
  }

  return subgraph_adj_map;
}

// Compute the induced map of the whole graph
adj_map_t mem_sub_com_t::compute_induced_graph_map() const {
  // Build adjacency map for the subgraph
  adj_map_t subgraph_adj_map;

  for (auto vertex : this->vertex_list()) {
    std::vector<v_id_t> filtered_neighbors;

    // Get all neighbors of this vertex
    size_t start = offsets[vertex];
    size_t end = offsets[vertex + 1];

    // Filter neighbors: only keep those in the vertex subset
    for (size_t i = start; i < end; ++i) {
      v_id_t neighbor = neighbor_list[i];
      filtered_neighbors.push_back(neighbor);
    }

    // Sort the filtered neighbors to ensure they are ordered
    std::sort(filtered_neighbors.begin(), filtered_neighbors.end());

    subgraph_adj_map[vertex] = std::move(filtered_neighbors);
  }

  return subgraph_adj_map;
}

// Compute induced subgraph
std::shared_ptr<mem_sub_csr_t>
mem_sub_com_t::compute_induced_subgraph(const std::vector<v_id_t>& vertex_subset) const {
  // Validate all vertices exist
  for (v_id_t vertex : vertex_subset) {
    if (!is_valid_vertex(vertex)) {
      throw std::invalid_argument("Vertex " + std::to_string(vertex) + " does not exist");
    }
  }

  // Convert vertex subset to set for fast lookup
  std::unordered_set<v_id_t> vertex_set(vertex_subset.begin(), vertex_subset.end());

  // Build adjacency map for the subgraph
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> subgraph_adj_map;

  for (v_id_t vertex : vertex_subset) {
    std::vector<v_id_t> filtered_neighbors;

    // Get all neighbors of this vertex
    size_t start = offsets[vertex];
    size_t end = offsets[vertex + 1];

    // Filter neighbors: only keep those in the vertex subset
    for (size_t i = start; i < end; ++i) {
      v_id_t neighbor = neighbor_list[i];
      if (vertex_set.find(neighbor) != vertex_set.end()) {
        filtered_neighbors.push_back(neighbor);
      }
    }

    // Sort the filtered neighbors to ensure they are ordered
    std::sort(filtered_neighbors.begin(), filtered_neighbors.end());

    subgraph_adj_map[vertex] = std::move(filtered_neighbors);
  }
  // Create and return the subgraph using mem_sub_csr_t
  return std::make_shared<mem_sub_csr_t>(subgraph_adj_map);
}

// Compute community subgraph
std::shared_ptr<mem_sub_csr_t>
mem_sub_com_t::compute_community_subgraph(comm_id_t community_id) const {
  std::vector<v_id_t> community_vertices = get_vertices_in_community(community_id);
  return compute_induced_subgraph(community_vertices);
}

// Get graph statistics
std::string mem_sub_com_t::get_graph_statistics() const {
  std::ostringstream oss;
  oss << "Graph Statistics:\n";
  oss << "  Vertices: " << num_vertices << "\n";
  oss << "  Edges: " << edge_count() << "\n";

  if (num_vertices > 0) {
    double avg_degree = static_cast<double>(edge_count()) / num_vertices;
    oss << "  Average degree: " << avg_degree << "\n";

    // Find min and max degree
    size_t min_degree = SIZE_MAX, max_degree = 0;
    for (size_t i = 0; i < num_vertices; ++i) {
      size_t degree = offsets[i + 1] - offsets[i];
      min_degree = std::min(min_degree, degree);
      max_degree = std::max(max_degree, degree);
    }
    oss << "  Min degree: " << min_degree << "\n";
    oss << "  Max degree: " << max_degree << "\n";
  }

  return oss.str();
}

// Get community statistics
std::string mem_sub_com_t::get_community_statistics() const {
  std::ostringstream oss;

  // Count communities
  std::unordered_map<comm_id_t, size_t> community_sizes;
  for (comm_id_t community : communities) {
    community_sizes[community]++;
  }

  oss << "Community Statistics:\n";
  oss << "  Number of communities: " << community_sizes.size() << "\n";

  if (!community_sizes.empty()) {
    size_t min_size = SIZE_MAX, max_size = 0;
    for (const auto& pair : community_sizes) {
      min_size = std::min(min_size, pair.second);
      max_size = std::max(max_size, pair.second);
    }

    double avg_size = static_cast<double>(num_vertices) / community_sizes.size();
    oss << "  Average community size: " << avg_size << "\n";
    oss << "  Min community size: " << min_size << "\n";
    oss << "  Max community size: " << max_size << "\n";

    oss << "  Community size distribution:\n";
    for (const auto& pair : community_sizes) {
      oss << "    Community " << pair.first << ": " << pair.second << " vertices\n";
    }
  }

  return oss.str();
}

// Run bfs.
std::vector<std::pair<v_id_t, uint64_t>> mem_sub_com_t::bfs(v_id_t start_vertex) {
  // Validate start vertex
  if (!is_valid_vertex(start_vertex)) {
    return {};
  }

  // Pre-allocate vectors with estimated size to reduce memory allocation
  std::vector<std::pair<v_id_t, uint64_t>> result;
  result.reserve(num_vertices);

  // Use bitset for visited tracking (more memory efficient than bool vector)
  std::vector<bool> visited(num_vertices, false);

  // Use two vectors for current and next level (ping-pong approach)
  // This avoids the overhead of std::queue
  std::vector<v_id_t> current_level, next_level;
  current_level.reserve(std::min(num_vertices, size_t(1024))); // Reserve reasonable initial size
  next_level.reserve(std::min(num_vertices, size_t(1024)));

  // Start BFS
  current_level.push_back(start_vertex);
  visited[start_vertex] = true;
  result.emplace_back(start_vertex, 0); // Distance 0 for start vertex

  uint64_t current_distance = 0;

  while (!current_level.empty()) {
    next_level.clear();
    current_distance++; // Increment distance for next level

    // Process all vertices in current level
    for (v_id_t vertex : current_level) {
      // Get neighbors using direct CSR access
      size_t start_idx = offsets[vertex];
      size_t end_idx = offsets[vertex + 1];

      // Iterate through neighbors
      for (size_t i = start_idx; i < end_idx; ++i) {
        v_id_t neighbor = neighbor_list[i];

        // Check if neighbor is already visited
        if (!visited[neighbor]) {
          visited[neighbor] = true;
          next_level.push_back(neighbor);
          result.emplace_back(neighbor, current_distance);
        }
      }
    }

    // Swap levels for next iteration (ping-pong)
    current_level.swap(next_level);

    // Optimize memory usage by shrinking if next_level is much smaller
    if (current_level.capacity() > current_level.size() * 4 && current_level.capacity() > 1024) {
      current_level.shrink_to_fit();
    }
  }

  // Final optimization: shrink result to actual size
  result.shrink_to_fit();

  return result;
}

// Run bfs parallel.
std::vector<std::pair<v_id_t, uint64_t>> mem_sub_com_t::bfs_parallel(v_id_t start_vertex) {
  // Validate start vertex
  if (!is_valid_vertex(start_vertex)) {
    return {};
  }

  // For most graphs, BFS has limited parallelism due to its inherent sequential
  // nature. Only use parallel version for very large graphs with high degree
  // vertices.
  if (num_vertices < 100000) {
    return bfs(start_vertex);
  }

  // Create task arena with exactly 4 threads
  tbb::task_arena arena(4);

  // Execute all parallel work inside the 4-thread arena
  return arena.execute([&]() -> std::vector<std::pair<v_id_t, uint64_t>> {
    // Use distance array instead of result vector for better cache performance
    std::vector<uint64_t> distances(num_vertices, UINT32_MAX);
    std::vector<bool> visited(num_vertices, false); // Use regular bool for better cache efficiency

    // Use simple vectors with manual synchronization - often faster than
    // concurrent containers
    std::vector<v_id_t> current_frontier, next_frontier;
    current_frontier.reserve(std::min(num_vertices / 10, size_t(10000)));
    next_frontier.reserve(std::min(num_vertices / 5, size_t(50000)));

    // Initialize
    current_frontier.push_back(start_vertex);
    distances[start_vertex] = 0;
    visited[start_vertex] = true;

    uint64_t current_distance = 0;

    while (!current_frontier.empty()) {
      next_frontier.clear();
      current_distance++;

      // Only parallelize if frontier is large enough to amortize overhead
      // Adjusted threshold for 4 threads - lower threshold since we have fewer
      // threads
      if (current_frontier.size() > 400) { // Reduced from 1000 for 4 threads
        // Use a simple spin lock for next_frontier - often faster than
        // concurrent_vector
        std::mutex next_frontier_mutex;

        // Adjust grain size for 4 threads - make it larger to reduce overhead
        size_t grain_size =
            std::max(size_t(16), current_frontier.size() / 8); // ~4 tasks per thread

        tbb::parallel_for(tbb::blocked_range<size_t>(0, current_frontier.size(), grain_size),
                          [&](const tbb::blocked_range<size_t>& range) {
                            std::vector<v_id_t> local_next;
                            local_next.reserve(256);

                            for (size_t idx = range.begin(); idx != range.end(); ++idx) {
                              v_id_t vertex = current_frontier[idx];

                              size_t start_idx = offsets[vertex];
                              size_t end_idx = offsets[vertex + 1];

                              for (size_t i = start_idx; i < end_idx; ++i) {
                                v_id_t neighbor = neighbor_list[i];

                                // Simple check without atomic operations
                                if (!visited[neighbor]) {
                                  // Double-check with synchronization only when needed
                                  {
                                    std::lock_guard<std::mutex> lock(next_frontier_mutex);
                                    if (!visited[neighbor]) {
                                      visited[neighbor] = true;
                                      distances[neighbor] = current_distance;
                                      local_next.push_back(neighbor);
                                    }
                                  }
                                }
                              }
                            }

                            // Bulk append to next_frontier
                            if (!local_next.empty()) {
                              std::lock_guard<std::mutex> lock(next_frontier_mutex);
                              next_frontier.insert(next_frontier.end(), local_next.begin(),
                                                   local_next.end());
                            }
                          });
      } else {
        // Sequential processing for small frontiers
        for (v_id_t vertex : current_frontier) {
          size_t start_idx = offsets[vertex];
          size_t end_idx = offsets[vertex + 1];

          for (size_t i = start_idx; i < end_idx; ++i) {
            v_id_t neighbor = neighbor_list[i];

            if (!visited[neighbor]) {
              visited[neighbor] = true;
              distances[neighbor] = current_distance;
              next_frontier.push_back(neighbor);
            }
          }
        }
      }

      current_frontier.swap(next_frontier);
    }

    // Build result from distances array
    std::vector<std::pair<v_id_t, uint64_t>> result;
    result.reserve(num_vertices);

    for (size_t i = 0; i < num_vertices; ++i) {
      if (distances[i] != UINT32_MAX) {
        result.emplace_back(static_cast<v_id_t>(i), distances[i]);
      }
    }

    // Sort by distance for consistent ordering (optional)
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    return result;
  });
}

// Graph sampling based on BFS (memory-optimized version)
std::shared_ptr<mem_sub_map_t> mem_sub_com_t::graph_sample_bfs(v_id_t start_vertex,
                                                               uint64_t vertex_count) {
  // Validate start vertex
  if (!has_vertex(start_vertex)) {
    return std::make_shared<mem_sub_map_t>(adj_map_t{});
  }

  // Track sampled vertices
  std::vector<v_id_t> sampled_vertices;
  sampled_vertices.reserve(vertex_count);

  // Track visited vertices to avoid revisiting
  folly::F14FastSet<v_id_t> visited_set;
  visited_set.reserve(vertex_count);

  // BFS queue
  std::queue<v_id_t> bfs_queue;

  // Start BFS from the start vertex
  bfs_queue.push(start_vertex);
  visited_set.insert(start_vertex);
  sampled_vertices.push_back(start_vertex);

  // BFS traversal until we collect enough vertices
  while (!bfs_queue.empty() && sampled_vertices.size() < vertex_count) {
    v_id_t current_vertex = bfs_queue.front();
    bfs_queue.pop();

    // Get neighbors of current vertex (zero-copy reference)
    neighbor_span_t neighbors = get_neighbors_span(current_vertex);

    // Add unvisited neighbors to the queue
    for (v_id_t neighbor : neighbors) {
      // Stop if we've reached the target vertex count
      if (sampled_vertices.size() >= vertex_count) {
        break;
      }

      // Skip if already visited
      if (visited_set.contains(neighbor)) {
        continue;
      }

      // Mark as visited and add to sampled vertices
      visited_set.insert(neighbor);
      sampled_vertices.push_back(neighbor);
      bfs_queue.push(neighbor);
    }
  }

  // Compute the induced subgraph containing only edges between sampled vertices
  // Pre-allocate the map with expected size
  adj_map_t induced_subgraph;
  induced_subgraph.reserve(sampled_vertices.size());

  for (v_id_t vertex : sampled_vertices) {
    // Get all neighbors of this vertex (zero-copy reference)
    neighbor_span_t neighbors = get_neighbors_span(vertex);

    // Pre-count valid neighbors to reserve exact capacity
    size_t valid_count = 0;
    for (v_id_t neighbor : neighbors) {
      if (visited_set.contains(neighbor)) {
        valid_count++;
      }
    }

    // Reserve exact capacity to avoid reallocations
    std::vector<v_id_t> filtered_neighbors;
    filtered_neighbors.reserve(valid_count);

    // Filter neighbors: only keep those in the sampled vertex set
    for (v_id_t neighbor : neighbors) {
      if (visited_set.contains(neighbor)) {
        filtered_neighbors.push_back(neighbor);
      }
    }

    // Sort the filtered neighbors to ensure they are ordered
    std::sort(filtered_neighbors.begin(), filtered_neighbors.end());

    // Move the vector into the map to avoid copy
    induced_subgraph[vertex] = std::move(filtered_neighbors);
  }

  // Move construct the adjacency map to avoid copy
  return std::make_shared<mem_sub_map_t>(std::move(induced_subgraph));
}

// Graph sampling based on random walk (zero-copy version)
std::shared_ptr<mem_sub_map_t> mem_sub_com_t::graph_sample_random_walk(v_id_t start_vertex,
                                                                       uint64_t max_vertices) {
  // Validate start vertex
  if (!is_valid_vertex(start_vertex)) {
    return std::make_shared<mem_sub_map_t>(folly::F14FastMap<v_id_t, std::vector<v_id_t>>{});
  }

  // Track sampled vertices in order
  std::vector<v_id_t> sampled_vertices;
  sampled_vertices.reserve(max_vertices);

  // Track which vertices have been sampled
  folly::F14FastSet<v_id_t> visited_set;
  visited_set.reserve(max_vertices);

  // Adjacency map to store walked edges
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> walked_edges;

  // Random number generator for selecting neighbors
  std::random_device rd;
  std::mt19937 gen(rd());

  // Start random walk from the start vertex
  v_id_t current_vertex = start_vertex;
  sampled_vertices.push_back(current_vertex);
  visited_set.insert(current_vertex);

  // Continue random walk until reaching exactly max_vertices
  while (sampled_vertices.size() < max_vertices) {
    // Get zero-copy neighbor span of current vertex
    neighbor_span_t neighbors_span = get_neighbors_span(current_vertex);

    // If current vertex has no neighbors, restart from a sampled vertex
    if (neighbors_span.empty()) {
      // Restart from a random previously sampled vertex
      if (sampled_vertices.size() > 1) {
        std::uniform_int_distribution<size_t> restart_dist(0, sampled_vertices.size() - 1);
        current_vertex = sampled_vertices[restart_dist(gen)];
        continue;
      } else {
        // Only have start vertex and it has no neighbors, cannot continue
        break;
      }
    }

    // Count valid neighbors without copying
    size_t valid_count = 0;
    for (auto const& neighbor : neighbors_span) {
      if (is_valid_vertex(neighbor)) {
        valid_count++;
      }
    }

    // If no valid neighbors found, restart from a sampled vertex
    if (valid_count == 0) {
      if (sampled_vertices.size() > 1) {
        std::uniform_int_distribution<size_t> restart_dist(0, sampled_vertices.size() - 1);
        current_vertex = sampled_vertices[restart_dist(gen)];
        continue;
      } else {
        break;
      }
    }

    // Randomly select one valid neighbor to visit next
    // Use reservoir sampling approach to avoid copying all valid neighbors
    std::uniform_int_distribution<size_t> selector_dist(0, valid_count - 1);
    size_t target_index = selector_dist(gen);

    v_id_t next_vertex = 0;
    size_t current_valid_index = 0;
    for (auto const& neighbor : neighbors_span) {
      if (is_valid_vertex(neighbor)) {
        if (current_valid_index == target_index) {
          next_vertex = neighbor;
          break;
        }
        current_valid_index++;
      }
    }

    // Record the walked edge: current_vertex -> next_vertex
    walked_edges[current_vertex].push_back(next_vertex);

    // If this is a new vertex, add it to sampled vertices
    if (visited_set.find(next_vertex) == visited_set.end()) {
      sampled_vertices.push_back(next_vertex);
      visited_set.insert(next_vertex);
    }

    // Move to the next vertex
    current_vertex = next_vertex;

    // Optional: With small probability, restart from a random sampled vertex
    // to explore different regions of the graph
    std::uniform_real_distribution<double> restart_prob(0.0, 1.0);
    if (restart_prob(gen) < 0.15 && sampled_vertices.size() > 1) {
      std::uniform_int_distribution<size_t> restart_dist(0, sampled_vertices.size() - 1);
      current_vertex = sampled_vertices[restart_dist(gen)];
    }
  }

  // Create the sampled subgraph containing only the walked edges and vertices
  return std::make_shared<mem_sub_map_t>(walked_edges);
}

// Compute degree distribution
std::vector<std::pair<uint64_t, double>>
mem_sub_com_t::compute_degree_distribution(uint64_t step) const {
  if (num_vertices == 0 || step == 0) {
    return {};
  }

  // Find maximum degree
  uint64_t max_degree = 0;
  for (size_t i = 0; i < num_vertices; ++i) {
    uint64_t degree = offsets[i + 1] - offsets[i];
    max_degree = std::max(max_degree, degree);
  }

  // Calculate number of buckets needed
  uint64_t num_buckets = (max_degree / step) + 1;

  // Count vertices in each degree range
  std::vector<uint64_t> degree_counts(num_buckets, 0);

  for (size_t i = 0; i < num_vertices; ++i) {
    uint64_t degree = offsets[i + 1] - offsets[i];
    uint64_t bucket_index = degree / step;
    degree_counts[bucket_index]++;
  }

  // Convert counts to percentages and build result
  std::vector<std::pair<uint64_t, double>> distribution;
  distribution.reserve(num_buckets);

  for (uint64_t i = 0; i < num_buckets; ++i) {
    if (degree_counts[i] > 0) {
      uint64_t range_start = i * step;
      double percentage = (static_cast<double>(degree_counts[i]) / num_vertices) * 100.0;
      distribution.emplace_back(range_start, percentage);
    }
  }

  return distribution;
}