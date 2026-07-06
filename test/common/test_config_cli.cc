#include "algo_cli.h"
#include "bw_graph/common/config.h"

#include <gtest/gtest.h>
#include <string>

namespace {

struct algorithm_config_guard_t {
  std::string bfs = bw_graph::BW_GRAPH_BFS_ALGO;
  std::string wcc = bw_graph::BW_GRAPH_WCC_ALGO;
  std::string pagerank = bw_graph::BW_GRAPH_PAGERANK_ALGO;
  std::string cdlp = bw_graph::BW_GRAPH_CDLP_ALGO;
  std::string cdlp_variant = bw_graph::BW_GRAPH_CDLP_VARIANT;
  size_t page_size = bw_graph::BW_GRAPH_PAGE_SIZE;
  uint64_t buffer_chunk_count = bw_graph::BW_BUFFER_CHUNK_COUNT;
  uint64_t buffer_chunk_size = bw_graph::BW_BUFFER_CHUNK_SIZE;

  ~algorithm_config_guard_t() {
    bw_graph::BW_GRAPH_BFS_ALGO = bfs;
    bw_graph::BW_GRAPH_WCC_ALGO = wcc;
    bw_graph::BW_GRAPH_PAGERANK_ALGO = pagerank;
    bw_graph::BW_GRAPH_CDLP_ALGO = cdlp;
    bw_graph::BW_GRAPH_CDLP_VARIANT = cdlp_variant;
    bw_graph::BW_GRAPH_PAGE_SIZE = page_size;
    bw_graph::BW_BUFFER_CHUNK_COUNT = buffer_chunk_count;
    bw_graph::BW_BUFFER_CHUNK_SIZE = buffer_chunk_size;
  }
};

} // namespace

TEST(ConfigCliTest, AcceptsSpaceSeparatedAlgorithmOverridesAndVotingAlias) {
  algorithm_config_guard_t guard;
  const char* args[] = {"dynamic_algorithms", "config/rn06.yaml", "--bfs-algo", "scan",
                        "--wcc-algo", "map", "--pagerank-algo", "scan", "--cdlp-algo",
                        "voting"};

  apply_cli_overrides(static_cast<int>(std::size(args)), const_cast<char**>(args));

  EXPECT_EQ(bw_graph::BW_GRAPH_BFS_ALGO, "scan");
  EXPECT_EQ(bw_graph::BW_GRAPH_WCC_ALGO, "map");
  EXPECT_EQ(bw_graph::BW_GRAPH_PAGERANK_ALGO, "scan");
  EXPECT_EQ(bw_graph::BW_GRAPH_CDLP_ALGO, "map");
  EXPECT_EQ(bw_graph::BW_GRAPH_CDLP_VARIANT, "voting");
}

TEST(ConfigCliTest, RetainsEqualsSeparatedOverrides) {
  algorithm_config_guard_t guard;
  const char* args[] = {"dynamic_algorithms", "config/rn06.yaml", "--bfs-algo=map",
                        "--cdlp-variant=original"};

  apply_cli_overrides(static_cast<int>(std::size(args)), const_cast<char**>(args));

  EXPECT_EQ(bw_graph::BW_GRAPH_BFS_ALGO, "map");
  EXPECT_EQ(bw_graph::BW_GRAPH_CDLP_VARIANT, "original");
}

TEST(ConfigCliTest, AppliesMemoryBudgetFromMegabytes) {
  algorithm_config_guard_t guard;
  bw_graph::BW_GRAPH_PAGE_SIZE = 64 * 1024;

  apply_buffer_pool_memory_budget_mb(10);

  EXPECT_EQ(bw_graph::BW_BUFFER_CHUNK_COUNT, 16);
  EXPECT_EQ(bw_graph::BW_BUFFER_CHUNK_SIZE, 10);
}

TEST(ConfigCliTest, KeepsConfiguredBufferWhenMemoryBudgetIsAbsent) {
  algorithm_config_guard_t guard;
  bw_graph::BW_BUFFER_CHUNK_COUNT = 8;
  bw_graph::BW_BUFFER_CHUNK_SIZE = 4;
  const char* args[] = {"bfs", "config/rn.yaml", "--algo", "scan"};

  apply_memory_cli_override(static_cast<int>(std::size(args)), const_cast<char**>(args));

  EXPECT_EQ(bw_graph::BW_BUFFER_CHUNK_COUNT, 8);
  EXPECT_EQ(bw_graph::BW_BUFFER_CHUNK_SIZE, 4);
}

TEST(ConfigCliTest, AppliesMemoryBudgetFromCliOverride) {
  algorithm_config_guard_t guard;
  bw_graph::BW_GRAPH_PAGE_SIZE = 64 * 1024;
  const char* args[] = {"bfs", "config/rn.yaml", "--mem", "10"};

  apply_memory_cli_override(static_cast<int>(std::size(args)), const_cast<char**>(args));

  EXPECT_EQ(bw_graph::BW_BUFFER_CHUNK_COUNT, 16);
  EXPECT_EQ(bw_graph::BW_BUFFER_CHUNK_SIZE, 10);
}
