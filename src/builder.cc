#include "bw_graph/io/builder.h"

#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"

#include <cassert>
#include <folly/container/F14Set.h>

// Build a CSR page builder
csr_page_builder_t::csr_page_builder_t(size_t page_size, uint16_t edge_slot_count)
    : page_size_(page_size), edge_slot_count_(edge_slot_count), current_edge_offset_(0) {
  // Pre-allocate reasonable capacity to reduce reallocation
  // Assume a page can hold approximately 256 vertices on average
  this->vertices_.reserve(256);
  this->degrees_.reserve(256);
  this->offsets_.reserve(256);

  // Assume each vertex has 8 neighbors on average, plus edge slots
  // 256 * (8 + edge_slot_count) is a reasonable initial capacity
  this->edges_.reserve(256 * (8 + edge_slot_count));

  this->giant_vertices_.reserve(128);
}

// Add vertex to this builder.
bool csr_page_builder_t::add_vertex(v_id_t vertex_id, neighbor_span_t& neighbors) {
  return add_vertex(vertex_id, neighbors, edge_slot_count_);
}

// Per-vertex slot overload used by the workload-driven SMO path.
bool csr_page_builder_t::add_vertex(v_id_t vertex_id, neighbor_span_t& neighbors,
                                    uint16_t per_vertex_slots) {
  // Check if this is a giant vertex (degree >= BW_GRAPH_GIANT_DEGREE_BOUND)
  uint32_t degree = static_cast<uint32_t>(neighbors.size());
  bool is_giant = degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND;

  // Calculate the size needed if we add this vertex
  size_t new_estimated_size = this->estimated_size();

  // Vertex entry overhead: 12 bytes (csr_vertex_t) - always needed
  new_estimated_size += sizeof(csr_vertex_t);

  if (!is_giant) {
    // Regular vertices: store neighbors + edge slots in page
    size_t neighbor_bytes = (degree + per_vertex_slots) * sizeof(v_id_t);
    new_estimated_size += neighbor_bytes;
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    // Per-edge timestamps for this vertex (upper bound: includes slots)
    new_estimated_size += (degree + per_vertex_slots) * sizeof(timestamp_t);
#endif
  }

  // Check if adding this vertex would exceed page size
  if (new_estimated_size > page_size_ && !vertices_.empty()) {
    return false;
  }

  // Add vertex ID (both giant and regular vertices)
  vertices_.push_back(vertex_id);

  // Add degree (both giant and regular vertices store their actual degree)
  degrees_.push_back(degree);

  if (is_giant) {
    // Giant vertex: neighbors stored externally (e.g., in giant_vertex_db)
    // Offset is set to 0 as a marker for giant vertices
    offsets_.push_back(0);

    // Track this as a giant vertex with its neighbors
    std::vector<v_id_t> neighbor_vec(neighbors.begin(), neighbors.end());
    giant_vertices_.push_back({vertex_id, std::move(neighbor_vec)});

    // Note: current_edge_offset_ doesn't change for giant vertices
    // since we don't store their neighbors in edges_

  } else {
    // Regular vertex: store neighbors in page
    // Record the byte offset where neighbors start
    if (current_edge_offset_ > UINT16_MAX) {
      throw std::runtime_error("Byte offset exceeds uint16_t limit");
    }
    offsets_.push_back(static_cast<uint16_t>(current_edge_offset_));

    // Add neighbors to the edge array
    edges_.insert(edges_.end(), neighbors.begin(), neighbors.end());

    // Add edge slots (filled with zeros) for future insertions
    for (uint16_t i = 0; i < per_vertex_slots; ++i) {
      edges_.push_back(0);
    }

    // Update byte offset for next vertex
    current_edge_offset_ += (degree + per_vertex_slots) * sizeof(v_id_t);
  }

  return true;
}

// Clear all the data for next page
void csr_page_builder_t::clear_all() {
  this->current_edge_offset_ = 0;
  this->edges_.clear();
  this->vertices_.clear();
  this->degrees_.clear();
  this->offsets_.clear();
}

// Check if the builder is empty.
bool csr_page_builder_t::is_empty() { return vertices_.empty(); }

// Estimate the size needed for the current page data.
size_t csr_page_builder_t::estimated_size() {
  // Metadata header: 16 bytes
  // [num_vertices: uint16_t, num_edges: uint16_t, used_bytes: uint32_t,
  //  parent_page_no: uint32_t, neighbor_page_count: uint32_t]
  const size_t metadata_size =
      sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

  // Neighbor block array: stores neighbor page numbers in hierarchy
  const size_t neighbor_block_size = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);

  // Vertex array: each vertex takes 12 bytes (csr_vertex_t)
  // This includes both regular and giant vertices
  const size_t vertex_array_size = vertices_.size() * sizeof(csr_vertex_t);

  // Neighbor data: only for regular vertices (not giant vertices)
  // edges_ only contains data for regular vertices
  const size_t neighbor_data_size = edges_.size() * sizeof(v_id_t);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Per-edge timestamp section: one timestamp_t per actual edge (not slots).
  // edges_.size() is an upper bound (includes slots), safe for size estimation.
  const size_t ts_data_size = edges_.size() * sizeof(timestamp_t);
  return metadata_size + neighbor_block_size + vertex_array_size + neighbor_data_size +
         ts_data_size;
#else
  return metadata_size + neighbor_block_size + vertex_array_size + neighbor_data_size;
#endif
}

// Build build.
std::pair<csr_page_t*, std::vector<std::pair<v_id_t, uint16_t>>> csr_page_builder_t::build() {
  // Check if builder is empty
  if (is_empty()) {
    throw std::runtime_error("Cannot build empty CSR page");
  }

  // Compute num_vertices and num_edges up front (needed for size calculation).
  size_t num_vertices = vertices_.size();
  size_t num_edges = 0;

  // Create giant vertex set for efficient lookup
  folly::F14FastSet<v_id_t> giant_set;
  for (const auto& [vid, neighbors] : giant_vertices_) {
    giant_set.insert(vid);
  }

  // Sum up degrees of non-giant vertices
  for (size_t i = 0; i < vertices_.size(); ++i) {
    if (giant_set.find(vertices_[i]) == giant_set.end()) {
      num_edges += degrees_[i];
    }
  }

  // Check limits before allocating
  if (num_vertices > UINT16_MAX) {
    throw std::runtime_error("Vertex count exceeds uint16_t limit");
  }
  if (num_edges > UINT16_MAX) {
    throw std::runtime_error("Edge count exceeds uint16_t limit");
  }

  // Calculate sizes
  const size_t metadata_size =
      sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
  const size_t neighbor_block_size = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);
  const size_t vertex_array_size = vertices_.size() * sizeof(csr_vertex_t);
  const size_t neighbor_data_size = edges_.size() * sizeof(v_id_t);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Parallel to neighbor array (includes slot entries), same length as edges_.
  const size_t ts_data_size = edges_.size() * sizeof(timestamp_t);
#else
  const size_t ts_data_size = 0;
#endif
  const size_t total_size =
      metadata_size + neighbor_block_size + vertex_array_size + neighbor_data_size + ts_data_size;

  // Verify size doesn't exceed page size
  if (total_size > page_size_) {
    throw std::runtime_error("CSR data too large for page: " + std::to_string(total_size) +
                             " bytes required, " + std::to_string(page_size_) + " bytes available");
  }

  // Create a new CSR page
  csr_page_t* page = new csr_page_t();

  // Get page data pointer
  char* page_data = page->get_data();
  if (page_data == nullptr) {
    delete page;
    throw std::runtime_error("Failed to get page data for CSR construction");
  }

  // Clear page data first
  std::memset(page_data, 0, page_size_);

  // Write compact metadata header
  char* ptr = page_data;
  *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(num_vertices);
  ptr += sizeof(uint16_t);
  *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(num_edges);
  ptr += sizeof(uint16_t);
  *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(total_size);
  ptr += sizeof(uint32_t);
  *reinterpret_cast<uint32_t*>(ptr) = 0; // parent_page_no
  ptr += sizeof(uint32_t);
  *reinterpret_cast<uint32_t*>(ptr) = 0; // neighbor_page_count
  ptr += sizeof(uint32_t);

  // Initialize neighbor block (all zeros)
  std::memset(page_data + metadata_size, 0, neighbor_block_size);

  // Write Vertex Array
  csr_vertex_t* vertex_array =
      reinterpret_cast<csr_vertex_t*>(page_data + metadata_size + neighbor_block_size);

  // Build vertex map: (vertex_id, index_in_page)
  std::vector<std::pair<v_id_t, uint16_t>> vertex_map;
  vertex_map.reserve(vertices_.size());

  for (size_t i = 0; i < vertices_.size(); ++i) {
    // Fill vertex structure directly from the three arrays
    vertex_array[i].vertex_id = vertices_[i];
    vertex_array[i].degree = degrees_[i];
    vertex_array[i].offset = offsets_[i];
    vertex_array[i].delete_mark = 0; // Not deleted

    // Add to vertex map
    vertex_map.emplace_back(vertices_[i], static_cast<uint16_t>(i));
  }

  // Write Neighbor Data
  v_id_t* neighbor_data = reinterpret_cast<v_id_t*>(page_data + metadata_size +
                                                    neighbor_block_size + vertex_array_size);

  // Copy all edges (only contains regular vertices' neighbors + edge slots)
  std::memcpy(neighbor_data, edges_.data(), neighbor_data_size);

  // Parse the page
  page->self_parse();

  // Clear builder data for next page
  this->clear_all();

  return {page, std::move(vertex_map)};
}
