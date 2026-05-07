# MS MARCO BM25 Search Engine - Implementation Plan

This file records the completed implementation phases and the evidence used by
the README. It is intentionally concise so the repository can be read as a
portfolio project, not as an internal scratchpad.

## Scope

The project implements a single-machine C++23 BM25 search engine for MS MARCO
Passage v1, plus benchmark, evaluation, and RAG demo tooling.

Headline claims supported by this repository:

- Built an inverted index over 8,841,823 MS MARCO passages.
- Implemented BM25 ranking over block-encoded posting lists.
- Added parallel batch query execution with a thread pool.
- Reduced posting-store bytes by 69.0% with VarByte versus Raw32 encoding.
- Evaluated retrieval with latency, index-size, compression, MRR, nDCG, and
  Recall@1000 artifacts.
- Exposed a JSONL `search_cli --server` path used by the Python RAG demo.

## Phase Status

| Phase | Status | Evidence |
| ----- | ------ | -------- |
| P0 Environment and data setup | Complete | MS MARCO Passage v1 indexed locally. |
| P1 Refactor and bug fixes | Complete | BM25, tokenizer, codec, inverted-list, and search engine tests. |
| P2 VarByte vs Raw32 codec ablation | Complete | `docs/compression_table.md`, `docs/benchmark_results.md`. |
| P3 Allocator benchmark harness | Complete | `bench/run_memory.sh`. |
| P4 Parallel query execution | Complete | `SearchEngine::search_batch`, `bench/bench_latency.cpp`. |
| P5 CLI and JSONL server | Complete | `src/search_cli.cpp`, `tests/test_rag_pipeline.py`. |
| P6 Benchmarks | Complete | Latency, compression, and index-size artifacts in `docs/`. |
| P7 MS MARCO / TREC metrics | Complete | `docs/metrics_dev.json`, `docs/metrics_dl19.json`, `docs/metrics_dl20.json`. |
| P8 Python RAG demo | Complete | `rag_demo/`, JSONL subprocess integration test. |
| P9 Tests and CI | Complete | CTest suite and GitHub Actions matrix for VarByte and Raw32. |
| P10 README and presentation docs | Complete | README, benchmark report, chart artifacts. |

## Key Results

| Metric | Value |
| ------ | ----- |
| Documents indexed | 8,841,823 |
| VarByte total index size | 1.00 GB |
| VarByte posting-store reduction vs Raw32 | 69.0% |
| 8-thread QPS | 385.4 |
| 8-thread P50 / P95 / P99 | 2.16 / 6.35 / 8.85 ms |
| MS MARCO dev MRR@10 | 0.1812 |
| DL19 / DL20 nDCG@10 | 0.4415 / 0.4976 |

Hardware: Apple M4 Pro, 24 GB RAM, macOS 26.4.1, Apple clang 17.0.0.

## Verification Commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

cmake -B build-review-raw32 -DIDX_CODEC=Raw32 -DCMAKE_BUILD_TYPE=Release
cmake --build build-review-raw32 -j
ctest --test-dir build-review-raw32 --output-on-failure
```

Additional release checks:

```bash
git diff --check
bash -n scripts/eval_all.sh
bash -n bench/run_memory.sh
```

## Resume-Safe Claims

Use these claims directly:

- Built a C++23 BM25 search engine indexing 8.8M MS MARCO passages.
- Reduced posting storage by 69.0% with delta-encoded VarByte postings versus
  a Raw32 baseline.
- Added parallel batch retrieval reaching 385.4 QPS at 8 threads on Apple M4
  Pro, with P95 latency of 6.35 ms.
- Built a reproducible evaluation pipeline for MRR@10, nDCG@10, and
  Recall@1000 using TREC run files and `pytrec_eval`.
- Exposed a JSONL retrieval server consumed by a Python RAG demo.

## Current Limitations

- ASCII-only tokenizer with query-time stop-word filtering.
- Single-machine build and query path; no sharding or distributed merge.
- No Block-Max WAND, MaxScore early termination, or SIMD-BP128 codec.

## Large-File Policy

Do not commit generated data or local artifacts:

- `data/`
- `build/`, `build-*`, `build*/`
- `bench_results/`
- `runs/`
- `.venv/`
- `final_sorted_*`, `document_info.txt`, `temp_index_*.bin`, `*.bin`

Before publishing, verify staged files with:

```bash
git diff --cached --name-status
git diff --cached --name-only --diff-filter=ACMR -z | xargs -0 du -h | sort -h | tail -20
```
