#!/usr/bin/env bash
# eval_all.sh: produce TREC run files for MS MARCO dev / DL19 / DL20 then
# evaluate them with pytrec_eval. Assumes the index has already been built
# under ./build and the data files have been fetched via
# eval/prepare_msmarco.sh.
#
# Usage:
#   bash scripts/eval_all.sh [index_dir]
#   INDEX_DIR=data/index_varbyte bash scripts/eval_all.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
INDEX="${INDEX_DIR:-${1:-${ROOT}/data/index_varbyte}}"
DATA="${ROOT}/data"
RUNS="${ROOT}/runs"
DOCS="${ROOT}/docs"
PYTHON="${PYTHON:-${ROOT}/.venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    PYTHON="python3"
fi

mkdir -p "${RUNS}" "${DOCS}"

INDEX_FLAGS=(--index "${INDEX}/final_sorted_index.bin"
             --lexicon "${INDEX}/final_sorted_lexicon.txt"
             --blocks "${INDEX}/final_sorted_block_info.bin"
             --doc-info "${INDEX}/document_info.txt")

for artifact in final_sorted_index.bin final_sorted_lexicon.txt final_sorted_block_info.bin document_info.txt; do
    if [[ ! -f "${INDEX}/${artifact}" ]]; then
        echo "[err] missing index artifact: ${INDEX}/${artifact}" >&2
        exit 2
    fi
done

run_set() {
    local name="$1"; local queries="$2"; local qrels="$3"
    if [[ ! -f "${queries}" || ! -f "${qrels}" ]]; then
        echo "[skip] ${name}: missing ${queries} or ${qrels}"
        return
    fi
    "${BUILD}/trec_run_writer" "${INDEX_FLAGS[@]}" \
        --queries "${queries}" --out "${RUNS}/${name}.run" \
        --threads 8 --top-k 1000
    "${PYTHON}" "${ROOT}/eval/run_eval.py" "${qrels}" "${RUNS}/${name}.run" \
        --out "${DOCS}/metrics_${name}.json"
}

run_set dev  "${DATA}/queries.dev.small.tsv"        "${DATA}/qrels.dev.small.tsv"
run_set dl19 "${DATA}/msmarco-test2019-queries.tsv" "${DATA}/2019qrels-pass.txt"
run_set dl20 "${DATA}/msmarco-test2020-queries.tsv" "${DATA}/2020qrels-pass.txt"

echo "[done] metrics in ${DOCS}/metrics_*.json"
