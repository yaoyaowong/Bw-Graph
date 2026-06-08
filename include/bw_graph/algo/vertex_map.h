#ifndef BW_GRAPH_ALGO_VERTEX_MAP_H
#define BW_GRAPH_ALGO_VERTEX_MAP_H

#include "bw_graph/common/type.h"
#include "bw_graph/mem/vertex_set.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

/**
 * @brief Apply a function to each active vertex in the frontier.
 *
 * @param frontier  Current active vertex subset
 * @param f         Vertex processing functor
 * @return None
 */
template <class F>
void bw_vertex_map(vertex_subset_t& frontier, F f) {
  if (frontier.is_empty()) {
    return;
  }

  if (frontier.dense()) {
    // Visit all active vertices in dense mode.
    long vertex_count = static_cast<long>(frontier.num_vertices());
    tbb::parallel_for(tbb::blocked_range<long>(0, vertex_count),
                      [&](const tbb::blocked_range<long>& range) {
                        for (long v = range.begin(); v < range.end(); ++v) {
                          if (frontier.is_in(static_cast<v_id_t>(v))) {
                            f(static_cast<v_id_t>(v));
                          }
                        }
                      });
  } else {
    // Visit only stored active vertices in sparse mode.
    long active_count = static_cast<long>(frontier.num_non_zeros());
    tbb::parallel_for(tbb::blocked_range<long>(0, active_count),
                      [&](const tbb::blocked_range<long>& range) {
                        for (long i = range.begin(); i < range.end(); ++i) {
                          f(frontier.vtx(static_cast<v_id_t>(i)));
                        }
                      });
  }
}

#endif // BW_GRAPH_ALGO_VERTEX_MAP_H
