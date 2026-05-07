# MS MARCO BM25 Search Engine — 改造與呈現計劃

本文件對齊 Cursor 對話初期的「總計劃」：**目錄與元件如何分工**、對外要如何呈現。

- **細部實作步驟、附錄與 Caveats**：見專案根目錄的 [IMPLEMENTATION.md](../IMPLEMENTATION.md)（Runbook）。
- **重現指令與指標占位表**：見 [README.md](../README.md)。
- **實跑後填入的數字**：見 [benchmark_results.md](benchmark_results.md) 與 [index_size.md](index_size.md)。

---

## 1. Repository layout（規劃結構）

以下為刻意設計的 **source-layout**；不包含本機才有的 `build/`、`build-*`、大型資料檔、`__pycache__` 等。

```text
msmarco-bm25-search-engine/
├── .github/
│   └── workflows/          # CI（VarByte / Raw32 matrix 等）
├── bench/                  # C++ benchmarks：latency, index size, compression
├── docs/                   # benchmark 結果、本計劃、圖檔占位
├── eval/                   # MS MARCO 準備 script、TREC run writer、trec_eval 包裝
├── include/                # 公開 headers（tokenizer, BM25, codec, allocator, SearchEngine …）
│   └── third_party/       # vendor single-header（如 nlohmann json）
├── rag_demo/               # Python retriever subprocess + mock/Ollama/OpenAI RAG demo
├── scripts/                # shell：雙 codec 建索引、eval 總編、繪圖輔助
├── src/                    # 實作：builder, inverted_list, search_engine, CLI
├── tests/                  # C++ ctest + Python e2e（RAG IPC）
├── CMakeLists.txt
├── IMPLEMENTATION.md       # Phase P0–P10 + 技術附錄
├── README.md               # Visitor-facing reproduce + metrics table + architecture
├── AGENTS.md               # （若存在）Agent / 協作說明
├── data/                   # git 僅占位；collection / index / qrels 本機填入（多半 .gitignore）
└── bench_results/          # latency CSV 等產物（多半 .gitignore）
```

設計用意：

| 路徑 | 職責 |
| ----- | ------ |
| `include/` / `src/` | C++ core：建索引、壓縮過 postings、DAAT BM25、mmap、`search_batch`。 |
| `bench/` | 可發表的 **latency / size / compression / memory** 量測二進位。 |
| `eval/` | MS MARCO + TREC 格式與 **MRR/nDCG/Recall** 管線。 |
| `rag_demo/` | 「RAG-Ready」對外演示：檢索當為 LLM grounding。 |
| `scripts/` | 一鍵重現多組對照實驗（兩種 codec、整包 eval）。 |
| `docs/` | 對人類讀取的報表與**本檔**：補 README 缺的「樹狀全貌」。 |

---

## 2. 呈現計劃（對履歷 / GitHub / 面試）

1. **首屏對齊**：README 第一段 bullet 對應實際 repo（8.8M、BM25、並行、`bench/`、`eval/` 數字）。
2. **證據鏈**：`bench_results/*.csv`、`docs/index_size.md`、`docs/benchmark_results.md`（或輸出的圖）與 **環境段落**（CPU、RAM、編譯器）同時出現，避免數字無法質證。
3. **對照實驗**：VarByte vs Raw32；build  allocator baseline vs slab arena——各對應腳本與表格。
4. **排名**：`scripts/eval_all.sh` + `pytrec_eval` dev / DL19 / DL20。
5. **demo 路徑**：`python -m rag_demo.rag_demo`（可先 mock），說清楚 C++ `--server` IPC 角色。

詳細口吻與 checklist 可再對齊 `IMPLEMENTATION.md` 末尾履歷對照區塊。

---

## 3. 與其他文件的對照

| 需求 | 讀這裡 |
|------|--------|
| 目錄樹長怎樣、為何這樣切 | **本檔 §1** |
| P6/P7、CMake、格式細節 | `IMPLEMENTATION.md` |
| 怎麼跑、表格占位 | `README.md` |

若之後調整資料夾，請同步更新 **本檔 §1** 中的樹狀結構。
