// build_index CLI: thin wrapper around idx::build::build_index.

#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#include "builder.hpp"

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <collection.tsv> [output_dir=. ] [spill_threshold]\n"
              << "  collection.tsv  one document per line, '<pid>\\t<text>'\n"
              << "  output_dir      directory for index artefacts (created if missing)\n"
              << "  spill_threshold approx postings between spills (default 4194304)\n";
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
    const std::filesystem::path input = argv[1];
    const std::filesystem::path output_dir = (argc >= 3) ? argv[2] : ".";
    idx::build::BuildOptions opts;
    if (argc >= 4 && !parse_size(argv[3], "spill_threshold", opts.spill_threshold)) {
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
