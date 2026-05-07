"""Regression tests for benchmark helper scripts."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_script(name: str):
    path = ROOT / "scripts" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


extract_rss = load_script("extract_rss")
plot_latency = load_script("plot_latency")
plot_summary = load_script("plot_summary")


class ExtractRssTest(unittest.TestCase):
    def test_parse_macos_time_output(self) -> None:
        text = "\n".join([
            "123.45 real",
            "  987654321  maximum resident set size",
        ])
        self.assertEqual(extract_rss.parse_max_rss_bytes(text), 987654321)

    def test_render_markdown_reports_reduction(self) -> None:
        md = extract_rss.render_markdown(1000, 750)
        self.assertIn("Vector partial index", md)
        self.assertIn("Compact partial index", md)
        self.assertIn("25.0%", md)

    def test_render_json_reports_reduction(self) -> None:
        text = extract_rss.render_json(1000, 750)
        self.assertIn('"reduction_percent": 25.0', text)
        self.assertIn("Compact partial index", text)


class PlotLatencyTest(unittest.TestCase):
    def test_load_latency_rows_accepts_plan_csv_shape(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "latency_t4.csv"
            p.write_text("threads,qps,p50_us,p95_us,p99_us\n4,10,100,200,300\n")
            rows = plot_latency.load_latency_rows(Path(d))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["threads"], 4)
        self.assertEqual(rows[0]["p99_us"], 300.0)


class PlotSummaryTest(unittest.TestCase):
    def test_posting_store_bytes_sums_index_and_blocks(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "final_sorted_index.bin").write_bytes(b"a" * 5)
            (root / "final_sorted_block_info.bin").write_bytes(b"b" * 7)
            self.assertEqual(plot_summary.posting_store_bytes(root), 12)


if __name__ == "__main__":
    unittest.main(verbosity=2)
