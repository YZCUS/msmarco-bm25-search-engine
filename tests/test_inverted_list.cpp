// test_inverted_list: contract for idx::query::InvertedList. The synthetic
// posting buffer is built programmatically through idx::codec so the same
// test exercises whichever codec the build was configured with.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "codec.hpp"
#include "inverted_list.hpp"
#include "posting.hpp"
#include "test_helpers.hpp"

namespace {

struct TinyIndex {
    std::vector<std::uint8_t> bytes;
    std::vector<idx::BlockMeta> blocks;
};

// Build a 2-block index for one term. Returns a buffer with layout:
//   [block_0 doc_ids][block_0 freqs][block_1 doc_ids][block_1 freqs]
// Both blocks are encoded with the active codec (VarByte or Raw32).
TinyIndex make_two_block_index() {
    // Postings: (10,2), (15,1), (42,3) | (100,4), (200,5)
    const std::pair<int, int> b0[] = {{10, 2}, {15, 1}, {42, 3}};
    const std::pair<int, int> b1[] = {{100, 4}, {200, 5}};

    TinyIndex out;
    auto encode_block = [&](auto begin, auto end, int prev_did) {
        const std::size_t mark = out.bytes.size();
        std::vector<std::uint8_t> doc_buf, freq_buf;
        int last_did = prev_did;
        for (auto it = begin; it != end; ++it) {
            const auto& [did, f] = *it;
            idx::codec::encode(static_cast<std::uint32_t>(did - last_did), doc_buf);
            idx::codec::encode(static_cast<std::uint32_t>(f), freq_buf);
            last_did = did;
        }
        const std::int64_t did_size = static_cast<std::int64_t>(doc_buf.size());
        const std::int64_t freq_size = static_cast<std::int64_t>(freq_buf.size());
        out.bytes.insert(out.bytes.end(), doc_buf.begin(), doc_buf.end());
        out.bytes.insert(out.bytes.end(), freq_buf.begin(), freq_buf.end());
        out.blocks.push_back({static_cast<std::int32_t>(last_did), did_size, freq_size,
                              static_cast<std::int64_t>(mark)});
        return last_did;
    };
    int last = 0;
    last = encode_block(std::begin(b0), std::end(b0), last);
    encode_block(std::begin(b1), std::end(b1), last);
    return out;
}

void test_advance_yields_postings_in_order() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  /*df=*/5, t.blocks);

    IDX_CHECK(list.advance()); IDX_CHECK_EQ(list.doc_id(), 10); IDX_CHECK_EQ(list.term_freq(), 2);
    IDX_CHECK(list.advance()); IDX_CHECK_EQ(list.doc_id(), 15); IDX_CHECK_EQ(list.term_freq(), 1);
    IDX_CHECK(list.advance()); IDX_CHECK_EQ(list.doc_id(), 42); IDX_CHECK_EQ(list.term_freq(), 3);
    IDX_CHECK(list.advance()); IDX_CHECK_EQ(list.doc_id(), 100); IDX_CHECK_EQ(list.term_freq(), 4);
    IDX_CHECK(list.advance()); IDX_CHECK_EQ(list.doc_id(), 200); IDX_CHECK_EQ(list.term_freq(), 5);
    IDX_CHECK(!list.advance());
    IDX_CHECK(list.exhausted());
    IDX_CHECK(!list.advance());
}

void test_advanceTo_skips_within_block() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    IDX_CHECK(list.advanceTo(20));
    IDX_CHECK_EQ(list.doc_id(), 42);
    IDX_CHECK_EQ(list.term_freq(), 3);
}

void test_advanceTo_skips_whole_block() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    IDX_CHECK(list.advanceTo(75));
    IDX_CHECK_EQ(list.doc_id(), 100);
    IDX_CHECK_EQ(list.term_freq(), 4);
}

void test_advanceTo_is_idempotent_when_already_satisfied() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    IDX_CHECK(list.advanceTo(15));
    IDX_CHECK_EQ(list.doc_id(), 15);
    IDX_CHECK(list.advanceTo(15));
    IDX_CHECK_EQ(list.doc_id(), 15);
    IDX_CHECK(list.advanceTo(10));
    IDX_CHECK_EQ(list.doc_id(), 15);
}

void test_advanceTo_past_end_returns_false() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    IDX_CHECK(!list.advanceTo(1000));
    IDX_CHECK(list.exhausted());
}

void test_df_reports_value_from_constructor() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    IDX_CHECK_EQ(list.df(), 5);
}

void test_advanceTo_after_partial_iteration() {
    const auto t = make_two_block_index();
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    IDX_CHECK(list.advance());
    IDX_CHECK_EQ(list.doc_id(), 10);
    IDX_CHECK(list.advanceTo(150));
    IDX_CHECK_EQ(list.doc_id(), 200);
    IDX_CHECK_EQ(list.term_freq(), 5);
}

void test_truncated_frequency_block_throws() {
    auto t = make_two_block_index();
    t.blocks[0].freq_size = 0;
    idx::query::InvertedList list(t.bytes.data(), 0,
                                  static_cast<std::int64_t>(t.bytes.size()),
                                  5, t.blocks);
    bool threw = false;
    try {
        (void)list.advance();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    IDX_CHECK(threw);
}

}  // namespace

int main() {
    test_advance_yields_postings_in_order();
    test_advanceTo_skips_within_block();
    test_advanceTo_skips_whole_block();
    test_advanceTo_is_idempotent_when_already_satisfied();
    test_advanceTo_past_end_returns_false();
    test_df_reports_value_from_constructor();
    test_advanceTo_after_partial_iteration();
    test_truncated_frequency_block_throws();
    return idx::testing::report("test_inverted_list");
}
