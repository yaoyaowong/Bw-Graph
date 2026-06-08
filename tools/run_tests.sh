#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

usage() {
    echo "Usage: $0 <test-name> [--txn] [args...]"
    echo ""
    echo "Available tests:"
    if compgen -G "${ROOT_DIR}/bw-test/*.cc" > /dev/null; then
        for test_file in "${ROOT_DIR}"/bw-test/*.cc; do
            basename "${test_file}" .cc
        done
    else
        echo "  (none)"
    fi
}

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage
    exit 0
fi

TEST_NAME="$1"
shift

ENABLE_TXN="OFF"
TEST_ARGS=()
for arg in "$@"; do
    if [ "${arg}" = "--txn" ]; then
        ENABLE_TXN="ON"
    else
        TEST_ARGS+=("${arg}")
    fi
done

TEST_SOURCE="${ROOT_DIR}/bw-test/${TEST_NAME}.cc"
TEST_TARGET="bw_test_${TEST_NAME}"
TEST_BIN="${BUILD_DIR}/bw-test/${TEST_NAME}"

if [ ! -f "${TEST_SOURCE}" ]; then
    echo "Unknown bw-test suite: ${TEST_NAME}" >&2
    usage >&2
    exit 2
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBWGRAPH_NEIGHBOR_COMPRESS=OFF \
    -DBW_GRAPH_ENABLE_TRANSACTION="${ENABLE_TXN}"

cmake --build "${BUILD_DIR}" --target "${TEST_TARGET}" -j

echo "=== Running bw-test/${TEST_NAME} ==="
if [ ${#TEST_ARGS[@]} -eq 0 ]; then
    "${TEST_BIN}" "${ROOT_DIR}"
else
    "${TEST_BIN}" "${ROOT_DIR}" "${TEST_ARGS[@]}"
fi
