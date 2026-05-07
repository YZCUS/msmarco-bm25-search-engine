// SearchEngine implementation: PIMPL-based, mmap-backed, thread-pool-driven.
//
// This file replaces the legacy global-namespace SearchEngine in
// src/search_engine.cpp. Once P1 is done the legacy file will be deleted and
// this file renamed to src/search_engine.cpp.

#include "search_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bm25.hpp"
#include "inverted_list.hpp"
#include "log.hpp"
#include "mmap_file.hpp"
#include "posting.hpp"
#include "thread_pool.hpp"
#include "tokenizer.hpp"

namespace idx::query {

namespace {

struct LexEntry {
    int term_id;
    int df;
    std::int64_t start_position;
    std::int64_t bytes_size;
};

void load_lexicon(const std::string& path,
                  std::unordered_map<std::string, LexEntry>& out) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("load_lexicon: cannot open " + path);
    std::string term;
    LexEntry e{};
    while (in >> term >> e.term_id >> e.df >> e.start_position >> e.bytes_size) {
        out.emplace(std::move(term), e);
        term.clear();
    }
}

void load_block_info(const std::string& path, std::vector<BlockMeta>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("load_block_info: cannot open " + path);
    std::int64_t cumulative = 0;
    BlockMeta m{};
    while (in.read(reinterpret_cast<char*>(&m.last_doc_id), sizeof(m.last_doc_id)) &&
           in.read(reinterpret_cast<char*>(&m.doc_id_size), sizeof(m.doc_id_size)) &&
           in.read(reinterpret_cast<char*>(&m.freq_size), sizeof(m.freq_size))) {
        if (m.doc_id_size <= 0 || m.freq_size <= 0) {
            throw std::runtime_error("load_block_info: invalid block size in " + path);
        }
        m.start_offset = cumulative;
        cumulative += m.doc_id_size + m.freq_size;
        out.push_back(m);
    }
}

void load_doc_info(const std::string& path,
                   std::vector<int>& doc_lengths,
                   std::vector<std::int64_t>& line_positions,
                   double& avgdl_out) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("load_doc_info: cannot open " + path);
    long long total_len = 0;
    int dl = 0;
    std::int64_t pos = 0;
    while (in >> dl >> pos) {
        doc_lengths.push_back(dl);
        line_positions.push_back(pos);
        total_len += dl;
    }
    if (doc_lengths.empty()) {
        avgdl_out = 0.0;
    } else {
        avgdl_out = static_cast<double>(total_len) /
                    static_cast<double>(doc_lengths.size());
    }
}

std::vector<std::string> tokenize_query(std::string_view q) {
    std::vector<std::string> out;
    idx::token::tokenize(q, out);
    // Query-time stopword filtering keeps high-df terms like "what" and "is"
    // from forcing a full scan of enormous posting lists. The index still
    // stores these terms, so this is a search-policy choice rather than an
    // indexing-format change.
    auto is_stopword = [](const std::string& t) {
        static const std::unordered_set<std::string> kStopwords = {
            "a", "an", "and", "are", "as", "at", "be", "by", "for",
            "from", "how", "in", "is", "it", "of", "on", "or", "that",
            "the", "to", "was", "what", "when", "where", "which", "who",
            "whom", "why", "with", "without", "do", "does", "did", "can",
            "could", "would", "should", "this", "these", "those",
        };
        return kStopwords.find(t) != kStopwords.end();
    };
    out.erase(std::remove_if(out.begin(), out.end(), is_stopword), out.end());
    return out;
}

}  // namespace

struct SearchEngine::Impl {
    SearchEnginePaths paths;
    idx::io::MmapFile index_file;
    std::unordered_map<std::string, LexEntry> lexicon;
    std::vector<BlockMeta> blocks;
    std::vector<int> doc_lengths;
    std::vector<std::int64_t> line_positions;
    int total_docs = 0;
    double avgdl = 0.0;

    std::unique_ptr<idx::concurrent::ThreadPool> pool;

    // Guards collection-file reads when filling passages from disk.
    mutable std::mutex collection_mutex;
    mutable std::ifstream collection_stream;

    explicit Impl(const SearchEnginePaths& p, unsigned threads)
        : paths(p), index_file(idx::io::MmapFile::open_readonly(p.index_file)) {
        load_lexicon(p.lexicon_file, lexicon);
        load_block_info(p.block_info_file, blocks);
        load_doc_info(p.doc_info_file, doc_lengths, line_positions, avgdl);
        total_docs = static_cast<int>(doc_lengths.size());
        IDX_LOG("SearchEngine loaded: docs=" << total_docs
                << " avgdl=" << avgdl
                << " terms=" << lexicon.size()
                << " blocks=" << blocks.size());
        if (threads > 0) pool = std::make_unique<idx::concurrent::ThreadPool>(threads);
        if (!paths.collection_file.empty()) {
            collection_stream.open(paths.collection_file, std::ios::binary);
        }
    }

    std::vector<idx::SearchResult> search(std::string_view query,
                                          SearchOptions opts) const;
    std::vector<idx::SearchResult> rank_disjunctive(
        const std::vector<const LexEntry*>& terms, int top_k) const;
    std::vector<idx::SearchResult> rank_conjunctive(
        const std::vector<const LexEntry*>& terms, int top_k) const;
    void fill_passages(std::vector<idx::SearchResult>& results) const;
};

namespace {

void finalize_top_k(std::vector<idx::SearchResult>& results, int top_k) {
    if (top_k <= 0) {
        results.clear();
        return;
    }
    std::sort(results.begin(), results.end(),
              [](const idx::SearchResult& a, const idx::SearchResult& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.doc_id < b.doc_id;
              });
    if (static_cast<int>(results.size()) > top_k) results.resize(top_k);
    for (std::size_t i = 0; i < results.size(); ++i) {
        results[i].rank = static_cast<int>(i) + 1;
    }
}

bool is_better(const idx::SearchResult& a, const idx::SearchResult& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc_id < b.doc_id;
}

struct WorstResultFirst {
    bool operator()(const idx::SearchResult& a, const idx::SearchResult& b) const {
        return is_better(a, b);
    }
};

using TopKHeap = std::priority_queue<
    idx::SearchResult,
    std::vector<idx::SearchResult>,
    WorstResultFirst>;

void push_top_k(TopKHeap& heap, idx::SearchResult result, int top_k) {
    if (top_k <= 0) return;
    if (static_cast<int>(heap.size()) < top_k) {
        heap.push(std::move(result));
        return;
    }
    if (is_better(result, heap.top())) {
        heap.pop();
        heap.push(std::move(result));
    }
}

std::vector<idx::SearchResult> drain_top_k(TopKHeap& heap, int top_k) {
    std::vector<idx::SearchResult> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        results.push_back(std::move(heap.top()));
        heap.pop();
    }
    finalize_top_k(results, top_k);
    return results;
}

}  // namespace

std::vector<idx::SearchResult> SearchEngine::Impl::rank_disjunctive(
    const std::vector<const LexEntry*>& terms, int top_k) const {
    if (top_k <= 0) return {};

    std::vector<InvertedList> lists;
    lists.reserve(terms.size());
    for (const auto* e : terms) {
        lists.emplace_back(index_file.data(), e->start_position, e->bytes_size,
                           e->df, blocks);
    }
    for (auto& l : lists) l.advance();

    TopKHeap top;

    while (true) {
        // Find the smallest doc_id present in any positioned list.
        int min_did = INT32_MAX;
        for (const auto& l : lists) {
            if (l.positioned() && l.doc_id() < min_did) min_did = l.doc_id();
        }
        if (min_did == INT32_MAX) break;

        double s = 0.0;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            if (lists[i].positioned() && lists[i].doc_id() == min_did) {
                s += idx::bm25::score(total_docs, terms[i]->df, lists[i].term_freq(),
                                      doc_lengths[min_did], avgdl);
                lists[i].advance();
            }
        }
        push_top_k(top, {min_did, s, 0, {}}, top_k);
    }

    return drain_top_k(top, top_k);
}

std::vector<idx::SearchResult> SearchEngine::Impl::rank_conjunctive(
    const std::vector<const LexEntry*>& terms, int top_k) const {
    if (top_k <= 0) return {};

    std::vector<InvertedList> lists;
    lists.reserve(terms.size());
    for (const auto* e : terms) {
        lists.emplace_back(index_file.data(), e->start_position, e->bytes_size,
                           e->df, blocks);
    }

    // Position every list on its first posting; if any list is empty there
    // is no possible intersection.
    int pivot = 0;
    for (auto& l : lists) {
        if (!l.advance()) return {};
        pivot = std::max(pivot, l.doc_id());
    }

    TopKHeap top;

    while (true) {
        bool aligned = true;
        int max_did = pivot;
        bool exhausted_any = false;
        for (auto& l : lists) {
            if (!l.advanceTo(pivot)) { exhausted_any = true; break; }
            if (l.doc_id() != pivot) aligned = false;
            if (l.doc_id() > max_did) max_did = l.doc_id();
        }
        if (exhausted_any) break;

        if (aligned) {
            double s = 0.0;
            for (std::size_t i = 0; i < lists.size(); ++i) {
                s += idx::bm25::score(total_docs, terms[i]->df, lists[i].term_freq(),
                                      doc_lengths[pivot], avgdl);
            }
            push_top_k(top, {pivot, s, 0, {}}, top_k);
            pivot += 1;
        } else {
            pivot = max_did;
        }
    }

    return drain_top_k(top, top_k);
}

void SearchEngine::Impl::fill_passages(std::vector<idx::SearchResult>& results) const {
    if (!collection_stream.is_open()) return;
    std::lock_guard lk(collection_mutex);
    for (auto& r : results) {
        if (r.doc_id < 0 || static_cast<std::size_t>(r.doc_id) >= line_positions.size()) {
            continue;
        }
        collection_stream.clear();
        collection_stream.seekg(line_positions[r.doc_id]);
        std::string line;
        if (std::getline(collection_stream, line)) {
            const auto tab = line.find('\t');
            r.passage = (tab == std::string::npos) ? line : line.substr(tab + 1);
            constexpr std::size_t kMaxLen = 512;
            if (r.passage.size() > kMaxLen) r.passage.resize(kMaxLen);
        }
    }
}

std::vector<idx::SearchResult> SearchEngine::Impl::search(std::string_view query,
                                                          SearchOptions opts) const {
    if (opts.top_k <= 0) return {};

    auto tokens = tokenize_query(query);

    std::vector<const LexEntry*> term_entries;
    term_entries.reserve(tokens.size());
    for (const auto& t : tokens) {
        const auto it = lexicon.find(t);
        if (it != lexicon.end()) term_entries.push_back(&it->second);
    }
    if (term_entries.empty()) return {};

    auto results = opts.conjunctive
                       ? rank_conjunctive(term_entries, opts.top_k)
                       : rank_disjunctive(term_entries, opts.top_k);
    if (opts.fill_passage) fill_passages(results);
    return results;
}

SearchEngine::SearchEngine(const SearchEnginePaths& paths, unsigned threads)
    : impl_(std::make_unique<Impl>(paths, threads)) {}

SearchEngine::~SearchEngine() = default;

std::vector<idx::SearchResult> SearchEngine::search(std::string_view query,
                                                    SearchOptions opts) {
    return impl_->search(query, opts);
}

std::vector<std::vector<idx::SearchResult>>
SearchEngine::search_batch(const std::vector<std::string>& queries,
                           SearchOptions opts) {
    std::vector<std::vector<idx::SearchResult>> out(queries.size());
    if (impl_->pool) {
        std::vector<std::future<void>> futs;
        futs.reserve(queries.size());
        std::exception_ptr first_error;
        try {
            for (std::size_t i = 0; i < queries.size(); ++i) {
                futs.push_back(impl_->pool->submit([this, i, &queries, opts, &out] {
                    out[i] = impl_->search(queries[i], opts);
                }));
            }
        } catch (...) {
            first_error = std::current_exception();
        }
        for (auto& f : futs) {
            try {
                f.get();
            } catch (...) {
                if (!first_error) first_error = std::current_exception();
            }
        }
        if (first_error) std::rethrow_exception(first_error);
    } else {
        for (std::size_t i = 0; i < queries.size(); ++i) {
            out[i] = impl_->search(queries[i], opts);
        }
    }
    return out;
}

int SearchEngine::total_docs() const noexcept { return impl_->total_docs; }
double SearchEngine::avg_doc_length() const noexcept { return impl_->avgdl; }

}  // namespace idx::query
