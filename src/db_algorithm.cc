#include "bw_graph/algo/edge_map.h"
#include "bw_graph/algo/edge_scan.h"
#include "bw_graph/algo/vertex_map.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/db.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <folly/ConcurrentBitSet.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <functional>
#include <iostream>
#include <kaminpar.h>
#include <math.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <random>
#include <span>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_vector.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for_each.h>
#include <tbb/spin_mutex.h>
#include <utility>
#include <vector>

namespace {

struct bfs_functor_t {
  std::atomic<v_id_t>* parent;

  // Update atomic.
  bool update_atomic(v_id_t src, v_id_t dst) {
    v_id_t expected = INVALID_VID;
    return parent[dst].compare_exchange_strong(expected, src, std::memory_order_acq_rel,
                                               std::memory_order_relaxed);
  }

  // Update update.
  bool update(v_id_t src, v_id_t dst) { return update_atomic(src, dst); }

  // Handle cond.
  bool cond(v_id_t v) { return parent[v].load(std::memory_order_relaxed) == INVALID_VID; }
};

// Run wcc find root.
inline v_id_t wcc_find_root(const std::atomic<v_id_t>* parent, v_id_t v) {
  while (true) {
    v_id_t p = parent[v].load(std::memory_order_relaxed);
    if (p == v) {
      return v;
    }
    v = p;
  }
}

struct wcc_hook_functor_t {
  std::atomic<v_id_t>* parent;
  std::atomic<bool>* changed;

  // Update update.
  bool update(v_id_t src, v_id_t dst) { return update_atomic(src, dst); }

  // Update atomic.
  bool update_atomic(v_id_t src, v_id_t dst) {
    v_id_t src_root = wcc_find_root(parent, src);
    v_id_t dst_root = wcc_find_root(parent, dst);
    if (src_root == dst_root) {
      return false;
    }

    v_id_t hi = std::max(src_root, dst_root);
    v_id_t lo = std::min(src_root, dst_root);
    v_id_t expected = hi;
    if (parent[hi].compare_exchange_strong(expected, lo, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
      changed->store(true, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  // Handle cond.
  bool cond(v_id_t) { return true; }
};

struct wcc_compress_functor_t {
  std::atomic<v_id_t>* parent;
  std::atomic<bool>* moved;

  // Handle operator.
  bool operator()(v_id_t v) {
    v_id_t p = parent[v].load(std::memory_order_relaxed);
    v_id_t gp = parent[p].load(std::memory_order_relaxed);
    if (p != gp) {
      parent[v].store(gp, std::memory_order_relaxed);
      moved->store(true, std::memory_order_relaxed);
    }
    return true;
  }
};

struct pagerank_edge_functor_t {
  std::atomic<double>* current;
  std::atomic<double>* next;
  const double* inverse_degree;

  // Update update.
  bool update(v_id_t src, v_id_t dst) {
    next[dst].fetch_add(current[src].load(std::memory_order_relaxed) * inverse_degree[src],
                        std::memory_order_relaxed);
    return true;
  }

  // Update atomic.
  bool update_atomic(v_id_t src, v_id_t dst) { return update(src, dst); }

  // Handle cond.
  bool cond(v_id_t) { return true; }
};

struct pagerank_vertex_functor_t {
  std::atomic<double>* next;
  double damping_factor;
  double added_constant;

  // Handle operator.
  bool operator()(v_id_t v) {
    double value = next[v].load(std::memory_order_relaxed);
    next[v].store(damping_factor * value + added_constant, std::memory_order_relaxed);
    return true;
  }
};

struct pagerank_reset_functor_t {
  std::atomic<double>* values;

  // Handle operator.
  bool operator()(v_id_t v) {
    values[v].store(0.0, std::memory_order_relaxed);
    return true;
  }
};

constexpr v_id_t kEmptyVoteKey = std::numeric_limits<v_id_t>::max();
constexpr uint32_t kCdlpInitSlots = 16;

struct cdlp_vote_slot_t {
  v_id_t key;
  uint32_t count;
};

struct cdlp_vote_functor_t {
  const std::atomic<v_id_t>* current_labels;
  std::atomic<v_id_t>* slot_keys;
  std::atomic<uint32_t>* slot_counts;
  std::atomic<bool>* overflow;

  // Handle cond.
  bool cond(v_id_t) { return true; }

  // Update update.
  bool update(v_id_t src, v_id_t dst) { return update_atomic(src, dst); }

  // Update atomic.
  bool update_atomic(v_id_t src, v_id_t dst) {
    if (overflow[dst].load(std::memory_order_relaxed)) {
      return false;
    }

    v_id_t label = current_labels[src].load(std::memory_order_relaxed);
    uint32_t base = dst * kCdlpInitSlots;

    for (uint32_t probe = 0; probe < kCdlpInitSlots; ++probe) {
      uint32_t idx = base + probe;
      v_id_t cur = slot_keys[idx].load(std::memory_order_relaxed);
      if (cur == label) {
        slot_counts[idx].fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      if (cur == kEmptyVoteKey) {
        v_id_t expected = kEmptyVoteKey;
        if (slot_keys[idx].compare_exchange_strong(expected, label, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
          slot_counts[idx].fetch_add(1, std::memory_order_relaxed);
          return true;
        }
        cur = slot_keys[idx].load(std::memory_order_relaxed);
        if (cur == label) {
          slot_counts[idx].fetch_add(1, std::memory_order_relaxed);
          return true;
        }
      }
    }

    overflow[dst].store(true, std::memory_order_relaxed);
    return false;
  }
};

struct cdlp_commit_functor_t {
  const cdlp_vote_slot_t* arena;
  const std::atomic<v_id_t>* current_labels;
  std::atomic<v_id_t>* next_labels;
  const std::atomic<bool>* overflow;

  // Handle operator.
  bool operator()(v_id_t v) {
    v_id_t current = current_labels[v].load(std::memory_order_relaxed);
    v_id_t winner = current;
    if (!overflow[v].load(std::memory_order_relaxed)) {
      uint32_t base = v * kCdlpInitSlots;
      uint32_t best_count = 0;
      for (uint32_t i = 0; i < kCdlpInitSlots; ++i) {
        const auto& slot = arena[base + i];
        if (slot.key == kEmptyVoteKey) {
          continue;
        }
        if (slot.count > best_count || (slot.count == best_count && slot.key < winner)) {
          best_count = slot.count;
          winner = slot.key;
        }
      }
    }
    next_labels[v].store(winner, std::memory_order_relaxed);
    return true;
  }
};

struct cdlp_reset_functor_t {
  std::atomic<v_id_t>* slot_keys;
  std::atomic<uint32_t>* slot_counts;
  std::atomic<bool>* overflow;

  // Handle operator.
  bool operator()(v_id_t v) {
    uint32_t base = v * kCdlpInitSlots;
    for (uint32_t i = 0; i < kCdlpInitSlots; ++i) {
      slot_keys[base + i].store(kEmptyVoteKey, std::memory_order_relaxed);
      slot_counts[base + i].store(0, std::memory_order_relaxed);
    }
    overflow[v].store(false, std::memory_order_relaxed);
    return true;
  }
};

// Atomically lower dst's component label and report first-round activation.
struct wcc_min_functor_t {
  std::atomic<v_id_t>* IDs;
  std::atomic<v_id_t>* prevIDs;

  // Update update.
  bool update(v_id_t src, v_id_t dst) { return update_atomic(src, dst); }

  // Update atomic.
  bool update_atomic(v_id_t src, v_id_t dst) {
    v_id_t src_id = IDs[src].load(std::memory_order_relaxed);
    v_id_t cur = IDs[dst].load(std::memory_order_relaxed);
    if (src_id >= cur)
      return false;

    // Remember the call-start value to detect first activation.
    v_id_t orig = cur;
    while (src_id < cur) {
      if (IDs[dst].compare_exchange_weak(cur, src_id, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
        // Activate dst only on its first round-local change.
        return orig == prevIDs[dst].load(std::memory_order_relaxed);
      }
      // cur now holds the value installed by a concurrent thread.
    }
    return false;
  }

  // Handle cond.
  bool cond(v_id_t) { return true; }
};

// Sync prevIDs ← IDs at the start of each WCC-Map round.
struct wcc_sync_functor_t {
  std::atomic<v_id_t>* IDs;
  std::atomic<v_id_t>* prevIDs;

  // Handle operator.
  bool operator()(v_id_t v) {
    prevIDs[v].store(IDs[v].load(std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
  }
};

// Handle make full frontier.
vertex_subset_t make_full_frontier(v_id_t vertex_count) {
  bool* active = newA(bool, vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    active[i] = true;
  }
  return vertex_subset_t(static_cast<long>(vertex_count), static_cast<long>(vertex_count), active);
}

// Compute bfs level.
uint64_t compute_bfs_level(v_id_t vertex_id, const std::vector<v_id_t>& parent,
                           std::vector<uint64_t>& levels, std::vector<uint8_t>& known) {
  if (known[vertex_id]) {
    return levels[vertex_id];
  }

  std::vector<v_id_t> path;
  v_id_t current = vertex_id;
  while (!known[current]) {
    path.push_back(current);
    current = parent[current];
  }

  uint64_t level = levels[current];
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    ++level;
    levels[*it] = level;
    known[*it] = 1;
  }
  return levels[vertex_id];
}

} // namespace

// Graph sampling based on random walk;
std::shared_ptr<mem_sub_map_t> bw_graph_db_t::graph_sample_walk(v_id_t start_vertex,
                                                                uint64_t max_vertices) {
  // Vertex count for boundary checking
  uint64_t vertex_count = this->get_vertex_count();

  // Track sampled vertices in order
  std::vector<v_id_t> sampled_vertices;
  sampled_vertices.reserve(max_vertices);

  // Track which vertices have been sampled
  folly::F14FastSet<v_id_t> visited_set;
  visited_set.reserve(max_vertices);

  // Adjacency map to store walked edges
  folly::F14FastMap<v_id_t, std::vector<v_id_t>> walked_edges;

  // Random number generator for selecting neighbors
  std::random_device rd;
  std::mt19937 gen(rd());

  // Start random walk from the start vertex
  v_id_t current_vertex = start_vertex;
  sampled_vertices.push_back(current_vertex);
  visited_set.insert(current_vertex);

  // Continue random walk until reaching exactly max_vertices
  while (sampled_vertices.size() < max_vertices) {
    // Read neighbors of current vertex
    auto [neighbors_span, current_page] = read_neighbor(current_vertex);

    // If current vertex has no neighbors, we need to restart from a sampled
    // vertex
    if (neighbors_span.empty()) {
      current_page->r_unlatch();

      // Restart from a random previously sampled vertex
      if (sampled_vertices.size() > 1) {
        std::uniform_int_distribution<size_t> restart_dist(0, sampled_vertices.size() - 1);
        current_vertex = sampled_vertices[restart_dist(gen)];
        continue;
      } else {
        // Only have start vertex and it has no neighbors, cannot continue
        break;
      }
    }

    // Collect all valid neighbors
    std::vector<v_id_t> valid_neighbors;
    valid_neighbors.reserve(neighbors_span.size());

    for (auto const& neighbor : neighbors_span) {
      if (neighbor < vertex_count) {
        valid_neighbors.push_back(neighbor);
      }
    }

    current_page->r_unlatch(); // Release the read latch

    // If no valid neighbors found, restart from a sampled vertex
    if (valid_neighbors.empty()) {
      if (sampled_vertices.size() > 1) {
        std::uniform_int_distribution<size_t> restart_dist(0, sampled_vertices.size() - 1);
        current_vertex = sampled_vertices[restart_dist(gen)];
        continue;
      } else {
        break;
      }
    }

    // Randomly select one neighbor to visit next
    std::uniform_int_distribution<size_t> dist(0, valid_neighbors.size() - 1);
    size_t random_index = dist(gen);
    v_id_t next_vertex = valid_neighbors[random_index];

    // Record the walked edge: current_vertex -> next_vertex
    walked_edges[current_vertex].push_back(next_vertex);

    // If this is a new vertex, add it to sampled vertices
    if (visited_set.find(next_vertex) == visited_set.end()) {
      sampled_vertices.push_back(next_vertex);
      visited_set.insert(next_vertex);
    }

    // Move to the next vertex
    current_vertex = next_vertex;

    // Optional: With small probability, restart from a random sampled vertex
    // to explore different regions of the graph
    std::uniform_real_distribution<double> restart_prob(0.0, 1.0);
    if (restart_prob(gen) < 0.15 && sampled_vertices.size() > 1) {
      std::uniform_int_distribution<size_t> restart_dist(0, sampled_vertices.size() - 1);
      current_vertex = sampled_vertices[restart_dist(gen)];
    }
  }

  // Create the sampled subgraph containing only the walked edges and vertices
  return std::make_shared<mem_sub_map_t>(walked_edges);
}

// Benchmark parallel bfs map.
double bw_graph_db_t::benchmark_parallel_bfs_map(v_id_t start_vertex, long threshold_divisor,
                                                 bool print_result) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0 || start_vertex >= vertex_count) {
    return 0.0;
  }
  const uint64_t resident_page_capacity =
      bw_graph::BW_BUFFER_CHUNK_COUNT * bw_graph::BW_BUFFER_CHUNK_SIZE;
  if (this->disk_manager->get_page_count() > resident_page_capacity) {
    threshold_divisor = std::max<long>(threshold_divisor, 40);
  }

  auto parent_atomic = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    parent_atomic[i].store(INVALID_VID, std::memory_order_relaxed);
  }
  parent_atomic[start_vertex].store(start_vertex, std::memory_order_relaxed);

  bfs_functor_t functor{parent_atomic.get()};
  vertex_subset_t frontier(static_cast<long>(vertex_count), start_vertex);

  auto begin = std::chrono::high_resolution_clock::now();
  while (!frontier.is_empty()) {
    vertex_subset_t next = bw_edge_map(*this, frontier, functor, threshold_divisor);
    frontier.del();
    frontier = std::move(next);
  }
  frontier.del();
  auto end = std::chrono::high_resolution_clock::now();

  if (print_result) {
    uint32_t discovered = 0;
    for (v_id_t i = 0; i < vertex_count; ++i) {
      if (parent_atomic[i].load(std::memory_order_relaxed) != INVALID_VID) {
        ++discovered;
      }
    }
    double elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
    printf("BFS from %-10u : discovered %u / %u vertices in %.4f s\n", start_vertex, discovered,
           vertex_count, elapsed);
  }

  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

// Run parallel bfs map.
std::vector<std::pair<v_id_t, uint32_t>> bw_graph_db_t::parallel_bfs_map(v_id_t start_vertex,
                                                                         long threshold_divisor) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0 || start_vertex >= vertex_count) {
    return {};
  }
  const uint64_t resident_page_capacity =
      bw_graph::BW_BUFFER_CHUNK_COUNT * bw_graph::BW_BUFFER_CHUNK_SIZE;
  if (this->disk_manager->get_page_count() > resident_page_capacity) {
    threshold_divisor = std::max<long>(threshold_divisor, 40);
  }

  auto parent_atomic = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    parent_atomic[i].store(INVALID_VID, std::memory_order_relaxed);
  }
  parent_atomic[start_vertex].store(start_vertex, std::memory_order_relaxed);

  bfs_functor_t functor{parent_atomic.get()};
  vertex_subset_t frontier(static_cast<long>(vertex_count), start_vertex);

  while (!frontier.is_empty()) {
    vertex_subset_t next = bw_edge_map(*this, frontier, functor, threshold_divisor);
    frontier.del();
    frontier = std::move(next);
  }
  frontier.del();

  std::vector<std::pair<v_id_t, uint32_t>> results;
  results.reserve(vertex_count);
  for (v_id_t v = 0; v < vertex_count; ++v) {
    if (parent_atomic[v].load(std::memory_order_relaxed) == INVALID_VID) {
      continue;
    }
    uint32_t dist = 0;
    v_id_t curr = v;
    while (parent_atomic[curr].load(std::memory_order_relaxed) != curr) {
      ++dist;
      curr = parent_atomic[curr].load(std::memory_order_relaxed);
    }
    results.emplace_back(v, dist);
  }
  return results;
}

// Benchmark wcc sv.
double bw_graph_db_t::benchmark_wcc_sv(long threshold_divisor, bool print_result) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return 0.0;
  }

  auto parent = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    parent[i].store(i, std::memory_order_relaxed);
  }

  auto changed = std::make_unique<std::atomic<bool>>(false);
  auto moved = std::make_unique<std::atomic<bool>>(false);
  wcc_hook_functor_t hook_functor{parent.get(), changed.get()};
  wcc_compress_functor_t compress_functor{parent.get(), moved.get()};
  vertex_subset_t all = make_full_frontier(vertex_count);

  auto begin = std::chrono::high_resolution_clock::now();
  uint32_t round = 0;

  while (true) {
    auto round_begin = std::chrono::high_resolution_clock::now();
    changed->store(false, std::memory_order_relaxed);
    vertex_subset_t output = bw_edge_map(*this, all, hook_functor, threshold_divisor);
    output.del();

    bool any_hook = changed->load(std::memory_order_relaxed);
    uint32_t compress_passes = 0;
    while (true) {
      moved->store(false, std::memory_order_relaxed);
      bw_vertex_map(all, compress_functor);
      ++compress_passes;
      if (!moved->load(std::memory_order_relaxed)) {
        break;
      }
    }

    auto round_end = std::chrono::high_resolution_clock::now();
    if (print_result) {
      double elapsed_ms =
          std::chrono::duration_cast<std::chrono::microseconds>(round_end - round_begin).count() /
          1e3;
      printf("  round %3u  hook=%s  compress_passes=%u  %.2f ms\n", round, any_hook ? "yes" : "no ",
             compress_passes, elapsed_ms);
    }
    ++round;

    if (!any_hook) {
      break;
    }
  }

  all.del();
  auto end = std::chrono::high_resolution_clock::now();

  if (print_result) {
    std::vector<uint8_t> seen(vertex_count, 0);
    uint32_t component_count = 0;
    for (v_id_t i = 0; i < vertex_count; ++i) {
      v_id_t root = wcc_find_root(parent.get(), i);
      if (!seen[root]) {
        seen[root] = 1;
        ++component_count;
      }
    }
    double elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
    printf("WCC-SV: %u rounds, %u components across %u vertices in %.4f s\n", round,
           component_count, vertex_count, elapsed);
  }

  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

// Run wcc sv.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::wcc_sv(long threshold_divisor) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return {};
  }

  auto parent = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    parent[i].store(i, std::memory_order_relaxed);
  }

  auto changed = std::make_unique<std::atomic<bool>>(false);
  auto moved = std::make_unique<std::atomic<bool>>(false);
  wcc_hook_functor_t hook_functor{parent.get(), changed.get()};
  wcc_compress_functor_t compress_functor{parent.get(), moved.get()};
  vertex_subset_t all = make_full_frontier(vertex_count);

  while (true) {
    changed->store(false, std::memory_order_relaxed);
    vertex_subset_t output = bw_edge_map(*this, all, hook_functor, threshold_divisor);
    output.del();

    bool any_hook = changed->load(std::memory_order_relaxed);
    while (true) {
      moved->store(false, std::memory_order_relaxed);
      bw_vertex_map(all, compress_functor);
      if (!moved->load(std::memory_order_relaxed)) {
        break;
      }
    }

    if (!any_hook) {
      break;
    }
  }
  all.del();

  std::vector<std::pair<v_id_t, v_id_t>> results;
  results.reserve(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    results.emplace_back(i, wcc_find_root(parent.get(), i));
  }
  return results;
}

// Benchmark wcc map.
double bw_graph_db_t::benchmark_wcc_map(long threshold_divisor, bool print_result) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0)
    return 0.0;

  // Each vertex starts as its own component (label = vertex ID).
  auto IDs = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  auto prevIDs = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t i = range.begin(); i < range.end(); ++i) {
                        IDs[i].store(i, std::memory_order_relaxed);
                        prevIDs[i].store(i, std::memory_order_relaxed);
                      }
                    });

  // All vertices are active initially.
  vertex_subset_t frontier = make_full_frontier(vertex_count);
  wcc_min_functor_t edge_functor{IDs.get(), prevIDs.get()};
  wcc_sync_functor_t sync_functor{IDs.get(), prevIDs.get()};

  auto begin = std::chrono::high_resolution_clock::now();
  uint32_t round = 0;

  // Iterate until no vertex label changes.
  while (!frontier.is_empty()) {
    // Snapshot current labels into prevIDs before this round's propagation.
    bw_vertex_map(frontier, sync_functor);
    vertex_subset_t output = bw_edge_map(*this, frontier, edge_functor, threshold_divisor);
    frontier.del();
    frontier = std::move(output);
    ++round;
  }
  frontier.del();

  auto end = std::chrono::high_resolution_clock::now();

  if (print_result) {
    std::vector<uint8_t> seen(vertex_count, 0);
    uint32_t component_count = 0;
    for (v_id_t i = 0; i < vertex_count; ++i) {
      v_id_t comp = IDs[i].load(std::memory_order_relaxed);
      if (comp < vertex_count && !seen[comp]) {
        seen[comp] = 1;
        ++component_count;
      }
    }
    double elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
    printf("WCC-Map: %u rounds, %u components across %u vertices in %.4f s\n", round,
           component_count, vertex_count, elapsed);
  }

  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

// Run wcc map.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::wcc_map(long threshold_divisor) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return {};
  }

  auto IDs = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  auto prevIDs = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t i = range.begin(); i < range.end(); ++i) {
                        IDs[i].store(i, std::memory_order_relaxed);
                        prevIDs[i].store(i, std::memory_order_relaxed);
                      }
                    });

  vertex_subset_t frontier = make_full_frontier(vertex_count);
  wcc_min_functor_t edge_functor{IDs.get(), prevIDs.get()};
  wcc_sync_functor_t sync_functor{IDs.get(), prevIDs.get()};

  while (!frontier.is_empty()) {
    bw_vertex_map(frontier, sync_functor);
    vertex_subset_t output = bw_edge_map(*this, frontier, edge_functor, threshold_divisor);
    frontier.del();
    frontier = std::move(output);
  }
  frontier.del();

  std::vector<std::pair<v_id_t, v_id_t>> results;
  results.reserve(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    results.emplace_back(i, IDs[i].load(std::memory_order_relaxed));
  }
  return results;
}

// Benchmark parallel pagerank map.
double bw_graph_db_t::benchmark_parallel_pagerank_map(uint64_t max_iterations,
                                                      double damping_factor,
                                                      double convergence_threshold,
                                                      bool print_result) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return 0.0;
  }

  auto inverse_degree = std::make_unique<double[]>(vertex_count);
  tbb::parallel_for(
      tbb::blocked_range<v_id_t>(0, vertex_count), [&](const tbb::blocked_range<v_id_t>& range) {
        for (v_id_t v = range.begin(); v < range.end(); ++v) {
          auto [neighbors, page] = this->read_neighbor(v);
          inverse_degree[v] = neighbors.empty() ? 0.0 : 1.0 / static_cast<double>(neighbors.size());
          page->r_unlatch();
        }
      });

  auto buffer0 = std::make_unique<std::atomic<double>[]>(vertex_count);
  auto buffer1 = std::make_unique<std::atomic<double>[]>(vertex_count);
  double initial_rank = 1.0 / static_cast<double>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        buffer0[v].store(initial_rank, std::memory_order_relaxed);
                        buffer1[v].store(0.0, std::memory_order_relaxed);
                      }
                    });

  std::atomic<double>* current = buffer0.get();
  std::atomic<double>* next = buffer1.get();
  std::atomic<double>* final_ranks = current;
  vertex_subset_t frontier = make_full_frontier(vertex_count);

  pagerank_edge_functor_t edge_functor{current, next, inverse_degree.get()};
  pagerank_vertex_functor_t vertex_functor{next, damping_factor,
                                           (1.0 - damping_factor) * initial_rank};
  pagerank_reset_functor_t reset_functor{current};

  auto begin = std::chrono::high_resolution_clock::now();
  double l1_norm = 1.0;
  uint64_t iteration = 0;

  for (; iteration < max_iterations; ++iteration) {
    bw_edge_map_no_output(*this, frontier, edge_functor);
    bw_vertex_map(frontier, vertex_functor);

    l1_norm = tbb::parallel_reduce(
        tbb::blocked_range<v_id_t>(0, vertex_count), 0.0,
        [&](const tbb::blocked_range<v_id_t>& range, double sum) {
          for (v_id_t v = range.begin(); v < range.end(); ++v) {
            sum += std::fabs(current[v].load(std::memory_order_relaxed) -
                             next[v].load(std::memory_order_relaxed));
          }
          return sum;
        },
        std::plus<double>());

    if (print_result) {
      printf("iter = %" PRIu64 ", L1_norm = %.9f\n", iteration + 1, l1_norm);
    }

    final_ranks = next;
    if (l1_norm < convergence_threshold) {
      ++iteration;
      break;
    }

    reset_functor.values = current;
    bw_vertex_map(frontier, reset_functor);

    std::swap(current, next);
    final_ranks = current;
    edge_functor.current = current;
    edge_functor.next = next;
    vertex_functor.next = next;
    reset_functor.values = current;
  }

  auto end = std::chrono::high_resolution_clock::now();
  if (print_result) {
    double elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
    printf("PageRank: %" PRIu64 " iterations, final L1_norm = %.9f, elapsed = %.4f s\n", iteration,
           l1_norm, elapsed);
  }

  frontier.del();
  (void) final_ranks;
  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

// Run parallel pagerank map.
std::vector<std::pair<v_id_t, double>>
bw_graph_db_t::parallel_pagerank_map(uint64_t max_iterations, double damping_factor,
                                     double convergence_threshold) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return {};
  }

  auto inverse_degree = std::make_unique<double[]>(vertex_count);
  tbb::parallel_for(
      tbb::blocked_range<v_id_t>(0, vertex_count), [&](const tbb::blocked_range<v_id_t>& range) {
        for (v_id_t v = range.begin(); v < range.end(); ++v) {
          auto [neighbors, page] = this->read_neighbor(v);
          inverse_degree[v] = neighbors.empty() ? 0.0 : 1.0 / static_cast<double>(neighbors.size());
          page->r_unlatch();
        }
      });

  auto buffer0 = std::make_unique<std::atomic<double>[]>(vertex_count);
  auto buffer1 = std::make_unique<std::atomic<double>[]>(vertex_count);
  double initial_rank = 1.0 / static_cast<double>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        buffer0[v].store(initial_rank, std::memory_order_relaxed);
                        buffer1[v].store(0.0, std::memory_order_relaxed);
                      }
                    });

  std::atomic<double>* current = buffer0.get();
  std::atomic<double>* next = buffer1.get();
  std::atomic<double>* final_ranks = current;
  vertex_subset_t frontier = make_full_frontier(vertex_count);

  pagerank_edge_functor_t edge_functor{current, next, inverse_degree.get()};
  pagerank_vertex_functor_t vertex_functor{next, damping_factor,
                                           (1.0 - damping_factor) * initial_rank};
  pagerank_reset_functor_t reset_functor{current};

  for (uint64_t iteration = 0; iteration < max_iterations; ++iteration) {
    bw_edge_map_no_output(*this, frontier, edge_functor);
    bw_vertex_map(frontier, vertex_functor);

    double l1_norm = tbb::parallel_reduce(
        tbb::blocked_range<v_id_t>(0, vertex_count), 0.0,
        [&](const tbb::blocked_range<v_id_t>& range, double sum) {
          for (v_id_t v = range.begin(); v < range.end(); ++v) {
            sum += std::fabs(current[v].load(std::memory_order_relaxed) -
                             next[v].load(std::memory_order_relaxed));
          }
          return sum;
        },
        std::plus<double>());

    final_ranks = next;
    if (l1_norm < convergence_threshold) {
      break;
    }

    reset_functor.values = current;
    bw_vertex_map(frontier, reset_functor);

    std::swap(current, next);
    final_ranks = current;
    edge_functor.current = current;
    edge_functor.next = next;
    vertex_functor.next = next;
    reset_functor.values = current;
  }

  frontier.del();

  std::vector<std::pair<v_id_t, double>> results;
  results.reserve(vertex_count);
  for (v_id_t v = 0; v < vertex_count; ++v) {
    results.emplace_back(v, final_ranks[v].load(std::memory_order_relaxed));
  }
  return results;
}

// Benchmark parallel cdlp map.
double bw_graph_db_t::benchmark_parallel_cdlp_map(uint64_t max_iterations, bool print_result) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return 0.0;
  }

  auto current_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  auto next_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        current_labels[v].store(v, std::memory_order_relaxed);
                        next_labels[v].store(v, std::memory_order_relaxed);
                      }
                    });

  size_t total_slots = static_cast<size_t>(vertex_count) * kCdlpInitSlots;
  auto arena = std::make_unique<cdlp_vote_slot_t[]>(total_slots);
  auto slot_keys = std::make_unique<std::atomic<v_id_t>[]>(total_slots);
  auto slot_counts = std::make_unique<std::atomic<uint32_t>[]>(total_slots);
  auto overflow = std::make_unique<std::atomic<bool>[]>(vertex_count);

  tbb::parallel_for(tbb::blocked_range<size_t>(0, total_slots),
                    [&](const tbb::blocked_range<size_t>& range) {
                      for (size_t i = range.begin(); i < range.end(); ++i) {
                        arena[i].key = kEmptyVoteKey;
                        arena[i].count = 0;
                        slot_keys[i].store(kEmptyVoteKey, std::memory_order_relaxed);
                        slot_counts[i].store(0, std::memory_order_relaxed);
                      }
                    });
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        overflow[v].store(false, std::memory_order_relaxed);
                      }
                    });

  vertex_subset_t frontier = make_full_frontier(vertex_count);
  cdlp_vote_functor_t vote_functor{current_labels.get(), slot_keys.get(), slot_counts.get(),
                                   overflow.get()};
  cdlp_commit_functor_t commit_functor{arena.get(), current_labels.get(), next_labels.get(),
                                       overflow.get()};
  cdlp_reset_functor_t reset_functor{slot_keys.get(), slot_counts.get(), overflow.get()};

  auto begin = std::chrono::high_resolution_clock::now();
  uint64_t iteration = 0;
  long changed = static_cast<long>(vertex_count);

  while (iteration < max_iterations && changed > 0) {
    auto round_begin = std::chrono::high_resolution_clock::now();

    bw_edge_map_no_output(*this, frontier, vote_functor);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, total_slots),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i < range.end(); ++i) {
                          arena[i].key = slot_keys[i].load(std::memory_order_relaxed);
                          arena[i].count = slot_counts[i].load(std::memory_order_relaxed);
                        }
                      });

    bw_vertex_map(frontier, commit_functor);

    changed = tbb::parallel_reduce(
        tbb::blocked_range<v_id_t>(0, vertex_count), 0L,
        [&](const tbb::blocked_range<v_id_t>& range, long sum) {
          for (v_id_t v = range.begin(); v < range.end(); ++v) {
            if (current_labels[v].load(std::memory_order_relaxed) !=
                next_labels[v].load(std::memory_order_relaxed)) {
              ++sum;
            }
          }
          return sum;
        },
        std::plus<long>());

    if (print_result) {
      uint32_t overflow_count = 0;
      for (v_id_t v = 0; v < vertex_count; ++v) {
        if (overflow[v].load(std::memory_order_relaxed)) {
          ++overflow_count;
        }
      }
      auto round_end = std::chrono::high_resolution_clock::now();
      double elapsed_ms =
          std::chrono::duration_cast<std::chrono::microseconds>(round_end - round_begin).count() /
          1e3;
      printf("  iter %3" PRIu64 "  changed=%ld  overflow_vertices=%u  %.2f ms\n", iteration + 1,
             changed, overflow_count, elapsed_ms);
    }

    bw_vertex_map(frontier, reset_functor);
    std::swap(current_labels, next_labels);
    vote_functor.current_labels = current_labels.get();
    commit_functor.current_labels = current_labels.get();
    commit_functor.next_labels = next_labels.get();
    ++iteration;
  }

  auto end = std::chrono::high_resolution_clock::now();
  if (print_result) {
    std::vector<uint8_t> seen(vertex_count, 0);
    uint32_t community_count = 0;
    for (v_id_t v = 0; v < vertex_count; ++v) {
      v_id_t label = current_labels[v].load(std::memory_order_relaxed);
      if (!seen[label]) {
        seen[label] = 1;
        ++community_count;
      }
    }
    double elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
    printf("LPA: %" PRIu64 " rounds, %u communities, elapsed=%.4f s\n", iteration, community_count,
           elapsed);
  }

  frontier.del();
  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

// Run parallel cdlp map.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::parallel_cdlp_map(uint64_t max_iterations) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return {};
  }

  auto current_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  auto next_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        current_labels[v].store(v, std::memory_order_relaxed);
                        next_labels[v].store(v, std::memory_order_relaxed);
                      }
                    });

  size_t total_slots = static_cast<size_t>(vertex_count) * kCdlpInitSlots;
  auto arena = std::make_unique<cdlp_vote_slot_t[]>(total_slots);
  auto slot_keys = std::make_unique<std::atomic<v_id_t>[]>(total_slots);
  auto slot_counts = std::make_unique<std::atomic<uint32_t>[]>(total_slots);
  auto overflow = std::make_unique<std::atomic<bool>[]>(vertex_count);

  tbb::parallel_for(tbb::blocked_range<size_t>(0, total_slots),
                    [&](const tbb::blocked_range<size_t>& range) {
                      for (size_t i = range.begin(); i < range.end(); ++i) {
                        arena[i].key = kEmptyVoteKey;
                        arena[i].count = 0;
                        slot_keys[i].store(kEmptyVoteKey, std::memory_order_relaxed);
                        slot_counts[i].store(0, std::memory_order_relaxed);
                      }
                    });
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        overflow[v].store(false, std::memory_order_relaxed);
                      }
                    });

  vertex_subset_t frontier = make_full_frontier(vertex_count);
  cdlp_vote_functor_t vote_functor{current_labels.get(), slot_keys.get(), slot_counts.get(),
                                   overflow.get()};
  cdlp_commit_functor_t commit_functor{arena.get(), current_labels.get(), next_labels.get(),
                                       overflow.get()};
  cdlp_reset_functor_t reset_functor{slot_keys.get(), slot_counts.get(), overflow.get()};

  uint64_t iteration = 0;
  long changed = static_cast<long>(vertex_count);
  while (iteration < max_iterations && changed > 0) {
    bw_edge_map_no_output(*this, frontier, vote_functor);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, total_slots),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i < range.end(); ++i) {
                          arena[i].key = slot_keys[i].load(std::memory_order_relaxed);
                          arena[i].count = slot_counts[i].load(std::memory_order_relaxed);
                        }
                      });

    bw_vertex_map(frontier, commit_functor);

    changed = tbb::parallel_reduce(
        tbb::blocked_range<v_id_t>(0, vertex_count), 0L,
        [&](const tbb::blocked_range<v_id_t>& range, long sum) {
          for (v_id_t v = range.begin(); v < range.end(); ++v) {
            if (current_labels[v].load(std::memory_order_relaxed) !=
                next_labels[v].load(std::memory_order_relaxed)) {
              ++sum;
            }
          }
          return sum;
        },
        std::plus<long>());

    bw_vertex_map(frontier, reset_functor);
    std::swap(current_labels, next_labels);
    vote_functor.current_labels = current_labels.get();
    commit_functor.current_labels = current_labels.get();
    commit_functor.next_labels = next_labels.get();
    ++iteration;
  }

  frontier.del();

  std::vector<std::pair<v_id_t, v_id_t>> results;
  results.reserve(vertex_count);
  for (v_id_t v = 0; v < vertex_count; ++v) {
    results.emplace_back(v, current_labels[v].load(std::memory_order_relaxed));
  }
  return results;
}

// Pull-based CDLP with hash-map vote counting (no fixed-slot overflow).
// Each vertex reads its neighbours' current labels, counts frequencies in a
// thread-local F14FastMap, and adopts the most-voted label (lowest ID on ties).
double bw_graph_db_t::benchmark_cdlp_voting_map(uint64_t max_iterations, bool print_result) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0)
    return 0.0;

  auto current_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  auto next_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        current_labels[v].store(v, std::memory_order_relaxed);
                        next_labels[v].store(v, std::memory_order_relaxed);
                      }
                    });

  auto begin = std::chrono::high_resolution_clock::now();
  uint64_t iteration = 0;
  long changed = static_cast<long>(vertex_count);

  while (iteration < max_iterations && changed > 0) {
    auto round_begin = std::chrono::high_resolution_clock::now();

    // Pull model: each vertex reads its own neighbours independently.
    // A thread-local F14FastMap accumulates votes without concurrent writes.
    tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                      [&](const tbb::blocked_range<v_id_t>& range) {
                        folly::F14FastMap<v_id_t, uint32_t> vote_map;
                        for (v_id_t v = range.begin(); v < range.end(); ++v) {
                          auto [neighbors, page] = this->read_neighbor(v);
                          if (neighbors.empty()) {
                            next_labels[v].store(current_labels[v].load(std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
                            page->r_unlatch();
                            continue;
                          }

                          vote_map.clear();
                          for (v_id_t nb : neighbors) {
                            if (nb < vertex_count) {
                              vote_map[current_labels[nb].load(std::memory_order_relaxed)]++;
                            }
                          }
                          page->r_unlatch();

                          // Pick label with highest count; break ties by lowest label ID.
                          v_id_t best = current_labels[v].load(std::memory_order_relaxed);
                          uint32_t best_cnt = 0;
                          for (const auto& [lbl, cnt] : vote_map) {
                            if (cnt > best_cnt || (cnt == best_cnt && lbl < best)) {
                              best_cnt = cnt;
                              best = lbl;
                            }
                          }
                          next_labels[v].store(best, std::memory_order_relaxed);
                        }
                      });

    // Count how many vertices changed label.
    changed = tbb::parallel_reduce(
        tbb::blocked_range<v_id_t>(0, vertex_count), 0L,
        [&](const tbb::blocked_range<v_id_t>& range, long sum) {
          for (v_id_t v = range.begin(); v < range.end(); ++v) {
            if (current_labels[v].load(std::memory_order_relaxed) !=
                next_labels[v].load(std::memory_order_relaxed))
              ++sum;
          }
          return sum;
        },
        std::plus<long>());

    if (print_result) {
      auto round_end = std::chrono::high_resolution_clock::now();
      double elapsed_ms =
          std::chrono::duration_cast<std::chrono::microseconds>(round_end - round_begin).count() /
          1e3;
      printf("  iter %3" PRIu64 "  changed=%ld  %.2f ms\n", iteration + 1, changed, elapsed_ms);
    }

    std::swap(current_labels, next_labels);
    ++iteration;
  }

  auto end = std::chrono::high_resolution_clock::now();
  if (print_result) {
    std::vector<uint8_t> seen(vertex_count, 0);
    uint32_t community_count = 0;
    for (v_id_t v = 0; v < vertex_count; ++v) {
      v_id_t label = current_labels[v].load(std::memory_order_relaxed);
      if (!seen[label]) {
        seen[label] = 1;
        ++community_count;
      }
    }
    double elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
    printf("CDLP-Voting: %" PRIu64 " rounds, %u communities, elapsed=%.4f s\n", iteration,
           community_count, elapsed);
  }

  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

// Run cdlp voting map.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::cdlp_voting_map(uint64_t max_iterations) {
  v_id_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    return {};
  }

  auto current_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  auto next_labels = std::make_unique<std::atomic<v_id_t>[]>(vertex_count);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t v = range.begin(); v < range.end(); ++v) {
                        current_labels[v].store(v, std::memory_order_relaxed);
                        next_labels[v].store(v, std::memory_order_relaxed);
                      }
                    });

  uint64_t iteration = 0;
  long changed = static_cast<long>(vertex_count);
  while (iteration < max_iterations && changed > 0) {
    tbb::parallel_for(tbb::blocked_range<v_id_t>(0, vertex_count),
                      [&](const tbb::blocked_range<v_id_t>& range) {
                        folly::F14FastMap<v_id_t, uint32_t> vote_map;
                        for (v_id_t v = range.begin(); v < range.end(); ++v) {
                          auto [neighbors, page] = this->read_neighbor(v);
                          if (neighbors.empty()) {
                            next_labels[v].store(current_labels[v].load(std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
                            page->r_unlatch();
                            continue;
                          }

                          vote_map.clear();
                          for (v_id_t nb : neighbors) {
                            if (nb < vertex_count) {
                              vote_map[current_labels[nb].load(std::memory_order_relaxed)]++;
                            }
                          }
                          page->r_unlatch();

                          v_id_t best = current_labels[v].load(std::memory_order_relaxed);
                          uint32_t best_cnt = 0;
                          for (const auto& [lbl, cnt] : vote_map) {
                            if (cnt > best_cnt || (cnt == best_cnt && lbl < best)) {
                              best_cnt = cnt;
                              best = lbl;
                            }
                          }
                          next_labels[v].store(best, std::memory_order_relaxed);
                        }
                      });

    changed = tbb::parallel_reduce(
        tbb::blocked_range<v_id_t>(0, vertex_count), 0L,
        [&](const tbb::blocked_range<v_id_t>& range, long sum) {
          for (v_id_t v = range.begin(); v < range.end(); ++v) {
            if (current_labels[v].load(std::memory_order_relaxed) !=
                next_labels[v].load(std::memory_order_relaxed)) {
              ++sum;
            }
          }
          return sum;
        },
        std::plus<long>());

    std::swap(current_labels, next_labels);
    ++iteration;
  }

  std::vector<std::pair<v_id_t, v_id_t>> results;
  results.reserve(vertex_count);
  for (v_id_t v = 0; v < vertex_count; ++v) {
    results.emplace_back(v, current_labels[v].load(std::memory_order_relaxed));
  }
  return results;
}

// Run bfs.
std::vector<std::pair<v_id_t, uint64_t>> bw_graph_db_t::bfs(v_id_t start_vertex) {
  // Vertex count to track visited order
  uint64_t vertex_count = this->get_vertex_count();
  std::vector<std::pair<v_id_t, uint64_t>> visited_order;
  visited_order.reserve(vertex_count);
  std::vector<bool> visited(vertex_count, false);
  std::vector<uint64_t> levels(vertex_count, 0);
  std::queue<v_id_t> to_visit;

  to_visit.push(start_vertex);
  visited[start_vertex] = true;
  levels[start_vertex] = 0;

  while (!to_visit.empty()) {
    v_id_t current_vertex = to_visit.front();
    to_visit.pop();
    visited_order.emplace_back(current_vertex, levels[current_vertex]);
    auto [neighbors_span, current_page] = read_neighbor(current_vertex);

    uint64_t next_level = levels[current_vertex] + 1;

    for (auto const& neighbor : neighbors_span) {
      if (neighbor < visited.size() && !visited[neighbor]) {
        visited[neighbor] = true;
        levels[neighbor] = next_level;
        to_visit.push(neighbor);
      }
    }

    current_page->r_unlatch(); // Release the read latch
  }

  return visited_order;
}

// Run wcc.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::wcc() {
  uint64_t vertex_count = this->get_vertex_count();
  std::vector<std::pair<v_id_t, v_id_t>> vertex_component;
  vertex_component.reserve(vertex_count);

  // Component ID for each vertex, initialized to unvisited (UINT32_MAX)
  std::vector<v_id_t> component_id(vertex_count, UINT32_MAX);
  std::queue<v_id_t> to_visit;

  v_id_t current_component = 0;

  // Iterate through all vertices to find all components
  for (v_id_t start_vertex = 0; start_vertex < vertex_count; ++start_vertex) {
    // Skip already visited vertices
    if (component_id[start_vertex] != UINT32_MAX) {
      continue;
    }

    // Start BFS for new component
    to_visit.push(start_vertex);
    component_id[start_vertex] = current_component;

    while (!to_visit.empty()) {
      v_id_t current_vertex = to_visit.front();
      to_visit.pop();

      // Get neighbors and the page (to release later)
      auto [neighbors_span, current_page] = read_neighbor(current_vertex);

      for (const v_id_t& neighbor : neighbors_span) {
        if (neighbor < vertex_count && component_id[neighbor] == UINT32_MAX) {
          component_id[neighbor] = current_component;
          to_visit.push(neighbor);
        }
      }

      if (current_page != nullptr) {
        current_page->r_unlatch(); // Release the read latch
      }
    }

    // Move to next component ID
    ++current_component;
  }

  // Build result: (vertex_id, component_id)
  for (v_id_t vertex = 0; vertex < vertex_count; ++vertex) {
    vertex_component.emplace_back(vertex, component_id[vertex]);
  }

  return vertex_component;
}

// Run parallel bfs.
std::vector<std::pair<v_id_t, uint32_t>> bw_graph_db_t::parallel_bfs(v_id_t start_vertex) {
  bit_map_t* active_in = new bit_map_t(this->get_vertex_count());
  bit_map_t* active_out = new bit_map_t(this->get_vertex_count());
  std::vector<v_id_t> parent(this->get_vertex_count(), INVALID_VID);

  active_out->clear();
  active_out->set_bit(start_vertex);
  parent[start_vertex] = start_vertex;

  uint32_t active_vertices = 1;
  double start_time = get_time();

  while (active_vertices != 0) {
    std::swap(active_in, active_out);
    active_out->clear();

    active_vertices = bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          if (parent[dst] == INVALID_VID) {
            if (__sync_bool_compare_and_swap(&parent[dst], INVALID_VID, src)) {
              active_out->set_bit(dst);
              return 1;
            }
          }
          return 0;
        },
        active_in);
  }

  double end_time = get_time();

  // Compute distances by traversing parent pointers
  std::vector<std::pair<v_id_t, uint32_t>> results;
  uint32_t discovered = 0;

  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    if (parent[v] != INVALID_VID) {
      discovered++;
      uint32_t dist = 0;
      v_id_t curr = v;
      while (parent[curr] != curr) {
        dist++;
        curr = parent[curr];
      }
      results.emplace_back(v, dist);
    }
  }

  printf("discovered %d vertices from %u in %6f seconds.\n", discovered, start_vertex,
         end_time - start_time);

  delete active_in;
  delete active_out;
  return results;
}

// Block based BFS;
std::vector<std::pair<v_id_t, uint64_t>> bw_graph_db_t::bfs_block(v_id_t start_vertex) {
  csr_page_t* start_page = this->read_page_vertex(start_vertex);
  // Vertex count to track visited order
  uint64_t vertex_count = this->get_vertex_count();
  std::vector<std::pair<v_id_t, uint64_t>> visited_order;
  visited_order.reserve(vertex_count);
  std::queue<std::pair<v_id_t, page_no_t>> block_queue;
  block_queue.push({start_vertex, start_page->get_page_no()});
  std::vector<bool> visited(vertex_count, false);
  folly::F14FastSet<page_no_t> visited_pages;

  while (!block_queue.empty()) {
    v_id_t current_vertex = block_queue.front().first;
    page_no_t current_page_no = block_queue.front().second;
    auto current_block = this->buffer_pool->buf_page_read(current_page_no, this->disk_manager);
    block_queue.pop();
    std::vector<v_id_t> outside;
    // Perform BFS on the current block
    std::vector<std::pair<v_id_t, uint64_t>> block_visited =
        this->bfs_block_inner(current_vertex, current_block, outside, visited);
    visited_pages.insert(current_block->get_page_no());
    visited_order.insert(visited_order.end(), block_visited.begin(), block_visited.end());

    // Push the corresponding blocks into the queue
    folly::F14FastMap<page_no_t, v_id_t> outside_map;
    for (v_id_t neighbor_id : outside) {
      auto v_loc = this->vertex_index->get_vertex_location(neighbor_id);
      if (visited[neighbor_id] || visited_pages.contains(v_loc.first)) {
        continue;
      }
      outside_map[v_loc.first] = neighbor_id;
    }

    for (const auto& [page_no, neighbor_id] : outside_map) {
      block_queue.push({neighbor_id, page_no});
    }
  }

  return visited_order;
}

// Read neighbor from cached page.
std::vector<v_id_t>
bw_graph_db_t::read_neighbor_from_cached_page(v_id_t vertex_id,
                                              std::shared_ptr<csr_page_t> cached_page) {
  std::vector<v_id_t> result_neighbors;

  if (this->vertex_index->items[vertex_id].vertex_type == GIANT) {
    return this->giant_db->read_neighbor_clone(vertex_id);
  }

  uint64_t offset = this->vertex_index->get_vertex_location(vertex_id).second;
  auto neighbors_span = cached_page->get_neighbors(offset);
  result_neighbors.assign(neighbors_span.begin(), neighbors_span.end());
  return result_neighbors;
}

// Thread-safe BFS block implementation
std::vector<std::pair<v_id_t, uint64_t>>
bw_graph_db_t::bfs_block_inner_parallel(v_id_t start_vertex, csr_page_t* block,
                                        tbb::concurrent_vector<v_id_t>& outside,
                                        std::vector<bool>& visited, std::mutex& visited_mutex) {
  std::vector<std::pair<v_id_t, uint64_t>> vertex_order;
  uint64_t num_vertices_ = block->get_num_vertices();
  page_no_t current_page_no = block->get_page_no();

  // Early return for empty page
  if (num_vertices_ == 0) {
    return vertex_order;
  }

  vertex_loc_t start_loc = this->vertex_index->get_vertex_location(start_vertex);
  if (start_loc.second >= num_vertices_ ||
      block->get_vertices()[start_loc.second].vertex_id != start_vertex) {
    return vertex_order;
  }

  // Pre-allocate result vector to avoid frequent reallocations
  vertex_order.reserve(num_vertices_);

  // Use two vectors to implement level-by-level BFS (more cache-friendly)
  std::vector<uint64_t> current_level;
  std::vector<uint64_t> next_level;

  // Reserve space to reduce allocations - adaptive sizing
  uint64_t reserve_size = std::min(static_cast<uint64_t>(1024), num_vertices_);
  current_level.reserve(reserve_size);
  next_level.reserve(reserve_size);

  // Initialize BFS with thread-safe visited check
  {
    std::lock_guard<std::mutex> lock(visited_mutex);
    if (visited[start_vertex]) {
      return vertex_order;
    }
    visited[start_vertex] = true;
  }

  current_level.push_back(start_loc.second);
  vertex_order.push_back({start_vertex, 0});

  uint64_t current_level_num = 0;

  // Level-by-level BFS traversal
  while (!current_level.empty()) {
    // Process all vertices in current level
    for (uint64_t current_offset : current_level) {
      // Read neighbors from block
      auto neighbor_span = block->get_neighbors(current_offset);

      // Collect neighbors that need processing
      std::vector<v_id_t> neighbors_to_process;

      // For each neighbor
      for (v_id_t neighbor_id : neighbor_span) {
        uint64_t neighbor_offset = this->vertex_index->get_vertex_location(neighbor_id).second;

        // Check if neighbor is an outside vertex
        if (this->vertex_index->get_vertex_location(neighbor_id).first != current_page_no) {
          outside.push_back(neighbor_id);
        } else if (neighbor_offset < num_vertices_ &&
                   block->get_vertices()[neighbor_offset].vertex_id == neighbor_id) {
          neighbors_to_process.push_back(neighbor_id);
        }
      }

      // Thread-safe processing of valid neighbors
      {
        std::lock_guard<std::mutex> lock(visited_mutex);
        for (v_id_t neighbor_id : neighbors_to_process) {
          if (!visited[neighbor_id]) {
            visited[neighbor_id] = true;
            vertex_order.push_back({neighbor_id, current_level_num + 1});
            uint64_t neighbor_offset = this->vertex_index->get_vertex_location(neighbor_id).second;
            next_level.push_back(neighbor_offset);
          }
        }
      }
    }

    // Move to next level efficiently
    current_level.clear();
    current_level.swap(next_level);
    current_level_num++;
  }

  return vertex_order;
}

// Parallel block-based BFS implementation
std::vector<std::pair<v_id_t, uint64_t>> bw_graph_db_t::bfs_block_parallel(v_id_t start_vertex) {
  csr_page_t* start_page = this->read_page_vertex(start_vertex);
  uint64_t vertex_count = this->get_vertex_count();

  // Thread-safe containers
  tbb::concurrent_vector<std::pair<v_id_t, uint64_t>> visited_order;
  tbb::concurrent_queue<std::pair<v_id_t, page_no_t>> block_queue;
  tbb::concurrent_hash_map<page_no_t, bool> visited_pages;

  // Initialize queue and visited tracking
  block_queue.push({start_vertex, start_page->get_page_no()});
  std::vector<bool> visited(vertex_count, false);
  std::mutex visited_mutex;

  // Atomic counter for active workers
  std::atomic<int> active_workers(0);
  std::atomic<bool> should_continue(true);

  // Task group for parallel execution
  tbb::task_group task_group;

  // Worker function for processing blocks
  auto worker = [&]() {
    active_workers.fetch_add(1);

    while (should_continue.load()) {
      std::pair<v_id_t, page_no_t> work_item;

      // Try to get work from queue
      if (!block_queue.try_pop(work_item)) {
        // Check if we should terminate
        if (active_workers.load() == 1) {
          // Last worker, check if queue is truly empty
          std::this_thread::sleep_for(std::chrono::microseconds(10));
          if (!block_queue.try_pop(work_item)) {
            should_continue.store(false);
            break;
          }
        } else {
          // Wait a bit and try again
          std::this_thread::sleep_for(std::chrono::microseconds(10));
          continue;
        }
      }

      v_id_t current_vertex = work_item.first;
      page_no_t current_page_no = work_item.second;

      // Check if page already visited
      tbb::concurrent_hash_map<page_no_t, bool>::accessor accessor;
      if (visited_pages.find(accessor, current_page_no)) {
        continue; // Page already processed
      }

      // Mark page as being processed
      if (!visited_pages.insert(accessor, current_page_no)) {
        continue; // Another thread got here first
      }
      accessor->second = true;
      accessor.release();

      // Read page and process
      auto current_block = this->buffer_pool->buf_page_read(current_page_no, this->disk_manager);
      tbb::concurrent_vector<v_id_t> outside;

      // Perform BFS on the current block
      std::vector<std::pair<v_id_t, uint64_t>> block_visited = this->bfs_block_inner_parallel(
          current_vertex, current_block, outside, visited, visited_mutex);

      // Add results to global visited order
      for (const auto& vertex_pair : block_visited) {
        visited_order.push_back(vertex_pair);
      }

      // Process outside vertices and add new pages to queue
      std::unordered_map<page_no_t, v_id_t> outside_map;

      for (v_id_t neighbor_id : outside) {
        auto v_loc = this->vertex_index->get_vertex_location(neighbor_id);

        // Thread-safe check for visited vertex and page
        bool skip = false;
        {
          std::lock_guard<std::mutex> lock(visited_mutex);
          if (visited[neighbor_id]) {
            skip = true;
          }
        }

        if (skip)
          continue;

        // Check if page already visited or queued
        tbb::concurrent_hash_map<page_no_t, bool>::const_accessor const_accessor;
        if (visited_pages.find(const_accessor, v_loc.first)) {
          continue;
        }

        // Add to local map (no concurrent access needed here)
        if (outside_map.find(v_loc.first) == outside_map.end()) {
          outside_map[v_loc.first] = neighbor_id;
        }
      }

      // Add new work to queue
      for (const auto& [page_no, neighbor_id] : outside_map) {
        block_queue.push({neighbor_id, page_no});
      }
    }

    active_workers.fetch_sub(1);
  };

  // Start parallel workers
  const int num_workers = std::min(static_cast<int>(std::thread::hardware_concurrency()), 8);
  for (int i = 0; i < num_workers; ++i) {
    task_group.run(worker);
  }

  // Wait for all workers to complete
  task_group.wait();

  // Convert concurrent_vector to regular vector and sort by level
  std::vector<std::pair<v_id_t, uint64_t>> result;
  result.reserve(visited_order.size());

  for (const auto& vertex_pair : visited_order) {
    result.push_back(vertex_pair);
  }

  // Sort by BFS level to maintain proper ordering
  std::sort(
      // Begin begin.
      result.begin(), result.end(),
      [](const std::pair<v_id_t, uint64_t>& a, const std::pair<v_id_t, uint64_t>& b) {
        return a.second < b.second;
      });

  return result;
}

// Run process page bfs.
void bw_graph_db_t::process_page_bfs(page_no_t page_no, const std::vector<v_id_t>& page_vertices,
                                     uint64_t current_level, concurrent_bit_map& visited,
                                     std::unique_ptr<std::atomic<uint64_t>[]>& levels,
                                     tbb::concurrent_vector<v_id_t>& next_frontier) {
  // Special handling for giant vertices
  if (page_no == GIANT_PAGE_NO) {
    process_giant_vertices(page_vertices, current_level, visited, levels, next_frontier);
    return;
  }

  // Step 1: Load the page from buffer pool
  csr_page_t* page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);

  // Ensure page is correctly parsed
  if (!page) {
    return;
  }

  // parallel
  if (page_vertices.size() < 16) {
    // Serial processing (to avoid parallel overhead)
    for (v_id_t vertex_id : page_vertices) {
      process_vertex_edges(vertex_id, page, current_level, visited, levels, next_frontier);
    }
  } else {
    // Parallel processing using TBB
    tbb::parallel_for(tbb::blocked_range<size_t>(0, page_vertices.size()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i != range.end(); ++i) {
                          process_vertex_edges(page_vertices[i], page, current_level, visited,
                                               levels, next_frontier);
                        }
                      });
  }

  // Step 3: Release page
  page->r_unlatch();
}

// Process vertex edges.
void bw_graph_db_t::process_vertex_edges(v_id_t vertex_id, csr_page_t* page, uint64_t current_level,
                                         concurrent_bit_map& visited,
                                         std::unique_ptr<std::atomic<uint64_t>[]>& levels,
                                         tbb::concurrent_vector<v_id_t>& next_frontier) {
  // Boundary check
  uint64_t vertex_offset = this->vertex_index->get_vertex_location(vertex_id).second;

  // Get neighbors
  neighbor_span_t neighbors = page->get_neighbors(vertex_offset);

  // For each neighbor
  for (v_id_t neighbor : neighbors) {
    // Boundary check
    if (neighbor >= visited.size()) {
      continue;
    }

    // Attempt to mark neighbor as visited
    if (!visited.set(neighbor, std::memory_order_relaxed)) {
      // First time visiting this neighbor
      levels[neighbor].store(current_level + 1, std::memory_order_relaxed);
      next_frontier.push_back(neighbor);
    }
  }
}

// Process giant vertices.
void bw_graph_db_t::process_giant_vertices(const std::vector<v_id_t>& giant_vertices,
                                           uint64_t current_level, concurrent_bit_map& visited,
                                           std::unique_ptr<std::atomic<uint64_t>[]>& levels,
                                           tbb::concurrent_vector<v_id_t>& next_frontier) {
  // Parallel processing of giant vertices
  tbb::parallel_for(
      tbb::blocked_range<size_t>(0, giant_vertices.size()),
      [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
          v_id_t giant_vertex = giant_vertices[i];

          // Read neighbor from giant vertex db
          std::vector<v_id_t> neighbors = this->giant_db->read_neighbor_clone(giant_vertex);

          // Process each neighbor
          for (v_id_t neighbor : neighbors) {
            if (neighbor == 0 || neighbor >= visited.size()) {
              continue;
            }
            if (neighbor < visited.size() && !visited.set(neighbor, std::memory_order_relaxed)) {
              levels[neighbor].store(current_level + 1, std::memory_order_relaxed);
              next_frontier.push_back(neighbor);
            }
          }
        }
      });
}

// Run parallel wcc.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::parallel_wcc() {
  // Allocate bitmaps for active vertex tracking
  bit_map_t* active_in = new bit_map_t(this->get_vertex_count());
  bit_map_t* active_out = new bit_map_t(this->get_vertex_count());

  // Label array: each vertex initially labeled with its own ID
  std::vector<v_id_t> label(this->get_vertex_count());

  // Initialize: all vertices are active, labeled with their own ID
  active_out->fill();
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    label[v] = v;
  }

  uint32_t active_vertices = this->get_vertex_count();
  double start_time = get_time();

  // Iterative label propagation
  while (active_vertices != 0) {
    std::swap(active_in, active_out);
    active_out->clear();

    // Propagate labels: each vertex tries to take minimum neighbor label
    active_vertices = bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          // If source has smaller label, try to update target
          if (label[src] < label[dst]) {
            // Atomically update dst's label to minimum
            v_id_t old_label = label[dst];
            while (label[src] < old_label) {
              if (__sync_bool_compare_and_swap(&label[dst], old_label, label[src])) {
                // Successfully updated: mark dst as active for next iteration
                active_out->set_bit(dst);
                return 1;
              }
              // CAS failed, reload current value
              old_label = label[dst];
            }
          }
          return 0;
        },
        active_in);
  }

  double end_time = get_time();

  // Count component sizes
  std::vector<uint32_t> component_size(this->get_vertex_count(), 0);
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    __sync_fetch_and_add(&component_size[label[v]], 1);
  }

  // Count number of components (non-zero sizes)
  uint32_t num_components = 0;
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    if (component_size[v] > 0) {
      num_components++;
    }
  }

  printf("%u components found in %.2f seconds\n", num_components, end_time - start_time);

  // Build results: all vertices with their component labels
  std::vector<std::pair<v_id_t, v_id_t>> results;
  results.reserve(this->get_vertex_count());

  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    results.emplace_back(v, label[v]);
  }

  // Cleanup
  delete active_in;
  delete active_out;

  return results;
}

// Run process page wcc.
void bw_graph_db_t::process_page_wcc(page_no_t page_no, const std::vector<v_id_t>& page_vertices,
                                     std::unique_ptr<std::atomic<v_id_t>[]>& components,
                                     std::atomic<bool>& changed) {
  // Special handling for giant vertices
  if (page_no == GIANT_PAGE_NO) {
    process_giant_vertices_wcc(page_vertices, components, changed);
    return;
  }

  // Step 1: Load the page from buffer pool
  csr_page_t* page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);

  if (!page) {
    return;
  }

  // Step 2: Parallel processing of vertices within the page
  if (page_vertices.size() < 16) {
    // Serial processing
    for (v_id_t vertex_id : page_vertices) {
      process_vertex_wcc(vertex_id, page, components, changed);
    }
  } else {
    // Parallel processing using TBB
    tbb::parallel_for(tbb::blocked_range<size_t>(0, page_vertices.size()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i != range.end(); ++i) {
                          process_vertex_wcc(page_vertices[i], page, components, changed);
                        }
                      });
  }

  // Step 3: Release page
  page->r_unlatch();
}

// Run process giant vertices wcc.
void bw_graph_db_t::process_giant_vertices_wcc(const std::vector<v_id_t>& giant_vertices,
                                               std::unique_ptr<std::atomic<v_id_t>[]>& components,
                                               std::atomic<bool>& changed) {
  tbb::parallel_for(
      tbb::blocked_range<size_t>(0, giant_vertices.size()),
      [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
          v_id_t giant_vertex = giant_vertices[i];
          v_id_t my_component = components[giant_vertex].load(std::memory_order_relaxed);

          // Read neighbors from giant vertex db
          std::vector<v_id_t> neighbors = this->giant_db->read_neighbor_clone(giant_vertex);

          // Process each neighbor
          for (v_id_t neighbor : neighbors) {
            // Boundary check
            if (neighbor == 0 || neighbor >= this->vertex_index->items.size()) {
              continue;
            }

            v_id_t neighbor_component = components[neighbor].load(std::memory_order_relaxed);

            // Propagate minimum component ID
            if (my_component < neighbor_component) {
              v_id_t expected = neighbor_component;
              if (components[neighbor].compare_exchange_strong(expected, my_component,
                                                               std::memory_order_relaxed)) {
                changed.store(true, std::memory_order_relaxed);
              }
            } else if (neighbor_component < my_component) {
              v_id_t expected = my_component;
              if (components[giant_vertex].compare_exchange_strong(expected, neighbor_component,
                                                                   std::memory_order_relaxed)) {
                my_component = neighbor_component;
                changed.store(true, std::memory_order_relaxed);
              }
            }
          }
        }
      });
}

// Run parallel pagerank.
std::vector<std::pair<v_id_t, float>>
bw_graph_db_t::parallel_pagerank(uint64_t max_iterations, float damping_factor,
                                 float convergence_threshold) {
  printf("Initializing PageRank...\n");

  // Initialize PageRank values
  std::vector<float> pagerank(this->get_vertex_count());
  std::vector<float> sum(this->get_vertex_count());

  // Initialize: PR(v) = 1/degree(v) (GridGraph style)
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    uint32_t deg = this->get_vertex_degree(v);
    pagerank[v] = (deg > 0) ? (1.0f / deg) : 0.0f;
    sum[v] = 0.0f;
  }

  // Iterative computation
  double start_time = get_time();
  for (uint64_t iter = 0; iter < max_iterations; iter++) {
    // Propagate ranks through edges
    bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          // Contribute pagerank[src] to sum[dst]
          write_add(&sum[dst], pagerank[src]);
          return 0;
        },
        nullptr); // No bitmap - scan all edges

    // Update pagerank and compute delta
    float delta = 0.0f;

    if (iter == max_iterations - 1) {
      // Last iteration: final pagerank without degree normalization
      for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
        float new_pr = (1.0f - damping_factor) + damping_factor * sum[v];
        delta += std::abs(new_pr - pagerank[v] * this->get_vertex_degree(v));
        pagerank[v] = new_pr;
      }
    } else {
      // Regular iteration: normalize by degree for next round
      for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
        uint32_t deg = this->get_vertex_degree(v);
        if (deg > 0) {
          float new_pr = ((1.0f - damping_factor) + damping_factor * sum[v]) / deg;
          delta += std::abs(new_pr * deg - pagerank[v] * deg);
          pagerank[v] = new_pr;
        }
        sum[v] = 0.0f; // Reset for next iteration
      }
    }

    // Check convergence
    if (convergence_threshold > 0 && delta < convergence_threshold) {
      printf("Converged after %" PRIu64 " iterations\n", iter + 1);
      break;
    }
  }

  double end_time = get_time();
  printf("PageRank computation took %.2f seconds\n", end_time - start_time);

  // Build results
  std::vector<std::pair<v_id_t, float>> results;
  results.reserve(this->get_vertex_count());

  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    results.emplace_back(v, pagerank[v]);
  }

  return results;
}

// Run pagerank.
std::vector<std::pair<v_id_t, double>> bw_graph_db_t::pagerank(uint64_t max_iterations,
                                                               double damping_factor,
                                                               double convergence_threshold) {
  uint64_t vertex_count = this->get_vertex_count();
  if (vertex_count == 0) {
    std::cout << "PageRank: vertex_count=0" << std::endl;
    return {};
  }

  double initial_rank = 1.0 / vertex_count;

  // Precompute out-degree and 1/out-degree for each vertex
  std::vector<uint64_t> out_degree(vertex_count, 0);
  std::vector<double> inv_degree(vertex_count, 0.0);
  size_t dangling_count = 0;

  for (v_id_t v = 0; v < vertex_count; ++v) {
    out_degree[v] = this->get_vertex_degree(v);
    if (out_degree[v] == 0) {
      dangling_count++;
    } else {
      inv_degree[v] = 1.0 / out_degree[v];
    }
  }

  // Initialize rank arrays - using vector for better cache performance
  std::vector<double> rank(vertex_count, initial_rank);
  std::vector<double> new_rank(vertex_count, 0.0);

  uint64_t iteration = 0;
  double delta = 0.0;

  // Iterate until convergence or max iterations
  while (iteration < max_iterations) {
    // Compute dangling node contribution (vertices with out-degree 0)
    double dangling_sum = 0.0;
    for (v_id_t v = 0; v < vertex_count; ++v) {
      if (out_degree[v] == 0) {
        dangling_sum += rank[v];
      }
    }

    double teleport = (1.0 - damping_factor) / vertex_count;
    double dangling_contrib = damping_factor * dangling_sum / vertex_count;

    // Pull model: each vertex pulls rank from incoming edges
    for (v_id_t v = 0; v < vertex_count; ++v) {
      double sum = 0.0;

      // Get incoming neighbors (vertices pointing to v)
      auto [neighbors_span, current_page] = read_neighbor(v);

      for (const v_id_t& neighbor : neighbors_span) {
        if (neighbor < vertex_count && out_degree[neighbor] > 0) {
          sum += rank[neighbor] * inv_degree[neighbor];
        }
      }

      if (current_page != nullptr) {
        current_page->r_unlatch();
      }

      new_rank[v] = teleport + dangling_contrib + damping_factor * sum;
    }

    // Compute convergence metric delta
    delta = 0.0;
    for (v_id_t v = 0; v < vertex_count; ++v) {
      delta += std::abs(new_rank[v] - rank[v]);
    }

    // Swap rank and new_rank for next iteration
    rank.swap(new_rank);

    iteration++;

    // Check convergence
    if (delta < convergence_threshold) {
      std::cout << "PageRank converged after " << iteration << " iterations (delta: " << delta
                << ")" << std::endl;
      break;
    }
  }

  // Compute statistics
  double sum_rank = 0.0;
  double max_rank = -1.0, min_rank = 1e100;
  v_id_t max_v = 0, min_v = 0;

  for (v_id_t v = 0; v < vertex_count; ++v) {
    double r = rank[v];
    sum_rank += r;
    if (r > max_rank) {
      max_rank = r;
      max_v = v;
    }
    if (r < min_rank) {
      min_rank = r;
      min_v = v;
    }
  }

  double avg_rank = sum_rank / vertex_count;
  double diff = std::abs(sum_rank - 1.0);

  std::cout << std::fixed << std::setprecision(6) << "PageRank stats: vertices=" << vertex_count
            << ", iterations=" << iteration << ", dangling_nodes=" << dangling_count
            << "\n  sum_rank=" << sum_rank << ", avg_rank=" << avg_rank << ", error=" << diff
            << "\n  max_rank=" << max_rank << " (vertex " << max_v << ")"
            << ", min_rank=" << min_rank << " (vertex " << min_v << ")" << std::endl;

  // Build result: (vertex_id, rank)
  std::vector<std::pair<v_id_t, double>> result;
  result.reserve(vertex_count);

  for (v_id_t vertex = 0; vertex < vertex_count; ++vertex) {
    result.emplace_back(vertex, rank[vertex]);
  }

  return result;
}

// Run process page pagerank.
void bw_graph_db_t::process_page_pagerank(page_no_t page_no,
                                          const std::vector<v_id_t>& page_vertices,
                                          std::unique_ptr<std::atomic<double>[]>& rank_current,
                                          std::unique_ptr<std::atomic<double>[]>& rank_next) {
  // Special handling for giant vertices
  if (page_no == GIANT_PAGE_NO) {
    process_giant_vertices_pagerank(page_vertices, rank_current, rank_next);
    return;
  }

  // Step 1: Load the page from buffer pool
  csr_page_t* page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);

  if (!page) {
    return;
  }

  // Step 2: Parallel processing of vertices within the page
  if (page_vertices.size() < 16) {
    // Serial processing for small pages
    for (v_id_t vertex_id : page_vertices) {
      process_vertex_pagerank(vertex_id, page, rank_current, rank_next);
    }
  } else {
    // Parallel processing using TBB
    tbb::parallel_for(tbb::blocked_range<size_t>(0, page_vertices.size()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i != range.end(); ++i) {
                          process_vertex_pagerank(page_vertices[i], page, rank_current, rank_next);
                        }
                      });
  }

  // Step 3: Release page
  page->r_unlatch();
}

// Run process vertex pagerank.
void bw_graph_db_t::process_vertex_pagerank(v_id_t vertex_id, csr_page_t* page,
                                            std::unique_ptr<std::atomic<double>[]>& rank_current,
                                            std::unique_ptr<std::atomic<double>[]>& rank_next) {
  uint64_t vertex_offset = this->vertex_index->get_vertex_location(vertex_id).second;
  uint64_t degree = this->get_vertex_degree(vertex_id);

  // Skip vertices with no edges (handled in dangling node computation)
  if (degree == 0) {
    return;
  }

  double my_rank = rank_current[vertex_id].load(std::memory_order_relaxed);
  double rank_contribution = my_rank / degree;

  // Get neighbors and distribute rank
  neighbor_span_t neighbors = page->get_neighbors(vertex_offset);

  for (v_id_t neighbor : neighbors) {
    // Boundary check
    if (neighbor >= this->vertex_index->items.size()) {
      continue;
    }

    // Atomic add to neighbor's next rank
    // For undirected graph, each edge contributes rank in both directions
    double old_rank = rank_next[neighbor].load(std::memory_order_relaxed);
    while (!rank_next[neighbor].compare_exchange_weak(old_rank, old_rank + rank_contribution,
                                                      std::memory_order_relaxed)) {
      // CAS retry loop
    }
  }
}

// Run process giant vertices pagerank.
void bw_graph_db_t::process_giant_vertices_pagerank(
    const std::vector<v_id_t>& giant_vertices, std::unique_ptr<std::atomic<double>[]>& rank_current,
    std::unique_ptr<std::atomic<double>[]>& rank_next) {
  tbb::parallel_for(tbb::blocked_range<size_t>(0, giant_vertices.size()),
                    [&](const tbb::blocked_range<size_t>& range) {
                      for (size_t i = range.begin(); i != range.end(); ++i) {
                        v_id_t giant_vertex = giant_vertices[i];
                        uint64_t degree = this->get_vertex_degree(giant_vertex);

                        // Skip if no edges
                        if (degree == 0) {
                          continue;
                        }

                        double my_rank = rank_current[giant_vertex].load(std::memory_order_relaxed);
                        double rank_contribution = my_rank / degree;

                        // Read neighbors from giant vertex db
                        std::vector<v_id_t> neighbors =
                            this->giant_db->read_neighbor_clone(giant_vertex);

                        // Distribute rank to each neighbor
                        for (v_id_t neighbor : neighbors) {
                          // Boundary check
                          if (neighbor == 0 || neighbor >= this->vertex_index->items.size()) {
                            continue;
                          }

                          // Atomic add to neighbor's next rank
                          // For undirected graph, each edge contributes rank in both
                          // directions
                          double old_rank = rank_next[neighbor].load(std::memory_order_relaxed);
                          while (!rank_next[neighbor].compare_exchange_weak(
                              old_rank, old_rank + rank_contribution, std::memory_order_relaxed)) {
                            // CAS retry loop
                          }
                        }
                      }
                    });
}

// Run process vertex wcc.
void bw_graph_db_t::process_vertex_wcc(v_id_t vertex_id, csr_page_t* page,
                                       std::unique_ptr<std::atomic<v_id_t>[]>& components,
                                       std::atomic<bool>& changed) {
  uint64_t vertex_offset = this->vertex_index->get_vertex_location(vertex_id).second;
  v_id_t my_component = components[vertex_id].load(std::memory_order_relaxed);

  // Get neighbors
  neighbor_span_t neighbors = page->get_neighbors(vertex_offset);

  // Propagate minimum component ID
  for (v_id_t neighbor : neighbors) {
    // Boundary check
    if (neighbor >= this->vertex_index->items.size()) {
      continue;
    }

    v_id_t neighbor_component = components[neighbor].load(std::memory_order_relaxed);

    // Try to propagate smaller component ID to neighbor
    if (my_component < neighbor_component) {
      v_id_t expected = neighbor_component;
      if (components[neighbor].compare_exchange_strong(expected, my_component,
                                                       std::memory_order_relaxed)) {
        changed.store(true, std::memory_order_relaxed);
      }
    }
    // Try to update my component ID if neighbor has smaller ID
    else if (neighbor_component < my_component) {
      v_id_t expected = my_component;
      if (components[vertex_id].compare_exchange_strong(expected, neighbor_component,
                                                        std::memory_order_relaxed)) {
        my_component = neighbor_component;
        changed.store(true, std::memory_order_relaxed);
      }
    }
  }
}

// CDLP algorithm
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::cdlp(uint64_t max_iterations,
                                                           bool use_random_order) {
  uint64_t vertex_count = this->get_vertex_count();

  // Initialize labels
  std::vector<v_id_t> labels(vertex_count);
  std::vector<v_id_t> new_labels(vertex_count);
  for (v_id_t i = 0; i < vertex_count; ++i) {
    labels[i] = i;
    new_labels[i] = i;
  }

  // Create vertex processing order
  std::vector<v_id_t> vertex_order(vertex_count);
  std::iota(vertex_order.begin(), vertex_order.end(), 0);

  // Global label count array - reuse across all vertices
  // This is the KEY optimization - similar to PageRank's approach
  std::vector<uint64_t> label_count(vertex_count, 0);
  std::vector<v_id_t> touched_labels; // Track which labels we modified
  touched_labels.reserve(10000);

  std::random_device rd;
  std::mt19937 rng(rd());

  uint64_t iteration = 0;
  bool changed = true;

  while (iteration < max_iterations && changed) {
    changed = false;

    if (use_random_order) {
      std::shuffle(vertex_order.begin(), vertex_order.end(), rng);
    }

    for (v_id_t vertex_id : vertex_order) {
      auto [neighbors_span, current_page] = read_neighbor(vertex_id);

      if (neighbors_span.empty()) {
        if (current_page != nullptr) {
          current_page->r_unlatch();
        }
        new_labels[vertex_id] = labels[vertex_id];
        continue;
      }

      // Clear touched labels (only reset what we used)
      for (v_id_t label : touched_labels) {
        label_count[label] = 0;
      }
      touched_labels.clear();

      // Count label frequencies - direct array access like PageRank
      for (const v_id_t& neighbor : neighbors_span) {
        if (neighbor < vertex_count) {
          v_id_t neighbor_label = labels[neighbor];
          if (label_count[neighbor_label] == 0) {
            touched_labels.push_back(neighbor_label);
          }
          label_count[neighbor_label]++;
        }
      }

      if (current_page != nullptr) {
        current_page->r_unlatch();
      }

      // Find most frequent label - single pass
      v_id_t new_label = labels[vertex_id];
      uint64_t max_count = 0;

      for (v_id_t label : touched_labels) {
        uint64_t count = label_count[label];
        if (count > max_count || (count == max_count && label < new_label)) {
          max_count = count;
          new_label = label;
        }
      }

      new_labels[vertex_id] = new_label;
      if (new_label != labels[vertex_id]) {
        changed = true;
      }
    }

    std::swap(labels, new_labels);
    iteration++;
  }

  if (changed) {
    std::cout << "CDLP reached max iterations (" << max_iterations << ") without full convergence"
              << std::endl;
  } else {
    std::cout << "CDLP converged after " << iteration << " iterations" << std::endl;
  }

  std::vector<std::pair<v_id_t, v_id_t>> result;
  result.reserve(vertex_count);

  for (v_id_t vertex = 0; vertex < vertex_count; ++vertex) {
    result.emplace_back(vertex, labels[vertex]);
  }

  return result;
}

// Find most frequent label.
v_id_t
bw_graph_db_t::find_most_frequent_label(const std::unordered_map<v_id_t, uint64_t>& label_count) {
  // Find maximum frequency
  uint64_t max_count = 0;
  for (const auto& [label, count] : label_count) {
    if (count > max_count) {
      max_count = count;
    }
  }

  // Collect all labels with maximum frequency
  std::vector<v_id_t> max_labels;
  for (const auto& [label, count] : label_count) {
    if (count == max_count) {
      max_labels.push_back(label);
    }
  }

  // If tie, randomly select one (or choose minimum for deterministic
  // behavior)
  if (max_labels.size() == 1) {
    return max_labels[0];
  }

  // Break ties by selecting the minimum label (deterministic)
  return *std::min_element(max_labels.begin(), max_labels.end());

  // Alternative: Random tie-breaking (uncomment if preferred)
  /*
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<size_t> dist(0, max_labels.size() - 1);
  return max_labels[dist(rng)];
  */
}

// Run parallel cdlp.
std::vector<std::pair<v_id_t, v_id_t>> bw_graph_db_t::parallel_cdlp(uint64_t max_iterations) {
  printf("Initializing LPA...\n");

  // Allocate bitmaps for active vertex tracking
  bit_map_t* active_in = new bit_map_t(this->get_vertex_count());
  bit_map_t* active_out = new bit_map_t(this->get_vertex_count());

  // Label array: each vertex initially labeled with its own ID
  std::vector<v_id_t> label(this->get_vertex_count());

  // Initialize: all vertices are active, labeled with their own ID
  active_out->fill();
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    label[v] = v;
  }

  uint32_t active_vertices = this->get_vertex_count();
  double start_time = get_time();
  // Main iteration loop
  for (uint64_t iter = 0; iter < max_iterations; iter++) {
    if (active_vertices == 0) {
      printf("Converged at iteration %" PRIu64 "\n", iter);
      break;
    }

    std::swap(active_in, active_out);
    active_out->clear();

    // Stream edges and propagate labels
    // GridGraph style: propagate smaller labels (like WCC)
    active_vertices = bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          // Propagate smaller label from source to target
          if (label[src] < label[dst]) {
            // Atomically update target's label to minimum
            v_id_t old_label = label[dst];
            while (label[src] < old_label) {
              if (__sync_bool_compare_and_swap(&label[dst], old_label, label[src])) {
                active_out->set_bit(dst);
                return 1;
              }
              old_label = label[dst];
            }
          }
          return 0;
        },
        active_in);
  }

  double end_time = get_time();
  printf("%" PRIu64 " iterations of LPA took %.2f seconds\n", max_iterations,
         end_time - start_time);

  std::vector<uint32_t> label_stat(this->get_vertex_count(), 0);
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    __sync_fetch_and_add(&label_stat[label[v]], 1);
  }

  uint32_t communities = 0;
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    if (label_stat[v] != 0) {
      communities++;
    }
  }

  printf("Detected %u communities\n", communities);

  // Build results
  std::vector<std::pair<v_id_t, v_id_t>> results;
  results.reserve(this->get_vertex_count());

  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    results.emplace_back(v, label[v]);
  }

  // Cleanup
  delete active_in;
  delete active_out;

  return results;
}

// Run process page cdlp optimized.
void bw_graph_db_t::process_page_cdlp_optimized(page_no_t page_no,
                                                const std::vector<v_id_t>& page_vertices,
                                                std::unique_ptr<std::atomic<v_id_t>[]>& labels,
                                                std::atomic<bool>& changed,
                                                std::vector<uint64_t>& label_count,
                                                std::vector<v_id_t>& touched_labels) {
  // Special handling for giant vertices
  if (page_no == GIANT_PAGE_NO) {
    process_giant_vertices_cdlp_optimized(page_vertices, labels, changed, label_count,
                                          touched_labels);
    return;
  }

  // Load the page from buffer pool
  csr_page_t* page = this->buffer_pool->buf_page_read(page_no, this->disk_manager);

  if (!page) {
    return;
  }

  // Process all vertices in this page serially
  // (Avoid nested parallelism overhead)
  for (v_id_t vertex_id : page_vertices) {
    process_vertex_cdlp_optimized(vertex_id, page, labels, changed, label_count, touched_labels);
  }

  // Release page
  page->r_unlatch();
}

// Run process vertex cdlp optimized.
void bw_graph_db_t::process_vertex_cdlp_optimized(v_id_t vertex_id, csr_page_t* page,
                                                  std::unique_ptr<std::atomic<v_id_t>[]>& labels,
                                                  std::atomic<bool>& changed,
                                                  std::vector<uint64_t>& label_count,
                                                  std::vector<v_id_t>& touched_labels) {
  uint64_t vertex_offset = this->vertex_index->get_vertex_location(vertex_id).second;
  v_id_t my_label = labels[vertex_id].load(std::memory_order_relaxed);

  // Get neighbors
  neighbor_span_t neighbors = page->get_neighbors(vertex_offset);

  if (neighbors.empty()) {
    return;
  }

  // Clear touched labels (only reset what we used)
  for (v_id_t label : touched_labels) {
    label_count[label] = 0;
  }
  touched_labels.clear();

  // Count label frequencies using direct array access
  for (v_id_t neighbor : neighbors) {
    if (neighbor >= this->vertex_index->items.size()) {
      continue;
    }

    v_id_t neighbor_label = labels[neighbor].load(std::memory_order_relaxed);
    if (label_count[neighbor_label] == 0) {
      touched_labels.push_back(neighbor_label);
    }
    label_count[neighbor_label]++;
  }

  // Find most frequent label in single pass
  v_id_t new_label = my_label;
  uint64_t max_count = 0;

  for (v_id_t label : touched_labels) {
    uint64_t count = label_count[label];
    if (count > max_count || (count == max_count && label < new_label)) {
      max_count = count;
      new_label = label;
    }
  }

  // Update label if it changed
  if (new_label != my_label) {
    labels[vertex_id].store(new_label, std::memory_order_relaxed);
    changed.store(true, std::memory_order_relaxed);
  }
}

// Run process giant vertices cdlp optimized.
void bw_graph_db_t::process_giant_vertices_cdlp_optimized(
    const std::vector<v_id_t>& giant_vertices, std::unique_ptr<std::atomic<v_id_t>[]>& labels,
    std::atomic<bool>& changed, std::vector<uint64_t>& label_count,
    std::vector<v_id_t>& touched_labels) {
  // Process giant vertices serially within this thread
  // (They're already being processed in parallel at page level)
  for (v_id_t giant_vertex : giant_vertices) {
    v_id_t my_label = labels[giant_vertex].load(std::memory_order_relaxed);

    // Read neighbors from giant vertex db
    std::vector<v_id_t> neighbors = this->giant_db->read_neighbor_clone(giant_vertex);

    if (neighbors.empty()) {
      continue;
    }

    // Clear touched labels
    for (v_id_t label : touched_labels) {
      label_count[label] = 0;
    }
    touched_labels.clear();

    // Count label frequencies
    for (v_id_t neighbor : neighbors) {
      if (neighbor == 0 || neighbor >= this->vertex_index->items.size()) {
        continue;
      }

      v_id_t neighbor_label = labels[neighbor].load(std::memory_order_relaxed);
      if (label_count[neighbor_label] == 0) {
        touched_labels.push_back(neighbor_label);
      }
      label_count[neighbor_label]++;
    }

    // Find most frequent label
    v_id_t new_label = my_label;
    uint64_t max_count = 0;

    for (v_id_t label : touched_labels) {
      uint64_t count = label_count[label];
      if (count > max_count || (count == max_count && label < new_label)) {
        max_count = count;
        new_label = label;
      }
    }

    // Update label if it changed
    if (new_label != my_label) {
      labels[giant_vertex].store(new_label, std::memory_order_relaxed);
      changed.store(true, std::memory_order_relaxed);
    }
  }
}

// Graph Sampling
std::shared_ptr<mem_sub_map_t> bw_graph_db_t::graph_sample_page(v_id_t start_vertex) {
  mem_sub_map_t sample;
  // Step 1: Locate the page;
  protected_vertex_version_t version =
      this->vertex_index->protect_vertex_version(start_vertex);
  auto [page, offset] = version.location();
  csr_page_t* current_page = this->buffer_pool->buf_page_read(page, this->disk_manager);
  version.reset();

  // Transport this csr page into memory subgraph;
  sample = mem_sub_map_t::from_csr_page(*current_page);

  return std::make_shared<mem_sub_map_t>(sample);
}

// Compute degree distribution with specified step size
std::vector<std::pair<uint64_t, double>> bw_graph_db_t::compute_degree_distribution(uint64_t step) {
  if (step == 0) {
    return {};
  }

  // Get total vertex count
  uint64_t vertex_count = this->get_vertex_count();

  if (vertex_count == 0) {
    return {};
  }

  // Find maximum degree and count degrees
  uint64_t max_degree = 0;
  std::vector<uint64_t> degrees;
  degrees.reserve(vertex_count);

  // Iterate through all vertices to get their degrees
  for (v_id_t vertex_id = 0; vertex_id < vertex_count; ++vertex_id) {
    auto [neighbors_span, current_page] = read_neighbor(vertex_id);

    uint64_t degree = neighbors_span.size();
    degrees.push_back(degree);
    max_degree = std::max(max_degree, degree);

    current_page->r_unlatch(); // Release the read latch
  }

  // Calculate number of buckets needed
  uint64_t num_buckets = (max_degree / step) + 1;

  // Count vertices in each degree range
  std::vector<uint64_t> degree_counts(num_buckets, 0);

  for (uint64_t degree : degrees) {
    uint64_t bucket_index = degree / step;
    degree_counts[bucket_index]++;
  }

  // Convert counts to percentages and build result
  std::vector<std::pair<uint64_t, double>> distribution;
  distribution.reserve(num_buckets);

  for (uint64_t i = 0; i < num_buckets; ++i) {
    if (degree_counts[i] > 0) {
      uint64_t range_start = i * step;
      double percentage = (static_cast<double>(degree_counts[i]) / vertex_count) * 100.0;
      distribution.emplace_back(range_start, percentage);
    }
  }

  return distribution;
}

// Run bfs block inner.
std::vector<std::pair<v_id_t, uint64_t>>
bw_graph_db_t::bfs_block_inner(v_id_t start_vertex, csr_page_t* block, std::vector<v_id_t>& outside,
                               std::vector<bool>& visited) {
  std::vector<std::pair<v_id_t, uint64_t>> vertex_order;
  uint64_t num_vertices_ = block->get_num_vertices();
  page_no_t current_page_no = block->get_page_no();

  // Early return for empty page
  if (num_vertices_ == 0) {
    return vertex_order;
  }

  vertex_loc_t start_loc = this->vertex_index->get_vertex_location(start_vertex);
  if (start_loc.second >= num_vertices_ ||
      block->get_vertices()[start_loc.second].vertex_id != start_vertex) {
    return vertex_order;
  }

  // Pre-allocate result vector to avoid frequent reallocations
  vertex_order.reserve(num_vertices_);

  // Use two vectors to implement level-by-level BFS (more cache-friendly)
  std::vector<uint64_t> current_level;
  std::vector<uint64_t> next_level;

  // Reserve space to reduce allocations - adaptive sizing
  uint64_t reserve_size = std::min(static_cast<uint64_t>(1024), num_vertices_);
  current_level.reserve(reserve_size);
  next_level.reserve(reserve_size);

  // Initialize BFS
  current_level.push_back(start_loc.second);
  visited[start_vertex] = true;
  vertex_order.push_back({start_vertex, 0});

  uint64_t current_level_num = 0;

  // Level-by-level BFS traversal
  while (!current_level.empty()) {
    // Process all vertices in current level
    for (uint64_t current_offset : current_level) {
      // Read neighbors from block
      auto neighbor_span = block->get_neighbors(current_offset);
      // For each neighbor;
      for (v_id_t neighbor_id : neighbor_span) {
        uint64_t neighbor_offset = this->vertex_index->get_vertex_location(neighbor_id).second;
        // Check if neighbor is an outside vertex
        if (this->vertex_index->get_vertex_location(neighbor_id).first != current_page_no) {
          outside.push_back(neighbor_id);
        } else if (neighbor_offset < num_vertices_ && !visited[neighbor_id] &&
                   block->get_vertices()[neighbor_offset].vertex_id == neighbor_id) {
          visited[neighbor_id] = true;
          vertex_order.push_back({neighbor_id, current_level_num + 1});
          next_level.push_back(neighbor_offset);
        }
      }
    }

    // Move to next level efficiently
    current_level.clear();
    current_level.swap(next_level);
    current_level_num++;
  }

  return vertex_order;
}
// Benchmark versions: run the core algorithm and return elapsed seconds.
// Result construction is omitted so timings reflect only the computation.
float bw_graph_db_t::benchmark_parallel_bfs_scan(v_id_t start_vertex, int num_threads) {
  bit_map_t* active_in = new bit_map_t(this->get_vertex_count());
  bit_map_t* active_out = new bit_map_t(this->get_vertex_count());
  std::vector<v_id_t> parent(this->get_vertex_count(), INVALID_VID);

  active_out->clear();
  active_out->set_bit(start_vertex);
  parent[start_vertex] = start_vertex;

  uint32_t active_vertices = 1;
  double start_time = get_time();

  while (active_vertices != 0) {
    std::swap(active_in, active_out);
    active_out->clear();

    active_vertices = bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          if (parent[dst] == INVALID_VID) {
            if (__sync_bool_compare_and_swap(&parent[dst], INVALID_VID, src)) {
              active_out->set_bit(dst);
              return 1;
            }
          }
          return 0;
        },
        active_in, uint32_t(0), num_threads);
  }

  double end_time = get_time();
  delete active_in;
  delete active_out;
  return static_cast<float>(end_time - start_time);
}

// Benchmark parallel wcc scan.
float bw_graph_db_t::benchmark_parallel_wcc_scan(int num_threads) {
  bit_map_t* active_in = new bit_map_t(this->get_vertex_count());
  bit_map_t* active_out = new bit_map_t(this->get_vertex_count());
  std::vector<v_id_t> label(this->get_vertex_count());

  active_out->fill();
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    label[v] = v;
  }

  uint32_t active_vertices = this->get_vertex_count();
  double start_time = get_time();

  while (active_vertices != 0) {
    std::swap(active_in, active_out);
    active_out->clear();

    active_vertices = bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          if (label[src] < label[dst]) {
            v_id_t old_label = label[dst];
            while (label[src] < old_label) {
              if (__sync_bool_compare_and_swap(&label[dst], old_label, label[src])) {
                active_out->set_bit(dst);
                return 1;
              }
              old_label = label[dst];
            }
          }
          return 0;
        },
        active_in, uint32_t(0), num_threads);
  }

  double end_time = get_time();
  delete active_in;
  delete active_out;
  return static_cast<float>(end_time - start_time);
}

// Benchmark parallel pagerank scan.
float bw_graph_db_t::benchmark_parallel_pagerank_scan(uint64_t max_iterations, float damping_factor,
                                                      float convergence_threshold,
                                                      int num_threads) {
  std::vector<float> pagerank(this->get_vertex_count());
  std::vector<float> sum(this->get_vertex_count());

  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    uint32_t deg = this->get_vertex_degree(v);
    pagerank[v] = (deg > 0) ? (1.0f / deg) : 0.0f;
    sum[v] = 0.0f;
  }

  double start_time = get_time();

  for (uint64_t iter = 0; iter < max_iterations; iter++) {
    bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          write_add(&sum[dst], pagerank[src]);
          return 0;
        },
        nullptr, uint32_t(0), num_threads);

    float delta = 0.0f;

    if (iter == max_iterations - 1) {
      for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
        float new_pr = (1.0f - damping_factor) + damping_factor * sum[v];
        delta += std::abs(new_pr - pagerank[v] * this->get_vertex_degree(v));
        pagerank[v] = new_pr;
      }
    } else {
      for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
        uint32_t deg = this->get_vertex_degree(v);
        if (deg > 0) {
          float new_pr = ((1.0f - damping_factor) + damping_factor * sum[v]) / deg;
          delta += std::abs(new_pr * deg - pagerank[v] * deg);
          pagerank[v] = new_pr;
        }
        sum[v] = 0.0f;
      }
    }

    if (convergence_threshold > 0 && delta < convergence_threshold) {
      break;
    }
  }

  double end_time = get_time();
  return static_cast<float>(end_time - start_time);
}

// Benchmark parallel cdlp scan.
float bw_graph_db_t::benchmark_parallel_cdlp_scan(uint64_t max_iterations, int num_threads) {
  bit_map_t* active_in = new bit_map_t(this->get_vertex_count());
  bit_map_t* active_out = new bit_map_t(this->get_vertex_count());
  std::vector<v_id_t> label(this->get_vertex_count());

  active_out->fill();
  for (v_id_t v = 0; v < this->get_vertex_count(); v++) {
    label[v] = v;
  }

  uint32_t active_vertices = this->get_vertex_count();
  double start_time = get_time();

  for (uint64_t iter = 0; iter < max_iterations; iter++) {
    if (active_vertices == 0)
      break;

    std::swap(active_in, active_out);
    active_out->clear();

    active_vertices = bw_edge_scan<uint32_t>(
        *this,
        [&](v_id_t src, v_id_t dst) -> uint32_t {
          if (label[src] < label[dst]) {
            v_id_t old_label = label[dst];
            while (label[src] < old_label) {
              if (__sync_bool_compare_and_swap(&label[dst], old_label, label[src])) {
                active_out->set_bit(dst);
                return 1;
              }
              old_label = label[dst];
            }
          }
          return 0;
        },
        active_in, uint32_t(0), num_threads);
  }

  double end_time = get_time();
  delete active_in;
  delete active_out;
  return static_cast<float>(end_time - start_time);
}
