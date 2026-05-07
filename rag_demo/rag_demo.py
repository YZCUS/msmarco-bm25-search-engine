"""End-to-end RAG demo: query -> top-k passages from the C++ search backend
-> LLM with citations.

Backends:
    --backend mock    : do not call any LLM. Print retrieved passages and the
                        composed prompt. Useful for tests / CI.
    --backend ollama  : call a local Ollama server. Requires
                        `pip install ollama` and `ollama pull <model>`.
    --backend openai  : call OpenAI. Requires OPENAI_API_KEY in the
                        environment and `pip install openai`.

Example:
    python -m rag_demo.rag_demo --q "what is bm25" \\
        --search-cli ./build/search_cli \\
        --index data/index/final_sorted_index.bin \\
        --lexicon data/index/final_sorted_lexicon.txt \\
        --blocks data/index/final_sorted_block_info.bin \\
        --doc-info data/index/document_info.txt \\
        --collection data/collection.tsv
"""

from __future__ import annotations

import argparse
import os
import sys
import textwrap
from typing import Any, Sequence

from .retriever import Retriever


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--q", required=True, help="user question")
    p.add_argument("--top-k", type=int, default=5)
    p.add_argument("--backend", choices=["mock", "ollama", "openai"], default="mock")
    p.add_argument("--model", default=None,
                   help="LLM model name (defaults: qwen2.5:7b for ollama, "
                        "gpt-4o-mini for openai)")
    p.add_argument("--max-passage-chars", type=int, default=512)
    p.add_argument("--search-cli", default="./build/search_cli")
    p.add_argument("--index", default="final_sorted_index.bin")
    p.add_argument("--lexicon", default="final_sorted_lexicon.txt")
    p.add_argument("--blocks", default="final_sorted_block_info.bin")
    p.add_argument("--doc-info", default="document_info.txt")
    p.add_argument("--collection", default="data/collection.tsv")
    return p.parse_args(argv)


def build_prompt(question: str, passages: list[dict[str, Any]],
                 max_chars: int) -> str:
    """Compose a citation-friendly prompt for the LLM."""
    intro = (
        "You are a careful assistant. Answer the question using ONLY the "
        "passages below. Cite each factual statement with the corresponding "
        "[doc_id]. If the passages are insufficient to answer, reply with: "
        '"I do not have enough information."'
    )
    body_lines = ["Passages:"]
    for p in passages:
        snippet = (p.get("passage") or "").replace("\n", " ").strip()
        if len(snippet) > max_chars:
            snippet = snippet[: max_chars - 1] + "…"
        body_lines.append(f"[{p['doc_id']}] {snippet}")
    body = "\n".join(body_lines)
    return f"{intro}\n\n{body}\n\nQuestion: {question}\nAnswer:"


def call_ollama(prompt: str, model: str) -> str:
    try:
        import ollama  # type: ignore
    except ImportError as e:
        raise RuntimeError("ollama package missing; `pip install ollama`") from e
    resp = ollama.chat(model=model,
                       messages=[{"role": "user", "content": prompt}])
    return resp["message"]["content"]


def call_openai(prompt: str, model: str) -> str:
    try:
        import openai  # type: ignore
    except ImportError as e:
        raise RuntimeError("openai package missing; `pip install openai`") from e
    if not os.environ.get("OPENAI_API_KEY"):
        raise RuntimeError("OPENAI_API_KEY environment variable is not set")
    client = openai.OpenAI()
    r = client.chat.completions.create(
        model=model,
        messages=[{"role": "user", "content": prompt}],
    )
    return r.choices[0].message.content or ""


def render_passages(passages: list[dict[str, Any]], max_chars: int = 120) -> str:
    out: list[str] = []
    for p in passages:
        snippet = (p.get("passage") or "").replace("\n", " ").strip()
        if len(snippet) > max_chars:
            snippet = snippet[: max_chars - 1] + "…"
        out.append(f"  rank={p.get('rank')} doc_id={p['doc_id']} "
                   f"score={p.get('score'):.3f} | {snippet}")
    return "\n".join(out)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)

    paths = {
        "index": args.index,
        "lexicon": args.lexicon,
        "blocks": args.blocks,
        "doc_info": args.doc_info,
        "collection": args.collection,
    }

    with Retriever(args.search_cli, **paths) as r:
        passages = r.search(args.q, k=args.top_k)

    print(f"=== Retrieved {len(passages)} passages for: {args.q!r} ===")
    print(render_passages(passages))

    prompt = build_prompt(args.q, passages, args.max_passage_chars)
    if args.backend == "mock":
        print("\n=== Prompt (mock backend, no LLM call) ===")
        print(textwrap.indent(prompt, "  "))
        return 0

    model = args.model or ("qwen2.5:7b" if args.backend == "ollama" else "gpt-4o-mini")
    try:
        if args.backend == "ollama":
            answer = call_ollama(prompt, model)
        else:
            answer = call_openai(prompt, model)
    except Exception as e:
        print(f"\n[error] LLM call failed: {e}", file=sys.stderr)
        print("\n=== Prompt that would have been sent ===", file=sys.stderr)
        print(textwrap.indent(prompt, "  "), file=sys.stderr)
        return 4

    print(f"\n=== Answer ({args.backend}/{model}) ===")
    print(answer)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
