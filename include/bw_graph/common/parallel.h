#ifndef BW_GRAPH_COMMON_PARALLEL_H
#define BW_GRAPH_COMMON_PARALLEL_H

#if defined(CILK)
#include <cilk/cilk.h>
#define bw_parallel_main main
#define bw_parallel_for cilk_for
#define bw_parallel_for_1 _Pragma("cilk_grainsize = 1") cilk_for
#define bw_parallel_for_256 _Pragma("cilk_grainsize = 256") cilk_for
#define bw_cilk_for cilk_for
#define bw_cilk_spawn
#define bw_cilk_sync
#include <cilk/cilk_api.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
static int getWorkers() { return __cilkrts_get_nworkers(); }
static void setWorkers(int n) {
  __cilkrts_end_cilk();
  //__cilkrts_init();
  std::stringstream ss;
  ss << n;
  if (0 != __cilkrts_set_param("nworkers", ss.str().c_str())) {
    std::cerr << "failed to set worker count!" << std::endl;
    std::abort();
  }
}

// intel cilk+
#elif defined(CILKP)
#include <cilk/cilk.h>
#define bw_parallel_for cilk_for
#define bw_parallel_main main
#define bw_parallel_for_1 _Pragma("cilk grainsize = 1") cilk_for
#define bw_parallel_for_256 _Pragma("cilk grainsize = 256") cilk_for
#define bw_cilk_for cilk_for
#define bw_cilk_spawn
#define bw_cilk_sync
#include <cilk/cilk_api.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
static int getWorkers() { return __cilkrts_get_nworkers(); }
static void setWorkers(int n) {
  __cilkrts_end_cilk();
  //__cilkrts_init();
  std::stringstream ss;
  ss << n;
  if (0 != __cilkrts_set_param("nworkers", ss.str().c_str())) {
    std::cerr << "failed to set worker count!" << std::endl;
    std::abort();
  }
}

// openmp
#elif defined(OPENMP)
#include <omp.h>
#define bw_cilk_spawn
#define bw_cilk_sync
#define bw_parallel_main main
#define bw_parallel_for _Pragma("omp parallel for") for
#define bw_parallel_for_1 _Pragma("omp parallel for schedule (static,1)") for
#define bw_parallel_for_256 _Pragma("omp parallel for schedule (static,256)") for
#define bw_cilk_for for
static int getWorkers() { return omp_get_max_threads(); }
static int getWorkersID() { return omp_get_thread_num(); }
static void setWorkers(int n) { omp_set_num_threads(n); }

// c++
#else
#define bw_cilk_spawn
#define bw_cilk_sync
#define bw_parallel_main main
#define bw_parallel_for for
#define bw_parallel_for_1 for
#define bw_parallel_for_256 for
#define bw_cilk_for for
static int getWorkers() { return 1; }
static void setWorkers(int n) { return; }

#endif

#include <limits.h>

#if defined(LONG)
typedef long intT;
typedef unsigned long uintT;
#define INT_T_MAX LONG_MAX
#define UINT_T_MAX ULONG_MAX
#else
typedef int intT;
typedef unsigned int uintT;
#define INT_T_MAX INT_MAX
#define UINT_T_MAX UINT_MAX
#endif

// edges store 32-bit quantities unless EDGELONG is defined
#if defined(EDGELONG)
typedef long intE;
typedef unsigned long uintE;
#define INT_E_MAX LONG_MAX
#define UINT_E_MAX ULONG_MAX
#else
typedef int intE;
typedef unsigned int uintE;
#define INT_E_MAX INT_MAX
#define UINT_E_MAX UINT_MAX
#endif

#endif // BW_GRAPH_COMMON_PARALLEL_H
