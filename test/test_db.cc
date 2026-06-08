#include "bw_graph/common/config.h"

#include <gtest/gtest.h>

TEST(StoragePrefixTest, ClassifiesPersistedFiles) {
  bw_graph::CSR_PREFIX = "/tmp/bwgraph/csr";
  bw_graph::DELTA_PREFIX = "/tmp/bwgraph/delta";
  bw_graph::META_PREFIX = "/tmp/bwgraph/meta";

  EXPECT_EQ(bw_graph::csr_db_path("example"), "/tmp/bwgraph/csr/example.db");
  EXPECT_EQ(bw_graph::delta_db_path("example"), "/tmp/bwgraph/delta/example_delta.db");
  EXPECT_EQ(bw_graph::vertex_index_path("example"),
            "/tmp/bwgraph/meta/example.vertex_index.bin");
  EXPECT_EQ(bw_graph::index_page_path("example"), "/tmp/bwgraph/meta/example.index_page.bin");
  EXPECT_EQ(bw_graph::partition_path("example"), "/tmp/bwgraph/meta/example.partition.bin");
  EXPECT_EQ(bw_graph::coarsening_path("example"), "/tmp/bwgraph/meta/example.coar.bin");
  EXPECT_EQ(bw_graph::giant_db_path("example"), "/tmp/bwgraph/meta/example_giantdb");
  EXPECT_EQ(bw_graph::property_db_path("example"), "/tmp/bwgraph/meta/example_propdb");
}
