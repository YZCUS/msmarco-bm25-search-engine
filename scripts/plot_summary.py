"""Render compression and peak-RSS bar charts for the benchmark report."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_rss import parse_max_rss_bytes


def posting_store_bytes(index_dir: Path) -> int:
    return (index_dir / "final_sorted_index.bin").stat().st_size + (
        index_dir / "final_sorted_block_info.bin"
    ).stat().st_size


def render_charts(varbyte_dir: Path, raw32_dir: Path, baseline_log: Path,
                  arena_log: Path, out_dir: Path) -> None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as e:
        raise RuntimeError("matplotlib is required; install rag_demo/requirements.txt") from e

    out_dir.mkdir(parents=True, exist_ok=True)
    raw_gb = posting_store_bytes(raw32_dir) / (1024 ** 3)
    var_gb = posting_store_bytes(varbyte_dir) / (1024 ** 3)

    plt.figure(figsize=(6, 4))
    plt.bar(["Raw32", "VarByte"], [raw_gb, var_gb], color=["#6b7280", "#2563eb"])
    plt.ylabel("Posting store (GB)")
    plt.title("Posting Store Compression")
    plt.grid(True, axis="y", alpha=0.25)
    plt.tight_layout()
    plt.savefig(out_dir / "compression_bar.png", dpi=160)
    plt.close()

    baseline_mb = parse_max_rss_bytes(baseline_log.read_text()) / (1024 ** 2)
    arena_mb = parse_max_rss_bytes(arena_log.read_text()) / (1024 ** 2)
    plt.figure(figsize=(6, 4))
    plt.bar(["Default", "SlabArena"], [baseline_mb, arena_mb],
            color=["#6b7280", "#0f766e"])
    plt.ylabel("Peak RSS (MB)")
    plt.title("Build Peak RSS")
    plt.grid(True, axis="y", alpha=0.25)
    plt.tight_layout()
    plt.savefig(out_dir / "rss_compare.png", dpi=160)
    plt.close()


def main(argv: list[str]) -> int:
    if len(argv) != 6:
        print(
            "usage: plot_summary.py <varbyte_dir> <raw32_dir> "
            "<baseline_log> <arena_log> <out_dir>",
            file=sys.stderr,
        )
        return 2
    try:
        render_charts(Path(argv[1]), Path(argv[2]), Path(argv[3]),
                      Path(argv[4]), Path(argv[5]))
    except Exception as e:
        print(f"plot_summary.py: {e}", file=sys.stderr)
        return 1
    print(f"wrote {Path(argv[5]) / 'compression_bar.png'} and {Path(argv[5]) / 'rss_compare.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
