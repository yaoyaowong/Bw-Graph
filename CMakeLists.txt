cmake_minimum_required(VERSION 3.14)
project(BwGraph)

option(BWGRAPH_NEIGHBOR_COMPRESS "Build with PTV-compressed CSR neighbors" OFF)
option(BW_GRAPH_ENABLE_TRANSACTION "Enable MVCC transaction support" OFF)

include(CheckCXXCompilerFlag)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING
      "Choose the type of build: Debug Release RelWithDebInfo MinSizeRel"
      FORCE)
  message(STATUS "Build: type not specified, defaulting to Debug")
endif()

# Build configuration
message(STATUS "Build: type = ${CMAKE_BUILD_TYPE}")
message(STATUS "Build: neighbor compression = ${BWGRAPH_NEIGHBOR_COMPRESS}")
message(STATUS "Build: transaction support = ${BW_GRAPH_ENABLE_TRANSACTION}")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(STATUS "Build: Debug mode enabled")
  add_definitions(-DBWGRAPH_DEBUG_MODE)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS "Build: Release mode enabled")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(UNIX)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -fcoroutines")
  endif()
endif()

# Platform configuration
if(APPLE)
  set(HOMEBREW_PREFIX /opt/homebrew)
  list(APPEND CMAKE_PREFIX_PATH ${HOMEBREW_PREFIX})

  set(gflags_ROOT ${HOMEBREW_PREFIX}/opt/gflags)
  set(glog_ROOT ${HOMEBREW_PREFIX}/opt/glog)
  set(folly_ROOT ${HOMEBREW_PREFIX}/opt/folly)
  set(rocksdb_ROOT ${HOMEBREW_PREFIX}/opt/rocksdb)
elseif(UNIX AND NOT APPLE)
  list(APPEND CMAKE_PREFIX_PATH /usr/local)
  list(APPEND CMAKE_PREFIX_PATH /usr)

  set(gflags_ROOT /usr/local)
  set(glog_ROOT /usr/local)
  set(folly_ROOT /usr/local)
  set(rocksdb_ROOT /usr/local)
endif()

# Core system packages
find_package(Threads REQUIRED)

if(APPLE)
  set(OpenMP_ROOT "${HOMEBREW_PREFIX}/opt/libomp")
  list(APPEND CMAKE_PREFIX_PATH "${OpenMP_ROOT}")
endif()

find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
  message(STATUS "Dependency OpenMP: found")
else()
  message(WARNING "Dependency OpenMP: not found, building without OpenMP")
endif()

find_package(ZLIB)
if(ZLIB_FOUND)
  message(STATUS "Dependency ZLIB: found")
  add_definitions(-DHAVE_ZLIB)
else()
  message(STATUS "Dependency ZLIB: not found")
endif()

# Third-party dependencies
# gflags
find_package(gflags QUIET)
if(NOT gflags_FOUND)
  find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h
            PATHS /usr/local/include /usr/include)
  find_library(GFLAGS_LIBRARY
               NAMES gflags
               PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu)
  if(GFLAGS_INCLUDE_DIR AND GFLAGS_LIBRARY)
    add_library(gflags UNKNOWN IMPORTED)
    set_target_properties(gflags PROPERTIES
      IMPORTED_LOCATION ${GFLAGS_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${GFLAGS_INCLUDE_DIR}
    )
    message(STATUS
            "Dependency gflags: found (library=${GFLAGS_LIBRARY}, include=${GFLAGS_INCLUDE_DIR})")
  else()
    message(FATAL_ERROR "Dependency gflags: not found")
  endif()
else()
  message(STATUS "Dependency gflags: found via find_package")
endif()

# glog
# Prefer find_package(CONFIG) so that glog 0.6+ exports GLOG_EXPORT correctly
# via its installed CMake config. Manual find_path/find_library does NOT set
# the required compile definitions and triggers
# "<glog/logging.h> was not included correctly" errors.
find_package(glog CONFIG QUIET)

if(glog_FOUND)
  message(STATUS "Dependency glog: found via find_package CONFIG")
else()
  message(STATUS "Dependency glog: CONFIG not found, falling back to manual search")
  find_path(GLOG_INCLUDE_DIR glog/logging.h
            PATHS /usr/include /usr/local/include)
  find_library(GLOG_LIBRARY
               NAMES glog
               PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib /usr/lib)

  if(GLOG_INCLUDE_DIR AND GLOG_LIBRARY)
    add_library(glog::glog UNKNOWN IMPORTED)
    set_target_properties(glog::glog PROPERTIES
      IMPORTED_LOCATION ${GLOG_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${GLOG_INCLUDE_DIR}
    )
    message(STATUS
            "Dependency glog: found manually (library=${GLOG_LIBRARY}, include=${GLOG_INCLUDE_DIR})")
  else()
    message(FATAL_ERROR
            "Dependency glog: not found. Install via 'brew install glog' or 'apt install libgoogle-glog-dev'.")
  endif()
endif()

# KaMinPar environment
if(APPLE)
  set(ENV{PKG_CONFIG_PATH}
      "${HOMEBREW_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  set(Sparsehash_INCLUDE_DIR "${HOMEBREW_PREFIX}/include")
  find_program(PKG_CONFIG_EXECUTABLE pkg-config PATHS ${HOMEBREW_PREFIX}/bin)
endif()

# KaMinPar
# Default install location: $HOME/Library/CppLib/KaMinPar (macOS) or $HOME/.local/lib/KaMinPar (Linux).
# Override with: cmake -DKAMINPAR_LOCAL_PATH=/your/path ..
if(NOT DEFINED KAMINPAR_LOCAL_PATH OR KAMINPAR_LOCAL_PATH STREQUAL "")
  if(APPLE)
    set(KAMINPAR_LOCAL_PATH "$ENV{HOME}/Library/CppLib/KaMinPar")
  else()
    set(KAMINPAR_LOCAL_PATH "$ENV{HOME}/.local/lib/KaMinPar")
  endif()
endif()
message(STATUS "Dependency KaMinPar: local path = ${KAMINPAR_LOCAL_PATH}")

set(CMAKE_PREFIX_PATH "${KAMINPAR_LOCAL_PATH};${CMAKE_PREFIX_PATH}")
find_package(KaMinPar QUIET)

if(KaMinPar_FOUND)
  if(TARGET KaMinPar::KaMinPar)
    set(KAMINPAR_TARGET KaMinPar::KaMinPar)
  elseif(TARGET kaminpar)
    set(KAMINPAR_TARGET kaminpar)
  else()
    message(FATAL_ERROR "Dependency KaMinPar: found but no known target available")
  endif()
  message(STATUS "Dependency KaMinPar: found via find_package (target=${KAMINPAR_TARGET})")
else()
  message(STATUS "Dependency KaMinPar: find_package failed, trying manual search")

  set(POSSIBLE_PATHS
      "${KAMINPAR_LOCAL_PATH}"
      "${KAMINPAR_LOCAL_PATH}/build"
      "${KAMINPAR_LOCAL_PATH}/install")

  find_path(KAMINPAR_INCLUDE_DIR
            NAMES kaminpar-shm/kaminpar.h
            PATHS ${POSSIBLE_PATHS}
            PATH_SUFFIXES include kaminpar-shm
            NO_DEFAULT_PATH)

  find_library(KAMINPAR_LIBRARY
               NAMES kaminpar KaMinPar kaminpar-shm
               PATHS ${POSSIBLE_PATHS}
               PATH_SUFFIXES lib build/kaminpar-shm build/apps
               NO_DEFAULT_PATH)

  if(KAMINPAR_INCLUDE_DIR AND KAMINPAR_LIBRARY)
    add_library(KaMinPar::KaMinPar UNKNOWN IMPORTED)
    set_target_properties(KaMinPar::KaMinPar PROPERTIES
      IMPORTED_LOCATION ${KAMINPAR_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${KAMINPAR_INCLUDE_DIR}
    )
    set(KAMINPAR_TARGET KaMinPar::KaMinPar)
    message(STATUS
            "Dependency KaMinPar: found manually (library=${KAMINPAR_LIBRARY}, include=${KAMINPAR_INCLUDE_DIR})")
  else()
    message(FATAL_ERROR "Dependency KaMinPar: not found in ${KAMINPAR_LOCAL_PATH}")
  endif()
endif()

message(STATUS "Dependency KaMinPar: using target ${KAMINPAR_TARGET}")
set(KAMINPAR_64BIT_NODE_IDS ON)
set(KAMINPAR_64BIT_EDGE_IDS ON)
set(KAMINPAR_64BIT_WEIGHT_IDS ON)

# GTest
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  if(APPLE)
    set(GTEST_INCLUDE_DIR ${HOMEBREW_PREFIX}/include)
    set(GTEST_LIB_DIR ${HOMEBREW_PREFIX}/lib)
  else()
    set(GTEST_INCLUDE_DIR /usr/local/include)
    set(GTEST_LIB_DIR /usr/local/lib)
  endif()

  find_library(GTEST_LIBRARY
               NAMES gtest
               PATHS ${GTEST_LIB_DIR} /usr/lib /usr/lib/x86_64-linux-gnu
               REQUIRED)

  find_library(GTEST_MAIN_LIBRARY
               NAMES gtest_main
               PATHS ${GTEST_LIB_DIR} /usr/lib /usr/lib/x86_64-linux-gnu
               REQUIRED)

  if(GTEST_LIBRARY AND GTEST_MAIN_LIBRARY)
    add_library(GTest::gtest UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES
      IMPORTED_LOCATION ${GTEST_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${GTEST_INCLUDE_DIR}
      INTERFACE_LINK_LIBRARIES "Threads::Threads"
    )

    add_library(GTest::gtest_main UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest_main PROPERTIES
      IMPORTED_LOCATION ${GTEST_MAIN_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${GTEST_INCLUDE_DIR}
      INTERFACE_LINK_LIBRARIES "GTest::gtest;Threads::Threads"
    )

    message(STATUS
            "Dependency GTest: found manually (gtest=${GTEST_LIBRARY}, gtest_main=${GTEST_MAIN_LIBRARY}, include=${GTEST_INCLUDE_DIR})")
  else()
    message(FATAL_ERROR
            "Dependency GTest: not found. Make sure googletest is installed.")
  endif()
else()
  message(STATUS "Dependency GTest: found via find_package")
endif()

# folly
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

find_package(folly QUIET)
if(NOT folly_FOUND)
  if(APPLE)
    set(FOLLY_INCLUDE_DIR /usr/local/include)
    set(FOLLY_LIB_DIR /usr/local/lib)
  else()
    set(FOLLY_INCLUDE_DIR /usr/local/include)
    set(FOLLY_LIB_DIR /usr/local/lib)
  endif()

  find_library(FOLLY_LIBRARY
               NAMES folly
               PATHS ${FOLLY_LIB_DIR}
               NO_DEFAULT_PATH
               REQUIRED)

  add_library(Folly::folly UNKNOWN IMPORTED)
  set_target_properties(Folly::folly PROPERTIES
    IMPORTED_LOCATION ${FOLLY_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${FOLLY_INCLUDE_DIR}
  )

  message(STATUS
          "Dependency folly: found manually (library=${FOLLY_LIBRARY}, include=${FOLLY_INCLUDE_DIR})")
else()
  message(STATUS "Dependency folly: found via find_package")
endif()

# SIMDCompressionAndIntersection (Linux only, optional)
if(UNIX AND NOT APPLE)
  find_path(SIMDCOMP_INCLUDE_DIR
    NAMES codecfactory.h
    PATHS /usr/local/include/SIMDCompressionAndIntersection
          /usr/include/SIMDCompressionAndIntersection
  )
  find_library(SIMDCOMP_LIBRARY
    NAMES SIMDCompressionAndIntersection
    PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
  )

  if(SIMDCOMP_INCLUDE_DIR AND SIMDCOMP_LIBRARY)
    add_library(SIMDCompressionAndIntersection UNKNOWN IMPORTED)
    set_target_properties(SIMDCompressionAndIntersection PROPERTIES
      IMPORTED_LOCATION ${SIMDCOMP_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${SIMDCOMP_INCLUDE_DIR}
    )
    set(SIMDCOMP_TARGET SIMDCompressionAndIntersection)
    set(USE_PFORDELTA ON)
    message(STATUS "Dependency SIMDCompressionAndIntersection: found "
                   "(library=${SIMDCOMP_LIBRARY}, include=${SIMDCOMP_INCLUDE_DIR}), "
                   "enabling PForDelta")
  else()
    set(USE_PFORDELTA OFF)
    message(STATUS "Dependency SIMDCompressionAndIntersection: not found, "
                   "using folly fallback")
  endif()
else()
  set(USE_PFORDELTA OFF)
endif()

# yaml-cpp
find_package(yaml-cpp REQUIRED)
if(NOT yaml-cpp_FOUND)
  if(APPLE)
    set(YAML_CPP_INCLUDE_DIR ${HOMEBREW_PREFIX}/include)
    set(YAML_CPP_LIB_DIR ${HOMEBREW_PREFIX}/lib)
  else()
    set(YAML_CPP_INCLUDE_DIR /usr/local/include)
    set(YAML_CPP_LIB_DIR /usr/local/lib)
  endif()

  find_library(YAML_CPP_LIBRARY
               NAMES yaml-cpp
               PATHS ${YAML_CPP_LIB_DIR}
               NO_DEFAULT_PATH
               REQUIRED)

  add_library(YamlCpp::yaml-cpp UNKNOWN IMPORTED)
  set_target_properties(YamlCpp::yaml-cpp PROPERTIES
    IMPORTED_LOCATION ${YAML_CPP_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${YAML_CPP_INCLUDE_DIR}
  )

  message(STATUS
          "Dependency yaml-cpp: found manually (library=${YAML_CPP_LIBRARY}, include=${YAML_CPP_INCLUDE_DIR})")
else()
  message(STATUS "Dependency yaml-cpp: found via find_package")
endif()

# indicators
find_package(indicators QUIET)
if(NOT indicators_FOUND)
  message(STATUS "Dependency indicators: find_package failed, trying manual search")

  if(APPLE)
    set(INDICATORS_INCLUDE_DIR ${HOMEBREW_PREFIX}/include)
  else()
    set(INDICATORS_INCLUDE_DIR /usr/local/include)
  endif()

  find_path(INDICATORS_HEADER_PATH
            NAMES indicators/progress_bar.hpp
            PATHS ${INDICATORS_INCLUDE_DIR}
            NO_DEFAULT_PATH)

  if(INDICATORS_HEADER_PATH)
    set(INDICATORS_INCLUDE_DIR ${INDICATORS_HEADER_PATH})

    add_library(indicators::indicators INTERFACE IMPORTED)
    set_target_properties(indicators::indicators PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES ${INDICATORS_INCLUDE_DIR}
    )

    message(STATUS
            "Dependency indicators: found manually (include=${INDICATORS_INCLUDE_DIR})")
  else()
    message(FATAL_ERROR "Dependency indicators: not found. Please install it:
    cd /tmp && git clone https://github.com/p-ranav/indicators.git &&
    cd indicators && mkdir build && cd build && cmake .. && sudo make install")
  endif()
else()
  message(STATUS "Dependency indicators: found via find_package")
endif()

# RocksDB
find_package(RocksDB QUIET)
if(NOT RocksDB_FOUND)
  if(APPLE)
    set(ROCKSDB_INCLUDE_DIR ${HOMEBREW_PREFIX}/include)
    set(ROCKSDB_LIB_DIR ${HOMEBREW_PREFIX}/lib)
  else()
    set(ROCKSDB_INCLUDE_DIR /usr/local/include)
    set(ROCKSDB_LIB_DIR /usr/local/lib)
  endif()

  find_library(ROCKSDB_LIBRARY
               NAMES rocksdb
               PATHS ${ROCKSDB_LIB_DIR}
               NO_DEFAULT_PATH
               REQUIRED)

  add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
  set_target_properties(RocksDB::rocksdb PROPERTIES
    IMPORTED_LOCATION ${ROCKSDB_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${ROCKSDB_INCLUDE_DIR}
  )

  message(STATUS
          "Dependency RocksDB: found manually (library=${ROCKSDB_LIBRARY}, include=${ROCKSDB_INCLUDE_DIR})")
else()
  message(STATUS "Dependency RocksDB: found via find_package")
endif()

# nlohmann_json
find_package(nlohmann_json REQUIRED)
if(NOT nlohmann_json_FOUND)
  if(APPLE)
    set(NLOHMANN_JSON_INCLUDE_DIR ${HOMEBREW_PREFIX}/include)
    set(NLOHMANN_JSON_LIB_DIR ${HOMEBREW_PREFIX}/lib)
  else()
    set(NLOHMANN_JSON_INCLUDE_DIR /usr/local/include)
    set(NLOHMANN_JSON_LIB_DIR /usr/local/lib)
  endif()

  find_library(NLOHMANN_JSON_LIBRARY
               NAMES nlohmann_json
               PATHS ${NLOHMANN_JSON_LIB_DIR}
               NO_DEFAULT_PATH
               REQUIRED)

  add_library(nlohmann_json::nlohmann_json UNKNOWN IMPORTED)
  set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    IMPORTED_LOCATION ${NLOHMANN_JSON_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${NLOHMANN_JSON_INCLUDE_DIR}
  )

  message(STATUS
          "Dependency nlohmann_json: found manually (library=${NLOHMANN_JSON_LIBRARY}, include=${NLOHMANN_JSON_INCLUDE_DIR})")
else()
  message(STATUS "Dependency nlohmann_json: found via find_package")
endif()

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.cc")
add_library(bwgraph ${SOURCES})

if(BWGRAPH_NEIGHBOR_COMPRESS)
  target_compile_definitions(bwgraph PUBLIC BWGRAPH_NEIGHBOR_COMPRESS)
endif()

if(BW_GRAPH_ENABLE_TRANSACTION)
  target_compile_definitions(bwgraph PUBLIC BW_GRAPH_ENABLE_TRANSACTION)
endif()

target_include_directories(bwgraph PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(bwgraph PUBLIC
  ${KAMINPAR_TARGET}
  Folly::folly
  gflags
  glog::glog
  RocksDB::rocksdb
  Threads::Threads
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json
  $<$<BOOL:${OpenMP_CXX_FOUND}>:OpenMP::OpenMP_CXX>
)

if(USE_PFORDELTA)
  target_compile_definitions(bwgraph PUBLIC USE_PFORDELTA)
  target_link_libraries(bwgraph PUBLIC ${SIMDCOMP_TARGET})
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    check_cxx_compiler_flag("-mssse3" BWGRAPH_COMPILER_SUPPORTS_MSSSE3)
    if(BWGRAPH_COMPILER_SUPPORTS_MSSSE3)
      target_compile_options(bwgraph PUBLIC -mssse3)
    else()
      message(WARNING "PForDelta is enabled, but the compiler does not support -mssse3")
    endif()
  endif()
endif()

target_compile_options(bwgraph PUBLIC
  $<$<CONFIG:Debug>:-g -fno-eliminate-unused-debug-types -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function>
  $<$<CONFIG:Release>:-O3 -DNDEBUG>
  $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
  $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
)

file(GLOB BIN_SOURCES "bin/*.cc")
foreach(BIN_FILE ${BIN_SOURCES})
  get_filename_component(BIN_NAME ${BIN_FILE} NAME_WE)

  add_executable(${BIN_NAME} ${BIN_FILE})
  target_link_libraries(${BIN_NAME} PRIVATE bwgraph)

  set_target_properties(${BIN_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )

  message(STATUS "Target executable: ${BIN_NAME} (${BIN_FILE})")
endforeach()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bw-test)
  file(GLOB BW_TEST_SOURCES "bw-test/*.cc")
  foreach(BW_TEST_FILE ${BW_TEST_SOURCES})
    get_filename_component(BW_TEST_NAME ${BW_TEST_FILE} NAME_WE)
    set(BW_TEST_TARGET "bw_test_${BW_TEST_NAME}")

    add_executable(${BW_TEST_TARGET} ${BW_TEST_FILE})
    target_link_libraries(${BW_TEST_TARGET} PRIVATE bwgraph)
    target_include_directories(${BW_TEST_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    set_target_properties(${BW_TEST_TARGET} PROPERTIES
      OUTPUT_NAME ${BW_TEST_NAME}
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bw-test
    )

    message(STATUS "Bw-test suite: ${BW_TEST_NAME} (${BW_TEST_FILE})")
  endforeach()
endif()

# Tests
enable_testing()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test)
  add_subdirectory(test)
else()
  message(WARNING "Tests: directory not found at ${CMAKE_CURRENT_SOURCE_DIR}/test")
endif()
