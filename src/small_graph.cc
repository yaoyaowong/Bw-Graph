#include "bw_graph/mem/small_graph.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace {

constexpr size_t vertex_logical_bytes = sizeof(small_vertex_t);
constexpr size_t edge_logical_bytes = sizeof(v_id_t) + sizeof(void*);

template <typename SetT>
std::vector<v_id_t> sorted_neighbors_from_set(const SetT& neighbors) {
  std::vector<v_id_t> result;
  result.reserve(neighbors.size());
  for (v_id_t neighbor : neighbors) {
    result.push_back(neighbor);
  }
  std::sort(result.begin(), result.end());
  return result;
}

} // namespace

v_id_t tmp_small_graph_t::insert_vertex() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  v_id_t vertex_id = static_cast<v_id_t>(vertices_.size());
  vertices_.emplace_back();
  ++live_vertex_count_;
  memory_usage_ += vertex_logical_bytes;
  return vertex_id;
}

bool tmp_small_graph_t::delete_vertex(v_id_t vertex_id) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (vertex_id >= vertices_.size()) {
    return false;
  }

  small_vertex_t& vertex = vertices_[vertex_id];
  if (vertex.deleted) {
    return false;
  }

  std::vector<v_id_t> outgoing(vertex.neighbors.begin(), vertex.neighbors.end());
  vertex.neighbors.clear();
  vertex.deleted = true;
  --live_vertex_count_;

  edge_count_ -= outgoing.size();
  memory_usage_ -= outgoing.size() * edge_logical_bytes;

  for (v_id_t src = 0; src < vertices_.size(); ++src) {
    if (src == vertex_id || vertices_[src].deleted) {
      continue;
    }
    auto& neighbors = vertices_[src].neighbors;
    if (neighbors.erase(vertex_id) != 0) {
      --edge_count_;
      memory_usage_ -= edge_logical_bytes;
    }
  }

  return true;
}

bool tmp_small_graph_t::insert_edge(v_id_t src, v_id_t dst) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (src >= vertices_.size() || dst >= vertices_.size()) {
    return false;
  }
  if (vertices_[src].deleted || vertices_[dst].deleted) {
    return false;
  }

  small_vertex_t& vertex = vertices_[src];
  auto insert_result = vertex.neighbors.insert(dst);
  if (!insert_result.second) {
    return false;
  }

  ++edge_count_;
  memory_usage_ += edge_logical_bytes;
  return true;
}

bool tmp_small_graph_t::delete_edge(v_id_t src, v_id_t dst) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (src >= vertices_.size() || dst >= vertices_.size()) {
    return false;
  }
  if (vertices_[src].deleted || vertices_[dst].deleted) {
    return false;
  }

  small_vertex_t& vertex = vertices_[src];
  if (vertex.neighbors.erase(dst) == 0) {
    return false;
  }

  --edge_count_;
  memory_usage_ -= edge_logical_bytes;
  return true;
}

bool tmp_small_graph_t::has_vertex(v_id_t vertex_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return vertex_id < vertices_.size() && !vertices_[vertex_id].deleted;
}

bool tmp_small_graph_t::has_edge(v_id_t src, v_id_t dst) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (src >= vertices_.size() || dst >= vertices_.size()) {
    return false;
  }
  const small_vertex_t& vertex = vertices_[src];
  if (vertex.deleted || vertices_[dst].deleted) {
    return false;
  }
  return vertex.neighbors.find(dst) != vertex.neighbors.end();
}

std::vector<v_id_t> tmp_small_graph_t::get_neighbors(v_id_t vertex_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (vertex_id >= vertices_.size() || vertices_[vertex_id].deleted) {
    return {};
  }
  return sorted_neighbors_from_set(vertices_[vertex_id].neighbors);
}

size_t tmp_small_graph_t::vertex_id_upper_bound() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return vertices_.size();
}

size_t tmp_small_graph_t::live_vertex_count() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return live_vertex_count_;
}

size_t tmp_small_graph_t::edge_count() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return edge_count_;
}

size_t tmp_small_graph_t::memory_usage() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return memory_usage_;
}

small_graph_snapshot_t tmp_small_graph_t::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  small_graph_snapshot_t snapshot;
  snapshot.vertices.reserve(vertices_.size());

  for (const auto& vertex : vertices_) {
    small_graph_vertex_snapshot_t vertex_snapshot;
    vertex_snapshot.deleted = vertex.deleted;
    vertex_snapshot.neighbors = sorted_neighbors_from_set(vertex.neighbors);
    snapshot.vertices.push_back(std::move(vertex_snapshot));
  }

  snapshot.edge_count = static_cast<uint64_t>(edge_count_);
  snapshot.memory_usage = static_cast<uint64_t>(memory_usage_);
  return snapshot;
}
