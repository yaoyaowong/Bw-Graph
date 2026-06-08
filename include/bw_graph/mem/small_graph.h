#ifndef BW_GRAPH_MEM_SMALL_GRAPH_H
#define BW_GRAPH_MEM_SMALL_GRAPH_H

#include "bw_graph/common/type.h"

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <vector>

#include <folly/container/F14Set.h>

struct small_vertex_t {
  bool deleted{false};
  folly::F14FastSet<v_id_t> neighbors;
};

struct small_graph_vertex_snapshot_t {
  bool deleted{false};
  std::vector<v_id_t> neighbors;
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  std::vector<timestamp_t> edge_commit_ts;
#endif // BW_GRAPH_ENABLE_TRANSACTION
};

struct small_graph_snapshot_t {
  std::vector<small_graph_vertex_snapshot_t> vertices;
  uint64_t edge_count{0};
  uint64_t memory_usage{0};
};

class tmp_small_graph_t {
private:
  std::vector<small_vertex_t> vertices_;
  size_t live_vertex_count_{0};
  size_t edge_count_{0};
  size_t memory_usage_{0};
  mutable std::shared_mutex mutex_;

public:
  tmp_small_graph_t() = default;

  v_id_t insert_vertex();
  bool delete_vertex(v_id_t vertex_id);
  bool insert_edge(v_id_t src, v_id_t dst);
  bool delete_edge(v_id_t src, v_id_t dst);
  bool has_vertex(v_id_t vertex_id) const;
  bool has_edge(v_id_t src, v_id_t dst) const;
  std::vector<v_id_t> get_neighbors(v_id_t vertex_id) const;
  size_t vertex_id_upper_bound() const;
  size_t live_vertex_count() const;
  size_t edge_count() const;
  size_t memory_usage() const;
  small_graph_snapshot_t snapshot() const;
};

#endif // BW_GRAPH_MEM_SMALL_GRAPH_H
