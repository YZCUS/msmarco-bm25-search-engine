// Block-based posting list cursor implementation.

#include "inverted_list.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "codec.hpp"

namespace idx::query {

namespace {

std::size_t lower_bound_offset(const std::vector<BlockMeta>& blocks,
                               std::int64_t pos) {
    auto it = std::lower_bound(
        blocks.begin(), blocks.end(), pos,
        [](const BlockMeta& m, std::int64_t v) { return m.start_offset < v; });
    return static_cast<std::size_t>(it - blocks.begin());
}

}  // namespace

InvertedList::InvertedList(const std::uint8_t* base,
                           std::int64_t start_position,
                           std::int64_t bytes_size,
                           int df,
                           const std::vector<BlockMeta>& blocks)
    : base_(base), df_(df), blocks_(&blocks) {
    first_block_index_ = lower_bound_offset(blocks, start_position);
    last_block_index_ = lower_bound_offset(blocks, start_position + bytes_size);

    if (first_block_index_ >= last_block_index_) {
        exhausted_ = true;
        return;
    }
    enter_block(first_block_index_);
}

void InvertedList::enter_block(std::size_t block_index) {
    current_block_index_ = block_index;
    doc_id_cursor_ = 0;
    freq_cursor_ = 0;
}

bool InvertedList::advance_one() {
    while (current_block_index_ < last_block_index_) {
        const BlockMeta& blk = (*blocks_)[current_block_index_];
        if (doc_id_cursor_ >= blk.doc_id_size) {
            // Block consumed; the next block's deltas chain off the running
            // absolute doc_id, which already equals blk.last_doc_id.
            ++current_block_index_;
            if (current_block_index_ >= last_block_index_) break;
            enter_block(current_block_index_);
            continue;
        }

        const std::uint8_t* doc_id_seg = base_ + blk.start_offset;
        const std::uint8_t* freq_seg = base_ + blk.start_offset + blk.doc_id_size;

        std::size_t consumed = 0;
        const auto doc_remaining =
            static_cast<std::size_t>(blk.doc_id_size - doc_id_cursor_);
        const std::uint32_t delta =
            idx::codec::decode_bounded(doc_id_seg + doc_id_cursor_, doc_remaining, consumed);
        doc_id_cursor_ += static_cast<std::int64_t>(consumed);

        if (freq_cursor_ >= blk.freq_size) {
            throw std::runtime_error("inverted_list: truncated frequency block");
        }
        consumed = 0;
        const auto freq_remaining =
            static_cast<std::size_t>(blk.freq_size - freq_cursor_);
        const std::uint32_t f =
            idx::codec::decode_bounded(freq_seg + freq_cursor_, freq_remaining, consumed);
        freq_cursor_ += static_cast<std::int64_t>(consumed);

        running_doc_id_ += static_cast<int>(delta);
        cur_doc_id_ = running_doc_id_;
        cur_freq_ = static_cast<int>(f);
        return true;
    }
    return false;
}

bool InvertedList::advance() {
    if (exhausted_) return false;
    if (!advance_one()) {
        exhausted_ = true;
        positioned_ = false;
        return false;
    }
    positioned_ = true;
    return true;
}

bool InvertedList::advanceTo(int target) {
    if (exhausted_) return false;

    if (positioned_ && cur_doc_id_ >= target) return true;

    // Whole-block skipping: if the next block's last_doc_id is still below
    // target, jump past it without decoding any postings.
    while (current_block_index_ < last_block_index_) {
        const BlockMeta& blk = (*blocks_)[current_block_index_];
        // Only skip blocks we have not started decoding yet.
        if (doc_id_cursor_ == 0 && blk.last_doc_id < target) {
            running_doc_id_ = blk.last_doc_id;
            ++current_block_index_;
            if (current_block_index_ >= last_block_index_) break;
            enter_block(current_block_index_);
            continue;
        }
        break;
    }

    while (advance_one()) {
        if (cur_doc_id_ >= target) {
            positioned_ = true;
            return true;
        }
    }
    exhausted_ = true;
    positioned_ = false;
    return false;
}

}  // namespace idx::query
