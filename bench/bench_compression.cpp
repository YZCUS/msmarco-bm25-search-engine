// bench_compression: print a side-by-side size comparison of two index
// directories, intended to be the VarByte build vs the Raw32 build.
//
// Usage:
//   bench_compression <varbyte_dir> <raw32_dir> [output.md]
//
// Output is a markdown table containing per-file bytes plus the aggregated
// posting-store size and the resulting compression ratio.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct DirStats {
    std::string label;
    fs::path dir;
    std::int64_t index_bytes = 0;
    std::int64_t blocks_bytes = 0;
    std::int64_t lexicon_bytes = 0;
    std::int64_t doc_info_bytes = 0;
};

std::int64_t safe_size(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return 0;
    return static_cast<std::int64_t>(fs::file_size(p, ec));
}

DirStats stat_dir(const std::string& label, const fs::path& dir) {
    return {
        label,
        dir,
        safe_size(dir / "final_sorted_index.bin"),
        safe_size(dir / "final_sorted_block_info.bin"),
        safe_size(dir / "final_sorted_lexicon.txt"),
        safe_size(dir / "document_info.txt"),
    };
}

std::string fmt_bytes(std::int64_t b) {
    constexpr std::int64_t kKB = 1024;
    constexpr std::int64_t kMB = 1024 * 1024;
    constexpr std::int64_t kGB = 1024LL * 1024 * 1024;
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    if (b >= kGB)      o << static_cast<double>(b) / kGB << " GB";
    else if (b >= kMB) o << static_cast<double>(b) / kMB << " MB";
    else if (b >= kKB) o << static_cast<double>(b) / kKB << " KB";
    else               o << b << " B";
    return o.str();
}

void emit_markdown(std::ostream& out, const DirStats& var, const DirStats& raw) {
    out << "# Compression Ratio\n\n"
        << "| Codec   | Index | Block info | Lexicon | Doc info | Posting store | vs Raw32 |\n"
        << "| ------- | ----- | ---------- | ------- | -------- | ------------- | -------- |\n";
    auto row = [&](const DirStats& s, double ratio) {
        const std::int64_t posting_store = s.index_bytes + s.blocks_bytes;
        out << "| " << s.label
            << " | " << fmt_bytes(s.index_bytes)
            << " | " << fmt_bytes(s.blocks_bytes)
            << " | " << fmt_bytes(s.lexicon_bytes)
            << " | " << fmt_bytes(s.doc_info_bytes)
            << " | " << fmt_bytes(posting_store)
            << " | ";
        out << std::fixed << std::setprecision(3) << ratio << "x |\n";
    };

    const std::int64_t raw_store = raw.index_bytes + raw.blocks_bytes;
    const std::int64_t var_store = var.index_bytes + var.blocks_bytes;
    const double var_ratio = (raw_store > 0) ? static_cast<double>(var_store) / raw_store : 0.0;
    const double raw_ratio = 1.0;

    row(raw, raw_ratio);
    row(var, var_ratio);

    if (raw_store > 0 && var_store < raw_store) {
        const double saved_pct = (1.0 - var_ratio) * 100.0;
        out << "\nVarByte saves " << std::fixed << std::setprecision(1) << saved_pct
            << "% of posting-store bytes versus Raw32.\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: bench_compression <varbyte_dir> <raw32_dir> [output.md]\n";
        return 1;
    }
    const fs::path var_dir = argv[1];
    const fs::path raw_dir = argv[2];
    const auto var = stat_dir("VarByte", var_dir);
    const auto raw = stat_dir("Raw32",   raw_dir);

    if (var.index_bytes == 0 || raw.index_bytes == 0) {
        std::cerr << "missing final_sorted_index.bin in one of the input directories\n";
        return 2;
    }

    if (argc >= 4) {
        std::ofstream f(argv[3]);
        if (!f) { std::cerr << "cannot open " << argv[3] << '\n'; return 3; }
        emit_markdown(f, var, raw);
        std::cerr << "wrote " << argv[3] << '\n';
    }
    emit_markdown(std::cout, var, raw);
    return 0;
}
