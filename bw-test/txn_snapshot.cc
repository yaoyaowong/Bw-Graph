#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
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

#ifdef BW_GRAPH_ENABLE_TRANSACTION

class phase_barrier_t {
public:
  explicit phase_barrier_t(size_t participants) : participants_(participants) {}

  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const size_t generation = generation_;
    if (++arrived_ == participants_) {
      arrived_ = 0;
      ++generation_;
      cv_.notify_all();
      return;
    }
    cv_.wait(lock, [&] { return generation_ != generation; });
  }

private:
  const size_t participants_;
  size_t arrived_{0};
  size_t generation_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
};

std::vector<v_id_t> sorted_unique(std::vector<v_id_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

bool contains(const std::vector<v_id_t>& neighbors, v_id_t dst) {
  return std::find(neighbors.begin(), neighbors.end(), dst) != neighbors.end();
}

std::filesystem::path make_temp_run_dir() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("bw_test_txn_snapshot_" + std::to_string(suffix));
  std::filesystem::create_directories(dir / "data");
  std::filesystem::create_directories(dir / "workspace");
  return dir;
}

void write_graph(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "t 8 4\n";
  for (v_id_t vertex = 0; vertex < 8; ++vertex) {
    out << "v " << vertex << " 0 0\n";
  }
  out << "e 0 1\n";
  out << "e 0 2\n";
  out << "e 3 4\n";
  out << "e 5 6\n";
}

void force_consolidation(bw_graph_db_t& db, page_no_t page_no) {
  const uint64_t saved_threshold = bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH;
  bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;
  csr_page_t* page = db.buffer_pool->buf_page_read(page_no, db.disk_manager);
  page->r_unlatch();
  db.smo_ctl->consolidate_pages(page, db.page_map_table, db.disk_manager, db.buffer_pool,
                                db.vertex_index, db.giant_db, db.vertex_update_sketch_, 1);
  db.wait_for_pending_consolidations();
  bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = saved_threshold;
}

std::vector<v_id_t> read_csr_neighbors(bw_graph_db_t& db, v_id_t vertex) {
  vertex_loc_t location = db.vertex_index->get_vertex_location(vertex);
  csr_page_t* page = db.buffer_pool->buf_page_read(location.first, db.disk_manager);
  neighbor_span_t span = page->get_neighbors(location.second);
  std::vector<v_id_t> result(span.begin(), span.end());
  page->r_unlatch();
  return sorted_unique(std::move(result));
}

void test_snapshot_isolation(suite_t& suite, bw_graph_db_t& db) {
  constexpr v_id_t src = 0;
  constexpr v_id_t inserted = 3;
  phase_barrier_t staged_write(3);
  phase_barrier_t readers_checked_uncommitted(3);
  phase_barrier_t writer_committed(3);

  bool writer_commit_ok = false;
  bool reader_b_before = false;
  bool reader_b_after = false;
  bool reader_c_before = false;
  bool reader_c_new_snapshot = false;

  std::thread writer([&] {
    std::unique_ptr<txn_t> txn = db.begin_txn();
    txn->insert_edge(src, inserted);
    staged_write.arrive_and_wait();
    readers_checked_uncommitted.arrive_and_wait();
    writer_commit_ok = txn->commit();
    writer_committed.arrive_and_wait();
  });

  std::thread reader_b([&] {
    std::unique_ptr<txn_t> txn = db.begin_txn();
    staged_write.arrive_and_wait();
    reader_b_before = !contains(txn->get_neighbors(src), inserted);
    readers_checked_uncommitted.arrive_and_wait();
    writer_committed.arrive_and_wait();
    reader_b_after = !contains(txn->get_neighbors(src), inserted);
  });

  std::thread reader_c([&] {
    std::unique_ptr<txn_t> txn = db.begin_txn();
    staged_write.arrive_and_wait();
    reader_c_before = !contains(txn->get_neighbors(src), inserted);
    readers_checked_uncommitted.arrive_and_wait();
    writer_committed.arrive_and_wait();
    txn.reset();
    txn = db.begin_txn();
    reader_c_new_snapshot = contains(txn->get_neighbors(src), inserted);
  });

  writer.join();
  reader_b.join();
  reader_c.join();

  suite.expect(writer_commit_ok, "writer transaction commits");
  suite.expect(reader_b_before && reader_c_before,
               "concurrent readers cannot see an uncommitted edge");
  suite.expect(reader_b_after, "existing reader keeps its original snapshot after commit");
  suite.expect(reader_c_new_snapshot, "new reader sees the committed edge");
}

void test_watermark_consolidation(suite_t& suite, bw_graph_db_t& db) {
  constexpr v_id_t src = 3;
  constexpr v_id_t early_edge = 5;
  constexpr v_id_t late_edge = 7;

  {
    std::unique_ptr<txn_t> early = db.begin_txn();
    early->insert_edge(src, early_edge);
    suite.expect(early->commit(), "early transaction commits");
  }
  {
    std::unique_ptr<txn_t> advance = db.begin_txn();
    advance->insert_edge(4, 6);
    suite.expect(advance->commit(), "timestamp-advance transaction commits");
  }

  std::unique_ptr<txn_t> long_reader = db.begin_txn();
  suite.expect(contains(long_reader->get_neighbors(src), early_edge),
               "long reader snapshot includes early edge");

  {
    std::unique_ptr<txn_t> late = db.begin_txn();
    late->insert_edge(src, late_edge);
    suite.expect(late->commit(), "late transaction commits");
  }

  page_no_t old_page = db.vertex_index->get_vertex_location(src).first;
  force_consolidation(db, old_page);

  std::vector<v_id_t> first_csr = read_csr_neighbors(db, src);
  suite.expect(contains(first_csr, early_edge), "record older than watermark is merged into CSR");
  suite.expect(!contains(first_csr, late_edge), "record at or after watermark stays out of CSR");
  suite.expect(db.vertex_index->delta_heads_[src].is_valid(),
               "deferred late record remains in delta chain");
  suite.expect(!contains(long_reader->get_neighbors(src), late_edge),
               "long reader does not see late committed edge");

  {
    std::unique_ptr<txn_t> current_reader = db.begin_txn();
    suite.expect(contains(current_reader->get_neighbors(src), late_edge),
                 "new reader sees deferred late edge");
  }

  long_reader.reset();
  {
    std::unique_ptr<txn_t> advance = db.begin_txn();
    advance->insert_edge(6, 7);
    suite.expect(advance->commit(), "post-reader timestamp advance commits");
  }

  page_no_t current_page = db.vertex_index->get_vertex_location(src).first;
  force_consolidation(db, current_page);

  std::vector<v_id_t> second_csr = read_csr_neighbors(db, src);
  suite.expect(contains(second_csr, late_edge),
               "deferred record is merged after oldest reader finishes");
  suite.expect(!db.vertex_index->delta_heads_[src].is_valid(),
               "delta chain is empty after deferred record consolidation");
}

#endif

int main() {
  suite_t suite("txn_snapshot");

#ifndef BW_GRAPH_ENABLE_TRANSACTION
  suite.expect(true, "transaction support disabled; suite skipped");
  return suite.finish();
#else
  try {
    std::filesystem::path run_dir = make_temp_run_dir();
    const std::string graph_name = "txn_snapshot";
    write_graph(run_dir / "data" / (graph_name + ".graph"));
    std::filesystem::current_path(run_dir);

    bw_graph::BW_DELTA_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_CSR_FLUSH_INTERVAL_MS = 0;
    bw_graph::BW_GRAPH_LEVELED = false;
    bw_graph::BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1000000;

    bw_graph_db_t db(graph_name, true, true, false, false, false);
    test_snapshot_isolation(suite, db);
    test_watermark_consolidation(suite, db);
  } catch (const std::exception& e) {
    suite.expect(false, std::string("unexpected exception: ") + e.what());
  } catch (...) {
    suite.expect(false, "unexpected non-standard exception");
  }

  return suite.finish();
#endif
}
