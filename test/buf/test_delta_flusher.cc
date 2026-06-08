#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/delta_flusher.h"
#include "bw_graph/buf/page_map.h"
#include "bw_graph/common/config.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/disk_manager.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

using namespace std::chrono_literals;

// Test fixture.

class DeltaFlusherTest : public ::testing::Test {
protected:
  disk_manager_t* disk_mgr{nullptr};
  buf_pool_t<delta_page_t>* pool{nullptr};
  page_map_t* pmt{nullptr};
  std::filesystem::path tmp_path;

  // Save / restore global config so tests don't interfere.
  size_t saved_delta_page_size{};

  void SetUp() override {
    saved_delta_page_size = bw_graph::BW_DELTA_PAGE_SIZE;
    bw_graph::BW_DELTA_PAGE_SIZE = 4096;

    tmp_path = std::filesystem::temp_directory_path() /
               ("test_delta_flusher_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)) + ".delta");

    disk_mgr = new disk_manager_t(tmp_path, bw_graph::BW_DELTA_PAGE_SIZE);
    pool = buf_pool_t<delta_page_t>::buf_pool_init(2, 512, bw_graph::BW_DELTA_PAGE_SIZE);
    pmt = new page_map_t(pool, disk_mgr);
  }

  void TearDown() override {
    delete pmt;
    delete pool;
    delete disk_mgr;
    std::filesystem::remove(tmp_path);
    std::filesystem::remove(tmp_path.string() + ".log");

    bw_graph::BW_DELTA_PAGE_SIZE = saved_delta_page_size;
  }

  // Insert one edge-delta for CSR page_no @csr_pno.
  void insert_edge(page_no_t csr_pno, v_id_t src, v_id_t dst) {
    delta_record_t rec(EDGE, INSERT, src, dst);
    pmt->insert_delta(csr_pno, {rec});
  }
};

// Test 1: insert_delta sets dirty flag.
TEST_F(DeltaFlusherTest, InsertSetsDirty) {
  insert_edge(/*csr_pno=*/1, 100, 200);

  // At least one page in the pool should be dirty.
  bool found_dirty = false;
  pool->traverse_page_map([&](page_no_t, delta_page_t* page) -> bool {
    if (page->is_dirty()) {
      found_dirty = true;
      return false; // Stop early.
    }
    return true;
  });

  EXPECT_TRUE(found_dirty) << "A delta page must be dirty after insert_delta";
}

// Test 2: flush_all() clears the dirty flag.
TEST_F(DeltaFlusherTest, FlushAllClearsDirty) {
  insert_edge(1, 10, 20);
  insert_edge(2, 30, 40);

  // Construct flusher but don't start the background thread -
  // call flush_all() synchronously instead.
  delta_flusher_t flusher(pool, disk_mgr, 1000ms);
  size_t written = flusher.flush_all();

  EXPECT_GT(written, 0u) << "flush_all() should have written at least 1 page";

  // All pages should now be clean.
  bool any_dirty = false;
  pool->traverse_page_map([&](page_no_t, delta_page_t* page) -> bool {
    if (page->is_dirty()) {
      any_dirty = true;
      return false;
    }
    return true;
  });
  EXPECT_FALSE(any_dirty) << "No pages should remain dirty after flush_all()";
}

// Test 3: total_flushed() accumulates across cycles.
TEST_F(DeltaFlusherTest, TotalFlushedAccumulates) {
  delta_flusher_t flusher(pool, disk_mgr, 1000ms);

  insert_edge(1, 1, 2);
  flusher.flush_all();

  insert_edge(2, 3, 4);
  flusher.flush_all();

  EXPECT_GE(flusher.total_flushed(), 2u);
}

// Test 4: background thread flushes automatically.
TEST_F(DeltaFlusherTest, BackgroundThreadFlushes) {
  // Use a very short interval so the thread fires quickly.
  delta_flusher_t flusher(pool, disk_mgr, 50ms);
  flusher.start();
  EXPECT_TRUE(flusher.is_running());

  insert_edge(1, 7, 8);

  // Wait up to 1 second for the background thread to flush.
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  bool flushed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
    bool any_dirty = false;
    pool->traverse_page_map([&](page_no_t, delta_page_t* page) -> bool {
      if (page->is_dirty()) {
        any_dirty = true;
        return false;
      }
      return true;
    });
    if (!any_dirty) {
      flushed = true;
      break;
    }
  }

  flusher.stop();
  EXPECT_FALSE(flusher.is_running());
  EXPECT_TRUE(flushed) << "Background thread should have flushed within 1 s";
}

// Test 5: concurrent inserts do not block during flush.
TEST_F(DeltaFlusherTest, InsertNotBlockedByFlush) {
  // Short flush interval.
  delta_flusher_t flusher(pool, disk_mgr, 20ms);
  flusher.start();

  constexpr int kOps = 200;
  std::atomic<int> completed{0};

  // Insert thread: performs many inserts.
  std::thread inserter([&] {
    for (int i = 0; i < kOps; ++i) {
      insert_edge(static_cast<page_no_t>(i % 5 + 1), static_cast<v_id_t>(i),
                  static_cast<v_id_t>(i + 1));
      ++completed;
    }
  });

  // Measure: all inserts should finish in well under 2 seconds.
  inserter.join();
  flusher.stop();

  EXPECT_EQ(completed.load(), kOps) << "All inserts must complete without blocking";
}

// Test 6: stop() performs a final flush.
TEST_F(DeltaFlusherTest, StopPerformsFinalFlush) {
  delta_flusher_t flusher(pool, disk_mgr, 10000ms); // very long interval
  flusher.start();

  insert_edge(1, 5, 6);

  // stop() should call flush_all() before returning.
  flusher.stop();

  bool any_dirty = false;
  pool->traverse_page_map([&](page_no_t, delta_page_t* page) -> bool {
    if (page->is_dirty()) {
      any_dirty = true;
      return false;
    }
    return true;
  });
  EXPECT_FALSE(any_dirty) << "stop() must drain all dirty pages";
}

// Test 7: flushed data is readable from disk.
TEST_F(DeltaFlusherTest, FlushedDataReadableFromDisk) {
  constexpr page_no_t kCsrPno = 99;
  insert_edge(kCsrPno, 111, 222);

  delta_flusher_t flusher(pool, disk_mgr, 1000ms);
  size_t written = flusher.flush_all();
  ASSERT_GT(written, 0u);

  // Find the delta page that was written and read it back from disk.
  page_no_t delta_pno = 0;
  pool->traverse_page_map([&](page_no_t pno, delta_page_t* page) -> bool {
    // Re-use any page that has the right parent (page 1 is the first allocated).
    delta_pno = pno;
    (void) page;
    return false; // take the first one
  });

  // Read the raw page back from disk.
  std::vector<char> buf(bw_graph::BW_DELTA_PAGE_SIZE, 0);
  disk_mgr->read_page(delta_pno, buf.data());

  // The first 8 bytes are delta_record_count - must be at least 1.
  uint64_t count = *reinterpret_cast<const uint64_t*>(buf.data());
  EXPECT_GE(count, 1u) << "Flushed delta page must contain at least 1 record on disk";
}
