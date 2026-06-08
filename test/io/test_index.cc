#include "bw_graph/common/type.h"
#include "bw_graph/io/io_index.h"
#include "test.h"

#include <gtest/gtest.h>
#include <vector>

// Constructor cases.
TEST(IndexConstructorTest, DefaultConstructorCreatesEmptyIndex) {
  index_page_t index_page;
  EXPECT_EQ(index_page.get_num_blocks(), 0);
  EXPECT_EQ(index_page.get_page_no(), 0);
  std::cout << "Before Free" << std::endl;
}

// Test IndexBuildTest.default constructor creates index.
TEST(IndexBuildTest, DefaultConstructorCreatesIndex) {
  block_adj_map_t test_block_adj_map = create_test_block_adjmap();
  index_page_t index_page(0, test_block_adj_map);
  EXPECT_EQ(index_page.get_num_blocks(), 13);
  EXPECT_EQ(index_page.get_page_no(), 0);
  std::cout << "Before Free" << std::endl;
  index_page.blocks_span_ = block_span_t{};
}

// Test IndexBlockTest.index block add test.
TEST(IndexBlockTest, IndexBlockAddTest) {
  block_adj_map_t test_block_adj_map = create_test_block_adjmap();
  index_page_t index_page(0, test_block_adj_map);
  bool res = index_page.insert_block(13);
  EXPECT_TRUE(res);
  EXPECT_EQ(index_page.get_num_blocks(), 14);
  EXPECT_EQ(index_page.get_page_no(), 0);
  std::cout << "Before Free" << std::endl;
  index_page.blocks_span_ = block_span_t{};
}

// Test IndexBlockEdgeTest.index block edge add test.
TEST(IndexBlockEdgeTest, IndexBlockEdgeAddTest) {
  block_adj_map_t test_block_adj_map = create_test_block_adjmap();
  index_page_t index_page(0, test_block_adj_map);
  bool res = index_page.insert_block(13);
  EXPECT_TRUE(res);
  bool invalid_result = index_page.insert_block_edge(13, 100);
  EXPECT_FALSE(invalid_result);
  bool valid_result = index_page.insert_block_edge(13, 5);
  EXPECT_TRUE(valid_result);
  std::vector<page_no_t> neighbors_13 = index_page.get_neighbor_blocks(13);
  EXPECT_EQ(neighbors_13.size(), 1);
  EXPECT_EQ(neighbors_13[0], 5);
  EXPECT_EQ(index_page.get_num_blocks(), 14);
  EXPECT_EQ(index_page.get_page_no(), 0);
  index_page.blocks_span_ = block_span_t{};
}

// Add Neighbor Page Tests
TEST(CSRFunctionalityTest, AddNeighborBlockTest) {
  block_adj_map_t test_graph = {{10, {20, 30}}, {20, {10}}, {30, {10, 40}}, {40, {30}}};
  index_page_t index_page(0, test_graph);
  EXPECT_EQ(index_page.read_neighbor_block().size(), 0);
  for (uint64_t i = 0; i < bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR; ++i) {
    index_page.add_neighbor_page_no(i);
  }
  EXPECT_EQ(index_page.read_neighbor_block().size(), bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);
  EXPECT_EQ(index_page.read_neighbor_block_clone().size(), bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);

  bool res_false = index_page.add_neighbor_page_no(100);
  EXPECT_FALSE(res_false);
}