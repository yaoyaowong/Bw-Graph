#include <bw_graph/buf/buf_pool.h>
#include <bw_graph/common/config.h>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>

// Test BufChunkTest.init test.
TEST(BufChunkTest, InitTest) {
  auto* chunk = buf_chunk_t<csr_page_t>::buf_chunk_init(10, bw_graph::BW_GRAPH_PAGE_SIZE);
  EXPECT_NE(chunk, nullptr);
  delete chunk;
}

// Test BufChunkTest.memory initialization test.
TEST(BufChunkTest, MemoryInitializationTest) {
  size_t page_count = 5;
  auto* chunk = buf_chunk_t<csr_page_t>::buf_chunk_init(page_count, bw_graph::BW_GRAPH_PAGE_SIZE);

  EXPECT_NE(chunk, nullptr);
  EXPECT_NE(chunk->pages, nullptr);
  EXPECT_NE(chunk->frames, nullptr);

  // Test that memory is properly allocated and accessible
  const char* test_data = "Hello, Buffer Pool!";
  char* first_page = chunk->frames;
  strcpy(first_page, test_data);

  EXPECT_STREQ(first_page, test_data);

  delete chunk;
}

// Test BufChunkTest.multiple page test.
TEST(BufChunkTest, MultiplePageTest) {
  size_t page_count = 3;
  auto* chunk = buf_chunk_t<csr_page_t>::buf_chunk_init(page_count, bw_graph::BW_GRAPH_PAGE_SIZE);

  for (size_t i = 0; i < page_count; ++i) {
    char* page_data = chunk->frames + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    snprintf(page_data, bw_graph::BW_GRAPH_PAGE_SIZE, "Page %zu data", i);
  }

  for (size_t i = 0; i < page_count; ++i) {
    char* page_data = chunk->frames + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    char expected[32];
    snprintf(expected, sizeof(expected), "Page %zu data", i);
    EXPECT_STREQ(page_data, expected);
  }

  delete chunk;
}

// Test BufFreeTest.traversal free test.
TEST(BufFreeTest, TraversalFreeTest) {
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(2, 10, bw_graph::BW_GRAPH_PAGE_SIZE);
  int count = 0;
  buf_pool->traverse_free_list([&count](csr_page_t* page) {
    std::cout << "Free page at address: " << static_cast<void*>(page->get_data()) << std::endl;
    count++;
    return true;
  });
  EXPECT_EQ(count, 20); // 2 chunks * 10 pages each
}
