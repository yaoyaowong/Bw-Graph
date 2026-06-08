#include "bw_graph/mem/mem_g0csr.h"

#include <algorithm>

// Construct mem_sub_csr_t.
mem_sub_csr_t::mem_sub_csr_t() {
  // Default constructor - creates empty CSR graph
}

// Construct mem_sub_csr_t.
mem_sub_csr_t::mem_sub_csr_t(const folly::F14FastMap<v_id_t, std::vector<v_id_t>>& adj_map) {
  // Clear existing data
  vertex_index.clear();
  offsets.clear();
  neighbor_list.clear();

  if (adj_map.empty()) {
    return;
  }

  // Create sorted list of vertex IDs for consistent ordering
  std::vector<v_id_t> sorted_vertices;
  sorted_vertices.reserve(adj_map.size());
  for (const auto& pair : adj_map) {
    sorted_vertices.push_back(pair.first);
  }
  std::sort(sorted_vertices.begin(), sorted_vertices.end());

  // Build vertex index mapping
  for (size_t i = 0; i < sorted_vertices.size(); ++i) {
    vertex_index[sorted_vertices[i]] = i;
  }

  // Build offsets and neighbor_list arrays
  offsets.reserve(sorted_vertices.size() + 1);
  size_t current_offset = 0;

  for (v_id_t vertex_id : sorted_vertices) {
    offsets.push_back(current_offset);

    const auto& neighbors = adj_map.at(vertex_id);
    for (v_id_t neighbor_id : neighbors) {
      neighbor_list.push_back(neighbor_id);
    }
    current_offset += neighbors.size();
  }

  // Add final offset for boundary checking
  offsets.push_back(current_offset);
}

// Get neighbors.
std::vector<v_id_t> mem_sub_csr_t::get_neighbors(v_id_t vertex_id) const {
  auto it = vertex_index.find(vertex_id);
  if (it == vertex_index.end()) {
    return {}; // Return empty vector if vertex does not exist
  }

  size_t vertex_inner_id = it->second;
  size_t start_pos = offsets[vertex_inner_id];
  size_t end_pos = offsets[vertex_inner_id + 1];

  return std::vector<v_id_t>(neighbor_list.begin() + start_pos, neighbor_list.begin() + end_pos);
}

// Check vertex.
bool mem_sub_csr_t::has_vertex(v_id_t vertex_id) const {
  return vertex_index.find(vertex_id) != vertex_index.end();
}

// Check edge.
bool mem_sub_csr_t::has_edge(v_id_t src_id, v_id_t dst_id) const {
  auto it = vertex_index.find(src_id);
  if (it == vertex_index.end()) {
    return false;
  }

  size_t vertex_pos = it->second;
  size_t start_pos = offsets[vertex_pos];
  size_t end_pos = offsets[vertex_pos + 1];

  // Binary search for the destination vertex in the neighbor_list list
  // Note: This assumes neighbors are sorted. If not, use linear search instead.
  return std::binary_search(neighbor_list.begin() + start_pos, neighbor_list.begin() + end_pos,
                            dst_id);
}

// Handle vertex list.
std::vector<v_id_t> mem_sub_csr_t::vertex_list() const {
  std::vector<v_id_t> vertices;
  vertices.reserve(vertex_index.size());

  for (const auto& pair : vertex_index) {
    vertices.push_back(pair.first);
  }

  return vertices;
}

// Handle to map.
folly::F14FastMap<v_id_t, std::vector<v_id_t>> mem_sub_csr_t::to_map() const {
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> adj_map;

  for (const auto& pair : vertex_index) {
    v_id_t vertex_id = pair.first;
    size_t vertex_pos = pair.second;

    size_t start_pos = offsets[vertex_pos];
    size_t end_pos = offsets[vertex_pos + 1];

    adj_map[vertex_id] =
        std::vector<v_id_t>(neighbor_list.begin() + start_pos, neighbor_list.begin() + end_pos);
  }

  return adj_map;
}

// Handle vertex count.
size_t mem_sub_csr_t::vertex_count() const { return vertex_index.size(); }

// Handle edge count.
size_t mem_sub_csr_t::edge_count() const { return neighbor_list.size(); }