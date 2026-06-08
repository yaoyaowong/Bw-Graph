#include <folly/concurrency/ConcurrentHashMap.h>
#include <gtest/gtest.h>

// Test BasicTest.basic arithmetic.
TEST(BasicTest, BasicArithmetic) { EXPECT_EQ(1 + 1, 2); }

// Test FollyTest.concurrent hash map test.
TEST(FollyTest, ConcurrentHashMapTest) {
  folly::ConcurrentHashMap<int, int> map;
  map.insert(1, 1);
  map.insert(2, 2);
  EXPECT_EQ(map.size(), 2);
}