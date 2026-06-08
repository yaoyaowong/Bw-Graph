#include "bw_graph/db/db.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

std::string make_test_graph() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::string graph_name = "read_neighbor_delta_" + std::to_string(suffix);
  std::filesystem::path test_root =
      std::filesystem::temp_directory_path() / graph_name;
  std::filesystem::create_directories(test_root / "data");
  std::filesystem::create_directories(test_root / "workspace");
  std::filesystem::current_path(test_root);

  std::ofstream out("data/" + graph_name + ".graph");
  out << "t 4 2\n";
  out << "v 0 0 0\n";
  out << "v 1 0 0\n";
  out << "v 2 0 0\n";
  out << "v 3 0 0\n";
  out << "e 0 1\n";
  out << "e 0 2\n";
  return graph_name;
}

std::vector<v_id_t> sorted(std::vector<v_id_t> values) {
  std::sort(values.begin(), values.end());
  return values;
}

} // namespace

TEST(ReadNeighborWithDeltaTest, SnapshotExposesDeltaHeadAndCloneAppliesNewestRecordFirst) {
  bw_graph_db_t db(make_test_graph(), true, true, false, false, false);

  auto [base_neighbors, empty_delta, base_page] = db.read_neighbor_with_delta(0);
  EXPECT_FALSE(empty_delta.is_valid());
  EXPECT_EQ(base_neighbors.size(), 2u);
  base_page->r_unlatch();

  ASSERT_TRUE(db.delete_edge(0, 3));
  ASSERT_TRUE(db.insert_edge(0, 3));

  auto [neighbors, delta_snapshot, csr_page] = db.read_neighbor_with_delta(0);
  EXPECT_TRUE(delta_snapshot.is_valid());
  EXPECT_EQ(delta_snapshot.page_no, db.vertex_index->delta_heads_[0].page_no);
  EXPECT_EQ(delta_snapshot.record_idx, db.vertex_index->delta_heads_[0].record_idx);
  EXPECT_EQ(neighbors.size(), 2u);
  csr_page->r_unlatch();

  EXPECT_EQ(sorted(db.read_neighbor_clone(0)), (std::vector<v_id_t>{1, 2, 3}));
}
