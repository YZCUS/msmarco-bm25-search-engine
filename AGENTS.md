# Repository Guidelines

## Project Structure & Module Organization

This repository builds a C++23 inverted-index search engine with a small Python RAG demo.

- `include/`: public headers for codecs, BM25, allocator, builder, search engine, tokenizer, and threading.
- `src/`: C++ implementation and CLI entry points such as `build_index` and `search_cli`.
- `tests/`: C++ unit tests plus `test_rag_pipeline.py`; CMake registers them with CTest.
- `bench/`: compression, latency, index-size, and memory benchmarks.
- `eval/`: MS MARCO preparation, TREC run writing, and pytrec_eval driver.
- `scripts/`: helper scripts for benchmark/eval workflows.
- `rag_demo/`: Python retriever and LLM demo.
- `docs/`: benchmark and index-size notes.

Keep generated data, indexes, and benchmark outputs out of source directories.

## Build, Test, and Development Commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Use the commands above for the normal release build and full registered test suite.

```bash
cmake -B build-asan -DIDX_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Use the ASAN/UBSAN build for memory-safety changes. To compare codecs, configure with `-DIDX_CODEC=VarByte` or `-DIDX_CODEC=Raw32`. Build an index with `./build/build_index data/collection.tsv data/index/`. Run the RAG demo dependencies with `pip install -r rag_demo/requirements.txt`.

## Coding Style & Naming Conventions

Match the existing C++ style: 4-space indentation, C++23, `#pragma once` headers, lower_snake_case functions and files, and namespaces under `idx::...`. Prefer small functions and direct data structures over speculative abstraction. Keep comments for non-obvious format, ownership, or algorithm details only.

## Testing Guidelines

Add focused tests in `tests/` for behavior changes. Name C++ tests `test_<feature>.cpp` and register new executables with `add_test()` in `CMakeLists.txt`. For index-format changes, include a round-trip builder/search test. Run `ctest --test-dir build --output-on-failure` before committing; use ASAN for allocator, codec, mmap, or threading work.

## Commit & Pull Request Guidelines

Recent commits use short, direct subjects such as `fix idf computing`. Keep commit messages imperative and scoped, for example `fix block iterator bounds`. Pull requests should summarize the change, list verification commands, note codec/build variants tested, and include benchmark or evaluation output when performance or ranking behavior changes. Link related issues and call out any required MS MARCO data setup.

## Agent-Specific Instructions

Make surgical changes only. State assumptions for ambiguous requests, prefer the simplest working implementation, and verify with the narrowest relevant build or test command.
