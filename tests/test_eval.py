"""Regression tests for eval/run_eval.py helpers."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("run_eval", ROOT / "eval" / "run_eval.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot import eval/run_eval.py")
run_eval = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_eval)


class RunEvalTest(unittest.TestCase):
    def test_mrr_cut_10_uses_first_relevant_rank_only(self) -> None:
        qrels = {
            "q1": {"d3": 1},
            "q2": {"d9": 1},
            "q3": {"d1": 1},
        }
        ranked = {
            "q1": ["d1", "d2", "d3"],
            "q2": ["d0"] + [f"x{i}" for i in range(10)] + ["d9"],
            "q3": ["d1"],
        }
        self.assertAlmostEqual(run_eval.mrr_cut(qrels, ranked, 10), (1 / 3 + 0 + 1) / 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
