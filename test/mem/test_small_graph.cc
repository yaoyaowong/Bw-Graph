#include "bw_graph/mem/small_graph.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

bool snapshot_is_consistent(const small_graph_snapshot_t& snapshot) {
  size_t counted_edges = 0;

  for (size_t src = 0; src < snapshot.vertices.size(); ++src) {
    const auto& vertex = snapshot.vertices[src];

    if (!std::is_sorted(vertex.neighbors.begin(), vertex.neighbors.end())) {
      return false;
    }

    for (size_t i = 1; i < vertex.neighbors.size(); ++i) {
      if (vertex.neighbors[i - 1] == vertex.neighbors[i]) {
        return false;
      }
    }

    if (vertex.deleted) {
      if (!vertex.neighbors.empty()) {
        return false;
      }
      continue;
    }

    for (v_id_t dst : vertex.neighbors) {
      if (dst >= snapshot.vertices.size() || snapshot.vertices[dst].deleted) {
        return false;
      }
    }

    counted_edges += vertex.neighbors.size();
  }

  return snapshot.edge_count == counted_edges;
}

} // namespace

TEST(TmpSmallGraphTest, AllocatesContinuousIdsWithoutReuse) {
  tmp_small_graph_t graph;
  EXPECT_EQ(graph.insert_vertex(), 0);
  EXPECT_EQ(graph.insert_vertex(), 1);
  ASSERT_TRUE(graph.delete_vertex(0));
  EXPECT_EQ(graph.insert_vertex(), 2);
  EXPECT_EQ(graph.vertex_id_upper_bound(), 3);
  EXPECT_EQ(graph.live_vertex_count(), 2);
}

TEST(TmpSmallGraphTest, MaintainsDirectedEdgesAndDeletesIncidentEdges) {
  tmp_small_graph_t graph;
  v_id_t v0 = graph.insert_vertex();
  v_id_t v1 = graph.insert_vertex();
  v_id_t v2 = graph.insert_vertex();
  EXPECT_TRUE(graph.has_vertex(v0));
  EXPECT_TRUE(graph.has_vertex(v1));
  EXPECT_TRUE(graph.has_vertex(v2));
  EXPECT_EQ(graph.edge_count(), 0u);
  EXPECT_TRUE(graph.insert_edge(v0, v1));
  EXPECT_EQ(graph.edge_count(), 1u);
  EXPECT_TRUE(graph.insert_edge(v2, v0));
  EXPECT_EQ(graph.edge_count(), 2u);
  EXPECT_FALSE(graph.insert_edge(v0, v1));
  EXPECT_FALSE(graph.insert_edge(v0, 99));
  EXPECT_TRUE(graph.delete_vertex(v0));
  EXPECT_FALSE(graph.has_vertex(v0));
  EXPECT_TRUE(graph.has_vertex(v1));
  EXPECT_TRUE(graph.has_vertex(v2));
  EXPECT_EQ(graph.edge_count(), 0u);
  EXPECT_FALSE(graph.has_edge(v2, v0));
  EXPECT_FALSE(graph.has_edge(v0, v1));
  EXPECT_TRUE(graph.get_neighbors(v2).empty());
}

TEST(TmpSmallGraphTest, RejectsMissingEndpointsAndDuplicateEdges) {
  tmp_small_graph_t graph;
  v_id_t v0 = graph.insert_vertex();
  v_id_t v1 = graph.insert_vertex();

  EXPECT_FALSE(graph.delete_edge(v0, v1));
  EXPECT_FALSE(graph.delete_vertex(99));
  EXPECT_TRUE(graph.insert_edge(v0, v1));
  EXPECT_FALSE(graph.insert_edge(v0, v1));
  EXPECT_FALSE(graph.insert_edge(v0, 99));
  EXPECT_FALSE(graph.insert_edge(99, v1));
  ASSERT_TRUE(graph.delete_vertex(v1));
  EXPECT_FALSE(graph.delete_vertex(v1));
  EXPECT_FALSE(graph.insert_edge(v0, v1));
  EXPECT_FALSE(graph.has_edge(v0, v1));
}

TEST(TmpSmallGraphTest, SnapshotIsDeterministicAndTracksMemoryAccounting) {
  tmp_small_graph_t graph;
  v_id_t v0 = graph.insert_vertex();
  v_id_t v1 = graph.insert_vertex();
  v_id_t v2 = graph.insert_vertex();

  size_t vertex_only_usage = graph.memory_usage();
  EXPECT_GT(vertex_only_usage, 0u);

  EXPECT_TRUE(graph.insert_edge(v0, v2));
  EXPECT_TRUE(graph.insert_edge(v0, v1));

  size_t with_edges_usage = graph.memory_usage();
  EXPECT_GT(with_edges_usage, vertex_only_usage);

  EXPECT_EQ(graph.get_neighbors(v0), std::vector<v_id_t>({v1, v2}));

  small_graph_snapshot_t snapshot = graph.snapshot();
  EXPECT_EQ(snapshot.vertices.size(), graph.vertex_id_upper_bound());
  EXPECT_FALSE(snapshot.vertices[v0].deleted);
  EXPECT_EQ(snapshot.vertices[v0].neighbors, std::vector<v_id_t>({v1, v2}));
  EXPECT_EQ(snapshot.edge_count, 2u);
  EXPECT_EQ(snapshot.memory_usage, graph.memory_usage());

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  ASSERT_TRUE(snapshot.vertices[v0].edge_commit_ts.empty());
  ASSERT_TRUE(snapshot.vertices[v1].edge_commit_ts.empty());
  ASSERT_TRUE(snapshot.vertices[v2].edge_commit_ts.empty());
#endif // BW_GRAPH_ENABLE_TRANSACTION

  EXPECT_TRUE(graph.delete_edge(v0, v1));
  EXPECT_LT(graph.memory_usage(), with_edges_usage);
  EXPECT_EQ(graph.edge_count(), 1u);

  size_t after_edge_delete_usage = graph.memory_usage();
  EXPECT_TRUE(graph.delete_vertex(v1));
  EXPECT_EQ(graph.memory_usage(), after_edge_delete_usage);
  EXPECT_EQ(graph.edge_count(), 1u);

  small_graph_snapshot_t after_delete = graph.snapshot();
  EXPECT_TRUE(after_delete.vertices[v1].deleted);
  EXPECT_TRUE(after_delete.vertices[v1].neighbors.empty());
  EXPECT_EQ(after_delete.memory_usage, graph.memory_usage());
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  ASSERT_TRUE(after_delete.vertices[v0].edge_commit_ts.empty());
  ASSERT_TRUE(after_delete.vertices[v1].edge_commit_ts.empty());
  ASSERT_TRUE(after_delete.vertices[v2].edge_commit_ts.empty());
#endif // BW_GRAPH_ENABLE_TRANSACTION
  EXPECT_TRUE(snapshot_is_consistent(after_delete));
}

TEST(TmpSmallGraphTest, ConcurrentReadersAndWritersCompleteWithoutInvalidObservations) {
  tmp_small_graph_t graph;
  for (int i = 0; i < 16; ++i) {
    graph.insert_vertex();
  }

  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  std::atomic<bool> observed_invalid{false};

  auto writer = std::thread([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (int i = 0; i < 5000; ++i) {
      v_id_t src = static_cast<v_id_t>(i % 16);
      v_id_t dst = static_cast<v_id_t>((i + 1) % 16);
      if ((i % 2) == 0) {
        graph.insert_edge(src, dst);
      } else {
        graph.delete_edge(src, dst);
      }
      if ((i % 32) == 0) {
        std::this_thread::yield();
      }
    }

    done.store(true, std::memory_order_release);
  });

  auto reader_fn = [&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    while (!done.load(std::memory_order_acquire)) {
      small_graph_snapshot_t snapshot = graph.snapshot();
      size_t counted_edges = 0;
      bool valid = true;

      for (size_t src = 0; src < snapshot.vertices.size(); ++src) {
        const auto& vertex = snapshot.vertices[src];
        if (!std::is_sorted(vertex.neighbors.begin(), vertex.neighbors.end())) {
          valid = false;
          break;
        }
        for (size_t i = 1; i < vertex.neighbors.size(); ++i) {
          if (vertex.neighbors[i - 1] == vertex.neighbors[i]) {
            valid = false;
            break;
          }
        }
        if (!valid) {
          break;
        }
        if (vertex.deleted && !vertex.neighbors.empty()) {
          valid = false;
          break;
        }
        for (v_id_t dst : vertex.neighbors) {
          if (dst >= snapshot.vertices.size() || snapshot.vertices[dst].deleted) {
            valid = false;
            break;
          }
        }
        if (!valid) {
          break;
        }
        counted_edges += vertex.neighbors.size();
      }

      if (!valid || counted_edges != snapshot.edge_count) {
        observed_invalid.store(true, std::memory_order_release);
        break;
      }
      std::this_thread::yield();
    }

    small_graph_snapshot_t final_snapshot = graph.snapshot();
    if (!snapshot_is_consistent(final_snapshot)) {
      observed_invalid.store(true, std::memory_order_release);
    }
  };

  std::thread reader_a(reader_fn);
  std::thread reader_b(reader_fn);

  start.store(true, std::memory_order_release);

  writer.join();
  reader_a.join();
  reader_b.join();

  EXPECT_FALSE(observed_invalid.load(std::memory_order_acquire));
  EXPECT_TRUE(snapshot_is_consistent(graph.snapshot()));
}
