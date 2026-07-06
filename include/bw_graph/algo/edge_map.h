#ifndef BW_GRAPH_ALGO_EDGE_MAP_H
#define BW_GRAPH_ALGO_EDGE_MAP_H

#include "bw_graph/db/db.h"
#include "bw_graph/mem/vertex_set.h"

#include <algorithm>
#include <vector>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>

/**
 * @brief Execute edge_map in dense mode.
 *
 * @param db        Graph database instance
 * @param frontier  Current active vertex subset
 * @param f         Edge processing functor
 * @return Next active vertex subset
 */
template <class F>
vertex_subset_t bw_edge_map_dense(bw_graph_db_t& db, vertex_subset_t& frontier, F& f) {
  // Allocate dense output bitmap.
  v_id_t vertex_count = db.get_vertex_count();
  bool* next = newA(bool, vertex_count);
  std::fill_n(next, vertex_count, false);
  uint64_t page_count = db.disk_manager->get_page_count();

  // Scan normal CSR pages in parallel.
  tbb::parallel_for(
      tbb::blocked_range<uint64_t>(0, page_count), [&](const tbb::blocked_range<uint64_t>& range) {
        for (uint64_t pno = range.begin(); pno != range.end(); ++pno) {
          csr_page_t* page =
              db.buffer_pool->buf_page_read(static_cast<page_no_t>(pno), db.disk_manager);
          vertex_span_t vertices = page->get_vertices();
          if (vertices.empty()) {
            page->r_unlatch();
            continue;
          }
          for (uint16_t vi = 0; vi < static_cast<uint16_t>(vertices.size()); ++vi) {
            const csr_vertex_t& vertex = vertices[vi];
            if (vertex.is_deleted()) {
              continue;
            }
            v_id_t dst = vertex.vertex_id;
            if (!f.cond(dst)) {
              continue;
            }
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
            for (v_id_t src : page->get_neighbor_ptv_range(vi)) {
              if (frontier.is_in(src) && f.update_atomic(src, dst)) {
                next[dst] = true;
                break;
              }
            }
#else
              neighbor_span_t neighbors = page->get_neighbors(vi);
              for (v_id_t src : neighbors) {
                if (frontier.is_in(src) && f.update_atomic(src, dst)) {
                  next[dst] = true;
                  break;
                }
              }
#endif
          }
          page->r_unlatch();
        }
      });

  // Scan giant vertices stored outside CSR pages.
  for (v_id_t gv = 0; gv < vertex_count; ++gv) {
    const auto& item = db.vertex_index->items[gv];
    if (item.is_delete || item.vertex_type != GIANT) {
      continue;
    }
    if (!f.cond(gv)) {
      continue;
    }
    neighbor_span_t neighbors = db.giant_db->read_neighbor(gv);
    for (v_id_t src : neighbors) {
      if (frontier.is_in(src)) {
        if (f.update_atomic(src, gv)) {
          next[gv] = true;
          break;
        }
      }
    }
  }

  return vertex_subset_t(vertex_count, next);
}

/**
 * @brief Execute edge_map in sparse mode.
 *
 * @param db        Graph database instance
 * @param frontier  Current active vertex subset
 * @param f         Edge processing functor
 * @return Next active vertex subset
 */
template <class F>
vertex_subset_t bw_edge_map_sparse(bw_graph_db_t& db, vertex_subset_t& frontier, F& f) {
  v_id_t vertex_count = db.get_vertex_count();
  v_id_t active_count = static_cast<v_id_t>(frontier.num_non_zeros());
  tbb::concurrent_vector<v_id_t> output;
  const uint64_t resident_page_capacity =
      bw_graph::BW_BUFFER_CHUNK_COUNT * bw_graph::BW_BUFFER_CHUNK_SIZE;
  const bool use_page_grouping =
      db.disk_manager->get_page_count() > resident_page_capacity && active_count > 64;

  if (!use_page_grouping) {
    tbb::parallel_for(tbb::blocked_range<v_id_t>(0, active_count),
                      [&](const tbb::blocked_range<v_id_t>& range) {
                        for (v_id_t i = range.begin(); i != range.end(); ++i) {
                          v_id_t src = frontier.vtx(i);
                          auto [neighbors, page] = db.read_neighbor(src);
                          for (v_id_t dst : neighbors) {
                            if (f.cond(dst) && f.update_atomic(src, dst)) {
                              output.push_back(dst);
                            }
                          }
                          page->r_unlatch();
                        }
                      });

    v_id_t output_size = static_cast<v_id_t>(output.size());
    v_id_t* vertices = newA(v_id_t, output_size == 0 ? 1 : output_size);
    tbb::parallel_for(tbb::blocked_range<v_id_t>(0, output_size),
                      [&](const tbb::blocked_range<v_id_t>& range) {
                        for (v_id_t i = range.begin(); i < range.end(); ++i) {
                          vertices[i] = output[i];
                        }
                      });

    return vertex_subset_t(vertex_count, static_cast<long>(output_size), vertices);
  }

  struct active_vertex_t {
    page_no_t page_no;
    uint16_t offset;
    v_id_t src;
    vertex_type_t vertex_type;
  };

  std::vector<active_vertex_t> active;
  active.reserve(active_count);
  for (v_id_t i = 0; i < active_count; ++i) {
    v_id_t src = frontier.vtx(i);
    db.vertex_index->v_r_lock(src);
    protected_vertex_version_t version = db.vertex_index->protect_vertex_version(src);
    vertex_type_t vertex_type = db.vertex_index->items[src].vertex_type;
    active.push_back({version.page_no(), version.offset(), src, vertex_type});
    db.vertex_index->v_r_unlock(src);
  }

  std::sort(active.begin(), active.end(), [](const active_vertex_t& lhs,
                                             const active_vertex_t& rhs) {
    if (lhs.page_no != rhs.page_no) {
      return lhs.page_no < rhs.page_no;
    }
    return lhs.offset < rhs.offset;
  });

  std::vector<size_t> group_starts;
  group_starts.reserve(active.size());
  for (size_t i = 0; i < active.size();) {
    group_starts.push_back(i);
    page_no_t page_no = active[i].page_no;
    do {
      ++i;
    } while (i < active.size() && active[i].page_no == page_no);
  }

  tbb::parallel_for(tbb::blocked_range<size_t>(0, group_starts.size()),
                    [&](const tbb::blocked_range<size_t>& range) {
                      for (size_t gi = range.begin(); gi != range.end(); ++gi) {
                        size_t begin = group_starts[gi];
                        size_t end =
                            (gi + 1 < group_starts.size()) ? group_starts[gi + 1] : active.size();

                        csr_page_t* page = db.buffer_pool->buf_page_read(active[begin].page_no,
                                                                          db.disk_manager);
                        for (size_t i = begin; i < end; ++i) {
                          const active_vertex_t& item = active[i];
                          if (item.vertex_type == GIANT) {
                            neighbor_span_t neighbors = db.giant_db->read_neighbor(item.src);
                            for (v_id_t dst : neighbors) {
                              if (f.cond(dst) && f.update_atomic(item.src, dst)) {
                                output.push_back(dst);
                              }
                            }
                            continue;
                          }

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
                          for (v_id_t dst : page->get_neighbor_ptv_range(item.offset)) {
                            if (f.cond(dst) && f.update_atomic(item.src, dst)) {
                              output.push_back(dst);
                            }
                          }
#else
                          neighbor_span_t neighbors = page->get_neighbors(item.offset);
                          for (v_id_t dst : neighbors) {
                            if (f.cond(dst) && f.update_atomic(item.src, dst)) {
                              output.push_back(dst);
                            }
                          }
#endif
                        }
                        page->r_unlatch();
                      }
                    });

  // Materialize sparse output vertices.
  v_id_t output_size = static_cast<v_id_t>(output.size());
  v_id_t* vertices = newA(v_id_t, output_size == 0 ? 1 : output_size);
  tbb::parallel_for(tbb::blocked_range<v_id_t>(0, output_size),
                    [&](const tbb::blocked_range<v_id_t>& range) {
                      for (v_id_t i = range.begin(); i < range.end(); ++i) {
                        vertices[i] = output[i];
                      }
                    });

  return vertex_subset_t(vertex_count, static_cast<long>(output_size), vertices);
}

/**
 * @brief Apply edge_map without generating an output frontier.
 *
 * @param db        Graph database instance
 * @param frontier  Current active vertex subset
 * @param f         Edge processing functor
 * @return None
 */
template <class F>
void bw_edge_map_no_output(bw_graph_db_t& db, vertex_subset_t& frontier, F f) {
  if (frontier.is_empty()) {
    return;
  }

  if (frontier.dense()) {
    // Scan all pages when the frontier is dense.
    uint64_t page_count = db.disk_manager->get_page_count();
    tbb::parallel_for(tbb::blocked_range<uint64_t>(0, page_count),
                      [&](const tbb::blocked_range<uint64_t>& range) {
                        for (uint64_t pno = range.begin(); pno != range.end(); ++pno) {
                          csr_page_t* page = db.buffer_pool->buf_page_read(
                              static_cast<page_no_t>(pno), db.disk_manager);
                          vertex_span_t vertices = page->get_vertices();
                          if (vertices.empty()) {
                            page->r_unlatch();
                            continue;
                          }
                          for (uint16_t vi = 0; vi < static_cast<uint16_t>(vertices.size()); ++vi) {
                            const csr_vertex_t& vertex = vertices[vi];
                            if (vertex.is_deleted()) {
                              continue;
                            }
                            v_id_t src = vertex.vertex_id;
                            if (!frontier.is_in(src)) {
                              continue;
                            }
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
                            for (v_id_t dst : page->get_neighbor_ptv_range(vi)) {
                              if (f.cond(dst)) {
                                f.update_atomic(src, dst);
                              }
                            }
#else
                neighbor_span_t neighbors = page->get_neighbors(vi);
                for (v_id_t dst : neighbors) {
                  if (f.cond(dst)) {
                    f.update_atomic(src, dst);
                  }
                }
#endif
                          }
                          page->r_unlatch();
                        }
                      });

    // Scan giant vertices separately.
    v_id_t vertex_count = db.get_vertex_count();
    for (v_id_t gv = 0; gv < vertex_count; ++gv) {
      const auto& item = db.vertex_index->items[gv];
      if (item.is_delete || item.vertex_type != GIANT) {
        continue;
      }
      if (!frontier.is_in(gv)) {
        continue;
      }
      neighbor_span_t neighbors = db.giant_db->read_neighbor(gv);
      for (v_id_t dst : neighbors) {
        if (f.cond(dst)) {
          f.update_atomic(gv, dst);
        }
      }
    }
  } else {
    // Scan only active source vertices when sparse.
    frontier.to_sparse();
    long active_count = static_cast<long>(frontier.num_non_zeros());
    tbb::parallel_for(tbb::blocked_range<long>(0, active_count),
                      [&](const tbb::blocked_range<long>& range) {
                        for (long i = range.begin(); i < range.end(); ++i) {
                          v_id_t src = frontier.vtx(static_cast<v_id_t>(i));
                          auto [neighbors, page] = db.read_neighbor(src);
                          for (v_id_t dst : neighbors) {
                            if (f.cond(dst)) {
                              f.update_atomic(src, dst);
                            }
                          }
                          page->r_unlatch();
                        }
                      });
  }
}

/**
 * @brief Execute edge_map with adaptive sparse/dense selection.
 *
 * @param db                 Graph database instance
 * @param frontier           Current active vertex subset
 * @param f                  Edge processing functor
 * @param threshold_divisor  Divisor used to choose sparse or dense mode
 * @return Next active vertex subset
 */
template <class F>
vertex_subset_t bw_edge_map(bw_graph_db_t& db, vertex_subset_t& frontier, F f,
                            long threshold_divisor = 20) {
  if (frontier.is_empty()) {
    return vertex_subset_t(static_cast<size_t>(db.get_vertex_count()));
  }

  v_id_t vertex_count = db.get_vertex_count();
  long active_count = static_cast<long>(frontier.num_non_zeros());
  uint64_t total_edges = db.get_total_edge_count();
  long threshold = total_edges > 0 ? static_cast<long>(total_edges / threshold_divisor)
                                   : static_cast<long>(vertex_count) / threshold_divisor;

  // Use dense mode for large frontiers.
  if (active_count > threshold) {
    frontier.to_dense();
    return bw_edge_map_dense(db, frontier, f);
  }

  // Use sparse mode for small frontiers.
  frontier.to_sparse();
  return bw_edge_map_sparse(db, frontier, f);
}

#endif // BW_GRAPH_ALGO_EDGE_MAP_H
