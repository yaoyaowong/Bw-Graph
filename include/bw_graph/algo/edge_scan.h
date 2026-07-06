#ifndef BW_GRAPH_ALGO_EDGE_SCAN_H
#define BW_GRAPH_ALGO_EDGE_SCAN_H

#include "bw_graph/common/bitmap.h"
#include "bw_graph/db/db.h"

#include <algorithm>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>
#include <tbb/task_arena.h>

inline bool bw_should_use_page_local_edge_scan(uint64_t page_count,
                                               uint64_t resident_page_capacity) {
  return page_count > resident_page_capacity;
}

template <typename T>
T bw_page_local_edge_scan(bw_graph_db_t& db, std::function<T(v_id_t src, v_id_t dst)> process,
                          bit_map_t* active_bitmap, T zero, int num_threads);

/**
 * @brief Atomic add for float
 *
 * @param ptr  Pointer to the target value
 * @param val  Value to add
 * @return None
 */
inline void write_add(float* ptr, float val) {
  float old_val, new_val;
  do {
    // Retry until the CAS succeeds.
    old_val = *ptr;
    new_val = old_val + val;
  } while (!__sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(ptr),
                                         *reinterpret_cast<uint32_t*>(&old_val),
                                         *reinterpret_cast<uint32_t*>(&new_val)));
}

/**
 * @brief Atomic add for uint32_t
 *
 * @param ptr  Pointer to the target value
 * @param val  Value to add
 * @return None
 */
inline void write_add(uint32_t* ptr, uint32_t val) { __sync_fetch_and_add(ptr, val); }

/**
 * @brief Scan edges and reduce the processed results.
 *
 * @param db             Graph database instance
 * @param process        Edge processing function
 * @param active_bitmap  Optional bitmap for active source vertices
 * @param zero           Initial reduction value
 * @param num_threads    Requested worker thread count, 0 for default
 * @return Reduced scan result
 */
template <typename T>
T bw_edge_scan(bw_graph_db_t& db, std::function<T(v_id_t src, v_id_t dst)> process,
               bit_map_t* active_bitmap = nullptr, T zero = T(), int num_threads = 0) {
  int max_threads = static_cast<int>(std::thread::hardware_concurrency());
  int actual_threads = (num_threads <= 0) ? 0 : std::min(num_threads, max_threads);

  auto do_scan = [&]() -> T {
    if (active_bitmap == nullptr) {
      // Scan all source vertices.
      return tbb::parallel_reduce(
          tbb::blocked_range<v_id_t>(0, db.get_vertex_count()), zero,
          [&](const tbb::blocked_range<v_id_t>& range, T local_result) -> T {
            for (v_id_t src = range.begin(); src != range.end(); ++src) {
              auto [neighbors_span, csr_page] = db.read_neighbor(src);
              for (v_id_t dst : neighbors_span) {
                local_result += process(src, dst);
              }
              csr_page->r_unlatch();
            }
            return local_result;
          },
          [](T a, T b) -> T { return a + b; });
    }

    // Scan only vertices marked in the bitmap.
    const uint64_t resident_page_capacity =
        bw_graph::BW_BUFFER_CHUNK_COUNT * bw_graph::BW_BUFFER_CHUNK_SIZE;
    if (bw_should_use_page_local_edge_scan(db.disk_manager->get_page_count(),
                                           resident_page_capacity)) {
      return bw_page_local_edge_scan<T>(db, process, active_bitmap, zero, num_threads);
    }

    size_t num_words = (db.get_vertex_count() + 63) / 64;
    return tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, num_words), zero,
        [&](const tbb::blocked_range<size_t>& range, T local_result) -> T {
          for (size_t word_idx = range.begin(); word_idx != range.end(); ++word_idx) {
            uint64_t word = active_bitmap->data[word_idx];
            if (word == 0) {
              continue;
            }

            v_id_t base_vid = word_idx * 64;
            for (int bit = 0; bit < 64; ++bit) {
              if (word & (1UL << bit)) {
                v_id_t src = base_vid + bit;
                if (src >= db.get_vertex_count()) {
                  break;
                }
                auto [neighbors_span, csr_page] = db.read_neighbor(src);
                for (v_id_t dst : neighbors_span) {
                  local_result += process(src, dst);
                }
                csr_page->r_unlatch();
              }
            }
          }
          return local_result;
        },
        [](T a, T b) -> T { return a + b; });
  };

  // Use TBB default scheduling when no explicit thread count is given.
  if (actual_threads == 0) {
    return do_scan();
  }

  // Run inside a bounded task arena when thread count is specified.
  tbb::task_arena arena(actual_threads);
  return arena.execute([&]() { return do_scan(); });
}

template <typename T>
T bw_page_local_edge_scan(bw_graph_db_t& db, std::function<T(v_id_t src, v_id_t dst)> process,
                          bit_map_t* active_bitmap, T zero, int num_threads) {
  if (active_bitmap == nullptr) {
    return bw_edge_scan<T>(db, process, nullptr, zero, num_threads);
  }

  struct active_vertex_t {
    page_no_t page_no;
    uint16_t offset;
    v_id_t src;
    vertex_type_t vertex_type;
  };

  const uint64_t page_count = db.disk_manager->get_page_count();
  static thread_local std::vector<active_vertex_t> active_vertices;
  static thread_local std::vector<page_no_t> active_pages;
  static thread_local std::vector<size_t> page_counts;
  static thread_local std::vector<uint8_t> page_seen;
  active_vertices.clear();
  active_pages.clear();
  page_counts.assign(page_count, 0);
  page_seen.assign(page_count, 0);

  const v_id_t vertex_count = db.get_vertex_count();
  const size_t num_words = (vertex_count + 63) / 64;
  for (size_t word_idx = 0; word_idx < num_words; ++word_idx) {
    uint64_t word = active_bitmap->data[word_idx];
    while (word != 0) {
      const int bit = __builtin_ctzll(word);
      const v_id_t src = static_cast<v_id_t>(word_idx * 64 + bit);
      if (src >= vertex_count) {
        break;
      }
      db.vertex_index->v_r_lock(src);
      protected_vertex_version_t version = db.vertex_index->protect_vertex_version(src);
      vertex_type_t vertex_type = db.vertex_index->items[src].vertex_type;
      const page_no_t page_no = version.page_no();
      if (page_no < page_count) {
        if (!page_seen[page_no]) {
          page_seen[page_no] = 1;
          active_pages.push_back(page_no);
        }
        ++page_counts[page_no];
        active_vertices.push_back({page_no, version.offset(), src, vertex_type});
      }
      db.vertex_index->v_r_unlock(src);
      word &= word - 1;
    }
  }

  if (active_vertices.empty()) {
    return zero;
  }

  std::sort(active_pages.begin(), active_pages.end());

  static thread_local std::vector<size_t> page_offsets;
  page_offsets.assign(page_count + 1, 0);
  for (uint64_t page_no = 0; page_no < page_count; ++page_no) {
    page_offsets[page_no + 1] = page_offsets[page_no] + page_counts[page_no];
  }

  static thread_local std::vector<size_t> page_cursors;
  static thread_local std::vector<active_vertex_t> grouped_vertices;
  page_cursors.resize(page_offsets.size());
  std::copy(page_offsets.begin(), page_offsets.end(), page_cursors.begin());
  grouped_vertices.resize(active_vertices.size());
  for (const active_vertex_t& item : active_vertices) {
    grouped_vertices[page_cursors[item.page_no]++] = item;
  }

  const std::vector<page_no_t>* active_pages_scan = &active_pages;
  const std::vector<size_t>* page_offsets_scan = &page_offsets;
  const std::vector<active_vertex_t>* grouped_vertices_scan = &grouped_vertices;

  auto do_scan = [&, active_pages_scan, page_offsets_scan, grouped_vertices_scan]() -> T {
    return tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, active_pages_scan->size()), zero,
        [&](const tbb::blocked_range<size_t>& range, T local_result) -> T {
          for (size_t active_page_idx = range.begin(); active_page_idx != range.end();
               ++active_page_idx) {
            const page_no_t page_no = (*active_pages_scan)[active_page_idx];
            const size_t begin = (*page_offsets_scan)[page_no];
            const size_t end = (*page_offsets_scan)[page_no + 1];
            csr_page_t* page = db.buffer_pool->buf_page_read(page_no, db.disk_manager);
            for (size_t idx = begin; idx < end; ++idx) {
              const active_vertex_t& item = (*grouped_vertices_scan)[idx];
              if (item.vertex_type == GIANT) {
                neighbor_span_t neighbors = db.giant_db->read_neighbor(item.src);
                for (v_id_t dst : neighbors) {
                  local_result += process(item.src, dst);
                }
                continue;
              }

#ifdef BWGRAPH_NEIGHBOR_COMPRESS
              for (v_id_t dst : page->get_neighbor_ptv_range(item.offset)) {
                local_result += process(item.src, dst);
              }
#else
              neighbor_span_t neighbors = page->get_neighbors(item.offset);
              for (v_id_t dst : neighbors) {
                local_result += process(item.src, dst);
              }
#endif
            }
            page->r_unlatch();
          }
          return local_result;
        },
        [](T a, T b) -> T { return a + b; });
  };

  int max_threads = static_cast<int>(std::thread::hardware_concurrency());
  int actual_threads = (num_threads <= 0) ? 0 : std::min(num_threads, max_threads);
  if (actual_threads == 0) {
    return do_scan();
  }

  tbb::task_arena arena(actual_threads);
  return arena.execute([&]() { return do_scan(); });
}

/**
 * @brief Scan edges page by page and reduce the processed results.
 *        [Unused Now][Try to Optimize]
 * @param db             Graph database instance
 * @param process        Edge processing function
 * @param active_bitmap  Optional bitmap for active source vertices
 * @param zero           Initial reduction value
 * @param num_threads    Requested worker thread count, 0 for default
 * @return Reduced scan result
 */
template <typename T>
T bw_paged_edge_scan(bw_graph_db_t& db, std::function<T(v_id_t src, v_id_t dst)> process,
                     bit_map_t* active_bitmap = nullptr, T zero = T(), int num_threads = 0) {
  int max_threads = static_cast<int>(std::thread::hardware_concurrency());
  int actual_threads = (num_threads <= 0) ? 0 : std::min(num_threads, max_threads);

  // Collect giant vertices stored outside normal pages.
  std::vector<v_id_t> giant_vertices;
  v_id_t vertex_count = db.get_vertex_count();
  for (v_id_t vid = 0; vid < vertex_count; ++vid) {
    const auto& item = db.vertex_index->items[vid];
    if (!item.is_delete && item.vertex_type == GIANT) {
      giant_vertices.push_back(vid);
    }
  }

  uint64_t page_count = db.disk_manager->get_page_count();

  auto do_scan = [&]() -> T {
    // Scan normal CSR pages in parallel.
    T result = tbb::parallel_reduce(
        tbb::blocked_range<uint64_t>(0, page_count), zero,
        [&](const tbb::blocked_range<uint64_t>& range, T local_result) -> T {
          for (uint64_t pno = range.begin(); pno != range.end(); ++pno) {
            csr_page_t* page =
                db.buffer_pool->buf_page_read(static_cast<page_no_t>(pno), db.disk_manager);
            vertex_span_t vertices = page->get_vertices();
            if (vertices.empty()) {
              page->r_unlatch();
              continue;
            }
            uint16_t num_vertices = static_cast<uint16_t>(vertices.size());
            for (uint16_t vi = 0; vi < num_vertices; ++vi) {
              const csr_vertex_t& vertex = vertices[vi];
              if (vertex.is_deleted()) {
                continue;
              }
              if (active_bitmap != nullptr) {
                if (!(active_bitmap->data[vertex.vertex_id >> 6] &
                      (1UL << (vertex.vertex_id & 63)))) {
                  continue;
                }
              }
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
              for (v_id_t dst : page->get_neighbor_ptv_range(vi)) {
                local_result += process(vertex.vertex_id, dst);
              }
#else
              neighbor_span_t neighbors = page->get_neighbors(vi);
              for (v_id_t dst : neighbors) {
                local_result += process(vertex.vertex_id, dst);
              }
#endif
            }
            page->r_unlatch();
          }
          return local_result;
        },
        [](T a, T b) -> T { return a + b; });

    // Scan giant vertices after paged traversal.
    for (v_id_t gv : giant_vertices) {
      if (active_bitmap != nullptr) {
        if (!(active_bitmap->data[gv >> 6] & (1UL << (gv & 63)))) {
          continue;
        }
      }
      neighbor_span_t neighbors = db.giant_db->read_neighbor(gv);
      for (v_id_t dst : neighbors) {
        result += process(gv, dst);
      }
    }

    return result;
  };

  // Use TBB default scheduling when no explicit thread count is given.
  if (actual_threads == 0) {
    return do_scan();
  }

  // Run inside a bounded task arena when thread count is specified.
  tbb::task_arena arena(actual_threads);
  return arena.execute([&]() { return do_scan(); });
}

#endif // BW_GRAPH_ALGO_EDGE_SCAN_H
