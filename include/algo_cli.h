#ifndef BW_GRAPH_ALGO_CLI
#define BW_GRAPH_ALGO_CLI
#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Check whether an argument is the positional config path.
 * @param argc   Argument count
 * @param argv   Argument vector
 * @param index  Argument index to check
 * @return True if argv[1] is a non-flag config path
 */
inline bool is_config_path_arg(int argc, char** argv, int index) {
  return index == 1 && argc > 1 && argv[1][0] != '-';
}

/**
 * @brief Read a CLI option value from "--name value" or "--name=value".
 * @param argc   Argument count
 * @param argv   Argument vector
 * @param index  Current argument index, advanced when value is in next argument
 * @param name   Option name to match
 * @param value  Output option value
 * @return True if the option is matched and a value is read
 */
inline bool read_cli_value(int argc, char** argv, int& index, const std::string& name,
                           std::string& value) {
  std::string arg(argv[index]);
  std::string prefix = name + "=";
  if (arg == name && index + 1 < argc) {
    value = argv[++index];
    return true;
  }
  if (arg.rfind(prefix, 0) == 0) {
    value = arg.substr(prefix.size());
    return true;
  }
  return false;
}

/**
 * @brief Read a CLI option value using long or short option names.
 * @param argc        Argument count
 * @param argv        Argument vector
 * @param index       Current argument index, advanced when value is in next argument
 * @param long_name   Long option name
 * @param short_name  Short option name
 * @param value       Output option value
 * @return True if either option name is matched and a value is read
 */
inline bool read_cli_value(int argc, char** argv, int& index, const std::string& long_name,
                           const std::string& short_name, std::string& value) {
  return read_cli_value(argc, argv, index, long_name, value) ||
         read_cli_value(argc, argv, index, short_name, value);
}

inline bool apply_memory_cli_override(int argc, char** argv) {
  bool applied = false;
  for (int i = 1; i < argc; ++i) {
    if (is_config_path_arg(argc, argv, i)) {
      continue;
    }
    std::string value;
    if (read_cli_value(argc, argv, i, "--mem", value)) {
      apply_buffer_pool_memory_budget_mb(
          static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10)));
      applied = true;
    } else if (read_cli_value(argc, argv, i, "--prefetch-workers", value)) {
      bw_graph::BW_BUFFER_PREFETCH_WORKER_COUNT =
          static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
      applied = true;
    }
  }
  return applied;
}

/**
 * @brief Print algorithm name and output mode.
 * @param algo       Algorithm name
 * @param benchmark  True for benchmark mode, false for result mode
 */
inline void print_mode(const std::string& algo, bool benchmark) {
  std::cout << "Algorithm: " << algo << std::endl;
  std::cout << "Mode: " << (benchmark ? "benchmark" : "result") << std::endl;
}

/**
 * @brief Measure the execution time of a callable.
 * @param fn  Callable to execute
 * @return Elapsed time in seconds
 */
template <typename Fn>
double measure_seconds(Fn&& fn) {
  auto begin = std::chrono::high_resolution_clock::now();
  fn();
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1e6;
}

/**
 * @brief Print the number of algorithm result pairs.
 * @param name     Result name
 * @param results  Vector of vertex-result pairs
 */
template <typename T>
void print_result_count(const std::string& name, const std::vector<std::pair<v_id_t, T>>& results) {
  std::cout << name << " result count: " << results.size() << std::endl;
}

#endif // BW_GRAPH_ALGO_CLI
