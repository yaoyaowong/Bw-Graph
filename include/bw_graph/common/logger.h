#ifndef BW_GRAPH_COMMON_LOGGER_H
#define BW_GRAPH_COMMON_LOGGER_H

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace bw_graph {

/**
 * @brief Debug logging functions for the database engine
 *
 * Unlike performance counters, these are just fprintf() turned on/off by
 * LOG_LEVEL compile option. The main concern here is not to add any overhead
 * on runtime performance when the logging is turned off. Use LOG_XXX_ENABLED
 * macros defined here to eliminate all instructions in the final binary.
 *
 * When logger::init() is called with a non-empty path, every enabled log call
 * also enqueues the formatted line to a background drain thread that writes it
 * to the log file asynchronously (no blocking on the calling thread).
 */

/* Type alias for const char pointer */
using cstr = const char*;

/**
 * @brief Helper function to extract filename from full path
 */
static constexpr auto past_last_slash(cstr a, cstr b) -> cstr {
  return *a == '\0' ? b : *a == '/' ? past_last_slash(a + 1, a + 1) : past_last_slash(a + 1, b);
}

static constexpr auto past_last_slash(cstr a) -> cstr { return past_last_slash(a, a); }

/* Macro to get short filename from __FILE__ */
#define BW_GRAPH_SHORT_FILE__                                                                      \
  ({                                                                                               \
    constexpr const char* sf__{bw_graph::past_last_slash(__FILE__)};                               \
    sf__;                                                                                          \
  })

/* Log level constants */
#define BW_GRAPH_LOG_LEVEL_OFF   1000
#define BW_GRAPH_LOG_LEVEL_ERROR  500
#define BW_GRAPH_LOG_LEVEL_WARN   400
#define BW_GRAPH_LOG_LEVEL_INFO   300
#define BW_GRAPH_LOG_LEVEL_DEBUG  200
#define BW_GRAPH_LOG_LEVEL_TRACE  100
#define BW_GRAPH_LOG_LEVEL_ALL      0

/* Log formatting constants */
#define BW_GRAPH_LOG_TIME_FORMAT "%Y-%m-%d %H:%M:%S"
#define BW_GRAPH_LOG_OUTPUT_STREAM stdout

/* Compile time log level configuration */
#ifndef BW_GRAPH_LOG_LEVEL
#ifndef NDEBUG
#define BW_GRAPH_LOG_LEVEL BW_GRAPH_LOG_LEVEL_DEBUG
#else
#define BW_GRAPH_LOG_LEVEL BW_GRAPH_LOG_LEVEL_INFO
#endif
#endif

/* For compilers which do not support __FUNCTION__ */
#if !defined(__FUNCTION__) && !defined(__GNUC__)
#define __FUNCTION__ ""
#endif

/**
 * @brief Format a log header into a caller-supplied buffer.
 *
 * Writes "time [file:line:func] LEVEL - " into buf.
 */
inline void format_log_header(char* buf, size_t buf_size,
                               const char* file, int line, const char* func, int level) {
  time_t t = ::time(nullptr);
  tm* cur_time = localtime(&t);
  char time_str[32];
  ::strftime(time_str, 32, BW_GRAPH_LOG_TIME_FORMAT, cur_time);

  const char* type;
  switch (level) {
  case BW_GRAPH_LOG_LEVEL_ERROR: type = "ERROR"; break;
  case BW_GRAPH_LOG_LEVEL_WARN:  type = "WARN "; break;
  case BW_GRAPH_LOG_LEVEL_INFO:  type = "INFO "; break;
  case BW_GRAPH_LOG_LEVEL_DEBUG: type = "DEBUG"; break;
  case BW_GRAPH_LOG_LEVEL_TRACE: type = "TRACE"; break;
  default:                       type = "UNKWN"; break;
  }
  ::snprintf(buf, buf_size, "%s [%s:%d:%s] %s - ", time_str, file, line, func, type);
}

/**
 * @brief Output log message header to stdout.
 */
inline void output_log_header(const char* file, int line, const char* func, int level) {
  char buf[256];
  format_log_header(buf, sizeof(buf), file, line, func, level);
  ::fputs(buf, BW_GRAPH_LOG_OUTPUT_STREAM);
}

/* --------------------------------------------------------------------------
 * Log macros — each level writes asynchronously to the log file only.
 * No stdout output. Output is silently dropped if logger::init() has not
 * been called (file sink not active).
 * -------------------------------------------------------------------------- */

/* Error logging macros */
#ifdef BW_GRAPH_LOG_ERROR_ENABLED
#undef BW_GRAPH_LOG_ERROR_ENABLED
#endif
#if BW_GRAPH_LOG_LEVEL <= BW_GRAPH_LOG_LEVEL_ERROR
#define BW_GRAPH_LOG_ERROR_ENABLED
#define BW_GRAPH_LOG_ERROR(...)                                                                    \
  if (bw_graph::logger::is_file_enabled()) {                                                       \
    char _bw_hdr_[128], _bw_msg_[512];                                                             \
    bw_graph::format_log_header(_bw_hdr_, sizeof(_bw_hdr_),                                       \
        BW_GRAPH_SHORT_FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_ERROR);                  \
    ::snprintf(_bw_msg_, sizeof(_bw_msg_), __VA_ARGS__);                                           \
    bw_graph::logger::enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");                           \
  }
#else
#define BW_GRAPH_LOG_ERROR(...) ((void) 0)
#endif

/* Warning logging macros */
#ifdef BW_GRAPH_LOG_WARN_ENABLED
#undef BW_GRAPH_LOG_WARN_ENABLED
#endif
#if BW_GRAPH_LOG_LEVEL <= BW_GRAPH_LOG_LEVEL_WARN
#define BW_GRAPH_LOG_WARN_ENABLED
#define BW_GRAPH_LOG_WARN(...)                                                                     \
  if (bw_graph::logger::is_file_enabled()) {                                                       \
    char _bw_hdr_[128], _bw_msg_[512];                                                             \
    bw_graph::format_log_header(_bw_hdr_, sizeof(_bw_hdr_),                                       \
        BW_GRAPH_SHORT_FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_WARN);                   \
    ::snprintf(_bw_msg_, sizeof(_bw_msg_), __VA_ARGS__);                                           \
    bw_graph::logger::enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");                           \
  }
#else
#define BW_GRAPH_LOG_WARN(...) ((void) 0)
#endif

/* Info logging macros */
#ifdef BW_GRAPH_LOG_INFO_ENABLED
#undef BW_GRAPH_LOG_INFO_ENABLED
#endif
#if BW_GRAPH_LOG_LEVEL <= BW_GRAPH_LOG_LEVEL_INFO
#define BW_GRAPH_LOG_INFO_ENABLED
#define BW_GRAPH_LOG_INFO(...)                                                                     \
  if (bw_graph::logger::is_file_enabled()) {                                                       \
    char _bw_hdr_[128], _bw_msg_[512];                                                             \
    bw_graph::format_log_header(_bw_hdr_, sizeof(_bw_hdr_),                                       \
        BW_GRAPH_SHORT_FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_INFO);                   \
    ::snprintf(_bw_msg_, sizeof(_bw_msg_), __VA_ARGS__);                                           \
    bw_graph::logger::enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");                           \
  }
#else
#define BW_GRAPH_LOG_INFO(...) ((void) 0)
#endif

/* Debug logging macros */
#ifdef BW_GRAPH_LOG_DEBUG_ENABLED
#undef BW_GRAPH_LOG_DEBUG_ENABLED
#endif
#if BW_GRAPH_LOG_LEVEL <= BW_GRAPH_LOG_LEVEL_DEBUG
#define BW_GRAPH_LOG_DEBUG_ENABLED
#define BW_GRAPH_LOG_DEBUG(...)                                                                    \
  if (bw_graph::logger::is_file_enabled()) {                                                       \
    char _bw_hdr_[128], _bw_msg_[512];                                                             \
    bw_graph::format_log_header(_bw_hdr_, sizeof(_bw_hdr_),                                       \
        BW_GRAPH_SHORT_FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_DEBUG);                  \
    ::snprintf(_bw_msg_, sizeof(_bw_msg_), __VA_ARGS__);                                           \
    bw_graph::logger::enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");                           \
  }
#else
#define BW_GRAPH_LOG_DEBUG(...) ((void) 0)
#endif

/* Trace logging macros */
#ifdef BW_GRAPH_LOG_TRACE_ENABLED
#undef BW_GRAPH_LOG_TRACE_ENABLED
#endif
#if BW_GRAPH_LOG_LEVEL <= BW_GRAPH_LOG_LEVEL_TRACE
#define BW_GRAPH_LOG_TRACE_ENABLED
#define BW_GRAPH_LOG_TRACE(...)                                                                    \
  if (bw_graph::logger::is_file_enabled()) {                                                       \
    char _bw_hdr_[128], _bw_msg_[512];                                                             \
    bw_graph::format_log_header(_bw_hdr_, sizeof(_bw_hdr_),                                       \
        BW_GRAPH_SHORT_FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_TRACE);                  \
    ::snprintf(_bw_msg_, sizeof(_bw_msg_), __VA_ARGS__);                                           \
    bw_graph::logger::enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");                           \
  }
#else
#define BW_GRAPH_LOG_TRACE(...) ((void) 0)
#endif

/**
 * @brief Logger class providing convenient logging interface and async file sink.
 *
 * Call logger::init(path) once at startup to enable file logging.
 * Call logger::shutdown() at teardown to flush and close the file.
 * All BW_GRAPH_LOG_* macros automatically route to the file when enabled.
 */
class logger {
public:
  /**
   * @brief Open the log file and start the drain thread.
   * Idempotent: a second call while already running is a no-op.
   * A call with an empty path is a no-op.
   */
  static void init(const std::string& log_file_path) {
    if (log_file_path.empty()) return;
    if (running_.load(std::memory_order_acquire)) return;
    file_.open(log_file_path, std::ios::app);
    if (!file_.is_open()) return;
    running_.store(true, std::memory_order_release);
    drain_thread_ = std::thread(&logger::drain_loop);
  }

  /**
   * @brief Signal the drain thread to stop, wait for it, flush and close file.
   */
  static void shutdown() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    cv_.notify_one();
    if (drain_thread_.joinable()) drain_thread_.join();
    file_.close();
  }

  /** @brief Returns true when the file sink is active. */
  static bool is_file_enabled() {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Enqueue a pre-formatted log line for async file write. */
  static void enqueue(std::string msg) {
    std::unique_lock<std::mutex> lk(mutex_);
    queue_.push(std::move(msg));
    lk.unlock();
    cv_.notify_one();
  }

  /* Convenience wrappers used by legacy call sites */
  static void log_error(const char* message) {
#ifdef BW_GRAPH_LOG_ERROR_ENABLED
    BW_GRAPH_LOG_ERROR("%s", message);
#endif
  }
  static void log_warn(const char* message) {
#ifdef BW_GRAPH_LOG_WARN_ENABLED
    BW_GRAPH_LOG_WARN("%s", message);
#endif
  }
  static void log_info(const char* message) {
#ifdef BW_GRAPH_LOG_INFO_ENABLED
    BW_GRAPH_LOG_INFO("%s", message);
#endif
  }
  static void log_debug(const char* message) {
#ifdef BW_GRAPH_LOG_DEBUG_ENABLED
    BW_GRAPH_LOG_DEBUG("%s", message);
#endif
  }
  static void log_trace(const char* message) {
#ifdef BW_GRAPH_LOG_TRACE_ENABLED
    BW_GRAPH_LOG_TRACE("%s", message);
#else
    (void) message;
#endif
  }

  static void log_error_format(const char* format, ...) {
#ifdef BW_GRAPH_LOG_ERROR_ENABLED
    if (!is_file_enabled()) return;
    char _bw_hdr_[128], _bw_msg_[512];
    format_log_header(_bw_hdr_, sizeof(_bw_hdr_), __FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_ERROR);
    va_list args; va_start(args, format);
    ::vsnprintf(_bw_msg_, sizeof(_bw_msg_), format, args);
    va_end(args);
    enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");
#else
    (void) format;
#endif
  }
  static void log_warn_format(const char* format, ...) {
#ifdef BW_GRAPH_LOG_WARN_ENABLED
    if (!is_file_enabled()) return;
    char _bw_hdr_[128], _bw_msg_[512];
    format_log_header(_bw_hdr_, sizeof(_bw_hdr_), __FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_WARN);
    va_list args; va_start(args, format);
    ::vsnprintf(_bw_msg_, sizeof(_bw_msg_), format, args);
    va_end(args);
    enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");
#else
    (void) format;
#endif
  }
  static void log_info_format(const char* format, ...) {
#ifdef BW_GRAPH_LOG_INFO_ENABLED
    if (!is_file_enabled()) return;
    char _bw_hdr_[128], _bw_msg_[512];
    format_log_header(_bw_hdr_, sizeof(_bw_hdr_), __FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_INFO);
    va_list args; va_start(args, format);
    ::vsnprintf(_bw_msg_, sizeof(_bw_msg_), format, args);
    va_end(args);
    enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");
#else
    (void) format;
#endif
  }
  static void log_debug_format(const char* format, ...) {
#ifdef BW_GRAPH_LOG_DEBUG_ENABLED
    if (!is_file_enabled()) return;
    char _bw_hdr_[128], _bw_msg_[512];
    format_log_header(_bw_hdr_, sizeof(_bw_hdr_), __FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_DEBUG);
    va_list args; va_start(args, format);
    ::vsnprintf(_bw_msg_, sizeof(_bw_msg_), format, args);
    va_end(args);
    enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");
#else
    (void) format;
#endif
  }
  static void log_trace_format(const char* format, ...) {
#ifdef BW_GRAPH_LOG_TRACE_ENABLED
    if (!is_file_enabled()) return;
    char _bw_hdr_[128], _bw_msg_[512];
    format_log_header(_bw_hdr_, sizeof(_bw_hdr_), __FILE__, __LINE__, __FUNCTION__, BW_GRAPH_LOG_LEVEL_TRACE);
    va_list args; va_start(args, format);
    ::vsnprintf(_bw_msg_, sizeof(_bw_msg_), format, args);
    va_end(args);
    enqueue(std::string(_bw_hdr_) + _bw_msg_ + "\n");
#else
    (void) format;
#endif
  }

private:
  static void drain_loop() {
    while (running_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait(lk, [] {
        return !queue_.empty() || !running_.load(std::memory_order_acquire);
      });
      while (!queue_.empty()) {
        file_ << queue_.front();
        queue_.pop();
      }
      file_.flush();
    }
    // Final drain after running_ set to false.
    std::unique_lock<std::mutex> lk(mutex_);
    while (!queue_.empty()) {
      file_ << queue_.front();
      queue_.pop();
    }
    file_.flush();
  }

  inline static std::ofstream           file_;
  inline static std::queue<std::string> queue_;
  inline static std::mutex              mutex_;
  inline static std::condition_variable cv_;
  inline static std::thread             drain_thread_;
  inline static std::atomic<bool>       running_{false};
};

} // namespace bw_graph

#endif // BW_GRAPH_COMMON_LOGGER_H
