#!/usr/bin/env bash
# build_two_indexes.sh: build the same collection twice, once with VarByte
# and once with Raw32, into separate directories. Then run bench_compression
# to produce docs/compression_table.md.
#
# Usage: bash scripts/build_two_indexes.sh <collection.tsv>
set -euo pipefail

INPUT="${1:-data/collection.tsv}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VAR_BUILD="${ROOT}/build/varbyte"
RAW_BUILD="${ROOT}/build/raw32"
VAR_OUT="${ROOT}/data/index_varbyte"
RAW_OUT="${ROOT}/data/index_raw32"
DOCS_OUT="${ROOT}/docs/compression_table.md"

if [[ ! -f "${INPUT}" ]]; then
    echo "[err] input file not found: ${INPUT}" >&2
    exit 1
fi

cmake -B "${VAR_BUILD}" -DCMAKE_BUILD_TYPE=Release -DIDX_CODEC=VarByte -S "${ROOT}"
cmake --build "${VAR_BUILD}" -j --target build_index bench_compression

cmake -B "${RAW_BUILD}" -DCMAKE_BUILD_TYPE=Release -DIDX_CODEC=Raw32 -S "${ROOT}"
cmake --build "${RAW_BUILD}" -j --target build_index

mkdir -p "${VAR_OUT}" "${RAW_OUT}"
echo "[varbyte] building index..."
"${VAR_BUILD}/build_index" "${INPUT}" "${VAR_OUT}"
echo "[raw32]   building index..."
"${RAW_BUILD}/build_index" "${INPUT}" "${RAW_OUT}"

mkdir -p "$(dirname "${DOCS_OUT}")"
"${VAR_BUILD}/bench_compression" "${VAR_OUT}" "${RAW_OUT}" "${DOCS_OUT}"

echo "[done] markdown table written to ${DOCS_OUT}"
