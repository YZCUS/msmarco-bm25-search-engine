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

struct BuildOptions {
    // Approximate number of postings to accumulate in memory before spilling.
    // Each Posting is ~16 bytes including hash-map overhead, so 4M postings
    // is a good 100 MB budget on a developer laptop.
    std::size_t spill_threshold = 4 * 1024 * 1024;

    // Maximum postings packed into a single block on disk. Must match what
    // the search-time block iterator expects.
    int postings_per_block = 128;
};

void build_index(const std::filesystem::path& input_tsv,
                 const std::filesystem::path& output_dir,
                 const BuildOptions& opts = {});

}  // namespace idx::build
