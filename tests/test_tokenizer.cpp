// test_tokenizer: pin down the ASCII fast-path tokenizer behavior so the
// indexer and the query path agree on which character sequences are tokens.

#include <string>
#include <string_view>
#include <vector>

#include "test_helpers.hpp"
#include "tokenizer.hpp"

namespace {

std::vector<std::string> tok(std::string_view s) {
    std::vector<std::string> out;
    idx::token::tokenize(s, out);
    return out;
}

void test_lowercases_letters() {
    const auto t = tok("Hello WORLD");
    IDX_CHECK_EQ(t.size(), 2u);
    IDX_CHECK_EQ(t[0], std::string{"hello"});
    IDX_CHECK_EQ(t[1], std::string{"world"});
}

void test_keeps_digits_within_token() {
    const auto t = tok("BM25 retrieval");
    IDX_CHECK_EQ(t.size(), 2u);
    IDX_CHECK_EQ(t[0], std::string{"bm25"});
    IDX_CHECK_EQ(t[1], std::string{"retrieval"});
}

void test_punctuation_splits_tokens() {
    const auto t = tok("hello, world! it's me.");
    IDX_CHECK_EQ(t.size(), 5u);
    IDX_CHECK_EQ(t[0], std::string{"hello"});
    IDX_CHECK_EQ(t[1], std::string{"world"});
    IDX_CHECK_EQ(t[2], std::string{"it"});
    IDX_CHECK_EQ(t[3], std::string{"s"});
    IDX_CHECK_EQ(t[4], std::string{"me"});
}

void test_html_residue_is_dropped() {
    // MS MARCO has HTML residue like "&amp;" and "<p>"; tokenizer drops the
    // angle brackets / ampersand / semicolon but keeps the alphabetic body.
    // Both the opening and the closing "p" survive as tokens since the
    // ASCII fast path does not look at surrounding context.
    const auto t = tok("<p>foo &amp; bar</p>");
    IDX_CHECK_EQ(t.size(), 5u);
    IDX_CHECK_EQ(t[0], std::string{"p"});
    IDX_CHECK_EQ(t[1], std::string{"foo"});
    IDX_CHECK_EQ(t[2], std::string{"amp"});
    IDX_CHECK_EQ(t[3], std::string{"bar"});
    IDX_CHECK_EQ(t[4], std::string{"p"});
}

void test_non_ascii_bytes_treated_as_separators() {
    // The ASCII fast path must NOT classify high bytes as token characters,
    // even though std::isalpha may do so under certain locales.
    const std::string s = "caf\xC3\xA9 latte";  // "café latte" in UTF-8
    const auto t = tok(s);
    IDX_CHECK_EQ(t.size(), 2u);
    IDX_CHECK_EQ(t[0], std::string{"caf"});
    IDX_CHECK_EQ(t[1], std::string{"latte"});
}

void test_empty_and_whitespace_only() {
    IDX_CHECK_EQ(tok("").size(), 0u);
    IDX_CHECK_EQ(tok("   \t\n").size(), 0u);
}

void test_appends_to_existing_buffer() {
    std::vector<std::string> out;
    out.emplace_back("seed");
    idx::token::tokenize("alpha beta", out);
    IDX_CHECK_EQ(out.size(), 3u);
    IDX_CHECK_EQ(out[0], std::string{"seed"});
    IDX_CHECK_EQ(out[1], std::string{"alpha"});
    IDX_CHECK_EQ(out[2], std::string{"beta"});
}

}  // namespace

int main() {
    test_lowercases_letters();
    test_keeps_digits_within_token();
    test_punctuation_splits_tokens();
    test_html_residue_is_dropped();
    test_non_ascii_bytes_treated_as_separators();
    test_empty_and_whitespace_only();
    test_appends_to_existing_buffer();
    return idx::testing::report("test_tokenizer");
}
