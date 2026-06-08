#include "bw_graph/buf/sketch.h"

#include <cassert>
#include <cstdint>
#include <stdexcept>

// vertex_stat_entry_t implementation
void vertex_stat_entry_t::increment_read() { read_count.fetch_add(1, std::memory_order_relaxed); }

// Handle increment write.
void vertex_stat_entry_t::increment_write() { write_count.fetch_add(1, std::memory_order_relaxed); }

// Get read count.
uint64_t vertex_stat_entry_t::get_read_count() const {
  return read_count.load(std::memory_order_relaxed);
}

// Get write count.
uint64_t vertex_stat_entry_t::get_write_count() const {
  return write_count.load(std::memory_order_relaxed);
}

// Get write ratio.
double vertex_stat_entry_t::get_write_ratio() const {
  uint64_t reads = read_count.load(std::memory_order_relaxed);
  uint64_t writes = write_count.load(std::memory_order_relaxed);
  uint64_t total = reads + writes;

  if (total == 0) {
    return 0.0;
  }

  return static_cast<double>(writes) / static_cast<double>(total);
}

// vertex_update_sketch_t implementation
vertex_update_sketch_t::vertex_update_sketch_t(uint64_t vertex_count) : vertex_count(vertex_count) {
  if (vertex_count == 0) {
    throw std::invalid_argument("Vertex count must be greater than 0");
  }
  vertex_stats = new vertex_stat_entry_t[vertex_count];
}

// Handle record read.
void vertex_update_sketch_t::record_read(v_id_t vertex_id) {
  if (!is_valid_vertex_id(vertex_id)) {
    return;
  }
  vertex_stats[vertex_id].increment_read();
}

// Handle record write.
void vertex_update_sketch_t::record_write(v_id_t vertex_id) {
  if (!is_valid_vertex_id(vertex_id)) {
    return;
  }
  vertex_stats[vertex_id].increment_write();
}

// Get reservation level.
uint8_t vertex_update_sketch_t::get_reservation_level(v_id_t vertex_id) const {
  if (!is_valid_vertex_id(vertex_id)) {
    return 0;
  }

  double write_ratio = vertex_stats[vertex_id].get_write_ratio();
  return calculate_reservation_level(write_ratio);
}

// Per-vertex reservation level (alias for clarity at call sites).
uint8_t vertex_update_sketch_t::get_per_vertex_reservation(v_id_t vertex_id) const {
  return get_reservation_level(vertex_id);
}

// Workload-driven trigger threshold (lock-free, hot-path safe).
uint64_t vertex_update_sketch_t::get_consolidation_threshold(v_id_t vertex_id,
                                                             uint64_t base_threshold) const {
  // Out-of-range or missing data falls back to the yaml baseline.
  if (!is_valid_vertex_id(vertex_id)) {
    return base_threshold == 0 ? 1 : base_threshold;
  }

  uint64_t reads = vertex_stats[vertex_id].get_read_count();
  uint64_t writes = vertex_stats[vertex_id].get_write_count();
  uint64_t total = reads + writes;

  // Not enough samples yet: keep behaviour predictable.
  if (total < MIN_SAMPLES_FOR_ADAPT) {
    return base_threshold == 0 ? 1 : base_threshold;
  }

  double write_ratio = static_cast<double>(writes) / static_cast<double>(total);

  // Very read-heavy: fire as soon as a single delta lands so reads do not
  // pay the merge cost.  We deliberately ignore base_threshold here so the
  // adaptive promise (threshold == 1 for read-only workloads) holds even
  // when the user picked a large yaml baseline.
  if (write_ratio < THRESHOLD_LEVEL_1) {
    return 1;
  }

  double factor = THRESHOLD_FACTOR_LV1;
  if (write_ratio >= THRESHOLD_LEVEL_4) {
    factor = THRESHOLD_FACTOR_LV4;
  } else if (write_ratio >= THRESHOLD_LEVEL_3) {
    factor = THRESHOLD_FACTOR_LV3;
  } else if (write_ratio >= THRESHOLD_LEVEL_2) {
    factor = THRESHOLD_FACTOR_LV2;
  }

  uint64_t baseline = base_threshold == 0 ? 1 : base_threshold;
  double scaled = static_cast<double>(baseline) * factor;
  uint64_t out = static_cast<uint64_t>(scaled);
  return out == 0 ? 1 : out;
}

// Get reservation levels.
uint8_t
vertex_update_sketch_t::get_reservation_levels(const std::vector<v_id_t>& vertex_ids) const {
  if (vertex_ids.empty()) {
    return 0;
  }
  uint64_t total_reads = 0;
  uint64_t total_writes = 0;

  for (v_id_t vertex_id : vertex_ids) {
    if (is_valid_vertex_id(vertex_id)) {
      total_reads += vertex_stats[vertex_id].get_read_count();
      total_writes += vertex_stats[vertex_id].get_write_count();
    }
  }

  uint64_t total_ops = total_reads + total_writes;
  if (total_ops == 0) {
    return 0;
  }

  double overall_write_ratio = static_cast<double>(total_writes) / static_cast<double>(total_ops);

  return calculate_reservation_level(overall_write_ratio);
}

// Get read count.
uint64_t vertex_update_sketch_t::get_read_count(v_id_t vertex_id) const {
  if (!is_valid_vertex_id(vertex_id)) {
    return 0;
  }
  return vertex_stats[vertex_id].get_read_count();
}

// Get write count.
uint64_t vertex_update_sketch_t::get_write_count(v_id_t vertex_id) const {
  if (!is_valid_vertex_id(vertex_id)) {
    return 0;
  }
  return vertex_stats[vertex_id].get_write_count();
}

// Get write ratio.
double vertex_update_sketch_t::get_write_ratio(v_id_t vertex_id) const {
  if (!is_valid_vertex_id(vertex_id)) {
    return 0.0;
  }
  return vertex_stats[vertex_id].get_write_ratio();
}

// Reset vertex stats.
void vertex_update_sketch_t::reset_vertex_stats(v_id_t vertex_id) {
  if (!is_valid_vertex_id(vertex_id)) {
    return;
  }

  vertex_stats[vertex_id].read_count.store(0, std::memory_order_relaxed);
  vertex_stats[vertex_id].write_count.store(0, std::memory_order_relaxed);
}

// Reset all stats.
void vertex_update_sketch_t::reset_all_stats() {
  for (uint64_t i = 0; i < vertex_count; ++i) {
    vertex_stats[i].read_count.store(0, std::memory_order_relaxed);
    vertex_stats[i].write_count.store(0, std::memory_order_relaxed);
  }
}

vertex_update_sketch_t::~vertex_update_sketch_t() { delete[] vertex_stats; }

// Get vertex count.
v_id_t vertex_update_sketch_t::get_vertex_count() const { return vertex_count; }

// Calculate reservation level.
uint8_t vertex_update_sketch_t::calculate_reservation_level(double write_ratio) const {
  if (write_ratio < THRESHOLD_LEVEL_1) {
    return 0;
  } else if (write_ratio < THRESHOLD_LEVEL_2) {
    return 1;
  } else if (write_ratio < THRESHOLD_LEVEL_3) {
    return 2;
  } else if (write_ratio < THRESHOLD_LEVEL_4) {
    return 3;
  } else {
    return 4;
  }
}

// Check valid vertex id.
bool vertex_update_sketch_t::is_valid_vertex_id(v_id_t vertex_id) const {
  return vertex_id < vertex_count;
}