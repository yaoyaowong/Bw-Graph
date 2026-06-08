#include "bw_graph/common/config.h"
#include "bw_graph/storage/disk_manager.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

// Disk Manager basic operation cases.
class DiskManagerBasicOperationsTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test DiskManagerBasicOperationsTest.disk manager creation and initialization.
TEST_F(DiskManagerBasicOperationsTest, DiskManagerCreationAndInitialization) {
  const std::filesystem::path test_db_file = "test_db.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  disk_manager_t disk_manager(test_db_file);

  // Test initial values
  EXPECT_EQ(disk_manager.get_num_flushes(), 0);
  EXPECT_EQ(disk_manager.get_num_writes(), 0);
  EXPECT_EQ(disk_manager.get_num_deletes(), 0);
  EXPECT_EQ(disk_manager.get_flush_state(), false);
  EXPECT_EQ(disk_manager.has_flush_log_future(), false);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Disk manager initialized successfully with default values" << std::endl;
}

// Test DiskManagerBasicOperationsTest.page write and read operations.
TEST_F(DiskManagerBasicOperationsTest, PageWriteAndReadOperations) {
  const std::filesystem::path test_db_file = "test_page_io.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Prepare test data
  char* write_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  char* read_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  const char test_string[] = "Hello BW Graph Disk Manager!";

  // Initialize write data
  std::memset(write_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
  std::memcpy(write_data, test_string, strlen(test_string));

  // Write page
  page_no_t test_page_no = 1;
  disk_manager.write_page(test_page_no, write_data);

  // Read page back
  std::memset(read_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
  disk_manager.read_page(test_page_no, read_data);

  // Verify data integrity
  EXPECT_EQ(std::memcmp(write_data, read_data, bw_graph::BW_GRAPH_PAGE_SIZE), 0);
  EXPECT_EQ(std::memcmp(read_data, test_string, strlen(test_string)), 0);
  EXPECT_EQ(disk_manager.get_num_writes(), 1);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Page write and read operations work correctly" << std::endl;
}

// Test DiskManagerBasicOperationsTest.multiple page operations.
TEST_F(DiskManagerBasicOperationsTest, MultiplePageOperations) {
  const std::filesystem::path test_db_file = "test_multi_page.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  const int num_pages = 5;
  std::vector<char> write_data(num_pages * bw_graph::BW_GRAPH_PAGE_SIZE);
  std::vector<char> read_data(num_pages * bw_graph::BW_GRAPH_PAGE_SIZE);

  // Prepare different data for each page
  for (int i = 0; i < num_pages; ++i) {
    char* page_data = write_data.data() + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    std::memset(page_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
    std::snprintf(page_data, bw_graph::BW_GRAPH_PAGE_SIZE, "Page %d data - BW Graph Test", i);
  }

  // Write all pages
  for (int i = 0; i < num_pages; ++i) {
    page_no_t page_no = static_cast<page_no_t>(i + 1);
    char* page_data = write_data.data() + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    disk_manager.write_page(page_no, page_data);
  }

  // Read all pages back
  for (int i = 0; i < num_pages; ++i) {
    page_no_t page_no = static_cast<page_no_t>(i + 1);
    char* page_data = read_data.data() + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    disk_manager.read_page(page_no, page_data);
  }

  // Verify all pages
  bool all_pages_correct = true;
  for (int i = 0; i < num_pages; ++i) {
    char* written_page = write_data.data() + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    char* read_page = read_data.data() + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    if (std::memcmp(written_page, read_page, bw_graph::BW_GRAPH_PAGE_SIZE) != 0) {
      all_pages_correct = false;
      break;
    }
  }

  EXPECT_EQ(all_pages_correct, true);
  EXPECT_EQ(disk_manager.get_num_writes(), num_pages);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Multiple page operations work correctly" << std::endl;
}

// Test DiskManagerBasicOperationsTest.page delete operations.
TEST_F(DiskManagerBasicOperationsTest, PageDeleteOperations) {
  const std::filesystem::path test_db_file = "test_delete.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Write a page
  char* write_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  std::memset(write_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
  const char test_string[] = "Page to be deleted";
  std::memcpy(write_data, test_string, strlen(test_string));

  page_no_t test_page_no = 1;
  disk_manager.write_page(test_page_no, write_data);
  EXPECT_EQ(disk_manager.get_num_writes(), 1);

  // Delete the page
  disk_manager.delete_page(test_page_no);
  EXPECT_EQ(disk_manager.get_num_deletes(), 1);

  // Try to read deleted page (should return zeros)
  char* read_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  disk_manager.read_page(test_page_no, read_data);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Page delete operations work correctly" << std::endl;
}

// Disk Manager log operation cases.
class DiskManagerLogOperationsTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test DiskManagerLogOperationsTest.log write and read operations.
TEST_F(DiskManagerLogOperationsTest, LogWriteAndReadOperations) {
  const std::filesystem::path test_db_file = "test_log.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  disk_manager_t disk_manager(test_db_file);

  // Test log operations
  const char log_message[] = "Test log entry for BW Graph";
  char log_data[256];
  std::memset(log_data, 0, sizeof(log_data));
  std::memcpy(log_data, log_message, strlen(log_message));

  // Write log
  disk_manager.write_log(log_data, strlen(log_message));

  // Read log back
  char read_log_data[256];
  std::memset(read_log_data, 0, sizeof(read_log_data));
  bool read_success = disk_manager.read_log(read_log_data, strlen(log_message), 0);

  EXPECT_EQ(read_success, true);
  EXPECT_EQ(std::memcmp(log_data, read_log_data, strlen(log_message)), 0);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Log write and read operations work correctly" << std::endl;
}

// Test DiskManagerLogOperationsTest.multiple log entries.
TEST_F(DiskManagerLogOperationsTest, MultipleLogEntries) {
  const std::filesystem::path test_db_file = "test_multi_log.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  disk_manager_t disk_manager(test_db_file);

  std::vector<std::string> log_messages = {"First log entry", "Second log entry",
                                           "Third log entry"};

  // Write multiple log entries
  for (const auto& message : log_messages) {
    char log_data[256];
    std::memset(log_data, 0, sizeof(log_data));
    std::memcpy(log_data, message.c_str(), message.length());
    disk_manager.write_log(log_data, message.length());
  }

  // Read and verify each entry
  int offset = 0;
  for (const auto& expected_message : log_messages) {
    char read_data[256];
    std::memset(read_data, 0, sizeof(read_data));
    bool read_success = disk_manager.read_log(read_data, expected_message.length(), offset);

    EXPECT_EQ(read_success, true);
    EXPECT_EQ(std::memcmp(read_data, expected_message.c_str(), expected_message.length()), 0);

    offset += expected_message.length();
  }

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Multiple log entries work correctly" << std::endl;
}

// Disk Manager threading cases.
class DiskManagerThreadingSafetyTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test DiskManagerThreadingSafetyTest.concurrent page write operations.
TEST_F(DiskManagerThreadingSafetyTest, ConcurrentPageWriteOperations) {
  const std::filesystem::path test_db_file = "test_concurrent.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);
  std::atomic<int> completed_writes{0};
  const int num_threads = 5;
  const int pages_per_thread = 3;

  auto writer_func = [&disk_manager, &completed_writes](int thread_id) {
    for (int i = 0; i < pages_per_thread; ++i) {
      char* write_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
      std::memset(write_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
      std::snprintf(write_data, bw_graph::BW_GRAPH_PAGE_SIZE, "Thread %d Page %d", thread_id, i);

      page_no_t page_no = static_cast<page_no_t>(thread_id * pages_per_thread + i + 1);
      disk_manager.write_page(page_no, write_data);
      delete[] write_data;
      completed_writes.fetch_add(1);
    }
  };

  // Start multiple writer threads
  std::vector<std::thread> writers;
  for (int i = 0; i < num_threads; ++i) {
    writers.emplace_back(writer_func, i);
  }

  // Wait for all writers to complete
  for (auto& writer : writers) {
    writer.join();
  }

  EXPECT_EQ(completed_writes.load(), num_threads * pages_per_thread);
  EXPECT_EQ(disk_manager.get_num_writes(), num_threads * pages_per_thread);

  // Verify all written pages
  bool all_pages_readable = true;
  for (int thread_id = 0; thread_id < num_threads; ++thread_id) {
    for (int i = 0; i < pages_per_thread; ++i) {
      page_no_t page_no = static_cast<page_no_t>(thread_id * pages_per_thread + i + 1);
      char* read_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
      disk_manager.read_page(page_no, read_data);

      char* expected_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
      std::memset(expected_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
      std::snprintf(expected_data, bw_graph::BW_GRAPH_PAGE_SIZE, "Thread %d Page %d", thread_id, i);

      if (std::memcmp(read_data, expected_data, strlen(expected_data)) != 0) {
        all_pages_readable = false;
        break;
      }
    }
    if (!all_pages_readable)
      break;
  }

  EXPECT_EQ(all_pages_readable, true);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Concurrent page write operations work correctly" << std::endl;
}

// Disk manager edge cases.
class DiskManagerEdgeCasesTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test DiskManagerEdgeCasesTest.file size operations.
TEST_F(DiskManagerEdgeCasesTest, FileSizeOperations) {
  const std::filesystem::path test_db_file = "test_filesize.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Initially file should be small or empty
  size_t initial_size = disk_manager.get_db_file_size();

  // Write a page
  char* write_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  std::memset(write_data, 'A', bw_graph::BW_GRAPH_PAGE_SIZE);
  disk_manager.write_page(1, write_data);
  delete[] write_data;

  // File size should have increased
  size_t after_write_size = disk_manager.get_db_file_size();
  EXPECT_GE(after_write_size, initial_size + bw_graph::BW_GRAPH_PAGE_SIZE);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "File size operations work correctly" << std::endl;
}

// Test DiskManagerEdgeCasesTest.read non existent page.
TEST_F(DiskManagerEdgeCasesTest, ReadNonExistentPage) {
  const std::filesystem::path test_db_file = "test_nonexistent.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Try to read a page that doesn't exist
  char* read_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  disk_manager.read_page(999, read_data);

  // Should return zeroed data
  bool is_zeroed = true;
  for (size_t i = 0; i < bw_graph::BW_GRAPH_PAGE_SIZE; ++i) {
    if (read_data[i] != 0) {
      is_zeroed = false;
      break;
    }
  }
  EXPECT_EQ(is_zeroed, true);

  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Reading non-existent page works correctly" << std::endl;
}

// Disk Manager page allocation cases.
class DiskManagerPageAllocationTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// Test DiskManagerPageAllocationTest.basic page allocation.
TEST_F(DiskManagerPageAllocationTest, BasicPageAllocation) {
  const std::filesystem::path test_db_file = "test_allocate_basic.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Get initial file size
  disk_manager.get_db_file_size();

  // Allocate first page - should be at the beginning/end of file
  size_t first_offset = disk_manager.allocate_page();
  EXPECT_GE(first_offset, 0);

  // Allocate second page - should be after the first page
  size_t second_offset = disk_manager.allocate_page();
  EXPECT_GT(second_offset, first_offset);
  EXPECT_EQ(second_offset, first_offset + bw_graph::BW_GRAPH_PAGE_SIZE);

  // Allocate third page
  size_t third_offset = disk_manager.allocate_page();
  EXPECT_GT(third_offset, second_offset);
  EXPECT_EQ(third_offset, second_offset + bw_graph::BW_GRAPH_PAGE_SIZE);

  // Test that we can actually write to allocated pages
  char* test_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  std::memset(test_data, 'X', bw_graph::BW_GRAPH_PAGE_SIZE);

  // Convert offset to page number for testing
  page_no_t first_page_no = static_cast<page_no_t>(first_offset / bw_graph::BW_GRAPH_PAGE_SIZE);
  disk_manager.write_page(first_page_no, test_data);
  EXPECT_EQ(disk_manager.get_num_writes(), 1);

  delete[] test_data;
  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Basic page allocation works correctly" << std::endl;
}

// Test DiskManagerPageAllocationTest.page allocation with reuse.
TEST_F(DiskManagerPageAllocationTest, PageAllocationWithReuse) {
  const std::filesystem::path test_db_file = "test_allocate_reuse.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Write some pages first to establish file structure
  char* test_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  std::memset(test_data, 'A', bw_graph::BW_GRAPH_PAGE_SIZE);

  page_no_t page1 = 1, page2 = 2, page3 = 3;
  disk_manager.write_page(page1, test_data);
  disk_manager.write_page(page2, test_data);
  disk_manager.write_page(page3, test_data);

  // Now allocate pages to get current offsets
  disk_manager.allocate_page();
  disk_manager.allocate_page();

  // Delete page 2 (middle page) - this should create a free slot
  disk_manager.delete_page(page2);
  EXPECT_EQ(disk_manager.get_num_deletes(), 1);

  // Allocate a new page - should reuse the deleted page's slot
  size_t reused_offset = disk_manager.allocate_page();

  // The reused offset should be the same as page2's offset
  size_t expected_page2_offset = static_cast<size_t>(page2) * bw_graph::BW_GRAPH_PAGE_SIZE;
  EXPECT_EQ(reused_offset, expected_page2_offset);

  // Allocate another page - since we used up the free slot, this should be at
  // the end
  size_t new_offset = disk_manager.allocate_page();
  EXPECT_GT(new_offset, reused_offset);

  // Test that we can write to the reused page
  std::memset(test_data, 'R', bw_graph::BW_GRAPH_PAGE_SIZE); // 'R' for reused
  page_no_t reused_page_no = static_cast<page_no_t>(reused_offset / bw_graph::BW_GRAPH_PAGE_SIZE);
  disk_manager.write_page(reused_page_no, test_data);

  // Verify the reused page can be read correctly
  char* read_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
  disk_manager.read_page(reused_page_no, read_data);
  EXPECT_EQ(std::memcmp(test_data, read_data, bw_graph::BW_GRAPH_PAGE_SIZE), 0);

  delete[] test_data;
  delete[] read_data;
  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Page allocation with reuse works correctly" << std::endl;
}

// Test DiskManagerPageAllocationTest.multiple delete and allocate cycle.
TEST_F(DiskManagerPageAllocationTest, MultipleDeleteAndAllocateCycle) {
  const std::filesystem::path test_db_file = "test_allocate_cycle.db";

  // Clean up any existing test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }

  disk_manager_t disk_manager(test_db_file);

  // Create and write several pages
  const int num_initial_pages = 5;
  char* test_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];

  for (int i = 1; i <= num_initial_pages; ++i) {
    std::memset(test_data, 'A' + i - 1, bw_graph::BW_GRAPH_PAGE_SIZE);
    disk_manager.write_page(static_cast<page_no_t>(i), test_data);
  }

  // Delete multiple pages to create several free slots
  std::vector<page_no_t> deleted_pages = {2, 4}; // Delete pages 2 and 4
  for (page_no_t page_no : deleted_pages) {
    disk_manager.delete_page(page_no);
  }
  EXPECT_EQ(disk_manager.get_num_deletes(), deleted_pages.size());

  // Allocate pages and verify they reuse the deleted slots
  std::vector<size_t> allocated_offsets;
  std::vector<size_t> expected_reused_offsets = {
      static_cast<size_t>(4) * bw_graph::BW_GRAPH_PAGE_SIZE, // Page 4's offset
      static_cast<size_t>(2) * bw_graph::BW_GRAPH_PAGE_SIZE  // Page 2's offset
  };

  // Allocate two pages - should reuse the deleted slots (in reverse order due
  // to stack behavior)
  for (size_t i = 0; i < deleted_pages.size(); ++i) {
    size_t offset = disk_manager.allocate_page();
    allocated_offsets.push_back(offset);

    // Verify it's one of the expected reused offsets
    bool found_expected = false;
    for (size_t expected_offset : expected_reused_offsets) {
      if (offset == expected_offset) {
        found_expected = true;
        break;
      }
    }
    EXPECT_TRUE(found_expected) << "Allocated offset " << offset
                                << " not in expected reused offsets";
  }

  // Allocate one more page - should be at the end since no more free slots
  size_t new_end_offset = disk_manager.allocate_page();
  size_t expected_new_offset =
      static_cast<size_t>(num_initial_pages) * bw_graph::BW_GRAPH_PAGE_SIZE;
  EXPECT_GE(new_end_offset, expected_new_offset);

  // Test writing to all allocated pages
  for (size_t i = 0; i < allocated_offsets.size(); ++i) {
    std::memset(test_data, 'Z' - i, bw_graph::BW_GRAPH_PAGE_SIZE);
    page_no_t page_no = static_cast<page_no_t>(allocated_offsets[i] / bw_graph::BW_GRAPH_PAGE_SIZE);
    disk_manager.write_page(page_no, test_data);

    // Verify read back
    char* read_data = new char[bw_graph::BW_GRAPH_PAGE_SIZE];
    disk_manager.read_page(page_no, read_data);
    EXPECT_EQ(std::memcmp(test_data, read_data, bw_graph::BW_GRAPH_PAGE_SIZE), 0);
    delete[] read_data;
  }

  delete[] test_data;
  disk_manager.shutdown();

  // Clean up test files
  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  if (std::filesystem::exists(test_db_file.string() + ".log")) {
    std::filesystem::remove(test_db_file.string() + ".log");
  }

  std::cout << "Multiple delete and allocate cycle works correctly" << std::endl;
}
