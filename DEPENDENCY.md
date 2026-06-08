# BwGraph Dependencies

## Build Tools

| Tool | Version | Notes |
|------|---------|-------|
| CMake | ≥ 3.14 | Build system |
| C++ Compiler | C++20 | GCC (with `-fcoroutines`) or Clang |
| pkg-config | any | Required on macOS for KaMinPar lookup |

---

## Required Libraries

These are hard requirements — CMake aborts with `FATAL_ERROR` if any is missing.

### gflags
Google command-line flag parsing library.
```bash
# macOS
brew install gflags

# Ubuntu / Debian
sudo apt install libgflags-dev
```

### glog
Google logging library.
```bash
# macOS
brew install glog

# Ubuntu / Debian
sudo apt install libgoogle-glog-dev
```

### yaml-cpp
YAML parser used by `load_yaml_config()` to read all `config/*.yaml` files.
```bash
# macOS
brew install yaml-cpp

# Ubuntu / Debian
sudo apt install libyaml-cpp-dev
```

### folly
Facebook's open-source C++ library (concurrent data structures, lock-free utilities).  
On macOS the CMake fallback searches `/usr/local`; on Linux it also checks `/usr/local`.
```bash
# macOS
brew install folly

# Ubuntu / Debian
sudo apt install libfolly-dev
```

> **Note:** folly itself transitively requires `gflags`, `glog`, `double-conversion`, `libfmt`, `libevent`, and `boost`. Installing via the package manager pulls these in automatically.

### RocksDB
Embedded key-value store.
```bash
# macOS
brew install rocksdb

# Ubuntu / Debian
sudo apt install librocksdb-dev
```

### nlohmann_json
Header-only JSON library.
```bash
# macOS
brew install nlohmann-json

# Ubuntu / Debian
sudo apt install nlohmann-json3-dev
```

### indicators
Header-only progress-bar library (p-ranav/indicators).  
Not available in most package managers; install from source if the system package is missing:
```bash
# macOS
brew install indicators

# From source (any platform)
cd /tmp && git clone https://github.com/p-ranav/indicators.git \
  && cd indicators && mkdir build && cd build \
  && cmake .. && sudo make install
```

### KaMinPar
Shared-memory graph partitioner. CMake looks for it at the local path  
`/Users/wangsongyao/Library/CppLib/KaMinPar` (configurable via `KAMINPAR_LOCAL_PATH`).  
Build and install from source:
```bash
git clone https://github.com/KaHIP/KaMinPar.git ~/Library/CppLib/KaMinPar
cd ~/Library/CppLib/KaMinPar
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DKAMINPAR_64BIT_NODE_IDS=ON \
         -DKAMINPAR_64BIT_EDGE_IDS=ON \
         -DKAMINPAR_64BIT_WEIGHTS=ON
make -j$(nproc)
```

### GTest (Google Test)
Unit testing framework.
```bash
# macOS
brew install googletest

# Ubuntu / Debian
sudo apt install libgtest-dev
```

### POSIX Threads
Provided by the OS on all supported platforms (`Threads::Threads` via CMake). No separate install needed.

---

## Optional Libraries

These are detected at configure time; the build succeeds without them but some features are disabled.

| Library | CMake variable | Effect when absent |
|---------|---------------|-------------------|
| **OpenMP** (`libomp`) | `OpenMP_CXX_FOUND` | Parallel algorithm variants (`BW_GRAPH_*_PARALLEL`) fall back to single-threaded |
| **ZLIB** | `ZLIB_FOUND` | `HAVE_ZLIB` not defined; compressed I/O paths disabled |

```bash
# macOS — OpenMP (Clang does not bundle it)
brew install libomp

# macOS — ZLIB (usually pre-installed via Xcode CLT)
brew install zlib

# Ubuntu / Debian
sudo apt install libomp-dev zlib1g-dev
```

---

## Quick-install Summary

### macOS (Homebrew)
```bash
brew install cmake gflags glog yaml-cpp folly rocksdb nlohmann-json \
             googletest indicators libomp
# KaMinPar must be built from source (see above)
```

### Ubuntu / Debian
```bash
sudo apt update
sudo apt install cmake build-essential pkg-config \
                 libgflags-dev libgoogle-glog-dev libyaml-cpp-dev \
                 libfolly-dev librocksdb-dev nlohmann-json3-dev \
                 libgtest-dev libomp-dev zlib1g-dev
# indicators and KaMinPar must be built from source (see above)
```
