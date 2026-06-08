#include "bw_graph/io/io_delta.h"

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/storage/disk_manager.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

// Disable further parsing (buffer-pool compatibility)
void delta_page_t::disable_parse() { is_parse_ = false; }

// Reinitialise an already-constructed frame as a fresh delta page
void delta_page_t::reinit(page_no_t page_no) {
  set_page_no(page_no);

  char* page_data = get_data();
  if (page_data != nullptr) {
    std::memset(page_data, 0, bw_graph::BW_DELTA_PAGE_SIZE);
    // Write metadata header: [delta_record_count=0][used_bytes=24][parent_page_no=0]
    *reinterpret_cast<uint64_t*>(page_data) = 0;
    *reinterpret_cast<uint64_t*>(page_data + sizeof(uint64_t)) = 3 * sizeof(uint64_t);
    *reinterpret_cast<uint64_t*>(page_data + 2 * sizeof(uint64_t)) = 0;
  }

  is_parse_ = false;
  num_delta_records_ = 0;
  used_bytes_ = 3 * sizeof(uint64_t);
  parent_page_no_ = 0;
  next_delta_page_ = nullptr;
  status_ = ACCUMULATE;
  delta_records_span_ = delta_span_t{};

  // A freshly reinitialised frame holds new in-memory metadata that has not
  // yet been written to disk; mark it dirty so the flusher persists it.
  set_dirty(true);
}

// Default constructor
delta_page_t::delta_page_t() : page_t(), num_delta_records_(0), delta_records_span_() {}

// Constructor with page number
delta_page_t::delta_page_t(page_no_t page_no)
    : page_t(page_no, DELTA_PAGE), num_delta_records_(0), delta_records_span_() {
  // Initialize page with empty metadata
  char* page_data = get_data();
  if (page_data != nullptr) {
    // Write metadata header: [delta_record_count][used_bytes]
    std::memset(page_data, 0, static_cast<size_t>(bw_graph::BW_DELTA_PAGE_SIZE));
    *reinterpret_cast<uint64_t*>(page_data) = 0; // delta record count
    *reinterpret_cast<uint64_t*>(page_data + sizeof(uint64_t)) =
        3 * sizeof(uint64_t); // used bytes (metadata only)
    *reinterpret_cast<uint64_t*>(page_data + 2 * sizeof(uint64_t)) = 0; // parent page no

    used_bytes_ = 3 * sizeof(uint64_t);
  }
}

// Self parse function
void delta_page_t::self_parse() {
  char* page_data = get_data();
  if (page_data == nullptr) {
    throw std::runtime_error("Failed to get page data for self-parsing");
  }

  // Read metadata from first 16 bytes: [delta_record_count][used_bytes]
  num_delta_records_ = *reinterpret_cast<const uint64_t*>(page_data);
  used_bytes_ = *reinterpret_cast<const uint64_t*>(page_data + sizeof(uint64_t));
  parent_page_no_ = *reinterpret_cast<const uint64_t*>(page_data + 2 * sizeof(uint64_t));

  // Set up span if there are existing records
  if (num_delta_records_ > 0) {
    size_t metadata_size = 3 * sizeof(uint64_t);
    delta_record_t* delta_ptr = reinterpret_cast<delta_record_t*>(page_data + metadata_size);
    delta_records_span_ = delta_span_t(delta_ptr, num_delta_records_);
  }

  is_parse_ = true;
}

// Insert delta in this delta page
bool delta_page_t::insert_delta(delta_record_t delta_record) {
  char* page_data = get_data();
  if (page_data == nullptr) {
    throw std::runtime_error("Failed to get page data for delta insertion");
  }

  // Parse existing data if not already parsed
  if (!is_parse_) {
    self_parse();
  }

  // Check if there's space for a new delta record
  size_t new_record_size = sizeof(delta_record_t);
  size_t required_space = used_bytes_ + new_record_size;

  if (required_space > static_cast<size_t>(bw_graph::BW_DELTA_PAGE_SIZE)) {
    return false; // Not enough space
  }

  // Calculate position for new record
  size_t metadata_size = 3 * sizeof(uint64_t);
  delta_record_t* new_record_ptr = reinterpret_cast<delta_record_t*>(
      page_data + metadata_size + num_delta_records_ * sizeof(delta_record_t));

  // Insert the new delta record
  *new_record_ptr = delta_record;

  // Update metadata
  num_delta_records_++;
  used_bytes_ += new_record_size;

  // Update metadata in page
  *reinterpret_cast<uint64_t*>(page_data) = num_delta_records_;
  *reinterpret_cast<uint64_t*>(page_data + sizeof(uint64_t)) = used_bytes_;
  *reinterpret_cast<uint64_t*>(page_data + 2 * sizeof(uint64_t)) = parent_page_no_;

  // Update span to include the new record
  delta_record_t* delta_ptr = reinterpret_cast<delta_record_t*>(page_data + metadata_size);
  delta_records_span_ = delta_span_t(delta_ptr, num_delta_records_);

  // Mark this page dirty so the async flusher knows to persist it.
  set_dirty(true);

  return true;
}

// Flush this page to disk without blocking on a busy latch.
bool delta_page_t::flush_to_disk(disk_manager_t* disk_mgr) {
  // Skip immediately if another path owns the page latch.
  if (!rw_latch_.try_w_lock()) {
    return false;
  }

  // Re-check dirty inside the latch - another flush may have beaten us.
  if (is_dirty()) {
    disk_mgr->write_page(get_page_no(), get_data());
    set_dirty(false);
  }

  rw_latch_.w_unlock();
  return true;
}

// Retrieve related delta records
std::vector<delta_record_t> delta_page_t::get_related_records(v_id_t vertex_id) {
  std::vector<delta_record_t> related_records;

  char* page_data = get_data();
  if (page_data == nullptr) {
    return related_records;
  }

  // Parse data if not already parsed
  if (!is_parse_) {
    self_parse();
  }

  // Early return if no records
  if (num_delta_records_ == 0) {
    return related_records;
  }

  // Reserve space to avoid frequent reallocations
  related_records.reserve(num_delta_records_);

  // Iterate through all delta records and find those related to the vertex
  for (uint64_t i = 0; i < num_delta_records_; ++i) {
    const delta_record_t& record = delta_records_span_[i];

    if (record.first == vertex_id) {
      related_records.push_back(record);
    }
  }

  return related_records;
}

// Set the parent page number
void delta_page_t::set_parent_page_no(uint64_t parent_page_no) {
  parent_page_no_ = parent_page_no;
  char* page_data = get_data();
  *reinterpret_cast<uint64_t*>(page_data + 2 * sizeof(uint64_t)) = parent_page_no_;
  // Header byte changed - the on-disk image is stale until the next flush.
  set_dirty(true);
}

// Set the next pointers
void delta_page_t::set_next_ptr(delta_page_t* next) { this->next_delta_page_ = next; }

// Get the next delta page pointer
delta_page_t* delta_page_t::get_next_delta_page() const {
  if (next_delta_page_ == nullptr) {
    return nullptr;
  } else {
    return this->next_delta_page_;
  }
}

// Set the status of the delta page
void delta_page_t::set_status(delta_page_status_t status) { this->status_ = status; }

// Get the status of the delta page
delta_page_status_t delta_page_t::get_status() const { return this->status_; }

// Get the span of delta records in this delta page
delta_span_t delta_page_t::get_delta_span() const { return this->delta_records_span_; }

// Return number of records stored in this page
uint64_t delta_page_t::get_num_records() const { return num_delta_records_; }

// Return a const reference to the record at the given index
const delta_record_t& delta_page_t::get_record(uint32_t idx) {
  if (!is_parse_) {
    self_parse();
  }
  return delta_records_span_[idx];
}

// Clear the next-link of an existing record so the chain stops at idx.
void delta_page_t::clear_record_next(uint32_t idx) {
  if (!is_parse_) {
    self_parse();
  }
  delta_record_t& rec = delta_records_span_[idx];
  rec.next_page_no = INVALID_DELTA_PAGE_NO;
  rec.next_record_idx = INVALID_DELTA_RECORD_IDX;
  // Header byte changed - the on-disk image is stale until the next flush.
  set_dirty(true);
}
