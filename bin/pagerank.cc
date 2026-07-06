#include "algo_cli.h"
#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <cstdlib>
#include <iostream>

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

  uint64_t max_iterations = bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION;
  double damping_factor = bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR;
  double convergence_threshold = bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD;
  std::string algo = bw_graph::BW_GRAPH_PAGERANK_ALGO;
  bool benchmark = true;

  for (int i = 1; i < argc; ++i) {
    if (is_config_path_arg(argc, argv, i)) {
      continue;
    }
    std::string arg(argv[i]);
    std::string value;
    if (read_cli_value(argc, argv, i, "--iters", "-i", value)) {
      max_iterations = static_cast<uint64_t>(std::atoll(value.c_str()));
    } else if (read_cli_value(argc, argv, i, "--damping", "-a", value)) {
      damping_factor = std::atof(value.c_str());
    } else if (read_cli_value(argc, argv, i, "--epsilon", "-e", value)) {
      convergence_threshold = std::atof(value.c_str());
    } else if (read_cli_value(argc, argv, i, "--algo", value) ||
               read_cli_value(argc, argv, i, "--pagerank-algo", value) ||
               read_cli_value(argc, argv, i, "--pr-algo", value)) {
      algo = value;
    } else if (arg == "--benchmark") {
      benchmark = true;
    } else if (arg == "--result") {
      benchmark = false;
    }
  }

  apply_memory_cli_override(argc, argv);
  bw_graph::BW_GRAPH_PAGERANK_ALGO = algo;
  bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION = max_iterations;
  bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR = damping_factor;
  bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD = convergence_threshold;
  std::cout << "Max iterations: " << max_iterations << std::endl;
  std::cout << "Damping factor: " << damping_factor << std::endl;
  std::cout << "Convergence threshold: " << convergence_threshold << std::endl;
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

  if (benchmark) {
    double seconds = 0.0;
    if (algo == "map") {
      seconds = db.benchmark_parallel_pagerank_map(max_iterations, damping_factor,
                                                   convergence_threshold, true);
    } else if (algo == "scan") {
      seconds =
          db.benchmark_parallel_pagerank_scan(max_iterations, static_cast<float>(damping_factor),
                                              static_cast<float>(convergence_threshold));
    } else if (algo == "sequential") {
      seconds = measure_seconds(
          [&] { (void) db.pagerank(max_iterations, damping_factor, convergence_threshold); });
    } else {
      std::cerr << "Unknown PageRank algorithm: " << algo << std::endl;
      return 1;
    }
    printf("PageRank benchmark seconds: %.6f\n", seconds);
  } else if (algo == "map") {
    auto results = db.parallel_pagerank_map(max_iterations, damping_factor, convergence_threshold);
    print_result_count("PageRank", results);
  } else if (algo == "scan") {
    auto results = db.parallel_pagerank(max_iterations, static_cast<float>(damping_factor),
                                        static_cast<float>(convergence_threshold));
    print_result_count("PageRank", results);
  } else if (algo == "sequential") {
    auto results = db.pagerank(max_iterations, damping_factor, convergence_threshold);
    print_result_count("PageRank", results);
  } else {
    std::cerr << "Unknown PageRank algorithm: " << algo << std::endl;
    return 1;
  }
  return 0;
}
