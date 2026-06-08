#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${PROJECT_ROOT}/config/example.yaml"

run_example() {
    local name="$1"
    local binary="$2"

    echo "Running ${name}..."
    "${PROJECT_ROOT}/${binary}" "${CONFIG_FILE}"
    echo "Finished ${name}."
    echo ""
}

echo "Starting example graph workloads."
echo ""

run_example "BFS" "build/bin/bfs"
run_example "WCC" "build/bin/wcc"
run_example "PageRank" "build/bin/pagerank"

echo "All example graph workloads completed successfully."
