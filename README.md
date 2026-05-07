# RAG-Ready Search Engine Indexer

> Built an inverted index over 8.8M MS MARCO passages with BM25 ranking and
> parallel query execution. VarByte compression cuts posting-store bytes by
> 69.0%; in this run the SlabArena path increased peak RSS by 3.5%, so the
> memory result is reported as a caveat rather than a win.

> Implementation status: P0–P10 benchmark/docs artifacts complete on real
> MS MARCO data, with CTest green under both `IDX_CODEC=VarByte` and
> `IDX_CODEC=Raw32`. No release tag has been cut yet. Full tables:
> [docs/benchmark_results.md](docs/benchmark_results.md). Phase plan / runbook:
> [IMPLEMENTATION.md](IMPLEMENTATION.md). **Repo layout + presentation:**
> [docs/renovation_presentation_plan.md](docs/renovation_presentation_plan.md).

## At a Glance

| Metric                            | Value           |
| --------------------------------- | --------------- |
| Documents indexed                 | 8,841,823       |
| Index size (VarByte)              | 1.00 GB total   |
| Compression vs. Raw32             | 69.0% posting-store reduction |
| Build peak RSS (default → arena)  | 428.00 MB → 443.11 MB (+3.5%) |
| Query latency P50 / P95 / P99     | 2.16 / 6.35 / 8.85 ms at 8 threads |
| QPS @ 8 threads                   | 385.4           |
| MRR@10 (MS MARCO dev)             | 0.1812          |
| nDCG@10 (TREC DL19 / DL20)        | 0.4415 / 0.4976 |
| Recall@1000 (DL19)                | 0.6848          |

Measured locally on Apple M4 Pro / 24 GB RAM / macOS 26.4.1. See
[docs/benchmark_results.md](docs/benchmark_results.md) for full tables and charts.

## Reproduce

```bash
# 1. Fetch MS MARCO Passage v1 + dev / DL19 / DL20 evaluation sets.
bash eval/prepare_msmarco.sh

# 2. Build (Release, VarByte codec).
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# 3. Build the inverted index.
./build/build_index data/collection.tsv data/index_varbyte/

# 4. Compression ablation (VarByte vs Raw32).
bash scripts/build_two_indexes.sh data/collection.tsv

# 5. Memory ablation (default allocator vs SlabArena).
bash bench/run_memory.sh data/collection.tsv

# 6. Latency benchmark across thread counts.
for n in 1 2 4 8; do
    ./build/bench_latency \
        --index data/index_varbyte/final_sorted_index.bin \
        --lexicon data/index_varbyte/final_sorted_lexicon.txt \
        --blocks data/index_varbyte/final_sorted_block_info.bin \
        --doc-info data/index_varbyte/document_info.txt \
        --queries data/queries.dev.small.tsv --threads $n \
        --csv bench_results/latency_t${n}.csv
done

# 7. Ranking metrics.
bash scripts/eval_all.sh data/index_varbyte

# 8. Try the RAG demo (requires Ollama or OPENAI_API_KEY).
python -m rag_demo.rag_demo --q "what is bm25" \
    --search-cli ./build/search_cli \
    --index data/index_varbyte/final_sorted_index.bin \
    --lexicon data/index_varbyte/final_sorted_lexicon.txt \
    --blocks data/index_varbyte/final_sorted_block_info.bin \
    --doc-info data/index_varbyte/document_info.txt \
    --collection data/collection.tsv
```

## Architecture

```mermaid
flowchart LR
    subgraph Build [Build Pipeline]
        TarGz["MS MARCO collection.tsv"] --> Tokenizer
        Tokenizer --> InMem["In-memory partial index<br/>(SlabArena)"]
        InMem --> Spill["Spill to temp_index_*.bin<br/>(VarByte)"]
        Spill --> Merge["External k-way merge"]
        Merge --> Final["final_sorted_index.bin<br/>+ block_info<br/>+ lexicon"]
    end
    subgraph Query [Query Path]
        Q["Query batch"] --> Pool["Thread pool"]
        Pool --> DAAT["DAAT BM25 over<br/>block-iterated lists"]
        DAAT --> TopK["Top-k JSON"]
    end
    Final --> DAAT
    TopK --> RAG["Python RAG demo<br/>(retriever + LLM)"]
    TopK --> Eval["TREC run -> pytrec_eval"]
```

## Layout

```
include/    public headers (varbyte / bm25 / allocator / thread_pool / ...)
src/        executable sources (build_index / search / search_cli)
bench/      latency, compression, index size, memory benchmarks
eval/       MS MARCO download + TREC run writer + pytrec_eval driver
rag_demo/   Python retriever + LLM demo
tests/      unit tests, run via ctest
docs/       benchmark results, charts
scripts/    helper scripts (build_two_indexes, eval_all, plotters)
```

## Design Highlights

- **VarByte vs Raw32 ablation** — same logical postings, different codec, gives
  a clean apples-to-apples compression number.
- **SlabArena allocator** — monotonic slab + std::pmr path is implemented and
  benchmarked; current peak RSS was slightly worse than the default allocator.
- **DAAT BM25 + block-skipping** — block-level metadata enables fast
  conjunctive queries; MaxScore / WAND are tracked under future work.
- **TREC eval pipeline** — produces the same run-file format used by Anserini /
  Pyserini, so results are directly comparable to published baselines.

## Limitations & Future Work

- ASCII-only tokenizer; no stemming, no stop-word removal.
- Block-Max WAND, MaxScore early termination, SIMD-BP128 codec are listed but
  not implemented.
- Single-machine; no sharding / distributed merge.

## License

MIT.
