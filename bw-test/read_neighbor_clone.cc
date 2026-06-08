#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

bool check_csr_neighbors(suite_t& suite, bw_graph_db_t& db, v_id_t vertex,
                         const std::vector<v_id_t>& expected, const std::string& phase) {
  vertex_loc_t location = db.vertex_index->get_vertex_location(vertex);
  csr_page_t* page = db.buffer_pool->buf_page_read(location.first, db.disk_manager);
  std::vector<v_id_t> actual = sorted_unique(std::vector<v_id_t>(
      page->get_neighbors(location.second).begin(), page->get_neighbors(location.second).end()));
  page->r_unlatch();
  return suite.expect(actual == sorted_unique(expected),
                      phase + " vertex " + std::to_string(vertex) + " stored in CSR page");
}

void add_expected_edge(std::vector<std::vector<v_id_t>>& expected, v_id_t src, v_id_t dst) {
  auto& list = expected[src];
  if (!has_neighbor(list, dst)) {
    list.push_back(dst);
    std::sort(list.begin(), list.end());
  }
}

void remove_expected_edge(std::vector<std::vector<v_id_t>>& expected, v_id_t src, v_id_t dst) {
  auto& list = expected[src];
  list.erase(std::remove(list.begin(), list.end(), dst), list.end());
}

std::filesystem::path make_temp_run_dir() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("bw_test_read_neighbor_clone_" + std::to_string(suffix));
  std::filesystem::create_directories(dir / "data");
  std::filesystem::create_directories(dir / "workspace");
  return dir;
}

void force_consolidation_for_pages(bw_graph_db_t& db, const std::set<page_no_t>& page_nos) {
  uint64_t saved_threshold = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
  bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;
  for (page_no_t page_no : page_nos) {
    if (db.page_map_table->get_delta_chain_length(page_no) == 0) {
      continue;
    }
    csr_page_t* page = db.buffer_pool->buf_page_read(page_no, db.disk_manager);
    page->r_unlatch();
    db.smo_ctl->consolidate_pages(page, db.page_map_table, db.disk_manager, db.buffer_pool,
                                  db.vertex_index, db.giant_db, db.vertex_update_sketch_);
  }
  bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = saved_threshold;
  db.wait_for_pending_consolidations();
  // delete_page fires inside a vertex_version_reclaim_batch_t callback which
  // only runs once the old vertex versions are GC'd. Drain GC so that any
  // get_num_deletes() check after this call sees the final delete count.
  db.vertex_index->wait_for_version_gc();
}

int main(int argc, char** argv) {
  suite_t suite("read_neighbor_clone");

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

    bw_graph::BW_DELTA_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_CSR_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_GRAPH_LEVELED = false;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1000000;

    bw_graph_db_t db("example", true, true, false, false, false);
    suite.expect(db.get_vertex_count() == expected.size(), "database vertex count matches graph");
    check_all_neighbors(suite, db, expected, "initial read_neighbor_clone");

    v_id_t src = 0;
    while (src < expected.size() && expected[src].empty()) {
      ++src;
    }
    if (src == expected.size()) {
      throw std::runtime_error("example.graph has no source vertex with neighbors");
    }

    std::set<v_id_t> reserved;
    v_id_t inserted_then_kept = choose_non_neighbor(expected, src, reserved);
    reserved.insert(inserted_then_kept);
    v_id_t inserted_then_deleted = choose_non_neighbor(expected, src, reserved);
    v_id_t deleted_existing = expected[src].front();

    std::set<page_no_t> touched_pages;
    touched_pages.insert(db.vertex_index->get_vertex_location(src).first);

    suite.expect(db.delete_edge(src, inserted_then_kept), "delete missing edge before insert");
    suite.expect(db.insert_edge(src, inserted_then_kept), "insert edge that should remain");
    add_expected_edge(expected, src, inserted_then_kept);

    suite.expect(db.insert_edge(src, inserted_then_deleted), "insert edge that will be deleted");
    suite.expect(db.delete_edge(src, inserted_then_deleted), "delete newly inserted edge");

    suite.expect(db.delete_edge(src, deleted_existing), "delete existing edge");
    remove_expected_edge(expected, src, deleted_existing);

    check_all_neighbors(suite, db, expected, "delta read_neighbor_clone");

    force_consolidation_for_pages(db, touched_pages);
    check_all_neighbors(suite, db, expected, "post-consolidation read_neighbor_clone");

    page_no_t promotion_seed_page = db.vertex_index->get_vertex_location(src).first;
    v_id_t promote_src = src;
    size_t page_max_degree = expected[src].size();
    for (v_id_t vertex = 0; vertex < expected.size(); ++vertex) {
      if (db.vertex_index->is_giant_vertex(vertex)) {
        continue;
      }
      if (db.vertex_index->get_vertex_location(vertex).first != promotion_seed_page) {
        continue;
      }
      if (expected[vertex].size() > page_max_degree) {
        promote_src = vertex;
        page_max_degree = expected[vertex].size();
      }
    }

    uint64_t saved_giant_bound = bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND;
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = static_cast<uint64_t>(page_max_degree + 3);
    suite.expect(!db.vertex_index->is_giant_vertex(promote_src),
                 "promotion source starts as normal vertex");

    std::set<v_id_t> promotion_reserved(expected[promote_src].begin(), expected[promote_src].end());
    for (size_t i = 0; i < 3; ++i) {
      v_id_t dst = choose_non_neighbor(expected, promote_src, promotion_reserved);
      promotion_reserved.insert(dst);
      suite.expect(db.insert_edge(promote_src, dst), "insert edge toward giant promotion");
      add_expected_edge(expected, promote_src, dst);
    }

    page_no_t promotion_page = db.vertex_index->get_vertex_location(promote_src).first;
    force_consolidation_for_pages(db, {promotion_page});
    suite.expect(db.vertex_index->is_giant_vertex(promote_src),
                 "consolidation promotes normal vertex to giant");
    suite.expect(sorted_unique(db.giant_db->read_neighbor_clone(promote_src)) ==
                     sorted_unique(expected[promote_src]),
                 "promoted giant external neighbors match ground truth");

    vertex_loc_t promoted_loc = db.vertex_index->get_vertex_location(promote_src);
    csr_page_t* promoted_page = db.buffer_pool->buf_page_read(promoted_loc.first, db.disk_manager);
    neighbor_span_t promoted_csr_neighbors =
        promoted_page->get_neighbors(static_cast<uint16_t>(promoted_loc.second));
    suite.expect(promoted_csr_neighbors.empty(), "promoted giant stores no CSR neighbor edges");
    promoted_page->r_unlatch();

    v_id_t post_promotion_dst = choose_non_neighbor(expected, promote_src, promotion_reserved);
    promotion_reserved.insert(post_promotion_dst);
    suite.expect(db.insert_edge(promote_src, post_promotion_dst),
                 "insert edge after giant promotion");
    add_expected_edge(expected, promote_src, post_promotion_dst);
    suite.expect(sorted_unique(db.read_neighbor_clone(promote_src)) ==
                     sorted_unique(expected[promote_src]),
                 "promoted giant delta read_neighbor_clone");

    page_no_t post_promotion_page = db.vertex_index->get_vertex_location(promote_src).first;
    force_consolidation_for_pages(db, {post_promotion_page});
    suite.expect(sorted_unique(db.giant_db->read_neighbor_clone(promote_src)) ==
                     sorted_unique(expected[promote_src]),
                 "post-promotion consolidation updates external neighbors");
    bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND = saved_giant_bound;
    check_all_neighbors(suite, db, expected, "post-promotion read_neighbor_clone");

    uint64_t deletes_before_stress = db.disk_manager->get_num_deletes();
    page_no_t hot_page = db.vertex_index->get_vertex_location(src).first;
    std::vector<v_id_t> hot_vertices;
    for (v_id_t vertex = 0; vertex < expected.size() && hot_vertices.size() < 4; ++vertex) {
      if (db.vertex_index->get_vertex_location(vertex).first == hot_page) {
        hot_vertices.push_back(vertex);
      }
    }
    suite.expect(!hot_vertices.empty(), "selected hot page vertices for threaded updates");

    std::vector<std::pair<v_id_t, v_id_t>> stress_edges;
    for (v_id_t vertex : hot_vertices) {
      std::set<v_id_t> reserved_for_vertex;
      stress_edges.emplace_back(vertex, choose_non_neighbor(expected, vertex, reserved_for_vertex));
    }

    std::vector<std::thread> workers;
    for (auto [stress_src, stress_dst] : stress_edges) {
      workers.emplace_back([&db, stress_src, stress_dst]() {
        for (int i = 0; i < 64; ++i) {
          db.insert_edge(stress_src, stress_dst);
          db.delete_edge(stress_src, stress_dst);
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }

    force_consolidation_for_pages(db, {hot_page});
    check_all_neighbors(suite, db, expected, "threaded post-consolidation read_neighbor_clone");
    suite.expect(db.disk_manager->get_num_deletes() > deletes_before_stress,
                 "threaded hot-page consolidation retired at least one old CSR slot");

    const std::string reopen_graph_name =
        "reopen_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::copy_file(source_graph, run_dir / "data" / (reopen_graph_name + ".graph"),
                               std::filesystem::copy_options::overwrite_existing);
    db.close();
    bw_graph::CSR_PREFIX = (run_dir / "csr-pages").string();
    bw_graph::DELTA_PREFIX = (run_dir / "delta-pages").string();
    bw_graph::META_PREFIX = (run_dir / "metadata").string();

    std::vector<std::pair<v_id_t, std::vector<v_id_t>>> inserted_vertices;
    {
      bw_graph_db_t reopen_db(reopen_graph_name, true, true, false, false, false);
      const v_id_t original_vertex_count = reopen_db.get_vertex_count();
      std::set<page_no_t> consolidation_pages;

      for (size_t i = 0; i < 3; ++i) {
        std::vector<v_id_t> neighbors = {static_cast<v_id_t>(i),
                                         static_cast<v_id_t>((i + 1) % original_vertex_count),
                                         static_cast<v_id_t>((i + 2) % original_vertex_count)};
        v_id_t inserted_vertex = reopen_db.insert_vertex_with_neighbors(neighbors);
        inserted_vertices.emplace_back(inserted_vertex, neighbors);
        consolidation_pages.insert(
            reopen_db.vertex_index->get_vertex_location(inserted_vertex).first);
      }

      force_consolidation_for_pages(reopen_db, consolidation_pages);
      reopen_db.wait_for_pending_consolidations();
      for (const auto& [vertex, neighbors] : inserted_vertices) {
        suite.expect(!reopen_db.vertex_index->delta_heads_[vertex].is_valid(),
                     "inserted vertex delta chain cleared after consolidation");
        check_csr_neighbors(suite, reopen_db, vertex, neighbors, "before close");
      }
      reopen_db.close();
    }

    suite.expect(std::filesystem::is_regular_file(bw_graph::csr_db_path(reopen_graph_name)),
                 "CSR page file uses csr_prefix");
    suite.expect(std::filesystem::is_regular_file(bw_graph::delta_db_path(reopen_graph_name)),
                 "Delta page file uses delta_prefix");
    suite.expect(std::filesystem::is_regular_file(bw_graph::vertex_index_path(reopen_graph_name)),
                 "vertex index uses meta_prefix");
    suite.expect(std::filesystem::is_directory(bw_graph::giant_db_path(reopen_graph_name)),
                 "giant database uses meta_prefix");
    suite.expect(std::filesystem::is_directory(bw_graph::property_db_path(reopen_graph_name)),
                 "property database uses meta_prefix");

    std::unique_ptr<bw_graph_db_t> reopened = bw_graph_db_t::open(reopen_graph_name);
    suite.expect(reopened->get_vertex_count() == inserted_vertices.back().first + 1,
                 "reopened database restores inserted vertex count");
    for (const auto& [vertex, neighbors] : inserted_vertices) {
      suite.expect(!reopened->vertex_index->delta_heads_[vertex].is_valid(),
                   "reopened vertex has no pending delta chain");
      check_csr_neighbors(suite, *reopened, vertex, neighbors, "after reopen");
      suite.expect(sorted_unique(reopened->read_neighbor_clone(vertex)) == sorted_unique(neighbors),
                   "after reopen read_neighbor_clone matches inserted neighbors");
    }
    reopened->close();

  } catch (const std::exception& e) {
    suite.expect(false, std::string("unexpected exception: ") + e.what());
  } catch (...) {
    suite.expect(false, "unexpected non-standard exception");
  }

  return suite.finish();
}
