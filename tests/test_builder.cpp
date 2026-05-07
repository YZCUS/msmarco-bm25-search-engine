// test_builder: end-to-end coverage of idx::build::build_index by writing a
// tiny collection to disk, building the index, and querying it through
// idx::query::SearchEngine. This guards the on-disk format contract between
// the writer and the reader.

#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "builder.hpp"
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
        dir = base / ("idx_builder_test_" + std::to_string(rng()));
        if (fs::create_directory(dir)) return dir;
    }
    throw std::runtime_error("make_temp_dir: failed");
}

void write_tiny_collection(const fs::path& path) {
    std::ofstream out(path);
    out << "0\tfoo bar baz\n"
        << "1\tfoo foo\n"
        << "2\tbar bar bar\n"
        << "3\tqux\n"
        << "4\tfoo bar\n";
}

idx::query::SearchEnginePaths paths_for(const fs::path& dir,
                                        const fs::path& collection) {
    return {
        (dir / "final_sorted_index.bin").string(),
        (dir / "final_sorted_lexicon.txt").string(),
        (dir / "final_sorted_block_info.bin").string(),
        (dir / "document_info.txt").string(),
        collection.string(),
    };
}

std::set<int> doc_ids_of(const std::vector<idx::SearchResult>& results) {
    std::set<int> out;
    for (const auto& r : results) out.insert(r.doc_id);
    return out;
}

void test_build_then_search_roundtrip() {
    auto dir = make_temp_dir();
    const auto coll = dir / "collection.tsv";
    write_tiny_collection(coll);

    idx::build::BuildOptions opts;
    opts.spill_threshold = 3;     // force at least one spill -> exercises merge path
    opts.postings_per_block = 2;  // exercises multi-block path
    idx::build::build_index(coll, dir, opts);

    idx::query::SearchEngine engine(paths_for(dir, coll));
    IDX_CHECK_EQ(engine.total_docs(), 5);
    IDX_CHECK_NEAR(engine.avg_doc_length(), 2.2, 1e-9);

    {
        const auto results = engine.search("foo", {.top_k = 10});
        IDX_CHECK_EQ(results.size(), 3u);
        const auto ids = doc_ids_of(results);
        IDX_CHECK(ids.count(0) == 1);
        IDX_CHECK(ids.count(1) == 1);
        IDX_CHECK(ids.count(4) == 1);
        // doc 1: "foo foo" -> highest tf for "foo"
        IDX_CHECK_EQ(results.front().doc_id, 1);
    }
    {
        const auto results = engine.search("foo bar",
                                           {.top_k = 10, .conjunctive = true});
        IDX_CHECK_EQ(results.size(), 2u);
        const auto ids = doc_ids_of(results);
        IDX_CHECK(ids.count(0) == 1);
        IDX_CHECK(ids.count(4) == 1);
    }
    {
        const auto results = engine.search("zzzzz", {.top_k = 10});
        IDX_CHECK(results.empty());
    }

    fs::remove_all(dir);
}

void test_passage_retrieval_when_collection_provided() {
    auto dir = make_temp_dir();
    const auto coll = dir / "collection.tsv";
    write_tiny_collection(coll);
    idx::build::build_index(coll, dir);

    idx::query::SearchEngine engine(paths_for(dir, coll));
    auto results = engine.search("qux", {.top_k = 1, .fill_passage = true});
    IDX_CHECK_EQ(results.size(), 1u);
    IDX_CHECK_EQ(results.front().doc_id, 3);
    IDX_CHECK_EQ(results.front().passage, std::string{"qux"});

    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_build_then_search_roundtrip();
    test_passage_retrieval_when_collection_provided();
    return idx::testing::report("test_builder");
}
