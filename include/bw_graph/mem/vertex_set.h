#ifndef BW_GRAPH_VERTEX_SET_H
#define BW_GRAPH_VERTEX_SET_H

#include "bw_graph/common/index_map.h"
#include "bw_graph/common/maybe.h"
#include "bw_graph/common/sequence.h"

#include <functional>

using namespace std;

/**
 * @brief Vertex subset with optional per-vertex data.
 */
template <class data>
struct vertex_subset_data_t {
  using S = tuple<v_id_t, data>;
  using D = tuple<bool, data>;

  /**
   * @brief Construct an empty vertex subset.
   * @param _n  Number of vertices in the graph
   */
  vertex_subset_data_t(size_t _n) : s(NULL), d(NULL), n(_n), m(0), is_dense(0) {}

  /**
   * @brief Construct a sparse vertex subset.
   * @param _n       Number of vertices in the graph
   * @param _m       Number of active vertices
   * @param indices  Sparse vertex-data entries
   */
  vertex_subset_data_t(long _n, long _m, S* indices)
      : s(indices), d(NULL), n(_n), m(_m), is_dense(0) {}

  /**
   * @brief Construct a dense vertex subset with known active count.
   * @param _n  Number of vertices in the graph
   * @param _m  Number of active vertices
   * @param _d  Dense flag-data entries
   */
  vertex_subset_data_t(long _n, long _m, D* _d) : s(NULL), d(_d), n(_n), m(_m), is_dense(1) {}

  /**
   * @brief Construct a dense vertex subset and compute the active count.
   * @param _n  Number of vertices in the graph
   * @param _d  Dense flag-data entries
   */
  vertex_subset_data_t(long _n, D* _d) : s(NULL), d(_d), n(_n), m(0), is_dense(1) {
    auto d_map = make_in_imap<size_t>(n, [&](size_t i) { return (size_t) get<0>(_d[i]); });
    m = pbbs::reduce_add(d_map);
  }

  /**
   * @brief Construct an empty zero-sized vertex subset.
   */
  vertex_subset_data_t() : s(NULL), d(NULL), n(0), m(0), is_dense(0) {}

  /**
   * @brief Free owned sparse and dense storage.
   */
  void del() {
    if (d != NULL)
      free(d);
    if (s != NULL)
      free(s);
  }

  /**
   * @brief Get sparse vertex ID by sparse index.
   * @param i  Sparse index
   * @return Vertex ID reference
   */
  inline v_id_t& vtx(const v_id_t& i) const { return std::get<0>(s[i]); }

  /**
   * @brief Get sparse vertex data by sparse index.
   * @param i  Sparse index
   * @return Vertex data reference
   */
  inline data& vtx_data(const v_id_t& i) const { return std::get<1>(s[i]); }

  /**
   * @brief Get sparse vertex ID and data by sparse index.
   * @param i  Sparse index
   * @return Tuple of vertex ID and data
   */
  inline tuple<v_id_t, data> vtx_and_data(const v_id_t& i) const { return s[i]; }

  /**
   * @brief Check whether a vertex is active in dense form.
   * @param v  Vertex ID
   * @return True if the vertex is active
   */
  inline bool is_in(const v_id_t& v) const { return std::get<0>(d[v]); }

  /**
   * @brief Get dense vertex data by vertex ID.
   * @param v  Vertex ID
   * @return Vertex data reference
   */
  inline data& ith_data(const v_id_t& v) const { return std::get<1>(d[v]); }

  /**
   * @brief Get a function view over active vertices.
   * @return Function returning Maybe<tuple<vertex, data>>
   */
  auto get_fn_repr() const {
    std::function<may_be<tuple<v_id_t, data>>(const v_id_t&)> fn;
    if (is_dense) {
      fn = [&](const v_id_t& v) -> may_be<tuple<v_id_t, data>> {
        auto ret = may_be<tuple<v_id_t, data>>(make_tuple(v, std::get<1>(d[v])));
        ret.exists = std::get<0>(d[v]);
        return ret;
      };
    } else {
      fn = [&](const v_id_t& i) -> may_be<tuple<v_id_t, data>> {
        return may_be<tuple<v_id_t, data>>(s[i]);
      };
    }
    return fn;
  }

  /**
   * @brief Get the number of active vertices.
   * @return Active vertex count
   */
  long size() const { return m; }

  /**
   * @brief Get the number of vertices in the graph.
   * @return Graph vertex count
   */
  long num_vertices() const { return n; }

  /**
   * @brief Get the number of rows.
   * @return Graph vertex count
   */
  long num_rows() const { return n; }

  /**
   * @brief Get the number of non-zero entries.
   * @return Active vertex count
   */
  long num_non_zeros() const { return m; }

  /**
   * @brief Check whether the subset is empty.
   * @return True if no vertices are active
   */
  bool is_empty() const { return m == 0; }

  /**
   * @brief Check whether the current representation is dense.
   * @return True if dense representation is active
   */
  bool dense() const { return is_dense; }

  /**
   * @brief Switch to sparse representation, building it if needed.
   */
  void to_sparse() {
    if (s == NULL && m > 0) {
      auto f = make_in_imap<D>(n, [&](size_t i) -> tuple<bool, data> { return d[i]; });
      auto out = pbbs::pack_index_and_data<v_id_t, data>(f, n);
      out.alloc = false;
      s = out.s;
      if (out.size() != m) {
        cout << "bad stored value of m" << endl;
        abort();
      }
    }
    is_dense = false;
  }

  /**
   * @brief Switch to dense representation, building it if needed.
   */
  void to_dense() {
    if (d == NULL) {
      d = newA(D, n);
      {
        bw_parallel_for(long i = 0; i < n; i++) std::get<0>(d[i]) = false;
      }
      {
        bw_parallel_for(long i = 0; i < m; i++) d[std::get<0>(s[i])] =
            make_tuple(true, std::get<1>(s[i]));
      }
    }
    is_dense = true;
  }

  S* s;
  D* d;
  size_t n, m;
  bool is_dense;
};

/**
 * @brief Vertex subset specialization without per-vertex data.
 */
template <>
struct vertex_subset_data_t<pbbs::empty> {
  using S = v_id_t;

  /**
   * @brief Construct an empty vertex subset.
   * @param _n  Number of vertices in the graph
   */
  vertex_subset_data_t(size_t _n) : s(NULL), d(NULL), n(_n), m(0), is_dense(0) {}

  /**
   * @brief Construct a sparse vertex subset with one vertex.
   * @param _n  Number of vertices in the graph
   * @param v   Active vertex ID
   */
  vertex_subset_data_t(long _n, v_id_t v) : s(NULL), d(NULL), n(_n), m(1), is_dense(0) {
    s = newA(v_id_t, 1);
    s[0] = v;
  }

  /**
   * @brief Construct a sparse vertex subset.
   * @param _n       Number of vertices in the graph
   * @param _m       Number of active vertices
   * @param indices  Sparse vertex IDs
   */
  vertex_subset_data_t(long _n, long _m, S* indices)
      : s(indices), d(NULL), n(_n), m(_m), is_dense(0) {}

  /**
   * @brief Construct a sparse vertex subset from tuple entries.
   * @param _n       Number of vertices in the graph
   * @param _m       Number of active vertices
   * @param indices  Sparse tuple entries
   */
  vertex_subset_data_t(long _n, long _m, tuple<v_id_t, pbbs::empty>* indices)
      : s((v_id_t*) indices), d(NULL), n(_n), m(_m), is_dense(0) {}

  /**
   * @brief Construct a dense vertex subset with known active count.
   * @param _n  Number of vertices in the graph
   * @param _m  Number of active vertices
   * @param _d  Dense active flags
   */
  vertex_subset_data_t(long _n, long _m, bool* _d) : s(NULL), d(_d), n(_n), m(_m), is_dense(1) {}

  /**
   * @brief Construct a dense vertex subset and compute the active count.
   * @param _n  Number of vertices in the graph
   * @param _d  Dense active flags
   */
  vertex_subset_data_t(long _n, bool* _d) : s(NULL), d(_d), n(_n), m(0), is_dense(1) {
    auto d_map = make_in_imap<size_t>(n, [&](size_t i) { return _d[i]; });
    auto f = [&](size_t i, size_t j) { return i + j; };
    m = pbbs::reduce(d_map, f);
  }

  /**
   * @brief Construct a dense vertex subset from tuple flags and compute count.
   * @param _n  Number of vertices in the graph
   * @param _d  Dense tuple flag entries
   */
  vertex_subset_data_t(long _n, tuple<bool, pbbs::empty>* _d)
      : s(NULL), d((bool*) _d), n(_n), m(0), is_dense(1) {
    auto d_map = make_in_imap<size_t>(n, [&](size_t i) { return get<0>(_d[i]); });
    auto f = [&](size_t i, size_t j) { return i + j; };
    m = pbbs::reduce(d_map, f);
  }

  /**
   * @brief Free owned sparse and dense storage.
   */
  void del() {
    if (d != NULL)
      free(d);
    if (s != NULL)
      free(s);
  }

  /**
   * @brief Get sparse vertex ID by sparse index.
   * @param i  Sparse index
   * @return Vertex ID reference
   */
  inline v_id_t& vtx(const v_id_t& i) const { return s[i]; }

  /**
   * @brief Get empty vertex data by sparse index.
   * @param i  Sparse index
   * @return Empty data marker
   */
  inline pbbs::empty vtx_data(const v_id_t& i) const { return pbbs::empty(); }

  /**
   * @brief Get sparse vertex ID and empty data by sparse index.
   * @param i  Sparse index
   * @return Tuple of vertex ID and empty marker
   */
  inline tuple<v_id_t, pbbs::empty> vtx_and_data(const v_id_t& i) const {
    return make_tuple(s[i], pbbs::empty());
  }

  /**
   * @brief Check whether a vertex is active in dense form.
   * @param v  Vertex ID
   * @return True if the vertex is active
   */
  inline bool is_in(const v_id_t& v) const { return d[v]; }

  /**
   * @brief Get empty vertex data by vertex ID.
   * @param v  Vertex ID
   * @return Empty data marker
   */
  inline pbbs::empty ith_data(const v_id_t& v) const { return pbbs::empty(); }

  /**
   * @brief Get a function view over active vertices.
   * @return Function returning Maybe<tuple<vertex, empty>>
   */
  auto get_fn_repr() const {
    std::function<may_be<tuple<v_id_t, pbbs::empty>>(const v_id_t&)> fn;
    if (is_dense) {
      fn = [&](const v_id_t& v) -> may_be<tuple<v_id_t, pbbs::empty>> {
        auto ret = may_be<tuple<v_id_t, pbbs::empty>>(make_tuple(v, pbbs::empty()));
        ret.exists = d[v];
        return ret;
      };
    } else {
      fn = [&](const v_id_t& i) -> may_be<tuple<v_id_t, pbbs::empty>> {
        return may_be<tuple<v_id_t, pbbs::empty>>(make_tuple(s[i], pbbs::empty()));
      };
    }
    return fn;
  }

  /**
   * @brief Get the number of active vertices.
   * @return Active vertex count
   */
  long size() { return m; }

  /**
   * @brief Get the number of vertices in the graph.
   * @return Graph vertex count
   */
  long num_vertices() { return n; }

  /**
   * @brief Get the number of rows.
   * @return Graph vertex count
   */
  long num_rows() { return n; }

  /**
   * @brief Get the number of non-zero entries.
   * @return Active vertex count
   */
  long num_non_zeros() { return m; }

  /**
   * @brief Check whether the subset is empty.
   * @return True if no vertices are active
   */
  bool is_empty() { return m == 0; }

  /**
   * @brief Check whether the current representation is dense.
   * @return True if dense representation is active
   */
  bool dense() { return is_dense; }

  /**
   * @brief Switch to sparse representation, building it if needed.
   */
  void to_sparse() {
    if (s == NULL && m > 0) {
      auto _d = d;
      auto f = [&](size_t i) { return _d[i]; };
      auto f_in = make_in_imap<bool>(n, f);
      auto out = pbbs::pack_index<v_id_t>(f_in);
      out.alloc = false;
      s = out.s;
      if (out.size() != m) {
        cout << "bad stored value of m" << endl;
        cout << "out.size = " << out.size() << " m = " << m << " n = " << n << endl;
        abort();
      }
    }
    is_dense = false;
  }

  /**
   * @brief Switch to dense representation, building it if needed.
   */
  void to_dense() {
    if (d == NULL) {
      d = newA(bool, n);
      {
        bw_parallel_for(long i = 0; i < n; i++) d[i] = 0;
      }
      {
        bw_parallel_for(long i = 0; i < m; i++) d[s[i]] = 1;
      }
    }
    is_dense = true;
  }

  /**
   * @brief Print active vertices from sparse representation.
   */
  void print_actice_sparse() {
    printf("print_active_sparse\n");
    for (long i = 0; i < m; i++) {
      printf("%d ", s[i]);
    }
    printf("\n");
  }

  /**
   * @brief Print active vertices from dense representation.
   */
  void print_activate_dense() {
    printf("print_activate_dense\n");
    for (long i = 0; i < n; i++) {
      if (d[i])
        printf("%ld ", i);
    }
    printf("\n");
  }

  /**
   * @brief Copy active dense vertices into an output array.
   * @param adj  Output array
   */
  void get_activate_dense(S* adj) {
    long idx = 0;
    for (long i = 0; i < n; i++) {
      if (d[i])
        adj[idx++] = i;
    }
  }

  S* s;
  bool* d;
  size_t n, m;
  bool is_dense;
};

using vertex_subset_t = vertex_subset_data_t<pbbs::empty>;

#endif // BW_GRAPH_VERTEX_SET_H
