#include "bw_graph/storage/disk_manager.h"

#include "bw_graph/common/logger.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

#if defined(__linux__) && defined(BW_GRAPH_ENABLE_IO_URING)
#include <liburing.h>

namespace {

constexpr unsigned int kBwGraphIoUringQueueDepth = 64;
constexpr uint64_t kBwGraphIoUringPathLogLimit = 8;

std::atomic<uint64_t> bw_graph_io_uring_read_path_logs{0};
std::atomic<uint64_t> bw_graph_io_uring_write_path_logs{0};
std::atomic<uint64_t> bw_graph_io_uring_fallback_logs{0};

void log_limited_uring_path(std::atomic<uint64_t>& counter, const char* op, off_t offset,
                            size_t page_size, ssize_t result) {
  uint64_t log_index = counter.fetch_add(1, std::memory_order_relaxed);
  if (log_index >= kBwGraphIoUringPathLogLimit) {
    return;
  }

  char message[256];
  ::snprintf(message, sizeof(message),
             "DiskManager io_uring %s path used: offset=%lld size=%zu result=%zd", op,
             static_cast<long long>(offset), page_size, result);
  bw_graph::logger::log_info(message);
  std::cout << message << std::endl;

  if (log_index + 1 == kBwGraphIoUringPathLogLimit) {
    char suppress_message[160];
    ::snprintf(suppress_message, sizeof(suppress_message),
               "DiskManager io_uring %s path log limit reached; suppressing further logs", op);
    bw_graph::logger::log_info(suppress_message);
    std::cout << suppress_message << std::endl;
  }
}

void log_limited_uring_fallback(const char* op, off_t offset, size_t page_size) {
  uint64_t log_index =
      bw_graph_io_uring_fallback_logs.fetch_add(1, std::memory_order_relaxed);
  if (log_index >= kBwGraphIoUringPathLogLimit) {
    return;
  }

  char message[256];
  ::snprintf(message, sizeof(message),
             "DiskManager io_uring %s submit unavailable; falling back to pread/pwrite: "
             "offset=%lld size=%zu",
             op, static_cast<long long>(offset), page_size);
  bw_graph::logger::log_info(message);
  std::cout << message << std::endl;

  if (log_index + 1 == kBwGraphIoUringPathLogLimit) {
    bw_graph::logger::log_info(
        "DiskManager io_uring fallback log limit reached; suppressing further logs");
    std::cout << "DiskManager io_uring fallback log limit reached; suppressing further logs"
              << std::endl;
  }
}

struct thread_io_uring_t {
  io_uring ring{};
  bool ready{false};

  thread_io_uring_t() {
    ready = (::io_uring_queue_init(kBwGraphIoUringQueueDepth, &ring, 0) == 0);
  }

  ~thread_io_uring_t() {
    if (ready) {
      ::io_uring_queue_exit(&ring);
    }
  }
};

thread_local thread_io_uring_t bw_graph_thread_io_uring;

bool submit_uring_read(int fd, void* page_data, size_t page_size, off_t offset,
                       ssize_t* bytes_read) {
  if (!bw_graph_thread_io_uring.ready) {
    return false;
  }

  io_uring_sqe* sqe = ::io_uring_get_sqe(&bw_graph_thread_io_uring.ring);
  if (sqe == nullptr) {
    return false;
  }
  ::io_uring_prep_read(sqe, fd, page_data, static_cast<unsigned int>(page_size), offset);

  if (::io_uring_submit(&bw_graph_thread_io_uring.ring) < 0) {
    return false;
  }

  io_uring_cqe* cqe = nullptr;
  int wait_result = ::io_uring_wait_cqe(&bw_graph_thread_io_uring.ring, &cqe);
  if (wait_result < 0 || cqe == nullptr) {
    return false;
  }

  if (cqe->res < 0) {
    errno = -cqe->res;
    *bytes_read = -1;
  } else {
    *bytes_read = cqe->res;
  }
  ::io_uring_cqe_seen(&bw_graph_thread_io_uring.ring, cqe);
  return true;
}

bool submit_uring_write(int fd, const void* page_data, size_t page_size, off_t offset,
                        ssize_t* bytes_written) {
  if (!bw_graph_thread_io_uring.ready) {
    return false;
  }

  io_uring_sqe* sqe = ::io_uring_get_sqe(&bw_graph_thread_io_uring.ring);
  if (sqe == nullptr) {
    return false;
  }
  ::io_uring_prep_write(sqe, fd, page_data, static_cast<unsigned int>(page_size), offset);

  if (::io_uring_submit(&bw_graph_thread_io_uring.ring) < 0) {
    return false;
  }

  io_uring_cqe* cqe = nullptr;
  int wait_result = ::io_uring_wait_cqe(&bw_graph_thread_io_uring.ring, &cqe);
  if (wait_result < 0 || cqe == nullptr) {
    return false;
  }

  if (cqe->res < 0) {
    errno = -cqe->res;
    *bytes_written = -1;
  } else {
    *bytes_written = cqe->res;
  }
  ::io_uring_cqe_seen(&bw_graph_thread_io_uring.ring, cqe);
  return true;
}

} // namespace
#endif

// Construct disk_manager_t.
disk_manager_t::disk_manager_t(const std::filesystem::path& db_file, size_t page_size)
    : page_size_(page_size), db_file_name_(db_file) {
  // Create log file name by appending .log to database file name
  log_file_name_ = db_file_name_.string() + ".log";

  // Ensure the workspace directory exists before creating files
  std::filesystem::path db_dir = db_file_name_.parent_path();
  if (!db_dir.empty()) {
    // Create directory if it doesn't exist (cross-platform)
    std::error_code ec;
    if (!std::filesystem::exists(db_dir, ec)) {
      if (!std::filesystem::create_directories(db_dir, ec)) {
        std::string error_msg = "Failed to create workspace directory: " + db_dir.string();
        bw_graph::logger::log_error(error_msg.c_str());
        throw std::runtime_error("Cannot create workspace directory");
      }

      // Set appropriate permissions for the created directory
#ifdef __APPLE__
      // macOS: Set directory permissions to 755 (rwxr-xr-x)
      if (::chmod(db_dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
        bw_graph::logger::log_error("Failed to set directory permissions on macOS");
      }
#elif defined(__linux__)
      // Linux: Set directory permissions to 755 (rwxr-xr-x)
      if (::chmod(db_dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
        bw_graph::logger::log_error("Failed to set directory permissions on Linux");
      }
#endif

      std::string success_msg = "Created workspace directory: " + db_dir.string();
      bw_graph::logger::log_info(success_msg.c_str());
    }
  }

  // SSD-optimized mode: Use cached file descriptors for better performance

  // Open database file with read/write access, create if doesn't exist
  db_fd_ = ::open(db_file_name_.c_str(), O_RDWR | O_CREAT, 0644);
  if (db_fd_ == -1) {
    bw_graph::logger::log_error("Failed to open database file");
    throw std::runtime_error("Cannot open database file");
  }

  // Open log file with read/write access and append mode, create if doesn't
  // exist
  log_fd_ = ::open(log_file_name_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (log_fd_ == -1) {
    bw_graph::logger::log_error("Failed to open log file");
    ::close(db_fd_); // Clean up database fd on failure
    db_fd_ = -1;
    throw std::runtime_error("Cannot open log file");
  }

  // Get initial database file size for capacity tracking
  struct stat stat_buf;
  if (::fstat(db_fd_, &stat_buf) == 0) {
    page_capacity_ = stat_buf.st_size;
    // Ensure minimum capacity even for empty files
    if (page_capacity_ == 0) {
      page_capacity_ = bw_graph::BW_GRAPH_DEFAULT_DB_IO_SIZE;
    }
  } else {
    page_capacity_ = bw_graph::BW_GRAPH_DEFAULT_DB_IO_SIZE;
  }

#ifdef __APPLE__
  // macOS: Enable optimizations for SSD
  ::fcntl(db_fd_, F_NOCACHE, 1);  // Disable system cache for direct SSD access
  ::fcntl(log_fd_, F_NOCACHE, 1); // Also optimize log file access
#elif defined(__linux__)
  // Linux: Advise kernel about access patterns for SSD optimization
  ::posix_fadvise(db_fd_, 0, 0,
                  POSIX_FADV_RANDOM); // Database file: random access
  ::posix_fadvise(log_fd_, 0, 0,
                  POSIX_FADV_SEQUENTIAL); // Log file: sequential access
#endif

#if defined(__linux__) && defined(BW_GRAPH_ENABLE_IO_URING)
  io_uring probe_ring{};
  int uring_init_result = ::io_uring_queue_init(kBwGraphIoUringQueueDepth, &probe_ring, 0);
  if (uring_init_result == 0) {
    io_uring_supported_ = true;
    ::io_uring_queue_exit(&probe_ring);
    bw_graph::logger::log_info("DiskManager io_uring backend enabled");
  } else {
    io_uring_supported_ = false;
    bw_graph::logger::log_error("DiskManager io_uring init failed; falling back to pread/pwrite");
  }
#endif

  // Compute initial valid page count via backward scan.
  // Mirrors the logic in bin/storage.cc: scan from the last page backward
  // until we find the first non-zero page; that gives us the page count.
  {
    uint64_t total_pages = page_capacity_ / page_size_;
    if (total_pages > 0) {
      std::vector<char> scan_buf(page_size_);
      for (uint64_t i = total_pages; i > 0; --i) {
        off_t scan_off = static_cast<off_t>((i - 1) * page_size_);
        ssize_t n = ::pread(db_fd_, scan_buf.data(), page_size_, scan_off);
        if (n != static_cast<ssize_t>(page_size_))
          break;
        bool all_zero = true;
        for (size_t j = 0; j < page_size_ && all_zero; ++j)
          if (scan_buf[j] != 0)
            all_zero = false;
        if (!all_zero) {
          valid_page_count_.store(i, std::memory_order_relaxed);
          break;
        }
      }
    }
    global_page_count_.store(valid_page_count_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
  }

  bw_graph::logger::log_info("DiskManager initialized successfully");
}

// Handle shutdown.
void disk_manager_t::shutdown() {
  bw_graph::logger::log_info("DiskManager shutdown initiated");

  // Wait for any pending flush operations to complete
  if (flush_log_f_ != nullptr && has_flush_log_future()) {
    try {
      flush_log_f_->wait(); // Block until flush completes
    } catch (const std::exception& e) {
      bw_graph::logger::log_error("Error waiting for flush operation during shutdown");
    }
    flush_log_f_ = nullptr;
  }

  // Ensure all data is written to disk before closing
  if (db_fd_ != -1) {
    try {
#ifdef __APPLE__
      // macOS: Force full sync before closing
      if (::fcntl(db_fd_, F_FULLFSYNC) != 0) {
        bw_graph::logger::log_error("Failed to sync database file during shutdown");
      }
#else // Linux/Ubuntu
      // Linux: Sync data and metadata before closing
      if (::fsync(db_fd_) != 0) {
        bw_graph::logger::log_error("Failed to sync database file during shutdown");
      }
#endif
    } catch (...) {
      bw_graph::logger::log_error("Exception during database file sync");
    }

    if (::close(db_fd_) != 0) {
      bw_graph::logger::log_error("Failed to close database file descriptor");
    }
    db_fd_ = -1;
  }

  // Close log file descriptor
  if (log_fd_ != -1) {
    try {
#ifdef __APPLE__
      if (::fcntl(log_fd_, F_FULLFSYNC) != 0) {
        bw_graph::logger::log_error("Failed to sync log file during shutdown");
      }
#else // Linux/Ubuntu
      if (::fsync(log_fd_) != 0) {
        bw_graph::logger::log_error("Failed to sync log file during shutdown");
      }
#endif
    } catch (...) {
      bw_graph::logger::log_error("Exception during log file sync");
    }

    if (::close(log_fd_) != 0) {
      bw_graph::logger::log_error("Failed to close log file descriptor");
    }
    log_fd_ = -1;
  }

  // Clear any remaining state
  {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    free_slots_.clear();
    flush_log_ = false;
  }

  // Reset statistics
  num_flushes_ = 0;
  num_writes_ = 0;
  num_deletes_ = 0;

  bw_graph::logger::log_info("DiskManager shutdown completed successfully");
}

// Delete page.
void disk_manager_t::delete_page(page_no_t page_no) {
  // Compute the offset of the page through the page no.
  size_t offset = static_cast<size_t>(page_no) * page_size_;

  // Lazy deletion: only mark the page as free for reuse
  {
    std::lock_guard<std::mutex> lock(db_io_latch_);

    // Check if the page is within current file capacity
    size_t required_size = offset + page_size_;
    if (required_size > page_capacity_) {
      bw_graph::logger::log_error("Attempted to delete page beyond file capacity");
      return;
    }

    // Add offset to free slots for reuse by allocate_page()
    free_slots_.push_back(offset);

    // Update statistics
    num_deletes_++;
  }

  // No disk I/O needed - page will be overwritten when reused
}

// Write page.
void disk_manager_t::write_page(page_no_t page_no, const char* page_data) {
  // Compute the offset of the page through the page no.
  size_t offset = static_cast<size_t>(page_no) * page_size_;

  // Check if we need to extend the file size (protect shared state)
  {
    std::lock_guard<std::mutex> capacity_lock(db_io_latch_);
    size_t required_size = offset + page_size_;
    if (required_size > page_capacity_) {
      // Extend file capacity with SSD-friendly preallocation strategy
      page_capacity_ = std::max(required_size, page_capacity_ * 2);

#ifdef __linux__
      // Ubuntu system: Use fallocate for efficient space preallocation
      if (::fallocate(db_fd_, 0, 0, page_capacity_) != 0) {
        bw_graph::logger::log_error("Failed to preallocate disk space");
      }
#endif
    }
  }

  // SSD-optimized write using cached file descriptor.
  ssize_t bytes_written = -1;
#if defined(__linux__) && defined(BW_GRAPH_ENABLE_IO_URING)
  if (io_uring_supported_) {
    if (submit_uring_write(db_fd_, page_data, page_size_, static_cast<off_t>(offset),
                           &bytes_written)) {
      log_limited_uring_path(bw_graph_io_uring_write_path_logs, "write",
                             static_cast<off_t>(offset), page_size_, bytes_written);
    } else {
      log_limited_uring_fallback("write", static_cast<off_t>(offset), page_size_);
      bytes_written = ::pwrite(db_fd_, page_data, page_size_, offset);
    }
  } else {
    bytes_written = ::pwrite(db_fd_, page_data, page_size_, offset);
  }
#else
  bytes_written = ::pwrite(db_fd_, page_data, page_size_, offset);
#endif

  if (bytes_written != static_cast<ssize_t>(page_size_)) {
    bw_graph::logger::log_error("I/O error while writing page");
    return;
  }

  // Atomically update statistics and handle flush
  {
    std::lock_guard<std::mutex> stats_lock(db_io_latch_);
    num_writes_++;

    // SSD optimized disk flush strategy
    if (flush_log_) {
#ifdef __APPLE__
      // macOS: Use F_FULLFSYNC to ensure data reaches SSD
      if (::fcntl(db_fd_, F_FULLFSYNC) != 0) {
        bw_graph::logger::log_error("Failed to sync database file");
      }
#else // Linux/Ubuntu
      // Ubuntu: Use fdatasync for better SSD performance (data only, no
      // metadata)
      if (::fdatasync(db_fd_) != 0) {
        bw_graph::logger::log_error("Failed to sync database file");
      }
#endif
      num_flushes_++;
    }
  }

  uint64_t next_page_count = static_cast<uint64_t>(page_no) + 1;
  uint64_t current_page_count = global_page_count_.load(std::memory_order_relaxed);
  while (current_page_count < next_page_count &&
         !global_page_count_.compare_exchange_weak(current_page_count, next_page_count,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
  }

  // Keep valid_page_count_ up to date (page_no is 0-indexed, count = no + 1)
  uint64_t new_count = static_cast<uint64_t>(page_no) + 1;
  uint64_t curr_vc = valid_page_count_.load(std::memory_order_relaxed);
  while (new_count > curr_vc &&
         !valid_page_count_.compare_exchange_weak(curr_vc, new_count, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
  }
}

// Read page.
void disk_manager_t::read_page(page_no_t page_no, char* page_data) {
  // Compute the offset of the page through the page no.
  size_t offset = static_cast<size_t>(page_no) * page_size_;

  // Check if the page is within current file capacity (protect shared state)
  size_t required_size = offset + page_size_;
  if (required_size > page_capacity_) {
    // Page is beyond current file size, return zeros
    std::memset(page_data, 0, page_size_);
    bw_graph::logger::log_error("Page read beyond current file size, returning zeros");
    std::cout << "Page " << page_no << " read beyond current file size, returning zeros."
              << std::endl;
    return;
  }

  // SSD-optimized read using cached file descriptor.
  ssize_t bytes_read = -1;
#if defined(__linux__) && defined(BW_GRAPH_ENABLE_IO_URING)
  if (io_uring_supported_) {
    if (submit_uring_read(db_fd_, page_data, page_size_, static_cast<off_t>(offset),
                          &bytes_read)) {
      log_limited_uring_path(bw_graph_io_uring_read_path_logs, "read",
                             static_cast<off_t>(offset), page_size_, bytes_read);
    } else {
      log_limited_uring_fallback("read", static_cast<off_t>(offset), page_size_);
      bytes_read = ::pread(db_fd_, page_data, page_size_, offset);
    }
  } else {
    bytes_read = ::pread(db_fd_, page_data, page_size_, offset);
  }
#else
  bytes_read = ::pread(db_fd_, page_data, page_size_, offset);
#endif

  if (bytes_read != static_cast<ssize_t>(page_size_)) {
    if (bytes_read == -1) {
      // System call failed
      bw_graph::logger::log_error("System call failed while reading page");
      std::memset(page_data, 0, page_size_);
    } else if (bytes_read == 0) {
      bw_graph::logger::log_error("End of file reached while reading page");
      // End of file reached, page doesn't exist
      std::memset(page_data, 0, page_size_);
    } else if (bytes_read > 0) {
      bw_graph::logger::log_error("Partial read occurred while reading page");
      // Partial read occurred, fill remaining bytes with zeros
      std::memset(page_data + bytes_read, 0, page_size_ - bytes_read);
    }
  }
  // If bytes_read == page_size_, read was successful, no action needed
}

bool disk_manager_t::is_io_uring_enabled() const { return io_uring_supported_; }

// Write log.
void disk_manager_t::write_log(char* log_data, int size) {
  // SSD-optimized log write using cached file descriptor with append mode
  // Using write() on a file opened with O_APPEND automatically appends to end
  ssize_t bytes_written = ::write(log_fd_, log_data, size);

  if (bytes_written != size) {
    if (bytes_written == -1) {
      // System call failed
      bw_graph::logger::log_error("System call failed while writing log");
    } else if (bytes_written >= 0) {
      // Partial write occurred - handle by attempting to write remaining data
      bw_graph::logger::log_error("Partial write occurred while writing log");

      // Attempt to write remaining data
      char* remaining_data = log_data + bytes_written;
      int remaining_size = size - bytes_written;
      ssize_t remaining_written = ::write(log_fd_, remaining_data, remaining_size);

      if (remaining_written != remaining_size) {
        bw_graph::logger::log_error("Failed to complete log write after partial write");
      }
    }
    return;
  }

  // Force flush log to ensure data persistence
#ifdef __APPLE__
  // macOS: Use F_FULLFSYNC for complete data integrity
  if (::fcntl(log_fd_, F_FULLFSYNC) != 0) {
    bw_graph::logger::log_error("Failed to sync log file");
  }
#else // Linux/Ubuntu
  // Ubuntu: Use fdatasync for better SSD performance
  if (::fdatasync(log_fd_) != 0) {
    bw_graph::logger::log_error("Failed to sync log file");
  }
#endif
}

// Read log.
bool disk_manager_t::read_log(char* log_data, int size, int offset) {
  // SSD-optimized log read using cached file descriptor
  ssize_t bytes_read = ::pread(log_fd_, log_data, size, offset);

  if (bytes_read != size) {
    if (bytes_read == -1) {
      // System call failed
      bw_graph::logger::log_error("System call failed while reading log");
      return false;
    } else if (bytes_read == 0) {
      // End of file reached, no data available at this offset
      bw_graph::logger::log_error("Attempted to read beyond end of log file");
      return false;
    } else if (bytes_read > 0) {
      // Partial read occurred - this might be valid if near end of file
      // Fill remaining bytes with zeros for consistency
      std::memset(log_data + bytes_read, 0, size - bytes_read);

      // Log a warning but return true since we got some data
      bw_graph::logger::log_error("Partial read occurred while reading log");
      return true; // Partial success
    }
  }

  // Full read successful
  return true;
}

// Get num flushes.
int disk_manager_t::get_num_flushes() const { return num_flushes_; }

// Get flush state.
bool disk_manager_t::get_flush_state() const { return flush_log_; }

// Get num writes.
int disk_manager_t::get_num_writes() const { return num_writes_; }

// Get num deletes.
int disk_manager_t::get_num_deletes() const { return num_deletes_; }

// Set flush log future.
void disk_manager_t::set_flush_log_future(std::future<void>* f) { flush_log_f_ = f; }

// Check flush log future.
bool disk_manager_t::has_flush_log_future() const { return flush_log_f_ != nullptr; }

// Get log file name.
std::filesystem::path disk_manager_t::get_log_file_name() const { return log_file_name_; }

// Get file size.
int disk_manager_t::get_file_size(const std::string& file_name) {
  // Use fstat with cached file descriptor for better performance and
  // compatibility
  struct stat stat_buf;

  // Try to use cached file descriptor first
  int fd = -1;
  bool should_close = false;

  if (file_name == db_file_name_.string() && db_fd_ != -1) {
    fd = db_fd_;
  } else if (file_name == log_file_name_.string() && log_fd_ != -1) {
    fd = log_fd_;
  } else {
    // Open file temporarily if no cached descriptor available
    fd = ::open(file_name.c_str(), O_RDONLY);
    should_close = true;
  }

  if (fd == -1) {
    return -1; // Failed to open file
  }

  if (::fstat(fd, &stat_buf) != 0) {
    if (should_close) {
      ::close(fd);
    }
    return -1; // Failed to get file statistics
  }

  if (should_close) {
    ::close(fd);
  }

  // Check for file size overflow when converting to int
  if (stat_buf.st_size > INT_MAX) {
    return -1; // File too large for int return type
  }

  return static_cast<int>(stat_buf.st_size);
}

// Get db file size.
size_t disk_manager_t::get_db_file_size() {
  // Directly use cached database file descriptor for optimal performance
  struct stat stat_buf;

  if (db_fd_ == -1) {
    bw_graph::logger::log_error("Database file descriptor not available");
    return static_cast<size_t>(-1);
  }

  if (::fstat(db_fd_, &stat_buf) != 0) {
    bw_graph::logger::log_error("Failed to get database file size");
    return static_cast<size_t>(-1);
  }

  return static_cast<size_t>(stat_buf.st_size);
}

// Get page count.
uint64_t disk_manager_t::get_page_count() const {
  return valid_page_count_.load(std::memory_order_acquire);
}

// Allocate page.
size_t disk_manager_t::allocate_page() {
  std::lock_guard<std::mutex> lock(db_io_latch_);

  // Check if there are any free slots to reuse
  if (!free_slots_.empty()) {
    size_t offset = free_slots_.back();
    free_slots_.pop_back();
    return offset;
  }

  // No free slots, allocate at end of file
  size_t current_size = this->global_page_count_.fetch_add(1) * page_size_;

  // Ensure file is large enough
  if (current_size < page_capacity_) {
    page_capacity_ = current_size + page_size_;
  }

  size_t offset = current_size;
  page_capacity_ += page_size_;

  return offset;
}
