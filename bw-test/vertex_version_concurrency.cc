#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

class suite_t {
public:
  explicit suite_t(std::string name) : name_(std::move(name)) {
    std::cout << "# bw-test: " << name_ << std::endl;
  }

  bool expect(bool condition, const std::string& message) {
    std::cout << (condition ? "ok - " : "not ok - ") << message << std::endl;
    failed_ = failed_ || !condition;
    return condition;
  }

  int finish() const {
    std::cout << (failed_ ? "FAIL " : "PASS ") << name_ << std::endl;
    return failed_ ? 1 : 0;
  }

private:
  std::string name_;
  bool failed_{false};
};

std::vector<std::vector<v_id_t>> load_graph(const std::filesystem::path& graph) {
  std::ifstream in(graph);
  if (!in.is_open()) {
    throw std::runtime_error("cannot open graph file: " + graph.string());
  }

  std::vector<std::vector<v_id_t>> neighbors;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream iss(line);
    char tag = 0;
    iss >> tag;
    if (tag == 't') {
      size_t vertex_count = 0;
      size_t edge_count = 0;
      iss >> vertex_count >> edge_count;
      (void) edge_count;
      neighbors.assign(vertex_count, {});
    } else if (tag == 'e') {
      v_id_t src = 0;
      v_id_t dst = 0;
      iss >> src >> dst;
      if (src < neighbors.size() && dst < neighbors.size() && src != dst) {
        neighbors[src].push_back(dst);
        neighbors[dst].push_back(src);
      }
    }
  }

  for (auto& list : neighbors) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }
  return neighbors;
}

std::vector<v_id_t> sorted_unique(std::vector<v_id_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

v_id_t choose_non_neighbor(const std::vector<std::vector<v_id_t>>& graph, v_id_t src,
                           const std::set<v_id_t>& reserved) {
  for (v_id_t dst = 0; dst < graph.size(); ++dst) {
    if (dst != src && !reserved.contains(dst) &&
        !std::binary_search(graph[src].begin(), graph[src].end(), dst)) {
      return dst;
    }
  }
  throw std::runtime_error("no non-neighbor available for stress edge");
}

bool valid_transient_read(const std::vector<v_id_t>& actual, const std::vector<v_id_t>& baseline,
                          v_id_t transient_dst) {
  if (actual == baseline) {
    return true;
  }
  std::vector<v_id_t> with_insert = baseline;
  with_insert.push_back(transient_dst);
  return actual == sorted_unique(std::move(with_insert));
}

void force_consolidate(bw_graph_db_t& db, page_no_t page_no) {
  if (db.page_map_table->get_delta_chain_length(page_no) == 0) {
    return;
  }
  csr_page_t* page = db.buffer_pool->buf_page_read(page_no, db.disk_manager);
  page->r_unlatch();
  db.smo_ctl->consolidate_pages(page, db.page_map_table, db.disk_manager, db.buffer_pool,
                                db.vertex_index, db.giant_db, db.vertex_update_sketch_);
}

int main(int argc, char** argv) {
  suite_t suite("vertex_version_concurrency");

  try {
    if (argc < 2) {
      suite.expect(false, "repo root argument is required");
      return suite.finish();
    }

    const std::filesystem::path repo_root = std::filesystem::absolute(argv[1]);
    const std::filesystem::path source_graph = repo_root / "data" / "example.graph";
    auto baseline = load_graph(source_graph);
    suite.expect(!baseline.empty(), "loaded example.graph");

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path run_dir =
        std::filesystem::temp_directory_path() /
        ("bw_test_vertex_version_concurrency_" + std::to_string(suffix));
    std::filesystem::create_directories(run_dir / "data");
    std::filesystem::create_directories(run_dir / "workspace");
    std::filesystem::copy_file(source_graph, run_dir / "data" / "example.graph",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::current_path(run_dir);

    bw_graph::BW_DELTA_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_CSR_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_GRAPH_LEVELED = false;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1000000;
    bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 2;
    bw_graph::version_gc_thd_count = 2;

    bw_graph_db_t db("example", true, true, false, false, false);
    suite.expect(db.vertex_index->get_version_gc_worker_count() == 2,
                 "vertex index started two configured version GC workers");

    v_id_t seed = 0;
    while (seed < baseline.size() && baseline[seed].empty()) {
      ++seed;
    }
    if (seed == baseline.size()) {
      throw std::runtime_error("example.graph has no non-isolated vertex");
    }

    page_no_t hot_page = db.vertex_index->get_vertex_location(seed).first;
    std::vector<v_id_t> hot_vertices;
    for (v_id_t v = 0; v < baseline.size() && hot_vertices.size() < 4; ++v) {
      if (db.vertex_index->get_vertex_location(v).first == hot_page) {
        hot_vertices.push_back(v);
      }
    }
    suite.expect(!hot_vertices.empty(), "selected vertices on one hot CSR page");

    std::vector<std::pair<v_id_t, v_id_t>> stress_edges;
    std::set<v_id_t> reserved;
    for (v_id_t src : hot_vertices) {
      v_id_t dst = choose_non_neighbor(baseline, src, reserved);
      reserved.insert(dst);
      stress_edges.emplace_back(src, dst);
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> invalid_read{false};
    std::atomic<uint64_t> read_count{0};
    std::atomic<uint64_t> write_count{0};

    std::vector<std::thread> readers;
    for (auto [src, dst] : stress_edges) {
      readers.emplace_back([&, src, dst] {
        while (!stop.load(std::memory_order_acquire)) {
          auto actual = sorted_unique(db.read_neighbor_clone(src));
          if (!valid_transient_read(actual, baseline[src], dst)) {
            invalid_read.store(true, std::memory_order_release);
            stop.store(true, std::memory_order_release);
            break;
          }
          read_count.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    std::vector<std::thread> writers;
    for (auto [src, dst] : stress_edges) {
      writers.emplace_back([&, src, dst] {
        while (!stop.load(std::memory_order_acquire)) {
          db.insert_edge(src, dst);
          db.delete_edge(src, dst);
          write_count.fetch_add(2, std::memory_order_relaxed);
          std::this_thread::yield();
        }
      });
    }

    for (int round = 0; round < 48 && !invalid_read.load(std::memory_order_acquire); ++round) {
      force_consolidate(db, hot_page);
      hot_page = db.vertex_index->get_vertex_location(hot_vertices.front()).first;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop.store(true, std::memory_order_release);
    for (auto& writer : writers) {
      writer.join();
    }
    for (auto& reader : readers) {
      reader.join();
    }
    db.wait_for_pending_consolidations();

    for (auto [src, dst] : stress_edges) {
      db.delete_edge(src, dst);
    }
    std::set<page_no_t> final_pages;
    for (v_id_t src : hot_vertices) {
      final_pages.insert(db.vertex_index->get_vertex_location(src).first);
    }
    for (page_no_t page_no : final_pages) {
      force_consolidate(db, page_no);
    }
    db.wait_for_pending_consolidations();
    db.vertex_index->wait_for_version_gc();

    suite.expect(!invalid_read.load(), "all concurrent reads observed a valid vertex state");
    suite.expect(read_count.load() > 0, "reader threads completed operations");
    suite.expect(write_count.load() > 0, "writer threads completed operations");

    bool final_ok = true;
    for (v_id_t src : hot_vertices) {
      if (sorted_unique(db.read_neighbor_clone(src)) != baseline[src]) {
        final_ok = false;
        break;
      }
    }
    suite.expect(final_ok, "final neighbor lists match the graph baseline");
    suite.expect(db.vertex_index->get_reclaimed_version_count() > 0,
                 "background hazard GC reclaimed old vertex versions");

    std::filesystem::current_path(repo_root);
    std::filesystem::remove_all(run_dir);
  } catch (const std::exception& e) {
    suite.expect(false, std::string("unexpected exception: ") + e.what());
  } catch (...) {
    suite.expect(false, "unexpected non-standard exception");
  }

  return suite.finish();
}
