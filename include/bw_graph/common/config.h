#ifndef BW_GRAPH_COMMON_CONFIG_H
#define BW_GRAPH_COMMON_CONFIG_H
#include "bw_graph/common/utils.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <yaml-cpp/yaml.h>

/**
 * @brief: The namespace for the graph database
 */
namespace bw_graph {
inline size_t BW_GRAPH_PAGE_SIZE = 4096 * 16;
inline uint64_t BW_GRAPH_DEFAULT_PAGE_NO = 0;
inline size_t BW_GRAPH_DEFAULT_DB_IO_SIZE = 16;
inline size_t BW_DELTA_PAGE_SIZE = 4096;
inline uint64_t BW_BUFFER_CHUNK_COUNT = 16;
inline uint64_t BW_BUFFER_CHUNK_SIZE = 1024 * 4;
inline uint64_t BW_DELTA_BUFFER_CHUNK_COUNT = 16;
inline uint64_t BW_DELTA_BUFFER_CHUNK_SIZE = 1024;
// Background delta-page flush interval in milliseconds (0 = disabled)
inline uint64_t BW_DELTA_FLUSH_INTERVAL_MS = 200;
// Background CSR-page flush interval in milliseconds (0 = disabled)
inline uint64_t BW_CSR_FLUSH_INTERVAL_MS = 500;
// Worker thread count for the delta-page flusher (0 = disabled).
inline uint64_t BW_DELTA_FLUSH_WORKER_COUNT = 1;
// Worker thread count for the CSR-page flusher (0 = disabled).
inline uint64_t BW_CSR_FLUSH_WORKER_COUNT = 1;
inline uint64_t BW_GRAPH_GIANT_DEGREE_BOUND = 128;
inline uint64_t BW_GRAPH_MAX_BLOCK_WEIGHT = 4096;
inline uint64_t BW_GRAPH_INDEX_PAGE_SIZE = 4096 * 4;
inline uint64_t BW_GRAPH_MAX_BLOCK_NEIGHBOR = 16;
inline uint64_t BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;
inline uint64_t BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 4;
// Background workers that submit retired vertex versions to Folly hazard GC.
// Must be at least one because page retirement can outlive the SMO worker.
inline uint64_t version_gc_thd_count = 1;
inline uint64_t BW_GRAPH_INDEX_MAX_BLOCKS =
    calculate_bw_capacity(BW_GRAPH_INDEX_PAGE_SIZE, BW_GRAPH_MAX_BLOCK_NEIGHBOR);
inline bool BW_GRAPH_LEVELED = false;
inline size_t LOAD_BUFFER_SIZE = 1024 * 1024 * 64; // 64MB
inline std::string GRAPH_NAME = "rn";
inline std::string CSR_PREFIX = "./workspace";
inline std::string DELTA_PREFIX = "./workspace";
inline std::string META_PREFIX = "./workspace";
inline bool BW_GRAPH_REBUILD_INDEX = false;
inline bool BW_GRAPH_REDO_PARTITION = false;
#ifdef BWGRAPH_NEIGHBOR_COMPRESS
inline constexpr bool BW_GRAPH_NEIGHBOR_COMPRESS = true;
#else
inline constexpr bool BW_GRAPH_NEIGHBOR_COMPRESS = false;
#endif
inline uint64_t BW_GRAPH_INSERTION_MODE = 0;
inline uint64_t BW_GRAPH_EDGE_SLOT_COUNT = 0;
inline bool BW_GRAPH_ID_AWARE = false;
inline uint64_t BW_GRAPH_COARSENING_WEIGHT = 1024 * 1024;
inline bool BW_GRAPH_USING_NATIVE_PARTITION = false;
inline std::string LOG_FILE_PATH = "";

inline std::filesystem::path storage_path(const std::string& prefix,
                                          const std::string& filename) {
  return std::filesystem::path(prefix) / filename;
}

inline std::filesystem::path csr_db_path(const std::string& graph_name) {
  return storage_path(CSR_PREFIX, graph_name + ".db");
}

inline std::filesystem::path delta_db_path(const std::string& graph_name) {
  return storage_path(DELTA_PREFIX, graph_name + "_delta.db");
}

inline std::filesystem::path vertex_index_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + ".vertex_index.bin");
}

inline std::filesystem::path index_page_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + ".index_page.bin");
}

inline std::filesystem::path partition_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + ".partition.bin");
}

inline std::filesystem::path coarsening_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + ".coar.bin");
}

inline std::filesystem::path giant_db_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + "_giantdb");
}

inline std::filesystem::path property_db_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + "_propdb");
}

inline std::filesystem::path log_file_path(const std::string& graph_name) {
  return storage_path(META_PREFIX, graph_name + ".log");
}

inline void ensure_storage_prefixes() {
  std::filesystem::create_directories(CSR_PREFIX);
  std::filesystem::create_directories(DELTA_PREFIX);
  std::filesystem::create_directories(META_PREFIX);
}

// BFS algorithm configuration
inline uint64_t BW_GRAPH_BFS_MAX_ITERATION = 100;
inline uint64_t BW_GRAPH_BFS_START_VERTEX = 0;
inline bool BW_GRAPH_BFS_IS_RANDOM = false;
inline uint64_t BW_GRAPH_BFS_RANDOM_SELECT = 10;

// WCC algorithm configuration
inline uint64_t BW_GRAPH_WCC_MAX_ITERATION = 100;

// PageRank algorithm configuration
inline uint64_t BW_GRAPH_PAGERANK_MAX_ITERATION = 100;
inline double BW_GRAPH_PAGERANK_DAMPING_FACTOR = 0.85;
inline double BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD = 1e-6;

// CDLP algorithm configuration
inline uint64_t BW_GRAPH_CDLP_MAX_ITERATION = 20;
inline uint64_t BW_GRAPH_EDGE_INSERT_RATE = 1000;

// Algorithm variant selection
// WCC:     "sv"   – Shiloach-Vishkin hook+compress via edge_map (default)
//          "map"  – Ligra-style min-label propagation via edge_map
//          "scan" – parallel edge scan label propagation
inline std::string BW_GRAPH_WCC_ALGO = "sv";
// BFS:     "map"  – edge_map based (default)
//          "scan" – edge scan based
inline std::string BW_GRAPH_BFS_ALGO = "map";
// PageRank:"map"        – edge_map based
//          "scan"       – edge scan based
//          "sequential" – single-threaded
inline std::string BW_GRAPH_PAGERANK_ALGO = "map";
// CDLP:    "map"        – edge_map based
//          "scan"       – edge scan based
//          "sequential" – single-threaded
inline std::string BW_GRAPH_CDLP_ALGO = "map";
// CDLP variant (only meaningful when CDLP_ALGO == "map"):
//   "original" – fixed-slot atomic voting (existing, default)
//   "voting"   – pull-based hash-map voting (no overflow)
inline std::string BW_GRAPH_CDLP_VARIANT = "original";

// Sampling configuration
inline uint64_t BW_GRAPH_SAMPLE_VERTEX_COUNT = 1000;
inline uint64_t BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS = 10;

} // namespace bw_graph

inline std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

static long long parse_int_like(const YAML::Node& n, long long def) {
  if (!n)
    return def;

  if (n.IsScalar()) {
    std::string s = trim(n.as<std::string>());
    if (s.empty())
      return def;

    // Support "a * b * c" style expressions
    long long acc = 1;
    std::stringstream ss(s);
    std::string token;
    bool has_star = (s.find('*') != std::string::npos);

    if (has_star) {
      while (std::getline(ss, token, '*')) {
        token = trim(token);
        if (token.empty())
          throw std::invalid_argument("Bad numeric token in expression: '" + s + "'");
        long long v = std::stoll(token);
        acc *= v;
      }
      return acc;
    } else {
      // Direct integer
      return std::stoll(s);
    }
  } else if (n.IsScalar() == false && n.IsNull() == false) {
    // If it's a YAML int type (some parsers will directly give int)
    try {
      return n.as<long long>();
    } catch (...) { /* fallthrough */
    }
  }
  return def;
}

static double parse_double_like(const YAML::Node& n, double def) {
  if (!n)
    return def;

  if (n.IsScalar()) {
    std::string s = trim(n.as<std::string>());
    if (s.empty())
      return def;
    return std::stod(s);
  } else if (n.IsScalar() == false && n.IsNull() == false) {
    try {
      return n.as<double>();
    } catch (...) { /* fallthrough */
    }
  }
  return def;
}

inline void load_yaml_config(const std::string& path = "config/default.yaml") {
  YAML::Node root = YAML::LoadFile(path);

  // Top-level optional fields
  if (auto node = root["graph"]) {
    bw_graph::GRAPH_NAME = node["name"].as<std::string>();
  }

  // storage
  if (auto st = root["storage"]) {
    if (st["csr_prefix"])
      bw_graph::CSR_PREFIX = st["csr_prefix"].as<std::string>();
    if (st["delta_prefix"])
      bw_graph::DELTA_PREFIX = st["delta_prefix"].as<std::string>();
    if (st["meta_prefix"])
      bw_graph::META_PREFIX = st["meta_prefix"].as<std::string>();
    bw_graph::BW_GRAPH_PAGE_SIZE =
        static_cast<size_t>(parse_int_like(st["base_page_size"], bw_graph::BW_GRAPH_PAGE_SIZE));
    bw_graph::BW_DELTA_PAGE_SIZE =
        static_cast<size_t>(parse_int_like(st["delta_page_size"], bw_graph::BW_DELTA_PAGE_SIZE));
    bw_graph::BW_GRAPH_DEFAULT_PAGE_NO = static_cast<uint64_t>(
        parse_int_like(st["default_page_no"], bw_graph::BW_GRAPH_DEFAULT_PAGE_NO));
    bw_graph::BW_GRAPH_INDEX_PAGE_SIZE = static_cast<size_t>(
        parse_int_like(st["index_page_size"], bw_graph::BW_GRAPH_INDEX_PAGE_SIZE));
    bw_graph::BW_GRAPH_LEVELED = st["have_level"].as<bool>();
    bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR = static_cast<uint64_t>(
        parse_int_like(st["max_block_neighbor"], bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR));
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = static_cast<uint64_t>(
        parse_int_like(st["max_delta_chain_length"], bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH));
    bw_graph::BW_GRAPH_REBUILD_INDEX = st["rebuild"].as<bool>();
    bw_graph::BW_GRAPH_EDGE_SLOT_COUNT = static_cast<uint64_t>(
        parse_int_like(st["edge_slot_count"], bw_graph::BW_GRAPH_EDGE_SLOT_COUNT));
    bw_graph::BW_GRAPH_ID_AWARE = st["id_aware"].as<bool>();
  }

  // buffer
  if (auto bf = root["buffer"]) {
    bw_graph::BW_BUFFER_CHUNK_COUNT =
        static_cast<uint64_t>(parse_int_like(bf["chunk_count"], bw_graph::BW_BUFFER_CHUNK_COUNT));
    bw_graph::BW_BUFFER_CHUNK_SIZE =
        static_cast<uint64_t>(parse_int_like(bf["chunk_size"], bw_graph::BW_BUFFER_CHUNK_SIZE));
    bw_graph::BW_GRAPH_DEFAULT_DB_IO_SIZE = static_cast<size_t>(
        parse_int_like(bf["db_io_size"], bw_graph::BW_GRAPH_DEFAULT_DB_IO_SIZE));
    bw_graph::LOAD_BUFFER_SIZE =
        static_cast<size_t>(parse_int_like(bf["load_buffer_size"], bw_graph::LOAD_BUFFER_SIZE));
    bw_graph::BW_CSR_FLUSH_INTERVAL_MS = static_cast<uint64_t>(
        parse_int_like(bf["csr_flush_interval_ms"], bw_graph::BW_CSR_FLUSH_INTERVAL_MS));
    bw_graph::BW_CSR_FLUSH_WORKER_COUNT = static_cast<uint64_t>(
        parse_int_like(bf["csr_flush_workers"], bw_graph::BW_CSR_FLUSH_WORKER_COUNT));
  }

  // delta_buffer
  if (auto db = root["delta_buffer"]) {
    bw_graph::BW_DELTA_BUFFER_CHUNK_COUNT = static_cast<uint64_t>(
        parse_int_like(db["chunk_count"], bw_graph::BW_DELTA_BUFFER_CHUNK_COUNT));
    bw_graph::BW_DELTA_BUFFER_CHUNK_SIZE = static_cast<uint64_t>(
        parse_int_like(db["chunk_size"], bw_graph::BW_DELTA_BUFFER_CHUNK_SIZE));
    bw_graph::BW_DELTA_FLUSH_INTERVAL_MS = static_cast<uint64_t>(
        parse_int_like(db["flush_interval_ms"], bw_graph::BW_DELTA_FLUSH_INTERVAL_MS));
    bw_graph::BW_DELTA_FLUSH_WORKER_COUNT = static_cast<uint64_t>(
        parse_int_like(db["flush_workers"], bw_graph::BW_DELTA_FLUSH_WORKER_COUNT));
  }

  // partition
  if (auto pt = root["partition"]) {
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = static_cast<uint64_t>(
        parse_int_like(pt["giant_degree_bound"], bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND));
    bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT = static_cast<uint64_t>(
        parse_int_like(pt["max_block_weight"], bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT));
    bw_graph::BW_GRAPH_REDO_PARTITION = pt["redo"].as<bool>();
    bw_graph::BW_GRAPH_COARSENING_WEIGHT = static_cast<uint64_t>(
        parse_int_like(pt["coarsening_weight"], bw_graph::BW_GRAPH_COARSENING_WEIGHT));
    bw_graph::BW_GRAPH_USING_NATIVE_PARTITION = pt["native"].as<bool>();
  }

  // thread
  if (auto th = root["thread"]) {
    bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT = static_cast<uint64_t>(
        parse_int_like(th["consolidation_workers"], bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT));
    bw_graph::version_gc_thd_count = static_cast<uint64_t>(
        parse_int_like(th["version_gc_thd_count"], bw_graph::version_gc_thd_count));
  }

  // algorithms
  if (auto algo = root["algorithms"]) {
    // New in v0.3.0: edge insertion rate
    bw_graph::BW_GRAPH_EDGE_INSERT_RATE = static_cast<uint64_t>(
        parse_int_like(algo["edge_insert_rate"], bw_graph::BW_GRAPH_EDGE_INSERT_RATE));
    bw_graph::BW_GRAPH_INSERTION_MODE = static_cast<uint64_t>(
        parse_int_like(algo["edge_insert_mode"], bw_graph::BW_GRAPH_INSERTION_MODE));
    // BFS algorithm
    if (auto bfs = algo["bfs"]) {
      bw_graph::BW_GRAPH_BFS_MAX_ITERATION = static_cast<uint64_t>(
          parse_int_like(bfs["max_iteration"], bw_graph::BW_GRAPH_BFS_MAX_ITERATION));
      bw_graph::BW_GRAPH_BFS_START_VERTEX = static_cast<uint64_t>(
          parse_int_like(bfs["start_vertex"], bw_graph::BW_GRAPH_BFS_START_VERTEX));
      bw_graph::BW_GRAPH_BFS_IS_RANDOM = bfs["is_random"].as<bool>();
      bw_graph::BW_GRAPH_BFS_RANDOM_SELECT = static_cast<uint64_t>(
          parse_int_like(bfs["random_select"], bw_graph::BW_GRAPH_BFS_RANDOM_SELECT));
      if (bfs["algo"])
        bw_graph::BW_GRAPH_BFS_ALGO = bfs["algo"].as<std::string>();
    }

    // WCC algorithm
    if (auto wcc = algo["wcc"]) {
      bw_graph::BW_GRAPH_WCC_MAX_ITERATION = static_cast<uint64_t>(
          parse_int_like(wcc["max_iteration"], bw_graph::BW_GRAPH_WCC_MAX_ITERATION));
      if (wcc["algo"])
        bw_graph::BW_GRAPH_WCC_ALGO = wcc["algo"].as<std::string>();
    }

    // PageRank algorithm
    if (auto pr = algo["pagerank"]) {
      bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION = static_cast<uint64_t>(
          parse_int_like(pr["max_iteration"], bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION));
      bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR =
          parse_double_like(pr["damping_factor"], bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR);
      bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD = parse_double_like(
          pr["convergence_threshold"], bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD);
      if (pr["algo"])
        bw_graph::BW_GRAPH_PAGERANK_ALGO = pr["algo"].as<std::string>();
    }

    // CDLP algorithm
    if (auto cdlp = algo["cdlp"]) {
      bw_graph::BW_GRAPH_CDLP_MAX_ITERATION = static_cast<uint64_t>(
          parse_int_like(cdlp["max_iteration"], bw_graph::BW_GRAPH_CDLP_MAX_ITERATION));
      if (cdlp["algo"])
        bw_graph::BW_GRAPH_CDLP_ALGO = cdlp["algo"].as<std::string>();
      if (cdlp["variant"])
        bw_graph::BW_GRAPH_CDLP_VARIANT = cdlp["variant"].as<std::string>();
    }

    // Sampling algorithm
    if (auto sample = algo["sample"]) {
      bw_graph::BW_GRAPH_SAMPLE_VERTEX_COUNT = static_cast<uint64_t>(
          parse_int_like(sample["vertex_count"], bw_graph::BW_GRAPH_SAMPLE_VERTEX_COUNT));
      bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS = static_cast<uint64_t>(parse_int_like(
          sample["distribution_steps"], bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS));
    }
  }

  // logging
  if (auto lg = root["logging"]) {
    if (lg["log_file"]) {
      std::string v = lg["log_file"].as<std::string>();
      bw_graph::LOG_FILE_PATH = v.empty()
          ? bw_graph::log_file_path(bw_graph::GRAPH_NAME).string()
          : v;
    }
  }
  if (bw_graph::LOG_FILE_PATH.empty()) {
    bw_graph::LOG_FILE_PATH = bw_graph::log_file_path(bw_graph::GRAPH_NAME).string();
  }

  // After loading: recalculate dependent constants
  bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS = calculate_bw_capacity(
      bw_graph::BW_GRAPH_INDEX_PAGE_SIZE, bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR);
}

inline std::string format_bytes(size_t bytes) {
  std::ostringstream oss;
  if (bytes >= 1024 * 1024) {
    oss << bytes << " bytes (" << (bytes / 1024 / 1024) << " MB)";
  } else if (bytes >= 1024) {
    oss << bytes << " bytes (" << (bytes / 1024) << " KB)";
  } else {
    oss << bytes << " bytes";
  }
  return oss.str();
}

inline void print_config() {
  const int name_width = 35;

  std::cout << std::string(80, '=') << std::endl;
  std::cout << std::setw(40) << "BwGraph Configuration" << std::endl;
  std::cout << std::string(80, '=') << std::endl;

  std::cout << std::left;

  // Storage
  std::cout << "\n[Storage Configuration]" << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_PAGE_SIZE:" << format_bytes(bw_graph::BW_GRAPH_PAGE_SIZE) << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_DEFAULT_PAGE_NO:" << bw_graph::BW_GRAPH_DEFAULT_PAGE_NO << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_DEFAULT_DB_IO_SIZE:" << bw_graph::BW_GRAPH_DEFAULT_DB_IO_SIZE
            << std::endl;
  std::cout << std::setw(name_width) << "  CSR_PREFIX:"
            << "\"" << bw_graph::CSR_PREFIX << "\"" << std::endl;
  std::cout << std::setw(name_width) << "  DELTA_PREFIX:"
            << "\"" << bw_graph::DELTA_PREFIX << "\"" << std::endl;
  std::cout << std::setw(name_width) << "  META_PREFIX:"
            << "\"" << bw_graph::META_PREFIX << "\"" << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_DELTA_PAGE_SIZE:" << format_bytes(bw_graph::BW_DELTA_PAGE_SIZE) << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_INDEX_PAGE_SIZE:" << format_bytes(bw_graph::BW_GRAPH_INDEX_PAGE_SIZE)
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_LEVELED:" << (bw_graph::BW_GRAPH_LEVELED ? "true" : "false")
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_MAX_BLOCK_COUNT:" << (bw_graph::BW_GRAPH_INDEX_MAX_BLOCKS) << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_MAX_BLOCK_NEIGHBOR:" << (bw_graph::BW_GRAPH_MAX_BLOCK_NEIGHBOR)
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_REBUILD_INDEX:" << (bw_graph::BW_GRAPH_REBUILD_INDEX ? "true" : "false")
            << std::endl;
  std::cout << std::setw(name_width) << "  BW_GRAPH_NEIGHBOR_COMPRESS:"
            << (bw_graph::BW_GRAPH_NEIGHBOR_COMPRESS ? "true" : "false") << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_ID_AWARE:" << (bw_graph::BW_GRAPH_ID_AWARE ? "true" : "false")
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_EDGE_SLOT_COUNT:" << (bw_graph::BW_GRAPH_EDGE_SLOT_COUNT) << std::endl;

  // Buffer Pool
  std::cout << "\n[Buffer Configuration]" << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_BUFFER_CHUNK_COUNT:" << bw_graph::BW_BUFFER_CHUNK_COUNT << std::endl;
  std::cout << std::setw(name_width) << "  BW_BUFFER_CHUNK_SIZE:" << bw_graph::BW_BUFFER_CHUNK_SIZE
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_DELTA_BUFFER_CHUNK_COUNT:" << bw_graph::BW_DELTA_BUFFER_CHUNK_COUNT
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_DELTA_BUFFER_CHUNK_SIZE:" << bw_graph::BW_DELTA_BUFFER_CHUNK_SIZE << std::endl;

  // Graph
  std::cout << "\n[Graph Algorithm Configuration]" << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_GIANT_DEGREE_BOUND:" << bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND
            << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_MAX_BLOCK_WEIGHT:" << bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT << std::endl;
  std::cout << std::setw(name_width) << "  LOAD_BUFFER_SIZE:" << bw_graph::LOAD_BUFFER_SIZE
            << std::endl;
  std::cout << std::setw(name_width) << "  BW_GRAPH_REDO_PARTITION:"
            << (bw_graph::BW_GRAPH_REDO_PARTITION ? "true" : "false") << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_COARSENING_WEIGHT:" << bw_graph::BW_GRAPH_COARSENING_WEIGHT << std::endl;
  std::cout << std::setw(name_width) << "  BW_GRAPH_USING_NATIVE_PARTITION:"
            << (bw_graph::BW_GRAPH_USING_NATIVE_PARTITION ? "true" : "false") << std::endl;

  // Thread
  std::cout << "\n[Thread Configuration]" << std::endl;
  std::cout << std::setw(name_width) << "  BW_GRAPH_CONSOLIDATION_WORKER_COUNT:"
            << bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT << std::endl;
  std::cout << std::setw(name_width) << "  BW_GRAPH_CONSOLIDATION_ENABLED:"
            << (bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT > 0 ? "true" : "false") << std::endl;
  std::cout << std::setw(name_width)
            << "  version_gc_thd_count:" << bw_graph::version_gc_thd_count << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_DELTA_FLUSH_WORKER_COUNT:" << bw_graph::BW_DELTA_FLUSH_WORKER_COUNT
            << std::endl;
  std::cout << std::setw(name_width) << "  BW_DELTA_FLUSH_ENABLED:"
            << (bw_graph::BW_DELTA_FLUSH_WORKER_COUNT > 0 ? "true" : "false") << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_CSR_FLUSH_WORKER_COUNT:" << bw_graph::BW_CSR_FLUSH_WORKER_COUNT << std::endl;
  std::cout << std::setw(name_width) << "  BW_CSR_FLUSH_ENABLED:"
            << (bw_graph::BW_CSR_FLUSH_WORKER_COUNT > 0 ? "true" : "false") << std::endl;

  // Algorithm Configuration
  std::cout << "\n[Algorithm Configuration]" << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_EDGE_INSERT_RATE:" << bw_graph::BW_GRAPH_EDGE_INSERT_RATE << std::endl;
  std::cout << std::setw(name_width)
            << "  BW_GRAPH_EDGE_INSERT_MODE:" << bw_graph::BW_GRAPH_INSERTION_MODE << std::endl;

  // BFS
  std::cout << "  BFS:" << std::endl;
  std::cout << std::setw(name_width) << "    ALGO:" << bw_graph::BW_GRAPH_BFS_ALGO << std::endl;
  std::cout << std::setw(name_width) << "    MAX_ITERATION:" << bw_graph::BW_GRAPH_BFS_MAX_ITERATION
            << std::endl;
  std::cout << std::setw(name_width) << "    START_VERTEX:" << bw_graph::BW_GRAPH_BFS_START_VERTEX
            << std::endl;
  std::cout << std::setw(name_width)
            << "    IS_RANDOM:" << (bw_graph::BW_GRAPH_BFS_IS_RANDOM ? "true" : "false")
            << std::endl;
  std::cout << std::setw(name_width) << "    RANDOM_SELECT:" << bw_graph::BW_GRAPH_BFS_RANDOM_SELECT
            << std::endl;

  // WCC
  std::cout << "  WCC:" << std::endl;
  std::cout << std::setw(name_width) << "    MAX_ITERATION:" << bw_graph::BW_GRAPH_WCC_MAX_ITERATION
            << std::endl;
  std::cout << std::setw(name_width) << "    ALGO:" << bw_graph::BW_GRAPH_WCC_ALGO << std::endl;

  // PageRank
  std::cout << "  PageRank:" << std::endl;
  std::cout << std::setw(name_width) << "    ALGO:" << bw_graph::BW_GRAPH_PAGERANK_ALGO
            << std::endl;
  std::cout << std::setw(name_width)
            << "    MAX_ITERATION:" << bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION << std::endl;
  std::cout << std::setw(name_width)
            << "    DAMPING_FACTOR:" << bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR << std::endl;
  std::cout << std::setw(name_width)
            << "    CONVERGENCE_THRESHOLD:" << bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD
            << std::endl;

  // CDLP
  std::cout << "  CDLP:" << std::endl;
  std::cout << std::setw(name_width) << "    ALGO:" << bw_graph::BW_GRAPH_CDLP_ALGO << std::endl;
  std::cout << std::setw(name_width) << "    VARIANT:" << bw_graph::BW_GRAPH_CDLP_VARIANT
            << std::endl;
  std::cout << std::setw(name_width)
            << "    MAX_ITERATION:" << bw_graph::BW_GRAPH_CDLP_MAX_ITERATION << std::endl;

  // Sampling
  std::cout << "  Sampling:" << std::endl;
  std::cout << std::setw(name_width)
            << "    VERTEX_COUNT:" << bw_graph::BW_GRAPH_SAMPLE_VERTEX_COUNT << std::endl;
  std::cout << std::setw(name_width)
            << "    DISTRIBUTION_STEPS:" << bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS
            << std::endl;

  // Basic Configuration
  std::cout << "\n[Basic Configuration]" << std::endl;
  std::cout << std::setw(name_width) << "  GRAPH_NAME:"
            << "\"" << bw_graph::GRAPH_NAME << "\"" << std::endl;
  std::cout << std::string(80, '=') << std::endl;
}

inline void apply_buffer_pool_memory_budget_mb(uint64_t mem_mb) {
  constexpr uint64_t bytes_per_mb = 1024ULL * 1024ULL;
  const uint64_t page_size =
      static_cast<uint64_t>(std::max<size_t>(bw_graph::BW_GRAPH_PAGE_SIZE, 1));
  uint64_t budget_bytes = 0;
  if (mem_mb > std::numeric_limits<uint64_t>::max() / bytes_per_mb) {
    budget_bytes = std::numeric_limits<uint64_t>::max();
  } else {
    budget_bytes = mem_mb * bytes_per_mb;
  }

  uint64_t resident_pages = budget_bytes / page_size;
  if (resident_pages == 0) {
    resident_pages = 1;
  }

  bw_graph::BW_BUFFER_CHUNK_COUNT = std::min<uint64_t>(16, resident_pages);
  bw_graph::BW_BUFFER_CHUNK_SIZE =
      std::max<uint64_t>(1, resident_pages / bw_graph::BW_BUFFER_CHUNK_COUNT);
}

/**
 * @brief Apply per-algorithm overrides from command-line arguments.
 *
 * Accepts arguments in --key=value or --key value form starting at argv[2]
 * (argv[0]=program, argv[1]=config file path).
 *
 * Supported keys:
 *   --wcc-algo=sv|map|scan
 *   --bfs-algo=map|scan
 *   --pagerank-algo=map|scan
 *   --cdlp-algo=map|scan|voting
 *   --cdlp-variant=original|voting
 *   --mem=MB
 *
 * CLI overrides take precedence over the YAML config file.
 */
inline void apply_cli_overrides(int argc, char** argv) {
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.size() < 5 || arg.substr(0, 2) != "--")
      continue;
    auto eq_pos = arg.find('=');
    std::string key;
    std::string val;
    if (eq_pos != std::string::npos) {
      key = arg.substr(2, eq_pos - 2);
      val = arg.substr(eq_pos + 1);
    } else {
      if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0)
        continue;
      key = arg.substr(2);
      val = argv[++i];
    }
    if (key == "wcc-algo")
      bw_graph::BW_GRAPH_WCC_ALGO = val;
    else if (key == "bfs-algo")
      bw_graph::BW_GRAPH_BFS_ALGO = val;
    else if (key == "pagerank-algo" || key == "pr-algo")
      bw_graph::BW_GRAPH_PAGERANK_ALGO = val;
    else if (key == "cdlp-algo") {
      if (val == "voting") {
        bw_graph::BW_GRAPH_CDLP_ALGO = "map";
        bw_graph::BW_GRAPH_CDLP_VARIANT = "voting";
      } else {
        bw_graph::BW_GRAPH_CDLP_ALGO = val;
      }
    }
    else if (key == "cdlp-variant")
      bw_graph::BW_GRAPH_CDLP_VARIANT = val;
    else if (key == "consolidation-workers")
      bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT = static_cast<uint64_t>(std::stoll(val));
    else if (key == "version-gc-thd-count")
      bw_graph::version_gc_thd_count = static_cast<uint64_t>(std::stoll(val));
    else if (key == "delta-flush-workers")
      bw_graph::BW_DELTA_FLUSH_WORKER_COUNT = static_cast<uint64_t>(std::stoll(val));
    else if (key == "csr-flush-workers")
      bw_graph::BW_CSR_FLUSH_WORKER_COUNT = static_cast<uint64_t>(std::stoll(val));
    else if (key == "mem")
      apply_buffer_pool_memory_budget_mb(static_cast<uint64_t>(std::stoull(val)));
    else
      std::cerr << "[Config] Unknown CLI override key: --" << key << std::endl;
  }
}

#endif // BW_GRAPH_COMMON_CONFIG_H
