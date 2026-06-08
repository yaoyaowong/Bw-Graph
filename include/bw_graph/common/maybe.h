#ifndef BW_GRAPH_COMMON_MAYBE_H
#define BW_GRAPH_COMMON_MAYBE_H

#include "type.h"

#include <cstdint>
#include <tuple>

using namespace std;

template <class T>
struct may_be {
  T t;
  bool exists;
  may_be(const T& _t) : t(_t), exists(true) {}
  may_be() : exists(false) {}
};

inline const may_be<tuple<v_id_t, v_id_t>> wrap(const v_id_t& l, const v_id_t& r) {
  auto t = may_be<tuple<v_id_t, v_id_t>>(make_tuple(l, r));
  t.exists = (l != UINT32_MAX) && (r != UINT32_MAX);
  return t;
}

template <class L, class R>
inline const may_be<tuple<L, R>> wrap(const L& l, const may_be<R>& r) {
  auto t = may_be<tuple<L, R>>(make_tuple(l, get_tp(r)));
  t.exists = r.exists;
  return t;
}

template <class L, class R>
inline may_be<tuple<L, R>> wrap(const may_be<L>& l, const R& r) {
  auto t = may_be<tuple<L, R>>(make_tuple(get_tp(l), r));
  t.exists = l.exists;
  return t;
}

template <class L, class R>
inline may_be<tuple<L, R>> wrap(const may_be<L>& l, const may_be<R>& r) {
  auto t = may_be<tuple<L, R>>(make_tuple(get_tp(l), get_tp(r)));
  t.exists = l.exists && r.exists;
  return t;
}

template <class T>
inline bool is_some(const may_be<T>& m) {
  return m.exists;
}

template <class T>
inline T get_tp(const may_be<T>& m) {
  return m.t;
}

#endif // BW_GRAPH_COMMON_MAYBE_H
