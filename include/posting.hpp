#pragma once

// Common types shared between the index builder and the search engine.
// Keeping them in a single header guarantees layout consistency across the
// two binaries.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace idx {

// A single (delta-encoded) doc_id paired with its term frequency.
struct Posting {
    int doc_id_delta;  // delta to previous doc_id (absolute for the first one)
    int frequency;     // raw term frequency in the document
};

// Per-term lexicon entry as persisted on disk.
// `df` (document frequency) replaces the misleading `posting_number` name
// from the original prototype.
struct LexiconEntry {
    int term_id;
    int df;
    std::int64_t start_position;  // byte offset into final_sorted_index.bin
    std::int64_t bytes_size;      // total posting bytes for this term
};

// One block-level metadata record.
//   - last_doc_id, doc_id_size, freq_size are persisted on disk.
//   - start_offset is computed at load time as the running sum of
//     (doc_id_size + freq_size) and is NOT serialized.
struct BlockMeta {
    std::int32_t last_doc_id;   // absolute doc_id of last posting in block
    std::int64_t doc_id_size;   // bytes of the doc_id varbyte segment
    std::int64_t freq_size;     // bytes of the freq varbyte segment
    std::int64_t start_offset;  // cumulative byte offset in the index file
};

// Search-time hit returned to the caller.
struct SearchResult {
    int doc_id;
    double score;
    int rank;             // 1-based rank within the result list
    std::string passage;  // optional: filled in only by server / CLI paths
};

}  // namespace idx
