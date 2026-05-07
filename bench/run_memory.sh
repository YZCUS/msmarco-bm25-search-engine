#!/usr/bin/env bash
# run_memory.sh: build the index twice on the same input, once with the
# decoded vector partial index and once with the compact compressed partial
# index, then write a local JSON comparison under bench_results/.
#
# Usage: bash bench/run_memory.sh <collection.tsv>
set -euo pipefail

INPUT="${1:-data/collection.tsv}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE_BUILD="${ROOT}/build/memory-vector"
COMPACT_BUILD="${ROOT}/build/memory-compact"
BASE_OUT="${ROOT}/data/index_vector"
COMPACT_OUT="${ROOT}/data/index_compact"
LOG_DIR="${ROOT}/bench_results"
JSON_OUT="${LOG_DIR}/memory_comparison.json"

if [[ ! -f "${INPUT}" ]]; then
    echo "[err] input not found: ${INPUT}" >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"
rm -rf "${BASE_OUT}" "${COMPACT_OUT}"
mkdir -p "${BASE_OUT}" "${COMPACT_OUT}"

cmake -B "${BASE_BUILD}" -DCMAKE_BUILD_TYPE=Release -DIDX_BUILDER_MODE=Vector -S "${ROOT}"
cmake --build "${BASE_BUILD}" -j --target build_index

cmake -B "${COMPACT_BUILD}" -DCMAKE_BUILD_TYPE=Release -DIDX_BUILDER_MODE=Compact -S "${ROOT}"
cmake --build "${COMPACT_BUILD}" -j --target build_index

echo "[vector] timing build with decoded posting vectors"
/usr/bin/time -l "${BASE_BUILD}/build_index" "${INPUT}" "${BASE_OUT}" \
    --stats-json "${LOG_DIR}/build_vector_stats.json" 2> "${LOG_DIR}/mem_vector.log"

echo "[compact] timing build with compressed posting buffers"
/usr/bin/time -l "${COMPACT_BUILD}/build_index" "${INPUT}" "${COMPACT_OUT}" \
    --stats-json "${LOG_DIR}/build_compact_stats.json" 2> "${LOG_DIR}/mem_compact.log"

python3 "${ROOT}/scripts/extract_rss.py" \
    --json "${LOG_DIR}/mem_vector.log" "${LOG_DIR}/mem_compact.log" \
    > "${JSON_OUT}"

echo "[done] memory comparison written to ${JSON_OUT}"
cat "${JSON_OUT}"
