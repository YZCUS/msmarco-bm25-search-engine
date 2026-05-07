"""End-to-end Python test for the RAG retrieval IPC layer.

Spins up the real build_index + search_cli binaries against a tiny synthetic
corpus and verifies that:

  1. Retriever can talk to search_cli's --server JSONL protocol without
     deadlocking.
  2. Retrieval results contain the expected doc_id and a non-empty passage.
  3. rag_demo.build_prompt produces a citation-friendly prompt.

The binary paths can be overridden via environment variables; defaults
assume CMake's `build/` layout. This script is wired into CTest with the
proper environment so the C++ binaries are picked up automatically.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

THIS = Path(__file__).resolve()
ROOT = THIS.parent.parent
sys.path.insert(0, str(ROOT))

from rag_demo.retriever import Retriever      # noqa: E402
from rag_demo.rag_demo import build_prompt    # noqa: E402


def _binary(name: str) -> Path:
    env = os.environ.get("IDX_BUILD_DIR")
    if env:
        return Path(env) / name
    return ROOT / "build" / name


COLLECTION = """\
0\tthe quick brown fox jumps over the lazy dog
1\ta journey of a thousand miles begins with a single step
2\tbm25 is a ranking function used by search engines
3\tthe inverted index maps terms to documents
4\tparallel queries scale across multiple cores efficiently
5\tretrieval augmented generation pairs a search engine with a large language model
"""


class RagPipelineTest(unittest.TestCase):

    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="idx_rag_test_"))
        self.collection = self.work / "collection.tsv"
        self.collection.write_text(COLLECTION)

        build_index = _binary("build_index")
        search_cli = _binary("search_cli")
        for b in (build_index, search_cli):
            self.assertTrue(b.exists(), f"missing binary: {b}; "
                            "set IDX_BUILD_DIR or run cmake --build first")

        # Build the index in the temp dir.
        subprocess.run([str(build_index), str(self.collection), str(self.work)],
                       check=True, capture_output=True, text=True)
        for f in ["final_sorted_index.bin", "final_sorted_block_info.bin",
                  "final_sorted_lexicon.txt", "document_info.txt"]:
            self.assertTrue((self.work / f).exists(), f"missing artefact: {f}")

        self.search_cli = search_cli

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def _retriever(self) -> Retriever:
        return Retriever(
            self.search_cli,
            index=str(self.work / "final_sorted_index.bin"),
            lexicon=str(self.work / "final_sorted_lexicon.txt"),
            blocks=str(self.work / "final_sorted_block_info.bin"),
            doc_info=str(self.work / "document_info.txt"),
            collection=str(self.collection),
        )

    def test_single_query_returns_expected_top_doc(self) -> None:
        with self._retriever() as r:
            results = r.search("bm25 ranking", k=3)
        self.assertGreaterEqual(len(results), 1)
        self.assertEqual(results[0]["doc_id"], 2)
        self.assertIn("bm25", results[0]["passage"].lower())
        self.assertGreater(results[0]["score"], 0.0)
        self.assertEqual(results[0]["rank"], 1)

    def test_multiple_queries_share_one_subprocess(self) -> None:
        with self._retriever() as r:
            r1 = r.search("inverted index", k=2)
            r2 = r.search("retrieval augmented generation", k=2)
            r3 = r.search("parallel cores", k=2)
        self.assertEqual(r1[0]["doc_id"], 3)
        self.assertEqual(r2[0]["doc_id"], 5)
        self.assertEqual(r3[0]["doc_id"], 4)

    def test_unknown_term_returns_empty_results(self) -> None:
        with self._retriever() as r:
            results = r.search("xyzzy_no_such_word", k=5)
        self.assertEqual(results, [])

    def test_build_prompt_includes_citations(self) -> None:
        passages = [
            {"doc_id": 2, "passage": "bm25 is a ranking function", "score": 1.0, "rank": 1},
            {"doc_id": 3, "passage": "inverted index maps terms",  "score": 0.5, "rank": 2},
        ]
        prompt = build_prompt("what is bm25", passages, max_chars=128)
        self.assertIn("[2]", prompt)
        self.assertIn("[3]", prompt)
        self.assertIn("Question: what is bm25", prompt)
        self.assertIn("Cite each factual statement", prompt)


if __name__ == "__main__":
    unittest.main(verbosity=2)
