#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/common/config.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <random>
#include <tbb/parallel_for.h>

// Test fixture - sets up a delta buf_pool and disk_manager for each test.

class PageMapTest : public ::testing::Test {
protected:
  disk_manager_t* disk_mgr{nullptr};
  buf_pool_t<delta_page_t>* pool{nullptr};
  page_map_t* map{nullptr};
  std::filesystem::path tmp_path;

  void SetUp() override {
    // Use a unique temp file per test to avoid interference
    tmp_path = std::filesystem::temp_directory_path() /
               ("test_page_map_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)) + ".delta");
    disk_mgr = new disk_manager_t(tmp_path, bw_graph::BW_DELTA_PAGE_SIZE);
    // 2 chunks × 1024 pages = 2048 delta pages (~8 MB)
    pool = buf_pool_t<delta_page_t>::buf_pool_init(2, 1024, bw_graph::BW_DELTA_PAGE_SIZE);
    map = new page_map_t(pool, disk_mgr);
  }

  void TearDown() override {
    delete map;
    delete pool;
    delete disk_mgr;
    std::filesystem::remove(tmp_path);
    std::filesystem::remove(tmp_path.string() + ".log");
  }
};

// Test PageMapTest.init test.
TEST_F(PageMapTest, InitTest) { EXPECT_NE(map, nullptr); }

// Test PageMapTest.insert delta test.
TEST_F(PageMapTest, InsertDeltaTest) {
  std::vector<delta_record_t> deltas;
  // Add various types of delta records
  for (v_id_t i = 1; i <= 5; ++i) {
    // Add vertex insertion
    delta_record_t vertex_record;
    vertex_record.target = VERTEX;
    vertex_record.type = INSERT;
    vertex_record.first = i * 10;
    vertex_record.time = generate_timestamp();
    deltas.push_back(vertex_record);

    // Add edge insertion
    delta_record_t edge_record;
    edge_record.target = EDGE;
    edge_record.type = INSERT;
    edge_record.first = i * 10;
    edge_record.second = (i * 10) + 1;
    edge_record.time = generate_timestamp();
    deltas.push_back(edge_record);
  }
  map->insert_delta(0, deltas);
}

// Test PageMapTest.insert more delta test.
TEST_F(PageMapTest, InsertMoreDeltaTest) {
  std::vector<delta_record_t> deltas;
  // Add various types of delta records
  std::cout << "Inserting first batch of delta records..." << std::endl;
  for (v_id_t i = 1; i <= 5000; ++i) {
    // Add vertex insertion
    delta_record_t vertex_record;
    vertex_record.target = VERTEX;
    vertex_record.type = INSERT;
    vertex_record.first = i * 10;
    vertex_record.time = generate_timestamp();
    deltas.push_back(vertex_record);

    // Add edge insertion
    delta_record_t edge_record;
    edge_record.target = EDGE;
    edge_record.type = INSERT;
    edge_record.first = i * 10;
    edge_record.second = (i * 10) + 1;
    edge_record.time = generate_timestamp();
    deltas.push_back(edge_record);
  }
  map->insert_delta(0, deltas);

  deltas.clear();
  std::cout << "Inserting next batch of delta records..." << std::endl;
  // Next batch of delta records
  for (v_id_t i = 1; i <= 5000; ++i) {
    // Add vertex insertion
    delta_record_t vertex_record;
    vertex_record.target = VERTEX;
    vertex_record.type = INSERT;
    vertex_record.first = i * 10;
    vertex_record.time = generate_timestamp();
    deltas.push_back(vertex_record);

    // Add edge insertion
    delta_record_t edge_record;
    edge_record.target = EDGE;
    edge_record.type = INSERT;
    edge_record.first = i * 10;
    edge_record.second = (i * 10) + 1;
    edge_record.time = generate_timestamp();
    deltas.push_back(edge_record);
  }
  map->insert_delta(0, deltas);
}

TEST_F(PageMapTest, FetchDeltasReturnsOldToNewAcrossPages) {
  page_no_t csr_page_no = 7;
  size_t records_per_page =
      (bw_graph::BW_DELTA_PAGE_SIZE - 3 * sizeof(uint64_t)) / sizeof(delta_record_t);
  ASSERT_GT(records_per_page, 0u);
  size_t record_count = records_per_page + 3;

  for (size_t i = 0; i < record_count; ++i) {
    delta_record_t rec(EDGE, (i % 2 == 0) ? INSERT : DELETE, 1, static_cast<v_id_t>(1000 + i));
    map->insert_delta_and_get_loc(csr_page_no, rec);
  }

  ASSERT_GT(map->get_delta_chain_length(csr_page_no), 1u);

  std::vector<delta_record_t> fetched = map->fetch_deltas_with_mark(csr_page_no);
  ASSERT_EQ(fetched.size(), record_count);
  for (size_t i = 0; i < fetched.size(); ++i) {
    EXPECT_EQ(fetched[i].second, static_cast<v_id_t>(1000 + i))
        << "SMO must apply deltas in insertion order";
  }
}

// Test PageMapTest.parallel insert delta test.
TEST_F(PageMapTest, ParallelInsertDeltaTest) {
  std::vector<csr_page_t> csr_pages;
  csr_pages.emplace_back(csr_page_t(0));
  csr_pages.emplace_back(csr_page_t(1));
  csr_pages.emplace_back(csr_page_t(2));

  const int num_threads = 10;
  const int operations_per_thread = 5000;

  std::atomic<int> total_operations{0};
  std::atomic<int> success_count{0};
  std::atomic<int> failure_count{0};
  std::atomic<long long> total_records_inserted{0};

  auto start_time = std::chrono::high_resolution_clock::now();

  std::cout << "Starting high contention test with " << num_threads << " threads competing for "
            << csr_pages.size() << " pages..." << std::endl;
  auto insertion_task = [&](const tbb::blocked_range<int>& range) {
    std::random_device rd;
    std::mt19937 gen(rd() + range.begin());
    std::uniform_int_distribution<> page_dis(0, 2);
    std::uniform_int_distribution<> record_dis(1, 1000);
    std::uniform_int_distribution<> batch_dis(1, 20);

    for (int thread_id = range.begin(); thread_id != range.end(); ++thread_id) {
      for (int op = 0; op < operations_per_thread; ++op) {
        try {
          // Select a random page
          int page_idx = page_dis(gen);
          csr_page_t* target_page = &csr_pages[page_idx];

          // Generate random batch of records
          std::vector<delta_record_t> deltas;
          int batch_size = batch_dis(gen);

          for (int j = 0; j < batch_size; ++j) {
            // Vertex delta record
            delta_record_t vertex_record;
            vertex_record.target = VERTEX;
            vertex_record.type = INSERT;
            vertex_record.first = record_dis(gen);
            vertex_record.time = generate_timestamp();
            deltas.push_back(vertex_record);

            // Edge delta record
            delta_record_t edge_record;
            edge_record.target = EDGE;
            edge_record.type = INSERT;
            edge_record.first = record_dis(gen);
            edge_record.second = record_dis(gen);
            edge_record.time = generate_timestamp();
            deltas.push_back(edge_record);
          }

          // Execute insertion operation
          map->insert_delta(target_page->get_page_no(), deltas);

          success_count++;
          total_records_inserted += deltas.size();

          // Print progress every 100 operations
          if (op % 100 == 0) {
            std::cout << "Thread " << thread_id << " completed " << op << " operations on page "
                      << page_idx << std::endl;
          }

        } catch (const std::exception& e) {
          failure_count++;
          std::cout << "Thread " << thread_id << " operation failed: " << e.what() << std::endl;
        } catch (...) {
          failure_count++;
          std::cout << "Thread " << thread_id << " operation failed with unknown exception"
                    << std::endl;
        }

        total_operations++;
      }

      std::cout << "Thread " << thread_id << " completed all operations" << std::endl;
    }
  };

  tbb::parallel_for(tbb::blocked_range<int>(0, num_threads), insertion_task);

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  // Print summary statistics
  std::cout << "\n=== High Contention Test Results ===" << std::endl;
  std::cout << "Total operations: " << total_operations << std::endl;
  std::cout << "Successful operations: " << success_count << std::endl;
  std::cout << "Failed operations: " << failure_count << std::endl;
  std::cout << "Total records inserted: " << total_records_inserted << std::endl;
  std::cout << "Test duration: " << duration.count() << " ms" << std::endl;
  std::cout << "Operations per second: " << (total_operations.load() * 1000.0 / duration.count())
            << std::endl;
  std::cout << "Success rate: " << (success_count.load() * 100.0 / total_operations.load()) << "%"
            << std::endl;
}
