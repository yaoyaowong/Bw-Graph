#include "bw_graph/io/io_csr.h"

#include "bw_graph/common/config.h"
#include "bw_graph/common/ptv_codec.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

// Default constructor
csr_page_t::csr_page_t() : page_t(), num_vertices_(0), num_edges_(0), neighbor_ptr_(nullptr) {}

// Build from adj list iterator
csr_page_t::csr_page_t(const page_no_t& page_no, const adj_map_iter_t& adjacency_map,
                       std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                       uint16_t edge_slot_count)
    : page_t(page_no), neighbor_ptr_(nullptr) {
  build_from_adjacency_map(adjacency_map, vertex_map, edge_slot_count);
}

// Per-vertex slot constructor.
csr_page_t::csr_page_t(const page_no_t& page_no, const adj_map_iter_t& adjacency_map,
                       std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                       const folly::F14FastMap<v_id_t, uint16_t>& per_vertex_slots)
    : page_t(page_no), neighbor_ptr_(nullptr) {
  build_from_adjacency_map(adjacency_map, vertex_map, per_vertex_slots);
}

// Move constructor
csr_page_t::csr_page_t(page_t&& page)
    : page_t(std::move(page)), num_vertices_(0), num_edges_(0), vertex_span_(),
      neighbor_ptr_(nullptr) {
  // Parse the CSR structure from the moved data
  parse_from_page_data();
}

// Get neighbor operator (uncompressed path only)
neighbor_span_t csr_page_t::get_neighbors(uint16_t vertex_offset) {
  if (vertex_offset >= num_vertices_) {
    return neighbor_span_t();
  }

  const csr_vertex_t& vertex = vertex_span_[vertex_offset];
  if (vertex.degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
    return neighbor_span_t();
  }

  uint64_t neighbor_element_offset = vertex.offset / sizeof(v_id_t);
  return neighbor_span_t(const_cast<v_id_t*>(neighbor_ptr_ + neighbor_element_offset),
                         vertex.degree);
}

// Get PTV-decoded neighbor range (compressed path)
bw_graph::ptv_neighbor_range_t csr_page_t::get_neighbor_ptv_range(uint16_t vertex_offset) {
  if (vertex_offset >= num_vertices_) {
    return bw_graph::ptv_neighbor_range_t(nullptr, 0u);
  }

  const csr_vertex_t& vertex = vertex_span_[vertex_offset];
  if (vertex.degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
    return bw_graph::ptv_neighbor_range_t(nullptr, 0u);
  }

  const uint8_t* base = reinterpret_cast<const uint8_t*>(neighbor_ptr_) + vertex.offset;
  return bw_graph::ptv_neighbor_range_t(base, vertex.degree);
}

// Get degree operator
uint64_t csr_page_t::get_vertex_degree(uint16_t vertex_offset) {
  if (vertex_offset >= num_vertices_) {
    return 0;
  }

  return vertex_span_[vertex_offset].degree;
}

// Get vertex view
vertex_span_t csr_page_t::get_vertices() { return vertex_span_; }

// Get vertex count
uint64_t csr_page_t::get_num_vertices() { return num_vertices_; }

// Get edge count
uint64_t csr_page_t::get_num_edges() { return num_edges_; }

// Build a paged csr from adj map
void csr_page_t::build_from_adjacency_map(const adj_map_iter_t& adjacency_map,
                                          std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                                          uint16_t edge_slot_count) {
  // Delegate to the per-vertex variant with a uniform map so the layout
  // logic only lives in one place. When edge_slot_count == 0 we skip the
  // map construction altogether so the hot CSR-build path on the legacy
  // uniform slot == 0 branch incurs zero per-vertex bookkeeping.
  if (edge_slot_count == 0) {
    folly::F14FastMap<v_id_t, uint16_t> empty;
    build_from_adjacency_map(adjacency_map, vertex_map, empty);
    return;
  }
  folly::F14FastMap<v_id_t, uint16_t> uniform_slots;
  uniform_slots.reserve(adjacency_map.size());
  for (const auto& [vid, _] : adjacency_map) {
    uniform_slots.emplace(vid, edge_slot_count);
  }
  build_from_adjacency_map(adjacency_map, vertex_map, uniform_slots);
}

// Per-vertex slot variant.
void csr_page_t::build_from_adjacency_map(
    const adj_map_iter_t& adjacency_map,
    std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
    const folly::F14FastMap<v_id_t, uint16_t>& per_vertex_slots) {
  // Handle empty adjacency map case
  if (adjacency_map.empty()) {
    num_vertices_ = 0;
    num_edges_ = 0;
    used_bytes_ = 0;
    parent_page_no_ = 0;
    neighbor_page_count_ = 0;
    vertex_span_ = vertex_span_t();
    neighbor_ptr_ = nullptr;
    neighbor_page_span_ = block_span_t();

    // Write empty metadata to page
    char* page_data = get_data();
    if (page_data == nullptr) {
      throw std::runtime_error("Failed to get page data for CSR construction");
    }

    std::memset(page_data, 0, static_cast<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE));

    // Write compact metadata layout
    char* ptr = page_data;
    *reinterpret_cast<uint16_t*>(ptr) = 0; // vertex count
    ptr += sizeof(uint16_t);
    *reinterpret_cast<uint16_t*>(ptr) = 0; // edge count
    ptr += sizeof(uint16_t);
    *reinterpret_cast<uint32_t*>(ptr) = 16; // used bytes (just metadata)
    ptr += sizeof(uint32_t);
    *reinterpret_cast<uint32_t*>(ptr) = 0; // parent page no
    ptr += sizeof(uint32_t);
    *reinterpret_cast<uint32_t*>(ptr) = 0; // neighbor page count

    return;
  }

  // Build vertices and neighbors vectors
  std::vector<csr_vertex_t> vertices;
  vertices.reserve(adjacency_map.size());

  uint32_t total_edge_count = 0;
  uint32_t total_vertex_count = static_cast<uint32_t>(adjacency_map.size());
  uint32_t current_byte_offset = 0;
  uint16_t page_offset = 0;

  // Storage for neighbor data (raw v_id_t array)
  std::vector<uint8_t> all_neighbor_bytes;
  all_neighbor_bytes.reserve(adjacency_map.size() * 10);

  // Process each vertex (no sorting, maintain input order)
  for (const auto& [vertex_id, neighbors] : adjacency_map) {
    // Per-vertex slot count: defaults to 0 when the caller did not specify
    // a reservation for this vertex (e.g. the legacy uniform slot == 0
    // path passes an empty map).
    uint16_t this_slot = 0;
    auto slot_it = per_vertex_slots.find(vertex_id);
    if (slot_it != per_vertex_slots.end()) {
      this_slot = slot_it->second;
    }

    // Check degree limit for uint32_t
    if (neighbors.size() > UINT32_MAX) {
      throw std::runtime_error("Vertex degree exceeds uint32_t limit");
    }

    uint32_t degree = static_cast<uint32_t>(neighbors.size());

    // Check if this is a giant vertex (degree >= GIANT_DEGREE_BOUND)
    if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      // Giant vertex: only store vertex metadata, no neighbor data
      // offset points to a special location (e.g., 0 or UINT16_MAX)
      csr_vertex_t vertex(vertex_id, degree,
                          0,  // Giant vertices use offset = 0 as marker
                          0); // delete_mark = 0 (exists)

      vertices.push_back(vertex);
      vertex_map.push_back({vertex_id, page_offset++});

      // Note: neighbors are NOT stored in this page for giant vertices
      // They should be handled separately (e.g., in a dedicated giant vertex
      // DB)

    } else {
      // Normal vertex: store both metadata and neighbor data

      size_t neighbor_bytes;
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
      // PTV-compressed: compute exact byte count for each neighbor
      uint32_t compressed_bytes = 0;
      for (size_t i = 0; i < degree; ++i) {
        compressed_bytes += bw_graph::ptv_encoded_size(neighbors[i]);
      }
      // Slots are always 4-byte PTV entries (tag 11)
      neighbor_bytes = compressed_bytes + static_cast<size_t>(this_slot) * 4;
#else
      neighbor_bytes = (degree + this_slot) * sizeof(v_id_t);
#endif

      // Check offset limit for uint16_t
      if (current_byte_offset + neighbor_bytes > UINT16_MAX) {
        throw std::runtime_error("Neighbor data offset exceeds uint16_t limit (65535 bytes). "
                                 "Page has too much neighbor data: " +
                                 std::to_string(current_byte_offset + neighbor_bytes) + " bytes");
      }

      // Create vertex entry
      csr_vertex_t vertex(vertex_id, degree, static_cast<uint16_t>(current_byte_offset),
                          0); // delete_mark = 0 (exists)

      total_edge_count += degree;
      vertices.push_back(vertex);
      vertex_map.push_back({vertex_id, page_offset++});

      // Resize to make room for this vertex's neighbors
      size_t current_size = all_neighbor_bytes.size();
      all_neighbor_bytes.resize(current_size + neighbor_bytes);

      uint8_t* raw_ptr = all_neighbor_bytes.data() + current_size;

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
      // Write PTV-encoded neighbors
      uint32_t written = 0;
      for (size_t i = 0; i < degree; ++i) {
        written += bw_graph::ptv_write(raw_ptr + written, neighbors[i]);
      }
      // Fill slots as 4-byte PTV entries with value 0 (empty marker)
      for (uint16_t s = 0; s < this_slot; ++s) {
        bw_graph::ptv_write_slot(raw_ptr + written, 0);
        written += 4;
      }
#else
      // Write neighbors as raw v_id_t values
      v_id_t* neighbor_ptr = reinterpret_cast<v_id_t*>(raw_ptr);
      for (size_t i = 0; i < degree; ++i) {
        neighbor_ptr[i] = neighbors[i];
      }
      // Fill edge slots with zeros (for future insertions)
      for (size_t i = degree; i < degree + this_slot; ++i) {
        neighbor_ptr[i] = 0;
      }
#endif

      // Update byte offset for next vertex
      current_byte_offset += neighbor_bytes;
    }
  }

  // Check vertex and edge count limits
  if (total_vertex_count > UINT16_MAX) {
    throw std::runtime_error("Vertex count exceeds uint16_t limit: " +
                             std::to_string(total_vertex_count));
  }
  if (total_edge_count > UINT16_MAX) {
    throw std::runtime_error("Edge count exceeds uint16_t limit: " +
                             std::to_string(total_edge_count));
  }

  // Calculate memory layout
  // Metadata: uint16_t + uint16_t + uint32_t + uint32_t + uint32_t = 16 bytes
  const size_t metadata_size =
      sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
  const size_t neighbor_block_data_size = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);
  size_t vertex_data_size = vertices.size() * sizeof(csr_vertex_t);
  size_t neighbor_data_size = all_neighbor_bytes.size();
  size_t total_required =
      metadata_size + neighbor_block_data_size + vertex_data_size + neighbor_data_size;

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Timestamp section is parallel to the neighbor array (same entry count, includes slots).
  // Each entry is timestamp_t; INVALID_TS (0) means always-visible.
  const size_t ts_data_size = neighbor_data_size / sizeof(v_id_t) * sizeof(timestamp_t);
  total_required += ts_data_size;
#endif

  std::vector<page_no_t> neighbor_pages(bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR, 0);

  // Check if page has enough space
  if (total_required > static_cast<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE)) {
    throw std::runtime_error(
        "CSR data too large for single page: " + std::to_string(total_required) +
        " bytes required, " + std::to_string(bw_graph::BW_GRAPH_PAGE_SIZE) + " bytes available");
  }

  used_bytes_ = static_cast<uint32_t>(total_required);

  // Get page data pointer
  char* page_data = get_data();

  // Clear page data first (zeros = INVALID_TS for timestamp section)
  std::memset(page_data, 0, static_cast<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE));

  // Write compact metadata header
  char* ptr = page_data;
  *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(total_vertex_count);
  ptr += sizeof(uint16_t);
  *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(total_edge_count);
  ptr += sizeof(uint16_t);
  *reinterpret_cast<uint32_t*>(ptr) = used_bytes_;
  ptr += sizeof(uint32_t);
  *reinterpret_cast<uint32_t*>(ptr) = parent_page_no_;
  ptr += sizeof(uint32_t);
  *reinterpret_cast<uint32_t*>(ptr) = neighbor_page_count_;
  ptr += sizeof(uint32_t);

  // Copy neighbor block data after metadata
  std::memcpy(page_data + metadata_size, neighbor_pages.data(), neighbor_block_data_size);

  // Copy vertex data after neighbor block
  std::memcpy(page_data + metadata_size + neighbor_block_data_size, vertices.data(),
              vertex_data_size);

  // Copy neighbor data (raw v_id_t array) after vertex data
  std::memcpy(page_data + metadata_size + neighbor_block_data_size + vertex_data_size,
              all_neighbor_bytes.data(), neighbor_data_size);

  // Set member variables
  num_vertices_ = static_cast<uint16_t>(total_vertex_count);
  num_edges_ = static_cast<uint16_t>(total_edge_count);
  is_parse_ = true;

  // Initialize spans - neighbor page after metadata
  page_no_t* neighbor_page_ptr = reinterpret_cast<page_no_t*>(page_data + metadata_size);
  neighbor_page_span_ = block_span_t(neighbor_page_ptr, neighbor_page_count_);

  // Initialize spans - vertices start after neighbor block
  csr_vertex_t* vertex_ptr =
      reinterpret_cast<csr_vertex_t*>(page_data + metadata_size + neighbor_block_data_size);
  vertex_span_ = vertex_span_t(vertex_ptr, num_vertices_);

  // Set neighbor pointer to the neighbor data section (raw v_id_t array)
  neighbor_ptr_ = reinterpret_cast<v_id_t*>(page_data + metadata_size + neighbor_block_data_size +
                                            vertex_data_size);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Timestamp section is zeroed (= INVALID_TS) by the memset above.
  // It is parallel to the neighbor array: same entry count (including slots).
  // Access: edge_ts_ptr_[vertex.offset / sizeof(v_id_t) + j]
  edge_ts_ptr_ = reinterpret_cast<timestamp_t*>(
      page_data + metadata_size + neighbor_block_data_size + vertex_data_size + neighbor_data_size);
#endif
}

// Partial parse from the page data
void csr_page_t::parse_from_page_data() {
  char* page_data = this->get_data();

  if (page_data == nullptr) {
    throw std::runtime_error("Failed to get page data for parsing");
  }

  // Initialize to empty state first
  num_vertices_ = 0;
  num_edges_ = 0;
  used_bytes_ = 0;
  parent_page_no_ = 0;
  neighbor_page_count_ = 0;
  vertex_span_ = vertex_span_t();
  neighbor_ptr_ = nullptr;
  neighbor_page_span_ = block_span_t();

  // Read compact metadata from first 16 bytes
  char* ptr = page_data;
  uint16_t vertex_count = *reinterpret_cast<const uint16_t*>(ptr);
  ptr += sizeof(uint16_t);
  uint16_t edge_count = *reinterpret_cast<const uint16_t*>(ptr);
  ptr += sizeof(uint16_t);
  uint32_t used_bytes = *reinterpret_cast<const uint32_t*>(ptr);
  ptr += sizeof(uint32_t);
  uint32_t parent_page_no = *reinterpret_cast<const uint32_t*>(ptr);
  ptr += sizeof(uint32_t);
  uint32_t neighbor_page_count = *reinterpret_cast<const uint32_t*>(ptr);
  ptr += sizeof(uint32_t);

  // Validate metadata
  if (vertex_count == 0 && edge_count == 0) {
    // Empty CSR page is valid
    is_parse_ = true;
    return;
  }

  if (vertex_count == 0 && edge_count > 0) {
    // Invalid: edges without vertices
    throw std::runtime_error("Invalid CSR page: edge_count > 0 but vertex_count == 0");
  }

  if (neighbor_page_count > bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR) {
    // Invalid: Neighbor count is too large
    throw std::runtime_error("Invalid CSR page: neighbor_page_count (" +
                             std::to_string(neighbor_page_count) + ") exceeds max (" +
                             std::to_string(bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR) + ")");
  }

  // Calculate layout offsets
  const size_t metadata_size = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) +
                               sizeof(uint32_t) + sizeof(uint32_t); // = 16 bytes
  const size_t neighbor_block_data_size = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);
  size_t vertex_data_size = vertex_count * sizeof(csr_vertex_t);

  // Validate used_bytes against the base minimum (without timestamp section so that
  // legacy pages written before per-edge timestamps were introduced still load correctly).
  size_t minimum_required = metadata_size + neighbor_block_data_size + vertex_data_size;
  if (used_bytes < minimum_required) {
    throw std::runtime_error("Invalid CSR page: used_bytes (" + std::to_string(used_bytes) +
                             ") is less than minimum required (" +
                             std::to_string(minimum_required) + ")");
  }

  if (used_bytes > static_cast<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE)) {
    throw std::runtime_error("Invalid CSR page: used_bytes (" + std::to_string(used_bytes) +
                             ") exceeds page size (" +
                             std::to_string(bw_graph::BW_GRAPH_PAGE_SIZE) + ")");
  }

  // All validations passed, set up the CSR structure
  this->num_vertices_ = vertex_count;
  this->num_edges_ = edge_count;
  this->used_bytes_ = used_bytes;
  this->parent_page_no_ = parent_page_no;
  this->neighbor_page_count_ = neighbor_page_count;

  // Set up neighbor page span - starts after metadata
  page_no_t* neighbor_page_ptr = reinterpret_cast<page_no_t*>(page_data + metadata_size);
  neighbor_page_span_ = block_span_t(neighbor_page_ptr, neighbor_page_count_);

  // Set up vertex span - starts after metadata + neighbor block
  csr_vertex_t* vertex_ptr =
      reinterpret_cast<csr_vertex_t*>(page_data + metadata_size + neighbor_block_data_size);
  vertex_span_ = vertex_span_t(vertex_ptr, num_vertices_);

  // Set up neighbor data pointer - starts after metadata + neighbor block + vertices
  neighbor_ptr_ = reinterpret_cast<v_id_t*>(page_data + metadata_size + neighbor_block_data_size +
                                            vertex_data_size);

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  // Timestamp array is parallel to the neighbor array (same entry count, includes slots).
  // remaining bytes = neighbor_data + ts_data, ratio ts:neighbor =
  // sizeof(timestamp_t):sizeof(v_id_t) = 2:1. So neighbor_data = remaining / 3 (always divisible:
  // each entry contributes 4+8=12 bytes).
  size_t fixed_size = metadata_size + neighbor_block_data_size + vertex_data_size;
  size_t remaining = (used_bytes > fixed_size) ? (used_bytes - fixed_size) : 0;
  size_t neighbor_data_sz = remaining * sizeof(v_id_t) / (sizeof(v_id_t) + sizeof(timestamp_t));
  edge_ts_ptr_ = reinterpret_cast<timestamp_t*>(page_data + fixed_size + neighbor_data_sz);
#endif

  is_parse_ = true;
}

// Check valid csr data.
bool csr_page_t::is_valid_csr_data() const {
  if (num_vertices_ == 0) {
    return true; // Empty CSR is valid
  }

  // Check if spans are valid
  if (vertex_span_.empty() || neighbor_ptr_ == nullptr) {
    return false;
  }

  // When compression is enabled, offsets are variable-length byte positions
  // and cannot be validated with fixed sizeof(v_id_t) arithmetic.
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  return true;
#endif

  // Uncompressed: check vertex offsets are consecutive sizeof(v_id_t) steps
  uint64_t expected_offset = 0;
  for (uint64_t i = 0; i < num_vertices_; ++i) {
    const csr_vertex_t& vertex = vertex_span_[i];

    if (vertex.offset != expected_offset) {
      return false;
    }

    expected_offset += vertex.degree * sizeof(v_id_t);
  }

  // Check that total neighbor data doesn't exceed what we expect
  if (expected_offset != num_edges_ * sizeof(v_id_t)) {
    return false;
  }

  return true;
}

// Calculate byte size for PTV-compressed neighbor list (no slots)
uint64_t csr_page_t::calculate_compressed_size(std::vector<v_id_t>& neighbors) {
  uint64_t total = 0;
  for (v_id_t id : neighbors) {
    total += bw_graph::ptv_encoded_size(id);
  }
  return total;
}

// Self Parse Implementation
void csr_page_t::self_parse() {
  if (is_parse_) {
    return; // Already parsed
  }
  parse_from_page_data();
  is_parse_ = true; // Mark as parsed
}

// Disable parse.
void csr_page_t::disable_parse() {
  is_parse_ = false; // Allow parsing again
}

#ifdef BW_GRAPH_ENABLE_TRANSACTION
// Get edge ts.
timestamp_t csr_page_t::get_edge_ts(uint16_t vertex_offset, uint32_t j) const {
  if (edge_ts_ptr_ == nullptr || vertex_offset >= num_vertices_) {
    return INVALID_TS;
  }
  // Timestamp array is parallel to neighbor array; use the same byte offset.
  uint32_t base = vertex_span_[vertex_offset].offset / sizeof(v_id_t);
  return edge_ts_ptr_[base + j];
}

// Write edge timestamps.
void csr_page_t::write_edge_timestamps(const edge_ts_map_t& ts_map) {
  if (edge_ts_ptr_ == nullptr) {
    return;
  }
  for (uint16_t vi = 0; vi < num_vertices_; ++vi) {
    const csr_vertex_t& v = vertex_span_[vi];
    if (v.degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      continue;
    }
    auto it = ts_map.find(v.vertex_id);
    if (it == ts_map.end()) {
      continue; // leave INVALID_TS (always visible)
    }
    const std::vector<timestamp_t>& ts_vec = it->second;
    uint32_t base = v.offset / sizeof(v_id_t);
    uint32_t copy_count = static_cast<uint32_t>(std::min<size_t>(ts_vec.size(), v.degree));
    for (uint32_t j = 0; j < copy_count; ++j) {
      edge_ts_ptr_[base + j] = ts_vec[j];
    }
  }
  mark_dirty();
}
#endif // BW_GRAPH_ENABLE_TRANSACTION

// Handle reinit.
void csr_page_t::reinit(page_no_t page_no) {
  // CSR pages are loaded from disk, so reinit is only used by buf_page_new
  // in tests or unusual paths. Just reset identity and parse flag.
  set_page_no(page_no);
  is_parse_ = false;
}

// Insert edge.
insert_status_t csr_page_t::insert_edge(uint16_t vertex_offset, v_id_t src, v_id_t dest) {
  // Validate parse state
  if (!is_parse_) {
    return FAILED; // Page not parsed
  }

  // In-place insertion into a variable-width PTV stream is not supported.
  // The caller must route through the delta chain instead.
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  return SLOT_FULL;
#endif

  // Check if vertex_offset is valid
  if (vertex_offset >= num_vertices_) {
    return FAILED; // Invalid vertex offset
  }

  csr_vertex_t* vertex_item = &vertex_span_[vertex_offset];

  // Verify source vertex ID matches
  if (src != vertex_item->vertex_id) {
    return FAILED; // Source vertex ID mismatch
  }

  // Check if vertex is deleted
  if (vertex_item->is_deleted()) {
    return FAILED; // Cannot insert edge to deleted vertex
  }

  // Check if this is a giant vertex
  if (vertex_item->degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
    return FAILED; // Giant vertices are handled externally
  }

  // Check for duplicate edge (optional, depends on your requirements)
  v_id_t* neighbors_start =
      reinterpret_cast<v_id_t*>(reinterpret_cast<uint8_t*>(neighbor_ptr_) + vertex_item->offset);

  for (uint32_t i = 0; i < vertex_item->degree; ++i) {
    if (neighbors_start[i] == dest) {
      return FAILED; // Duplicate edge (or return a specific status)
    }
  }

  // Calculate the neighbor data boundaries for this vertex
  // neighbor_start_idx: where this vertex's neighbor data begins (in v_id_t
  // units)
  uint32_t neighbor_start_idx = vertex_item->offset / sizeof(v_id_t);
  uint32_t neighbor_end_idx = neighbor_start_idx + vertex_item->degree;

  // Find the end boundary (where we can insert without overwriting next
  // vertex's data)
  uint32_t neighbor_capacity_end_idx;

  if (vertex_offset + 1 < num_vertices_) {
    // Not the last vertex - check next vertex's offset
    const csr_vertex_t* next_vertex = &vertex_span_[vertex_offset + 1];
    neighbor_capacity_end_idx = next_vertex->offset / sizeof(v_id_t);
  } else {
    // Last vertex - capacity extends to end of used neighbor data
    const size_t metadata_size = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t); // = 16 bytes
    const size_t neighbor_block_data_size =
        bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);
    size_t vertex_data_size = num_vertices_ * sizeof(csr_vertex_t);

    // Total neighbor data size
    size_t neighbor_data_size =
        used_bytes_ - metadata_size - neighbor_block_data_size - vertex_data_size;
    neighbor_capacity_end_idx = neighbor_data_size / sizeof(v_id_t);
  }

  // Check if there's available slot space
  if (neighbor_end_idx >= neighbor_capacity_end_idx) {
    return SLOT_FULL; // No space to insert new edge
  }

  // Check if inserting would exceed uint16_t offset limit
  uint32_t new_degree = vertex_item->degree + 1;
  uint32_t new_end_offset = (neighbor_start_idx + new_degree) * sizeof(v_id_t);
  if (new_end_offset > UINT16_MAX) {
    return SLOT_FULL; // Would exceed uint16_t offset limit
  }

  // Check if total edges would exceed uint16_t limit
  if (num_edges_ >= UINT16_MAX) {
    return FAILED; // Total edge count would exceed uint16_t limit
  }

  // Insert the new edge by shifting existing neighbors
  // We need to insert at the beginning to maintain sorted order (if required)
  // Or at the end for simplicity - assuming insertion at end here

  // Option 1: Insert at the end (simpler, faster)
  neighbor_ptr_[neighbor_end_idx] = dest;

  // Option 2: Insert at the beginning (uncomment if you need sorted neighbors)
  /*
  for (uint32_t i = neighbor_end_idx; i > neighbor_start_idx; --i) {
    neighbor_ptr_[i] = neighbor_ptr_[i - 1];
  }
  neighbor_ptr_[neighbor_start_idx] = dest;
  */

  // Update vertex metadata
  vertex_item->degree = new_degree;

  // Update page metadata
  num_edges_++;

  // Update used_bytes if we expanded into previously unused space
  const size_t metadata_size_final = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) +
                                     sizeof(uint32_t) + sizeof(uint32_t); // = 16 bytes
  uint32_t required_bytes =
      metadata_size_final + bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t) +
      num_vertices_ * sizeof(csr_vertex_t) + (neighbor_end_idx + 1) * sizeof(v_id_t);
  if (required_bytes > used_bytes_) {
    used_bytes_ = required_bytes;

    // Update used_bytes in page data
    char* page_data = get_data();
    *reinterpret_cast<uint32_t*>(page_data + 4) = used_bytes_;
  }

  // Update edge count in page data
  char* page_data = get_data();
  *reinterpret_cast<uint16_t*>(page_data + 2) = num_edges_;

  return INSERTED; // Edge successfully inserted
}

// Apply deltas.
insert_status_t
csr_page_t::apply_deltas(std::span<delta_record_t> deltas,
                         std::vector<std::pair<v_id_t, uint16_t>>& vertex_map,
                         std::vector<std::pair<v_id_t, std::vector<v_id_t>>>& giant_vertex_ids) {
  // Step 1 - Fetch all vertices and edges from the old csr page
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> old_graph;
  old_graph.reserve(this->vertex_span_.size());

  // For all old vertices
  uint16_t current_vertex_idx = 0;
  for (const auto& vertex_item : this->vertex_span_) {
    // Skip deleted vertices
    if (vertex_item.is_deleted()) {
      ++current_vertex_idx;
      continue;
    }

    // Initialize empty neighbor list
    if (!old_graph.contains(vertex_item.vertex_id)) {
      old_graph[vertex_item.vertex_id] = std::vector<v_id_t>();
    }

    // Handle giant vertices separately
    if (vertex_item.degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      // Giant vertex: neighbors are stored externally
      // Keep the vertex but with empty neighbor list
      old_graph[vertex_item.vertex_id] = std::vector<v_id_t>();
    } else {
      // Normal vertex: read neighbors from page
      try {
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
        auto range = this->get_neighbor_ptv_range(current_vertex_idx);
        std::vector<v_id_t> neighbors;
        neighbors.reserve(vertex_item.degree);
        for (v_id_t n : range) {
          neighbors.push_back(n);
        }
        old_graph[vertex_item.vertex_id] = std::move(neighbors);
#else
        neighbor_span_t neighbors_span = this->get_neighbors(current_vertex_idx);
        old_graph[vertex_item.vertex_id] =
            std::vector<v_id_t>(neighbors_span.begin(), neighbors_span.end());
#endif
      } catch (const std::runtime_error& e) {
        // If neighbor read fails (e.g., for giant vertex), use empty list
        old_graph[vertex_item.vertex_id] = std::vector<v_id_t>();
      }
    }
    ++current_vertex_idx;
  }

  // Step 2 - Apply the delta records
  for (const delta_record_t& delta_record : deltas) {
    if (delta_record.target == EDGE) {
      auto it = old_graph.find(delta_record.first);
      if (it != old_graph.end()) {
        if (delta_record.type == INSERT) {
          // Check for duplicate before inserting
          auto& neighbors = it->second;
          if (std::find(neighbors.begin(), neighbors.end(), delta_record.second) ==
              neighbors.end()) {
            neighbors.push_back(delta_record.second);
          }
        } else if (delta_record.type == DELETE) {
          auto& neighbors = it->second;
          neighbors.erase(std::remove(neighbors.begin(), neighbors.end(), delta_record.second),
                          neighbors.end());
        }
      }
    } else if (delta_record.target == VERTEX) {
      if (delta_record.type == INSERT) {
        // Insert new vertex with empty neighbor list
        if (!old_graph.contains(delta_record.first)) {
          old_graph[delta_record.first] = std::vector<v_id_t>();
        }
      } else if (delta_record.type == DELETE) {
        // Remove vertex entirely
        old_graph.erase(delta_record.first);
      }
    }
  }

  // Step 3 - Separate giant vertices and calculate space requirements
  // Metadata: 16 bytes (new compact layout)
  const size_t metadata_size =
      sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
  const size_t neighbor_block_data_size = bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);

  size_t used_bytes = metadata_size + neighbor_block_data_size;
  size_t normal_vertex_count = 0;
  uint16_t total_edge_count = 0;

  // First pass: identify giant vertices and calculate space
  std::vector<v_id_t> giant_vertices_to_remove;

  for (auto& [vertex_id, neighbors] : old_graph) {
    if (neighbors.size() >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      // This is a giant vertex - move to external storage
      used_bytes += sizeof(csr_vertex_t); // Only store metadata
      giant_vertex_ids.push_back({vertex_id, neighbors});
      giant_vertices_to_remove.push_back(vertex_id);
    } else {
      // Normal vertex
      used_bytes += sizeof(csr_vertex_t);
      used_bytes += neighbors.size() * sizeof(v_id_t);
      normal_vertex_count++;

      if (total_edge_count + neighbors.size() > UINT16_MAX) {
        return FAILED; // Edge count would exceed uint16_t limit
      }
      total_edge_count += static_cast<uint16_t>(neighbors.size());
    }
  }

  // Check vertex count limit
  if (normal_vertex_count > UINT16_MAX) {
    return FAILED; // Too many vertices
  }

  // Check if page has enough space
  if (used_bytes > static_cast<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE)) {
    return FAILED; // The new CSR page is too large
  }

  // Step 4 - Convert to adj_map_iter_t format (zero-copy spans)
  // We need to create stable storage for the neighbor data
  std::vector<std::vector<v_id_t>> neighbor_storage;
  neighbor_storage.reserve(old_graph.size());

  adj_map_iter_t adj_map_spans;
  adj_map_spans.reserve(old_graph.size());

  for (auto& [vertex_id, neighbors] : old_graph) {
    // Skip giant vertices (they're handled externally)
    if (std::find(giant_vertices_to_remove.begin(), giant_vertices_to_remove.end(), vertex_id) !=
        giant_vertices_to_remove.end()) {
      continue;
    }

    // Move neighbors to stable storage
    neighbor_storage.push_back(std::move(neighbors));

    // Create span pointing to the stable storage
    // std::span can be constructed from a container directly
    adj_map_spans[vertex_id] = std::span<v_id_t>{neighbor_storage.back()};
  }

  // Step 5 - Rebuild the CSR page with the new adjacency map
  try {
    // Clear current page data
    char* page_data = this->get_data();
    std::memset(page_data, 0, static_cast<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE));

    // Clear vertex_map for rebuild
    vertex_map.clear();

    // Use build_from_adjacency_map to construct the new page
    // Note: edge_slot_count can be 0 or a small value for compaction
    constexpr uint64_t edge_slot_count = 2; // Or 0 for no slots
    this->build_from_adjacency_map(adj_map_spans, vertex_map, edge_slot_count);

    // Parse the newly built page
    this->is_parse_ = false;
    this->parse_from_page_data();

  } catch (const std::runtime_error& e) {
    return FAILED; // Failed to reconstruct CSR page
  }

  return INSERTED;
}

// Generate desc.
std::unordered_map<std::string, std::string> csr_page_t::generate_desc() {
  std::unordered_map<std::string, std::string> desc;
  desc["type"] = "CSR Page";
  desc["page_number"] = std::to_string(this->get_page_no());
  desc["vertex_count"] = std::to_string(this->get_num_vertices());
  desc["edge_count"] = std::to_string(this->get_num_edges());
  desc["used_bytes"] = std::to_string(this->used_bytes_);
  desc["parent_page_no"] = std::to_string(this->parent_page_no_);

  std::vector<v_id_t> vertex_list_sample;

  vertex_span_t vertices = this->get_vertices();
  uint64_t printed_count = 0;
  for (const auto& vertex : vertices) {
    if (printed_count >= 10) {
      break;
    }
    vertex_list_sample.push_back(vertex.vertex_id);
    printed_count++;
  }

  // Build the sample string
  std::ostringstream oss;
  oss << "  Vertices:";
  for (const auto& vertex_id : vertex_list_sample) {
    oss << " " << vertex_id;
  }
  oss << (vertices.size() > 10 ? " ... {total: " + std::to_string(vertices.size()) + "}" : "");
  desc["vertices"] = oss.str();

  desc["neighbor_page_count"] = std::to_string(this->neighbor_page_count_);

  std::ostringstream oss_neighbor_pages;
  oss_neighbor_pages << "  Neighbor Pages:";
  for (const auto& neighbor : this->read_neighbor_block_clone()) {
    oss_neighbor_pages << " " << neighbor;
  }
  desc["neighbor_pages"] = oss_neighbor_pages.str();

  return desc;
}

// Set the parent page number
void csr_page_t::set_parent_page_no(page_no_t parent_page_no) {
  // Step 1 - Update the member variable
  parent_page_no_ = parent_page_no;

  // Step 2 - Update the data in the page
  char* page_data = get_data();
  *reinterpret_cast<page_no_t*>(page_data + 4 * sizeof(page_no_t)) = parent_page_no_;
}

// Add a new neighbor for this page;
bool csr_page_t::add_neighbor_page_no(page_no_t page_no) {
  // Step 1 - Check the page no is full;
  if (this->neighbor_page_count_ >= bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR) {
    // The neighbor page is full;
    return false;
  }

  // Step 2 - Check the added page no exists;
  for (auto& pn : this->neighbor_page_span_) {
    if (page_no == pn) {
      // This page no already exists;
      return false;
    }
  }

  // Step 3 - Add the new page no;
  char* page_data = this->get_data();
  const size_t metadata_size = 6 * sizeof(uint64_t);
  *reinterpret_cast<page_no_t*>(page_data + metadata_size +
                                this->neighbor_page_count_ * sizeof(page_no_t)) = page_no;

  page_no_t* neighbor_pages_ptr = reinterpret_cast<page_no_t*>(page_data + metadata_size);
  this->neighbor_page_count_++;

  // Reset the neighbor page count in the "frame" field!
  // Do not forget!
  *reinterpret_cast<uint64_t*>(page_data + 5 * sizeof(uint64_t)) = this->neighbor_page_count_;

  this->neighbor_page_span_ = neighbor_span_t(neighbor_pages_ptr, this->neighbor_page_count_);
  return true;
}

// Read the neighbor block in clone manner;
std::vector<page_no_t> csr_page_t::read_neighbor_block_clone() {
  std::vector<page_no_t> result;
  result.assign(this->neighbor_page_span_.begin(), this->neighbor_page_span_.end());
  return result;
}

// Read the neighbor block in zero-copy manner;
block_span_t csr_page_t::read_neighbor_block() { return this->neighbor_page_span_; }

// Insert into slot.
insert_status_t csr_page_t::insert_edge_in_slot(uint16_t src_vertex_offset, v_id_t src_vertex_id,
                                                v_id_t dest_vertex_id) {
  // Validate the src vertex offset
  if (src_vertex_offset >= this->num_vertices_) {
    return FAILED; // Invalid vertex offset
  }

  // Slot management in a PTV-compressed page would require scanning the
  // variable-width stream to locate the slot area. Not supported; the caller
  // must use the delta-chain path instead.
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
  return SLOT_FULL;
#endif

  // Validate the src vertex id
  csr_vertex_t* vertex_item = &this->vertex_span_[src_vertex_offset];
  // Check if source vertex ID matches
  if (src_vertex_id != vertex_item->vertex_id) {
    // Source vertex ID mismatch;
    return FAILED;
  }

  // Check if this is a giant vertex
  if (vertex_item->degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
    return FAILED;
  }

  // Step 1 - Get the neighbor count
  uint32_t vertex_degree = vertex_item->degree;

  // Step 2 - Calculate current neighbor data boundaries
  uint32_t neighbor_start_idx = vertex_item->offset / sizeof(v_id_t);
  uint32_t neighbor_end_idx = neighbor_start_idx + vertex_degree;

  // Step 3 - Get the offset of the next vertex to determine capacity
  uint32_t neighbor_capacity_end_idx;
  csr_vertex_t* next_vertex_item = nullptr;

  if (src_vertex_offset + 1 < this->num_vertices_) {
    // Not the last vertex - use next vertex's offset as boundary
    next_vertex_item = &this->vertex_span_[src_vertex_offset + 1];
    neighbor_capacity_end_idx = next_vertex_item->offset / sizeof(v_id_t);
  } else {
    // Last vertex - calculate capacity from used_bytes_
    const size_t metadata_size = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t); // = 16 bytes
    const size_t neighbor_block_data_size =
        bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t);
    size_t vertex_data_size = this->num_vertices_ * sizeof(csr_vertex_t);

    // Total neighbor data size (exclude parallel timestamp section in txn builds).
    // In txn mode: remaining = neighbor_data + ts_data, ratio ts:nbr =
    // sizeof(timestamp_t):sizeof(v_id_t) = 2:1. neighbor_data = remaining / 3.
    size_t remaining =
        this->used_bytes_ - metadata_size - neighbor_block_data_size - vertex_data_size;
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    size_t neighbor_data_size =
        (edge_ts_ptr_ != nullptr)
            ? remaining * sizeof(v_id_t) / (sizeof(v_id_t) + sizeof(timestamp_t))
            : remaining;
#else
    size_t neighbor_data_size = remaining;
#endif
    neighbor_capacity_end_idx = neighbor_data_size / sizeof(v_id_t);
  }

  // Step 4 - Check if there's available slot space
  if (neighbor_end_idx >= neighbor_capacity_end_idx) {
    return SLOT_FULL; // No space to insert new edge
  }

  // Step 5 - Check if inserting would exceed uint16_t offset limit
  uint32_t new_degree = vertex_degree + 1;

  // Step 7 - Insert the new edge at the end (use the reserved slot)
  this->neighbor_ptr_[neighbor_end_idx] = dest_vertex_id;

  // Step 8 - Update vertex metadata
  vertex_item->degree = new_degree;

  // Step 9 - Update page metadata
  this->num_edges_++;

  // Step 10 - Update used_bytes if we expanded into previously unused space
  const size_t metadata_size_final = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) +
                                     sizeof(uint32_t) + sizeof(uint32_t); // = 16 bytes
  uint32_t required_bytes =
      metadata_size_final + bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t) +
      this->num_vertices_ * sizeof(csr_vertex_t) + (neighbor_end_idx + 1) * sizeof(v_id_t);

  if (required_bytes > this->used_bytes_) {
    this->used_bytes_ = required_bytes;

    // Update used_bytes in page data
    char* page_data = get_data();
    *reinterpret_cast<uint32_t*>(page_data + 4) = this->used_bytes_;
  }

  // Step 11 - Update edge count in page data
  char* page_data = get_data();
  *reinterpret_cast<uint16_t*>(page_data + 2) = this->num_edges_;

  return INSERTED; // Edge successfully inserted in reserved slot
}

// Flush to disk.
bool csr_page_t::flush_to_disk(disk_manager_t* disk_mgr) {
  if (!rw_latch_.try_w_lock()) {
    return false; // Latch busy; retry next cycle.
  }
  if (is_dirty()) {
    disk_mgr->write_page(get_page_no(), get_data());
    set_dirty(false);
  }
  rw_latch_.w_unlock();
  return true;
}
