// build_index CLI: thin wrapper around idx::build::build_index.

#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "builder.hpp"

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <collection.tsv> [output_dir=. ] [spill_threshold] [--stats-json <path>]\n"
              << "  collection.tsv  one document per line, '<pid>\\t<text>'\n"
              << "  output_dir      directory for index artefacts (created if missing)\n"
              << "  spill_threshold approx postings between spills (default 4194304)\n"
              << "  --stats-json    write build stats JSON for benchmark runs\n";
}

bool parse_size(std::string_view text, const char* name, std::size_t& out) {
    std::size_t value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value == 0) {
        std::cerr << "invalid " << name << ": " << text
                  << " (expected a positive integer)\n";
        return false;
    }
    out = value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::vector<std::string_view> positional;
    idx::build::BuildOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--stats-json") {
            if (i + 1 >= argc) {
                std::cerr << "--stats-json requires a path\n";
                return EXIT_FAILURE;
            }
            opts.stats_json_path = argv[++i];
            continue;
        }
        positional.push_back(arg);
    }

    if (positional.empty() || positional.size() > 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::filesystem::path input = positional[0];
    const std::filesystem::path output_dir = (positional.size() >= 2) ? positional[1] : ".";
    if (positional.size() >= 3 &&
        !parse_size(positional[2], "spill_threshold", opts.spill_threshold)) {
        return EXIT_FAILURE;
    }

    try {
        idx::build::build_index(input, output_dir, opts);
    } catch (const std::exception& e) {
        std::cerr << "build_index: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "build_index: ok\n";
    return EXIT_SUCCESS;
}
