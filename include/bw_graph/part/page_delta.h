#ifndef BW_GRAPH_PART_PAGE_DELTA_H
#define BW_GRAPH_PART_PAGE_DELTA_H

#include "bw_graph/common/type.h"
#include "bw_graph/index/vertex_index.h"

#include <folly/container/F14Map.h>
#include <mutex>

/**
 * @brief Forwarding table installed on a CSR page that is being consolidated.
 *
 * Between consolidation phases, concurrent writers must not append deltas to
 * the chain of the old CSR page because that chain has already been drained.
 * Instead, every writer reads the page_delta_t attached to the old page,
 * looks up the new CSR page number assigned to its source vertex, and writes
 * the delta into the new page's chain.
 *
 * The struct is sized to be cheap to install/uninstall:
 *   - forward_map_      : v_id -> new CSR page_no for that vertex.
 *   - rerouted_head_    : v_id -> first delta record this writer prepended
 *                          onto the new chain during the consolidation window.
 *                          On finalize, the next_* field of that record is
 *                          reset to INVALID so the chain stops just before
 *                          the old (already-merged) delta records.
 *   - mutex_            : protects rerouted_head_ during concurrent writes.
 */
struct page_delta_t {
  /** Vertex -> target new CSR page_no. */
  folly::F14FastMap<v_id_t, page_no_t> forward_map_;

  /**
   * For each vertex that received at least one delta during the consolidation
   * window, records the (page_no, record_idx) of the FIRST delta the writer
   * prepended onto the new chain.  That record's next_* link must be cleared
   * when the consolidation finalises, because the new CSR page already
   * embeds every delta that lived in the old chain.
   */
  folly::F14FastMap<v_id_t, delta_head_t> rerouted_tail_;

  /** Serialises updates to rerouted_tail_. */
  std::mutex rerouted_mutex_;

  /**
   * @brief Look up the new CSR page assigned to vertex v.
   * @param v       Source vertex of the incoming delta record.
   * @param out_pno Receives the new page_no on hit.
   * @return true on hit, false on miss (caller must fall back to old path).
   */
  bool lookup_forward(v_id_t v, page_no_t& out_pno) const {
    auto it = forward_map_.find(v);
    if (it == forward_map_.end()) {
      return false;
    }
    out_pno = it->second;
    return true;
  }

  /**
   * @brief Record the location of the first rerouted delta for vertex v.
   *
   * Idempotent: only the first call for a given v is honoured, because every
   * later record is appended in front of the first one and therefore reaches
   * the same tail.
   * @param v       Source vertex.
   * @param tail    Delta record location written for v.
   * @return None.
   */
  void note_rerouted_tail(v_id_t v, delta_head_t tail) {
    std::lock_guard<std::mutex> guard(rerouted_mutex_);
    rerouted_tail_.try_emplace(v, tail);
  }
};

#endif // BW_GRAPH_PART_PAGE_DELTA_H
