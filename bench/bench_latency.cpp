// bench_latency: replay queries through SearchEngine and report P50/P95/P99
// latency plus optional batch throughput mode (parallel queries via search_batch).
//
// Usage:
//   bench_latency --index <p> --lexicon <p> --blocks <p> --doc-info <p>
//                 --queries <queries.tsv> [--threads 1] [--top-k 10]
//                 [--repeat 1] [--limit N] [--offset N]
//                 [--progress-every N]
//                 [--mode latency|throughput] [--throughput-chunk N]
//                 [--csv out.csv]
//
// `queries.tsv` is the standard MS MARCO format: <qid>\t<query>. The qid
// column is ignored.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
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
    std::string index, lexicon, blocks, doc_info, queries, csv;
    std::string mode = "latency";  // latency | throughput
    int threads = 1;
    int top_k = 10;
    int repeat = 1;
    int limit = 0;            // 0 = all queries
    int offset = 0;
    int progress_every = 0;   // 0 = quiet
    int throughput_chunk = 0;   // 0 = one giant search_batch; else max queries per batch
};

constexpr int kMaxTopK = 100000;
constexpr int kMaxThreads = 256;
constexpr int kMaxRepeat = 100000;

void usage(const char* p) {
    std::cerr <<
        "Usage: " << p << " --index <p> --lexicon <p> --blocks <p>\n"
        "                  --doc-info <p> --queries <queries.tsv>\n"
        "                  [--threads N] [--top-k 10] [--repeat 1]\n"
        "                  [--limit N] [--offset N] [--progress-every N]\n"
        "                  [--mode latency|throughput]\n"
        "                  [--throughput-chunk N]   (throughput mode; 0 = all at once)\n"
        "                  [--csv out.csv]\n";
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
        else if (s == "--csv"      && need(i)) a.csv      = argv[++i];
        else if (s == "--threads"  && need(i)) {
            if (!parse_int(argv[++i], "--threads", 0, kMaxThreads, a.threads)) return false;
        }
        else if (s == "--top-k"    && need(i)) {
            if (!parse_int(argv[++i], "--top-k", 1, kMaxTopK, a.top_k)) return false;
        }
        else if (s == "--repeat"   && need(i)) {
            if (!parse_int(argv[++i], "--repeat", 1, kMaxRepeat, a.repeat)) return false;
        }
        else if (s == "--limit"    && need(i)) {
            if (!parse_int(argv[++i], "--limit", 0, INT32_MAX, a.limit)) return false;
        }
        else if (s == "--offset"   && need(i)) {
            if (!parse_int(argv[++i], "--offset", 0, INT32_MAX, a.offset)) return false;
        }
        else if (s == "--progress-every" && need(i)) {
            if (!parse_int(argv[++i], "--progress-every", 0, INT32_MAX, a.progress_every)) return false;
        }
        else if (s == "--mode"           && need(i)) a.mode = argv[++i];
        else if (s == "--throughput-chunk" && need(i)) {
            if (!parse_int(argv[++i], "--throughput-chunk", 0, INT32_MAX,
                           a.throughput_chunk)) return false;
        }
        else { usage(argv[0]); return false; }
    }
    return !a.index.empty() && !a.queries.empty();
}

std::vector<std::string> load_queries(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<std::string> qs;
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        qs.push_back(tab == std::string::npos ? line : line.substr(tab + 1));
    }
    return qs;
}

double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    const std::size_t i = std::min<std::size_t>(static_cast<std::size_t>(p * v.size()), v.size() - 1);
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) return EXIT_FAILURE;

    idx::query::SearchEnginePaths paths{args.index, args.lexicon, args.blocks,
                                        args.doc_info, ""};
    idx::query::SearchEngine engine(paths, static_cast<unsigned>(args.threads));
    if (args.mode != "latency" && args.mode != "throughput") {
        std::cerr << "unknown --mode (use latency or throughput)\n";
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    auto queries = load_queries(args.queries);
    if (queries.empty()) {
        std::cerr << "no queries loaded from " << args.queries << '\n';
        return EXIT_FAILURE;
    }
    if (args.offset > 0 || args.limit > 0) {
        const std::size_t begin = std::min<std::size_t>(args.offset, queries.size());
        const std::size_t end = (args.limit > 0)
            ? std::min<std::size_t>(begin + static_cast<std::size_t>(args.limit), queries.size())
            : queries.size();
        queries = std::vector<std::string>(queries.begin() + begin, queries.begin() + end);
    }
    if (queries.empty()) {
        std::cerr << "query slice is empty\n";
        return EXIT_FAILURE;
    }

    idx::query::SearchOptions opts{.top_k = args.top_k, .conjunctive = false};

    // Warm-up: page-fault the lexicon hash and the most-touched blocks.
    constexpr int kWarmup = 20;
    const std::vector<std::string> warmup_pool(
        queries.begin(), queries.begin() + std::min<std::size_t>(queries.size(), 200));
    for (int i = 0; i < kWarmup; ++i) {
        (void)engine.search(queries[i % queries.size()], opts);
    }
    if (args.mode == "throughput") {
        std::vector<std::string> wq(warmup_pool.begin(),
                                    warmup_pool.begin() +
                                        std::min<std::size_t>(warmup_pool.size(), 32));
        (void)engine.search_batch(wq, opts);
    }

    std::vector<double> latencies_us;
    latencies_us.reserve(queries.size() * args.repeat);

    const auto t0 = std::chrono::steady_clock::now();
    if (args.mode == "throughput") {
        std::vector<std::string> chunk;
        chunk.reserve(queries.empty() ? 0
                      : (args.throughput_chunk > 0
                             ? static_cast<std::size_t>(args.throughput_chunk)
                             : queries.size()));
        for (int rep = 0; rep < args.repeat; ++rep) {
            if (args.throughput_chunk <= 0) {
                const auto a0 = std::chrono::steady_clock::now();
                (void)engine.search_batch(queries, opts);
                const auto a1 = std::chrono::steady_clock::now();
                latencies_us.push_back(
                    std::chrono::duration<double, std::micro>(a1 - a0).count());
            } else {
                const auto a0 = std::chrono::steady_clock::now();
                const std::size_t step = static_cast<std::size_t>(args.throughput_chunk);
                for (std::size_t off = 0; off < queries.size(); off += step) {
                    const std::size_t end = std::min(off + step, queries.size());
                    chunk.assign(queries.begin() + off, queries.begin() + end);
                    (void)engine.search_batch(chunk, opts);
                    if (args.progress_every > 0 && (end % static_cast<std::size_t>(args.progress_every) == 0 ||
                                                    end == queries.size())) {
                        std::cerr << "[bench_latency] rep=" << rep
                                  << " processed=" << end << "/" << queries.size() << '\n';
                    }
                }
                const auto a1 = std::chrono::steady_clock::now();
                latencies_us.push_back(
                    std::chrono::duration<double, std::micro>(a1 - a0).count());
            }
        }
    } else {
        std::vector<std::string> chunk;
        const std::size_t step = static_cast<std::size_t>(std::max(1, args.threads));
        chunk.reserve(step);
        for (int rep = 0; rep < args.repeat; ++rep) {
            for (std::size_t i = 0; i < queries.size(); i += step) {
                const std::size_t end = std::min(i + step, queries.size());
                chunk.assign(queries.begin() + i, queries.begin() + end);
                const auto a0 = std::chrono::steady_clock::now();
                (void)engine.search_batch(chunk, opts);
                const auto a1 = std::chrono::steady_clock::now();
                const double per_query_us =
                    std::chrono::duration<double, std::micro>(a1 - a0).count()
                    / static_cast<double>(chunk.size());
                for (std::size_t j = i; j < end; ++j) {
                    latencies_us.push_back(per_query_us);
                }
                if (args.progress_every > 0 &&
                    (end % static_cast<std::size_t>(args.progress_every) == 0 ||
                     end == queries.size())) {
                    std::cerr << "[bench_latency] rep=" << rep
                              << " processed=" << end
                              << "/" << queries.size() << '\n';
                }
            }
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_s = std::chrono::duration<double>(t1 - t0).count();
    const double qps = (queries.size() * args.repeat) / total_s;

    const double p50 = pct(latencies_us, 0.50);
    const double p95 = pct(latencies_us, 0.95);
    const double p99 = pct(latencies_us, 0.99);

    std::cout << std::fixed << std::setprecision(3)
              << "threads=" << args.threads
              << " mode=" << args.mode
              << " queries=" << queries.size() * args.repeat;

    if (args.mode == "throughput") {
        // latencies_us stores one batch wall interval (microseconds) per repeat when
        // throughput_chunk<=0; when chunked it stores aggregate wall-us per repeat.
        std::cout << " repeats=" << args.repeat << " throughput_qps=" << qps
                  << " batch_P50_us=" << p50 << " batch_P95_us=" << p95 << " batch_P99_us="
                  << p99;
        if (args.throughput_chunk > 0) {
            std::cout << " throughput_chunk=" << args.throughput_chunk;
        }
    } else {
        std::cout << " per_query_qps=" << qps << " P50_us=" << p50 << " P95_us=" << p95
                  << " P99_us=" << p99;
    }
    std::cout << '\n';

    if (!args.csv.empty()) {
        std::ofstream f(args.csv);
        f << "threads,mode,qps,p50_us,p95_us,p99_us,throughput_chunk\n";
        f << args.threads << ',' << args.mode << ',' << qps << ',' << p50 << ',' << p95 << ','
          << p99 << ',' << args.throughput_chunk << '\n';
    }
    return 0;
}
