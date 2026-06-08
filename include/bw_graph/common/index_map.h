#ifndef BW_GRAPH_INDEX_MAP_H
#define BW_GRAPH_INDEX_MAP_H

#include "utils.h"

template <typename E, typename F>
struct in_imap {
  using T = E;
  F f;
  size_t s, e;
  in_imap(size_t n, F f) : f(f), s(0), e(n) {};
  in_imap(size_t s, size_t e, F f) : f(f), s(s), e(e) {};
  inline T operator[](const size_t i) { return f(i + s); }
  inline T operator()(const size_t i) { return f(i + s); }
  in_imap<T, F> cut(size_t ss, size_t ee) { return in_imap<T, F>(s + ss, s + ee, f); }
  in_imap<T, F> slice(size_t ss, size_t ee) { return in_imap<T, F>(s + ss, s + ee, f); }
  size_t size() { return e - s; }
};

// used so second template argument can be inferred
template <class E, class F>
in_imap<E, F> make_in_imap(size_t n, F f) {
  return in_imap<E, F>(n, f);
}

template <typename E, typename F>
struct out_imap {
  using T = E;
  F f;
  size_t s, e;
  out_imap(size_t n, F f) : f(f), s(0), e(n) {};
  out_imap(size_t s, size_t e, F f) : f(f), s(s), e(e) {};
  out_imap<T, F> cut(size_t ss, size_t ee) { return out_imap<T, F>(s + ss, s + ee, f); }
  void update(size_t i, const T& val) { f(i + s, val); }
  size_t size() { return e - s; }
};

// used so second template argument can be inferred
template <class E, class F>
out_imap<E, F> make_out_imap(size_t n, F f) {
  return out_imap<E, F>(n, f);
}

template <typename Iterator>
struct iter_imap {
  using T = typename std::iterator_traits<Iterator>::value_type;
  Iterator s;
  const Iterator e;
  iter_imap(const iter_imap& b) : s(b.s), e(b.e) {}
  iter_imap() {}
  iter_imap(Iterator s, Iterator e) : s(s), e(e) {};
  T& operator[](const size_t i) const { return s[i]; }
  iter_imap<Iterator> cut(size_t ss, size_t ee) { return iter_imap<Iterator>(s + ss, s + ee); }
  void update(size_t i, const T& val) { s[i] = val; }
  size_t size() { return e - s; }
};

// used so template argument can be inferred
template <class Iterator>
iter_imap<Iterator> make_iter_imap(Iterator s, Iterator e) {
  return iter_imap<Iterator>(s, e);
}

template <typename E>
struct array_imap {
  using T = E;
  E *s, *e;
  bool alloc;
  array_imap(const array_imap& b) : s(b.s), e(b.e), alloc(false) {}
  array_imap() : alloc(false) {}
  array_imap(E* s, size_t n, bool alloc = false) : s(s), e(s + n), alloc(alloc) {};
  array_imap(const size_t n) : s(pbbs::new_array_no_init<E>(n)), e(s + n), alloc(true) {};
  template <class F>
  array_imap(const size_t n, F f) : s(pbbs::new_array_no_init<E>(n)), e(s + n), alloc(true) {
    granular_for(i, 0, n, (n > 2000), { s[i] = f(i); });
  };
  ~array_imap() {
    if (alloc) {
      free(s);
    }
  }
  inline E& operator[](const size_t i) const { return s[i]; }
  inline E& operator()(const size_t i) const { return s[i]; }
  inline array_imap<T> cut(size_t ss, size_t ee) { return array_imap<T>(s + ss, ee - ss); }
  inline void update(size_t i, const E& val) { s[i] = val; }
  //  inline void del() { if (alloc) { alloc = false; free(s); }}
  inline size_t size() { return e - s; }
};

// used so template argument can be inferred
template <class E>
array_imap<E> make_array_imap(E* A, size_t n) {
  return array_imap<E>(A, n);
}

#endif // BW_GRAPH_INDEX_MAP_H
