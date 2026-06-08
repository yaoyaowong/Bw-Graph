#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/common/utils.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>

// Helper function to create test delta records
std::vector<delta_record_t> create_test_delta_records() {
  std::vector<delta_record_t> records;

  // Add vertex insertion
  delta_record_t vertex_insert;
  vertex_insert.target = VERTEX;
  vertex_insert.type = INSERT;
  vertex_insert.first = 100;
  vertex_insert.second = 0;
  vertex_insert.time = generate_timestamp();
  records.push_back(vertex_insert);

  // Add edge insertion
  delta_record_t edge_insert;
  edge_insert.target = EDGE;
  edge_insert.type = INSERT;
  edge_insert.first = 100;
  edge_insert.second = 200;
  edge_insert.time = generate_timestamp();
  records.push_back(edge_insert);

  // Add edge deletion
  delta_record_t edge_delete;
  edge_delete.target = EDGE;
  edge_delete.type = DELETE;
  edge_delete.first = 200;
  edge_delete.second = 300;
  edge_delete.time = generate_timestamp();
  records.push_back(edge_delete);

  return records;
}

// Constructor cases.
TEST(DeltaPageConstructorTest, DefaultConstructorCreatesEmptyPage) {
  delta_page_t delta_page(bw_graph::BW_GRAPH_DEFAULT_PAGE_NO);

  // Should have default page number
  EXPECT_EQ(delta_page.get_page_no(), bw_graph::BW_GRAPH_DEFAULT_PAGE_NO);

  // Should have valid data pointer
  EXPECT_NE(delta_page.get_data(), nullptr);

  // Should be empty initially
  std::vector<delta_record_t> records = delta_page.get_related_records(100);
  EXPECT_EQ(records.size(), 0);
}

// Test DeltaPageConstructorTest.parameterized constructor sets page number.
TEST(DeltaPageConstructorTest, ParameterizedConstructorSetsPageNumber) {
  page_no_t test_page_no = 42;
  delta_page_t delta_page(test_page_no);

  EXPECT_EQ(delta_page.get_page_no(), test_page_no);
  EXPECT_NE(delta_page.get_data(), nullptr);

  // Should be empty initially
  std::vector<delta_record_t> records = delta_page.get_related_records(100);
  EXPECT_EQ(records.size(), 0);
}

// Insert Delta Tests
TEST(DeltaPageFunctionalityTest, InsertSingleDelta) {
  delta_page_t delta_page(1);

  delta_record_t record;
  record.target = VERTEX;
  record.type = INSERT;
  record.first = 100;
  record.second = 0;
  record.time = generate_timestamp();

  // Insert the record
  EXPECT_NO_THROW(delta_page.insert_delta(record));

  // Retrieve related records
  std::vector<delta_record_t> related = delta_page.get_related_records(100);
  EXPECT_EQ(related.size(), 1);
  EXPECT_EQ(related[0].target, VERTEX);
  EXPECT_EQ(related[0].type, INSERT);
  EXPECT_EQ(related[0].first, 100);
  EXPECT_EQ(related[0].second, 0);
}

// Test DeltaPageFunctionalityTest.insert multiple deltas.
TEST(DeltaPageFunctionalityTest, InsertMultipleDeltas) {
  delta_page_t delta_page(2);
  auto test_records = create_test_delta_records();

  // Insert all test records
  for (const auto& record : test_records) {
    EXPECT_NO_THROW(delta_page.insert_delta(record));
  }

  // Test retrieval for vertex 100 (should find 2 records)
  std::vector<delta_record_t> records_100 = delta_page.get_related_records(100);
  EXPECT_EQ(records_100.size(), 2);

  // Test retrieval for vertex 200 (should find 2 records)
  std::vector<delta_record_t> records_200 = delta_page.get_related_records(200);
  EXPECT_EQ(records_200.size(), 1);

  // Test retrieval for vertex 300 (should find 1 record)
  std::vector<delta_record_t> records_300 = delta_page.get_related_records(300);
  EXPECT_EQ(records_300.size(), 0);

  // Test retrieval for non-existent vertex
  std::vector<delta_record_t> records_404 = delta_page.get_related_records(404);
  EXPECT_EQ(records_404.size(), 0);
}

// Self Parse Tests
TEST(DeltaPageParseTest, SelfParseWithoutData) {
  delta_page_t delta_page(3);

  // Self parse on empty page should not throw
  EXPECT_NO_THROW(delta_page.self_parse());

  // Should still return empty results
  std::vector<delta_record_t> records = delta_page.get_related_records(100);
  EXPECT_EQ(records.size(), 0);
}

// Disk I/O Tests
TEST(DeltaPageDiskIOTest, WriteAndReadFromDisk) {
  const std::string test_file = "test_delta_page.bin";
  page_no_t test_page_no = 123;

  // Clean up any existing test file
  if (std::filesystem::exists(test_file)) {
    std::filesystem::remove(test_file);
  }

  // Create and populate delta page
  {
    delta_page_t write_page(test_page_no);
    auto test_records = create_test_delta_records();

    for (const auto& record : test_records) {
      write_page.insert_delta(record);
    }

    // Write page to disk
    std::ofstream out_file(test_file, std::ios::binary);
    ASSERT_TRUE(out_file.is_open());

    out_file.write(reinterpret_cast<const char*>(write_page.get_data()),
                   bw_graph::BW_DELTA_PAGE_SIZE);
    out_file.close();
  }

  // Read page from disk and verify
  {
    // Create new page and read data
    delta_page_t read_page(test_page_no);

    std::ifstream in_file(test_file, std::ios::binary);
    ASSERT_TRUE(in_file.is_open());

    in_file.read(reinterpret_cast<char*>(read_page.get_data()), bw_graph::BW_DELTA_PAGE_SIZE);
    in_file.close();

    // Must call self_parse to initialize internal structures
    EXPECT_NO_THROW(read_page.self_parse());

    // Verify data integrity
    std::vector<delta_record_t> records_100 = read_page.get_related_records(100);
    EXPECT_EQ(records_100.size(), 2);

    std::vector<delta_record_t> records_200 = read_page.get_related_records(200);
    EXPECT_EQ(records_200.size(), 1);

    std::vector<delta_record_t> records_300 = read_page.get_related_records(300);
    EXPECT_EQ(records_300.size(), 0);

    // Verify page number is preserved
    EXPECT_EQ(read_page.get_page_no(), test_page_no);
  }

  // Clean up test file
  std::filesystem::remove(test_file);
}

// Test DeltaPageDiskIOTest.write empty page and read.
TEST(DeltaPageDiskIOTest, WriteEmptyPageAndRead) {
  const std::string test_file = "test_empty_delta_page.bin";
  page_no_t test_page_no = 456;

  // Clean up any existing test file
  if (std::filesystem::exists(test_file)) {
    std::filesystem::remove(test_file);
  }

  // Create empty page and write to disk
  {
    delta_page_t empty_page(test_page_no);

    std::ofstream out_file(test_file, std::ios::binary);
    ASSERT_TRUE(out_file.is_open());

    std::cout << "Before delta page written to disk." << std::endl;

    out_file.write(reinterpret_cast<const char*>(empty_page.get_data()),
                   bw_graph::BW_DELTA_PAGE_SIZE);

    std::cout << "After delta page written to disk." << std::endl;

    out_file.close();
  }

  // Read empty page from disk
  {
    delta_page_t read_page(test_page_no);

    std::ifstream in_file(test_file, std::ios::binary);
    ASSERT_TRUE(in_file.is_open());

    in_file.read(reinterpret_cast<char*>(read_page.get_data()), bw_graph::BW_DELTA_PAGE_SIZE);
    in_file.close();

    // Self parse should handle empty page correctly
    EXPECT_NO_THROW(read_page.self_parse());

    // Should return empty results
    std::vector<delta_record_t> records = read_page.get_related_records(100);
    EXPECT_EQ(records.size(), 0);

    EXPECT_EQ(read_page.get_page_no(), test_page_no);
  }

  // Clean up test file
  std::filesystem::remove(test_file);
}

// Integration Test
TEST(DeltaPageIntegrationTest, CompleteWorkflow) {
  const std::string test_file = "test_integration_delta.bin";
  page_no_t test_page_no = 789;

  // Clean up any existing test file
  if (std::filesystem::exists(test_file)) {
    std::filesystem::remove(test_file);
  }

  // Phase 1: Create, populate, and save page
  {
    delta_page_t delta_page(test_page_no);

    // Add various types of delta records
    for (v_id_t i = 1; i <= 5; ++i) {
      // Add vertex insertion
      delta_record_t vertex_record;
      vertex_record.target = VERTEX;
      vertex_record.type = INSERT;
      vertex_record.first = i * 10;
      vertex_record.time = generate_timestamp();
      delta_page.insert_delta(vertex_record);

      // Add edge insertion
      delta_record_t edge_record;
      edge_record.target = EDGE;
      edge_record.type = INSERT;
      edge_record.first = i * 10;
      edge_record.second = (i * 10) + 1;
      edge_record.time = generate_timestamp();
      delta_page.insert_delta(edge_record);
    }

    // Save to disk
    std::ofstream out_file(test_file, std::ios::binary);
    ASSERT_TRUE(out_file.is_open());
    out_file.write(reinterpret_cast<const char*>(delta_page.get_data()),
                   bw_graph::BW_DELTA_PAGE_SIZE);
    out_file.close();
  }

  // Phase 2: Load from disk and verify
  {
    delta_page_t loaded_page(test_page_no);

    // Load from disk
    std::ifstream in_file(test_file, std::ios::binary);
    ASSERT_TRUE(in_file.is_open());
    in_file.read(reinterpret_cast<char*>(loaded_page.get_data()), bw_graph::BW_DELTA_PAGE_SIZE);
    in_file.close();

    // Parse the loaded data
    EXPECT_NO_THROW(loaded_page.self_parse());

    // Verify all data is correctly loaded
    for (v_id_t i = 1; i <= 5; ++i) {
      v_id_t vertex_id = i * 10;
      std::vector<delta_record_t> records = loaded_page.get_related_records(vertex_id);

      // Each vertex should have 2 related records (vertex insert + edge insert)
      EXPECT_EQ(records.size(), 2) << "Vertex " << vertex_id << " should have 2 records";

      // Verify record types
      bool found_vertex_insert = false;
      bool found_edge_insert = false;

      for (const auto& record : records) {
        if (record.target == VERTEX && record.type == INSERT) {
          found_vertex_insert = true;
          EXPECT_EQ(record.first, vertex_id);
        } else if (record.target == EDGE && record.type == INSERT) {
          found_edge_insert = true;
          EXPECT_EQ(record.first, vertex_id);
          EXPECT_EQ(record.second, vertex_id + 1);
        }
      }

      EXPECT_TRUE(found_vertex_insert) << "Missing vertex insert for " << vertex_id;
      EXPECT_TRUE(found_edge_insert) << "Missing edge insert for " << vertex_id;
    }
  }

  // Clean up test file
  std::filesystem::remove(test_file);
}

class DeltaRecordManagementTest : public ::testing::Test {
protected:
  std::filesystem::path tmp_dir_;
  disk_manager_t* disk_mgr_{nullptr};
  buf_pool_t<delta_page_t>* pool_{nullptr};
  page_map_t* page_map_{nullptr};

  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() /
               ("test_delta_record_mgmt_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(tmp_dir_);

    disk_mgr_ = new disk_manager_t(tmp_dir_ / "delta.db", bw_graph::BW_DELTA_PAGE_SIZE);
    pool_ = buf_pool_t<delta_page_t>::buf_pool_init(2, 128, bw_graph::BW_DELTA_PAGE_SIZE);
    page_map_ = new page_map_t(pool_, disk_mgr_);
  }

  void TearDown() override {
    delete page_map_;
    delete pool_;
    delete disk_mgr_;
    std::filesystem::remove_all(tmp_dir_);
  }

  delta_record_t read_record(page_no_t page_no, uint32_t record_idx) {
    delta_page_t* page = pool_->buf_page_read(page_no, disk_mgr_);
    delta_record_t rec = page->get_record(record_idx);
    page->r_unlatch();
    return rec;
  }

  static size_t records_per_page() {
    size_t payload_bytes = bw_graph::BW_DELTA_PAGE_SIZE - 3 * sizeof(uint64_t);
    return payload_bytes / sizeof(delta_record_t);
  }
};

// Test DeltaRecordChainTest.link fields round trip in delta page.
TEST(DeltaRecordChainTest, LinkFieldsRoundTripInDeltaPage) {
  delta_page_t page(7);
  delta_record_t rec(EDGE, INSERT, 10, 11);
  rec.next_page_no = 33;
  rec.next_record_idx = 9;
  ASSERT_TRUE(page.insert_delta(rec));

  delta_page_t loaded(7);
  std::memcpy(loaded.get_data(), page.get_data(), bw_graph::BW_DELTA_PAGE_SIZE);
  loaded.self_parse();

  const delta_record_t& loaded_rec = loaded.get_record(0);
  EXPECT_TRUE(loaded_rec.has_next());
  EXPECT_EQ(loaded_rec.next_page_no, 33);
  EXPECT_EQ(loaded_rec.next_record_idx, 9u);
  EXPECT_EQ(loaded_rec.first, 10u);
  EXPECT_EQ(loaded_rec.second, 11u);
}

// Test DeltaRecordManagementTest.insert delta and get loc stores stable location and next link.
TEST_F(DeltaRecordManagementTest, InsertDeltaAndGetLocStoresStableLocationAndNextLink) {
  constexpr page_no_t csr_page_no = 100;

  delta_record_t first(EDGE, INSERT, 1, 101);
  auto [first_page_no, first_idx] = page_map_->insert_delta_and_get_loc(csr_page_no, first);

  delta_record_t second(EDGE, DELETE, 1, 102);
  second.next_page_no = first_page_no;
  second.next_record_idx = first_idx;
  auto [second_page_no, second_idx] = page_map_->insert_delta_and_get_loc(csr_page_no, second);

  EXPECT_GE(page_map_->get_delta_chain_length(csr_page_no), 1u);

  delta_record_t second_stored = read_record(second_page_no, second_idx);
  EXPECT_EQ(second_stored.first, 1u);
  EXPECT_EQ(second_stored.second, 102u);
  EXPECT_EQ(second_stored.next_page_no, first_page_no);
  EXPECT_EQ(second_stored.next_record_idx, first_idx);
}

// Test DeltaRecordManagementTest.insert delta and get loc prepends new page when head becomes full.
TEST_F(DeltaRecordManagementTest, InsertDeltaAndGetLocPrependsNewPageWhenHeadBecomesFull) {
  constexpr page_no_t csr_page_no = 200;
  const size_t capacity = records_per_page();
  ASSERT_GT(capacity, 1u);

  std::pair<page_no_t, uint32_t> last_loc{};
  for (size_t i = 0; i < capacity; ++i) {
    delta_record_t rec(EDGE, INSERT, 2, static_cast<v_id_t>(1000 + i));
    last_loc = page_map_->insert_delta_and_get_loc(csr_page_no, rec);
  }

  EXPECT_EQ(page_map_->get_delta_chain_length(csr_page_no), 1u);

  delta_record_t overflow_rec(EDGE, INSERT, 2, 9999);
  overflow_rec.next_page_no = last_loc.first;
  overflow_rec.next_record_idx = last_loc.second;

  auto [new_head_page_no, new_head_idx] =
      page_map_->insert_delta_and_get_loc(csr_page_no, overflow_rec);

  EXPECT_EQ(new_head_idx, 0u);
  EXPECT_NE(new_head_page_no, last_loc.first);
  EXPECT_EQ(page_map_->get_delta_chain_length(csr_page_no), 2u);

  delta_record_t stored = read_record(new_head_page_no, new_head_idx);
  EXPECT_EQ(stored.next_page_no, last_loc.first);
  EXPECT_EQ(stored.next_record_idx, last_loc.second);
}

// Test DeltaRecordManagementTest.fetch deltas with mark resets chain and allows fresh insertion.
TEST_F(DeltaRecordManagementTest, FetchDeltasWithMarkResetsChainAndAllowsFreshInsertion) {
  constexpr page_no_t csr_page_no = 300;

  std::pair<page_no_t, uint32_t> prev_loc{INVALID_DELTA_PAGE_NO, INVALID_DELTA_RECORD_IDX};
  for (v_id_t dst = 501; dst <= 503; ++dst) {
    delta_record_t rec(EDGE, INSERT, 3, dst);
    if (prev_loc.first != INVALID_DELTA_PAGE_NO) {
      rec.next_page_no = prev_loc.first;
      rec.next_record_idx = prev_loc.second;
    }
    prev_loc = page_map_->insert_delta_and_get_loc(csr_page_no, rec);
  }

  auto fetched = page_map_->fetch_deltas_with_mark(csr_page_no);
  EXPECT_EQ(fetched.size(), 3u);

  std::set<v_id_t> fetched_dsts;
  for (const auto& rec : fetched) {
    fetched_dsts.insert(rec.second);
  }
  EXPECT_EQ(fetched_dsts, (std::set<v_id_t>{501, 502, 503}));
  EXPECT_EQ(page_map_->get_delta_chain_length(csr_page_no), 0u);

  delta_record_t fresh(EDGE, DELETE, 3, 700);
  auto [fresh_page_no, fresh_idx] = page_map_->insert_delta_and_get_loc(csr_page_no, fresh);
  EXPECT_EQ(fresh_idx, 0u);
  EXPECT_EQ(page_map_->get_delta_chain_length(csr_page_no), 1u);

  delta_record_t fresh_stored = read_record(fresh_page_no, fresh_idx);
  EXPECT_EQ(fresh_stored.type, DELETE);
  EXPECT_EQ(fresh_stored.second, 700u);
}
