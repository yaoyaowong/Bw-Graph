#ifndef BW_GRAPH_STORAGE_DISK_MANAGER_H
#define BW_GRAPH_STORAGE_DISK_MANAGER_H

#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"

/**
 * @brief Disk manager for handling page allocation and I/O operations
 *
 * DiskManager handles the allocation and deallocation of pages within a
 * database. It performs reading and writing of pages to and from disk,
 * providing a logical file layer within the context of a database management
 * system.
 *
 * DiskManager uses lazy allocation, meaning that it only allocates space on
 * disk when it is first accessed. It maintains a mapping of page ids to their
 * corresponding offsets in the database file. When a page is deleted, it is
 * marked as free and can be reused by future allocations.
 */
class disk_manager_t {
public:
  /**
   * @brief Constructor with database file path and page size
   *
   * @param db_file   Path to the database file
   * @param page_size Size of each page in bytes (default: BW_GRAPH_PAGE_SIZE)
   */
  explicit disk_manager_t(const std::filesystem::path& db_file,
                          size_t page_size = bw_graph::BW_GRAPH_PAGE_SIZE);

  /**
   * @brief Default constructor for testing purposes
   */
  disk_manager_t() = default;

  /**
   * @brief Virtual destructor
   */
  virtual ~disk_manager_t() = default;

  /**
   * @brief Shutdown the disk manager and close all file streams
   */
  void shutdown();

  /**
   * @brief Write a page to the database file
   *
   * @param page_no ID of the page to write
   * @param page_data Raw page data to write
   */
  virtual void write_page(page_no_t page_no, const char* page_data);

  /**
   * @brief Read a page from the database file
   *
   * @param page_no ID of the page to read
   * @param page_data Output buffer to store the read data
   */
  virtual void read_page(page_no_t page_no, char* page_data);

  /**
   * @brief Delete a page from the database file and reclaim disk space
   *
   * @param page_no ID of the page to delete
   */
  virtual void delete_page(page_no_t page_no);

  /**
   * @brief Write log data to the log file
   *
   * @param log_data Log data to write
   * @param size Size of the log data in bytes
   */
  void write_log(char* log_data, int size);

  /**
   * @brief Read log data from the log file
   *
   * @param log_data Output buffer to store the read log data
   * @param size Size of data to read in bytes
   * @param offset Offset in the log file to start reading from
   * @return true if read operation was successful, false otherwise
   */
  bool read_log(char* log_data, int size, int offset);

  /**
   * @brief Get the number of flush operations performed
   *
   * @return Number of flush operations
   */
  int get_num_flushes() const;

  /**
   * @brief Get the current flush state
   *
   * @return true if flush is in progress, false otherwise
   */
  bool get_flush_state() const;

  /**
   * @brief Get the number of write operations performed
   *
   * @return Number of write operations
   */
  int get_num_writes() const;

  /**
   * @brief Get the number of delete operations performed
   *
   * @return Number of delete operations
   */
  int get_num_deletes() const;

  /**
   * @brief Set the future for non-blocking flush operations
   *
   * @param f Pointer to the future object for flush checking
   */
  void set_flush_log_future(std::future<void>* f);

  /**
   * @brief Check if the non-blocking flush future was set
   *
   * @return true if flush log future is set, false otherwise
   */
  bool has_flush_log_future() const;

  /**
   * @brief Get the log file name
   *
   * @return Path to the log file
   */
  std::filesystem::path get_log_file_name() const;

  /**
   * @brief Get the current database file size
   *
   * @return Size of the database file in bytes
   */
  size_t get_db_file_size();

  /**
   * @brief Get the number of valid pages in the database file.
   *
   * Computed once at startup via a backward scan from the end of the file
   * (same logic as bin/storage.cc). Automatically updated via CAS whenever
   * a page with a higher index is written.
   *
   * @return Number of valid (non-zero) pages currently in the database
   */
  uint64_t get_page_count() const;

  /**
   * @brief Allocate a new page in the database file
   *
   * @return Offset of the allocated page
   */
  size_t allocate_page();

protected:
  /* Statistics counters */
  int num_flushes_{0};
  int num_writes_{0};
  int num_deletes_{0};

  /* Size of a single page in bytes (set at construction time) */
  size_t page_size_{bw_graph::BW_GRAPH_PAGE_SIZE};

  /* The capacity of the file used for storage on disk */
  size_t page_capacity_{bw_graph::BW_GRAPH_DEFAULT_DB_IO_SIZE};

private:
  /**
   * @brief Get the size of a file
   *
   * @param file_name Name of the file
   * @return Size of the file in bytes, -1 if error
   */
  int get_file_size(const std::string& file_name);

  /* Global page count (used by allocate_page / write_page CAS) */
  std::atomic<uint64_t> global_page_count_{0};

  /* Valid page count: set at startup via backward scan, updated on write */
  std::atomic<uint64_t> valid_page_count_{0};

  std::filesystem::path log_file_name_;
  std::filesystem::path db_file_name_;

  /* Optional: Cache file descriptors to reduce open/close overhead */
  mutable int db_fd_{-1};
  mutable int log_fd_{-1};
  mutable std::mutex fd_mutex_;

  /* Records the free slots in the database file if pages are deleted */
  std::vector<size_t> free_slots_;

  /* Flush log flag */
  bool flush_log_{false};

  /* Future for non-blocking flush operations */
  std::future<void>* flush_log_f_{nullptr};

  /* Mutex to protect file access with multiple buffer pool instances */
  std::mutex db_io_latch_;
};

#endif // BW_GRAPH_STORAGE_DISK_MANAGER_H