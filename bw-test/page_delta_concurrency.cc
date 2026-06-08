// MTR-style stress: validate that concurrent writers never lose deltas while
// a consolidation is in flight on the same CSR page.
//
// The test loads the bundled example graph, picks one hot CSR page that
// holds at least four vertices, then spawns N writer threads that flip
// (insert, delete) for a private edge per vertex.  The main thread keeps
// triggering consolidation on the same page in a loop.  After joining the
// writers and draining the SMO queue, every neighbor list must match the
// pre-test ground truth - any lost or duplicated delta would corrupt the
// observed degree.

#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
    if (condition) {
      std::cout << "ok - " << message << std::endl;
      return true;
    }
    std::cout << "not ok - " << message << std::endl;
    failed_ = true;
    return false;
  }

  int finish() const {
    std::cout << (failed_ ? "FAIL " : "PASS ") << name_ << std::endl;
    return failed_ ? 1 : 0;
  }

private:
  std::string name_;
  bool failed_{false};
};

// Parse the .graph file shipped with the repo into the canonical undirected
// neighbor lists used as ground truth.
std::vector<std::vector<v_id_t>> load_example_ground_truth(const std::filesystem::path& graph) {
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
      if (src >= neighbors.size() || dst >= neighbors.size() || src == dst) {
        continue;
      }
      neighbors[src].push_back(dst);
      neighbors[dst].push_back(src);
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

bool has_neighbor(const std::vector<v_id_t>& neighbors, v_id_t dst) {
  return std::binary_search(neighbors.begin(), neighbors.end(), dst);
}

v_id_t choose_non_neighbor(const std::vector<std::vector<v_id_t>>& expected, v_id_t src,
                           const std::set<v_id_t>& reserved) {
  for (v_id_t dst = 0; dst < expected.size(); ++dst) {
    if (dst != src && !reserved.contains(dst) && !has_neighbor(expected[src], dst)) {
      return dst;
    }
  }
  throw std::runtime_error("no non-neighbor candidate found");
}

bool check_all_neighbors(suite_t& suite, bw_graph_db_t& db,
                         const std::vector<std::vector<v_id_t>>& expected,
                         const std::string& phase) {
  bool ok = true;
  for (v_id_t vertex = 0; vertex < expected.size(); ++vertex) {
    std::vector<v_id_t> actual = sorted_unique(db.read_neighbor_clone(vertex));
    if (actual != expected[vertex]) {
      std::ostringstream oss;
      oss << phase << " vertex " << vertex << " mismatch; expected degree "
          << expected[vertex].size() << ", got " << actual.size();
      suite.expect(false, oss.str());
      ok = false;
      break;
    }
  }
  if (ok) {
    suite.expect(true, phase + " all vertices match ground truth");
  }
  return ok;
}

std::filesystem::path make_temp_run_dir() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("bw_test_page_delta_concurrency_" + std::to_string(suffix));
  std::filesystem::create_directories(dir / "data");
  std::filesystem::create_directories(dir / "workspace");
  return dir;
}

// Issue a synchronous consolidate on the given CSR page; used by the main
// thread to repeatedly fight with the writer threads.
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
  suite_t suite("page_delta_concurrency");

  try {
    if (argc < 2) {
      suite.expect(false, "repo root argument is required");
      return suite.finish();
    }

    std::filesystem::path repo_root = std::filesystem::absolute(argv[1]);
    std::filesystem::path source_graph = repo_root / "data" / "example.graph";
    auto expected = load_example_ground_truth(source_graph);
    suite.expect(!expected.empty(), "loaded example.graph ground truth");

    std::filesystem::path run_dir = make_temp_run_dir();
    std::filesystem::copy_file(source_graph, run_dir / "data" / "example.graph",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::current_path(run_dir);

    // Disable background flushers & raise the chain threshold so we control
    // every SMO event explicitly from the main thread.
    bw_graph::BW_DELTA_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_CSR_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_GRAPH_LEVELED = false;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1000000;
    bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 2;

    bw_graph_db_t db("example", true, true, false, false, false);
    suite.expect(db.get_vertex_count() == expected.size(), "database vertex count matches graph");
    check_all_neighbors(suite, db, expected, "initial");

    // Pick a hot CSR page and four vertices that live on it.
    v_id_t seed = 0;
    while (seed < expected.size() && expected[seed].empty()) {
      ++seed;
    }
    if (seed == expected.size()) {
      throw std::runtime_error("example.graph has no source vertex with neighbors");
    }
    page_no_t hot_page = db.vertex_index->get_vertex_location(seed).first;
    std::vector<v_id_t> hot_vertices;
    for (v_id_t vertex = 0; vertex < expected.size() && hot_vertices.size() < 4; ++vertex) {
      if (db.vertex_index->get_vertex_location(vertex).first == hot_page) {
        hot_vertices.push_back(vertex);
      }
    }
    suite.expect(!hot_vertices.empty(), "selected hot page vertices for stress");

    // Reserve one private (insert, delete) pair per writer so we can assert
    // ground-truth equivalence even after concurrent execution.
    std::vector<std::pair<v_id_t, v_id_t>> writer_edges;
    {
      std::set<v_id_t> reserved;
      for (v_id_t vertex : hot_vertices) {
        v_id_t dst = choose_non_neighbor(expected, vertex, reserved);
        reserved.insert(dst);
        writer_edges.emplace_back(vertex, dst);
      }
    }

    constexpr int kRounds = 32;
    constexpr int kOpsPerRound = 50;

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writer_ops{0};

    // Writer threads: each flips one private edge over and over.
    std::vector<std::thread> writers;
    writers.reserve(writer_edges.size());
    for (auto [src, dst] : writer_edges) {
      writers.emplace_back([&, src, dst]() {
        while (!stop.load(std::memory_order_acquire)) {
          for (int i = 0; i < kOpsPerRound; ++i) {
            db.insert_edge(src, dst);
            db.delete_edge(src, dst);
            writer_ops.fetch_add(2, std::memory_order_relaxed);
          }
          std::this_thread::yield();
        }
      });
    }

    // Consolidation loop: keeps forcing the same page through the SMO so
    // every writer iteration races against a Phase B/Phase C transition.
    for (int round = 0; round < kRounds; ++round) {
      force_consolidate(db, hot_page);
      // hot_page may have been retired; recompute from a live vertex.
      hot_page = db.vertex_index->get_vertex_location(hot_vertices.front()).first;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop.store(true, std::memory_order_release);
    for (auto& w : writers) {
      w.join();
    }
    db.wait_for_pending_consolidations();

    // Final sweep of consolidations to drain any straggler chain.
    std::set<page_no_t> final_pages;
    for (v_id_t v : hot_vertices) {
      final_pages.insert(db.vertex_index->get_vertex_location(v).first);
    }
    for (page_no_t p : final_pages) {
      force_consolidate(db, p);
    }
    db.wait_for_pending_consolidations();

    std::cout << "# total writer ops issued: " << writer_ops.load() << std::endl;

    // Every (src, dst) pair was inserted+deleted the same number of times, so
    // the final neighborhood must equal the ground truth.
    check_all_neighbors(suite, db, expected, "post-stress");

  } catch (const std::exception& e) {
    suite.expect(false, std::string("unexpected exception: ") + e.what());
  } catch (...) {
    suite.expect(false, "unexpected non-standard exception");
  }

  return suite.finish();
}
