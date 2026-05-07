"""Parse `/usr/bin/time -l` output and emit a peak-RSS comparison table."""

from __future__ import annotations

import sys
from pathlib import Path


def parse_max_rss_bytes(text: str) -> int:
    for line in text.splitlines():
        if "maximum resident set size" not in line:
            continue
        parts = line.split()
        if parts:
            return int(parts[0])
    raise ValueError("maximum resident set size not found")


def fmt_bytes(n: int) -> str:
    for unit, scale in (("GB", 2**30), ("MB", 2**20), ("KB", 2**10)):
        if n >= scale:
            return f"{n / scale:.2f} {unit}"
    return f"{n} B"


def render_markdown(baseline_rss: int, experiment_rss: int) -> str:
    saved = (baseline_rss - experiment_rss) / baseline_rss * 100 if baseline_rss else 0.0
    return "\n".join([
        "# Peak RSS comparison",
        "",
        "| Build path | Peak RSS | Reduction |",
        "| ---------- | -------- | --------- |",
        f"| Default allocator | {fmt_bytes(baseline_rss)} | baseline |",
        f"| SlabArena + std::pmr | {fmt_bytes(experiment_rss)} | {saved:+.1f}% |",
        "",
    ])


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: extract_rss.py <baseline.log> <experiment.log>", file=sys.stderr)
        return 2
    try:
        baseline = parse_max_rss_bytes(Path(argv[1]).read_text())
        experiment = parse_max_rss_bytes(Path(argv[2]).read_text())
    except Exception as e:
        print(f"extract_rss.py: {e}", file=sys.stderr)
        return 1
    print(render_markdown(baseline, experiment), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
