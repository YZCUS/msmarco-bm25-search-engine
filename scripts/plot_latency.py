"""Plot latency and QPS charts from bench_latency summary CSV outputs."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path


def load_latency_rows(csv_dir: Path) -> list[dict[str, float | int | str]]:
    rows: list[dict[str, float | int | str]] = []
    paths = sorted(csv_dir.glob("latency_t*.csv"))
    canonical = [p for p in paths if re.fullmatch(r"latency_t\d+\.csv", p.name)]
    if canonical:
        paths = canonical
    for path in paths:
        with path.open(newline="") as f:
            reader = csv.DictReader(f)
            for raw in reader:
                if not raw:
                    continue
                row: dict[str, float | int | str] = {
                    "source": path.name,
                    "threads": int(raw["threads"]),
                    "qps": float(raw["qps"]),
                    "p50_us": float(raw["p50_us"]),
                    "p95_us": float(raw["p95_us"]),
                    "p99_us": float(raw["p99_us"]),
                }
                rows.append(row)
    rows.sort(key=lambda r: int(r["threads"]))
    return rows


def render_charts(rows: list[dict[str, float | int | str]], out_dir: Path) -> None:
    if not rows:
        raise ValueError("no latency_t*.csv rows found")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as e:
        raise RuntimeError("matplotlib is required; install rag_demo/requirements.txt") from e

    out_dir.mkdir(parents=True, exist_ok=True)
    threads = [int(r["threads"]) for r in rows]
    p50 = [float(r["p50_us"]) / 1000.0 for r in rows]
    p95 = [float(r["p95_us"]) / 1000.0 for r in rows]
    p99 = [float(r["p99_us"]) / 1000.0 for r in rows]
    qps = [float(r["qps"]) for r in rows]

    plt.figure(figsize=(7, 4))
    plt.plot(threads, p50, marker="o", label="P50")
    plt.plot(threads, p95, marker="o", label="P95")
    plt.plot(threads, p99, marker="o", label="P99")
    plt.xlabel("Threads")
    plt.ylabel("Latency (ms)")
    plt.title("Query Latency Percentiles")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "latency_cdf.png", dpi=160)
    plt.close()

    plt.figure(figsize=(7, 4))
    plt.bar([str(t) for t in threads], qps)
    plt.xlabel("Threads")
    plt.ylabel("QPS")
    plt.title("QPS vs Threads")
    plt.grid(True, axis="y", alpha=0.25)
    plt.tight_layout()
    plt.savefig(out_dir / "qps_vs_threads.png", dpi=160)
    plt.close()


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: plot_latency.py <csv_dir> [out_dir]", file=sys.stderr)
        return 2
    csv_dir = Path(argv[1])
    out_dir = Path(argv[2]) if len(argv) >= 3 else Path("docs")
    try:
        rows = load_latency_rows(csv_dir)
        render_charts(rows, out_dir)
    except Exception as e:
        print(f"plot_latency.py: {e}", file=sys.stderr)
        return 1
    print(f"wrote {out_dir / 'latency_cdf.png'} and {out_dir / 'qps_vs_threads.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
