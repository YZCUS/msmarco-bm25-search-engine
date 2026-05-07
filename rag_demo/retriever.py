"""Thin Python wrapper around the search_cli --server JSONL protocol."""

from __future__ import annotations

import json
import subprocess
import threading
from pathlib import Path
from typing import Any


class Retriever:
    def __init__(self, search_cli: str | Path, **paths: str) -> None:
        self._cli = str(search_cli)
        self._paths = paths
        self._proc: subprocess.Popen[str] | None = None
        self._stderr_thread: threading.Thread | None = None
        self._stop = threading.Event()

    def open(self) -> None:
        if self._proc is not None:
            return
        args = [
            self._cli,
            "--server",
            "--index", self._paths["index"],
            "--lexicon", self._paths["lexicon"],
            "--blocks", self._paths["blocks"],
            "--doc-info", self._paths["doc_info"],
            "--collection", self._paths["collection"],
        ]
        self._proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self) -> None:
        assert self._proc and self._proc.stderr
        for line in self._proc.stderr:
            if not self._stop.is_set():
                print(f"[search_cli] {line}", end="")

    def search(self, query: str, k: int = 10) -> list[dict[str, Any]]:
        if self._proc is None:
            self.open()
        assert self._proc and self._proc.stdin and self._proc.stdout
        self._proc.stdin.write(json.dumps({"q": query, "k": k}) + "\n")
        self._proc.stdin.flush()
        line = self._proc.stdout.readline()
        if not line:
            raise RuntimeError("search_cli closed unexpectedly")
        payload = json.loads(line)
        if "error" in payload:
            raise RuntimeError(payload["error"])
        return payload.get("results", [])

    def close(self) -> None:
        self._stop.set()
        if self._proc is None:
            return
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
        finally:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None

    def __enter__(self) -> "Retriever":
        self.open()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()
