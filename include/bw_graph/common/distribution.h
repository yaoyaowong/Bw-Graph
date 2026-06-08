#ifndef BW_GRAPH_DISTRIBUTION_H
#define BW_GRAPH_DISTRIBUTION_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

/**
 * @brief Normalize degree distribution to ensure probabilities sum to 1.0
 * @param distribution The degree distribution to normalize
 * @return Normalized distribution where percentages sum to 100.0
 */
inline std::vector<std::pair<uint64_t, double>>
normalize_distribution(const std::vector<std::pair<uint64_t, double>>& distribution) {
  if (distribution.empty()) {
    return {};
  }

  // Calculate total percentage
  double total = 0.0;
  for (const auto& [range_start, percentage] : distribution) {
    total += percentage;
  }

  // Normalize to 100%
  std::vector<std::pair<uint64_t, double>> normalized;
  normalized.reserve(distribution.size());

  for (const auto& [range_start, percentage] : distribution) {
    double normalized_percentage = (percentage / total) * 100.0;
    normalized.emplace_back(range_start, normalized_percentage);
  }

  return normalized;
}

/**
 * @brief Resample degree distribution to a different step size
 * @param distribution The original degree distribution
 * @param original_step The step size used in the original distribution
 * @param target_step The desired step size for resampling
 * @return Resampled distribution with the target step size
 */
inline std::vector<std::pair<uint64_t, double>>
resample_distribution(const std::vector<std::pair<uint64_t, double>>& distribution,
                      uint64_t original_step, uint64_t target_step) {
  if (distribution.empty() || original_step == 0 || target_step == 0) {
    return {};
  }

  if (original_step == target_step) {
    return distribution;
  }

  // Build a map of degree ranges to percentages
  std::unordered_map<uint64_t, double> degree_map;
  for (const auto& [range_start, percentage] : distribution) {
    degree_map[range_start] = percentage;
  }

  // Find maximum degree range
  uint64_t max_range = 0;
  for (const auto& [range_start, percentage] : distribution) {
    max_range = std::max(max_range, range_start);
  }

  // Resample to target step
  std::unordered_map<uint64_t, double> resampled_map;

  if (target_step > original_step) {
    // Merging buckets: combine multiple original buckets into larger ones
    for (const auto& [range_start, percentage] : distribution) {
      uint64_t target_bucket = (range_start / target_step) * target_step;
      resampled_map[target_bucket] += percentage;
    }
  } else {
    // Splitting buckets: distribute each original bucket into smaller ones
    for (const auto& [range_start, percentage] : distribution) {
      // Calculate how many target buckets this original bucket spans
      uint64_t original_end = range_start + original_step;

      for (uint64_t degree = range_start; degree < original_end; degree += target_step) {
        uint64_t target_bucket = (degree / target_step) * target_step;

        // Calculate overlap between original bucket and target bucket
        uint64_t overlap_start = std::max(degree, range_start);
        uint64_t overlap_end = std::min(degree + target_step, original_end);

        if (overlap_end > overlap_start) {
          double overlap_ratio = static_cast<double>(overlap_end - overlap_start) / original_step;
          resampled_map[target_bucket] += percentage * overlap_ratio;
        }
      }
    }
  }

  // Convert map back to vector and sort by range_start
  std::vector<std::pair<uint64_t, double>> result;
  result.reserve(resampled_map.size());

  for (const auto& [range_start, percentage] : resampled_map) {
    if (percentage > 0.0) {
      result.emplace_back(range_start, percentage);
    }
  }

  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  return result;
}

/**
 * @brief Compute KL divergence between two degree distributions
 * @param dist_p The first distribution (P)
 * @param dist_q The second distribution (Q)
 * @param step_p The step size used in dist_p
 * @param step_q The step size used in dist_q
 * @param epsilon Small constant to avoid log(0) (default: 1e-10)
 * @return KL divergence D_KL(P||Q), returns infinity if distributions are
 * incompatible
 */
inline double compute_kl_divergence(const std::vector<std::pair<uint64_t, double>>& dist_p,
                                    const std::vector<std::pair<uint64_t, double>>& dist_q,
                                    uint64_t step_p = 10, uint64_t step_q = 10,
                                    double epsilon = 1e-10) {
  if (dist_p.empty() || dist_q.empty()) {
    return std::numeric_limits<double>::infinity();
  }

  // Normalize both distributions
  auto normalized_p = normalize_distribution(dist_p);
  auto normalized_q = normalize_distribution(dist_q);

  // Resample to common step size (use the finer step)
  uint64_t common_step = std::min(step_p, step_q);

  auto resampled_p = resample_distribution(normalized_p, step_p, common_step);
  auto resampled_q = resample_distribution(normalized_q, step_q, common_step);

  // Build maps for easy lookup
  std::unordered_map<uint64_t, double> map_p, map_q;

  for (const auto& [range_start, percentage] : resampled_p) {
    map_p[range_start] = percentage / 100.0; // Convert percentage to probability
  }

  for (const auto& [range_start, percentage] : resampled_q) {
    map_q[range_start] = percentage / 100.0; // Convert percentage to probability
  }

  // Collect all unique degree ranges
  std::vector<uint64_t> all_ranges;
  for (const auto& [range_start, prob] : map_p) {
    all_ranges.push_back(range_start);
  }
  for (const auto& [range_start, prob] : map_q) {
    if (map_p.find(range_start) == map_p.end()) {
      all_ranges.push_back(range_start);
    }
  }

  std::sort(all_ranges.begin(), all_ranges.end());

  // Compute KL divergence: D_KL(P||Q) = Σ P(i) * log(P(i)/Q(i))
  double kl_divergence = 0.0;

  for (uint64_t range_start : all_ranges) {
    double p = map_p.count(range_start) ? map_p[range_start] : epsilon;
    double q = map_q.count(range_start) ? map_q[range_start] : epsilon;

    // Ensure probabilities are valid
    p = std::max(p, epsilon);
    q = std::max(q, epsilon);

    // KL divergence formula
    kl_divergence += p * std::log(p / q);
  }

  return kl_divergence;
}

/**
 * @brief Compute symmetric KL divergence (Jensen-Shannon divergence based)
 * @param dist_p The first distribution (P)
 * @param dist_q The second distribution (Q)
 * @param step_p The step size used in dist_p
 * @param step_q The step size used in dist_q
 * @param epsilon Small constant to avoid log(0) (default: 1e-10)
 * @return Symmetric divergence: (D_KL(P||Q) + D_KL(Q||P)) / 2
 */
inline double
compute_symmetric_kl_divergence(const std::vector<std::pair<uint64_t, double>>& dist_p,
                                const std::vector<std::pair<uint64_t, double>>& dist_q,
                                uint64_t step_p = 10, uint64_t step_q = 10,
                                double epsilon = 1e-10) {
  double kl_pq = compute_kl_divergence(dist_p, dist_q, step_p, step_q, epsilon);
  double kl_qp = compute_kl_divergence(dist_q, dist_p, step_q, step_p, epsilon);

  if (std::isinf(kl_pq) || std::isinf(kl_qp)) {
    return std::numeric_limits<double>::infinity();
  }

  return (kl_pq + kl_qp) / 2.0;
}

#endif // BW_GRAPH_DISTRIBUTION_H