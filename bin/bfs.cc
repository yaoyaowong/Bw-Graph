#include "algo_cli.h"
#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/db.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";
  if (argc > 1 && argv[1][0] != '-') {
    config_path = argv[1];
    std::cout << "Config: " << config_path << std::endl;
    load_yaml_config(config_path);
  } else {
    std::cout << "Config: default" << std::endl;
    load_yaml_config();
  }

  long threshold_divisor = 20;
  std::string algo = bw_graph::BW_GRAPH_BFS_ALGO;
  bool benchmark = true;
  v_id_t start_vertex = static_cast<v_id_t>(bw_graph::BW_GRAPH_BFS_START_VERTEX);

  for (int i = 1; i < argc; ++i) {
    if (is_config_path_arg(argc, argv, i)) {
      continue;
    }
    std::string arg(argv[i]);
    std::string value;
    if (read_cli_value(argc, argv, i, "--divisor", "-d", value)) {
      threshold_divisor = std::atol(value.c_str());
    } else if (read_cli_value(argc, argv, i, "--algo", value) ||
               read_cli_value(argc, argv, i, "--bfs-algo", value)) {
      algo = value;
    } else if (read_cli_value(argc, argv, i, "--start", "-s", value)) {
      start_vertex = static_cast<v_id_t>(std::strtoull(value.c_str(), nullptr, 10));
    } else if (arg == "--benchmark") {
      benchmark = true;
    } else if (arg == "--result") {
      benchmark = false;
    }
  }

  bw_graph::BW_GRAPH_BFS_ALGO = algo;
  bw_graph::BW_GRAPH_BFS_START_VERTEX = start_vertex;
  std::cout << "Threshold divisor: " << threshold_divisor << std::endl;
  std::cout << "Start vertex: " << start_vertex << std::endl;
  print_mode(algo, benchmark);
  print_config();

  std::cout << "Opening database..." << std::endl;
  bw_graph_db_t db(bw_graph::GRAPH_NAME, bw_graph::BW_GRAPH_REBUILD_INDEX,
                   bw_graph::BW_GRAPH_REDO_PARTITION, bw_graph::BW_GRAPH_NEIGHBOR_COMPRESS,
                   bw_graph::BW_GRAPH_ID_AWARE, true, true);

  db.warm_up();

  std::cout << "Computing edge count for adaptive threshold..." << std::endl;
  db.init_edge_count();
  std::cout << "Total stored edges (both directions): " << db.get_total_edge_count() << std::endl;

  v_id_t vertex_count = db.get_vertex_count();
  if (bw_graph::BW_GRAPH_BFS_IS_RANDOM && benchmark) {
    const uint64_t rounds = bw_graph::BW_GRAPH_BFS_RANDOM_SELECT;
    std::mt19937 rng(42);
    std::uniform_int_distribution<v_id_t> dist(0, vertex_count - 1);

    double total_seconds = 0.0;
    for (uint64_t round = 0; round < rounds; ++round) {
      v_id_t source = dist(rng);
      if (algo == "map") {
        total_seconds += db.benchmark_parallel_bfs_map(source, threshold_divisor, true);
      } else if (algo == "scan") {
        total_seconds += db.benchmark_parallel_bfs_scan(source);
      } else if (algo == "sequential") {
        total_seconds += measure_seconds([&] { (void) db.bfs(source); });
      } else {
        std::cerr << "Unknown BFS algorithm: " << algo << std::endl;
        return 1;
      }
    }
    printf("\nRounds: %llu  |  Total: %.4f s  |  Average: %.4f s\n",
           static_cast<unsigned long long>(rounds), total_seconds, total_seconds / rounds);
  } else {
    if (benchmark) {
      double seconds = 0.0;
      if (algo == "map") {
        seconds = db.benchmark_parallel_bfs_map(start_vertex, threshold_divisor, true);
      } else if (algo == "scan") {
        seconds = db.benchmark_parallel_bfs_scan(start_vertex);
      } else if (algo == "sequential") {
        seconds = measure_seconds([&] { (void) db.bfs(start_vertex); });
      } else {
        std::cerr << "Unknown BFS algorithm: " << algo << std::endl;
        return 1;
      }
      printf("BFS benchmark seconds: %.6f\n", seconds);
    } else if (algo == "map") {
      auto results = db.parallel_bfs_map(start_vertex, threshold_divisor);
      print_result_count("BFS", results);
    } else if (algo == "scan") {
      auto results = db.parallel_bfs(start_vertex);
      print_result_count("BFS", results);
    } else if (algo == "sequential") {
      auto results = db.bfs(start_vertex);
      print_result_count("BFS", results);
    } else {
      std::cerr << "Unknown BFS algorithm: " << algo << std::endl;
      return 1;
    }
  }

  return 0;
}
