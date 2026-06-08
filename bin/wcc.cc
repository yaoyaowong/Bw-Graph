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

  long threshold_divisor = 20;
  std::string algo = bw_graph::BW_GRAPH_WCC_ALGO;
  bool benchmark = true;

  for (int i = 1; i < argc; ++i) {
    if (is_config_path_arg(argc, argv, i)) {
      continue;
    }
    std::string arg(argv[i]);
    std::string value;
    if (read_cli_value(argc, argv, i, "--divisor", "-d", value)) {
      threshold_divisor = std::atol(value.c_str());
    } else if (read_cli_value(argc, argv, i, "--algo", value) ||
               read_cli_value(argc, argv, i, "--wcc-algo", value)) {
      algo = value;
    } else if (arg == "--benchmark") {
      benchmark = true;
    } else if (arg == "--result") {
      benchmark = false;
    }
  }

  bw_graph::BW_GRAPH_WCC_ALGO = algo;
  std::cout << "Threshold divisor: " << threshold_divisor << std::endl;
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
    if (algo == "sv") {
      seconds = db.benchmark_wcc_sv(threshold_divisor, true);
    } else if (algo == "map") {
      seconds = db.benchmark_wcc_map(threshold_divisor, true);
    } else if (algo == "scan") {
      seconds = db.benchmark_parallel_wcc_scan();
    } else if (algo == "sequential") {
      seconds = measure_seconds([&] { (void) db.wcc(); });
    } else {
      std::cerr << "Unknown WCC algorithm: " << algo << std::endl;
      return 1;
    }
    printf("WCC benchmark seconds: %.6f\n", seconds);
  } else if (algo == "sv") {
    auto results = db.wcc_sv(threshold_divisor);
    print_result_count("WCC", results);
  } else if (algo == "map") {
    auto results = db.wcc_map(threshold_divisor);
    print_result_count("WCC", results);
  } else if (algo == "scan") {
    auto results = db.parallel_wcc();
    print_result_count("WCC", results);
  } else if (algo == "sequential") {
    auto results = db.wcc();
    print_result_count("WCC", results);
  } else {
    std::cerr << "Unknown WCC algorithm: " << algo << std::endl;
    return 1;
  }
  return 0;
}
