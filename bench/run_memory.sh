#!/usr/bin/env bash
# run_memory.sh: build the index twice on the same input — once with the
# default-allocator baseline, once with SlabArena + std::pmr — and emit a
# markdown table of peak RSS to docs/memory_table.md.
#
# Usage: bash bench/run_memory.sh <collection.tsv>
set -euo pipefail

INPUT="${1:-data/collection.tsv}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE_BUILD="${ROOT}/build/baseline"
ARENA_BUILD="${ROOT}/build/arena"
BASE_OUT="${ROOT}/data/index_baseline"
ARENA_OUT="${ROOT}/data/index_arena"
LOG_DIR="${ROOT}/bench_results"
DOCS_OUT="${ROOT}/docs/memory_table.md"

if [[ ! -f "${INPUT}" ]]; then
    echo "[err] input not found: ${INPUT}" >&2
    exit 1
fi

mkdir -p "${LOG_DIR}" "${BASE_OUT}" "${ARENA_OUT}"

cmake -B "${BASE_BUILD}" -DCMAKE_BUILD_TYPE=Release -DIDX_BUILDER_BASELINE=ON -S "${ROOT}"
cmake --build "${BASE_BUILD}" -j --target build_index

cmake -B "${ARENA_BUILD}" -DCMAKE_BUILD_TYPE=Release -DIDX_BUILDER_BASELINE=OFF -S "${ROOT}"
cmake --build "${ARENA_BUILD}" -j --target build_index

echo "[baseline] timing build with default allocator"
/usr/bin/time -l "${BASE_BUILD}/build_index" "${INPUT}" "${BASE_OUT}" 2> "${LOG_DIR}/mem_baseline.log"

echo "[arena] timing build with SlabArena"
/usr/bin/time -l "${ARENA_BUILD}/build_index" "${INPUT}" "${ARENA_OUT}" 2> "${LOG_DIR}/mem_arena.log"

mkdir -p "$(dirname "${DOCS_OUT}")"
python3 "${ROOT}/scripts/extract_rss.py" \
    "${LOG_DIR}/mem_baseline.log" "${LOG_DIR}/mem_arena.log" > "${DOCS_OUT}"

echo "[done] memory comparison written to ${DOCS_OUT}"
cat "${DOCS_OUT}"
