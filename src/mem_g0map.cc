#include "bw_graph/mem/mem_g0map.h"

#include <algorithm>
#include <cstdint>

// Constructor
mem_sub_map_t::mem_sub_map_t() : total_edges(0), edge_count_valid(true) {}

// Constructor with adjacency map
mem_sub_map_t::mem_sub_map_t(const folly::F14FastMap<v_id_t, std::vector<v_id_t>>& adj_map)
    : adjacency_map(adj_map), total_edges(0), edge_count_valid(false) {
  update_edge_count();
}

// Helper function to check if a neighbor exists
bool mem_sub_map_t::has_neighbor(const std::vector<v_id_t>& neighbors, v_id_t target_id) const {
  return std::find(neighbors.begin(), neighbors.end(), target_id) != neighbors.end();
}

// Helper function to remove a neighbor
bool mem_sub_map_t::remove_neighbor(std::vector<v_id_t>& neighbors, v_id_t target_id) {
  auto it = std::find(neighbors.begin(), neighbors.end(), target_id);
  if (it != neighbors.end()) {
    neighbors.erase(it);
    return true;
  }
  return false;
}

// Build a subgraph from csr page;
mem_sub_map_t mem_sub_map_t::from_csr_page(csr_page_t& page) {
  mem_sub_map_t sub_map;

  auto vertex_span = page.get_vertices();
  uint64_t edge_count = 0;
  for (const auto& vertex : vertex_span) {
    v_id_t vertex_id = vertex.vertex_id;
    auto neighbor_span = page.get_neighbors(vertex.offset);
    edge_count += neighbor_span.size();
    std::vector<v_id_t> neighbors(neighbor_span.begin(), neighbor_span.end());
    sub_map.adjacency_map[vertex_id] = std::move(neighbors);
  }
  sub_map.total_edges = edge_count;
  return sub_map;
}

// Compute degree distribution
std::vector<std::pair<uint64_t, double>>
mem_sub_map_t::compute_degree_distribution(uint64_t step) const {
  if (adjacency_map.empty() || step == 0) {
    return {};
  }

  // Find maximum degree
  uint64_t max_degree = 0;
  for (const auto& pair : adjacency_map) {
    uint64_t degree = pair.second.size();
    max_degree = std::max(max_degree, degree);
  }

  // Calculate number of buckets needed
  uint64_t num_buckets = (max_degree / step) + 1;

  // Count vertices in each degree range
  std::vector<uint64_t> degree_counts(num_buckets, 0);

  for (const auto& pair : adjacency_map) {
    uint64_t degree = pair.second.size();
    uint64_t bucket_index = degree / step;
    degree_counts[bucket_index]++;
  }

  // Convert counts to percentages and build result
  std::vector<std::pair<uint64_t, double>> distribution;
  distribution.reserve(num_buckets);

  size_t total_vertices = adjacency_map.size();
  for (uint64_t i = 0; i < num_buckets; ++i) {
    if (degree_counts[i] > 0) {
      uint64_t range_start = i * step;
      double percentage = (static_cast<double>(degree_counts[i]) / total_vertices) * 100.0;
      distribution.emplace_back(range_start, percentage);
    }
  }

  return distribution;
}

// Update edge count cache
void mem_sub_map_t::update_edge_count() const {
  total_edges = 0;
  for (const auto& pair : adjacency_map) {
    total_edges += pair.second.size();
  }
  edge_count_valid = true;
}

// Get neighbors of a vertex
std::vector<v_id_t> mem_sub_map_t::get_neighbors(v_id_t vertex_id) const {
  auto it = adjacency_map.find(vertex_id);
  if (it == adjacency_map.end()) {
    // Vertex does not exist.
    return {};
  }
  return it->second;
}

// Get direct reference to neighbor vector
const std::vector<v_id_t>& mem_sub_map_t::get_neighbors_ref(v_id_t vertex_id) const {
  auto it = adjacency_map.find(vertex_id);
  if (it == adjacency_map.end()) {
    static std::vector<v_id_t> empty_vector;
    return empty_vector; // Return empty vector if vertex does not exist
  }
  return it->second;
}

// Check if vertex exists
bool mem_sub_map_t::has_vertex(v_id_t vertex_id) const {
  return adjacency_map.find(vertex_id) != adjacency_map.end();
}

// Check if edge exists
bool mem_sub_map_t::has_edge(v_id_t src_id, v_id_t dst_id) const {
  auto it = adjacency_map.find(src_id);
  if (it == adjacency_map.end()) {
    return false;
  }
  return has_neighbor(it->second, dst_id);
}

// Get list of all vertices
std::vector<v_id_t> mem_sub_map_t::vertex_list() const {
  std::vector<v_id_t> vertices;
  vertices.reserve(adjacency_map.size());
  for (const auto& pair : adjacency_map) {
    vertices.push_back(pair.first);
  }
  return vertices;
}

// Convert to adjacency map
folly::F14FastMap<v_id_t, std::vector<v_id_t>> mem_sub_map_t::to_map() const {
  return adjacency_map;
}

// Get vertex count
size_t mem_sub_map_t::vertex_count() const { return adjacency_map.size(); }

// Get edge count
size_t mem_sub_map_t::edge_count() const {
  if (!edge_count_valid) {
    update_edge_count();
  }
  return total_edges;
}

// Add vertex
bool mem_sub_map_t::add_vertex(v_id_t vertex_id) {
  if (has_vertex(vertex_id)) {
    return false;
  }
  adjacency_map[vertex_id] = std::vector<v_id_t>();
  return true;
}

// Add edge
bool mem_sub_map_t::add_edge(v_id_t src_id, v_id_t dst_id) {
  // Ensure both vertices exist
  if (!has_vertex(src_id)) {
    add_vertex(src_id);
  }
  if (!has_vertex(dst_id)) {
    add_vertex(dst_id);
  }

  // Check if edge already exists
  auto& neighbors = adjacency_map[src_id];
  if (has_neighbor(neighbors, dst_id)) {
    return false;
  }

  // Add the edge
  neighbors.push_back(dst_id);
  edge_count_valid = false;
  return true;
}

// Remove vertex
bool mem_sub_map_t::remove_vertex(v_id_t vertex_id) {
  auto it = adjacency_map.find(vertex_id);
  if (it == adjacency_map.end()) {
    return false;
  }

  // Remove all edges pointing to this vertex
  for (auto& pair : adjacency_map) {
    if (pair.first != vertex_id) {
      remove_neighbor(pair.second, vertex_id);
    }
  }

  // Remove the vertex itself
  adjacency_map.erase(it);
  edge_count_valid = false;
  return true;
}

// Remove edge
bool mem_sub_map_t::remove_edge(v_id_t src_id, v_id_t dst_id) {
  auto it = adjacency_map.find(src_id);
  if (it == adjacency_map.end()) {
    return false;
  }

  if (remove_neighbor(it->second, dst_id)) {
    edge_count_valid = false;
    return true;
  }
  return false;
}

// Clear all vertices and edges
void mem_sub_map_t::clear() {
  adjacency_map.clear();
  total_edges = 0;
  edge_count_valid = true;
}