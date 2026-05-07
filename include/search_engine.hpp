#pragma once

// SearchEngine public API. Real method bodies live in src/search_engine.cpp
// and will be filled in during P1 (bug fixes) and P4 (parallel batching).

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "posting.hpp"

namespace idx::query {

struct SearchOptions {
    int top_k = 10;
    bool conjunctive = false;
    bool fill_passage = false;  // server / CLI sets true to include the source passage
};

struct SearchEnginePaths {
    std::string index_file;
    std::string lexicon_file;
    std::string block_info_file;
    std::string doc_info_file;
    std::string collection_file;  // raw MS MARCO collection.tsv, optional
};

class SearchEngine {
public:
    explicit SearchEngine(const SearchEnginePaths& paths, unsigned threads = 0);
    ~SearchEngine();

    SearchEngine(const SearchEngine&) = delete;
    SearchEngine& operator=(const SearchEngine&) = delete;

    // Single-query convenience wrapper.
    std::vector<SearchResult> search(std::string_view query, SearchOptions opts);

    // Parallel batch path used by the benchmark harness and the RAG demo.
    std::vector<std::vector<SearchResult>>
    search_batch(const std::vector<std::string>& queries, SearchOptions opts);

    int total_docs() const noexcept;
    double avg_doc_length() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace idx::query
