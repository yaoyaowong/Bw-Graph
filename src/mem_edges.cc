#include "bw_graph/mem/mem_edges.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

// Default constructor
mem_edge_list_t::mem_edge_list_t() : graph_name(""), file_path(""), buffer_size(64 * 1024) {}

// Constructor with graph name
mem_edge_list_t::mem_edge_list_t(const std::string& graph_name, size_t buffer_size)
    : graph_name(graph_name), file_path("data/" + graph_name + ".delta"), buffer_size(buffer_size) {
}

// Constructor with custom file path
mem_edge_list_t::mem_edge_list_t(const std::string& graph_name, const std::string& file_path,
                                 size_t buffer_size)
    : graph_name(graph_name), file_path(file_path), buffer_size(buffer_size) {}

// Generate workload for smo.
mem_edge_list_t mem_edge_list_t::generate_workload_for_smo(std::string graph_name,
                                                           v_id_t num_vertices,
                                                           size_t edges_per_vertex, uint64_t seed) {
  // Create an empty edge list
  mem_edge_list_t edge_list;
  edge_list.graph_name = graph_name;
  edge_list.file_path = "";

  // Calculate total number of edges
  size_t total_edges = static_cast<size_t>(num_vertices) * edges_per_vertex;

  // Reserve space for all edges
  edge_list.edges.reserve(total_edges);

  // Initialize random number generator
  std::mt19937_64 rng;
  if (seed == 0) {
    std::random_device rd;
    rng.seed(rd());
  } else {
    rng.seed(seed);
  }

  // Uniform distribution for destination vertices [0, num_vertices)
  std::uniform_int_distribution<v_id_t> dist(0, num_vertices - 1);

  std::cout << "Generating SMO workload: " << num_vertices << " vertices, " << edges_per_vertex
            << " edges per vertex" << std::endl;

  // Generate edges for each vertex
  for (v_id_t src = 0; src < num_vertices; ++src) {
    // Use a set to avoid duplicate edges for the same source
    std::unordered_set<v_id_t> neighbors;
    neighbors.reserve(edges_per_vertex);

    // Generate unique random neighbors
    while (neighbors.size() < edges_per_vertex) {
      v_id_t dst = dist(rng);

      // Avoid self-loops (optional, remove if you want self-loops)
      if (dst != src) {
        neighbors.insert(dst);
      }
    }

    // Add all edges to the edge list
    for (v_id_t dst : neighbors) {
      edge_list.edges.emplace_back(src, dst);
    }
  }

  // Shrink to fit to release excess memory
  edge_list.edges.shrink_to_fit();

  std::cout << "Generated " << edge_list.edges.size() << " edges for " << num_vertices
            << " vertices" << std::endl;

  return edge_list;
}

// Load edges from file
bool mem_edge_list_t::load_from_file() { return load_from_file(file_path); }

// Load edges from file with custom path
bool mem_edge_list_t::load_from_file(const std::string& custom_path) {
  std::ifstream file(custom_path);
  if (!file.is_open()) {
    std::cerr << "Cannot open file: " << custom_path << std::endl;
    return false;
  }

// Platform-Specific Optimizations
#if defined(__linux__)
  int fd = open(custom_path.c_str(), O_RDONLY);
  if (fd != -1) {
    std::cout << "Enabling readahead on Linux for file: " << custom_path << std::endl;
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
    close(fd);
  }
#elif defined(__APPLE__)
  int fd = open(custom_path.c_str(), O_RDONLY);
  if (fd != -1) {
    std::cout << "Enabling readahead on macOS for file: " << custom_path << std::endl;
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
  std::vector<char> io_buffer(buffer_size);
  file.rdbuf()->pubsetbuf(io_buffer.data(), buffer_size);

  // Clear existing edges
  edges.clear();

  // Reserve space for edges (assume average file has 1M edges)
  edges.reserve(1000000);

  std::string line;
  size_t line_count = 0;
  const size_t PRINT_INTERVAL = 10000000;

  std::cout << "Loading edges from file: " << custom_path << std::endl;

  while (std::getline(file, line)) {
    line_count++;

    if (line_count % PRINT_INTERVAL == 0) {
      std::cout << "Processed " << line_count << " lines..." << std::endl;
    }

    // Skip empty lines and comments
    if (line.empty() || line[0] == '#') {
      continue;
    }

    // Fast parsing using pointer arithmetic
    const char* ptr = line.c_str();

    // Skip leading whitespace
    while (*ptr == ' ' || *ptr == '\t') {
      ++ptr;
    }

    // Parse source vertex
    v_id_t src = 0;
    while (*ptr >= '0' && *ptr <= '9') {
      src = src * 10 + (*ptr++ - '0');
    }

    // Skip whitespace between numbers
    while (*ptr == ' ' || *ptr == '\t') {
      ++ptr;
    }

    // Parse destination vertex
    v_id_t dst = 0;
    while (*ptr >= '0' && *ptr <= '9') {
      dst = dst * 10 + (*ptr++ - '0');
    }

    // Add edge to list
    edges.emplace_back(src, dst);
  }

  file.close();

  // Shrink to fit to release excess memory
  edges.shrink_to_fit();

  std::cout << "Loaded " << edges.size() << " edges from file" << std::endl;

  return true;
}

// Get edge count
size_t mem_edge_list_t::edge_count() const { return edges.size(); }

// Get graph name
const std::string& mem_edge_list_t::get_graph_name() const { return graph_name; }

// Get file path
const std::string& mem_edge_list_t::get_file_path() const { return file_path; }

// Get edges reference
const std::vector<std::pair<v_id_t, v_id_t>>& mem_edge_list_t::get_edges() const { return edges; }

// Clear edges
void mem_edge_list_t::clear() {
  edges.clear();
  edges.shrink_to_fit();
}

// Add single edge
void mem_edge_list_t::add_edge(v_id_t src, v_id_t dst) { edges.emplace_back(src, dst); }

// Apply operation sequentially with insertion mode
void mem_edge_list_t::apply_sequential(const std::function<void(v_id_t, v_id_t)>& operation,
                                       insert_mode_t mode, bool is_shuffle) const {
  if (mode == insert_mode_t::SEQUENTIAL) {
    // Create a sorted copy of edges by source ID
    std::vector<std::pair<v_id_t, v_id_t>> sorted_edges = edges;
    std::sort(sorted_edges.begin(), sorted_edges.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first)
        return a.first < b.first;
      return a.second < b.second;
    });

    std::cout << "Processing " << sorted_edges.size() << " edges in SEQUENTIAL mode" << std::endl;

    for (const auto& edge : sorted_edges) {
      operation(edge.first, edge.second);
    }
  } else {
    // RANDOM mode - shuffle and process edges
    std::vector<std::pair<v_id_t, v_id_t>> shuffled_edges = edges;
    if (is_shuffle) {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::shuffle(shuffled_edges.begin(), shuffled_edges.end(), gen);

      std::cout << "Processing " << shuffled_edges.size() << " edges in RANDOM mode" << std::endl;
    }

    for (const auto& edge : shuffled_edges) {
      operation(edge.first, edge.second);
    }
  }
}

// Apply operation in parallel with insertion mode
void mem_edge_list_t::apply_parallel(const std::function<void(v_id_t, v_id_t)>& operation,
                                     int num_threads, insert_mode_t mode) const {
  if (edges.empty()) {
    return;
  }

  // Determine number of threads
  if (num_threads <= 0) {
    num_threads = tbb::info::default_concurrency();
  }

  std::cout << "Processing " << edges.size() << " edges with " << num_threads << " threads in "
            << (mode == insert_mode_t::SEQUENTIAL ? "SEQUENTIAL" : "RANDOM") << " mode"
            << std::endl;

  // Create task arena with specified threads
  tbb::task_arena arena(num_threads);

  if (mode == insert_mode_t::SEQUENTIAL) {
    // SEQUENTIAL mode: sort edges, then partition among threads
    std::vector<std::pair<v_id_t, v_id_t>> sorted_edges = edges;

    std::cout << "Sorting edges by source ID..." << std::endl;
    std::sort(sorted_edges.begin(), sorted_edges.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first)
        return a.first < b.first;
      return a.second < b.second;
    });

    std::cout << "Partitioning sorted edges among " << num_threads << " threads..." << std::endl;

    arena.execute([&] {
      // Each thread gets a contiguous chunk of sorted edges
      size_t chunk_size = (sorted_edges.size() + num_threads - 1) / num_threads;

      tbb::parallel_for(tbb::blocked_range<int>(0, num_threads),
                        [&](const tbb::blocked_range<int>& thread_range) {
                          for (int thread_id = thread_range.begin();
                               thread_id != thread_range.end(); ++thread_id) {
                            size_t start_idx = thread_id * chunk_size;
                            size_t end_idx = std::min(start_idx + chunk_size, sorted_edges.size());

                            // Process edges in sequential order within this thread
                            for (size_t i = start_idx; i < end_idx; ++i) {
                              operation(sorted_edges[i].first, sorted_edges[i].second);
                            }
                          }
                        });
    });
  } else if (mode == insert_mode_t::RANDOM) {
    // RANDOM mode: shuffle edges then process in parallel
    std::vector<std::pair<v_id_t, v_id_t>> shuffled_edges = edges;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(shuffled_edges.begin(), shuffled_edges.end(), gen);

    std::cout << "Shuffled edges for random insertion" << std::endl;

    arena.execute([&] {
      size_t grain_size = std::max(size_t(1000), shuffled_edges.size() / (num_threads * 4));

      tbb::parallel_for(tbb::blocked_range<size_t>(0, shuffled_edges.size(), grain_size),
                        [&](const tbb::blocked_range<size_t>& range) {
                          for (size_t i = range.begin(); i != range.end(); ++i) {
                            operation(shuffled_edges[i].first, shuffled_edges[i].second);
                          }
                        });
    });
  } else {
    // Perform edge insertion in default mode;
    size_t grain_size = std::max(size_t(1000), this->edges.size() / (num_threads * 4));
    std::cout << "Perform default edge insertion." << std::endl;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, this->edges.size(), grain_size),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i != range.end(); ++i) {
                          operation(this->edges[i].first, this->edges[i].second);
                        }
                      });
  }

  std::cout << "Finished processing edges" << std::endl;
}

// Apply operation to range
void mem_edge_list_t::apply_range(const std::function<void(v_id_t, v_id_t)>& operation,
                                  size_t start_idx, size_t end_idx) const {
  if (start_idx >= edges.size() || end_idx > edges.size() || start_idx >= end_idx) {
    throw std::invalid_argument("Invalid range for apply_range");
  }

  for (size_t i = start_idx; i < end_idx; ++i) {
    operation(edges[i].first, edges[i].second);
  }
}

// Transform to Zipfian distribution
void mem_edge_list_t::to_zipfian(double alpha, uint64_t seed, size_t max_edges_per_source) {
  if (edges.empty()) {
    std::cerr << "Cannot apply Zipfian distribution to empty edge list" << std::endl;
    return;
  }
  if (!std::isfinite(alpha) || alpha < 0.0) {
    throw std::invalid_argument("Zipfian alpha must be finite and non-negative");
  }

  std::cout << "\n=== Transforming to Zipfian Distribution ===" << std::endl;
  std::cout << "Alpha (skewness): " << alpha << std::endl;
  if (max_edges_per_source == 0) {
    std::cout << "Max edges per source: uncapped" << std::endl;
  } else {
    std::cout << "Max edges per source: " << max_edges_per_source << std::endl;
  }
  std::cout << "Original edge count: " << edges.size() << std::endl;

  // Use provided seed or generate random one
  if (seed == 0) {
    std::random_device rd;
    seed = rd();
  }
  std::mt19937_64 gen(seed);
  std::cout << "Random seed: " << seed << std::endl;

  // Step 1: Collect all unique source vertices and their edges
  std::unordered_map<v_id_t, std::vector<v_id_t>> src_to_dsts;
  std::unordered_set<v_id_t> all_dsts;

  for (const auto& edge : edges) {
    src_to_dsts[edge.first].push_back(edge.second);
    all_dsts.insert(edge.second);
  }

  std::vector<v_id_t> unique_sources;
  unique_sources.reserve(src_to_dsts.size());
  for (const auto& [src, dsts] : src_to_dsts) {
    unique_sources.push_back(src);
  }
  std::sort(unique_sources.begin(), unique_sources.end());
  std::shuffle(unique_sources.begin(), unique_sources.end(), gen);

  size_t num_sources = unique_sources.size();
  std::cout << "Unique source vertices: " << num_sources << std::endl;
  std::cout << "Unique destination vertices: " << all_dsts.size() << std::endl;
  std::cout << "Average source degree: " << (static_cast<double>(edges.size()) / num_sources)
            << std::endl;

  size_t total_edges = edges.size();
  const size_t effective_source_cap =
      max_edges_per_source == 0 ? total_edges : max_edges_per_source;
  if (effective_source_cap == 0) {
    throw std::invalid_argument("Zipfian max_edges_per_source cannot be zero for non-empty input");
  }
  if (max_edges_per_source != 0) {
    if (num_sources > std::numeric_limits<size_t>::max() / max_edges_per_source) {
      throw std::overflow_error("Zipfian source capacity overflow");
    }
    const size_t total_capacity = num_sources * max_edges_per_source;
    if (total_capacity < total_edges) {
      throw std::invalid_argument(
          "Zipfian max_edges_per_source is too small to preserve the edge count");
    }
  }

  // Step 2: Calculate Zipfian probabilities
  std::vector<double> weights(num_sources);

  for (size_t i = 0; i < num_sources; ++i) {
    weights[i] = 1.0 / std::pow(i + 1, alpha);
  }

  // Step 3: Calculate capped expected edge count for each source.  This is a
  // weighted water-fill: hot sources keep their Zipfian weight, but once a
  // source reaches the cap, its remaining mass is redistributed to sources
  // that still have capacity.
  std::vector<size_t> edge_counts(num_sources, 0);
  std::vector<size_t> active_sources(num_sources);
  std::iota(active_sources.begin(), active_sources.end(), 0);

  size_t remaining_edges = total_edges;
  while (remaining_edges > 0) {
    double active_weight_sum = 0.0;
    std::vector<size_t> next_active_sources;
    next_active_sources.reserve(active_sources.size());

    for (size_t idx : active_sources) {
      if (edge_counts[idx] < effective_source_cap) {
        active_weight_sum += weights[idx];
      }
    }
    if (active_weight_sum <= 0.0) {
      throw std::runtime_error("Zipfian redistribution ran out of active sources");
    }

    const size_t round_remaining = remaining_edges;
    size_t assigned_this_round = 0;
    for (size_t idx : active_sources) {
      const size_t capacity = effective_source_cap - edge_counts[idx];
      if (capacity == 0) {
        continue;
      }

      const double expected = (weights[idx] / active_weight_sum) * round_remaining;
      size_t add_edges = std::min(capacity, static_cast<size_t>(expected));
      add_edges = std::min(add_edges, round_remaining - assigned_this_round);

      edge_counts[idx] += add_edges;
      assigned_this_round += add_edges;

      if (edge_counts[idx] < effective_source_cap) {
        next_active_sources.push_back(idx);
      }
      if (assigned_this_round == round_remaining) {
        break;
      }
    }

    remaining_edges -= assigned_this_round;
    active_sources.swap(next_active_sources);
    if (remaining_edges == 0) {
      break;
    }
    if (active_sources.empty()) {
      throw std::runtime_error("Zipfian redistribution exhausted source capacity");
    }

    // Finish small rounding residues without biasing everything back to rank 1.
    if (assigned_this_round == 0) {
      for (size_t idx : active_sources) {
        if (remaining_edges == 0) {
          break;
        }
        edge_counts[idx]++;
        remaining_edges--;
      }
    }
  }

  // Step 4: Create new edge list following Zipfian distribution
  std::vector<std::pair<v_id_t, v_id_t>> new_edges;
  new_edges.reserve(total_edges);

  // Convert destination set to vector for random selection
  std::vector<v_id_t> dst_vector(all_dsts.begin(), all_dsts.end());
  std::uniform_int_distribution<size_t> dst_dist(0, dst_vector.size() - 1);

  for (size_t i = 0; i < num_sources; ++i) {
    v_id_t src = unique_sources[i];
    size_t num_edges = edge_counts[i];

    for (size_t j = 0; j < num_edges; ++j) {
      v_id_t dst = dst_vector[dst_dist(gen)];
      new_edges.emplace_back(src, dst);
    }
  }

  // Step 5: Shuffle the new edges for random insertion order
  std::shuffle(new_edges.begin(), new_edges.end(), gen);

  // Replace old edges with new Zipfian-distributed edges
  edges = std::move(new_edges);

  // Print statistics
  std::cout << "\n=== Zipfian Transformation Complete ===" << std::endl;
  std::cout << "New edge count: " << edges.size() << std::endl;

  // Calculate and display top sources
  std::map<v_id_t, size_t> src_edge_count;
  for (const auto& edge : edges) {
    src_edge_count[edge.first]++;
  }

  std::vector<std::pair<v_id_t, size_t>> sorted_src_counts(src_edge_count.begin(),
                                                           src_edge_count.end());
  // Handle sort.
  std::sort(sorted_src_counts.begin(), sorted_src_counts.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  std::cout << "\nTop 10 hottest sources:" << std::endl;
  for (size_t i = 0; i < std::min(size_t(10), sorted_src_counts.size()); ++i) {
    double percentage = (sorted_src_counts[i].second * 100.0) / edges.size();
    std::cout << "  Source " << sorted_src_counts[i].first << ": " << sorted_src_counts[i].second
              << " edges (" << percentage << "%)" << std::endl;
  }

  // Calculate concentration metrics
  size_t top_20_percent_count = std::max(size_t(1), num_sources / 5);
  size_t top_20_edges = 0;
  for (size_t i = 0; i < std::min(top_20_percent_count, sorted_src_counts.size()); ++i) {
    top_20_edges += sorted_src_counts[i].second;
  }

  double concentration_ratio = (top_20_edges * 100.0) / edges.size();
  std::cout << "\nTop 20% sources account for " << concentration_ratio << "% of all edges"
            << std::endl;
  std::cout << "=========================================\n" << std::endl;
}

// Get statistics
std::string mem_edge_list_t::get_statistics() const {
  std::ostringstream oss;
  oss << "Edge List Statistics:\n";
  oss << "  Graph name: " << graph_name << "\n";
  oss << "  File path: " << file_path << "\n";
  oss << "  Number of edges: " << edges.size() << "\n";

  if (!edges.empty()) {
    // Find unique vertices
    std::unordered_set<v_id_t> unique_vertices;
    v_id_t max_vertex = 0;

    for (const auto& edge : edges) {
      unique_vertices.insert(edge.first);
      unique_vertices.insert(edge.second);
      max_vertex = std::max(max_vertex, std::max(edge.first, edge.second));
    }

    oss << "  Unique vertices: " << unique_vertices.size() << "\n";
    oss << "  Max vertex ID: " << max_vertex << "\n";

    // Calculate memory usage
    size_t memory_bytes = edges.capacity() * sizeof(std::pair<v_id_t, v_id_t>);
    double memory_mb = memory_bytes / (1024.0 * 1024.0);
    oss << "  Memory usage: " << memory_mb << " MB\n";
  }

  return oss.str();
}

// Apply operation at constant rate
void mem_edge_list_t::apply_at_rate(const std::function<void(v_id_t, v_id_t)>& operation,
                                    size_t edges_per_second) const {
  if (edges.empty() || edges_per_second == 0) {
    return;
  }

  // Calculate time interval between edges in nanoseconds
  uint64_t interval_ns = 1000000000ULL / edges_per_second;

  std::cout << "Processing " << edges.size() << " edges at " << edges_per_second
            << " edges/sec (interval: " << interval_ns << " ns)" << std::endl;

  auto start_time = std::chrono::high_resolution_clock::now();
  size_t processed_count = 0;
  const size_t PRINT_INTERVAL = edges_per_second; // Print every second

  for (const auto& edge : edges) {
    auto edge_start = std::chrono::high_resolution_clock::now();

    // Process the edge
    operation(edge.first, edge.second);

    processed_count++;

    // Print progress periodically
    if (processed_count % PRINT_INTERVAL == 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::high_resolution_clock::now() - start_time)
                         .count();
      double actual_rate = processed_count / (elapsed + 1.0);
      std::cout << "Processed " << processed_count << "/" << edges.size() << " edges ("
                << (processed_count * 100.0 / edges.size()) << "%) - Actual rate: " << actual_rate
                << " edges/sec" << std::endl;
    }

    // Calculate how long to sleep to maintain target rate
    auto edge_end = std::chrono::high_resolution_clock::now();
    auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(edge_end - edge_start).count();

    // Sleep for remaining time if we processed too fast
    if (elapsed_ns < static_cast<int64_t>(interval_ns)) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(interval_ns - elapsed_ns));
    }
  }

  auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::high_resolution_clock::now() - start_time)
                           .count();
  double actual_rate = (edges.size() * 1000.0) / total_elapsed;

  std::cout << "Finished processing " << edges.size() << " edges in " << total_elapsed << " ms"
            << std::endl;
  std::cout << "Target rate: " << edges_per_second << " edges/sec, Actual rate: " << actual_rate
            << " edges/sec" << std::endl;
}
