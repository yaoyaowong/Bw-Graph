#include "bw_graph/common/logger.h"

#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

TEST(LoggerAsync, WritesToFileAndShutdownFlushes) {
  const std::string path = "/tmp/bw_test_logger.log";
  ::remove(path.c_str());

  bw_graph::logger::init(path);
  ASSERT_TRUE(bw_graph::logger::is_file_enabled());

  BW_GRAPH_LOG_INFO("hello async logger");
  BW_GRAPH_LOG_INFO("second line");

  bw_graph::logger::shutdown();
  ASSERT_FALSE(bw_graph::logger::is_file_enabled());

  std::ifstream f(path);
  ASSERT_TRUE(f.is_open());
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("hello async logger"), std::string::npos);
  EXPECT_NE(content.find("second line"), std::string::npos);
  EXPECT_NE(content.find("INFO"), std::string::npos);
  ::remove(path.c_str());
}

TEST(LoggerAsync, InitWithEmptyPathIsNoop) {
  bw_graph::logger::init("");
  EXPECT_FALSE(bw_graph::logger::is_file_enabled());
}

TEST(LoggerAsync, DoubleInitIsIdempotent) {
  const std::string path = "/tmp/bw_test_logger2.log";
  ::remove(path.c_str());
  bw_graph::logger::init(path);
  bw_graph::logger::init(path); // second call must not crash or double-start
  ASSERT_TRUE(bw_graph::logger::is_file_enabled());
  bw_graph::logger::shutdown();
  ::remove(path.c_str());
}
