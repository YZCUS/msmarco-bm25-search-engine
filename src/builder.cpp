// Builder pipeline: tokenize collection.tsv, accumulate postings in memory,
// spill on threshold, then k-way merge into the final index.

#include "builder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
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

struct TermDictionary {
    std::unordered_map<std::string, int> term_id;
    std::vector<std::string> id_to_term;
    std::size_t term_string_bytes_estimate = 0;

    int touch(const std::string& term) {
        auto it = term_id.find(term);
        if (it != term_id.end()) return it->second;
        const int tid = static_cast<int>(id_to_term.size());
        id_to_term.push_back(term);
        auto [map_it, _] = term_id.emplace(id_to_term.back(), tid);
        term_string_bytes_estimate += id_to_term.back().capacity() + map_it->first.capacity();
        return tid;
    }

    void clear() {
        term_id.clear();
        id_to_term.clear();
        term_string_bytes_estimate = 0;
    }

    void release_memory() {
        std::unordered_map<std::string, int>().swap(term_id);
        std::vector<std::string>().swap(id_to_term);
        term_string_bytes_estimate = 0;
    }

    bool empty() const noexcept { return id_to_term.empty(); }
    std::size_t size() const noexcept { return id_to_term.size(); }
    std::string_view term(int tid) const noexcept { return id_to_term[tid]; }

    std::size_t estimated_bytes() const noexcept {
        return id_to_term.capacity() * sizeof(std::string)
            + term_string_bytes_estimate
            + term_id.bucket_count() * sizeof(void*)
            + term_id.size() * 48;
    }
};

// In-memory partial index for a single spill batch.
//
// IDX_BUILDER_VECTOR stores decoded (doc_id, freq) pairs and is the memory
// benchmark baseline. IDX_BUILDER_ARENA keeps that layout but allocates inner
// vectors from a monotonic resource. IDX_BUILDER_COMPACT stores postings as
// already-compressed VarByte chunks, which is the memory-plan implementation.
struct PartialIndex {
#if defined(IDX_BUILDER_VECTOR)
    using PostingList = std::vector<std::pair<int, int>>;
    using Postings    = std::vector<PostingList>;

    TermDictionary terms;
    Postings postings;

    int touch(const std::string& term) {
        const int tid = terms.touch(term);
        if (static_cast<std::size_t>(tid) == postings.size()) postings.emplace_back();
        return tid;
    }

    void clear() {
        terms.clear();
        postings.clear();
    }

    void release_memory() {
        terms.release_memory();
        Postings().swap(postings);
    }

    bool empty() const noexcept { return terms.empty(); }
    std::size_t term_count() const noexcept { return terms.size(); }
    std::string_view term(int tid) const noexcept { return terms.term(tid); }
    std::size_t posting_count(int tid) const noexcept { return postings[tid].size(); }

    void add(int tid, int doc_id, int freq) {
        auto& list = postings[tid];
        if (!list.empty() && list.back().first == doc_id) {
            list.back().second += freq;
        } else {
            list.emplace_back(doc_id, freq);
        }
    }

    void write_postings(int tid, std::ostream& out) const {
        std::vector<std::uint8_t> buf;
        int prev_did = 0;
        for (const auto& [did, freq] : postings[tid]) {
            idx::varbyte::encode(static_cast<std::uint32_t>(did - prev_did), buf);
            idx::varbyte::encode(static_cast<std::uint32_t>(freq), buf);
            prev_did = did;
        }
        if (!buf.empty()) {
            out.write(reinterpret_cast<const char*>(buf.data()),
                      static_cast<std::streamsize>(buf.size()));
        }
    }

    std::size_t encoded_bytes() const noexcept {
        std::size_t n = 0;
        for (const auto& list : postings) n += list.size() * sizeof(std::pair<int, int>);
        return n;
    }

    std::size_t estimated_bytes() const noexcept {
        return terms.estimated_bytes()
            + postings.capacity() * sizeof(PostingList)
            + encoded_bytes()
            + postings.size() * 16;
    }
#elif defined(IDX_BUILDER_ARENA)
    idx::mem::SlabArena arena{ /*slab_bytes=*/8 * 1024 * 1024 };
    idx::mem::SlabResource resource{ &arena };

    using PostingList = std::pmr::vector<std::pair<int, int>>;
    // CRITICAL: only the INNER posting buffers live on the arena. The outer
    // vector uses the default allocator so that arena.reset() in clear()
    // does not invalidate the outer vector's storage. Putting the outer
    // vector on the arena causes use-after-reset memory corruption that
    // shows up as a segfault on the second spill batch.
    using Postings = std::vector<PostingList>;

    TermDictionary terms;
    Postings postings;

    int touch(const std::string& term) {
        const int tid = terms.touch(term);
        if (static_cast<std::size_t>(tid) != postings.size()) return tid;
        // Explicitly bind the inner vector to the arena resource. We cannot
        // rely on uses-allocator construction here because the outer vector
        // uses the default allocator.
        postings.emplace_back(&resource);
        return tid;
    }

    void clear() {
        terms.clear();
        // Destroy inner vectors first so their destructors run while the
        // arena is still alive, then reclaim the arena in O(1).
        postings.clear();
        arena.reset();
    }

    void release_memory() {
        clear();
        Postings().swap(postings);
    }

    bool empty() const noexcept { return terms.empty(); }
    std::size_t term_count() const noexcept { return terms.size(); }
    std::string_view term(int tid) const noexcept { return terms.term(tid); }
    std::size_t posting_count(int tid) const noexcept { return postings[tid].size(); }

    void add(int tid, int doc_id, int freq) {
        auto& list = postings[tid];
        if (!list.empty() && list.back().first == doc_id) {
            list.back().second += freq;
        } else {
            list.emplace_back(doc_id, freq);
        }
    }

    void write_postings(int tid, std::ostream& out) const {
        std::vector<std::uint8_t> buf;
        int prev_did = 0;
        for (const auto& [did, freq] : postings[tid]) {
            idx::varbyte::encode(static_cast<std::uint32_t>(did - prev_did), buf);
            idx::varbyte::encode(static_cast<std::uint32_t>(freq), buf);
            prev_did = did;
        }
        if (!buf.empty()) {
            out.write(reinterpret_cast<const char*>(buf.data()),
                      static_cast<std::streamsize>(buf.size()));
        }
    }

    std::size_t encoded_bytes() const noexcept {
        std::size_t n = 0;
        for (const auto& list : postings) n += list.size() * sizeof(std::pair<int, int>);
        return n;
    }

    std::size_t estimated_bytes() const noexcept {
        return terms.estimated_bytes()
            + postings.capacity() * sizeof(PostingList)
            + arena.bytes_in_use()
            + postings.size() * 16;
    }
#else
    static constexpr std::uint32_t kNoChunk = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t kChunkBytes = 64;

    struct TermState {
        int last_doc_id = 0;
        std::uint32_t df = 0;
        std::uint32_t first_chunk = kNoChunk;
        std::uint32_t last_chunk = kNoChunk;
        std::size_t byte_count = 0;
    };

    struct Chunk {
        std::uint32_t next = kNoChunk;
        std::uint8_t size = 0;
        std::array<std::uint8_t, kChunkBytes> data{};
    };

    TermDictionary terms;
    std::vector<TermState> states;
    std::vector<Chunk> chunks;

    int touch(const std::string& term) {
        const int tid = terms.touch(term);
        if (static_cast<std::size_t>(tid) == states.size()) states.emplace_back();
        return tid;
    }

    void clear() {
        terms.clear();
        states.clear();
        chunks.clear();
    }

    void release_memory() {
        terms.release_memory();
        std::vector<TermState>().swap(states);
        std::vector<Chunk>().swap(chunks);
    }

    bool empty() const noexcept { return terms.empty(); }
    std::size_t term_count() const noexcept { return terms.size(); }
    std::string_view term(int tid) const noexcept { return terms.term(tid); }
    std::size_t posting_count(int tid) const noexcept { return states[tid].df; }

    void add(int tid, int doc_id, int freq) {
        auto& st = states[tid];
        if (st.df > 0 && doc_id <= st.last_doc_id) {
            throw std::runtime_error("compact builder: doc_ids must increase per term");
        }
        append_varbyte(st, static_cast<std::uint32_t>(doc_id - st.last_doc_id));
        append_varbyte(st, static_cast<std::uint32_t>(freq));
        st.last_doc_id = doc_id;
        ++st.df;
    }

    void write_postings(int tid, std::ostream& out) const {
        for (std::uint32_t c = states[tid].first_chunk; c != kNoChunk; c = chunks[c].next) {
            const auto& chunk = chunks[c];
            out.write(reinterpret_cast<const char*>(chunk.data.data()), chunk.size);
        }
    }

    std::size_t encoded_bytes() const noexcept {
        std::size_t n = 0;
        for (const auto& st : states) n += st.byte_count;
        return n;
    }

    std::size_t estimated_bytes() const noexcept {
        return terms.estimated_bytes()
            + states.capacity() * sizeof(TermState)
            + chunks.capacity() * sizeof(Chunk)
            + states.size() * 8;
    }

private:
    void append_varbyte(TermState& st, std::uint32_t v) {
        while (v >= 0x80u) {
            append_byte(st, static_cast<std::uint8_t>(v | 0x80u));
            v >>= 7;
        }
        append_byte(st, static_cast<std::uint8_t>(v));
    }

    void append_byte(TermState& st, std::uint8_t byte) {
        if (st.last_chunk == kNoChunk || chunks[st.last_chunk].size == kChunkBytes) {
            const auto idx = static_cast<std::uint32_t>(chunks.size());
            chunks.emplace_back();
            if (st.first_chunk == kNoChunk) {
                st.first_chunk = idx;
            } else {
                chunks[st.last_chunk].next = idx;
            }
            st.last_chunk = idx;
        }
        auto& chunk = chunks[st.last_chunk];
        chunk.data[chunk.size] = byte;
        ++chunk.size;
        ++st.byte_count;
    }
#endif
};

// Add (doc_id, freq) to a term's posting list, coalescing if the same
// document was just appended.
void add_posting(PartialIndex& idx, const std::string& term, int doc_id, int freq) {
    const int tid = idx.touch(term);
    idx.add(tid, doc_id, freq);
}

// Spill format (lexicographically sorted by term):
//   <term_size:vbyte> <term_bytes>
//   <num_postings:vbyte>
//   for each posting: <doc_id_delta:vbyte> <freq:vbyte>
void spill_partial(const PartialIndex& idx, const fs::path& path) {
    std::vector<int> tids(idx.term_count());
    for (std::size_t i = 0; i < tids.size(); ++i) tids[i] = static_cast<int>(i);
    std::sort(tids.begin(), tids.end(),
              [&](int a, int b) { return idx.term(a) < idx.term(b); });

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("spill: cannot open " + path.string());

    std::vector<std::uint8_t> buf;
    for (int tid : tids) {
        const auto& term = idx.term(tid);
        const auto postings = idx.posting_count(tid);
        if (postings == 0) continue;

        buf.clear();
        idx::varbyte::encode(static_cast<std::uint32_t>(term.size()), buf);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        out.write(term.data(), static_cast<std::streamsize>(term.size()));

        buf.clear();
        idx::varbyte::encode(static_cast<std::uint32_t>(postings), buf);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        idx.write_postings(tid, out);
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

    void begin_term(const std::string& term) {
        if (term_open_) throw std::runtime_error("FinalWriter: term already open");
        current_term_ = term;
        term_start_ = cur_offset_;
        doc_buf_.clear();
        freq_buf_.clear();
        prev_did_ = 0;
        last_did_ = 0;
        in_block_ = 0;
        current_df_ = 0;
        have_pending_ = false;
        pending_did_ = 0;
        pending_freq_ = 0;
        term_open_ = true;
    }

    void add_posting(int did, int freq) {
        if (!term_open_) throw std::runtime_error("FinalWriter: no term open");
        if (have_pending_) {
            if (did < pending_did_) {
                throw std::runtime_error("FinalWriter: postings must be sorted by doc_id");
            }
            if (did == pending_did_) {
                pending_freq_ += freq;
                return;
            }
            flush_pending();
        }
        pending_did_ = did;
        pending_freq_ = freq;
        have_pending_ = true;
    }

    void finish_term() {
        if (!term_open_) return;
        flush_pending();
        flush_block();
        if (current_df_ == 0) {
            current_term_.clear();
            term_open_ = false;
            return;
        }

        const std::int64_t bytes_size = cur_offset_ - term_start_;
        lex_out_ << current_term_ << ' ' << term_id_counter_ << ' ' << current_df_
                 << ' ' << term_start_ << ' ' << bytes_size << '\n';
        ++term_id_counter_;
        current_term_.clear();
        term_open_ = false;
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
    std::string current_term_;
    std::int64_t term_start_ = 0;
    std::vector<std::uint8_t> doc_buf_;
    std::vector<std::uint8_t> freq_buf_;
    int prev_did_ = 0;
    int last_did_ = 0;
    int in_block_ = 0;
    std::size_t current_df_ = 0;
    bool term_open_ = false;
    bool have_pending_ = false;
    int pending_did_ = 0;
    int pending_freq_ = 0;

    void flush_pending() {
        if (!have_pending_) return;
        const int delta = pending_did_ - prev_did_;
        idx::codec::encode(static_cast<std::uint32_t>(delta), doc_buf_);
        idx::codec::encode(static_cast<std::uint32_t>(pending_freq_), freq_buf_);
        prev_did_ = pending_did_;
        last_did_ = pending_did_;
        ++in_block_;
        ++current_df_;
        have_pending_ = false;
        if (in_block_ == postings_per_block_) flush_block();
    }

    void flush_block() {
        if (in_block_ == 0) return;
        index_out_.write(reinterpret_cast<const char*>(doc_buf_.data()),
                         static_cast<std::streamsize>(doc_buf_.size()));
        index_out_.write(reinterpret_cast<const char*>(freq_buf_.data()),
                         static_cast<std::streamsize>(freq_buf_.size()));
        const std::int32_t last_did_v = static_cast<std::int32_t>(last_did_);
        const std::int64_t did_size = static_cast<std::int64_t>(doc_buf_.size());
        const std::int64_t freq_size = static_cast<std::int64_t>(freq_buf_.size());
        blocks_out_.write(reinterpret_cast<const char*>(&last_did_v), sizeof(last_did_v));
        blocks_out_.write(reinterpret_cast<const char*>(&did_size), sizeof(did_size));
        blocks_out_.write(reinterpret_cast<const char*>(&freq_size), sizeof(freq_size));
        cur_offset_ += did_size + freq_size;
        doc_buf_.clear();
        freq_buf_.clear();
        in_block_ = 0;
    }
};

void write_stats_json(const fs::path& path, const BuildStats& stats) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("write_stats_json: cannot open " + path.string());
    out << "{\n"
        << "  \"docs_processed\": " << stats.docs_processed << ",\n"
        << "  \"total_postings\": " << stats.total_postings << ",\n"
        << "  \"spill_count\": " << stats.spill_count << ",\n"
        << "  \"peak_unique_terms\": " << stats.peak_unique_terms << ",\n"
        << "  \"peak_partial_postings\": " << stats.peak_partial_postings << ",\n"
        << "  \"peak_partial_bytes_estimate\": " << stats.peak_partial_bytes_estimate << ",\n"
        << "  \"final_terms\": " << stats.final_terms << ",\n"
        << "  \"final_index_bytes\": " << stats.final_index_bytes << "\n"
        << "}\n";
}

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
    BuildStats stats;

    auto update_peak = [&] {
        stats.peak_unique_terms = std::max(stats.peak_unique_terms, partial.term_count());
        stats.peak_partial_postings = std::max(stats.peak_partial_postings, partial_postings);
        stats.peak_partial_bytes_estimate =
            std::max(stats.peak_partial_bytes_estimate, partial.estimated_bytes());
    };

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
            ++stats.total_postings;
        }
        update_peak();

        doc_info_out << doc_length << ' ' << this_line_pos << '\n';

        if (partial_postings >= opts.spill_threshold) {
            const fs::path temp_path = temp_prefix + std::to_string(temp_files.size()) + ".bin";
            spill_partial(partial, temp_path);
            temp_files.push_back(temp_path);
            ++stats.spill_count;
            partial.clear();
            partial_postings = 0;
            IDX_LOG("spilled batch " << temp_files.size() << " at doc " << doc_id_counter);
        }
        ++doc_id_counter;
    }

    if (!partial.empty()) {
        const fs::path temp_path = temp_prefix + std::to_string(temp_files.size()) + ".bin";
        spill_partial(partial, temp_path);
        temp_files.push_back(temp_path);
        ++stats.spill_count;
        partial.clear();
    }
    partial.release_memory();
    stats.docs_processed = static_cast<std::size_t>(doc_id_counter);
    doc_info_out.close();

    IDX_LOG("merging " << temp_files.size() << " spills");

    // Open spill files and prime the priority queue with one entry per file.
    std::vector<std::ifstream> files;
    files.reserve(temp_files.size());
    for (const auto& p : temp_files) {
        files.emplace_back(p, std::ios::binary);
        if (!files.back()) throw std::runtime_error("merge: cannot open " + p.string());
    }

    auto cmp = [](const SpillEntry& a, const SpillEntry& b) {
        if (a.term != b.term) return a.term > b.term;
        return a.file_index > b.file_index;
    };
    std::vector<SpillEntry> heap;
    heap.reserve(files.size());
    auto push_entry = [&](SpillEntry entry) {
        heap.push_back(std::move(entry));
        std::push_heap(heap.begin(), heap.end(), cmp);
    };
    auto pop_entry = [&] {
        std::pop_heap(heap.begin(), heap.end(), cmp);
        SpillEntry entry = std::move(heap.back());
        heap.pop_back();
        return entry;
    };
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        SpillEntry e;
        if (read_spill_entry(files[i], e, i)) push_entry(std::move(e));
    }

    FinalWriter writer(output_dir, opts.postings_per_block);
    while (!heap.empty()) {
        const std::string term = heap.front().term;
        writer.begin_term(term);
        while (!heap.empty() && heap.front().term == term) {
            SpillEntry e = pop_entry();
            for (const auto& p : e.postings) writer.add_posting(p.first, p.second);
            const int idx_in_files = e.file_index;
            SpillEntry next_entry;
            if (read_spill_entry(files[idx_in_files], next_entry, idx_in_files)) {
                push_entry(std::move(next_entry));
            }
        }
        writer.finish_term();
    }

    for (const auto& p : temp_files) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    stats.final_terms = static_cast<std::size_t>(writer.term_count());
    stats.final_index_bytes = static_cast<std::size_t>(writer.total_bytes());
    if (opts.stats) *opts.stats = stats;
    if (!opts.stats_json_path.empty()) write_stats_json(opts.stats_json_path, stats);
    IDX_LOG("done. terms=" << writer.term_count() << " bytes=" << writer.total_bytes());
}

}  // namespace idx::build
