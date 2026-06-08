#include "bw_graph/mem/mem_edges.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Helper function to create test edge list file
inline void create_test_edge_file(const std::string& filename,
                                  const std::vector<std::pair<v_id_t, v_id_t>>& edges) {
  std::ofstream file(filename);

  for (const auto& edge : edges) {
    file << edge.first << " " << edge.second << "\n";
  }

  file.close();
}

// Helper function to create test edge list file with comments
inline void create_test_edge_file_with_comments(const std::string& filename) {
  std::ofstream file(filename);

  file << "# This is a comment\n";
  file << "0 1\n";
  file << "# Another comment\n";
  file << "1 2\n";
  file << "\n"; // Empty line
  file << "2 3\n";
  file << "3 4\n";

  file.close();
}

// Helper function to create large test edge list file
inline void create_large_test_edge_file(const std::string& filename, size_t num_edges) {
  std::ofstream file(filename);

  for (size_t i = 0; i < num_edges; ++i) {
    file << i << " " << (i + 1) << "\n";
  }

  file.close();
}

// mem_edge_list_t constructor cases.
TEST(MemEdgeListConstructorTest, DefaultConstructorCreatesEmptyEdgeList) {
  mem_edge_list_t edge_list;

  EXPECT_EQ(edge_list.edge_count(), 0);
  EXPECT_EQ(edge_list.get_graph_name(), "");
  EXPECT_TRUE(edge_list.get_edges().empty());
}

// Test MemEdgeListConstructorTest.constructor with graph name sets correct path.
TEST(MemEdgeListConstructorTest, ConstructorWithGraphNameSetsCorrectPath) {
  mem_edge_list_t edge_list("test_graph");

  EXPECT_EQ(edge_list.get_graph_name(), "test_graph");
  EXPECT_EQ(edge_list.get_file_path(), "data/test_graph.delta");
  EXPECT_EQ(edge_list.edge_count(), 0);
}

// Test MemEdgeListConstructorTest.constructor with custom path sets correct path.
TEST(MemEdgeListConstructorTest, ConstructorWithCustomPathSetsCorrectPath) {
  mem_edge_list_t edge_list("test_graph", "custom/path/edges.txt");

  EXPECT_EQ(edge_list.get_graph_name(), "test_graph");
  EXPECT_EQ(edge_list.get_file_path(), "custom/path/edges.txt");
  EXPECT_EQ(edge_list.edge_count(), 0);
}

// Test MemEdgeListConstructorTest.constructor with custom buffer size.
TEST(MemEdgeListConstructorTest, ConstructorWithCustomBufferSize) {
  mem_edge_list_t edge_list("test_graph", 128 * 1024);

  EXPECT_EQ(edge_list.get_graph_name(), "test_graph");
  EXPECT_EQ(edge_list.edge_count(), 0);
}

// mem_edge_list_t File Loading Tests
TEST(MemEdgeListFileLoadingTest, LoadFromFileReadsEdgesCorrectly) {
  std::string test_file = "test_edges_basic.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  bool success = edge_list.load_from_file();

  EXPECT_TRUE(success);
  EXPECT_EQ(edge_list.edge_count(), 5);

  const auto& edges = edge_list.get_edges();
  for (size_t i = 0; i < test_edges.size(); ++i) {
    EXPECT_EQ(edges[i].first, test_edges[i].first);
    EXPECT_EQ(edges[i].second, test_edges[i].second);
  }

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListFileLoadingTest.load from file handles comments and empty lines.
TEST(MemEdgeListFileLoadingTest, LoadFromFileHandlesCommentsAndEmptyLines) {
  std::string test_file = "test_edges_comments.txt";
  create_test_edge_file_with_comments(test_file);

  mem_edge_list_t edge_list("test_graph", test_file);
  bool success = edge_list.load_from_file();

  EXPECT_TRUE(success);
  EXPECT_EQ(edge_list.edge_count(), 4);

  const auto& edges = edge_list.get_edges();
  EXPECT_EQ(edges[0].first, 0);
  EXPECT_EQ(edges[0].second, 1);
  EXPECT_EQ(edges[1].first, 1);
  EXPECT_EQ(edges[1].second, 2);
  EXPECT_EQ(edges[2].first, 2);
  EXPECT_EQ(edges[2].second, 3);
  EXPECT_EQ(edges[3].first, 3);
  EXPECT_EQ(edges[3].second, 4);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListFileLoadingTest.load from file returns false for non existent file.
TEST(MemEdgeListFileLoadingTest, LoadFromFileReturnsFalseForNonExistentFile) {
  mem_edge_list_t edge_list("test_graph", "nonexistent_file.txt");
  bool success = edge_list.load_from_file();

  EXPECT_FALSE(success);
  EXPECT_EQ(edge_list.edge_count(), 0);
}

// Test MemEdgeListFileLoadingTest.load from file with custom path.
TEST(MemEdgeListFileLoadingTest, LoadFromFileWithCustomPath) {
  std::string test_file = "test_edges_custom_path.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{10, 20}, {20, 30}, {30, 40}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph");
  bool success = edge_list.load_from_file(test_file);

  EXPECT_TRUE(success);
  EXPECT_EQ(edge_list.edge_count(), 3);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListFileLoadingTest.load from file handles large numbers.
TEST(MemEdgeListFileLoadingTest, LoadFromFileHandlesLargeNumbers) {
  std::string test_file = "test_edges_large_numbers.txt";
  std::ofstream file(test_file);
  file << "1000000 2000000\n";
  file << "3000000 4000000\n";
  file << "5000000 6000000\n";
  file.close();

  mem_edge_list_t edge_list("test_graph", test_file);
  bool success = edge_list.load_from_file();

  EXPECT_TRUE(success);
  EXPECT_EQ(edge_list.edge_count(), 3);

  const auto& edges = edge_list.get_edges();
  EXPECT_EQ(edges[0].first, 1000000);
  EXPECT_EQ(edges[0].second, 2000000);
  EXPECT_EQ(edges[1].first, 3000000);
  EXPECT_EQ(edges[1].second, 4000000);
  EXPECT_EQ(edges[2].first, 5000000);
  EXPECT_EQ(edges[2].second, 6000000);

  // Clean up
  std::remove(test_file.c_str());
}

// mem_edge_list_t Edge Manipulation Tests
TEST(MemEdgeListEdgeManipulationTest, AddEdgeIncreasesEdgeCount) {
  mem_edge_list_t edge_list;

  EXPECT_EQ(edge_list.edge_count(), 0);

  edge_list.add_edge(0, 1);
  EXPECT_EQ(edge_list.edge_count(), 1);

  edge_list.add_edge(1, 2);
  EXPECT_EQ(edge_list.edge_count(), 2);

  edge_list.add_edge(2, 3);
  EXPECT_EQ(edge_list.edge_count(), 3);
}

// Test MemEdgeListEdgeManipulationTest.add edge stores correct values.
TEST(MemEdgeListEdgeManipulationTest, AddEdgeStoresCorrectValues) {
  mem_edge_list_t edge_list;

  edge_list.add_edge(10, 20);
  edge_list.add_edge(30, 40);
  edge_list.add_edge(50, 60);

  const auto& edges = edge_list.get_edges();
  EXPECT_EQ(edges.size(), 3);
  EXPECT_EQ(edges[0].first, 10);
  EXPECT_EQ(edges[0].second, 20);
  EXPECT_EQ(edges[1].first, 30);
  EXPECT_EQ(edges[1].second, 40);
  EXPECT_EQ(edges[2].first, 50);
  EXPECT_EQ(edges[2].second, 60);
}

// Test MemEdgeListEdgeManipulationTest.clear removes all edges.
TEST(MemEdgeListEdgeManipulationTest, ClearRemovesAllEdges) {
  mem_edge_list_t edge_list;

  edge_list.add_edge(0, 1);
  edge_list.add_edge(1, 2);
  edge_list.add_edge(2, 3);

  EXPECT_EQ(edge_list.edge_count(), 3);

  edge_list.clear();

  EXPECT_EQ(edge_list.edge_count(), 0);
  EXPECT_TRUE(edge_list.get_edges().empty());
}

// mem_edge_list_t Sequential Operation Tests
TEST(MemEdgeListSequentialOperationTest, ApplySequentialExecutesForAllEdges) {
  std::string test_file = "test_edges_sequential.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  std::vector<std::pair<v_id_t, v_id_t>> processed_edges;

  edge_list.apply_sequential(
      [&](v_id_t src, v_id_t dst) { processed_edges.emplace_back(src, dst); },
      insert_mode_t::RANDOM, false);

  EXPECT_EQ(processed_edges.size(), 5);
  for (size_t i = 0; i < test_edges.size(); ++i) {
    EXPECT_EQ(processed_edges[i].first, test_edges[i].first);
    EXPECT_EQ(processed_edges[i].second, test_edges[i].second);
  }

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListSequentialOperationTest.apply sequential counts edges correctly.
TEST(MemEdgeListSequentialOperationTest, ApplySequentialCountsEdgesCorrectly) {
  std::string test_file = "test_edges_count.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  size_t count = 0;
  edge_list.apply_sequential([&](v_id_t src, v_id_t dst) {
    (void) src;
    (void) dst;
    count++;
  });

  EXPECT_EQ(count, 3);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListSequentialOperationTest.apply sequential works with empty list.
TEST(MemEdgeListSequentialOperationTest, ApplySequentialWorksWithEmptyList) {
  mem_edge_list_t edge_list;

  size_t count = 0;
  edge_list.apply_sequential([&](v_id_t src, v_id_t dst) {
    (void) src;
    (void) dst;
    count++;
  });

  EXPECT_EQ(count, 0);
}

// mem_edge_list_t Parallel Operation Tests
TEST(MemEdgeListParallelOperationTest, ApplyParallelExecutesForAllEdges) {
  std::string test_file = "test_edges_parallel.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges;
  for (int i = 0; i < 1000; ++i) {
    test_edges.emplace_back(i, i + 1);
  }
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  std::atomic<size_t> count{0};

  edge_list.apply_parallel(
      [&](v_id_t src, v_id_t dst) {
        (void) src;
        (void) dst;
        count++;
      },
      4);

  EXPECT_EQ(count, 1000);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListParallelOperationTest.apply parallel with default thread count.
TEST(MemEdgeListParallelOperationTest, ApplyParallelWithDefaultThreadCount) {
  std::string test_file = "test_edges_parallel_default.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges;
  for (int i = 0; i < 500; ++i) {
    test_edges.emplace_back(i, i + 1);
  }
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  std::atomic<size_t> count{0};

  edge_list.apply_parallel([&](v_id_t src, v_id_t dst) {
    (void) src;
    (void) dst;
    count++;
  });

  EXPECT_EQ(count, 500);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListParallelOperationTest.apply parallel works with empty list.
TEST(MemEdgeListParallelOperationTest, ApplyParallelWorksWithEmptyList) {
  mem_edge_list_t edge_list;

  std::atomic<size_t> count{0};
  edge_list.apply_parallel(
      [&](v_id_t src, v_id_t dst) {
        (void) src;
        (void) dst;
        count++;
      },
      4);

  EXPECT_EQ(count, 0);
}

// mem_edge_list_t Range Operation Tests
TEST(MemEdgeListRangeOperationTest, ApplyRangeExecutesForSpecifiedRange) {
  std::string test_file = "test_edges_range.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5},
                                                       {5, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  std::vector<std::pair<v_id_t, v_id_t>> processed_edges;

  edge_list.apply_range([&](v_id_t src, v_id_t dst) { processed_edges.emplace_back(src, dst); }, 2,
                        7);

  EXPECT_EQ(processed_edges.size(), 5);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(processed_edges[i].first, test_edges[i + 2].first);
    EXPECT_EQ(processed_edges[i].second, test_edges[i + 2].second);
  }

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListRangeOperationTest.apply range throws for invalid range.
TEST(MemEdgeListRangeOperationTest, ApplyRangeThrowsForInvalidRange) {
  std::string test_file = "test_edges_invalid_range.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  // Test start >= end
  EXPECT_THROW(edge_list.apply_range(
                   [&](v_id_t src, v_id_t dst) {
                     (void) src;
                     (void) dst;
                   },
                   5, 3),
               std::invalid_argument);

  // Test end > size
  EXPECT_THROW(edge_list.apply_range(
                   [&](v_id_t src, v_id_t dst) {
                     (void) src;
                     (void) dst;
                   },
                   0, 10),
               std::invalid_argument);

  // Test start >= size
  EXPECT_THROW(edge_list.apply_range(
                   [&](v_id_t src, v_id_t dst) {
                     (void) src;
                     (void) dst;
                   },
                   10, 15),
               std::invalid_argument);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListRangeOperationTest.apply range works for full range.
TEST(MemEdgeListRangeOperationTest, ApplyRangeWorksForFullRange) {
  std::string test_file = "test_edges_full_range.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  size_t count = 0;
  edge_list.apply_range(
      [&](v_id_t src, v_id_t dst) {
        (void) src;
        (void) dst;
        count++;
      },
      0, edge_list.edge_count());

  EXPECT_EQ(count, 4);

  // Clean up
  std::remove(test_file.c_str());
}

// mem_edge_list_t Statistics Tests
TEST(MemEdgeListStatisticsTest, GetStatisticsReturnsCorrectInformation) {
  std::string test_file = "test_edges_statistics.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 0}};
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  std::string stats = edge_list.get_statistics();

  EXPECT_NE(stats.find("test_graph"), std::string::npos);
  EXPECT_NE(stats.find("5"), std::string::npos); // Number of edges

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListStatisticsTest.get statistics for empty list.
TEST(MemEdgeListStatisticsTest, GetStatisticsForEmptyList) {
  mem_edge_list_t edge_list;

  std::string stats = edge_list.get_statistics();

  EXPECT_NE(stats.find("0"), std::string::npos); // Number of edges
}

// mem_edge_list_t Edge Count Tests
TEST(MemEdgeListEdgeCountTest, EdgeCountReturnsZeroForEmptyList) {
  mem_edge_list_t edge_list;

  EXPECT_EQ(edge_list.edge_count(), 0);
}

// Test MemEdgeListEdgeCountTest.edge count returns correct value after loading.
TEST(MemEdgeListEdgeCountTest, EdgeCountReturnsCorrectValueAfterLoading) {
  std::string test_file = "test_edges_edge_count.txt";
  std::vector<std::pair<v_id_t, v_id_t>> test_edges;
  for (int i = 0; i < 100; ++i) {
    test_edges.emplace_back(i, i + 1);
  }
  create_test_edge_file(test_file, test_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  EXPECT_EQ(edge_list.edge_count(), 100);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListEdgeCountTest.edge count updates after adding edges.
TEST(MemEdgeListEdgeCountTest, EdgeCountUpdatesAfterAddingEdges) {
  mem_edge_list_t edge_list;

  EXPECT_EQ(edge_list.edge_count(), 0);

  for (int i = 0; i < 50; ++i) {
    edge_list.add_edge(i, i + 1);
  }

  EXPECT_EQ(edge_list.edge_count(), 50);
}

// mem_edge_list_t Large File Tests
TEST(MemEdgeListLargeFileTest, LoadsLargeFileCorrectly) {
  std::string test_file = "test_edges_large.txt";
  size_t num_edges = 10000;
  create_large_test_edge_file(test_file, num_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  bool success = edge_list.load_from_file();

  EXPECT_TRUE(success);
  EXPECT_EQ(edge_list.edge_count(), num_edges);

  // Clean up
  std::remove(test_file.c_str());
}

// Test MemEdgeListLargeFileTest.parallel processing on large file.
TEST(MemEdgeListLargeFileTest, ParallelProcessingOnLargeFile) {
  std::string test_file = "test_edges_large_parallel.txt";
  size_t num_edges = 50000;
  create_large_test_edge_file(test_file, num_edges);

  mem_edge_list_t edge_list("test_graph", test_file);
  edge_list.load_from_file();

  std::atomic<size_t> count{0};

  edge_list.apply_parallel(
      [&](v_id_t src, v_id_t dst) {
        (void) src;
        (void) dst;
        count++;
      },
      8);

  EXPECT_EQ(count, num_edges);

  // Clean up
  std::remove(test_file.c_str());
}

// mem_edge_list_t Zipfian workload tests
TEST(MemEdgeListZipfianTest, CappedZipfianLimitsPerSourceCount) {
  mem_edge_list_t edge_list;
  for (v_id_t src = 0; src < 20; ++src) {
    for (v_id_t dst = 0; dst < 10; ++dst) {
      edge_list.add_edge(src, dst + 100);
    }
  }

  edge_list.to_zipfian(1.5, 42, 15);

  EXPECT_EQ(edge_list.edge_count(), 200);

  std::unordered_map<v_id_t, size_t> counts;
  for (const auto& edge : edge_list.get_edges()) {
    counts[edge.first]++;
  }

  size_t hottest_count = 0;
  for (const auto& [src, count] : counts) {
    (void) src;
    hottest_count = std::max(hottest_count, count);
    EXPECT_LE(count, 15);
  }
  EXPECT_EQ(hottest_count, 15);
}

TEST(MemEdgeListZipfianTest, ThrowsWhenCapCannotPreserveEdgeCount) {
  mem_edge_list_t edge_list;
  for (v_id_t src = 0; src < 4; ++src) {
    for (v_id_t dst = 0; dst < 5; ++dst) {
      edge_list.add_edge(src, dst + 100);
    }
  }

  EXPECT_THROW(edge_list.to_zipfian(0.8, 42, 4), std::invalid_argument);
}

// mem_edge_list_t Getter Tests
TEST(MemEdgeListGetterTest, GetGraphNameReturnsCorrectName) {
  mem_edge_list_t edge_list("my_graph");

  EXPECT_EQ(edge_list.get_graph_name(), "my_graph");
}

// Test MemEdgeListGetterTest.get file path returns correct path.
TEST(MemEdgeListGetterTest, GetFilePathReturnsCorrectPath) {
  mem_edge_list_t edge_list("my_graph");

  EXPECT_EQ(edge_list.get_file_path(), "data/my_graph.delta");
}

// Test MemEdgeListGetterTest.get file path returns custom path.
TEST(MemEdgeListGetterTest, GetFilePathReturnsCustomPath) {
  mem_edge_list_t edge_list("my_graph", "custom/path.txt");

  EXPECT_EQ(edge_list.get_file_path(), "custom/path.txt");
}

// Test MemEdgeListGetterTest.get edges returns empty vector for empty list.
TEST(MemEdgeListGetterTest, GetEdgesReturnsEmptyVectorForEmptyList) {
  mem_edge_list_t edge_list;

  const auto& edges = edge_list.get_edges();
  EXPECT_TRUE(edges.empty());
}

// Test MemEdgeListGetterTest.get edges returns correct edges.
TEST(MemEdgeListGetterTest, GetEdgesReturnsCorrectEdges) {
  mem_edge_list_t edge_list;

  edge_list.add_edge(10, 20);
  edge_list.add_edge(30, 40);

  const auto& edges = edge_list.get_edges();
  EXPECT_EQ(edges.size(), 2);
  EXPECT_EQ(edges[0].first, 10);
  EXPECT_EQ(edges[0].second, 20);
  EXPECT_EQ(edges[1].first, 30);
  EXPECT_EQ(edges[1].second, 40);
}
