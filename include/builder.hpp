#pragma once

// Builder API: turn a MS MARCO style collection.tsv into the four index
// artefacts read by SearchEngine.
//
//   <output_dir>/final_sorted_index.bin
//   <output_dir>/final_sorted_block_info.bin
//   <output_dir>/final_sorted_lexicon.txt
//   <output_dir>/document_info.txt
//
// The function is exposed as a library entry point so end-to-end tests can
// build a tiny corpus without spawning a subprocess.

#include <cstddef>
#include <filesystem>
#include <string>

namespace idx::build {

struct BuildStats {
    std::size_t docs_processed = 0;
    std::size_t total_postings = 0;
    std::size_t spill_count = 0;
    std::size_t peak_unique_terms = 0;
    std::size_t peak_partial_postings = 0;
    std::size_t peak_partial_bytes_estimate = 0;
    std::size_t final_terms = 0;
    std::size_t final_index_bytes = 0;
};

struct BuildOptions {
    // Approximate number of postings to accumulate in memory before spilling.
    // Each Posting is ~16 bytes including hash-map overhead, so 4M postings
    // is a good 100 MB budget on a developer laptop.
    std::size_t spill_threshold = 4 * 1024 * 1024;

    // Maximum postings packed into a single block on disk. Must match what
    // the search-time block iterator expects.
    int postings_per_block = 128;

    // Optional JSON output for build-time stats. Intended for local benchmark
    // runs; generated files should stay under bench_results/.
    std::filesystem::path stats_json_path;

    // Optional in-process stats sink used by tests and embedding callers.
    BuildStats* stats = nullptr;
};

void build_index(const std::filesystem::path& input_tsv,
                 const std::filesystem::path& output_dir,
                 const BuildOptions& opts = {});

}  // namespace idx::build
