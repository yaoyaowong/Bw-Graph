#ifndef BW_GRAPH_COMMON_SEQUENCE_H
#define BW_GRAPH_COMMON_SEQUENCE_H

#include "index_map.h"

#include <tuple>

namespace pbbs {
using namespace std;

constexpr const size_t _log_block_size = 12;
constexpr const size_t _block_size = (1 << _log_block_size);

/**
 * @brief Calculate the number of blocks needed for n items.
 * @param n           Number of items
 * @param block_size  Number of items per block
 * @return Number of blocks
 */
inline size_t num_blocks(size_t n, size_t block_size) { return (1 + ((n) -1) / (block_size)); }

/**
 * @brief Run a function over contiguous slices in parallel.
 * @param n           Number of items to slice
 * @param block_size  Number of items per slice
 * @param f           Function called as f(block_id, begin, end)
 */
template <class F>
void sliced_for(size_t n, size_t block_size, const F& f) {
  size_t l = num_blocks(n, block_size);
  bw_parallel_for_1(size_t i = 0; i < l; i++) {
    size_t s = i * block_size;
    size_t e = min(s + block_size, n);
    f(i, s, e);
  }
}

/**
 * @brief Reduce an index map sequentially.
 * @param A  Input index map
 * @param f  Binary reduction function
 * @return Reduced value
 */
template <class Index_Map, class F>
auto reduce_serial(Index_Map A, const F& f) -> typename Index_Map::T {
  typename Index_Map::T r = A[0];
  for (size_t j = 1; j < A.size(); j++) {
    r = f(r, A[j]);
  }
  return r;
}

/**
 * @brief Reduce an index map, using parallel block reduction when profitable.
 * @param A   Input index map
 * @param f   Binary reduction function
 * @param fl  Execution flags
 * @return Reduced value
 */
template <class Index_Map, class F>
auto reduce(Index_Map A, const F& f, flags fl = no_flag) -> typename Index_Map::T {
  using T = typename Index_Map::T;
  size_t n = A.size();
  size_t l = num_blocks(n, _block_size);
  if (l <= 1 || (fl & fl_sequential))
    return reduce_serial(A, f);
  array_imap<T> Sums(l);
  // Reduce each block first, then reduce the block results.
  sliced_for(n, _block_size,
             [&](size_t i, size_t s, size_t e) { Sums[i] = reduce_serial(A.cut(s, e), f); });
  T r = reduce_serial(Sums, f);
  return r;
}

/**
 * @brief Reduce an index map with operator+.
 * @param I   Input index map
 * @param fl  Execution flags
 * @return Sum of all values
 */
template <class Index_Map>
auto reduce_add(Index_Map I, flags fl = no_flag) -> typename Index_Map::T {
  using T = typename Index_Map::T;
  auto add = [](T x, T y) { return x + y; };
  return reduce(I, add, fl);
}

const flags fl_scan_inclusive = (1 << 4);

/**
 * @brief Run a sequential prefix scan over an input index map.
 * @param In    Input index map
 * @param Out   Output index map
 * @param f     Binary scan function
 * @param zero  Initial value
 * @param fl    Scan flags
 * @return Total reduction value
 */
template <class Imap_In, class Imap_Out, class F>
auto scan_serial(Imap_In In, Imap_Out Out, const F& f, typename Imap_In::T zero, flags fl = no_flag)
    -> typename Imap_In::T {
  using T = typename Imap_In::T;
  T r = zero;
  size_t n = In.size();
  bool inclusive = fl & fl_scan_inclusive;
  if (inclusive) {
    for (size_t i = 0; i < n; i++) {
      r = f(r, In[i]);
      Out.update(i, r);
    }
  } else {
    for (size_t i = 0; i < n; i++) {
      T t = In[i];
      Out.update(i, r);
      r = f(r, t);
    }
  }
  return r;
}

/**
 * @brief Run a prefix scan, using parallel block scan when profitable.
 * @param In    Input index map
 * @param Out   Output index map
 * @param f     Binary scan function
 * @param zero  Initial value
 * @param fl    Scan flags
 * @return Total reduction value
 */
template <class Imap_In, class Imap_Out, class F>
auto scan(Imap_In In, Imap_Out Out, const F& f, typename Imap_In::T zero, flags fl = no_flag) ->
    typename Imap_In::T {
  using T = typename Imap_In::T;
  size_t n = In.size();
  size_t l = num_blocks(n, _block_size);
  if (l <= 2 || fl & fl_sequential)
    return scan_serial(In, Out, f, zero, fl);
  array_imap<T> Sums(l);
  // Compute each block total, scan block totals, then scan blocks with offsets.
  sliced_for(n, _block_size,
             [&](size_t i, size_t s, size_t e) { Sums[i] = reduce_serial(In.cut(s, e), f); });
  T total = scan_serial(Sums, Sums, f, zero, 0);
  sliced_for(n, _block_size, [&](size_t i, size_t s, size_t e) {
    scan_serial(In.cut(s, e), Out.cut(s, e), f, Sums[i], fl);
  });
  return total;
}

/**
 * @brief Run a prefix scan using operator+.
 * @param In   Input index map
 * @param Out  Output index map
 * @param fl   Scan flags
 * @return Total sum
 */
template <class Imap_In, class Imap_Out>
auto scan_add(Imap_In In, Imap_Out Out, flags fl = no_flag) -> typename Imap_In::T {
  using T = typename Imap_In::T;
  auto add = [](T x, T y) { return x + y; };
  return scan(In, Out, add, (T) 0, fl);
}

/**
 * @brief Count truthy values in an index map sequentially.
 * @param I  Input flag map
 * @return Number of truthy values
 */
template <class Index_Map>
size_t sum_flags_serial(Index_Map I) {
  size_t r = 0;
  for (size_t j = 0; j < I.size(); j++)
    r += I[j];
  return r;
}

/**
 * @brief Pack selected input values into a new array sequentially.
 * @param In  Input value map
 * @param Fl  Flag map indicating selected values
 * @return Packed array map
 */
template <class Imap_In, class Imap_Fl>
auto pack_serial(Imap_In In, Imap_Fl Fl) -> array_imap<typename Imap_In::T> {
  using T = typename Imap_In::T;
  size_t n = In.size();
  size_t m = sum_flags_serial(Fl);
  T* Out = new_array_no_init<T>(m);
  size_t k = 0;
  for (size_t i = 0; i < n; i++)
    if (Fl[i])
      assign_uninitialized(Out[k++], In[i]);
  return make_array_imap(Out, m);
}

/**
 * @brief Pack selected input values into a provided output buffer sequentially.
 * @param In   Input value map
 * @param Out  Output buffer
 * @param Fl   Flag map indicating selected values
 */
template <class Imap_In, class Imap_Fl>
void pack_serial_at(Imap_In In, typename Imap_In::T* Out, Imap_Fl Fl) {
  size_t k = 0;
  for (size_t i = 0; i < In.size(); i++)
    if (Fl[i])
      Out[k++] = In[i];
}

/**
 * @brief Pack selected input values into a new array.
 * @param In  Input value map
 * @param Fl  Flag map indicating selected values
 * @param fl  Execution flags
 * @return Packed array map
 */
template <class Imap_In, class Imap_Fl>
auto pack(Imap_In In, Imap_Fl Fl, flags fl = no_flag) -> array_imap<typename Imap_In::T> {
  using T = typename Imap_In::T;
  size_t n = In.size();
  size_t l = num_blocks(n, _block_size);
  if (l <= 1 || fl & fl_sequential)
    return pack_serial(In, Fl);
  array_imap<size_t> Sums(l);
  // Count selected values per block, scan counts, then pack into offsets.
  sliced_for(n, _block_size,
             [&](size_t i, size_t s, size_t e) { Sums[i] = sum_flags_serial(Fl.cut(s, e)); });
  size_t m = scan_add(Sums, Sums);
  T* Out = new_array_no_init<T>(m);
  sliced_for(n, _block_size, [&](size_t i, size_t s, size_t e) {
    pack_serial_at(In.cut(s, e), Out + Sums[i], Fl.cut(s, e));
  });
  return make_array_imap(Out, m);
}

/**
 * @brief Pack indexes whose flags are true.
 * @param Fl  Flag map indicating selected indexes
 * @param fl  Execution flags
 * @return Packed index array map
 */
template <class Idx_Type, class Imap_Fl>
array_imap<Idx_Type> pack_index(Imap_Fl Fl, flags fl = no_flag) {
  auto identity = [](size_t i) { return (Idx_Type) i; };
  return pack(make_in_imap<Idx_Type>(Fl.size(), identity), Fl, fl);
}

/**
 * @brief Pack indexes and associated data whose flags are true.
 * @param f     Function returning tuple(flag, data)
 * @param size  Number of input entries
 * @param fl    Execution flags
 * @return Packed array of tuple(index, data)
 */
template <class Idx_Type, class D, class F>
array_imap<tuple<Idx_Type, D>> pack_index_and_data(F& f, size_t size, flags fl = no_flag) {
  auto identity = [&](size_t i) { return make_tuple((Idx_Type) i, get<1>(f(i))); };
  auto flgs_in = make_in_imap<bool>(size, [&](size_t i) { return get<0>(f(i)); });
  return pack(make_in_imap<tuple<Idx_Type, D>>(size, identity), flgs_in, fl);
}

/**
 * @brief Filter an input array sequentially.
 * @param In   Input array
 * @param Out  Output array
 * @param n    Number of input items
 * @param p    Predicate function
 * @return Number of output items
 */
template <class T, class PRED>
size_t filter_serial(T* In, T* Out, size_t n, PRED p) {
  size_t k = 0;
  for (size_t i = 0; i < n; i++)
    if (p(In[i]))
      Out[k++] = In[i];
  return k;
}

/**
 * @brief Filter an input array, using parallel block compaction when profitable.
 * @param In   Input array, compacted in place by blocks
 * @param Out  Output array
 * @param n    Number of input items
 * @param p    Predicate function
 * @return Number of output items
 */
template <class T, class PRED>
size_t filterf(T* In, T* Out, size_t n, PRED p) {
  size_t b = _F_BSIZE;
  if (n < b)
    return filter_serial(In, Out, n, p);
  size_t l = nblocks(n, b);
  b = nblocks(n, l);
  size_t* Sums = new_array_no_init<size_t>(l + 1);
  bw_parallel_for_1(size_t i = 0; i < l; i++) {
    // Compact each block locally and store its selected count.
    size_t s = i * b;
    size_t e = min(s + b, n);
    size_t k = s;
    for (size_t j = s; j < e; j++)
      if (p(In[j]))
        In[k++] = In[j];
    Sums[i] = k - s;
  }
  auto isums = array_imap<size_t>(Sums, l);
  // Convert block counts to output offsets.
  size_t m = scan_add(isums, isums);
  Sums[l] = m;
  bw_parallel_for_1(size_t i = 0; i < l; i++) {
    T* I = In + i * b;
    T* O = Out + Sums[i];
    for (size_t j = 0; j < Sums[i + 1] - Sums[i]; j++) {
      O[j] = I[j];
    }
  }
  free(Sums);
  return m;
}

/**
 * @brief Filter an input array and clear moved entries.
 * @param In     Input array, compacted and cleared in place by blocks
 * @param Out    Output array
 * @param n      Number of input items
 * @param p      Predicate function
 * @param empty  Empty value used to clear moved entries
 * @param Sums   Scratch buffer with at least one entry per block plus one
 * @return Number of output items
 */
template <class T, class PRED>
size_t filterf_and_clear(T* In, T* Out, size_t n, PRED p, T& empty, size_t* Sums) {
  size_t b = _F_BSIZE;
  if (n < b)
    return filter_serial(In, Out, n, p);
  size_t l = nblocks(n, b);
  b = nblocks(n, l);
  bw_parallel_for_1(size_t i = 0; i < l; i++) {
    // Compact each block locally and clear values left behind.
    size_t s = i * b;
    size_t e = min(s + b, n);
    size_t k = s;
    for (size_t j = s; j < e; j++) {
      if (p(In[j])) {
        In[k] = In[j];
        if (j > k) {
          In[j] = empty;
        }
        k++;
      }
    }
    Sums[i] = k - s;
  }
  auto isums = array_imap<size_t>(Sums, l);
  // Convert block counts to output offsets.
  size_t m = scan_add(isums, isums);
  Sums[l] = m;
  bw_parallel_for_1(size_t i = 0; i < l; i++) {
    T* I = In + i * b;
    T* O = Out + Sums[i];
    for (size_t j = 0; j < Sums[i + 1] - Sums[i]; j++) {
      O[j] = I[j];
      I[j] = empty;
    }
  }
  return m;
}

} // namespace pbbs

#endif // BW_GRAPH_COMMON_SEQUENCE
