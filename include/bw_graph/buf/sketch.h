#ifndef BW_GRAPH_BUF_SKETCH_H
#define BW_GRAPH_BUF_SKETCH_H

#include "bw_graph/common/type.h"

#include <atomic>
#include <cstdint>
#include <vector>

/**
 * @brief Vertex statistics entry
 */
struct vertex_stat_entry_t {
  /* Atomic counter for read operations */
  std::atomic<uint64_t> read_count{0};

  /* Atomic counter for write operations */
  std::atomic<uint64_t> write_count{0};

  /**
   * @brief Default constructor
   */
  vertex_stat_entry_t() = default;

  /**
   * @brief Increment read count
   */
  void increment_read();

  /**
   * @brief Increment write count
   */
  void increment_write();

  /**
   * @brief Get current read count
   * @return Current read count
   */
  uint64_t get_read_count() const;

  /**
   * @brief Get current write count
   * @return Current write count
   */
  uint64_t get_write_count() const;

  /**
   * @brief Calculate write ratio (writes / (reads + writes))
   * @return Write ratio in range [0.0, 1.0]
   */
  double get_write_ratio() const;
};

/**
 * @brief Vertex update sketch structure for tracking vertex access patterns
 */
struct vertex_update_sketch_t {
private:
  /* Pointer to vertex statistics entries array */
  vertex_stat_entry_t* vertex_stats;

  /* Total number of vertices */
  uint64_t vertex_count;

  /* Threshold constants for space reservation levels */
  static constexpr double THRESHOLD_LEVEL_1 = 0.1;  // 10% writes
  static constexpr double THRESHOLD_LEVEL_2 = 0.25; // 25% writes
  static constexpr double THRESHOLD_LEVEL_3 = 0.5;  // 50% writes
  static constexpr double THRESHOLD_LEVEL_4 = 0.75; // 75% writes

  /**
   * Adaptive consolidation threshold tuning.
   *
   * Read-heavy vertices want consolidation to fire as soon as the chain
   * forms (length == 1) so subsequent reads do not pay any merge cost.
   * Write-heavy vertices want consolidation to wait until the chain is
   * meaningfully longer so we do not pay the SMO cost only to immediately
   * dirty the new page again.  In between we scale the user-supplied yaml
   * baseline (BW_GRAPH_MAX_DELTA_CHAIN_LENGTH) by a workload factor.
   */
  static constexpr uint64_t MIN_SAMPLES_FOR_ADAPT = 8;
  static constexpr double THRESHOLD_FACTOR_LV1 = 1.0;
  static constexpr double THRESHOLD_FACTOR_LV2 = 1.5;
  static constexpr double THRESHOLD_FACTOR_LV3 = 3.0;
  static constexpr double THRESHOLD_FACTOR_LV4 = 5.0;

public:
  /**
   * @brief Constructor for vertex_update_sketch_t
   * @param vertex_count Total number of vertices to track
   */
  explicit vertex_update_sketch_t(uint64_t vertex_count);

  /**
   * @brief Destructor for vertex_update_sketch_t
   */
  ~vertex_update_sketch_t();

  /**
   * @brief Increment read count for a specific vertex
   * @param vertex_id The vertex ID to update
   */
  void record_read(v_id_t vertex_id);

  /**
   * @brief Increment write count for a specific vertex
   * @param vertex_id The vertex ID to update
   */
  void record_write(v_id_t vertex_id);

  /**
   * @brief Get space reservation level for a single vertex
   * @param vertex_id The vertex ID to query
   * @return Space reservation level (0-4)
   *         0: No reservation (read-heavy)
   *         1: Reserve 1 slot
   *         2: Reserve 2 slots
   *         3: Reserve 3 slots
   *         4: Reserve 4 slots (write-heavy)
   */
  uint8_t get_reservation_level(v_id_t vertex_id) const;

  /**
   * @brief Get space reservation levels for a batch of vertices
   * @param vertex_ids Vector of vertex IDs to query
   * @return Vector of space reservation levels corresponding to input vertices
   */
  uint8_t get_reservation_levels(const std::vector<v_id_t>& vertex_ids) const;

  /**
   * @brief Workload-driven consolidation trigger threshold for one vertex.
   *
   * The trigger lives on the writer hot path, so this call must stay
   * lock-free and O(1).  We look at the per-vertex (read,write) counters
   * collected so far and scale the user-supplied baseline:
   *
   *   - very read-heavy  -> 1                 (drain ASAP)
   *   - mildly read-heavy -> base * 1.0       (use yaml baseline)
   *   - balanced          -> base * 1.5
   *   - write-leaning     -> base * 3.0
   *   - very write-heavy  -> base * 5.0       (defer merge)
   *
   * Vertices without enough samples fall back to the baseline so cold
   * pages keep predictable behaviour. The returned threshold is always
   * >= 1 to preserve the invariant in db_dyn.
   *
   * @param vertex_id Source vertex of the incoming write.
   * @param base_threshold yaml-driven BW_GRAPH_MAX_DELTA_CHAIN_LENGTH.
   * @return Adaptive trigger threshold (>= 1).
   */
  uint64_t get_consolidation_threshold(v_id_t vertex_id, uint64_t base_threshold) const;

  /**
   * @brief Per-vertex slot reservation level (0-4).
   *
   * Same scale as get_reservation_level but exposed under an explicit name
   * so call sites that want adaptive per-vertex slot allocation read more
   * clearly.  Equivalent to get_reservation_level(vertex_id).
   * @param vertex_id Vertex to query.
   * @return Reservation level in [0, 4].
   */
  uint8_t get_per_vertex_reservation(v_id_t vertex_id) const;

  /**
   * @brief Get read count for a specific vertex
   * @param vertex_id The vertex ID to query
   * @return Current read count
   */
  uint64_t get_read_count(v_id_t vertex_id) const;

  /**
   * @brief Get write count for a specific vertex
   * @param vertex_id The vertex ID to query
   * @return Current write count
   */
  uint64_t get_write_count(v_id_t vertex_id) const;

  /**
   * @brief Get write ratio for a specific vertex
   * @param vertex_id The vertex ID to query
   * @return Write ratio in range [0.0, 1.0]
   */
  double get_write_ratio(v_id_t vertex_id) const;

  /**
   * @brief Reset statistics for a specific vertex
   * @param vertex_id The vertex ID to reset
   */
  void reset_vertex_stats(v_id_t vertex_id);

  /**
   * @brief Reset all statistics
   */
  void reset_all_stats();

  /**
   * @brief Get total vertex count
   * @return Total number of vertices being tracked
   */
  v_id_t get_vertex_count() const;

private:
  /**
   * @brief Calculate reservation level based on write ratio
   * @param write_ratio The write ratio to evaluate
   * @return Space reservation level (0-4)
   */
  uint8_t calculate_reservation_level(double write_ratio) const;

  /**
   * @brief Validate vertex ID is within valid range
   * @param vertex_id The vertex ID to validate
   * @return true if valid, false otherwise
   */
  bool is_valid_vertex_id(v_id_t vertex_id) const;
};

#endif // BW_GRAPH_BUF_SKETCH_H