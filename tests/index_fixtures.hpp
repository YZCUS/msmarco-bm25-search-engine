#pragma once

// Helpers for writing a hand-crafted inverted index to disk so that
// SearchEngine end-to-end tests can run against real files without invoking
// the full build_index pipeline.
//
// The on-disk layout exactly matches what src/build_index.cpp will produce
// after the P1 refactor, so any incompatibility surfaces immediately as a
// test failure on either the writer or the reader.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"

namespace idx::testing {

struct TinyDoc {
    int length;        // raw token count (used as |d| in BM25)
    std::string raw;   // optional passage text written into collection.tsv
};

struct TinyTerm {
    std::string text;
    // Postings sorted by ascending doc_id. Pair: (doc_id, frequency).
    std::vector<std::pair<int, int>> postings;
};

struct TinyIndexLayout {
    int total_docs = 0;
    int postings_per_block = 2;   // tiny default so multi-block paths are hit
};

namespace detail {

inline std::int64_t write_term_blocks(
    std::ofstream& index_out,
    std::vector<std::uint8_t>& doc_id_buf,
    std::vector<std::uint8_t>& freq_buf,
    std::vector<std::tuple<std::int32_t, std::int64_t, std::int64_t>>& block_meta,
    const TinyTerm& term, int postings_per_block,
    std::int64_t* out_bytes_size) {
    const std::int64_t start_position = index_out.tellp();

    int prev_doc_id = 0;
    int last_doc_id = 0;
    int in_block = 0;
    doc_id_buf.clear();
    freq_buf.clear();

    auto flush = [&] {
        if (in_block == 0) return;
        index_out.write(reinterpret_cast<const char*>(doc_id_buf.data()),
                        static_cast<std::streamsize>(doc_id_buf.size()));
        index_out.write(reinterpret_cast<const char*>(freq_buf.data()),
                        static_cast<std::streamsize>(freq_buf.size()));
        block_meta.emplace_back(static_cast<std::int32_t>(last_doc_id),
                                static_cast<std::int64_t>(doc_id_buf.size()),
                                static_cast<std::int64_t>(freq_buf.size()));
        doc_id_buf.clear();
        freq_buf.clear();
        in_block = 0;
    };

    for (const auto& [doc_id, freq] : term.postings) {
        const std::uint32_t delta = static_cast<std::uint32_t>(doc_id - prev_doc_id);
        idx::codec::encode(delta, doc_id_buf);
        idx::codec::encode(static_cast<std::uint32_t>(freq), freq_buf);
        prev_doc_id = doc_id;
        last_doc_id = doc_id;
        ++in_block;
        if (in_block == postings_per_block) flush();
    }
    flush();

    const std::int64_t end_position = index_out.tellp();
    *out_bytes_size = end_position - start_position;
    return start_position;
}

inline void write_block_info(const std::filesystem::path& path,
                             const std::vector<std::tuple<std::int32_t,
                                                          std::int64_t,
                                                          std::int64_t>>& blocks) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("write_block_info: cannot open " + path.string());
    for (const auto& [last_did, did_size, freq_size] : blocks) {
        out.write(reinterpret_cast<const char*>(&last_did), sizeof(last_did));
        out.write(reinterpret_cast<const char*>(&did_size), sizeof(did_size));
        out.write(reinterpret_cast<const char*>(&freq_size), sizeof(freq_size));
    }
}

}  // namespace detail

// Materialize the tiny index into `dir`. The directory must already exist.
inline void write_tiny_index(const std::filesystem::path& dir,
                             const std::vector<TinyDoc>& docs,
                             std::vector<TinyTerm> terms,
                             TinyIndexLayout layout = {}) {
    std::sort(terms.begin(), terms.end(),
              [](const TinyTerm& a, const TinyTerm& b) { return a.text < b.text; });

    const auto index_path     = dir / "final_sorted_index.bin";
    const auto block_path     = dir / "final_sorted_block_info.bin";
    const auto lexicon_path   = dir / "final_sorted_lexicon.txt";
    const auto doc_info_path  = dir / "document_info.txt";
    const auto collection_path = dir / "collection.tsv";

    std::ofstream index_out(index_path, std::ios::binary);
    std::ofstream lex_out(lexicon_path);
    std::vector<std::tuple<std::int32_t, std::int64_t, std::int64_t>> block_meta;

    std::vector<std::uint8_t> doc_id_buf, freq_buf;
    int term_id = 0;
    for (const auto& term : terms) {
        std::int64_t bytes_size = 0;
        const std::int64_t start_position = detail::write_term_blocks(
            index_out, doc_id_buf, freq_buf, block_meta, term,
            layout.postings_per_block, &bytes_size);
        const int df = static_cast<int>(term.postings.size());
        lex_out << term.text << ' ' << term_id << ' ' << df << ' '
                << start_position << ' ' << bytes_size << '\n';
        ++term_id;
    }
    index_out.close();
    lex_out.close();

    detail::write_block_info(block_path, block_meta);

    // document_info.txt: one row per doc_id from 0..N-1.
    // line_position is the byte offset into collection.tsv.
    std::ofstream doc_out(doc_info_path);
    std::ofstream coll_out(collection_path);
    std::int64_t line_pos = 0;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        doc_out << docs[i].length << ' ' << line_pos << '\n';
        const std::string line = std::to_string(i) + "\t" + docs[i].raw + "\n";
        coll_out << line;
        line_pos += static_cast<std::int64_t>(line.size());
    }
}

}  // namespace idx::testing
