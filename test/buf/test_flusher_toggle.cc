#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/csr_flusher.h"
#include "bw_graph/buf/delta_flusher.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/common/config.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// Fixture provides a delta pool/disk pair plus a CSR pool/disk pair so the
// same suite can verify dirty propagation, the disabled toggle, and the
// thread-pool flush semantics for both flushers.
class FlusherToggleTest : public ::testing::Test {
protected:
  size_t saved_delta_page_size{};
  size_t saved_csr_page_size{};

  std::filesystem::path delta_path;
  std::filesystem::path csr_path;

  disk_manager_t* delta_disk{nullptr};
  disk_manager_t* csr_disk{nullptr};
  buf_pool_t<delta_page_t>* delta_pool{nullptr};
  buf_pool_t<csr_page_t>* csr_pool{nullptr};
  page_map_t* pmt{nullptr};

  void SetUp() override {
    saved_delta_page_size = bw_graph::BW_DELTA_PAGE_SIZE;
    saved_csr_page_size = bw_graph::BW_GRAPH_PAGE_SIZE;
    bw_graph::BW_DELTA_PAGE_SIZE = 4096;
    bw_graph::BW_GRAPH_PAGE_SIZE = 1024;

    auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    delta_path = std::filesystem::temp_directory_path() / ("test_flusher_toggle_delta_" + stamp);
    csr_path = std::filesystem::temp_directory_path() / ("test_flusher_toggle_csr_" + stamp);

    delta_disk = new disk_manager_t(delta_path, bw_graph::BW_DELTA_PAGE_SIZE);
    csr_disk = new disk_manager_t(csr_path, bw_graph::BW_GRAPH_PAGE_SIZE);

    delta_pool = buf_pool_t<delta_page_t>::buf_pool_init(2, 512, bw_graph::BW_DELTA_PAGE_SIZE);
    csr_pool = buf_pool_t<csr_page_t>::buf_pool_init(2, 256, bw_graph::BW_GRAPH_PAGE_SIZE);

    pmt = new page_map_t(delta_pool, delta_disk);
  }

  void TearDown() override {
    delete pmt;
    delete delta_pool;
    delete csr_pool;
    delete delta_disk;
    delete csr_disk;
    std::filesystem::remove(delta_path);
    std::filesystem::remove(delta_path.string() + ".log");
    std::filesystem::remove(csr_path);
    std::filesystem::remove(csr_path.string() + ".log");

    bw_graph::BW_DELTA_PAGE_SIZE = saved_delta_page_size;
    bw_graph::BW_GRAPH_PAGE_SIZE = saved_csr_page_size;
  }

  // Insert one edge-delta for CSR page_no @csr_pno.
  void insert_edge(page_no_t csr_pno, v_id_t src, v_id_t dst) {
    delta_record_t rec(EDGE, INSERT, src, dst);
    pmt->insert_delta(csr_pno, {rec});
  }

  // Count dirty delta pages currently held in the pool.
  size_t count_dirty_delta() {
    size_t n = 0;
    delta_pool->traverse_page_map([&](page_no_t, delta_page_t* page) -> bool {
      if (page->is_dirty()) {
        ++n;
      }
      return true;
    });
    return n;
  }
};

// Dirty-mechanism check: a freshly reinitialised delta-page frame must be
// marked dirty so that its new metadata is eventually persisted.
TEST_F(FlusherToggleTest, ReinitMarksDirty) {
  delta_page_t page(/*page_no=*/7);
  page.mark_clean();
  ASSERT_FALSE(page.is_dirty());

  page.reinit(/*page_no=*/7);

  EXPECT_TRUE(page.is_dirty()) << "reinit must publish a fresh on-disk image";
}

// Dirty-mechanism check: set_parent_page_no rewrites a header byte so the
// page must transition into the dirty state.
TEST_F(FlusherToggleTest, SetParentMarksDirty) {
  delta_page_t page(/*page_no=*/3);
  page.mark_clean();
  ASSERT_FALSE(page.is_dirty());

  page.set_parent_page_no(99);

  EXPECT_TRUE(page.is_dirty()) << "set_parent_page_no must mark the page dirty";
}

// Disabled delta flusher (worker_count == 0) must not leak a thread, must
// report itself as disabled, and must not drain dirty pages.
TEST_F(FlusherToggleTest, DeltaFlusher_DisabledWhenZeroWorkers) {
  delta_flusher_t flusher(delta_pool, delta_disk, 50ms, /*worker_count=*/0);
  EXPECT_FALSE(flusher.is_enabled());
  EXPECT_EQ(flusher.get_worker_count(), 0u);

  // start() must be a no-op (no dispatcher thread spawned).
  flusher.start();
  EXPECT_FALSE(flusher.is_running());

  insert_edge(/*csr_pno=*/1, 100, 200);
  size_t dirty_before = count_dirty_delta();
  ASSERT_GT(dirty_before, 0u) << "insert_delta must produce a dirty page";

  // flush_all() must short-circuit and not write anything.
  EXPECT_EQ(flusher.flush_all(), 0u);
  EXPECT_EQ(flusher.total_flushed(), 0u);
  EXPECT_EQ(count_dirty_delta(), dirty_before)
      << "disabled flusher must not clean any dirty page";

  // stop() is also a no-op (no dispatcher to join).
  flusher.stop();
}

// Single-worker delta flusher must drain every dirty page when flush_all()
// is invoked, just like the legacy implementation.
TEST_F(FlusherToggleTest, DeltaFlusher_SingleWorkerDrains) {
  delta_flusher_t flusher(delta_pool, delta_disk, 1000ms, /*worker_count=*/1);
  EXPECT_TRUE(flusher.is_enabled());

  for (int i = 0; i < 5; ++i) {
    insert_edge(static_cast<page_no_t>(i + 1), static_cast<v_id_t>(i),
                static_cast<v_id_t>(i + 100));
  }
  size_t dirty_before = count_dirty_delta();
  ASSERT_GT(dirty_before, 0u);

  size_t written = flusher.flush_all();
  EXPECT_EQ(written, dirty_before);
  EXPECT_EQ(count_dirty_delta(), 0u) << "single-worker flusher must clear all dirty pages";
}

// Multi-worker delta flusher must remain correct: every dirty page is
// written exactly once, total_flushed() matches the dirty-page count, and
// no page stays dirty afterwards.
TEST_F(FlusherToggleTest, DeltaFlusher_MultiWorkerDrains) {
  delta_flusher_t flusher(delta_pool, delta_disk, 1000ms, /*worker_count=*/4);
  EXPECT_TRUE(flusher.is_enabled());
  EXPECT_EQ(flusher.get_worker_count(), 4u);

  // Spread dirty pages across many CSR page numbers to encourage parallel
  // dispatch onto the arena.
  for (int i = 0; i < 32; ++i) {
    insert_edge(static_cast<page_no_t>(i + 1), static_cast<v_id_t>(i),
                static_cast<v_id_t>(i + 1));
  }
  size_t dirty_before = count_dirty_delta();
  ASSERT_GT(dirty_before, 0u);

  size_t written = flusher.flush_all();
  EXPECT_EQ(written, dirty_before)
      << "multi-worker flusher must persist every dirty page exactly once";
  EXPECT_EQ(count_dirty_delta(), 0u);
  EXPECT_EQ(flusher.total_flushed(), dirty_before);
}

// Background dispatcher with worker_count > 0 must still wake on its own
// timer and clean dirty pages.
TEST_F(FlusherToggleTest, DeltaFlusher_BackgroundLoopDrains) {
  delta_flusher_t flusher(delta_pool, delta_disk, 50ms, /*worker_count=*/2);
  flusher.start();
  EXPECT_TRUE(flusher.is_running());

  insert_edge(/*csr_pno=*/1, 7, 8);
  insert_edge(/*csr_pno=*/2, 9, 10);

  const auto deadline = std::chrono::steady_clock::now() + 1s;
  bool flushed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
    if (count_dirty_delta() == 0) {
      flushed = true;
      break;
    }
  }

  flusher.stop();
  EXPECT_FALSE(flusher.is_running());
  EXPECT_TRUE(flushed) << "background dispatcher must flush within 1 s";
}

// stop() must perform a final synchronous flush before returning, even
// when the dispatcher was woken less frequently than dirties accrued.
TEST_F(FlusherToggleTest, DeltaFlusher_StopPerformsFinalFlush) {
  delta_flusher_t flusher(delta_pool, delta_disk, 10000ms, /*worker_count=*/2);
  flusher.start();

  insert_edge(/*csr_pno=*/1, 5, 6);
  flusher.stop();

  EXPECT_EQ(count_dirty_delta(), 0u) << "stop() must drain pending dirty pages";
}

// Build a tiny CSR page directly in the pool, mark it dirty, and verify the
// CSR flusher honours the same disabled/enabled semantics.
TEST_F(FlusherToggleTest, CsrFlusher_DisabledAndEnabled) {
  std::vector<char> page_data(bw_graph::BW_GRAPH_PAGE_SIZE, 0);
  page_no_t pno = static_cast<page_no_t>(csr_disk->allocate_page() / bw_graph::BW_GRAPH_PAGE_SIZE);
  csr_disk->write_page(pno, page_data.data());

  csr_page_t* frame = csr_pool->buf_page_cp(pno, page_data.data(), csr_disk);
  ASSERT_NE(frame, nullptr);
  frame->mark_dirty();
  ASSERT_TRUE(frame->is_dirty());

  // worker_count == 0: the flusher must leave dirty pages alone.
  {
    csr_flusher_t flusher(csr_pool, csr_disk, 50ms, /*worker_count=*/0);
    EXPECT_FALSE(flusher.is_enabled());
    flusher.start();
    EXPECT_FALSE(flusher.is_running());
    EXPECT_EQ(flusher.flush_all(), 0u);
    flusher.trigger_async_flush();
    EXPECT_TRUE(frame->is_dirty()) << "disabled CSR flusher must not clean any page";
    flusher.stop();
  }

  // worker_count > 0: the flusher must drain the dirty page.
  {
    csr_flusher_t flusher(csr_pool, csr_disk, 1000ms, /*worker_count=*/2);
    EXPECT_TRUE(flusher.is_enabled());
    EXPECT_EQ(flusher.flush_all(), 1u);
    EXPECT_FALSE(frame->is_dirty()) << "enabled CSR flusher must clear the dirty bit";
  }
}
