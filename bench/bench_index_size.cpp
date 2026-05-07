// bench_index_size: report raw collection size, index artefact sizes, and
// the resulting compression ratio against the original collection.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::int64_t safe_size(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return 0;
    return static_cast<std::int64_t>(fs::file_size(p, ec));
}

std::string fmt_bytes(std::int64_t b) {
    constexpr std::int64_t kKB = 1024;
    constexpr std::int64_t kMB = 1024 * 1024;
    constexpr std::int64_t kGB = 1024LL * 1024 * 1024;
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    if      (b >= kGB) o << static_cast<double>(b) / kGB << " GB";
    else if (b >= kMB) o << static_cast<double>(b) / kMB << " MB";
    else if (b >= kKB) o << static_cast<double>(b) / kKB << " KB";
    else               o << b << " B";
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: bench_index_size <collection.tsv> <index_dir> [output.md]\n";
        return 1;
    }
    const fs::path collection = argv[1];
    const fs::path dir        = argv[2];

    const std::int64_t coll_bytes = safe_size(collection);
    const std::int64_t index_bytes = safe_size(dir / "final_sorted_index.bin");
    const std::int64_t blocks_bytes = safe_size(dir / "final_sorted_block_info.bin");
    const std::int64_t lex_bytes = safe_size(dir / "final_sorted_lexicon.txt");
    const std::int64_t doc_bytes = safe_size(dir / "document_info.txt");
    const std::int64_t total_index = index_bytes + blocks_bytes + lex_bytes + doc_bytes;

    std::ostringstream out;
    out << "# Index Size\n\n"
        << "| Artifact | Size |\n"
        << "| -------- | ---- |\n"
        << "| collection.tsv | " << fmt_bytes(coll_bytes) << " |\n"
        << "| final_sorted_index.bin | " << fmt_bytes(index_bytes) << " |\n"
        << "| final_sorted_block_info.bin | " << fmt_bytes(blocks_bytes) << " |\n"
        << "| final_sorted_lexicon.txt | " << fmt_bytes(lex_bytes) << " |\n"
        << "| document_info.txt | " << fmt_bytes(doc_bytes) << " |\n"
        << "| **Total index** | " << fmt_bytes(total_index) << " |\n"
        << '\n';
    if (coll_bytes > 0) {
        out << std::fixed << std::setprecision(3)
            << "Total index is "
            << static_cast<double>(total_index) / coll_bytes
            << "x the raw collection size.\n";
    }

    std::cout << out.str();
    if (argc >= 4) {
        std::ofstream f(argv[3]);
        f << out.str();
    }
    return 0;
}
