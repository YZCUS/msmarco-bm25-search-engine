# MS MARCO BM25 Search Engine - Repository Presentation Plan

This document keeps the public repository layout aligned with the README and
the implementation status in `IMPLEMENTATION.md`.

## Repository Layout

```text
msmarco-bm25-search-engine/
|-- .github/
|   `-- workflows/          # CI matrix for VarByte and Raw32 builds
|-- bench/                  # C++ benchmarks for latency, size, and compression
|-- docs/                   # Benchmark tables, charts, and presentation notes
|-- eval/                   # MS MARCO preparation, TREC writer, pytrec_eval driver
|-- include/                # Public C++ headers
|   `-- third_party/        # Vendored single-header dependencies
|-- rag_demo/               # Python retriever and RAG demo
|-- scripts/                # Reproducibility helpers and plotters
|-- src/                    # Builder, inverted-list cursor, search engine, CLI
|-- tests/                  # C++ tests and Python integration tests
|-- IMPLEMENTATION.md       # Completed phase status and resume-safe claims
|-- README.md               # Public overview and reproduction guide
|-- data/                   # Local-only corpus, qrels, and generated indexes
`-- bench_results/          # Local-only latency CSV outputs
```

`data/` and `bench_results/` are intentionally ignored. The repository keeps
small generated documentation artifacts under `docs/`, but not raw runs or
index binaries.

## Presentation Goals

1. The README first screen should map directly to evidence in `docs/`.
2. Headline claims must be measurable: 8.8M passages, BM25 ranking, VarByte
   compression, parallel retrieval latency, and ranking metrics.
3. Negative or inconclusive experiments should not be highlighted in the
   README. Keep them in benchmark notes only when they are useful context.
4. Reproduction commands should start from a clean checkout and never require
   committing MS MARCO data or generated index files.

## Evidence Map

| Claim | Evidence |
| ----- | -------- |
| 8.8M passage index | `README.md`, `docs/benchmark_results.md` |
| 69.0% posting-store reduction | `docs/compression_table.md`, `docs/compression_bar.png` |
| Parallel query execution | `bench/bench_latency.cpp`, `docs/qps_vs_threads.png` |
| Ranking evaluation | `eval/`, `docs/metrics_*.json` |
| RAG-ready retrieval path | `src/search_cli.cpp`, `rag_demo/`, `tests/test_rag_pipeline.py` |

## Naming

Public project name: `MS MARCO BM25 Search Engine`

GitHub repository: `https://github.com/YZCUS/msmarco-bm25-search-engine`
