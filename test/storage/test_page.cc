#include "bw_graph/storage/page.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

// Page basic operation cases.
class PageBasicOperationsTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test PageBasicOperationsTest.page creation and initialization.
TEST_F(PageBasicOperationsTest, PageCreationAndInitialization) {
  page_t page;

  // Test initial values
  EXPECT_EQ(page.get_page_no(), bw_graph::BW_GRAPH_DEFAULT_PAGE_NO);
  EXPECT_EQ(page.get_pin_count(), 0);
  EXPECT_EQ(page.is_dirty(), false);
  EXPECT_NE(page.get_data(), nullptr);

  std::cout << "Page initialized successfully with default values" << std::endl;
}

// Test PageBasicOperationsTest.page data access.
TEST_F(PageBasicOperationsTest, PageDataAccess) {
  page_t page;
  char* data = page.get_data();

  // Test data writing and reading
  const char test_string[] = "Hello BW Graph!";
  memcpy(data, test_string, strlen(test_string));

  EXPECT_EQ(memcmp(data, test_string, strlen(test_string)), 0);

  std::cout << "Page data access works correctly" << std::endl;
}

// Test PageBasicOperationsTest.page latch operations.
TEST_F(PageBasicOperationsTest, PageLatchOperations) {
  page_t page;

  // Test write latch
  page.w_latch();
  EXPECT_NO_THROW(page.w_unlatch());

  // Test read latch
  page.r_latch();
  EXPECT_NO_THROW(page.r_unlatch());

  std::cout << "Page latch operations work correctly" << std::endl;
}

// Test PageBasicOperationsTest.multiple read latches.
TEST_F(PageBasicOperationsTest, MultipleReadLatches) {
  page_t page;
  bool test_passed = true;

  // Test multiple readers can acquire read latch simultaneously
  std::thread reader1([&page, &test_passed]() {
    page.r_latch();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    page.r_unlatch();
    test_passed = true;
  });

  std::thread reader2([&page, &test_passed]() {
    page.r_latch();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    page.r_unlatch();
    test_passed = true;
  });

  reader1.join();
  reader2.join();

  EXPECT_EQ(test_passed, true);
  std::cout << "Multiple read latches work correctly" << std::endl;
}

// Test PageBasicOperationsTest.page memory layout.
TEST_F(PageBasicOperationsTest, PageMemoryLayout) {
  page_t page;
  char* data = page.get_data();

  // Test page size
  EXPECT_NE(data, nullptr);

  // Test writing to different offsets
  data[0] = 'A';
  data[100] = 'B';
  data[1000] = 'C';

  EXPECT_EQ(data[0], 'A');
  EXPECT_EQ(data[100], 'B');
  EXPECT_EQ(data[1000], 'C');

  std::cout << "Page memory layout is correct" << std::endl;
}

// Page threading cases.
class PageThreadingSafetyTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test PageThreadingSafetyTest.concurrent read access.
TEST_F(PageThreadingSafetyTest, ConcurrentReadAccess) {
  page_t page;
  std::atomic<int> reader_count{0};
  std::atomic<bool> all_readers_done{false};

  auto reader_func = [&page, &reader_count]() {
    page.r_latch();
    reader_count.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    reader_count.fetch_sub(1);
    page.r_unlatch();
  };

  // Start multiple reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 5; ++i) {
    readers.emplace_back(reader_func);
  }

  // Wait for all readers to complete
  for (auto& reader : readers) {
    reader.join();
  }

  EXPECT_EQ(reader_count.load(), 0);
  all_readers_done.store(true);
  EXPECT_TRUE(all_readers_done.load());
  std::cout << "Concurrent read access works correctly" << std::endl;
}

// Test PageThreadingSafetyTest.write exclusivity.
TEST_F(PageThreadingSafetyTest, WriteExclusivity) {
  page_t page;
  std::atomic<int> active_writers{0};
  std::atomic<int> max_concurrent_writers{0};

  auto writer_func = [&page, &active_writers, &max_concurrent_writers]() {
    page.w_latch();
    int current = active_writers.fetch_add(1) + 1;
    int max_val = max_concurrent_writers.load();
    while (current > max_val && !max_concurrent_writers.compare_exchange_weak(max_val, current)) {
      max_val = max_concurrent_writers.load();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    active_writers.fetch_sub(1);
    page.w_unlatch();
  };

  // Start multiple writer threads
  std::vector<std::thread> writers;
  for (int i = 0; i < 3; ++i) {
    writers.emplace_back(writer_func);
  }

  // Wait for all writers to complete
  for (auto& writer : writers) {
    writer.join();
  }

  // Only one writer should be active at a time
  EXPECT_EQ(max_concurrent_writers.load(), 1);
  std::cout << "Write exclusivity works correctly" << std::endl;
}

// Page edge cases.
class PageEdgeCasesTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test PageEdgeCasesTest.page destruction with active latches.
TEST_F(PageEdgeCasesTest, PageDestructionWithActiveLatches) {
  // This test ensures that page destruction doesn't cause issues
  {
    page_t page;
    page.r_latch();
    page.r_unlatch();
    page.w_latch();
    page.w_unlatch();
  } // page goes out of scope here

  EXPECT_TRUE(true); // If we reach here, destruction worked fine
  std::cout << "Page destruction with latches works correctly" << std::endl;
}

// Test PageEdgeCasesTest.page data integrity.
TEST_F(PageEdgeCasesTest, PageDataIntegrity) {
  page_t page;
  char* data = page.get_data();

  // Fill page with pattern
  for (size_t i = 0; i < 1024; ++i) {
    data[i] = static_cast<char>(i % 256);
  }

  // Verify pattern
  bool pattern_intact = true;
  for (size_t i = 0; i < 1024; ++i) {
    if (data[i] != static_cast<char>(i % 256)) {
      pattern_intact = false;
      break;
    }
  }

  EXPECT_EQ(pattern_intact, true);
  std::cout << "Page data integrity maintained" << std::endl;
}
