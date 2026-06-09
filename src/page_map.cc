#include "bw_graph/buf/page_map.h"

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/part/page_delta.h"

#include <cstddef>
#include <sys/types.h>

namespace {

page_map_entry_t* get_or_create_entry(page_map_inner_t& page_map_inner, page_no_t page_no) {
  auto it = page_map_inner.find(page_no);
  if (it != page_map_inner.end()) {
    return it->second;
  }

  page_map_entry_t* new_entry = new page_map_entry_t(page_no, nullptr, 0);
  auto [emplaced_it, inserted] = page_map_inner.emplace(page_no, new_entry);
  if (!inserted) {
    delete new_entry;
  }
  return emplaced_it->second;
}

} // namespace

// Construct a page-map entry.
page_map_entry_t::page_map_entry_t(page_no_t page_no, delta_page_t* delta_page,
                                   uint64_t delta_page_count) {
  this->page_no = page_no;
  this->delta_page = delta_page;
  this->delta_page_count.store(delta_page_count);
}

// Get delta page count.
uint64_t page_map_entry_t::get_delta_page_count() const { return this->delta_page_count.load(); }

// Get first delta page.
delta_page_t* page_map_entry_t::get_first_delta_page() { return this->delta_page; }

// Get first delta page.
delta_page_t* page_map_entry_t::get_first_delta_page() const { return this->delta_page; }

// Set delta page ptr.
void page_map_entry_t::set_delta_page_ptr(delta_page_t* new_delta_page) {
  this->delta_page = new_delta_page;
}

// Handle inc delta page count.
void page_map_entry_t::inc_delta_page_count() {
  this->delta_page_count.fetch_add(1, std::memory_order_acq_rel);
}

// Reset delta page count.
void page_map_entry_t::reset_delta_page_count() {
  this->delta_page_count.store(0, std::memory_order_release);
}

// Construct a page map with its delta pool and disk manager.
page_map_t::page_map_t(buf_pool_t<delta_page_t>* pool, disk_manager_t* disk_mgr)
    : delta_pool_(pool), delta_disk_mgr_(disk_mgr) {}

// Set pool.
void page_map_t::set_pool(buf_pool_t<delta_page_t>* pool, disk_manager_t* disk_mgr) {
  delta_pool_ = pool;
  delta_disk_mgr_ = disk_mgr;
}

// Allocate a fresh delta page from disk and buffer pool.
delta_page_t* page_map_t::alloc_delta_page() {
  // Allocate a disk slot and derive its page number.
  size_t offset = delta_disk_mgr_->allocate_page();
  page_no_t new_page_no = static_cast<page_no_t>(offset / bw_graph::BW_DELTA_PAGE_SIZE);

  // Get a reinitialized pool frame and release temporary ownership.
  delta_page_t* page = delta_pool_->buf_page_new(new_page_no, delta_disk_mgr_);
  page->w_unlatch();
  page->dec_pin_count();
  return page;
}

// Insert delta records into the page's delta chain.
void page_map_t::insert_delta(page_no_t page_no, std::vector<delta_record_t> delta_records) {
  page_map_entry_t* entry_ptr = get_or_create_entry(page_map_inner, page_no);
  scoped_w_lock_t guard(entry_ptr->entry_latch);

  delta_page_t* head = entry_ptr->get_first_delta_page();

  // Recreate the chain after consolidation reset.
  if (head == nullptr) {
    head = alloc_delta_page();
    entry_ptr->set_delta_page_ptr(head);
    entry_ptr->inc_delta_page_count();
  }

  // Append to the current head, prepending pages when full.
  for (size_t i = 0; i < delta_records.size();) {
    bool ok = head->insert_delta(delta_records[i]);
    if (ok) {
      ++i;
    } else {
      // Prepend a new head page when the current head is full.
      delta_page_t* new_page = alloc_delta_page();
      new_page->set_next_ptr(head);
      entry_ptr->set_delta_page_ptr(new_page);
      entry_ptr->inc_delta_page_count();
      head = new_page;
    }
  }
}

// Insert one delta record and return its physical location.
std::pair<page_no_t, uint32_t> page_map_t::insert_delta_and_get_loc(page_no_t csr_page_no,
                                                                    delta_record_t record) {
  page_map_entry_t* entry_ptr = get_or_create_entry(page_map_inner, csr_page_no);
  scoped_w_lock_t guard(entry_ptr->entry_latch);

  delta_page_t* head = entry_ptr->get_first_delta_page();

  // Recreate the chain after consolidation reset.
  if (head == nullptr) {
    head = alloc_delta_page();
    entry_ptr->set_delta_page_ptr(head);
    entry_ptr->inc_delta_page_count();
  }

  uint32_t record_idx = static_cast<uint32_t>(head->get_num_records());
  bool ok = head->insert_delta(record);
  if (!ok) {
    // Prepend a new head page when the current head is full.
    delta_page_t* new_page = alloc_delta_page();
    new_page->set_next_ptr(head);
    entry_ptr->set_delta_page_ptr(new_page);
    entry_ptr->inc_delta_page_count();
    head = new_page;
    record_idx = 0;
    ok = head->insert_delta(record);
    if (!ok) {
      throw std::runtime_error("insert_delta_and_get_loc: insert failed on fresh page");
    }
  }
  return {head->get_page_no(), record_idx};
}

// Fetch all deltas for a page and mark their pages for consolidation.
std::vector<delta_record_t> page_map_t::fetch_deltas_with_mark(page_no_t page_no) {
  auto target_page_entry = page_map_inner.find(page_no);
  if (target_page_entry == page_map_inner.end()) {
    return {};
  }

  auto entry_ptr = target_page_entry->second;
  entry_ptr->entry_latch.w_lock();

  std::vector<std::vector<delta_record_t>> page_records;
  delta_page_t* current = entry_ptr->get_first_delta_page();

  while (current != nullptr) {
    // Save next before unpinning because the frame may be reused.
    delta_page_t* next = current->get_next_delta_page();

    auto delta_span = current->get_delta_span();
    page_records.emplace_back(delta_span.begin(), delta_span.end());

    current->set_status(CONSOLIDATE);
    current->dec_pin_count();

    current = next;
  }

  // Reset the entry so new deltas start a fresh chain.
  entry_ptr->set_delta_page_ptr(nullptr);
  entry_ptr->reset_delta_page_count();

  entry_ptr->entry_latch.w_unlock();
  std::vector<delta_record_t> result;
  for (auto it = page_records.rbegin(); it != page_records.rend(); ++it) {
    result.insert(result.end(), it->begin(), it->end());
  }
  return result;
}

// Get the number of delta pages in a page's chain.
uint64_t page_map_t::get_delta_chain_length(page_no_t page_no) {
  auto target_page_entry = page_map_inner.find(page_no);
  if (target_page_entry == page_map_inner.end()) {
    return 0;
  }

  auto entry_ptr = target_page_entry->second;
  entry_ptr->entry_latch.r_lock();
  uint64_t length = entry_ptr->get_delta_page_count();
  entry_ptr->entry_latch.r_unlock();
  return length;
}

// Get the inner page map.
page_map_inner_t& page_map_t::get_inner_page_map() { return this->page_map_inner; }

// Truncate the per-vertex chain at a boundary record location.
void page_map_t::truncate_chain_at(delta_head_t loc) {
  if (!loc.is_valid() || delta_pool_ == nullptr || delta_disk_mgr_ == nullptr) {
    return;
  }

  // buf_page_read returns the frame with the r_latch already held; the latch
  // keeps the frame resident in the page map for the duration of the in-place
  // edit. clear_record_next overwrites next_page_no first, so concurrent
  // readers stop chain traversal at this record even if they observe a torn
  // half-update (next_record_idx still equal to the old value).
  delta_page_t* boundary_page = delta_pool_->buf_page_read(loc.page_no, delta_disk_mgr_);
  boundary_page->clear_record_next(loc.record_idx);
  boundary_page->r_unlatch();
}

// Forwarder-aware single record insert.
std::pair<page_no_t, uint32_t> page_map_t::insert_delta_with_forward(page_no_t csr_page_no,
                                                                     v_id_t src,
                                                                     delta_record_t record,
                                                                     bool& out_rerouted) {
  out_rerouted = false;

  // One ConcurrentHashMap probe serves both purposes: locate the source
  // entry AND, when consolidation is in flight, look up the forwarder.
  page_no_t target_pno = csr_page_no;
  std::shared_ptr<page_delta_t> forwarder;
  auto src_entry = page_map_inner.find(csr_page_no);
  if (src_entry != page_map_inner.end()) {
    forwarder = src_entry->second->acquire_forwarder();
    if (forwarder != nullptr) {
      page_no_t new_pno = 0;
      if (forwarder->lookup_forward(src, new_pno)) {
        target_pno = new_pno;
        out_rerouted = true;
      }
    }
  }

  auto loc = insert_delta_and_get_loc(target_pno, record);

  if (out_rerouted) {
    forwarder->note_rerouted_tail(src, {loc.first, loc.second});
  }
  return loc;
}

// Install a forwarder on the entry for @p csr_page_no.
void page_map_t::install_forwarder_for(page_no_t csr_page_no,
                                       std::shared_ptr<page_delta_t> forwarder) {
  auto it = page_map_inner.find(csr_page_no);
  if (it != page_map_inner.end()) {
    it->second->install_forwarder(std::move(forwarder));
    return;
  }
  // No entry yet (no delta has been inserted on this page).  Materialise an
  // empty entry so writers observing this page_no during the consolidation
  // window can still discover the forwarder.
  page_map_entry_t* new_entry = new page_map_entry_t(csr_page_no, nullptr, 0);
  new_entry->install_forwarder(forwarder);
  auto [emplaced_it, inserted] = page_map_inner.emplace(csr_page_no, new_entry);
  if (!inserted) {
    // Another writer raced us; install on the winning entry and drop ours.
    delete new_entry;
    emplaced_it->second->install_forwarder(std::move(forwarder));
  }
}

// Acquire shared ownership of the forwarder on @p csr_page_no.
std::shared_ptr<page_delta_t> page_map_t::acquire_forwarder_for(page_no_t csr_page_no) const {
  auto it = page_map_inner.find(csr_page_no);
  if (it == page_map_inner.end()) {
    return {};
  }
  return it->second->acquire_forwarder();
}

// Detach the forwarder on the entry for @p csr_page_no.
std::shared_ptr<page_delta_t> page_map_t::detach_forwarder_for(page_no_t csr_page_no) {
  auto it = page_map_inner.find(csr_page_no);
  if (it == page_map_inner.end()) {
    return {};
  }
  return it->second->detach_forwarder();
}

// Claim one in-flight consolidation for @p csr_page_no.
bool page_map_t::try_begin_consolidation_for(page_no_t csr_page_no) {
  auto it = page_map_inner.find(csr_page_no);
  if (it == page_map_inner.end()) {
    return false;
  }
  return it->second->try_begin_consolidation();
}

// Release the in-flight consolidation claim for @p csr_page_no.
void page_map_t::end_consolidation_for(page_no_t csr_page_no) {
  auto it = page_map_inner.find(csr_page_no);
  if (it != page_map_inner.end()) {
    it->second->end_consolidation();
  }
}
