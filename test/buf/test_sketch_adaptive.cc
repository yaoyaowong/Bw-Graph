#include "bw_graph/buf/sketch.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

// L1 adaptive consolidation threshold tests.
class SketchAdaptiveThresholdTest : public ::testing::Test {};

// Vertex with no samples must fall back to the supplied baseline so the
// behaviour is identical to the pre-adaptive code path for cold pages.
TEST_F(SketchAdaptiveThresholdTest, NoSamplesFallsBackToBaseline) {
  vertex_update_sketch_t sketch(64);
  uint64_t baseline = 4;
  EXPECT_EQ(sketch.get_consolidation_threshold(0, baseline), baseline);
  EXPECT_EQ(sketch.get_consolidation_threshold(10, 1), 1u);
}

// Vertex with too few samples (< MIN_SAMPLES_FOR_ADAPT) is still treated
// as cold so we keep predictable behaviour during the warm-up window.
TEST_F(SketchAdaptiveThresholdTest, FewSamplesFallsBackToBaseline) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 3; ++i) {
    sketch.record_read(5);
  }
  EXPECT_EQ(sketch.get_consolidation_threshold(5, 4), 4u);
}

// Pure-read vertex: the adaptive rule says fire at chain length 1 so the
// next reader does not have to pay the merge cost.
TEST_F(SketchAdaptiveThresholdTest, ReadHeavyFiresImmediately) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 100; ++i) {
    sketch.record_read(7);
  }
  EXPECT_EQ(sketch.get_consolidation_threshold(7, 4), 1u);
}

// Mostly-read vertex (write_ratio in [LV1, LV2) ~ 10..25%): uses the user
// baseline unchanged (factor == 1.0).
TEST_F(SketchAdaptiveThresholdTest, MildlyReadHeavyUsesBaseline) {
  vertex_update_sketch_t sketch(64);
  // 90 reads + 10 writes => ratio = 0.1, factor = LV1 = 1.0.
  for (int i = 0; i < 90; ++i) {
    sketch.record_read(3);
  }
  for (int i = 0; i < 10; ++i) {
    sketch.record_write(3);
  }
  EXPECT_EQ(sketch.get_consolidation_threshold(3, 4), 4u);
}

// Balanced workload (~25..50% writes): factor 1.5.
TEST_F(SketchAdaptiveThresholdTest, BalancedWorkloadScalesBy1_5) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 70; ++i) {
    sketch.record_read(2);
  }
  for (int i = 0; i < 30; ++i) {
    sketch.record_write(2);
  }
  EXPECT_EQ(sketch.get_consolidation_threshold(2, 4), 6u);
}

// Write-leaning workload (~50..75% writes): factor 3.
TEST_F(SketchAdaptiveThresholdTest, WriteLeaningScalesBy3) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 40; ++i) {
    sketch.record_read(1);
  }
  for (int i = 0; i < 60; ++i) {
    sketch.record_write(1);
  }
  EXPECT_EQ(sketch.get_consolidation_threshold(1, 4), 12u);
}

// Very write-heavy workload (>=75% writes): factor 5 - we delay the merge.
TEST_F(SketchAdaptiveThresholdTest, WriteHeavyDelaysMerge) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 10; ++i) {
    sketch.record_read(0);
  }
  for (int i = 0; i < 90; ++i) {
    sketch.record_write(0);
  }
  EXPECT_EQ(sketch.get_consolidation_threshold(0, 4), 20u);
}

// Threshold is always >= 1 even when baseline == 0.
TEST_F(SketchAdaptiveThresholdTest, ZeroBaselineNeverReturnsZero) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 100; ++i) {
    sketch.record_write(4);
  }
  EXPECT_GE(sketch.get_consolidation_threshold(4, 0), 1u);
}

// Out-of-range id must not crash and must return baseline.
TEST_F(SketchAdaptiveThresholdTest, OutOfRangeVertexReturnsBaseline) {
  vertex_update_sketch_t sketch(64);
  EXPECT_EQ(sketch.get_consolidation_threshold(1024, 7), 7u);
}

// L2 per-vertex slot reservation tests.
class SketchPerVertexReservationTest : public ::testing::Test {};

// Read-heavy vertex: reserve 0 slots (no wasted space).
TEST_F(SketchPerVertexReservationTest, ReadHeavyReservesZeroSlots) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 100; ++i) {
    sketch.record_read(7);
  }
  EXPECT_EQ(sketch.get_per_vertex_reservation(7), 0u);
}

// Write-heavy vertex: reserve maximum slots so future writes are in-place.
TEST_F(SketchPerVertexReservationTest, WriteHeavyReservesMaxSlots) {
  vertex_update_sketch_t sketch(64);
  for (int i = 0; i < 90; ++i) {
    sketch.record_write(9);
  }
  for (int i = 0; i < 10; ++i) {
    sketch.record_read(9);
  }
  EXPECT_EQ(sketch.get_per_vertex_reservation(9), 4u);
}

// Within the same sketch, two vertices with different workloads must get
// different reservations.  This is the critical property L2 needed for
// adaptive per-vertex slot allocation on the same CSR page.
TEST_F(SketchPerVertexReservationTest, DifferentVerticesGetDifferentSlots) {
  vertex_update_sketch_t sketch(64);
  // Vertex 1: 100% reads => 0 slots.
  for (int i = 0; i < 100; ++i) {
    sketch.record_read(1);
  }
  // Vertex 2: 90% writes => 4 slots.
  for (int i = 0; i < 10; ++i) {
    sketch.record_read(2);
  }
  for (int i = 0; i < 90; ++i) {
    sketch.record_write(2);
  }
  // Vertex 3: balanced ~ 30% writes => level 2.
  for (int i = 0; i < 70; ++i) {
    sketch.record_read(3);
  }
  for (int i = 0; i < 30; ++i) {
    sketch.record_write(3);
  }

  uint8_t s1 = sketch.get_per_vertex_reservation(1);
  uint8_t s2 = sketch.get_per_vertex_reservation(2);
  uint8_t s3 = sketch.get_per_vertex_reservation(3);

  EXPECT_LT(s1, s3);
  EXPECT_LT(s3, s2);
  EXPECT_EQ(s1, 0u);
  EXPECT_EQ(s2, 4u);
}

// Out-of-range id must not crash and must return 0.
TEST_F(SketchPerVertexReservationTest, OutOfRangeReturnsZero) {
  vertex_update_sketch_t sketch(64);
  EXPECT_EQ(sketch.get_per_vertex_reservation(1024), 0u);
}
