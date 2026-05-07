#pragma once

// ASCII-only tokenizer used by both the indexer and the query path.
// Lower-cases A-Z, keeps 0-9, splits on every other character.
//
// Rationale: std::isalpha is locale-dependent and may produce surprising
// behavior on UTF-8 input; an explicit ASCII fast path keeps the tokenization
// deterministic across platforms and reproduces results across machines.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace idx::token {

inline bool is_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Tokenize `text` into lower-cased tokens, appending them to `out`. The
// previous contents of `out` are preserved so the caller can reuse a buffer
// across many documents.
inline void tokenize(std::string_view text, std::vector<std::string>& out) {
    std::string current;
    current.reserve(32);
    for (char c : text) {
        if (is_token_char(c)) {
            current.push_back(to_lower_ascii(c));
        } else if (!current.empty()) {
            out.push_back(std::move(current));
            current.clear();
            current.reserve(32);
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
}

}  // namespace idx::token
