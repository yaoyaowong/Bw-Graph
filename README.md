# Bw-Graph: An Efficient Graph Storage System Harmonizing Topology-Aware Tree with Paged CSR

This repository contains the source code implementation of **Bw-Graph**, a graph storage system presented in our paper "Bw-Graph: An Efficient Graph Storage System Harmonizing Topology-Aware Tree with Paged CSR".

## Building and Running

### Build Script Usage

The project provides a convenient build script with the following options:
```bash
bash ./tools/build.sh [OPTIONS]
```

**Available Options:**
- `-t <mode>`: Build type (`debug` or `release`, default: `release`)
- `-c`: Clean build (remove existing build directory)
- `--no-test`: Skip running tests after build
- `-h`: Display help message

**Examples:**
```bash
# Release build with clean and tests (recommended)
bash ./tools/build.sh -t release -c

# Debug build without tests
bash ./tools/build.sh -t debug --no-test

# Quick rebuild without cleaning
bash ./tools/build.sh
```

### Quick Start

1. **Clone the repository:**
```bash
   git clone <repository-url> Bw-Graph/
   cd Bw-Graph/
```

2. **Install dependencies:**
   Follow the installation instructions for your operating system above.

3. **Configure KaMinPar path:**
   Edit `CMakeLists.txt` and update the `KAMINPAR_LOCAL_PATH` variable.

4. **Build the project:**
```bash
   bash ./tools/build.sh -t release -c
```
   
   This command will:
   - Clean any existing build artifacts
   - Configure CMake in release mode
   - Compile the project
   - Automatically run all tests

5. **Run the example:**
```bash
   ./build/bin/bfs ./config/example.yaml --algo map --benchmark

   # MySQL MTR Style Testing
   bash ./tools/run_tests.sh read_neighbor_clone
```
   
   If the program executes without errors, the system is successfully built and configured.

### Manual Build (Alternative)

If you prefer manual control over the build process:
```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
ctest
```

## Citation

Our paper:

```bibtex
@article{10.1145/3802025,
  author    = {Wang, Songyao and Wang, Chaokun and Li, Zecheng and Zhang, Aoqi},
  title     = {Bw-Graph: An Efficient Graph Storage System Harmonizing Topology-Aware Tree with Paged CSR},
  year      = {2026},
  issue_date = {June 2026},
  publisher = {Association for Computing Machinery},
  address   = {New York, NY, USA},
  volume    = {4},
  number    = {3},
  url       = {https://doi.org/10.1145/3802025},
  doi       = {10.1145/3802025},
  journal   = {Proc. ACM Manag. Data},
  month     = may,
  articleno = {148},
  numpages  = {27},
  keywords  = {graph storage system, topology-aware organization, bw-tree, csr representation}
}
```

## Notes

> **This codebase is actively maintained.** Bugs will be fixed and stability improvements will be applied over time. Please always check for the latest version and re-clone or pull as needed.

- Bw-Graph will be integrated into our graph database **MonacGraph** in the near future.

