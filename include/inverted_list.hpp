#pragma once

// Block-based posting list cursor.
//
// API style follows the standard IR DAAT convention used by Anserini /
// Lucene: the cursor is "unpositioned" right after construction; the first
// call to advance() or advanceTo() decodes the first posting; subsequent
// reads via doc_id() / term_freq() observe the cached current posting.
//
//   advance()           -> move to next posting; returns false on exhaustion
//   advanceTo(target)   -> advance until cur >= target; idempotent if already
//                          positioned with cur >= target
//   doc_id() / term_freq()
//                       -> only valid after a successful advance/advanceTo
//
// On-disk format (per Appendix A of IMPLEMENTATION.md):
//
//   [doc_id_section_block_0][freq_section_block_0]
//   [doc_id_section_block_1][freq_section_block_1]
//   ...
//
//   doc_id sections store delta-encoded VarByte; deltas carry across block
//   boundaries (first delta of block i+1 is taken against block i's last
//   absolute doc_id, NOT zero).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "posting.hpp"

namespace idx::query {

class InvertedList {
public:
    InvertedList(const std::uint8_t* base,
                 std::int64_t start_position,
                 std::int64_t bytes_size,
                 int df,
                 const std::vector<BlockMeta>& blocks);

    bool advance();
    bool advanceTo(int target);

    int doc_id() const noexcept { return cur_doc_id_; }
    int term_freq() const noexcept { return cur_freq_; }
    int df() const noexcept { return df_; }
    bool exhausted() const noexcept { return exhausted_; }
    bool positioned() const noexcept { return positioned_ && !exhausted_; }

private:
    bool advance_one();
    void enter_block(std::size_t block_index);

    const std::uint8_t* base_;
    int df_;
    const std::vector<BlockMeta>* blocks_;

    std::size_t first_block_index_ = 0;
    std::size_t last_block_index_ = 0;     // exclusive
    std::size_t current_block_index_ = 0;

    int running_doc_id_ = 0;   // running absolute doc_id across blocks
    int cur_doc_id_ = 0;       // last decoded absolute doc_id
    int cur_freq_ = 0;

    std::int64_t doc_id_cursor_ = 0;
    std::int64_t freq_cursor_ = 0;

    bool positioned_ = false;
    bool exhausted_ = false;
};

}  // namespace idx::query
