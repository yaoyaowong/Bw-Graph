#include <atomic>
#include <bw_graph/buf/buf_pool.h>
#include <bw_graph/common/config.h>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

// Test BufChunkTest.init test.
TEST(BufChunkTest, InitTest) {
  auto* chunk = buf_chunk_t<csr_page_t>::buf_chunk_init(10, bw_graph::BW_GRAPH_PAGE_SIZE);
  EXPECT_NE(chunk, nullptr);
  delete chunk;
}

// Test BufChunkTest.memory initialization test.
TEST(BufChunkTest, MemoryInitializationTest) {
  size_t page_count = 5;
  auto* chunk = buf_chunk_t<csr_page_t>::buf_chunk_init(page_count, bw_graph::BW_GRAPH_PAGE_SIZE);

  EXPECT_NE(chunk, nullptr);
  EXPECT_NE(chunk->pages, nullptr);
  EXPECT_NE(chunk->frames, nullptr);

  // Test that memory is properly allocated and accessible
  const char* test_data = "Hello, Buffer Pool!";
  char* first_page = chunk->frames;
  strcpy(first_page, test_data);

  EXPECT_STREQ(first_page, test_data);

  delete chunk;
}

// Test BufChunkTest.multiple page test.
TEST(BufChunkTest, MultiplePageTest) {
  size_t page_count = 3;
  auto* chunk = buf_chunk_t<csr_page_t>::buf_chunk_init(page_count, bw_graph::BW_GRAPH_PAGE_SIZE);

  for (size_t i = 0; i < page_count; ++i) {
    char* page_data = chunk->frames + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    snprintf(page_data, bw_graph::BW_GRAPH_PAGE_SIZE, "Page %zu data", i);
  }

  for (size_t i = 0; i < page_count; ++i) {
    char* page_data = chunk->frames + i * bw_graph::BW_GRAPH_PAGE_SIZE;
    char expected[32];
    snprintf(expected, sizeof(expected), "Page %zu data", i);
    EXPECT_STREQ(page_data, expected);
  }

  delete chunk;
}

// Test BufFreeTest.traversal free test.
TEST(BufFreeTest, TraversalFreeTest) {
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(2, 10, bw_graph::BW_GRAPH_PAGE_SIZE);
  int count = 0;
  buf_pool->traverse_free_list([&count](csr_page_t* page) {
    std::cout << "Free page at address: " << static_cast<void*>(page->get_data()) << std::endl;
    count++;
    return true;
  });
  EXPECT_EQ(count, 20); // 2 chunks * 10 pages each
}

namespace {

class zero_disk_manager_t : public disk_manager_t {
public:
  void read_page(page_no_t, char* page_data) override {
    std::memset(page_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
  }

  void write_page(page_no_t, const char*) override {}
};

class counting_disk_manager_t : public disk_manager_t {
public:
  void read_page(page_no_t page_no, char* page_data) override {
    (void) page_no;
    read_count.fetch_add(1, std::memory_order_relaxed);
    std::memset(page_data, 0, bw_graph::BW_GRAPH_PAGE_SIZE);
    if (read_delay.count() > 0) {
      std::this_thread::sleep_for(read_delay);
    }
  }

  void write_page(page_no_t, const char*) override {
    write_count.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<uint64_t> read_count{0};
  std::atomic<uint64_t> write_count{0};
  std::chrono::milliseconds read_delay{0};
};

class prefetch_worker_guard_t {
public:
  explicit prefetch_worker_guard_t(uint64_t worker_count)
      : old_worker_count(bw_graph::BW_BUFFER_PREFETCH_WORKER_COUNT) {
    bw_graph::BW_BUFFER_PREFETCH_WORKER_COUNT = worker_count;
  }

  ~prefetch_worker_guard_t() {
    bw_graph::BW_BUFFER_PREFETCH_WORKER_COUNT = old_worker_count;
  }

private:
  uint64_t old_worker_count;
};

} // namespace

TEST(BufPoolTest, ReadLatchPinsPageUntilReleased) {
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(1, 1, bw_graph::BW_GRAPH_PAGE_SIZE);
  zero_disk_manager_t disk_manager;

  csr_page_t* page = buf_pool->buf_page_read(0, &disk_manager);
  EXPECT_GT(page->get_pin_count(), 0);

  page->r_unlatch();
  EXPECT_EQ(page->get_pin_count(), 0);

  delete buf_pool;
}

TEST(BufPoolTest, ConcurrentColdMissesShareOneDiskRead) {
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(1, 8, bw_graph::BW_GRAPH_PAGE_SIZE);
  counting_disk_manager_t disk_manager;
  disk_manager.read_delay = std::chrono::milliseconds(20);

  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      csr_page_t* page = buf_pool->buf_page_read(7, &disk_manager);
      EXPECT_EQ(page->get_page_no(), 7);
      page->r_unlatch();
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  auto stats = buf_pool->get_stats();
  EXPECT_EQ(disk_manager.read_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(stats.cache_misses, 1);
  EXPECT_GT(stats.cache_hits, 0);
  EXPECT_GT(stats.duplicate_miss_waits, 0);

  delete buf_pool;
}

TEST(BufPoolTest, DirtyEvictionUpdatesStatsAndWritesOnce) {
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(1, 1, bw_graph::BW_GRAPH_PAGE_SIZE);
  counting_disk_manager_t disk_manager;

  csr_page_t* first = buf_pool->buf_page_read(1, &disk_manager);
  first->mark_dirty();
  first->r_unlatch();

  csr_page_t* second = buf_pool->buf_page_read(2, &disk_manager);
  second->r_unlatch();

  auto stats = buf_pool->get_stats();
  EXPECT_EQ(disk_manager.write_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(stats.evictions, 1);
  EXPECT_EQ(stats.dirty_writebacks, 1);

  delete buf_pool;
}

TEST(BufPoolTest, PrefetchWarmsPageForDemandRead) {
  prefetch_worker_guard_t guard(2);
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(1, 2, bw_graph::BW_GRAPH_PAGE_SIZE);
  counting_disk_manager_t disk_manager;

  buf_pool->buf_page_prefetch(3, &disk_manager);
  buf_pool->wait_for_prefetch_idle();
  EXPECT_EQ(disk_manager.read_count.load(std::memory_order_relaxed), 1);

  csr_page_t* page = buf_pool->buf_page_read(3, &disk_manager);
  EXPECT_EQ(page->get_page_no(), 3);
  page->r_unlatch();

  auto stats = buf_pool->get_stats();
  EXPECT_EQ(disk_manager.read_count.load(std::memory_order_relaxed), 1);
  EXPECT_GE(stats.prefetches, 1);
  EXPECT_EQ(stats.prefetch_enqueued, 1);
  EXPECT_EQ(stats.prefetch_completed, 1);
  EXPECT_GE(stats.cache_hits, 1);

  delete buf_pool;
}

TEST(BufPoolTest, DisabledPrefetchDoesNotRead) {
  prefetch_worker_guard_t guard(0);
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(1, 2, bw_graph::BW_GRAPH_PAGE_SIZE);
  counting_disk_manager_t disk_manager;

  buf_pool->buf_page_prefetch(4, &disk_manager);
  buf_pool->wait_for_prefetch_idle();

  auto stats = buf_pool->get_stats();
  EXPECT_EQ(disk_manager.read_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(stats.prefetch_enqueued, 0);
  EXPECT_EQ(stats.prefetch_completed, 0);
  EXPECT_EQ(stats.prefetch_dropped, 1);

  delete buf_pool;
}

TEST(BufPoolTest, AsyncPrefetchDeduplicatesQueuedPages) {
  prefetch_worker_guard_t guard(1);
  auto* buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(1, 2, bw_graph::BW_GRAPH_PAGE_SIZE);
  counting_disk_manager_t disk_manager;
  disk_manager.read_delay = std::chrono::milliseconds(20);

  for (int i = 0; i < 8; ++i) {
    buf_pool->buf_page_prefetch(4, &disk_manager);
  }
  buf_pool->wait_for_prefetch_idle();

  auto stats = buf_pool->get_stats();
  EXPECT_EQ(disk_manager.read_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(stats.prefetch_enqueued, 1);
  EXPECT_EQ(stats.prefetch_completed, 1);
  EXPECT_GT(stats.prefetch_dropped, 0);

  delete buf_pool;
}
