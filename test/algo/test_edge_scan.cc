#include "bw_graph/algo/edge_scan.h"

#include <gtest/gtest.h>

TEST(EdgeScanPolicyTest, UsesPageLocalScanOnlyWhenPagesExceedResidentCapacity) {
  EXPECT_FALSE(bw_should_use_page_local_edge_scan(394, 4096));
  EXPECT_FALSE(bw_should_use_page_local_edge_scan(394, 394));
  EXPECT_TRUE(bw_should_use_page_local_edge_scan(394, 32));
}
