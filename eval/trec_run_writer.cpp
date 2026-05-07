// trec_run_writer: read <qid>\t<query> per line, run disjunctive top-k
// against SearchEngine, emit a TREC-format run file:
//
//   <qid> Q0 <docid> <rank> <score> <tag>
//
// Use scripts/eval_all.sh + eval/run_eval.py to turn this into MRR / nDCG /
// Recall metrics via pytrec_eval.

#include <cstdint>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "search_engine.hpp"

namespace {

struct Args {
    std::string index, lexicon, blocks, doc_info;
    std::string queries;
    std::string out;
    std::string tag = "IDX_BM25";
    int top_k = 1000;
    int threads = 0;
};

constexpr int kMaxTopK = 100000;
constexpr int kMaxThreads = 256;

void usage(const char* p) {
    std::cerr <<
        "Usage: " << p << " --index <p> --lexicon <p> --blocks <p>\n"
        "                  --doc-info <p> --queries <queries.tsv>\n"
        "                  --out <run.txt> [--top-k 1000] [--tag IDX_BM25]\n"
        "                  [--threads N]\n";
}

bool parse_int(std::string_view text, const char* name, int min_value,
               int max_value, int& out) {
    int value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value < min_value || value > max_value) {
        std::cerr << "invalid " << name << ": " << text
                  << " (expected " << min_value << ".." << max_value << ")\n";
        return false;
    }
    out = value;
    return true;
}

bool parse(int argc, char** argv, Args& a) {
    auto need = [&](int& i) { if (i + 1 >= argc) { usage(argv[0]); return false; } return true; };
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "--index"    && need(i)) a.index    = argv[++i];
        else if (s == "--lexicon"  && need(i)) a.lexicon  = argv[++i];
        else if (s == "--blocks"   && need(i)) a.blocks   = argv[++i];
        else if (s == "--doc-info" && need(i)) a.doc_info = argv[++i];
        else if (s == "--queries"  && need(i)) a.queries  = argv[++i];
        else if (s == "--out"      && need(i)) a.out      = argv[++i];
        else if (s == "--tag"      && need(i)) a.tag      = argv[++i];
        else if (s == "--top-k"    && need(i)) {
            if (!parse_int(argv[++i], "--top-k", 1, kMaxTopK, a.top_k)) return false;
        }
        else if (s == "--threads"  && need(i)) {
            if (!parse_int(argv[++i], "--threads", 0, kMaxThreads, a.threads)) return false;
        }
        else { usage(argv[0]); return false; }
    }
    return !a.index.empty() && !a.queries.empty() && !a.out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) return EXIT_FAILURE;

    idx::query::SearchEnginePaths paths{args.index, args.lexicon, args.blocks,
                                        args.doc_info, ""};
    idx::query::SearchEngine engine(paths, static_cast<unsigned>(args.threads));

    std::ifstream in(args.queries);
    if (!in) { std::cerr << "cannot open " << args.queries << '\n'; return 2; }
    std::ofstream out(args.out);
    if (!out) { std::cerr << "cannot open " << args.out << '\n'; return 2; }

    std::vector<std::string> qids;
    std::vector<std::string> qs;
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        qids.push_back(line.substr(0, tab));
        qs.push_back(line.substr(tab + 1));
    }
    if (qs.empty()) {
        std::cerr << "no queries\n";
        return 2;
    }

    const auto results = engine.search_batch(
        qs, {.top_k = args.top_k, .conjunctive = false});

    out << std::fixed << std::setprecision(6);
    for (std::size_t i = 0; i < results.size(); ++i) {
        for (const auto& r : results[i]) {
            out << qids[i] << " Q0 " << r.doc_id << ' ' << r.rank
                << ' ' << r.score << ' ' << args.tag << '\n';
        }
    }
    std::cerr << "wrote " << args.out << " (" << qs.size() << " queries)\n";
    return 0;
}
