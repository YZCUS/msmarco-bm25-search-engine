// search_cli: interactive REPL + line-delimited JSON server for SearchEngine.
//
// Two modes share one engine:
//
//   default (interactive): reads queries from stdin, prints a numbered list
//   of (rank, score, doc_id, passage-prefix) hits to stdout.
//
//   --server: reads one JSON object per line from stdin and writes one JSON
//   reply per line to stdout. Schema:
//
//     request:  {"q": "...", "k": 10, "mode": "disjunctive" | "conjunctive"}
//     response: {"q": "...", "results": [{"rank":1,"doc_id":...,
//                                          "score":...,"passage":"..."}]}
//     error:    {"error": "<message>"}
//
// All progress / debug logging goes to stderr so the JSONL channel stays
// pristine for the Python wrapper.

#include <cstdlib>
#include <cstring>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "search_engine.hpp"
#include "third_party/json.hpp"

namespace {

struct Args {
    std::string index;
    std::string lexicon;
    std::string blocks;
    std::string doc_info;
    std::string collection;
    int top_k = 10;
    bool conjunctive = false;
    bool server = false;
    unsigned threads = 0;
};

constexpr int kMaxTopK = 100000;
constexpr int kMaxThreads = 256;

void print_usage(const char* argv0) {
    std::cerr <<
        "Usage: " << argv0 << " --index <p> --lexicon <p> --blocks <p>\n"
        "                  --doc-info <p> [--collection <p>]\n"
        "                  [--top-k 10] [--conjunctive] [--server]\n"
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

bool parse_args(int argc, char** argv, Args& args) {
    auto need = [&](int& i) {
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << argv[i] << '\n';
            return false;
        }
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--index"      && need(i)) args.index      = argv[++i];
        else if (a == "--lexicon"    && need(i)) args.lexicon    = argv[++i];
        else if (a == "--blocks"     && need(i)) args.blocks     = argv[++i];
        else if (a == "--doc-info"   && need(i)) args.doc_info   = argv[++i];
        else if (a == "--collection" && need(i)) args.collection = argv[++i];
        else if (a == "--top-k"      && need(i)) {
            if (!parse_int(argv[++i], "--top-k", 1, kMaxTopK, args.top_k)) return false;
        }
        else if (a == "--threads"    && need(i)) {
            int n = 0;
            if (!parse_int(argv[++i], "--threads", 0, kMaxThreads, n)) return false;
            args.threads = static_cast<unsigned>(n);
        }
        else if (a == "--conjunctive") args.conjunctive = true;
        else if (a == "--server")      args.server      = true;
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return false; }
        else { std::cerr << "unknown arg: " << a << '\n'; print_usage(argv[0]); return false; }
    }
    if (args.index.empty() || args.lexicon.empty() ||
        args.blocks.empty() || args.doc_info.empty()) {
        print_usage(argv[0]);
        return false;
    }
    return true;
}

idx::query::SearchEnginePaths paths_from(const Args& a) {
    return {a.index, a.lexicon, a.blocks, a.doc_info, a.collection};
}

nlohmann::json results_to_json(std::string_view q,
                               const std::vector<idx::SearchResult>& results) {
    nlohmann::json out;
    out["q"] = q;
    out["results"] = nlohmann::json::array();
    for (const auto& r : results) {
        out["results"].push_back({
            {"rank", r.rank},
            {"doc_id", r.doc_id},
            {"score", r.score},
            {"passage", r.passage},
        });
    }
    return out;
}

int run_server(idx::query::SearchEngine& engine, const Args& args) {
    std::ios::sync_with_stdio(false);
    std::cout.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nlohmann::json resp;
        try {
            const auto req = nlohmann::json::parse(line);
            const std::string q = req.value("q", "");
            const int k = req.value("k", args.top_k);
            if (k < 1 || k > kMaxTopK) {
                throw std::invalid_argument("k must be between 1 and 100000");
            }
            const std::string mode = req.value("mode", std::string{"disjunctive"});
            if (mode != "disjunctive" && mode != "conjunctive") {
                throw std::invalid_argument("mode must be disjunctive or conjunctive");
            }
            const bool conj = mode == "conjunctive";
            const auto results = engine.search(
                q, {.top_k = k, .conjunctive = conj, .fill_passage = !args.collection.empty()});
            resp = results_to_json(q, results);
        } catch (const std::exception& e) {
            resp = {{"error", std::string("server: ") + e.what()}};
        }
        std::cout << resp.dump() << '\n';
        std::cout.flush();
    }
    return 0;
}

int run_interactive(idx::query::SearchEngine& engine, const Args& args) {
    std::cout << "search_cli interactive: type a query and press enter, 'q' to quit.\n";
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line == "q" || line == "quit") break;
        if (line.empty()) continue;

        const auto results = engine.search(
            line,
            {.top_k = args.top_k, .conjunctive = args.conjunctive,
             .fill_passage = !args.collection.empty()});

        if (results.empty()) {
            std::cout << "  (no results)\n";
            continue;
        }
        for (const auto& r : results) {
            std::cout << "  " << r.rank << ". score=" << r.score
                      << " doc_id=" << r.doc_id;
            if (!r.passage.empty()) {
                std::string snippet = r.passage;
                if (snippet.size() > 120) snippet.resize(120);
                std::cout << "  | " << snippet;
            }
            std::cout << '\n';
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return EXIT_FAILURE;

    try {
        idx::query::SearchEngine engine(paths_from(args), args.threads);
        return args.server ? run_server(engine, args) : run_interactive(engine, args);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
