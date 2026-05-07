// Builder pipeline: tokenize collection.tsv, accumulate postings in memory,
// spill on threshold, then k-way merge into the final index.

#include "builder.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <memory_resource>

#include "allocator.hpp"
#include "codec.hpp"
#include "log.hpp"
#include "tokenizer.hpp"
#include "varbyte.hpp"  // spill files always use VarByte regardless of IDX_CODEC

namespace idx::build {

namespace fs = std::filesystem;

namespace {

// In-memory partial index for a single spill batch.
//
// Two storage modes are available, gated by IDX_BUILDER_BASELINE:
//
//   default (arena):   std::pmr::vector backed by SlabArena. Inner posting
//                      lists draw from a single bump pointer that gets reset
//                      to zero between spill batches in O(1) — ideal for the
//                      build hot path which churns millions of small
//                      allocations per batch.
//
//   IDX_BUILDER_BASELINE: plain std::vector path used as the "before" arm of
//                      the memory benchmark. Each inner vector goes through
//                      the system allocator, exhibiting the reallocation
//                      churn the arena is meant to eliminate.
struct PartialIndex {
#if defined(IDX_BUILDER_BASELINE)
    using PostingList = std::vector<std::pair<int, int>>;
    using Postings    = std::vector<PostingList>;

    std::unordered_map<std::string, int> term_id;
    std::vector<std::string> id_to_term;
    Postings postings;

    int touch(const std::string& term) {
        auto it = term_id.find(term);
        if (it != term_id.end()) return it->second;
        const int tid = static_cast<int>(id_to_term.size());
        id_to_term.push_back(term);
        postings.emplace_back();
        term_id.emplace(id_to_term.back(), tid);
        return tid;
    }

    void clear() {
        term_id.clear();
        id_to_term.clear();
        postings.clear();
    }

    std::size_t arena_bytes_in_use() const noexcept { return 0; }
#else
    idx::mem::SlabArena arena{ /*slab_bytes=*/8 * 1024 * 1024 };
    idx::mem::SlabResource resource{ &arena };

    using PostingList = std::pmr::vector<std::pair<int, int>>;
    // CRITICAL: only the INNER posting buffers live on the arena. The outer
    // vector uses the default allocator so that arena.reset() in clear()
    // does not invalidate the outer vector's storage. Putting the outer
    // vector on the arena causes use-after-reset memory corruption that
    // shows up as a segfault on the second spill batch.
    using Postings = std::vector<PostingList>;

    std::unordered_map<std::string, int> term_id;
    std::vector<std::string> id_to_term;
    Postings postings;

    int touch(const std::string& term) {
        auto it = term_id.find(term);
        if (it != term_id.end()) return it->second;
        const int tid = static_cast<int>(id_to_term.size());
        id_to_term.push_back(term);
        // Explicitly bind the inner vector to the arena resource. We cannot
        // rely on uses-allocator construction here because the outer vector
        // uses the default allocator.
        postings.emplace_back(&resource);
        term_id.emplace(id_to_term.back(), tid);
        return tid;
    }

    void clear() {
        term_id.clear();
        id_to_term.clear();
        // Destroy inner vectors first so their destructors run while the
        // arena is still alive, then reclaim the arena in O(1).
        postings.clear();
        arena.reset();
    }

    std::size_t arena_bytes_in_use() const noexcept { return arena.bytes_in_use(); }
#endif
};

// Add (doc_id, freq) to a term's posting list, coalescing if the same
// document was just appended.
void add_posting(PartialIndex& idx, const std::string& term, int doc_id, int freq) {
    const int tid = idx.touch(term);
    auto& list = idx.postings[tid];
    if (!list.empty() && list.back().first == doc_id) {
        list.back().second += freq;
    } else {
        list.emplace_back(doc_id, freq);
    }
}

// Spill format (lexicographically sorted by term):
//   <term_size:vbyte> <term_bytes>
//   <num_postings:vbyte>
//   for each posting: <doc_id_delta:vbyte> <freq:vbyte>
void spill_partial(const PartialIndex& idx, const fs::path& path) {
    std::vector<int> tids(idx.id_to_term.size());
    for (std::size_t i = 0; i < tids.size(); ++i) tids[i] = static_cast<int>(i);
    std::sort(tids.begin(), tids.end(),
              [&](int a, int b) { return idx.id_to_term[a] < idx.id_to_term[b]; });

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("spill: cannot open " + path.string());

    std::vector<std::uint8_t> buf;
    for (int tid : tids) {
        const auto& term = idx.id_to_term[tid];
        const auto& postings = idx.postings[tid];
        if (postings.empty()) continue;

        buf.clear();
        idx::varbyte::encode(static_cast<std::uint32_t>(term.size()), buf);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        out.write(term.data(), static_cast<std::streamsize>(term.size()));

        buf.clear();
        idx::varbyte::encode(static_cast<std::uint32_t>(postings.size()), buf);
        int prev_did = 0;
        for (std::size_t i = 0; i < postings.size(); ++i) {
            const int delta = postings[i].first - prev_did;
            idx::varbyte::encode(static_cast<std::uint32_t>(delta), buf);
            idx::varbyte::encode(static_cast<std::uint32_t>(postings[i].second), buf);
            prev_did = postings[i].first;
        }
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
    }
}

bool read_varbyte(std::ifstream& in, std::uint32_t& v) {
    v = 0;
    std::uint32_t shift = 0;
    while (true) {
        char c = 0;
        if (!in.read(&c, 1)) return false;
        const std::uint8_t b = static_cast<std::uint8_t>(c);
        v |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) return true;
        shift += 7;
        if (shift >= 35) throw std::runtime_error("read_varbyte: overflow");
    }
}

struct SpillEntry {
    std::string term;
    std::vector<std::pair<int, int>> postings;
    int file_index = 0;
};

bool read_spill_entry(std::ifstream& in, SpillEntry& entry, int file_index) {
    std::uint32_t term_size = 0;
    if (!read_varbyte(in, term_size)) return false;
    entry.term.resize(term_size);
    if (term_size > 0 && !in.read(entry.term.data(), term_size)) return false;

    std::uint32_t num_postings = 0;
    if (!read_varbyte(in, num_postings)) return false;
    entry.postings.clear();
    entry.postings.reserve(num_postings);

    int prev = 0;
    for (std::uint32_t i = 0; i < num_postings; ++i) {
        std::uint32_t delta = 0;
        std::uint32_t freq = 0;
        if (!read_varbyte(in, delta) || !read_varbyte(in, freq)) return false;
        prev += static_cast<int>(delta);
        entry.postings.emplace_back(prev, static_cast<int>(freq));
    }
    entry.file_index = file_index;
    return true;
}

// Final merge: write (block-encoded) postings + per-block metadata + lexicon.
class FinalWriter {
public:
    FinalWriter(const fs::path& dir, int postings_per_block)
        : index_out_(dir / "final_sorted_index.bin", std::ios::binary),
          blocks_out_(dir / "final_sorted_block_info.bin", std::ios::binary),
          lex_out_(dir / "final_sorted_lexicon.txt"),
          postings_per_block_(postings_per_block) {
        if (!index_out_) throw std::runtime_error("FinalWriter: open final_sorted_index.bin");
        if (!blocks_out_) throw std::runtime_error("FinalWriter: open final_sorted_block_info.bin");
        if (!lex_out_)   throw std::runtime_error("FinalWriter: open final_sorted_lexicon.txt");
    }

    void write_term(const std::string& term, std::vector<std::pair<int, int>> postings) {
        if (postings.empty()) return;
        std::sort(postings.begin(), postings.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Coalesce duplicate doc_ids that may show up across spill files.
        std::vector<std::pair<int, int>> merged;
        merged.reserve(postings.size());
        for (const auto& p : postings) {
            if (!merged.empty() && merged.back().first == p.first) {
                merged.back().second += p.second;
            } else {
                merged.push_back(p);
            }
        }

        const std::int64_t term_start = cur_offset_;
        std::vector<std::uint8_t> doc_buf, freq_buf;
        int prev_did = 0;
        int last_did = 0;
        int in_block = 0;

        auto flush = [&] {
            if (in_block == 0) return;
            index_out_.write(reinterpret_cast<const char*>(doc_buf.data()),
                             static_cast<std::streamsize>(doc_buf.size()));
            index_out_.write(reinterpret_cast<const char*>(freq_buf.data()),
                             static_cast<std::streamsize>(freq_buf.size()));
            const std::int32_t last_did_v = static_cast<std::int32_t>(last_did);
            const std::int64_t did_size  = static_cast<std::int64_t>(doc_buf.size());
            const std::int64_t freq_size = static_cast<std::int64_t>(freq_buf.size());
            blocks_out_.write(reinterpret_cast<const char*>(&last_did_v), sizeof(last_did_v));
            blocks_out_.write(reinterpret_cast<const char*>(&did_size),    sizeof(did_size));
            blocks_out_.write(reinterpret_cast<const char*>(&freq_size),   sizeof(freq_size));
            cur_offset_ += did_size + freq_size;
            doc_buf.clear();
            freq_buf.clear();
            in_block = 0;
        };

        for (const auto& [did, f] : merged) {
            const int delta = did - prev_did;
            idx::codec::encode(static_cast<std::uint32_t>(delta), doc_buf);
            idx::codec::encode(static_cast<std::uint32_t>(f), freq_buf);
            prev_did = did;
            last_did = did;
            ++in_block;
            if (in_block == postings_per_block_) flush();
        }
        flush();

        const std::int64_t bytes_size = cur_offset_ - term_start;
        lex_out_ << term << ' ' << term_id_counter_ << ' ' << merged.size()
                 << ' ' << term_start << ' ' << bytes_size << '\n';
        ++term_id_counter_;
    }

    int term_count() const noexcept { return term_id_counter_; }
    std::int64_t total_bytes() const noexcept { return cur_offset_; }

private:
    std::ofstream index_out_;
    std::ofstream blocks_out_;
    std::ofstream lex_out_;
    std::int64_t cur_offset_ = 0;
    int term_id_counter_ = 0;
    int postings_per_block_;
};

}  // namespace

void build_index(const fs::path& input_tsv, const fs::path& output_dir,
                 const BuildOptions& opts) {
    fs::create_directories(output_dir);
    std::ifstream in(input_tsv, std::ios::binary);
    if (!in) throw std::runtime_error("build_index: cannot open " + input_tsv.string());

    std::ofstream doc_info_out(output_dir / "document_info.txt");
    if (!doc_info_out) throw std::runtime_error("build_index: cannot open document_info.txt");

    const std::string temp_prefix = (output_dir / "temp_index_").string();
    std::vector<fs::path> temp_files;

    PartialIndex partial;
    std::size_t partial_postings = 0;

    std::string line;
    line.reserve(2048);
    std::int64_t line_pos = 0;
    int doc_id_counter = 0;
    std::vector<std::string> tokens;
    tokens.reserve(128);

    while (std::getline(in, line)) {
        const std::int64_t this_line_pos = line_pos;
        line_pos += static_cast<std::int64_t>(line.size()) + 1;  // assume LF separators

        std::string_view view{line};
        const auto tab = view.find('\t');
        const std::string_view text = (tab == std::string_view::npos)
                                          ? view
                                          : view.substr(tab + 1);

        tokens.clear();
        idx::token::tokenize(text, tokens);
        const int doc_length = static_cast<int>(tokens.size());

        // Per-document term-frequency aggregation, then push into partial index.
        std::unordered_map<std::string, int> per_doc;
        per_doc.reserve(tokens.size());
        for (auto& t : tokens) per_doc[std::move(t)]++;
        for (const auto& [t, f] : per_doc) {
            add_posting(partial, t, doc_id_counter, f);
            ++partial_postings;
        }

        doc_info_out << doc_length << ' ' << this_line_pos << '\n';

        if (partial_postings >= opts.spill_threshold) {
            const fs::path temp_path = temp_prefix + std::to_string(temp_files.size()) + ".bin";
            spill_partial(partial, temp_path);
            temp_files.push_back(temp_path);
            partial.clear();
            partial_postings = 0;
            IDX_LOG("spilled batch " << temp_files.size() << " at doc " << doc_id_counter);
        }
        ++doc_id_counter;
    }

    if (!partial.id_to_term.empty()) {
        const fs::path temp_path = temp_prefix + std::to_string(temp_files.size()) + ".bin";
        spill_partial(partial, temp_path);
        temp_files.push_back(temp_path);
        partial.clear();
    }
    doc_info_out.close();

    IDX_LOG("merging " << temp_files.size() << " spills");

    // Open spill files and prime the priority queue with one entry per file.
    std::vector<std::ifstream> files;
    files.reserve(temp_files.size());
    for (const auto& p : temp_files) {
        files.emplace_back(p, std::ios::binary);
        if (!files.back()) throw std::runtime_error("merge: cannot open " + p.string());
    }

    auto cmp = [](const SpillEntry& a, const SpillEntry& b) { return a.term > b.term; };
    std::priority_queue<SpillEntry, std::vector<SpillEntry>, decltype(cmp)> pq(cmp);
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        SpillEntry e;
        if (read_spill_entry(files[i], e, i)) pq.push(std::move(e));
    }

    FinalWriter writer(output_dir, opts.postings_per_block);
    while (!pq.empty()) {
        const std::string term = pq.top().term;
        std::vector<std::pair<int, int>> merged;
        while (!pq.empty() && pq.top().term == term) {
            SpillEntry e = pq.top();
            pq.pop();
            for (const auto& p : e.postings) merged.push_back(p);
            const int idx_in_files = e.file_index;
            SpillEntry next_entry;
            if (read_spill_entry(files[idx_in_files], next_entry, idx_in_files)) {
                pq.push(std::move(next_entry));
            }
        }
        writer.write_term(term, std::move(merged));
    }

    for (const auto& p : temp_files) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    IDX_LOG("done. terms=" << writer.term_count() << " bytes=" << writer.total_bytes());
}

}  // namespace idx::build
