#ifndef BW_GRAPH_PART_PARTITION_H
#define BW_GRAPH_PART_PARTITION_H

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/mem/mem_g0com.h"
#include "bw_graph/mem/mem_g0csr.h"
#include "bw_graph/mem/mem_g0map.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <iostream>
#include <kaminpar.h>
#include <mutex>
#include <stdexcept>
#include <tbb/info.h>
#include <vector>

/**
 * @brief Perform k-way partitioning on a subgraph with non-continuous IDs,
 *        returning equivalence classes (originalID -> partition ID, starting
 * from 0)
 *
 * @param subgraph  mem_sub_csr_t subgraph (vertex IDs may be non-continuous)
 * @param k         target number of partitions (k >= 1)
 * @return folly::F14FastMap<v_id_t, size_t> mapping:
 *         original vertex ID -> [0, k - 1] partition ID
 */
inline folly::F14FastMap<v_id_t, size_t> kway_graph_partition(const mem_sub_csr_t& subgraph,
                                                              size_t k) {
  if (k == 0) {
    throw std::invalid_argument("k must be >= 1");
  }

  const size_t n = subgraph.vertex_count();
  const size_t m_hint = subgraph.edge_count();
  if (n == 0) {
    return {};
  }
  if (k > n) {
    k = n; // Extra partitions will be empty; truncate to n
  }

  // Vertex list and original->compact index mapping
  const auto nodes = subgraph.vertex_list();
  folly::F14FastMap<v_id_t, size_t> id2idx;
  id2idx.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    id2idx.emplace(nodes[i], i);
  }

  // Construct KaMinPar input CSR
  std::vector<kaminpar::shm::EdgeID> xadj(n + 1);
  std::vector<kaminpar::shm::NodeID> adjncy;
  adjncy.reserve(m_hint); // Reserve capacity to avoid frequent reallocations

  std::vector<kaminpar::shm::NodeWeight> node_weights(n, 1);

  size_t cursor = 0;
  for (size_t i = 0; i < n; ++i) {
    const v_id_t u = nodes[i];
    const auto ngh = subgraph.get_neighbors(u);

    xadj[i] = static_cast<kaminpar::shm::EdgeID>(cursor);

    // Only include neighbors still in the subgraph (checked via id2idx)
    size_t deg = 0;
    for (const v_id_t v : ngh) {
      auto it = id2idx.find(v);
      if (it == id2idx.end())
        continue;
      adjncy.push_back(static_cast<kaminpar::shm::NodeID>(it->second));
      ++deg;
    }

    // Node weight: 1 + degree (high-degree nodes treated as 1, consistent with
    // global strategy)
    if (deg >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
      node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1);
    } else {
      node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1 + deg);
    }

    cursor = adjncy.size();
  }
  xadj[n] = static_cast<kaminpar::shm::EdgeID>(adjncy.size());

  // Create KaMinPar partitioner
  kaminpar::KaMinPar shm_partitioner(10, kaminpar::shm::create_default_context());
  shm_partitioner.set_output_level(kaminpar::OutputLevel::QUIET);
  shm_partitioner.borrow_and_mutate_graph(xadj, adjncy, node_weights);
  shm_partitioner.set_k(static_cast<kaminpar::shm::BlockID>(k));

  // Compute partition
  std::vector<kaminpar::shm::BlockID> part(n, 0);
  (void) shm_partitioner.compute_partition(part);

  // Map to 0-based partition IDs: originalID -> [0..k-1]
  folly::F14FastMap<v_id_t, size_t> result;
  result.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    result.emplace(nodes[i], static_cast<size_t>(part[i]));
  }
  return result;
}

// Compute the weight of a single vertex in mem_sub_com_t
static inline kaminpar::shm::NodeWeight node_weight_original(const mem_sub_com_t& graph, v_id_t v) {
  const auto ng = graph.get_neighbors(v);
  const size_t deg = ng.size();
  if (deg >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND)
    return static_cast<kaminpar::shm::NodeWeight>(1);
  return static_cast<kaminpar::shm::NodeWeight>(1 + deg);
}

// Compute the weight of a set of vertices in mem_sub_com_t
static inline kaminpar::shm::NodeWeight block_weight_original(const mem_sub_com_t& graph,
                                                              const std::vector<v_id_t>& nodes) {
  kaminpar::shm::NodeWeight sum = 0;
  for (const auto v : nodes)
    sum += node_weight_original(graph, v);
  return sum;
}

// Collect the vertex list of each partition according to the current mapping
// (partition IDs start from 0)
static inline std::vector<std::vector<v_id_t>>
collect_blocks(const folly::F14FastMap<v_id_t, size_t>& block_of, size_t& max_block_id) {
  max_block_id = 0;
  for (const auto& kv : block_of)
    if (kv.second > max_block_id)
      max_block_id = kv.second;
  std::vector<std::vector<v_id_t>> buckets(max_block_id + 1);
  for (const auto& kv : block_of) {
    const size_t b = kv.second;
    buckets[b].push_back(kv.first);
  }
  return buckets;
}

// [OPT-11] Compress the current equivalence-class mapping into continuous
// [0..B-1] Memory optimization: use vector instead of hash map for remap, and
// release it immediately
static inline void compress_block_ids(folly::F14FastMap<v_id_t, size_t>& block_of) {
  if (block_of.empty())
    return;

  // [OPT-12] First pass: find the maximum block ID to size the remap vector
  // appropriately
  size_t max_block_id = 0;
  for (const auto& [vid, bid] : block_of) {
    max_block_id = std::max(max_block_id, bid);
  }

  // [OPT-13] Use vector instead of hash map for O(1) lookup with less memory
  // overhead Initialize with sentinel value to detect unmapped IDs
  std::vector<size_t> remap(max_block_id + 1, SIZE_MAX);

  // [OPT-14] Second pass: build the remap table
  size_t next_id = 0;
  for (const auto& [vid, bid] : block_of) {
    if (remap[bid] == SIZE_MAX) {
      remap[bid] = next_id++;
    }
  }

  // [OPT-15] Third pass: apply the remapping in-place
  for (auto& [vid, bid] : block_of) {
    bid = remap[bid];
  }

  // [OPT-16] Explicitly release remap vector memory
  remap.clear();
  remap.shrink_to_fit();
}

/**
 * @brief Absolute weight-constrained graph partitioning:
 *        The total weight (1+deg) of any partition must not exceed
 * max_block_weight.
 * @param graph             Original graph (mem_sub_com_t), subgraphs can be
 * induced from it
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs start at
 * 0 and are compressed to [0..B-1])
 */
inline folly::F14FastMap<v_id_t, size_t> absolute_weight_graph_partition_non_giant(
    const mem_sub_com_t& graph, kaminpar::shm::NodeWeight max_block_weight,
    bool enable_edge_weight = true, bool refine = true, uint32_t num_threads = 10) {
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
  if (max_block_weight == 0) {
    throw std::invalid_argument("max_block_weight must be > 0");
  }
  const auto all_nodes = graph.vertex_list();
  if (all_nodes.empty())
    return {};

  // 1) Initial k: estimate based on the sum of original graph node weights
  const size_t n = graph.vertex_count();
  const size_t m = graph.edge_count();

  // xadj / adjncy
  std::vector<kaminpar::shm::EdgeID> xadj(n + 1);
  std::vector<kaminpar::shm::NodeID> adjncy(m);

  // Convert from CSR format
  for (size_t i = 0; i <= n; ++i) {
    xadj[i] = static_cast<kaminpar::shm::EdgeID>(graph.offsets[i]);
  }

  for (size_t i = 0; i < m; ++i) {
    adjncy[i] = static_cast<kaminpar::shm::NodeID>(graph.neighbor_list[i]);
  }

  // Node weight: 1 + degree (high-degree nodes treated as 1, consistent with
  // strategy)
  std::vector<kaminpar::shm::NodeWeight> node_weights(n);
  kaminpar::shm::NodeWeight total_weight = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t degree = static_cast<size_t>(graph.offsets[i + 1] - graph.offsets[i]);
    if (enable_edge_weight) {
      node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1 + degree);
      total_weight += node_weights[i];
    } else {
      node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1);
      total_weight += node_weights[i];
    }
  }

  // Number of partitions needed (at least 1)
  kaminpar::shm::BlockID num_partitions =
      static_cast<kaminpar::shm::BlockID>((total_weight + max_block_weight - 1) / max_block_weight);

  // 2) Partition the entire graph (exported sub_csr)
  // Create KaMinPar partitioner
  kaminpar::KaMinPar shm_partitioner(num_threads, kaminpar::shm::create_fast_context());
  shm_partitioner.set_output_level(kaminpar::OutputLevel::QUIET);

  // Provide graph and weights
  shm_partitioner.borrow_and_mutate_graph(xadj, adjncy, node_weights);

  // Set k and strong constraint (absolute upper bound)
  shm_partitioner.set_k(num_partitions);
  std::vector<kaminpar::shm::BlockWeight> max_block_weights(num_partitions, max_block_weight);
  shm_partitioner.set_absolute_max_block_weights(max_block_weights);

  // Perform first partition
  std::cout << "Starting initial partition attempt." << std::endl;
  std::vector<kaminpar::shm::BlockID> init_partition(n);
  shm_partitioner.compute_partition(init_partition);

  std::cout << "Initial partition successful." << std::endl;
  folly::F14FastMap<v_id_t, size_t> block_of;
  block_of.reserve(n);
  // mem_sub_com_t::vertex_list() returns original ID order; map sequentially by
  // index
  const auto& verts = graph.vertex_list();
  for (size_t i = 0; i < n; ++i) {
    block_of.emplace(verts[i], static_cast<size_t>(init_partition[i]));
  }

  // [OPT-1] Release initial partition result immediately after use
  init_partition.clear();
  init_partition.shrink_to_fit();

  if (refine) {
    std::cout << "Refining partitions..." << std::endl;

    // 3) Iteratively refine all partitions exceeding constraints until
    // satisfied
    while (true) {
      size_t max_bid = 0;
      bool any_over = false;

      // [OPT-2] Process blocks in batches: collect overweight block IDs first,
      // then process them one by one to avoid keeping all buckets in memory
      std::vector<size_t> overweight_blocks;
      {
        // [OPT-3] Scoped lifetime: buckets will be destructed after this block
        auto buckets = collect_blocks(block_of, max_bid);
        const size_t round_limit = max_bid;

        // First pass: identify which blocks need refinement
        for (size_t b = 0; b <= round_limit; ++b) {
          const auto& verts = buckets[b];
          if (verts.empty())
            continue;

          const kaminpar::shm::NodeWeight bw = block_weight_original(graph, verts);
          if (bw > max_block_weight) {
            overweight_blocks.push_back(b);
            any_over = true;
            std::cout << "Partition " << b << " exceeds limit, weight = " << bw
                      << ", vertices = " << verts.size() << std::endl;
          }
        }

        // Second pass: process each overweight block individually
        for (size_t b : overweight_blocks) {
          auto& verts = buckets[b];
          const kaminpar::shm::NodeWeight bw = block_weight_original(graph, verts);

          // [OPT-4] Nested scope: ensure subgraph and local partition are
          // released immediately after processing
          {
            // 3.1 Export induced subgraph for this partition
            auto sub_ptr = graph.compute_induced_subgraph(verts);
            if (!sub_ptr || sub_ptr->vertex_count() == 0) {
              continue;
            }

            // 3.2 Required number of sub-partitions to satisfy constraint
            const kaminpar::shm::NodeWeight need_k_ll = static_cast<kaminpar::shm::NodeWeight>(
                (bw + max_block_weight - 1) / max_block_weight);
            size_t k2 = static_cast<size_t>(std::max<kaminpar::shm::NodeWeight>(2, need_k_ll));

            // 3.3 k-way partition of subgraph (result: 0..k2-1)
            auto local_map = kway_graph_partition(*sub_ptr, k2);

            // [OPT-5] Explicitly release subgraph memory (if shared_ptr)
            sub_ptr.reset();

            // 3.4 Assign globally unique IDs for new sub-blocks: start from max
            // ID + 1
            size_t cur_max = max_bid;
            std::vector<size_t> new_ids(k2);
            new_ids[0] = b;
            for (size_t i = 1; i < k2; ++i) {
              new_ids[i] = ++cur_max;
            }

            // 3.5 Update block_of: each vertex in subset assigned new ID
            for (auto v : verts) {
              auto it = local_map.find(v);
              if (it != local_map.end()) {
                const size_t lb = it->second; // 0..k2-1
                block_of[v] = new_ids[lb];
              }
            }

            // Update overall max ID
            max_bid = cur_max;

            // [OPT-6] Clear local_map explicitly to release memory
            local_map.clear();

          } // [OPT-7] Subgraph and local partition destructed here

          // [OPT-8] Clear vertex list for current block to release memory
          verts.clear();
          verts.shrink_to_fit();
        }

      } // [OPT-9] buckets destructed here, releasing significant memory

      if (!any_over) {
        std::cout << "All partitions satisfy weight constraints." << std::endl;
        break;
      }

      // [OPT-10] Clear temporary vector after each iteration
      overweight_blocks.clear();
      overweight_blocks.shrink_to_fit();
    }

    std::cout << "Partition refinement finished." << std::endl;
  }

  // 4) Compress partition IDs to [0..B-1] and return
  compress_block_ids(block_of);
  return block_of;
}

/**
 * @brief Absolute weight-constrained graph partitioning:
 *        The total weight (1+deg) of any partition must not exceed
 * max_block_weight.
 * @param graph             Original graph (mem_sub_com_t), subgraphs can be
 * induced from it
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs start at
 * 0 and are compressed to [0..B-1])
 */
inline folly::F14FastMap<v_id_t, size_t> absolute_weight_graph_partition(
    const mem_sub_com_t& graph, kaminpar::shm::NodeWeight max_block_weight,
    bool enable_edge_weight = true, bool refine = true, uint32_t num_threads = 10) {
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
  if (max_block_weight == 0) {
    throw std::invalid_argument("max_block_weight must be > 0");
  }
  const auto all_nodes = graph.vertex_list();
  if (all_nodes.empty())
    return {};

  // 1) Initial k: estimate based on the sum of original graph node weights
  const size_t n = graph.vertex_count();
  const size_t m = graph.edge_count();

  // xadj / adjncy
  std::vector<kaminpar::shm::EdgeID> xadj(n + 1);
  std::vector<kaminpar::shm::NodeID> adjncy(m);

  // Convert from CSR format
  for (size_t i = 0; i <= n; ++i) {
    xadj[i] = static_cast<kaminpar::shm::EdgeID>(graph.offsets[i]);
  }

  for (size_t i = 0; i < m; ++i) {
    adjncy[i] = static_cast<kaminpar::shm::NodeID>(graph.neighbor_list[i]);
  }

  // Node weight: 1 + degree (high-degree nodes treated as 1, consistent with
  // strategy)
  std::vector<kaminpar::shm::NodeWeight> node_weights(n);
  kaminpar::shm::NodeWeight total_weight = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t degree = static_cast<size_t>(graph.offsets[i + 1] - graph.offsets[i]);
    if (enable_edge_weight) {
      if (degree >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND) {
        node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1);
      } else {
        node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1 + degree);
      }
      total_weight += node_weights[i];
    } else {
      node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1);
      total_weight += node_weights[i];
    }
  }

  // Number of partitions needed (at least 1)
  kaminpar::shm::BlockID num_partitions =
      static_cast<kaminpar::shm::BlockID>((total_weight + max_block_weight - 1) / max_block_weight);

  // 2) Partition the entire graph (exported sub_csr)
  // Create KaMinPar partitioner
  kaminpar::KaMinPar shm_partitioner(num_threads, kaminpar::shm::create_fast_context());
  shm_partitioner.set_output_level(kaminpar::OutputLevel::QUIET); // or DEBUG

  // Provide graph and weights
  shm_partitioner.borrow_and_mutate_graph(xadj, adjncy, node_weights);

  // Set k and strong constraint (absolute upper bound)
  shm_partitioner.set_k(num_partitions);
  std::vector<kaminpar::shm::BlockWeight> max_block_weights(num_partitions, max_block_weight);
  shm_partitioner.set_absolute_max_block_weights(max_block_weights);

  std::vector<kaminpar::shm::BlockID> init_partition(n);
  shm_partitioner.compute_partition(init_partition);
  folly::F14FastMap<v_id_t, size_t> block_of;
  block_of.reserve(n);
  // mem_sub_com_t::vertex_list() returns original ID order; map sequentially by
  // index
  const auto& verts = graph.vertex_list();
  for (size_t i = 0; i < n; ++i) {
    block_of.emplace(verts[i], static_cast<size_t>(init_partition[i]));
  }

  // [OPT-1] Release initial partition result immediately after use
  init_partition.clear();
  init_partition.shrink_to_fit();

  if (refine) {
    std::cout << "Refining partitions..." << std::endl;

    // 3) Iteratively refine all partitions exceeding constraints until
    // satisfied
    while (true) {
      size_t max_bid = 0;
      bool any_over = false;

      // [OPT-2] Process blocks in batches: collect overweight block IDs first,
      // then process them one by one to avoid keeping all buckets in memory
      std::vector<size_t> overweight_blocks;
      {
        // [OPT-3] Scoped lifetime: buckets will be destructed after this block
        auto buckets = collect_blocks(block_of, max_bid);
        const size_t round_limit = max_bid;

        // First pass: identify which blocks need refinement
        for (size_t b = 0; b <= round_limit; ++b) {
          const auto& verts = buckets[b];
          if (verts.empty())
            continue;

          const kaminpar::shm::NodeWeight bw = block_weight_original(graph, verts);
          if (bw > max_block_weight) {
            overweight_blocks.push_back(b);
            any_over = true;
            std::cout << "Partition " << b << " exceeds limit, weight = " << bw
                      << ", vertices = " << verts.size() << std::endl;
          }
        }

        // Second pass: process each overweight block individually
        for (size_t b : overweight_blocks) {
          auto& verts = buckets[b];
          const kaminpar::shm::NodeWeight bw = block_weight_original(graph, verts);

          // [OPT-4] Nested scope: ensure subgraph and local partition are
          // released immediately after processing
          {
            // 3.1 Export induced subgraph for this partition
            auto sub_ptr = graph.compute_induced_subgraph(verts);
            if (!sub_ptr || sub_ptr->vertex_count() == 0) {
              continue;
            }

            // 3.2 Required number of sub-partitions to satisfy constraint
            const kaminpar::shm::NodeWeight need_k_ll = static_cast<kaminpar::shm::NodeWeight>(
                (bw + max_block_weight - 1) / max_block_weight);
            size_t k2 = static_cast<size_t>(std::max<kaminpar::shm::NodeWeight>(2, need_k_ll));

            // 3.3 k-way partition of subgraph (result: 0..k2-1)
            auto local_map = kway_graph_partition(*sub_ptr, k2);

            // [OPT-5] Explicitly release subgraph memory (if shared_ptr)
            sub_ptr.reset();

            // 3.4 Assign globally unique IDs for new sub-blocks: start from max
            // ID + 1
            size_t cur_max = max_bid;
            std::vector<size_t> new_ids(k2);
            new_ids[0] = b;
            for (size_t i = 1; i < k2; ++i) {
              new_ids[i] = ++cur_max;
            }

            // 3.5 Update block_of: each vertex in subset assigned new ID
            for (auto v : verts) {
              auto it = local_map.find(v);
              if (it != local_map.end()) {
                const size_t lb = it->second; // 0..k2-1
                block_of[v] = new_ids[lb];
              }
            }

            // Update overall max ID
            max_bid = cur_max;

            // [OPT-6] Clear local_map explicitly to release memory
            local_map.clear();

          } // [OPT-7] Subgraph and local partition destructed here

          // [OPT-8] Clear vertex list for current block to release memory
          verts.clear();
          verts.shrink_to_fit();
        }

      } // [OPT-9] buckets destructed here, releasing significant memory

      if (!any_over) {
        std::cout << "All partitions satisfy weight constraints." << std::endl;
        break;
      }

      // [OPT-10] Clear temporary vector after each iteration
      overweight_blocks.clear();
      overweight_blocks.shrink_to_fit();
    }

    std::cout << "Partition refinement finished." << std::endl;
  }

  // 4) Compress partition IDs to [0..B-1] and return
  compress_block_ids(block_of);
  return block_of;
}

/**
 * @brief Absolute weight-constrained graph partitioning:
 *        The total weight (1+deg) of any partition must not exceed
 * max_block_weight.
 * @param graph             Original graph (mem_sub_com_t), subgraphs can be
 * induced from it
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs
 * start at 0 and are compressed to [0..B-1])
 */
inline graph_partition_t
compute_absolute_weight_graph_partition(const mem_sub_com_t& graph,
                                        kaminpar::shm::NodeWeight max_block_weight) {
  // Compute the max threads;
  int max_threads = tbb::info::default_concurrency();
  std::cout << "Using " << max_threads << " threads for partitioning." << std::endl;
  auto partition_result =
      absolute_weight_graph_partition(graph, max_block_weight, true, true, max_threads);
  // Process it into graph_partition_t
  graph_partition_t g_part;
  // Compute the partition count;
  size_t max_part = 0;
  for (const auto& entry : partition_result) {
    max_part = std::max(max_part, entry.second);
  }
  g_part.resize(max_part + 1);

  // Group vertices by their assigned partitions
  for (size_t i = 0; i < graph.vertex_count(); ++i) {
    kaminpar::shm::BlockID part = partition_result[i];
    g_part[part].push_back(static_cast<v_id_t>(i));
  }
  return g_part;
}

/**
 * @brief Absolute weight-constrained graph partitioning:
 *        The total weight (1+deg) of any partition must not exceed
 * max_block_weight.
 * @param graph             Original graph (mem_sub_com_t), subgraphs can be
 * induced from it
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs
 * start at 0 and are compressed to [0..B-1])
 */
inline graph_partition_t
compute_absolute_weight_graph_partition_non_giant(const mem_sub_com_t& graph,
                                                  kaminpar::shm::NodeWeight max_block_weight) {
  // Compute the max threads;
  int max_threads = tbb::info::default_concurrency();
  std::cout << "Using " << max_threads << " threads for partitioning." << std::endl;
  auto partition_result =
      absolute_weight_graph_partition_non_giant(graph, max_block_weight, true, false, max_threads);
  // Process it into graph_partition_t
  graph_partition_t g_part;
  // Compute the partition count;
  size_t max_part = 0;
  for (const auto& entry : partition_result) {
    max_part = std::max(max_part, entry.second);
  }
  g_part.resize(max_part + 1);

  // Group vertices by their assigned partitions
  for (size_t i = 0; i < graph.vertex_count(); ++i) {
    kaminpar::shm::BlockID part = partition_result[i];
    g_part[part].push_back(static_cast<v_id_t>(i));
  }
  return g_part;
}

/**
 * @brief Absolute weight-constrained graph partitioning (map version).
 *        Converts map-based graph into CSR format, calls the CSR partitioner,
 *        then maps the result back to original vertex IDs.
 * @param graph             Original map-based graph (mem_sub_map_t)
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs start at
 * 0 and are compressed to [0..B-1])
 */
inline folly::F14FastMap<v_id_t, size_t>
absolute_weight_graph_partition(const mem_sub_map_t& graph,
                                kaminpar::shm::NodeWeight max_block_weight) {
  if (max_block_weight == 0) {
    throw std::invalid_argument("max_block_weight must be > 0");
  }
  const auto all_nodes = graph.vertex_list();
  if (all_nodes.empty())
    return {};

  // === Step 1. Build mapping between original IDs and CSR IDs ===
  folly::F14FastMap<v_id_t, size_t> orig2csr;
  std::vector<v_id_t> csr2orig(all_nodes.size());
  orig2csr.reserve(all_nodes.size());
  for (size_t i = 0; i < all_nodes.size(); ++i) {
    orig2csr[all_nodes[i]] = i;
    csr2orig[i] = all_nodes[i];
  }

  // === Step 2. Construct CSR representation ===
  const size_t n = all_nodes.size();
  std::vector<size_t> offsets(n + 1, 0);
  std::vector<v_id_t> neighbor_list;

  size_t edge_count = 0;
  for (size_t i = 0; i < n; ++i) {
    v_id_t orig_id = csr2orig[i];
    auto neighs = graph.get_neighbors(orig_id);
    offsets[i] = edge_count;
    for (auto nb : neighs) {
      auto it = orig2csr.find(nb);
      if (it != orig2csr.end()) {
        neighbor_list.push_back(static_cast<v_id_t>(it->second));
        ++edge_count;
      }
    }
  }
  offsets[n] = edge_count;

  // Build CSR graph
  mem_sub_com_t csr_graph(offsets, neighbor_list, {});

  // === Step 3. Call existing CSR partition function ===
  auto csr_partition = absolute_weight_graph_partition(csr_graph, max_block_weight);

  // === Step 4. Map result back to original vertex IDs ===
  folly::F14FastMap<v_id_t, size_t> block_of;
  block_of.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    v_id_t orig_id = csr2orig[i];
    block_of.emplace(orig_id, csr_partition.at(i));
  }

  return block_of;
}

inline std::vector<page_no_t>
get_neighbor_pages(const folly::F14FastMap<page_no_t, std::vector<page_no_t>>& graph,
                   page_no_t page) {
  auto it = graph.find(page);
  if (it != graph.end()) {
    return it->second;
  }
  return {};
}

inline std::vector<v_id_t> get_neighbor(const adj_map_t& graph, page_no_t page) {
  auto it = graph.find(page);
  if (it != graph.end()) {
    return it->second;
  }
  return {};
}

/**
 * @brief Absolute weight-constrained graph partitioning (map version).
 *        Converts map-based graph into CSR format, calls the CSR partitioner,
 *        then maps the result back to original vertex IDs.
 * @param graph             Original map-based graph (mem_sub_map_t)
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs start at
 * 0 and are compressed to [0..B-1])
 */
inline std::pair<block_partition_t, folly::F14FastMap<v_id_t, size_t>>
absolute_weight_block_partition(const folly::F14FastMap<page_no_t, std::vector<page_no_t>>& graph,
                                kaminpar::shm::NodeWeight max_block_weight) {
  if (max_block_weight == 0) {
    throw std::invalid_argument("max_block_weight must be > 0");
  }

  if (graph.empty())
    return {};

  // === Step 1. Build mapping between original IDs and CSR IDs ===
  folly::F14FastMap<page_no_t, size_t> orig2csr;
  std::vector<page_no_t> csr2orig(graph.size());
  orig2csr.reserve(graph.size());

  uint64_t idx = 0;
  for (const auto& [page_no, _] : graph) {
    orig2csr[page_no] = idx;
    csr2orig[idx] = (page_no);
    ++idx;
  }

  // === Step 2. Construct CSR representation ===
  const size_t n = graph.size();
  std::vector<size_t> offsets(n + 1, 0);
  std::vector<v_id_t> neighbor_list;

  size_t edge_count = 0;
  for (size_t i = 0; i < n; ++i) {
    v_id_t orig_id = csr2orig[i];
    auto neighs = get_neighbor_pages(graph, orig_id);
    offsets[i] = edge_count;
    for (auto nb : neighs) {
      auto it = orig2csr.find(nb);
      if (it != orig2csr.end()) {
        neighbor_list.push_back(static_cast<v_id_t>(it->second));
        ++edge_count;
      } else {
        // std::cerr << "Warning: neighbor page " << nb << " not found in
        // orig2csr map." << std::endl;
      }
    }
  }
  offsets[n] = edge_count;

  // Build CSR graph
  mem_sub_com_t csr_graph(offsets, neighbor_list, {});

  // === Step 3. Call existing CSR partition function ===
  folly::F14FastMap<v_id_t, size_t> csr_partition =
      absolute_weight_graph_partition(csr_graph, max_block_weight, false, false);

  // === Step 4. Map result back to original vertex IDs ===
  folly::F14FastMap<v_id_t, size_t> block_of;
  block_of.reserve(n);
  size_t max_part_id = 0;
  for (const auto& [csr_id, part_id] : csr_partition) {
    block_of[csr2orig[csr_id]] = part_id;
    max_part_id = std::max(max_part_id, part_id);
  }

  block_partition_t block_of_vec(max_part_id + 1);
  for (const auto& [csr_id, part_id] : csr_partition) {
    block_of_vec[part_id].push_back(csr2orig[csr_id]);
  }

  return {block_of_vec, block_of};
}

/**
 * @brief Absolute weight-constrained graph partitioning (map version).
 *        Converts map-based graph into CSR format, calls the CSR partitioner,
 *        then maps the result back to original vertex IDs.
 * @param graph             Original map-based graph (mem_sub_map_t)
 * @param max_block_weight  Maximum allowed weight per partition (>0,
 * kaminpar::shm::NodeWeight)
 * @return Equivalence classes: original vertex ID -> partition ID (IDs start at
 * 0 and are compressed to [0..B-1])
 */
inline graph_partition_t absolute_weight_partition(adj_map_t& graph,
                                                   kaminpar::shm::NodeWeight max_block_weight,
                                                   bool enable_edge_weight = true,
                                                   bool refine = true, uint32_t num_threads = 1) {
  if (max_block_weight == 0) {
    throw std::invalid_argument("max_block_weight must be > 0");
  }

  if (graph.empty())
    return {};

  // === Step 1. Build mapping between original IDs and CSR IDs ===
  folly::F14FastMap<v_id_t, size_t> orig2csr;
  std::vector<v_id_t> csr2orig(graph.size());
  orig2csr.reserve(graph.size());

  uint64_t idx = 0;
  for (const auto& [v_id, _] : graph) {
    orig2csr[v_id] = idx;
    csr2orig[idx] = v_id;
    ++idx;
  }

  // === Step 2. Construct CSR representation ===
  const size_t n = graph.size();
  std::vector<size_t> offsets(n + 1, 0);
  std::vector<v_id_t> neighbor_list;

  size_t edge_count = 0;
  for (size_t i = 0; i < n; ++i) {
    v_id_t orig_id = csr2orig[i];
    auto neighs = get_neighbor(graph, orig_id);
    offsets[i] = edge_count;
    for (auto nb : neighs) {
      auto it = orig2csr.find(nb);
      if (it != orig2csr.end()) {
        neighbor_list.push_back(static_cast<v_id_t>(it->second));
        ++edge_count;
      } else {
        // std::cerr << "Warning: neighbor page " << nb << " not found in
        // orig2csr map." << std::endl;
      }
    }
  }
  offsets[n] = edge_count;

  // Build CSR graph
  mem_sub_com_t csr_graph(offsets, neighbor_list, {});

  // === Step 3. Call existing CSR partition function ===
  folly::F14FastMap<v_id_t, size_t> csr_partition = absolute_weight_graph_partition(
      csr_graph, max_block_weight, enable_edge_weight, refine, num_threads);

  // === Step 4. Map result back to original vertex IDs ===
  folly::F14FastMap<v_id_t, size_t> block_of;
  block_of.reserve(n);
  size_t max_part_id = 0;
  for (const auto& [csr_id, part_id] : csr_partition) {
    block_of[csr2orig[csr_id]] = part_id;
    max_part_id = std::max(max_part_id, part_id);
  }

  block_partition_t block_of_vec(max_part_id + 1);
  for (const auto& [csr_id, part_id] : csr_partition) {
    block_of_vec[part_id].push_back(csr2orig[csr_id]);
  }

  return block_of_vec;
}

/**
 * @brief Induced subgraph for a specific graph.
 * @param block_graph The original block graph.
 * @param block The specific block for which to induce the subgraph.
 * @return The induced subgraph.
 */
inline adj_map_t induced_subgraph(adj_map_t& graph, const std::vector<v_id_t>& vertices,
                                  bool filtered = true) {
  adj_map_t subgraph;
  for (auto& block_id : vertices) {
    subgraph.insert({block_id, {}});
  }
  for (auto& v_id : vertices) {
    std::vector<page_no_t> neighbors = get_neighbor(graph, v_id);
    for (auto& neighbor : neighbors) {
      if (!filtered || subgraph.contains(neighbor)) {
        subgraph[v_id].push_back(neighbor);
      }
    }
  }
  return subgraph;
}

/**
 * @brief Induced subgraph for a specific block.
 * @param block_graph The original block graph.
 * @param block The specific block for which to induce the subgraph.
 * @return The induced subgraph.
 */
inline block_adj_map_t induced_block_subgraph(block_adj_map_t& block_graph,
                                              const std::vector<v_id_t>& block) {
  block_adj_map_t subgraph;
  for (auto& block_id : block) {
    subgraph.insert({block_id, {}});
  }
  for (auto& block_id : block) {
    std::vector<page_no_t> neighbors = get_neighbor_pages(block_graph, block_id);
    for (auto& neighbor : neighbors) {
      if (subgraph.contains(neighbor)) {
        subgraph[block_id].push_back(neighbor);
      }
    }
  }
  return subgraph;
}

/**
 * @brief Sparsifies the graph by limiting the degree of each node.
 * @param graph The original block graph.
 * @param max_degree The maximum allowed degree for each node.
 * @return The sparsified graph.
 */
inline block_adj_map_t graph_sparsification(const block_adj_map_t& graph, uint64_t max_degree) {
  if (max_degree == 0 || graph.empty()) {
    return block_adj_map_t{};
  }

  // Result graph and degree tracking
  block_adj_map_t result;
  folly::F14FastMap<page_no_t, uint64_t> current_degrees;

  // Initialize all nodes in result graph and degree tracking
  for (const auto& [node, _] : graph) {
    result[node] = std::vector<page_no_t>{};
    current_degrees[node] = 0;
  }

  // Helper lambda to add edge to result if degree constraints allow
  auto try_add_edge = [&](page_no_t u, page_no_t v) -> bool {
    if (current_degrees[u] < max_degree && current_degrees[v] < max_degree) {
      result[u].push_back(v);
      result[v].push_back(u);
      current_degrees[u]++;
      current_degrees[v]++;
      return true;
    }
    return false;
  };

  // Helper lambda to calculate edge importance (degree sum of endpoints)
  auto calculate_edge_importance = [&](page_no_t u, page_no_t v) -> uint64_t {
    auto it_u = graph.find(u);
    auto it_v = graph.find(v);
    uint64_t degree_u = (it_u != graph.end()) ? it_u->second.size() : 0;
    uint64_t degree_v = (it_v != graph.end()) ? it_v->second.size() : 0;
    return degree_u + degree_v;
  };

  // Collect all edges with their importance scores
  std::vector<std::tuple<uint64_t, page_no_t, page_no_t>> edges_with_scores;
  std::set<std::pair<page_no_t, page_no_t>> processed_edges;

  for (const auto& [u, neighbors] : graph) {
    for (page_no_t v : neighbors) {
      // Avoid duplicate edges (u,v) and (v,u)
      auto edge_pair = std::make_pair(std::min(u, v), std::max(u, v));
      if (processed_edges.find(edge_pair) == processed_edges.end()) {
        processed_edges.insert(edge_pair);
        uint64_t importance = calculate_edge_importance(u, v);
        edges_with_scores.emplace_back(importance, u, v);
      }
    }
  }

  // Sort edges by importance (descending)
  std::sort(edges_with_scores.begin(), edges_with_scores.end(),
            [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });

  // Phase 1: Build degree-constrained spanning tree for connectivity
  folly::F14FastMap<page_no_t, page_no_t> parent;
  folly::F14FastMap<page_no_t, uint64_t> rank;

  // Initialize Union-Find
  for (const auto& [node, _] : graph) {
    parent[node] = node;
    rank[node] = 0;
  }

  // Union-Find operations
  auto find_root = [&](page_no_t x) -> page_no_t {
    std::vector<page_no_t> path;
    while (parent[x] != x) {
      path.push_back(x);
      x = parent[x];
    }
    // Path compression
    for (page_no_t node : path) {
      parent[node] = x;
    }
    return x;
  };

  auto union_sets = [&](page_no_t x, page_no_t y) -> bool {
    page_no_t root_x = find_root(x);
    page_no_t root_y = find_root(y);

    if (root_x == root_y)
      return false;

    // Union by rank
    if (rank[root_x] < rank[root_y]) {
      parent[root_x] = root_y;
    } else if (rank[root_x] > rank[root_y]) {
      parent[root_y] = root_x;
    } else {
      parent[root_y] = root_x;
      rank[root_x]++;
    }
    return true;
  };

  // Build spanning tree with degree constraints
  uint64_t tree_edges = 0;
  uint64_t target_tree_edges = graph.size() > 0 ? graph.size() - 1 : 0;

  for (const auto& [importance, u, v] : edges_with_scores) {
    if (tree_edges >= target_tree_edges)
      break;

    if (find_root(u) != find_root(v) && current_degrees[u] < max_degree &&
        current_degrees[v] < max_degree) {
      if (union_sets(u, v)) {
        try_add_edge(u, v);
        tree_edges++;
      }
    }
  }

  // Phase 2: Add remaining important edges within degree constraints
  for (const auto& [importance, u, v] : edges_with_scores) {
    if (current_degrees[u] < max_degree && current_degrees[v] < max_degree) {
      // Check if edge already exists
      bool edge_exists = false;
      auto it = result.find(u);
      if (it != result.end()) {
        for (page_no_t neighbor : it->second) {
          if (neighbor == v) {
            edge_exists = true;
            break;
          }
        }
      }

      if (!edge_exists) {
        try_add_edge(u, v);
      }
    }
  }

  return result;
}

/**
 * @brief Preprocesses the graph by removing duplicated edges and converting to
 * undirected graph.
 * @param old_graph The original directed graph with potential duplicate edges.
 * @return The preprocessed undirected graph without duplicate edges.
 */
inline adj_map_t pre_partition_handle(adj_map_t&& old_graph) {
  // Use sets to automatically handle deduplication
  folly::F14FastMap<v_id_t, folly::F14FastSet<v_id_t>> temp_graph;

  // Pre-allocate vertices to avoid repeated lookups
  for (auto& [vertex, neighbors] : old_graph) {
    temp_graph[vertex]; // Creates empty set if not exists
  }

  // Single pass: build undirected graph with automatic deduplication
  for (auto& [vertex, neighbors] : old_graph) {
    for (const auto& neighbor : neighbors) {
      // Skip self-loops
      if (neighbor == vertex)
        continue;

      // Add bidirectional edges (sets automatically handle duplicates)
      temp_graph[vertex].insert(neighbor);
      if (!temp_graph.contains(neighbor)) {
        temp_graph[neighbor]; // Ensure neighbor exists in map
      }
      temp_graph[neighbor].insert(vertex);
    }
  }

  // Convert sets back to vectors for final adj_map_t
  adj_map_t new_graph;
  new_graph.reserve(temp_graph.size()); // Pre-allocate for known size

  for (auto& [vertex, neighbor_set] : temp_graph) {
    new_graph[vertex].reserve(neighbor_set.size());
    new_graph[vertex].assign(neighbor_set.begin(), neighbor_set.end());
  }

  return new_graph;
}
#endif // BW_GRAPH_PART_PARTITION_H
