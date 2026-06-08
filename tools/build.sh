#!/bin/bash

# build.sh - Build script for BwGraph project
# Usage: ./build.sh [-t|--type debug|release] [-c|--clean] [-j|--jobs N] [--compress] [--txn] [-h|--help]

set -e  # Exit on any error

# Default values
BUILD_TYPE="Debug"  # Default to Debug mode
CLEAN_BUILD=false
RUN_TESTS=true
NEIGHBOR_COMPRESS=false
ENABLE_TXN=false

# Detect system type and set appropriate CPU core count
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    JOBS=$(sysctl -n hw.ncpu)
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Linux
    JOBS=$(nproc)
else
    # Fallback for other systems
    JOBS=4
    echo "Warning: Unknown system type, defaulting to 4 parallel jobs"
fi

# Function to show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -t, --type TYPE     Build type: debug|release|relwithdebinfo|minsizerel (default: debug)"
    echo "  -c, --clean         Clean build directory before building"
    echo "  -j, --jobs N        Number of parallel jobs (default: auto-detected)"
    echo "  --compress          Build with PTV-compressed CSR neighbors (default: off)"
    echo "  --txn               Enable transaction support (default: off)"
    echo "  --no-tests          Skip running tests"
    echo "  -h, --help          Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                  # Build in debug mode (default)"
    echo "  $0 -t release       # Build in release mode"
    echo "  $0 -c -t debug      # Clean build in debug mode"
    echo "  $0 -j 4 -t release  # Build release with 4 parallel jobs"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--type)
            case $2 in
                debug|Debug|DEBUG)
                    BUILD_TYPE="Debug"
                    ;;
                release|Release|RELEASE)
                    BUILD_TYPE="Release"
                    ;;
                relwithdebinfo|RelWithDebInfo|RELWITHDEBINFO)
                    BUILD_TYPE="RelWithDebInfo"
                    ;;
                minsizerel|MinSizeRel|MINSIZEREL)
                    BUILD_TYPE="MinSizeRel"
                    ;;
                *)
                    echo "Error: Invalid build type '$2'"
                    echo "Valid types: debug, release, relwithdebinfo, minsizerel"
                    exit 1
                    ;;
            esac
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -j|--jobs)
            JOBS=$2
            shift 2
            ;;
        --no-tests)
            RUN_TESTS=false
            shift
            ;;
        --compress)
            NEIGHBOR_COMPRESS=true
            shift
            ;;
        --txn)
            ENABLE_TXN=true
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option '$1'"
            show_usage
            exit 1
            ;;
    esac
done

echo "=== BwGraph Build Script ==="
echo "Build type: $BUILD_TYPE"
echo "Parallel jobs: $JOBS"
echo "Clean build: $CLEAN_BUILD"
echo "Run tests: $RUN_TESTS"
echo "Neighbor compression: $NEIGHBOR_COMPRESS"
echo "Transaction support: $ENABLE_TXN"
echo ""

if [ "$NEIGHBOR_COMPRESS" = true ]; then
    CMAKE_NEIGHBOR_COMPRESS="ON"
else
    CMAKE_NEIGHBOR_COMPRESS="OFF"
fi

if [ "$ENABLE_TXN" = true ]; then
    CMAKE_ENABLE_TXN="ON"
else
    CMAKE_ENABLE_TXN="OFF"
fi

# Clean build directory if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo "Cleaning build directory..."
    rm -rf build
fi

# Check if build directory exists
if [ -d "build" ]; then
    echo "Using existing build directory..."
    cd build
    
    # Check if we need to reconfigure (build type changed)
    if [ -f "CMakeCache.txt" ]; then
        CURRENT_BUILD_TYPE=$(grep "CMAKE_BUILD_TYPE:STRING=" CMakeCache.txt | cut -d'=' -f2 2>/dev/null || echo "")
        CURRENT_NEIGHBOR_COMPRESS=$(grep "BWGRAPH_NEIGHBOR_COMPRESS:BOOL=" CMakeCache.txt | cut -d'=' -f2 2>/dev/null || echo "")
        CURRENT_ENABLE_TXN=$(grep "BW_GRAPH_ENABLE_TRANSACTION:BOOL=" CMakeCache.txt | cut -d'=' -f2 2>/dev/null || echo "")
        if [ "$CURRENT_BUILD_TYPE" != "$BUILD_TYPE" ] || [ "$CURRENT_NEIGHBOR_COMPRESS" != "$CMAKE_NEIGHBOR_COMPRESS" ] || [ "$CURRENT_ENABLE_TXN" != "$CMAKE_ENABLE_TXN" ]; then
            echo "Build configuration changed, reconfiguring..."
            cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DBWGRAPH_NEIGHBOR_COMPRESS=$CMAKE_NEIGHBOR_COMPRESS -DBW_GRAPH_ENABLE_TRANSACTION=$CMAKE_ENABLE_TXN ..
        else
            echo "Build type unchanged, skipping cmake configuration..."
        fi
    else
        echo "No CMakeCache.txt found, running cmake configuration..."
        cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DBWGRAPH_NEIGHBOR_COMPRESS=$CMAKE_NEIGHBOR_COMPRESS -DBW_GRAPH_ENABLE_TRANSACTION=$CMAKE_ENABLE_TXN ..
    fi
else
    echo "Creating new build directory..."
    mkdir build
    cd build
    
    # Run cmake configuration for new build directory
    echo "Configuring project with CMake..."
    cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DBWGRAPH_NEIGHBOR_COMPRESS=$CMAKE_NEIGHBOR_COMPRESS -DBW_GRAPH_ENABLE_TRANSACTION=$CMAKE_ENABLE_TXN ..
fi

# Build the project
echo "Building project with $JOBS parallel jobs..."
make -j$JOBS

# Run tests if enabled
if [ "$RUN_TESTS" = true ]; then
    echo "Running tests..."
    if ! ctest --output-on-failure; then
        echo "Warning: Some tests failed"
        exit 1
    fi
else
    echo "Skipping tests (--no-tests specified)"
fi

echo ""
echo "=== Build completed successfully! ==="
echo "Build type: $BUILD_TYPE"
echo "Neighbor compression: $NEIGHBOR_COMPRESS"
echo "Transaction support: $ENABLE_TXN"
echo "Executables are in: $(pwd)/bin/"
echo ""

# Show some useful information
if [ "$BUILD_TYPE" = "Debug" ]; then
    echo "Debug build completed. You can now use debuggers:"
    echo "  GDB: gdb ./bin/your_executable"
    echo "  LLDB: lldb ./bin/your_executable"
elif [ "$BUILD_TYPE" = "Release" ]; then
    echo "Release build completed with optimizations enabled."
fi
