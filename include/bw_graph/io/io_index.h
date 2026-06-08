#ifndef BW_GRAPH_IO_INDEX_H
#define BW_GRAPH_IO_INDEX_H
#include "bw_graph/common/type.h"
#include "bw_graph/storage/page.h"

/**
 * @brief: The index page of Bw-Graph
 */
class index_page_t : public page_t {
public:
  bool is_parsed_{false};

  /**
   * @brief: Number of blocks (pages) in the index page
   */
  uint64_t num_blocks_;

  /**
   * @brief: Capacity of each block (page) in the index page
   */
  uint64_t block_capacity_{};

  /**
   * @brief: The parent page number in the hierarchy
   */
  uint64_t parent_page_no_{0};

  /**
   * @brief: Span of blocks (pages) in the index page
   */
  block_span_t blocks_span_;

  /**
   * @brief: Number of neighbor pages in the hierarchy
   */
  uint64_t neighbor_page_count_{0};

  /**
   * @brief: Span view of neighbor pages
   */
  block_span_t neighbor_page_span_;

  /**
   * @brief: Pointer to the block bitmap
   */
  char* block_bitmap_ptr_;

public:
  /**
   * @brief: Parse the index page
   */
  void self_parse();

  /**
   * @brief: Default constructor for the index page
   */
  explicit index_page_t();

  /**
   * @brief: Constructor for the index page
   *
   * @param page_no: The page number
   * @param block_adj_map: The block adjacency map
   */
  explicit index_page_t(const page_no_t& page_no, const block_adj_map_t& block_adj_map);

  /**
   * @brief: Get the neighbor blocks of a given block
   *
   * @param block_no: The block number
   * @return: A vector of neighboring block numbers
   */
  std::vector<page_no_t> get_neighbor_blocks(page_no_t block_no);

  /**
   * @brief: Insert a new block into the index page
   *
   * @param block_no: The block number
   * @return: True if the block is successfully inserted, false otherwise
   */
  bool insert_block(page_no_t block_no);

  /**
   * @brief: Insert a new edge between two blocks in the index page
   *
   * @param src_block: The source block number
   * @param dest_block: The destination block number
   * @return: True if the edge is successfully inserted, false otherwise
   */
  bool insert_block_edge(page_no_t src_block, page_no_t dest_block);

  /**
   * @brief: Get the number of blocks in the index page
   * @return: The number of blocks
   */
  uint64_t get_num_blocks();

  /**
   * @brief: Get the span of blocks in the index page
   * @return: The span of blocks
   */
  block_span_t get_blocks();

  /**
   * @brief: Generate a description of the index page
   * @return: A string description of the index page
   */
  std::unordered_map<std::string, std::string> generate_desc();

  /**
   * @brief Set the parent page number
   * @param parent_page_no The parent page number to set
   */
  void set_parent_page_no(uint64_t parent_page_no);

  /**
   * @brief Add a neighbor page number to the list
   * @param page_no The neighbor page number to add
   * @return true if success, false if fail, meaning the block is full
   */
  bool add_neighbor_page_no(page_no_t page_no);

  /**
   * @brief Read the neighbor block and return a clone
   * @return A vector containing the neighbor page numbers
   */
  std::vector<page_no_t> read_neighbor_block_clone();

  /**
   * @brief Read the neighbor block and return a span
   * @return A span containing the neighbor page numbers
   */
  block_span_t read_neighbor_block();

  /**
   * @brief Reconstruct the block adjacency map from page data.
   * @return block_adj_map_t mapping each block page_no to its neighbors.
   */
  block_adj_map_t get_block_adj_map();

private:
  /**
   * @brief: Build the index page from the block adjacency map
   *
   * @param block_adj_map: The block adjacency map
   */
  void build_from_block_adjmap(const block_adj_map_t& block_adj_map);

  /**
   * @brief: Parse the index page from the page data
   */
  void parse_from_page_data();

  /**
   * @brief: Set a bit in the bitmap
   * @param from_index: Source block index
   * @param to_index: Destination block index
   */
  void set_bitmap_bit(uint64_t from_index, uint64_t to_index);

  /**
   * @brief: Get a bit from the bitmap
   * @param from_index: Source block index
   * @param to_index: Destination block index
   * @return: True if there's an edge, false otherwise
   */
  bool get_bitmap_bit(uint64_t from_index, uint64_t to_index) const;

  /**
   * @brief: Get the block index of a given block number
   * @param block_no: The block number
   * @return: A pair of (block index, is success)
   */
  std::pair<uint64_t, bool> get_block_index(page_no_t block_no);
};
#endif // BW_GRAPH_IO_INDEX_H