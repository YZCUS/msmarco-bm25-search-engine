"""Evaluate a TREC run file with pytrec_eval and emit aggregate metrics.

Usage:
    python eval/run_eval.py <qrels_path> <run_path> [--out metrics.json]

Computes MRR@10, nDCG@10 and Recall@1000 (as well as plain MRR / MAP) and
prints a JSON summary to stdout. Optionally also writes the same JSON to
the path given by --out.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Dict, List


def parse_qrels(path: Path) -> Dict[str, Dict[str, int]]:
    """Standard TREC qrels: <qid> 0 <docid> <rel> per line."""
    out: Dict[str, Dict[str, int]] = collections.defaultdict(dict)
    with path.open() as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            qid, _, did, rel = parts
            out[qid][did] = int(rel)
    return out


def parse_run(path: Path) -> Dict[str, Dict[str, float]]:
    """TREC run file: <qid> Q0 <docid> <rank> <score> <tag>."""
    out: Dict[str, Dict[str, float]] = collections.defaultdict(dict)
    with path.open() as f:
        for line in f:
            parts = line.split()
            if len(parts) != 6:
                continue
            qid, _, did, _rank, score, _tag = parts
            out[qid][did] = float(score)
    return out


def parse_run_ranked(path: Path) -> Dict[str, List[str]]:
    """Return docids ordered by TREC rank for each qid."""
    tmp: Dict[str, list[tuple[int, str]]] = collections.defaultdict(list)
    with path.open() as f:
        for line in f:
            parts = line.split()
            if len(parts) != 6:
                continue
            qid, _, did, rank, _score, _tag = parts
            tmp[qid].append((int(rank), did))
    return {qid: [did for _rank, did in sorted(items)] for qid, items in tmp.items()}


def mrr_cut(qrels: Dict[str, Dict[str, int]], ranked_run: Dict[str, List[str]],
            cutoff: int) -> float:
    if not qrels:
        return 0.0
    total = 0.0
    for qid, rels in qrels.items():
        relevant = {did for did, rel in rels.items() if rel > 0}
        rr = 0.0
        for rank, did in enumerate(ranked_run.get(qid, [])[:cutoff], start=1):
            if did in relevant:
                rr = 1.0 / rank
                break
        total += rr
    return total / len(qrels)


def aggregate(per_q: Dict[str, Dict[str, float]]) -> Dict[str, float]:
    if not per_q:
        return {}
    metrics = list(next(iter(per_q.values())).keys())
    out: Dict[str, float] = {}
    for m in metrics:
        vals = [per_q[q][m] for q in per_q]
        out[m] = sum(vals) / len(vals)
    return out


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qrels", type=Path)
    parser.add_argument("run", type=Path)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument(
        "--metrics",
        nargs="+",
        default=["ndcg_cut.10", "recall.1000", "map", "recip_rank"],
    )
    args = parser.parse_args(argv)

    if not args.qrels.exists() or not args.run.exists():
        print(f"missing input: {args.qrels} or {args.run}", file=sys.stderr)
        return 2

    try:
        import pytrec_eval  # type: ignore
    except ImportError:
        print(
            "pytrec_eval is not installed. Run `pip install pytrec_eval` "
            "(or `pip install -r rag_demo/requirements.txt`).",
            file=sys.stderr,
        )
        return 3

    qrels = parse_qrels(args.qrels)
    run = parse_run(args.run)
    ranked_run = parse_run_ranked(args.run)

    pytrec_metrics = {m for m in args.metrics if m != "recip_rank_cut.10"}
    evaluator = pytrec_eval.RelevanceEvaluator(qrels, pytrec_metrics)
    per_q = evaluator.evaluate(run)
    summary = aggregate(per_q)
    summary["mrr_cut_10"] = mrr_cut(qrels, ranked_run, 10)
    summary["num_queries"] = len(per_q)

    payload = {
        "qrels": str(args.qrels),
        "run": str(args.run),
        "metrics": summary,
    }
    print(json.dumps(payload, indent=2))
    if args.out:
        args.out.write_text(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
