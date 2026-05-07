// test_search_engine: end-to-end coverage of idx::query::SearchEngine using
// a hand-crafted 5-document index.
//
// Synthetic dataset:
//   doc 0: "foo bar baz"  (length 3)
//   doc 1: "foo foo"      (length 2)
//   doc 2: "bar bar bar"  (length 3)
//   doc 3: "qux"          (length 1)
//   doc 4: "foo bar"      (length 2)
//
// Postings (sorted lexicographically by term):
//   bar -> {(0,1), (2,3), (4,1)}, df = 3
//   baz -> {(0,1)},               df = 1
//   foo -> {(0,1), (1,2), (4,1)}, df = 3
//   qux -> {(3,1)},               df = 1
//
// Expectations encoded in tests:
//   - Disjunctive search for "foo" returns docs {0, 1, 4} only.
//   - Conjunctive search for "foo bar" returns docs {0, 4} only.
//   - top_k truncates correctly.
//   - search_batch produces one result vector per query.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "index_fixtures.hpp"
#include "search_engine.hpp"
#include "test_helpers.hpp"

namespace {

namespace fs = std::filesystem;

fs::path make_temp_dir() {
    auto base = fs::temp_directory_path();
    std::random_device rd;
    std::mt19937_64 rng(rd());
    fs::path dir;
    for (int i = 0; i < 10; ++i) {
        dir = base / ("idx_test_" + std::to_string(rng()));
        if (fs::create_directory(dir)) return dir;
    }
    throw std::runtime_error("make_temp_dir: failed after retries");
}

idx::query::SearchEnginePaths paths_for(const fs::path& dir) {
    return {
        (dir / "final_sorted_index.bin").string(),
        (dir / "final_sorted_lexicon.txt").string(),
        (dir / "final_sorted_block_info.bin").string(),
        (dir / "document_info.txt").string(),
        (dir / "collection.tsv").string(),
    };
}

void populate_tiny_corpus(const fs::path& dir) {
    using idx::testing::TinyDoc;
    using idx::testing::TinyTerm;

    const std::vector<TinyDoc> docs = {
        {3, "foo bar baz"},
        {2, "foo foo"},
        {3, "bar bar bar"},
        {1, "qux"},
        {2, "foo bar"},
    };
    std::vector<TinyTerm> terms = {
        {"bar", {{0, 1}, {2, 3}, {4, 1}}},
        {"baz", {{0, 1}}},
        {"foo", {{0, 1}, {1, 2}, {4, 1}}},
        {"qux", {{3, 1}}},
    };
    idx::testing::write_tiny_index(dir, docs, std::move(terms));
}

std::set<int> doc_ids_of(const std::vector<idx::SearchResult>& results) {
    std::set<int> out;
    for (const auto& r : results) out.insert(r.doc_id);
    return out;
}

void test_disjunctive_returns_all_matching_docs() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir));

    auto results = engine.search("foo", {.top_k = 10, .conjunctive = false});
    IDX_CHECK_EQ(results.size(), 3u);
    const auto ids = doc_ids_of(results);
    IDX_CHECK(ids.count(0) == 1);
    IDX_CHECK(ids.count(1) == 1);
    IDX_CHECK(ids.count(4) == 1);

    // Results are sorted by score descending. Doc 1 has the highest tf for
    // "foo" (freq 2 in a length-2 doc), so it must out-rank docs 0 and 4.
    IDX_CHECK_EQ(results.front().doc_id, 1);
    IDX_CHECK(results.front().score > results.back().score);
    IDX_CHECK_EQ(results.front().rank, 1);

    fs::remove_all(dir);
}

void test_conjunctive_only_intersection() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir));

    auto results = engine.search("foo bar", {.top_k = 10, .conjunctive = true});
    IDX_CHECK_EQ(results.size(), 2u);
    const auto ids = doc_ids_of(results);
    IDX_CHECK(ids.count(0) == 1);
    IDX_CHECK(ids.count(4) == 1);

    fs::remove_all(dir);
}

void test_top_k_truncates() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir));

    auto results = engine.search("foo", {.top_k = 2, .conjunctive = false});
    IDX_CHECK_EQ(results.size(), 2u);

    fs::remove_all(dir);
}

void test_non_positive_top_k_returns_empty() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir));

    IDX_CHECK(engine.search("foo", {.top_k = 0}).empty());
    IDX_CHECK(engine.search("foo", {.top_k = -1}).empty());

    fs::remove_all(dir);
}

void test_unknown_term_yields_empty_results() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir));

    auto results = engine.search("zzzzz", {.top_k = 10, .conjunctive = false});
    IDX_CHECK(results.empty());

    fs::remove_all(dir);
}

void test_search_batch_returns_per_query_results() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir), /*threads=*/2);

    auto batch = engine.search_batch({"foo", "qux"},
                                     {.top_k = 5, .conjunctive = false});
    IDX_CHECK_EQ(batch.size(), 2u);
    IDX_CHECK_EQ(batch[0].size(), 3u);  // foo: docs 0, 1, 4
    IDX_CHECK_EQ(batch[1].size(), 1u);  // qux: doc 3
    IDX_CHECK_EQ(batch[1].front().doc_id, 3);

    fs::remove_all(dir);
}

void test_engine_reports_stats() {
    auto dir = make_temp_dir();
    populate_tiny_corpus(dir);
    idx::query::SearchEngine engine(paths_for(dir));

    IDX_CHECK_EQ(engine.total_docs(), 5);
    // avgdl = (3 + 2 + 3 + 1 + 2) / 5 = 2.2
    IDX_CHECK_NEAR(engine.avg_doc_length(), 2.2, 1e-9);

    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_disjunctive_returns_all_matching_docs();
    test_conjunctive_only_intersection();
    test_top_k_truncates();
    test_non_positive_top_k_returns_empty();
    test_unknown_term_yields_empty_results();
    test_search_batch_returns_per_query_results();
    test_engine_reports_stats();
    return idx::testing::report("test_search_engine");
}
