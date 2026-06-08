#include "bw_graph/io/io_index.h"

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

// Construct index_page_t.
index_page_t::index_page_t(const page_no_t& page_no, const block_adj_map_t& block_adj_map)
    : page_t(page_no, INDEX_PAGE), block_capacity_(bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS),
      block_bitmap_ptr_(nullptr) {
  // Initialize other members if needed
  this->build_from_block_adjmap(block_adj_map);
}

// Construct index_page_t.
index_page_t::index_page_t()
    : page_t(0, INDEX_PAGE), block_capacity_(bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS),
      block_bitmap_ptr_(nullptr) {
  // Initialize other members if needed
  block_adj_map_t empty_block_adj_map;
  this->page_no_ = 0;
  this->build_from_block_adjmap(empty_block_adj_map);
}

// Build from block adjmap.
void index_page_t::build_from_block_adjmap(const block_adj_map_t& block_adj_map) {
  // Check if the page count is valid
  if (block_adj_map.size() > this->block_capacity_) {
    throw std::runtime_error("Block adjacency map exceeds "
                             "index page capacity");
  }

  // 1. Prepare the space and write the
  // metadata
  const size_t metadata_size = 3 * sizeof(page_no_t); // 12 bytes
  const size_t block_entry_size = this->block_capacity_ * sizeof(page_no_t);
  const size_t block_neighbor_data_size =
      (bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t));

  std::vector<page_no_t> blocks;
  std::vector<page_no_t> neighbor_pages(bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR, 0);

  char* page_data = this->get_data();
  char* current_ptr = page_data;

  *reinterpret_cast<page_no_t*>(current_ptr) = static_cast<page_no_t>(this->num_blocks_);
  current_ptr += sizeof(page_no_t);

  *reinterpret_cast<page_no_t*>(current_ptr) = static_cast<page_no_t>(this->parent_page_no_);
  current_ptr += sizeof(page_no_t);

  *reinterpret_cast<page_no_t*>(current_ptr) = static_cast<page_no_t>(this->neighbor_page_count_);
  current_ptr += sizeof(page_no_t);

  this->block_bitmap_ptr_ = reinterpret_cast<char*>(page_data + metadata_size +
                                                    block_neighbor_data_size + block_entry_size);

  // 2. Build the index page from the block adjacency map
  for (const auto& [block_no, adj_blocks] : block_adj_map) {
    blocks.emplace_back(block_no);
  }

  uint64_t block_array_size = blocks.size() * sizeof(page_no_t);

  // Copy neighbor block data after metadata
  std::memcpy(page_data + metadata_size, neighbor_pages.data(), block_neighbor_data_size);

  // Copy block data after metadata and neighbor data
  std::memcpy(page_data + metadata_size + block_neighbor_data_size, blocks.data(),
              block_array_size);

  this->num_blocks_ = blocks.size();
  this->neighbor_page_count_ = 0;

  // Create a mapping from block_no to
  // index
  folly::F14FastMap<page_no_t, uint64_t> block_to_index;
  for (size_t i = 0; i < blocks.size(); ++i) {
    block_to_index[blocks[i]] = i;
  }

  // 3. Write block bitmaps (block
  // numbers)
  size_t bitmap_size = (this->block_capacity_ * this->block_capacity_ + 7) / 8;
  size_t total_used = metadata_size + block_entry_size + bitmap_size + block_neighbor_data_size;
  if (total_used > bw_graph::BW_GRAPH_INDEX_PAGE_SIZE) {
    std::cout << "All size: " << (metadata_size + block_entry_size + bitmap_size) << std::endl;
    std::cout << "Index Page Size: " << bw_graph::BW_GRAPH_INDEX_PAGE_SIZE << std::endl;
    std::cout << "Block Capacity: " << this->block_capacity_ << std::endl;
    throw std::runtime_error("Index page size exceeded while "
                             "building block bitmap");
  }
  std::memset(this->block_bitmap_ptr_, 0, bitmap_size);

  // 4. Build the adjacency bitmap
  for (const auto& [block_no, adj_blocks] : block_adj_map) {
    // Get the index of the source block
    auto src_it = block_to_index.find(block_no);
    if (src_it == block_to_index.end()) {
      continue; // Should not happen if
                // input is valid
    }
    uint64_t src_index = src_it->second;

    // Set bits for all adjacent blocks
    for (page_no_t adj_block : adj_blocks) {
      auto dest_it = block_to_index.find(adj_block);
      if (dest_it == block_to_index.end()) {
        // Adjacent block not in our
        // block list, skip or handle as
        // needed
        continue;
      }
      uint64_t dest_index = dest_it->second;

      // Set the bit in the adjacency
      // matrix
      set_bitmap_bit(src_index, dest_index);
    }
  }

  // 5. Set the block span;
  page_no_t* neighbor_block_ptr = reinterpret_cast<page_no_t*>(page_data + metadata_size);
  this->neighbor_page_span_ = block_span_t(neighbor_block_ptr, this->neighbor_page_count_);

  page_no_t* block_ptr =
      reinterpret_cast<page_no_t*>(page_data + metadata_size + block_neighbor_data_size);
  this->blocks_span_ = block_span_t(block_ptr, this->num_blocks_);
  this->is_parsed_ = true;
}

// Helper method to set a bit in the bitmap
void index_page_t::set_bitmap_bit(uint64_t from_index, uint64_t to_index) {
  if (from_index >= this->block_capacity_ || to_index >= this->block_capacity_) {
    return;
  }

  // Calculate bit position in the adjacency matrix
  uint64_t bit_index = from_index * this->block_capacity_ + to_index;
  uint64_t byte_index = bit_index / 8;
  uint8_t bit_offset = bit_index % 8;

  // Set the bit
  this->block_bitmap_ptr_[byte_index] |= (1 << bit_offset);
}

// Helper method to get a bit from the bitmap
bool index_page_t::get_bitmap_bit(uint64_t from_index, uint64_t to_index) const {
  if (from_index >= this->block_capacity_ || to_index >= this->block_capacity_) {
    return false;
  }

  uint64_t bit_index = from_index * this->block_capacity_ + to_index;
  uint64_t byte_index = bit_index / 8;
  uint8_t bit_offset = bit_index % 8;

  return (this->block_bitmap_ptr_[byte_index] & (1 << bit_offset)) != 0;
}

// Get num blocks.
uint64_t index_page_t::get_num_blocks() { return this->num_blocks_; }

// Self Parse function;
void index_page_t::self_parse() {
  if (this->is_parsed_) {
    return; // Already parsed
  }

  // Parse the page data to initialize the index page structure
  char* page_data = this->get_data();
  this->num_blocks_ = *reinterpret_cast<const page_no_t*>(page_data);
  this->parent_page_no_ = *reinterpret_cast<const uint64_t*>(page_data + sizeof(uint64_t));
  this->neighbor_page_count_ = *reinterpret_cast<const uint64_t*>(page_data + 2 * sizeof(uint64_t));

  const size_t metadata_size = 3 * sizeof(page_no_t);
  const size_t block_data_size = this->block_capacity_ * sizeof(page_no_t);
  const size_t block_neighbor_data_size =
      (bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR * sizeof(page_no_t));

  this->block_bitmap_ptr_ = reinterpret_cast<char*>(page_data + metadata_size + block_data_size +
                                                    block_neighbor_data_size);

  // Set up the neighbor pages span
  page_no_t* neighbor_ptr = reinterpret_cast<page_no_t*>(page_data + metadata_size);
  this->neighbor_page_span_ = block_span_t(neighbor_ptr, this->neighbor_page_count_);

  // Set up the blocks span
  page_no_t* block_ptr =
      reinterpret_cast<page_no_t*>(page_data + metadata_size + block_neighbor_data_size);
  this->blocks_span_ = block_span_t(block_ptr, this->num_blocks_);
}

// Get neighbor blocks.
std::vector<page_no_t> index_page_t::get_neighbor_blocks(page_no_t block_no) {
  std::vector<page_no_t> neighbors;

  if (!this->is_parsed_) {
    this->self_parse();
  }

  // Find block index
  uint64_t block_index = UINT32_MAX;
  for (uint64_t i = 0; i < this->num_blocks_; ++i) {
    if (blocks_span_[i] == block_no) {
      block_index = i;
      break;
    }
  }

  // Page not found, return empty list;
  if (block_index == UINT32_MAX)
    return neighbors;

  // Collect neighbors
  neighbors.reserve(this->num_blocks_); // Pre-allocate for efficiency
  for (uint64_t j = 0; j < this->num_blocks_; ++j) {
    if (get_bitmap_bit(block_index, j)) {
      neighbors.push_back(blocks_span_[j]);
    }
  }

  return neighbors;
}

// Insert a new block into an index page;
bool index_page_t::insert_block(page_no_t block_no) {
  if (!this->is_parsed_) {
    this->self_parse();
  }

  // Check if there is place for this block no;
  if (this->num_blocks_ >= this->block_capacity_) {
    return false; // No space to insert new block
  }

  // Check if this block already exists;
  for (uint64_t i = 0; i < this->num_blocks_; ++i) {
    if (blocks_span_[i] == block_no) {
      return false; // Block already exists
    }
  }

  // Insert the new block
  blocks_span_[this->num_blocks_] = block_no;
  this->num_blocks_++;
  char* page_data = this->get_data();
  *reinterpret_cast<uint64_t*>(page_data) = this->num_blocks_;
  this->blocks_span_ = block_span_t(this->blocks_span_.data(), this->num_blocks_);
  return true;
}

// Get block index.
std::pair<uint64_t, bool> index_page_t::get_block_index(page_no_t block_no) {
  if (!this->is_parsed_) {
    this->self_parse();
  }

  for (uint64_t i = 0; i < this->num_blocks_; ++i) {
    if (blocks_span_[i] == block_no) {
      return {i, true};
    }
  }

  return {UINT32_MAX, false};
}

// Insert a relation between blocks;
bool index_page_t::insert_block_edge(page_no_t src_block, page_no_t dest_block) {
  // Check if both blocks exist
  if (!this->is_parsed_) {
    this->self_parse();
  }

  auto src_res = get_block_index(src_block);
  auto dst_res = get_block_index(dest_block);

  if (!src_res.second || !dst_res.second) {
    return false; // One of the blocks does not exist
  } else {
    auto src_index = src_res.first;
    auto dest_index = dst_res.first;
    set_bitmap_bit(src_index, dest_index);
    return true;
  }
}

// Generate desc.
std::unordered_map<std::string, std::string> index_page_t::generate_desc() {
  std::unordered_map<std::string, std::string> desc;
  desc["page_number"] = std::to_string(this->get_page_no());
  desc["num_blocks"] = std::to_string(this->get_num_blocks());
  desc["parent_page_no"] = std::to_string(this->parent_page_no_);
  desc["neighbor_page_count"] = std::to_string(this->neighbor_page_count_);

  // Build the sample string
  std::ostringstream oss;
  oss << "  Page Blocks:";
  uint64_t printed_count = 0;
  for (const auto& block : this->blocks_span_) {
    oss << " " << block;
    printed_count++;
    if (printed_count >= 10) {
      oss << " ... {total: " << this->blocks_span_.size() << "}";
      break;
    }
  }
  desc["blocks"] = oss.str();

  std::ostringstream oss_neighbor_pages;
  oss_neighbor_pages << "  Neighbor Pages:";
  for (const auto& neighbor : this->read_neighbor_block_clone()) {
    oss_neighbor_pages << " " << neighbor;
  }
  desc["neighbor_pages"] = oss_neighbor_pages.str();

  return desc;
}

// Get blocks.
block_span_t index_page_t::get_blocks() { return this->blocks_span_; }

// Set the parent page number
void index_page_t::set_parent_page_no(uint64_t parent_page_no) {
  // Step 1 - Update the member variable
  parent_page_no_ = parent_page_no;

  // Step 2 - Update the data in the page
  char* page_data = get_data();
  *reinterpret_cast<uint64_t*>(page_data + sizeof(uint64_t)) = parent_page_no_;
}

// Add a new neighbor for this page
bool index_page_t::add_neighbor_page_no(page_no_t page_no) {
  // Step 1 - Check if the neighbor page array is full
  if (this->neighbor_page_count_ >= bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR) {
    return false;
  }

  // Step 2 - Check if the page_no already exists
  for (auto& pn : this->neighbor_page_span_) {
    if (page_no == pn) {
      return false; // Already exists
    }
  }

  // Step 3 - Add the new page_no
  char* page_data = this->get_data();
  const size_t metadata_size = 3 * sizeof(page_no_t); // 12 bytes

  *reinterpret_cast<page_no_t*>(page_data + metadata_size +
                                this->neighbor_page_count_ * sizeof(page_no_t)) = page_no;
  this->neighbor_page_count_++;

  *reinterpret_cast<page_no_t*>(page_data + 2 * sizeof(page_no_t)) =
      static_cast<page_no_t>(this->neighbor_page_count_);
  page_no_t* neighbor_pages_ptr = reinterpret_cast<page_no_t*>(page_data + metadata_size);
  this->neighbor_page_span_ = neighbor_span_t(neighbor_pages_ptr, this->neighbor_page_count_);

  return true;
}

// Read the neighbor block in clone manner;
std::vector<page_no_t> index_page_t::read_neighbor_block_clone() {
  std::vector<page_no_t> result;
  result.assign(this->neighbor_page_span_.begin(), this->neighbor_page_span_.end());
  return result;
}

// Read the neighbor block in zero-copy manner;
block_span_t index_page_t::read_neighbor_block() { return this->neighbor_page_span_; }

// Get block adj map.
block_adj_map_t index_page_t::get_block_adj_map() {
  if (!this->is_parsed_) {
    this->self_parse();
  }
  block_adj_map_t adj;
  for (uint64_t i = 0; i < this->num_blocks_; ++i) {
    page_no_t src = this->blocks_span_[i];
    adj[src] = {};
    for (uint64_t j = 0; j < this->num_blocks_; ++j) {
      if (get_bitmap_bit(i, j)) {
        adj[src].push_back(this->blocks_span_[j]);
      }
    }
  }
  return adj;
}
