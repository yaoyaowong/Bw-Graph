#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/mem/mem_g0com.h"

#include <algorithm>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <indicators/progress_bar.hpp>
#include <queue>

/**
 * @brief Reorder vertices with a GOrder-style sliding window heuristic.
 * @param adj_map      Adjacency map for vertices to reorder
 * @param window_size  Number of recent vertices used for locality scoring
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> gorder_reordering(const adj_map_t& adj_map, size_t window_size = 20) {
  const size_t n = adj_map.size();
  if (n == 0) {
    return {};
  }

  // Track giant vertices to apply a small score penalty.
  folly::F14FastSet<v_id_t> giant_set;
  for (const auto& [u, neighbors] : adj_map) {
    if (neighbors.size() >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      giant_set.insert(u);
    }
  }

  // Use neighbor count as the internal degree for this adjacency map.
  folly::F14FastMap<v_id_t, size_t> internal_degree;
  internal_degree.reserve(n);

  for (const auto& [u, neighbors] : adj_map) {
    internal_degree[u] = neighbors.size();
  }

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  folly::F14FastSet<v_id_t> visited;
  visited.reserve(n);

  // Maintain a sliding window and candidate neighbor counts.
  std::deque<v_id_t> window_queue;
  folly::F14FastSet<v_id_t> window_set;
  window_set.reserve(window_size);

  folly::F14FastMap<v_id_t, size_t> window_neighbor_count;
  window_neighbor_count.reserve(n);

  for (const auto& [u, _] : adj_map) {
    window_neighbor_count[u] = 0;
  }

  // Start from the highest-degree vertex.
  v_id_t seed = 0;
  size_t max_deg = 0;
  for (const auto& [u, deg] : internal_degree) {
    if (deg > max_deg) {
      max_deg = deg;
      seed = u;
    }
  }

  if (seed == 0) {
    seed = adj_map.begin()->first;
  }

  ordered.push_back(seed);
  visited.insert(seed);
  window_queue.push_back(seed);
  window_set.insert(seed);

  // Seed the window-neighbor counts.
  for (v_id_t ngh : adj_map.at(seed)) {
    if (!visited.contains(ngh)) {
      window_neighbor_count[ngh]++;
    }
  }

  while (visited.size() < n) {
    // Candidate vertices are unvisited neighbors of the current window.
    folly::F14FastSet<v_id_t> candidates;
    candidates.reserve(window_size * 50); // Rough estimate

    for (const v_id_t w : window_queue) {
      for (const v_id_t v : adj_map.at(w)) {
        if (!visited.contains(v)) {
          candidates.insert(v);
        }
      }
    }

    // Pick the highest-scoring candidate by locality and degree.
    v_id_t next_vertex = 0;
    double max_score = -1.0;

    for (const v_id_t v : candidates) {
      double score = static_cast<double>(window_neighbor_count[v]);
      score += 0.1 * internal_degree[v];

      if (giant_set.contains(v)) {
        score *= 0.8;
      }

      if (score > max_score) {
        max_score = score;
        next_vertex = v;
      }
    }

    // Start a new component when the window has no reachable candidates.
    if (next_vertex == 0) {
      for (const auto& [u, _] : adj_map) {
        if (!visited.contains(u)) {
          next_vertex = u;
          break;
        }
      }

      window_queue.clear();
      window_set.clear();

      for (auto& [u, count] : window_neighbor_count) {
        count = 0;
      }
    }

    ordered.push_back(next_vertex);
    visited.insert(next_vertex);

    // Add the selected vertex to the sliding window.
    window_queue.push_back(next_vertex);
    window_set.insert(next_vertex);

    // Increment counts for unvisited neighbors of the selected vertex.
    for (v_id_t ngh : adj_map.at(next_vertex)) {
      if (!visited.contains(ngh)) {
        window_neighbor_count[ngh]++;
      }
    }

    // Remove the oldest window vertex and undo its count contribution.
    if (window_queue.size() > window_size) {
      v_id_t removed = window_queue.front();
      window_queue.pop_front();
      window_set.erase(removed);

      for (v_id_t ngh : adj_map.at(removed)) {
        if (!visited.contains(ngh) && window_neighbor_count.contains(ngh)) {
          window_neighbor_count[ngh]--;
        }
      }
    }
  }

  return ordered;
}

/**
 * @brief Reorder adjacency-map vertices with an optimized GOrder heuristic.
 * @param adj_map      Adjacency map for vertices to reorder
 * @param window_size  Number of recent vertices used for locality scoring
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> gorder_reordering_optimized(const adj_map_t& adj_map,
                                                       size_t window_size = 20) {
  const size_t n = adj_map.size();
  if (n == 0) {
    return {};
  }

  constexpr v_id_t INVALID_VERTEX = std::numeric_limits<v_id_t>::max();

  auto get_neighbors = [&adj_map](v_id_t v) -> const std::vector<v_id_t>& {
    static const std::vector<v_id_t> empty_vec;
    auto it = adj_map.find(v);
    if (it != adj_map.end()) {
      return it->second;
    }
    return empty_vec;
  };

  folly::F14FastSet<v_id_t> giant_set;
  giant_set.reserve(n / 100);

  for (const auto& [u, neighbors] : adj_map) {
    if (neighbors.size() >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      giant_set.insert(u);
    }
  }

  folly::F14FastMap<v_id_t, float> base_score;
  base_score.reserve(n);

  for (const auto& [u, neighbors] : adj_map) {
    float score = 0.1f * neighbors.size();
    if (giant_set.contains(u)) {
      score *= 0.8f;
    }
    base_score[u] = score;
  }

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  folly::F14FastSet<v_id_t> visited;
  visited.reserve(n);

  std::deque<v_id_t> window_queue;
  folly::F14FastSet<v_id_t> window_set;
  window_set.reserve(window_size);

  folly::F14FastMap<v_id_t, uint8_t> window_neighbor_count;
  window_neighbor_count.reserve(n);

  folly::F14FastSet<v_id_t> active_candidates;
  active_candidates.reserve(window_size * 50);

  v_id_t seed = INVALID_VERTEX;
  size_t max_deg = 0;
  for (const auto& [u, neighbors] : adj_map) {
    if (neighbors.size() > max_deg) {
      max_deg = neighbors.size();
      seed = u;
    }
  }

  if (seed == INVALID_VERTEX && !adj_map.empty()) {
    seed = adj_map.begin()->first;
  }

  if (seed == INVALID_VERTEX) {
    return {};
  }

  ordered.push_back(seed);
  visited.insert(seed);
  window_queue.push_back(seed);
  window_set.insert(seed);

  const std::vector<v_id_t>& seed_neighbors = get_neighbors(seed);
  for (v_id_t ngh : seed_neighbors) {
    // Track only internal vertices; skip cross-partition edges.
    if (!adj_map.contains(ngh) || visited.contains(ngh)) {
      continue;
    }
    window_neighbor_count[ngh] = 1;
    active_candidates.insert(ngh);
  }

  while (visited.size() < n) {
    v_id_t next_vertex = INVALID_VERTEX;

    if (!active_candidates.empty()) {
      constexpr size_t MAX_BUCKET = 21;
      std::array<std::vector<v_id_t>, MAX_BUCKET> buckets;

      for (const v_id_t v : active_candidates) {
        auto it = window_neighbor_count.find(v);
        uint8_t win_count = (it != window_neighbor_count.end()) ? it->second : 0;
        size_t bucket_idx = std::min<size_t>(win_count, MAX_BUCKET - 1);
        buckets[bucket_idx].push_back(v);
      }

      for (int i = MAX_BUCKET - 1; i >= 0; --i) {
        if (!buckets[i].empty()) {
          constexpr size_t SAMPLE_SIZE = 8;
          size_t sample_count = std::min<size_t>(SAMPLE_SIZE, buckets[i].size());

          float max_score = -1.0f;
          for (size_t j = 0; j < sample_count; ++j) {
            v_id_t candidate = buckets[i][buckets[i].size() - 1 - j];

            auto win_it = window_neighbor_count.find(candidate);
            auto base_it = base_score.find(candidate);

            float win_contrib =
                (win_it != window_neighbor_count.end()) ? static_cast<float>(win_it->second) : 0.0f;
            float base_contrib = (base_it != base_score.end()) ? base_it->second : 0.0f;

            float score = win_contrib + base_contrib;

            if (score > max_score) {
              max_score = score;
              next_vertex = candidate;
            }
          }
          break;
        }
      }
    }

    // Start a new component when there are no active candidates.
    if (next_vertex == INVALID_VERTEX) {
      for (const auto& [u, _] : adj_map) {
        if (!visited.contains(u)) {
          next_vertex = u;
          break;
        }
      }

      window_queue.clear();
      window_set.clear();
      active_candidates.clear();
      window_neighbor_count.clear();
    }

    if (next_vertex == INVALID_VERTEX) {
      std::cerr << "Warning: Cannot find next vertex, visited " << visited.size() << " out of " << n
                << " vertices" << std::endl;
      break;
    }

    ordered.push_back(next_vertex);
    visited.insert(next_vertex);
    active_candidates.erase(next_vertex);

    window_queue.push_back(next_vertex);
    window_set.insert(next_vertex);

    const std::vector<v_id_t>& next_neighbors = get_neighbors(next_vertex);
    for (v_id_t ngh : next_neighbors) {
      // Track only internal vertices; cross-partition neighbors do not need ordering.
      if (!adj_map.contains(ngh) || visited.contains(ngh)) {
        continue;
      }
      auto it = window_neighbor_count.find(ngh);
      if (it != window_neighbor_count.end()) {
        it->second++;
      } else {
        window_neighbor_count[ngh] = 1;
        active_candidates.insert(ngh);
      }
    }

    // Slide the window and remove stale candidate counts.
    if (window_queue.size() > window_size) {
      v_id_t removed = window_queue.front();
      window_queue.pop_front();
      window_set.erase(removed);

      const std::vector<v_id_t>& removed_neighbors = get_neighbors(removed);
      for (v_id_t ngh : removed_neighbors) {
        // Only internal vertices were inserted into window_neighbor_count.
        if (!adj_map.contains(ngh) || visited.contains(ngh)) {
          continue;
        }
        auto it = window_neighbor_count.find(ngh);
        if (it != window_neighbor_count.end()) {
          if (it->second == 1) {
            window_neighbor_count.erase(it);
            active_candidates.erase(ngh);
          } else {
            it->second--;
          }
        }
      }
    }
  }

  return ordered;
}

/**
 * @brief Reorder compressed-memory graph vertices with an optimized GOrder heuristic.
 * @param graph        Graph to reorder
 * @param window_size  Number of recent vertices used for locality scoring
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> gorder_reordering_optimized(mem_sub_com_t& graph,
                                                       size_t window_size = 20) {
  const size_t n = graph.vertex_count();
  if (n == 0) {
    return {};
  }

  constexpr v_id_t INVALID_VERTEX = std::numeric_limits<v_id_t>::max();

  // Precompute giant status and static degree-based scores.
  folly::F14FastSet<v_id_t> giant_set;
  giant_set.reserve(n / 100);

  std::vector<float> base_score(n, 0.0f);

  for (v_id_t u = 0; u < n; ++u) {
    neighbor_span_t neighbors = graph.get_neighbors_span(u);
    size_t degree = neighbors.size();

    if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      giant_set.insert(u);
    }

    float score = 0.1f * degree;
    if (giant_set.contains(u)) {
      score *= 0.8f;
    }
    base_score[u] = score;
  }

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  std::vector<bool> visited(n, false);

  std::deque<v_id_t> window_queue;
  folly::F14FastSet<v_id_t> window_set;
  window_set.reserve(window_size);

  std::vector<uint8_t> window_neighbor_count(n, 0);

  folly::F14FastSet<v_id_t> active_candidates;
  active_candidates.reserve(window_size * 50);

  // Start from the highest-degree vertex.
  v_id_t seed = INVALID_VERTEX;
  size_t max_deg = 0;
  for (v_id_t u = 0; u < n; ++u) {
    size_t degree = graph.get_neighbors_span(u).size();
    if (degree > max_deg) {
      max_deg = degree;
      seed = u;
    }
  }

  if (seed == INVALID_VERTEX) {
    return {};
  }

  ordered.push_back(seed);
  visited[seed] = true;
  window_queue.push_back(seed);
  window_set.insert(seed);

  neighbor_span_t seed_neighbors = graph.get_neighbors_span(seed);
  for (v_id_t ngh : seed_neighbors) {
    if (!visited[ngh]) {
      window_neighbor_count[ngh] = 1;
      active_candidates.insert(ngh);
    }
  }

  while (ordered.size() < n) {
    v_id_t next_vertex = INVALID_VERTEX;

    if (!active_candidates.empty()) {
      constexpr size_t MAX_BUCKET = 21;
      std::array<std::vector<v_id_t>, MAX_BUCKET> buckets;

      for (const v_id_t v : active_candidates) {
        uint8_t win_count = window_neighbor_count[v];
        size_t bucket_idx = std::min<size_t>(win_count, MAX_BUCKET - 1);
        buckets[bucket_idx].push_back(v);
      }

      for (int i = MAX_BUCKET - 1; i >= 0; --i) {
        if (!buckets[i].empty()) {
          constexpr size_t SAMPLE_SIZE = 8;
          size_t sample_count = std::min<size_t>(SAMPLE_SIZE, buckets[i].size());

          float max_score = -1.0f;
          for (size_t j = 0; j < sample_count; ++j) {
            v_id_t candidate = buckets[i][buckets[i].size() - 1 - j];

            float win_contrib = static_cast<float>(window_neighbor_count[candidate]);
            float base_contrib = base_score[candidate];
            float score = win_contrib + base_contrib;

            if (score > max_score) {
              max_score = score;
              next_vertex = candidate;
            }
          }
          break;
        }
      }
    }

    // Start a new component when there are no active candidates.
    if (next_vertex == INVALID_VERTEX) {
      for (v_id_t u = 0; u < n; ++u) {
        if (!visited[u]) {
          next_vertex = u;
          break;
        }
      }

      window_queue.clear();
      window_set.clear();
      active_candidates.clear();
      std::fill(window_neighbor_count.begin(), window_neighbor_count.end(), 0);
    }

    if (next_vertex == INVALID_VERTEX) {
      std::cerr << "Warning: Cannot find next vertex, visited " << ordered.size() << " out of " << n
                << " vertices" << std::endl;
      break;
    }

    // Add the selected vertex and update window candidate counts.
    ordered.push_back(next_vertex);
    visited[next_vertex] = true;
    active_candidates.erase(next_vertex);

    window_queue.push_back(next_vertex);
    window_set.insert(next_vertex);

    neighbor_span_t next_neighbors = graph.get_neighbors_span(next_vertex);
    for (v_id_t ngh : next_neighbors) {
      if (!visited[ngh]) {
        if (window_neighbor_count[ngh] == 0) {
          active_candidates.insert(ngh);
        }
        window_neighbor_count[ngh]++;
      }
    }

    // Slide the window and remove stale candidate counts.
    if (window_queue.size() > window_size) {
      v_id_t removed = window_queue.front();
      window_queue.pop_front();
      window_set.erase(removed);

      neighbor_span_t removed_neighbors = graph.get_neighbors_span(removed);
      for (v_id_t ngh : removed_neighbors) {
        if (!visited[ngh]) {
          window_neighbor_count[ngh]--;
          if (window_neighbor_count[ngh] == 0) {
            active_candidates.erase(ngh);
          }
        }
      }
    }
  }

  return ordered;
}

/**
 * @brief Reorder compressed-memory graph vertices with BFS traversal.
 * @param graph         Graph to reorder
 * @param start_vertex  Starting vertex for the first BFS component
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> bfs_reordering_for_csr(mem_sub_com_t& graph, v_id_t start_vertex) {
  const size_t n = graph.vertex_count();
  if (n == 0) {
    return {};
  }

  // Reject invalid start vertices before allocating traversal state.
  if (!graph.is_valid_vertex(start_vertex)) {
    std::cerr << "Invalid start vertex: " << start_vertex << std::endl;
    return {};
  }

  indicators::ProgressBar bfs_bar{indicators::option::BarWidth{50},
                                  indicators::option::Start{"["},
                                  indicators::option::Fill{"="},
                                  indicators::option::Lead{">"},
                                  indicators::option::Remainder{"-"},
                                  indicators::option::End{"]"},
                                  indicators::option::PostfixText{"BFS Reordering"},
                                  indicators::option::ForegroundColor{indicators::Color::cyan},
                                  indicators::option::ShowElapsedTime{true},
                                  indicators::option::ShowRemainingTime{true},
                                  indicators::option::MaxProgress{n}};

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  std::vector<bool> visited(n, false);
  std::deque<v_id_t> queue;

  const size_t progress_interval = (n < 10000) ? 1 : (n / 10000);

  // Traverse one connected component from the given seed.
  auto bfs_from = [&](v_id_t seed) {
    queue.push_back(seed);
    visited[seed] = true;

    while (!queue.empty()) {
      v_id_t current = queue.front();
      queue.pop_front();
      ordered.push_back(current);

      if (ordered.size() % progress_interval == 0 || ordered.size() == n) {
        bfs_bar.set_progress(ordered.size());
      }

      neighbor_span_t neighbors = graph.get_neighbors_span(current);
      for (v_id_t neighbor : neighbors) {
        if (!visited[neighbor]) {
          visited[neighbor] = true;
          queue.push_back(neighbor);
        }
      }
    }
  };

  bfs_from(start_vertex);

  // Continue from unvisited vertices to cover disconnected components.
  for (v_id_t v = 0; v < n; ++v) {
    if (!visited[v]) {
      bfs_from(v);
    }
  }

  bfs_bar.set_progress(n);
  bfs_bar.mark_as_completed();

  return ordered;
}

/**
 * @brief Reorder vertices by internal degree.
 * @param adj_map  Adjacency map for vertices to reorder
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> internal_degree_reordering(const adj_map_iter_t& adj_map) {
  const size_t n = adj_map.size();
  if (n == 0) {
    return {};
  }

  // Build a vertex set so cross-partition neighbors can be ignored.
  folly::F14FastSet<v_id_t> vertex_set;
  vertex_set.reserve(n);
  for (const auto& [u, _] : adj_map) {
    vertex_set.insert(u);
  }

  std::vector<std::pair<v_id_t, size_t>> degree_pairs;
  degree_pairs.reserve(n);

  for (const auto& [u, neighbors] : adj_map) {
    size_t internal_degree = 0;
    for (const v_id_t v : neighbors) {
      if (vertex_set.contains(v)) {
        internal_degree++;
      }
    }
    degree_pairs.emplace_back(u, internal_degree);
  }

  // Sort by internal degree descending, then vertex ID for determinism.
  std::sort(degree_pairs.begin(), degree_pairs.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second;
    }
    return a.first < b.first;
  });

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  for (const auto& [vid, _] : degree_pairs) {
    ordered.push_back(vid);
  }

  return ordered;
}

/**
 * @brief Reorder vertices by degree descending.
 * @param adj_map  Adjacency map for vertices to reorder
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> degree_reordering(const adj_map_t& adj_map) {
  const size_t n = adj_map.size();
  if (n == 0) {
    return {};
  }

  // Build vertex-degree pairs and sort by degree descending.
  std::vector<std::pair<v_id_t, size_t>> degree_pairs;
  degree_pairs.reserve(n);

  for (const auto& [u, neighbors] : adj_map) {
    degree_pairs.emplace_back(u, neighbors.size());
  }

  std::sort(degree_pairs.begin(), degree_pairs.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second;
    }
    return a.first < b.first;
  });

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  for (const auto& [vid, _] : degree_pairs) {
    ordered.push_back(vid);
  }

  return ordered;
}

/**
 * @brief Reorder adjacency-map vertices with BFS traversal.
 * @param adj_map      Adjacency map for vertices to reorder
 * @param seed_vertex  Optional starting vertex, auto-selected when 0
 * @return Ordered vertex IDs
 */
inline std::vector<v_id_t> bfs_reordering(const adj_map_t& adj_map, v_id_t seed_vertex = 0) {
  const size_t n = adj_map.size();
  if (n == 0) {
    return {};
  }

  // Identify giant vertices even though they still participate in BFS.
  folly::F14FastSet<v_id_t> giant_set;
  for (const auto& [u, neighbors] : adj_map) {
    if (neighbors.size() >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      giant_set.insert(u);
    }
  }

  // Select a valid seed, preferring the highest internal degree.
  v_id_t seed = seed_vertex;
  if (seed == 0 || !adj_map.contains(seed)) {
    folly::F14FastMap<v_id_t, size_t> internal_degree;
    for (const auto& [u, neighbors] : adj_map) {
      size_t int_deg = 0;
      for (const v_id_t v : neighbors) {
        if (adj_map.contains(v)) {
          ++int_deg;
        }
      }
      internal_degree[u] = int_deg;
    }

    if (!internal_degree.empty()) {
      seed = std::max_element(internal_degree.begin(), internal_degree.end(),
                              [](const auto& a, const auto& b) { return a.second < b.second; })
                 ->first;
    } else {
      seed = adj_map.begin()->first;
    }
  }

  std::vector<v_id_t> ordered;
  ordered.reserve(n);

  folly::F14FastSet<v_id_t> visited;
  visited.reserve(n);

  std::queue<v_id_t> bfs_queue;
  bfs_queue.push(seed);
  visited.insert(seed);

  while (!bfs_queue.empty() || visited.size() < n) {
    // Start a new BFS component when the queue is empty.
    if (bfs_queue.empty()) {
      for (const auto& [u, _] : adj_map) {
        if (!visited.contains(u)) {
          bfs_queue.push(u);
          visited.insert(u);
          break;
        }
      }
    }

    const v_id_t u = bfs_queue.front();
    bfs_queue.pop();
    ordered.push_back(u);

    const auto& neighbors = adj_map.at(u);

    // Add unvisited internal neighbors to the BFS queue.
    for (const v_id_t v : neighbors) {
      if (adj_map.contains(v) && !visited.contains(v)) {
        bfs_queue.push(v);
        visited.insert(v);
      }
    }
  }

  return ordered;
}
