#include "bw_graph/common/latch.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

class LatchTest : public ::testing::Test {
protected:
  void SetUp() override { latch_ = std::make_unique<rw_latch_t>(); }

  void TearDown() override { latch_.reset(); }

  std::unique_ptr<rw_latch_t> latch_;
};

// Basic functionality cases.
TEST_F(LatchTest, BasicConstruction) {
  EXPECT_EQ(latch_->get_reader_count(), 0);
  EXPECT_FALSE(latch_->is_write_locked());
  EXPECT_EQ(rw_latch_t::size(), 4);
}

// Test LatchTest.single reader lock.
TEST_F(LatchTest, SingleReaderLock) {
  EXPECT_TRUE(latch_->try_r_lock());
  EXPECT_EQ(latch_->get_reader_count(), 1);
  EXPECT_FALSE(latch_->is_write_locked());

  latch_->r_unlock();
  EXPECT_EQ(latch_->get_reader_count(), 0);
}

// Test LatchTest.single writer lock.
TEST_F(LatchTest, SingleWriterLock) {
  EXPECT_TRUE(latch_->try_w_lock());
  EXPECT_TRUE(latch_->is_write_locked());
  EXPECT_EQ(latch_->get_reader_count(), 0);

  latch_->w_unlock();
  EXPECT_FALSE(latch_->is_write_locked());
}

// Test LatchTest.multiple readers.
TEST_F(LatchTest, MultipleReaders) {
  const int num_readers = 100;

  for (int i = 0; i < num_readers; ++i) {
    EXPECT_TRUE(latch_->try_r_lock());
  }

  EXPECT_EQ(latch_->get_reader_count(), num_readers);
  EXPECT_FALSE(latch_->is_write_locked());

  for (int i = 0; i < num_readers; ++i) {
    latch_->r_unlock();
  }

  EXPECT_EQ(latch_->get_reader_count(), 0);
}

// Test LatchTest.writer excludes readers.
TEST_F(LatchTest, WriterExcludesReaders) {
  EXPECT_TRUE(latch_->try_w_lock());
  EXPECT_FALSE(latch_->try_r_lock());

  latch_->w_unlock();
  EXPECT_TRUE(latch_->try_r_lock());
  latch_->r_unlock();
}

// Test LatchTest.readers exclude writer.
TEST_F(LatchTest, ReadersExcludeWriter) {
  EXPECT_TRUE(latch_->try_r_lock());
  EXPECT_FALSE(latch_->try_w_lock());

  latch_->r_unlock();
  EXPECT_TRUE(latch_->try_w_lock());
  latch_->w_unlock();
}

// RAII Tests
TEST_F(LatchTest, ScopedReadLock) {
  {
    scoped_r_lock_t guard(*latch_);
    EXPECT_EQ(latch_->get_reader_count(), 1);
    EXPECT_FALSE(latch_->is_write_locked());
  }
  EXPECT_EQ(latch_->get_reader_count(), 0);
}

// Test LatchTest.scoped write lock.
TEST_F(LatchTest, ScopedWriteLock) {
  {
    scoped_w_lock_t guard(*latch_);
    EXPECT_TRUE(latch_->is_write_locked());
    EXPECT_FALSE(latch_->try_r_lock());
  }
  EXPECT_FALSE(latch_->is_write_locked());
  EXPECT_TRUE(latch_->try_r_lock());
  latch_->r_unlock();
}

// Test LatchTest.nested scoped read locks.
TEST_F(LatchTest, NestedScopedReadLocks) {
  {
    scoped_r_lock_t guard1(*latch_);
    EXPECT_EQ(latch_->get_reader_count(), 1);

    {
      scoped_r_lock_t guard2(*latch_);
      EXPECT_EQ(latch_->get_reader_count(), 2);
    }

    EXPECT_EQ(latch_->get_reader_count(), 1);
  }
  EXPECT_EQ(latch_->get_reader_count(), 0);
}

// Test LatchTest.exception safety.
TEST_F(LatchTest, ExceptionSafety) {
  try {
    scoped_r_lock_t guard(*latch_);
    EXPECT_EQ(latch_->get_reader_count(), 1);
    throw std::runtime_error("Test exception");
  } catch (const std::exception&) {
    // Lock should be released even after exception
    EXPECT_EQ(latch_->get_reader_count(), 0);
  }
}

// Concurrent Access Tests
TEST_F(LatchTest, ConcurrentReaders) {
  const int num_threads = 10;
  const int iterations_per_thread = 1000;
  std::atomic<int> successful_reads{0};
  std::atomic<int> total_attempts{0};

  std::vector<std::thread> threads;

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < iterations_per_thread; ++j) {
        total_attempts.fetch_add(1);

        scoped_r_lock_t guard(*latch_);
        successful_reads.fetch_add(1);

        // Simulate some work
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(successful_reads.load(), num_threads * iterations_per_thread);
  EXPECT_EQ(total_attempts.load(), num_threads * iterations_per_thread);
  EXPECT_EQ(latch_->get_reader_count(), 0);
}

// Test LatchTest.readers and writers.
TEST_F(LatchTest, ReadersAndWriters) {
  const int num_readers = 5;
  const int num_writers = 2;
  const int iterations = 100;

  std::atomic<int> reads_completed{0};
  std::atomic<int> writes_completed{0};
  std::atomic<int> shared_data{0};
  std::vector<std::thread> threads;

  // Reader threads
  for (int i = 0; i < num_readers; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < iterations; ++j) {
        scoped_r_lock_t guard(*latch_);

        // Read shared data
        shared_data.load();
        reads_completed.fetch_add(1);

        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    });
  }

  // Writer threads
  for (int i = 0; i < num_writers; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < iterations; ++j) {
        scoped_w_lock_t guard(*latch_);

        // Modify shared data
        int old_value = shared_data.load();
        shared_data.store(old_value + 1);
        writes_completed.fetch_add(1);

        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(reads_completed.load(), num_readers * iterations);
  EXPECT_EQ(writes_completed.load(), num_writers * iterations);
  EXPECT_EQ(shared_data.load(), num_writers * iterations);
  EXPECT_EQ(latch_->get_reader_count(), 0);
  EXPECT_FALSE(latch_->is_write_locked());
}

// Test LatchTest.writer starvation prevention.
TEST_F(LatchTest, WriterStarvationPrevention) {
  std::atomic<bool> writer_acquired{false};
  std::atomic<bool> stop_readers{false};
  std::atomic<int> reader_cycles{0};

  // Start multiple reader threads that will keep acquiring read locks
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&]() {
      while (!stop_readers.load()) {
        if (latch_->try_r_lock()) {
          reader_cycles.fetch_add(1);
          // Hold the lock for a short time
          std::this_thread::sleep_for(std::chrono::microseconds(50));
          latch_->r_unlock();
        }
        // Small delay to allow writer a chance
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    });
  }

  // Give readers time to start acquiring locks
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Start a writer thread
  std::thread writer([&]() {
    auto start_time = std::chrono::steady_clock::now();

    // Try to acquire write lock (should eventually succeed)
    scoped_w_lock_t guard(*latch_);
    writer_acquired.store(true);

    auto end_time = std::chrono::steady_clock::now();
    auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Writer should acquire the lock within reasonable time (less than 1
    // second)
    EXPECT_LT(wait_time.count(), 1000);

    stop_readers.store(true);
  });

  // Set a timeout for the entire test
  auto test_start = std::chrono::steady_clock::now();

  writer.join();
  for (auto& r : readers) {
    r.join();
  }

  auto test_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - test_start);

  EXPECT_TRUE(writer_acquired.load());
  EXPECT_GT(reader_cycles.load(), 0); // Readers should have done some work
  EXPECT_LT(test_duration.count(),
            2000); // Entire test should finish within 2 seconds

  std::cout << "Writer starvation test completed in " << test_duration.count() << "ms with "
            << reader_cycles.load() << " reader cycles" << std::endl;
}

// Performance cases.
TEST_F(LatchTest, PerformanceBenchmark) {
  const int num_operations = 100000;

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < num_operations; ++i) {
    scoped_r_lock_t guard(*latch_);
    // Minimal work to test lock overhead
    volatile int dummy = i;
    dummy += 1; // Prevent optimization
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  double ops_per_second = (double) num_operations / duration.count() * 1000000;

  // Expect at least 1M operations per second (very conservative)
  EXPECT_GT(ops_per_second, 1000000.0);

  std::cout << "Lock performance: " << ops_per_second << " operations per second" << std::endl;
}

// Test LatchTest.try lock performance.
TEST_F(LatchTest, TryLockPerformance) {
  const int num_operations = 1000000;
  int successful_locks = 0;

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < num_operations; ++i) {
    if (latch_->try_r_lock()) {
      successful_locks++;
      latch_->r_unlock();
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  EXPECT_EQ(successful_locks, num_operations);

  double ops_per_second = (double) num_operations / duration.count() * 1000000;

  std::cout << "Try-lock performance: " << ops_per_second << " operations per second" << std::endl;
}

// Edge and stress cases.
TEST_F(LatchTest, MaxReaderCount) {
  // Test approaching the maximum reader count (2^31 - 1)
  // We'll test a smaller number for practical reasons
  const uint32_t test_readers = 10000;

  for (uint32_t i = 0; i < test_readers; ++i) {
    EXPECT_TRUE(latch_->try_r_lock());
  }

  EXPECT_EQ(latch_->get_reader_count(), test_readers);

  // Should still be able to add more readers
  EXPECT_TRUE(latch_->try_r_lock());
  EXPECT_EQ(latch_->get_reader_count(), test_readers + 1);

  // Clean up
  for (uint32_t i = 0; i <= test_readers; ++i) {
    latch_->r_unlock();
  }

  EXPECT_EQ(latch_->get_reader_count(), 0);
}

// Test LatchTest.rapid lock unlock.
TEST_F(LatchTest, RapidLockUnlock) {
  const int iterations = 10000;

  // Rapid read lock/unlock
  for (int i = 0; i < iterations; ++i) {
    latch_->r_lock();
    latch_->r_unlock();
  }

  // Rapid write lock/unlock
  for (int i = 0; i < iterations; ++i) {
    latch_->w_lock();
    latch_->w_unlock();
  }

  EXPECT_EQ(latch_->get_reader_count(), 0);
  EXPECT_FALSE(latch_->is_write_locked());
}

// Test LatchTest.memory alignment.
TEST_F(LatchTest, MemoryAlignment) {
  // Test that the latch is properly aligned
  EXPECT_EQ(sizeof(rw_latch_t), 4);
  EXPECT_GE(alignof(rw_latch_t), 4);

  // Test multiple instances
  std::vector<rw_latch_t> latches(100);

  for (auto& latch : latches) {
    EXPECT_EQ(latch.get_reader_count(), 0);
    EXPECT_FALSE(latch.is_write_locked());
  }
}