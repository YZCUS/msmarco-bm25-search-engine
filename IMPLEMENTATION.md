# MS MARCO BM25 Search Engine — 實作 Runbook

> 本文件對應 `.cursor/plans/rag-ready_bm25_indexer_*.plan.md` 的詳細落地版本。
> **Repository 目錄樹與對外呈現摘要**（總覽用）另見
> [docs/renovation_presentation_plan.md](docs/renovation_presentation_plan.md)。
> 所有 code comment 一律以英文撰寫，文件本身以繁體中文敘述。

---

## 目錄

- [P0 環境與資料準備](#p0-環境與資料準備)
- [P1 重構與既有 bug 修復](#p1-重構與既有-bug-修復)
- [P2 VarByte ON/OFF 對照](#p2-varbyte-onoff-對照)
- [P3 Custom Slab Allocator + Move 路徑](#p3-custom-slab-allocator--move-路徑)
- [P4 Parallel Query Execution](#p4-parallel-query-execution)
- [P5 search_cli：互動 + JSONL Server 模式](#p5-search_cli互動--jsonl-server-模式)
- [P6 Benchmarks（latency / index size / memory）](#p6-benchmarkslatency--index-size--memory)
- [P7 MS MARCO + TREC DL Ranking Metrics](#p7-ms-marco--trec-dl-ranking-metrics)
- [P8 Python RAG Demo](#p8-python-rag-demo)
- [P9 測試與 CI](#p9-測試與-ci)
- [P10 README、圖表與發佈](#p10-readme圖表與發佈)
- [跨階段共用注意事項](#跨階段共用注意事項)

---

## P0 環境與資料準備

### 目標
讓後續所有 phase 都能在同一台機器上 reproducible：固定 toolchain、固定資料、固定相對路徑。

### 步驟
1. **編譯器**：macOS 用 `clang++` 17+（`brew install llvm`），Linux 用 `g++-13` 以上。確認支援 C++23（`std::jthread`、`std::pmr`）。
2. **依賴**：
   ```bash
   brew install cmake libarchive zlib pkg-config
   python3 -m venv .venv && source .venv/bin/activate
   pip install -r rag_demo/requirements.txt   # 步驟在 P8 建立
   ```
3. **資料目錄**：建立 `data/`（已加入 `.gitignore`），所有原始檔放這裡：
   ```
   data/
     collection.tsv                 # 8.8M passages, pid<TAB>passage
     queries.dev.small.tsv
     qrels.dev.small.tsv
     msmarco-test2019-queries.tsv
     2019qrels-pass.txt
     msmarco-test2020-queries.tsv
     2020qrels-pass.txt
   ```
4. **目錄結構建立**：
   ```bash
   mkdir -p include src bench eval rag_demo tests docs scripts
   ```

### 注意事項
- MS MARCO Passage v1 的 `collection.tsv` 沒有 header，行格式是 `pid \t passage`，`pid` 從 0 連續到 8,841,822。
- 不要直接使用 `collection.tar.gz`（因為原始碼會用到 stdin/seek 取原文）；解壓後存純 tsv 即可。
- 把 `data/` 與 `build/`、`*.bin`、`temp_index_*.bin`、`final_sorted_*` 全列入 `.gitignore`。
- macOS 上 `clang++` 預設不會啟用 `-march=native` 等同於 Linux 的效果，請改用 `-mcpu=apple-m1` 或 `-mtune=native`，視機型。

### 驗收
- `ls data/collection.tsv` 顯示約 2.9 GB。
- `wc -l data/collection.tsv` 約 8841823。

---

## P1 重構與既有 bug 修復

### 目標
- 把現有 `src/build_index.cpp` 與 `src/search_engine.cpp` 內部重複的 type/helper 抽到 `include/`。
- 修正 `search_engine.cpp` 既有的兩個阻塞性 bug。
- 把所有 `std::cout` debug 噪音收進 `#ifdef IDX_DEBUG`。

### 步驟

#### 1.1 建立 header 群

依下列分工拆檔：

- `include/varbyte.hpp`：`encode(uint32_t, OutputBuffer&)`、`decode(const uint8_t*, size_t&)`。把目前 `varbyte_encode_test.cpp`、`build_index.cpp`、`search_engine.cpp` 三份不一致的實作合一。
- `include/posting.hpp`：`Posting`、`LexiconInfo`、`IndexEntry`、`SearchResult`。
- `include/tokenizer.hpp`：把 `processSentencePart` 抽出，提供 `tokenize_inplace(string_view, std::vector<std::string_view>&)`，並支援 ASCII fast path（後續可換 ICU）。
- `include/bm25.hpp`：`idf(N, df)`、`tf(freq, dl, avgdl, k1, b)`，`constexpr` 化常數 k1=1.2、b=0.75。
- `include/inverted_list.hpp`：把 `InvertedList` 從 `search_engine.cpp` 拉出，加上 docstring。
- `include/search_engine.hpp`：宣告 `SearchEngine` 類別。

#### 1.2 修 `search_engine.cpp` 的兩個 bug

| Bug | 現況 | 修法 |
| --- | --- | --- |
| `term_id_to_word` 是 `SearchEngine` 的成員，但只在 `loadLexicon()` 的「區域同名變數」被填值，導致主成員永遠空 | 把 `loadLexicon` 中的 `std::unordered_map<int, std::string> term_id_to_word;` 區域變數刪掉，直接寫入成員 | 影響範圍：BM25 IDF 算錯（一定 0） |
| `conjunctiveSearch` 在同輪迴圈內呼叫兩次 `lists[i].next(...)`，第二次會吃掉一個 posting | 改成標準 DAAT max-pivot：用 `nextGEQ(target_did)` API 取代 `next()` | 必要時為 `InvertedList` 增加 `bool nextGEQ(int target, int& did, int& freq)` |

`nextGEQ` 偽碼：
```cpp
// Advance until current posting's doc_id >= target. Returns false on exhaustion.
bool InvertedList::nextGEQ(int target, int& doc_id, int& freq) {
    while (next(doc_id, freq)) {
        if (doc_id >= target) return true;
    }
    return false;
}
```

DAAT 主迴圈骨架：
```cpp
int pivot = 0;
while (true) {
    bool all_aligned = true;
    int max_did = pivot;
    for (size_t i = 0; i < lists.size(); ++i) {
        if (!lists[i].nextGEQ(pivot, dids[i], freqs[i])) return results;
        if (dids[i] != pivot) all_aligned = false;
        max_did = std::max(max_did, dids[i]);
    }
    if (all_aligned) {
        results.push_back({pivot, score(pivot, freqs)});
        ++pivot;
    } else {
        pivot = max_did;
    }
}
```

#### 1.3 統一 logging

加入 `include/log.hpp`：

```cpp
#ifdef IDX_DEBUG
  #define IDX_LOG(x) do { std::cerr << x << '\n'; } while (0)
#else
  #define IDX_LOG(x) do {} while (0)
#endif
```

把 `search_engine.cpp` 與 `build_index.cpp` 內所有「進度型」的 `std::cout` 全替換為 `IDX_LOG(...)`，只保留「使用者可見」訊息（最終 result、build summary）。

### 注意事項
- `varbyteDecode` 在不同檔有「正向 shift」與「反向 shift」兩種寫法，**必須統一**為「LSB 在第一個 byte，shift += 7」（這也是業界慣例）；目前 `build_index.cpp` 的 `varbyteDecode(const std::vector<uint8_t>&)` 是反向的，會與 `search_engine.cpp` 的 `varbyteDecode(const uint8_t*, size_t&)` 衝突，請以後者為準。
- `processLine` 假設 `doc_id < last_doc_id` 是 invalid，但 MS MARCO 的 pid 是 0-based 嚴格遞增，所以實作層面是對的；改 header 時不要破壞此前提。
- 抽到 `string_view` 的 tokenizer 要小心：`std::istringstream` 內 `iss >> sentence_part` 會 copy，建議直接用 `std::string_view` 的手動掃描以省一次配置。

### 驗收
- `cmake --build build && ./build/build_index data/collection.tsv` 成功跑完，產出 4 個檔（index、lexicon、block_info、document_info）。
- `./build/search` 能正確返回非空、score 不為 0 的結果。
- `IDX_DEBUG=OFF` 下，stdout 只有最終結果（不再有「Loading block index...」洪水）。

---

## P2 VarByte ON/OFF 對照

### 目標
產出明確的「VarByte 比 Raw32 小 X%」實證數字，撐起履歷 *Reduced index memory by 60% via VarByte compression* 的論述。

### 步驟

#### 2.1 在 `include/varbyte.hpp` 加 codec 抽象

```cpp
enum class Codec { VarByte, Raw32 };

struct CodecOps {
    void (*encode)(uint32_t, std::vector<uint8_t>&);
    uint32_t (*decode)(const uint8_t*, size_t&);
};

constexpr CodecOps kVarByteOps = { &encode_varbyte, &decode_varbyte };
constexpr CodecOps kRaw32Ops   = { &encode_raw32,   &decode_raw32   };
```

#### 2.2 CMake option

於 `CMakeLists.txt`：
```cmake
set(CODEC "VarByte" CACHE STRING "Posting codec: VarByte | Raw32")
target_compile_definitions(build_index PRIVATE IDX_CODEC_${CODEC})
target_compile_definitions(search       PRIVATE IDX_CODEC_${CODEC})
```

#### 2.3 雙路徑建立索引

提供腳本 `scripts/build_two_indexes.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail
cmake -B build/varbyte -DCODEC=VarByte -DCMAKE_BUILD_TYPE=Release
cmake --build build/varbyte -j
./build/varbyte/build_index data/collection.tsv
mv final_sorted_index.bin   index_varbyte.bin
mv final_sorted_block_info.bin block_info_varbyte.bin

cmake -B build/raw32 -DCODEC=Raw32 -DCMAKE_BUILD_TYPE=Release
cmake --build build/raw32 -j
./build/raw32/build_index data/collection.tsv
mv final_sorted_index.bin   index_raw32.bin
mv final_sorted_block_info.bin block_info_raw32.bin
```

#### 2.4 `bench/bench_compression.cpp`

讀兩個 index 的 stat，寫 markdown 表至 `docs/bench_compression.md`：

```text
| Codec   | Index size | Block info | Lexicon | Total | Ratio |
| ------- | ---------- | ---------- | ------- | ----- | ----- |
| Raw32   | __ GB      | __ MB      | __ MB   | __ GB | 1.00x |
| VarByte | __ GB      | __ MB      | __ MB   | __ GB | __x   |
```

### 注意事項
- **公平比較條件**：兩種 codec 都使用「delta-encoded doc_id + raw freq」這同一份邏輯資料，差別只在 byte serialization。不要把 Raw32 baseline 寫成「不做 delta」，否則會誇大壓縮比。
- Raw32 編碼成 4 bytes little-endian，decode 用 `std::memcpy`。
- 兩種 codec 的 block 邏輯結構保持一致（128 postings/block），block_info 也都記錄 `(last_doc_id, doc_id_size, freq_size)`。
- 預期：MS MARCO Passage 上 VarByte 通常落在 Raw32 的 35–45%（也就是壓縮 55–65%），履歷的 60% 在合理區間。
- 如果結果不到 60%，**不要硬寫**，改寫實際數字 + 用 `-Olog` (delta + frequency 都壓 VarByte 後) 的事實描述。

### 驗收
- `bench_compression` 產出兩列數字，比例與大小寫進 `docs/benchmark_results.md`。
- 兩個 index 都能被 `search` 對應 codec 正確讀取（同一個 query 結果完全一致）。

---

## P3 Custom Slab Allocator + Move 路徑

### 目標
- 在 build 階段，把高頻的小型 `vector<pair<int,int>>` 配置改走 arena，降低 peak RSS。
- 在 query 階段，把 per-query 的暫存（score map、posting buffer）也走 arena，降低 query 延遲尾巴。
- 量化「baseline vs arena+move」的 peak RSS，兌現履歷 *lowered peak memory by 40%*。

### 步驟

#### 3.1 `include/allocator.hpp`：SlabArena

最小可行版本：

```cpp
class SlabArena {
public:
    explicit SlabArena(std::size_t slab_bytes = 8 * 1024 * 1024)
        : slab_bytes_(slab_bytes) { add_slab(); }

    void* allocate(std::size_t bytes, std::size_t align) {
        auto p = std::align(align, bytes, cursor_, remaining_);
        if (!p || remaining_ < bytes) { add_slab(); return allocate(bytes, align); }
        cursor_ = static_cast<char*>(cursor_) + bytes;
        remaining_ -= bytes;
        return p;
    }

    void reset() {
        // Keep first slab, drop the rest, reset cursor.
    }

private:
    void add_slab();
    std::vector<std::unique_ptr<char[]>> slabs_;
    void* cursor_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t slab_bytes_;
};

// PMR adapter so std::pmr containers can plug in.
class SlabResource final : public std::pmr::memory_resource {
    SlabArena* arena_;
    void* do_allocate(std::size_t b, std::size_t a) override { return arena_->allocate(b, a); }
    void  do_deallocate(void*, std::size_t, std::size_t) override {}
    bool  do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }
public:
    explicit SlabResource(SlabArena* a) : arena_(a) {}
};
```

#### 3.2 改 build_index 用 PMR

把 `std::unordered_map<int, std::vector<std::pair<int,int>>> index;` 改為：

```cpp
SlabArena arena;
SlabResource res(&arena);
std::pmr::unordered_map<int, std::pmr::vector<std::pair<int,int>>> index{&res};
```

寫入 temp index 檔後 `arena.reset()`（保留第一個 slab），下一輪繼續用，避免高頻 free。

#### 3.3 改 external sort 的 buffer 為 arena

`merged_doc_ids` / `merged_counts` 改為 `std::pmr::vector<uint8_t>` 或者直接 `arena.allocate(POSTING_PER_BLOCK * 5, 1)` 拿一塊大 buffer。

#### 3.4 SearchEngine 加 per-thread arena

```cpp
struct QueryContext {
    SlabArena arena;
    std::pmr::unordered_map<int, double> score_acc{ &res };
};
```

每個 thread 持有一個 `QueryContext`，每筆 query 開頭 `arena.reset()`。

#### 3.5 Move semantics 補完

巡查 `build_index.cpp` 與 `search_engine.cpp`，把所有「複製大型 vector」的點改成 move：

- `IndexEntry` 已是 move-friendly；確認 `pq.push(std::move(entry))` 沒有 silently 退化成 copy（因 `priority_queue::top()` 回傳 `const&`，必須先 `std::move(const_cast<IndexEntry&>(top))` 或重構成 `std::vector` + manual heap）。
- 把 lexicon 拷貝改成 `lexicon[term_id_to_word.at(top.term_id)]` 用 reference，避免每行都重新 hash + copy string。

### 注意事項
- `priority_queue` 的 `top()` 是 const，要 move 出來只能 `const_cast`，**或** 改用手刻 heap（推薦後者，因為這也讓 IndexEntry 可以變成 trivially relocatable）。
- `SlabArena::reset()` 必須保留至少一個 slab，否則每輪都重 allocate 反而更慢。
- PMR 容器的 `clear()` 不會還記憶體給 arena；要還記憶體只能 reset 整個 arena。
- 量測 peak RSS 時，**baseline** 跑「default allocator + 原本 vector copy」，**experimental** 跑「PMR + arena + move」，兩者輸入完全相同（同一份 collection、同一份 codec）。
- macOS：`/usr/bin/time -l` 報 `maximum resident set size` 單位是 bytes；Linux 的 `time -v` 是 KB。寫入 README 時統一換成 GB。

### 驗收
- `bench/run_memory.sh` 跑兩次，輸出兩個 RSS 數字，差值 ≥ 30%（若達不到 40% 須誠實調整履歷數字或進一步優化）。
- `valgrind --tool=massif`（Linux）或 `leaks`（macOS）顯示 build 階段的 small allocation 數量大幅下降。

---

## P4 Parallel Query Execution

### 目標
- 提供 `SearchEngine::search_batch(queries, k, conjunctive)`，在 thread pool 上並行處理多筆 query。
- benchmark 報告 concurrency 1/2/4/8 下的 QPS 與 P50/P95/P99 延遲。

### 步驟

#### 4.1 `include/thread_pool.hpp`

最小實作（C++20 起的 `std::jthread` + `std::stop_token`）：

```cpp
class ThreadPool {
public:
    explicit ThreadPool(unsigned n);
    ~ThreadPool();
    template <class F> std::future<std::invoke_result_t<F>> submit(F&& f);
private:
    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};
```

#### 4.2 thread-safe 的 index 讀取

兩條路線擇一：

- **A. mmap（推薦）**：用 `posix_mmap` 把 `final_sorted_index.bin` 整檔映射到唯讀記憶體；多個 thread 共享同一段虛擬位址，`InvertedList` 把 `ifstream` 換成 `const uint8_t* base + offset`。
- **B. per-thread ifstream**：每個 worker 持有自己的 `std::ifstream`（同一檔開多次），lexicon 與 block_info 為 immutable shared。

> 推薦走 A：實作後 latency 通常下降 20–40%，且天然 thread-safe。

#### 4.3 SearchEngine API

```cpp
struct SearchOptions { int top_k = 10; bool conjunctive = false; };

class SearchEngine {
public:
    std::vector<SearchResult>          search(std::string_view q, SearchOptions);
    std::vector<std::vector<SearchResult>>
        search_batch(const std::vector<std::string>& qs, SearchOptions);
};
```

`search_batch` 將每筆 query 包成 task 投進 pool，等待 `future::get()`。

### 注意事項
- `std::ifstream` **不是** thread-safe；同一個 fd 在多 thread 間 seek + read 會錯亂。如果走路線 B，請務必 per-thread 開檔。
- mmap 後的 buffer **唯讀**：寫入會 segfault。記得 `mmap(... PROT_READ ...)`。
- macOS 上 mmap 大檔（>2 GB）需確保程式為 64-bit 編譯（預設就是）。
- thread pool 用 `std::function` 會多一次 type erasure；若 latency 敏感可改用 lock-free SPMC queue，但對 query workload 通常不必要。
- 同一筆 query 內**不要**再切 thread（邊際效益低、cache miss 暴增）；只在 query 之間並行。

### 驗收
- `bench_latency` 在 `--threads=8` 下 QPS 比 `--threads=1` 至少 4 倍以上（受 I/O 與 lexicon hash 影響，理想 6–7 倍）。
- 跑 100k 筆 dev queries 完全沒 segfault、無 race（用 `clang -fsanitize=thread` 跑 1000 筆 sanity check）。

---

## P5 search_cli：互動 + JSONL Server 模式

### 目標
為 RAG demo 提供穩定、可被 Python `subprocess` 串接的協議。

### 步驟

#### 5.1 命令列介面

```bash
search_cli --index final_sorted_index.bin \
           --lexicon final_sorted_lexicon.txt \
           --blocks final_sorted_block_info.bin \
           --doc-info document_info.txt \
           --collection data/collection.tsv \
           [--server] [--top-k 10] [--conjunctive]
```

#### 5.2 互動模式

- 使用者輸入一行 query，回印 top-k；`q` 離開。
- 預設返回的每行包含 `rank score doc_id passage_text_first_120_chars`。

#### 5.3 Server (JSONL) 模式

協議：stdin 一行一筆 JSON，stdout 一行一筆 JSON。

Request：
```json
{"q": "what is bm25", "k": 10, "mode": "disjunctive"}
```

Response：
```json
{"q": "what is bm25", "results":[{"rank":1,"doc_id":1234,"score":18.42,"passage":"BM25 is..."}]}
```

實作要點：
- `std::cout.sync_with_stdio(false)`，每筆 response 後 `std::cout << '\n' << std::flush;`
- 解析 JSON 用 [nlohmann/json](https://github.com/nlohmann/json) single-header（放 `include/third_party/json.hpp`）。
- 從 `collection.tsv` 取原文：用 `lines_pos[doc_id]` seek 後 `getline`，截前 N 字（避免 stdout 過大）。

### 注意事項
- **非阻塞錯誤回應**：解析失敗時也要回 `{"error": "..."}` 並換行，否則 Python 端會死等。
- 不要在 server 模式輸出任何 debug；所有 log 走 `std::cerr`。
- Windows 終端會把 `\n` 變 `\r\n`，跨平台時要 `std::cout << "\n"` 而非 `std::endl`，並在 Python 讀取端用 `universal_newlines=True`。

### 驗收
- `echo '{"q":"machine learning","k":3}' | ./build/search_cli --server` 回傳合法 JSON 一行。

---

## P6 Benchmarks（latency / index size / memory）

### 目標
產出三組可貼進 README 的表格與圖：latency 分佈、索引 size、peak RSS。

### 步驟

#### 6.1 `bench/bench_latency.cpp`

- 讀 `data/queries.dev.small.tsv`（約 6980 筆）。
- 依 `--threads=N` 分批投進 `search_batch`。
- 對每筆 query 紀錄 `std::chrono::steady_clock` 起訖（ns 級），跑完後算 P50/P95/P99 與 QPS。
- 輸出 csv：`bench_results/latency_t{N}.csv`。

#### 6.2 `bench/bench_index_size.cpp`

純 stat 工具，產出：
- `collection.tsv` 大小
- `final_sorted_index.bin`、`final_sorted_lexicon.txt`、`final_sorted_block_info.bin`、`document_info.txt` 大小
- 比例：`index_total / collection_size`

#### 6.3 `bench/run_memory.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
echo "== baseline (default allocator, copy path) =="
/usr/bin/time -l ./build/baseline/build_index data/collection.tsv 2>&1 | tee bench_results/mem_baseline.log
echo "== arena + move path =="
/usr/bin/time -l ./build/arena/build_index data/collection.tsv 2>&1 | tee bench_results/mem_arena.log
python3 scripts/extract_rss.py bench_results/mem_baseline.log bench_results/mem_arena.log \
    > docs/memory_table.md
```

`scripts/extract_rss.py`：解析 `maximum resident set size` 行，輸出 markdown 表。

#### 6.4 圖表

`scripts/plot_latency.py`（matplotlib）：讀 csv 畫 latency CDF 與 QPS bar，輸出 png 至 `docs/`。

### 注意事項
- 所有 benchmark 都要 **warm cache**：先跑 100 筆暖機 query 再開始量測，避免第一筆 page fault 噪音。
- `--threads` 超過實體 core 數時 QPS 會下降，這是正常的；報告時最多到 `nproc`，不要過度膨脹。
- macOS 上 `time -l` 的 RSS 包含被 mmap 但未被 dirty 的頁；如果走 mmap 路徑，要把這部分扣除（用 `task_info` 取 `resident_size_max - mmap_resident`），或者改報「dirty + anonymous」。
- benchmark 應該支援 `--repeat=K`（預設 K=3）取最佳值或中位數，避免單次抖動。

### 驗收
- 三份 csv + 兩張 png + 一份 markdown 表全部到位，且 README 能直接 link。

---

## P7 MS MARCO + TREC DL Ranking Metrics

### 目標
產出真實 ranking metrics（MRR@10、nDCG@10、Recall@1000），對應履歷 *Programmatically evaluated retrieval via ... ranking metrics*。

### 步驟

#### 7.1 `eval/prepare_msmarco.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
mkdir -p data && cd data

# Passage collection
[ -f collection.tsv ] || {
  curl -L -o collection.tar.gz https://msmarco.z22.web.core.windows.net/msmarcoranking/collection.tar.gz
  tar -xzf collection.tar.gz
}

# Dev queries + qrels
curl -L -O https://msmarco.z22.web.core.windows.net/msmarcoranking/queries.dev.small.tsv
curl -L -O https://msmarco.z22.web.core.windows.net/msmarcoranking/qrels.dev.small.tsv

# TREC DL 2019 / 2020
curl -L -O https://trec.nist.gov/data/deep/2019qrels-pass.txt
curl -L -O https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco-test2019-queries.tsv.gz
gunzip -f msmarco-test2019-queries.tsv.gz

curl -L -O https://trec.nist.gov/data/deep/2020qrels-pass.txt
curl -L -O https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco-test2020-queries.tsv.gz
gunzip -f msmarco-test2020-queries.tsv.gz
```

#### 7.2 `eval/trec_run_writer.cpp`

讀 `queries.tsv`（`qid \t query`），對每筆 query 跑 disjunctive top-1000，寫成 TREC run 格式：
```
qid Q0 docid rank score IDX_BM25
```
qid、docid 都用「passage id」當字串輸出（直接 `std::to_string(doc_id)`）。

#### 7.3 `eval/run_eval.py`

```python
import pytrec_eval, json, sys
qrels = parse_qrels(sys.argv[1])
run   = parse_run(sys.argv[2])
ev = pytrec_eval.RelevanceEvaluator(qrels,
        {'recip_rank_cut.10', 'ndcg_cut_10', 'recall_1000'})
results = ev.evaluate(run)
print(json.dumps(aggregate(results), indent=2))
```

聚合方式：對所有 query 取算術平均。

#### 7.4 端到端腳本 `scripts/eval_all.sh`

```bash
./build/release/trec_run_writer --queries data/queries.dev.small.tsv  --out runs/dev.run
python3 eval/run_eval.py data/qrels.dev.small.tsv  runs/dev.run  > docs/metrics_dev.json

./build/release/trec_run_writer --queries data/msmarco-test2019-queries.tsv --out runs/dl19.run
python3 eval/run_eval.py data/2019qrels-pass.txt   runs/dl19.run > docs/metrics_dl19.json

./build/release/trec_run_writer --queries data/msmarco-test2020-queries.tsv --out runs/dl20.run
python3 eval/run_eval.py data/2020qrels-pass.txt   runs/dl20.run > docs/metrics_dl20.json
```

### 注意事項
- **MRR@10** 標準 metric key 是 `recip_rank_cut.10`（pytrec_eval 命名）；不同版本 pytrec_eval 對 cut 的拼法略有差異，請用 `pip show pytrec_eval` 對照官方文件。
- **DL19 / DL20 qrels** 的 relevance grade 是 0/1/2/3，閥值通常以 `>=2` 視為 relevant；計算 `recall_1000` 前要先 binarize。pytrec_eval 預設行為已正確處理 graded qrels。
- 對 8.8M docs 跑 6980 筆 dev queries，BM25 disjunctive top-1000 大約需 10–30 分鐘（單機、單 thread）；用 P4 的 thread pool 可壓到 3–5 分鐘。
- BM25 baseline 在 MS MARCO Passage v1 dev 上 MRR@10 約 0.184–0.187（業界公認），DL19 nDCG@10 約 0.50、DL20 約 0.49。如果你的數字差很多，先檢查：
  1. tokenizer 是否處理 lowercasing（目前已做）。
  2. IDF 公式 N 是否用「`document_info.txt` 的行數」（即 8841823），而非 lexicon 大小。
  3. avgdl 是否是「不去 stopword 的 raw token 數」。
  4. `posting_number` 是否真的等於 document frequency（df），不是 collection frequency（cf）。

### 驗收
- 三份 metrics JSON 落地，數字落在 BM25 baseline 合理區間（MRR@10 ≈ 0.18，nDCG@10 ≈ 0.5）。
- README 表格直接 reference 這些 JSON。

---

## P8 Python RAG Demo

### 目標
demo 一次 query → top-k 檢索 → LLM 引用回答的完整 RAG pipeline，撐起 *RAG-Ready* 的 marketing。

### 步驟

#### 8.1 `rag_demo/requirements.txt`

```
pytrec_eval>=0.5
rich>=13.0
ollama>=0.3.0    ; python_version >= "3.10"
openai>=1.40.0
matplotlib>=3.8
pandas>=2.2
```

#### 8.2 `rag_demo/retriever.py`

```python
import json, subprocess

class Retriever:
    def __init__(self, search_cli: str, **paths):
        self.proc = subprocess.Popen(
            [search_cli, "--server",
             "--index", paths["index"],
             "--lexicon", paths["lexicon"],
             "--blocks", paths["blocks"],
             "--doc-info", paths["doc_info"],
             "--collection", paths["collection"]],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1)

    def search(self, q: str, k: int = 10) -> list[dict]:
        self.proc.stdin.write(json.dumps({"q": q, "k": k}) + "\n")
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())["results"]

    def close(self): self.proc.stdin.close(); self.proc.wait()
```

#### 8.3 `rag_demo/rag_demo.py`

- 預設 backend：Ollama 本機（`llama3:8b` 或 `qwen2.5:7b`）。
- fallback：OpenAI（`OPENAI_API_KEY` 存在時可選 `--backend openai`）。
- 把 top-5 passage 拼進 prompt，要求 LLM 用 `[doc_id]` 引用：

```text
You are a helpful assistant. Answer the question using ONLY the passages
below. Cite each fact with [doc_id]. If passages are insufficient, say so.

Passages:
[1234] BM25 is a ranking function ...
[5678] In information retrieval ...

Question: what is bm25?
Answer:
```

#### 8.4 README demo 段

附上一張 terminal screenshot：左上輸入 query，下方顯示 top-5 passage + LLM 引用回答。

### 注意事項
- Ollama 沒裝會直接 connection refused；rag_demo 要 graceful fallback 並印「請先 `ollama pull qwen2.5:7b` 或設定 OPENAI_API_KEY」。
- subprocess pipe 一定要 `bufsize=1` 並把 child stdout flush 在每行結尾，否則會死鎖（這是 Python subprocess 最常見坑）。
- 不要把整段 collection passage 餵給 LLM，超過 context 會被截掉；統一截 512 tokens / passage。
- LLM 回答**不要** mock；如果跑 CI 不接 LLM，就只跑「retriever returns top-5」這一段。

### 驗收
- `python -m rag_demo.rag_demo --q "what is bm25"` 印出帶 `[doc_id]` 引用的答案，且 doc_id 確實能在 collection.tsv 找到對應原文。

---

## P9 測試與 CI

### 目標
保證重構後既有功能不退化，且在 GitHub 上每次 push 都自動驗證。

### 步驟

#### 9.1 升級 `tests/test_varbyte.cpp`

- 涵蓋邊界值 0、127、128、2^14-1、2^21-1、2^28-1、UINT32_MAX。
- 測 round-trip：encode → decode → 比對。
- 測序列 round-trip：寫多個 number 到 buffer，依序 decode。

#### 9.2 `tests/test_inverted_list.cpp`

- 用合成資料（10 個 doc、3 個 term）建出小 index，驗證 `next()` / `nextGEQ()` 行為。

#### 9.3 `tests/test_bm25.cpp`

- 對固定 (df, N, freq, dl, avgdl) 比對手算結果。

#### 9.4 `tests/test_thread_pool.cpp`

- 投 10000 個 lambda，全部回傳 i*2，驗證沒掉、沒重。

#### 9.5 CMake ctest

```cmake
include(CTest)
add_executable(test_varbyte tests/test_varbyte.cpp)
add_test(NAME varbyte COMMAND test_varbyte)
# ... 重複其他三個
```

#### 9.6 `.github/workflows/ci.yml`

```yaml
name: ci
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y libarchive-dev zlib1g-dev cmake g++-13
      - run: cmake -B build -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Release
      - run: cmake --build build -j
      - run: ctest --test-dir build --output-on-failure
```

### 注意事項
- CI 跑不了完整 8.8M build；只跑 unit test。可選擇性新增「smoke test」：用 100 行合成 collection 跑一次 `build_index` + `search`。
- macOS 與 Linux 的 ifstream 行為對 `\r\n` 不同，測試資料要明確用 `\n`。

### 驗收
- 本機 `ctest` 全綠。
- GH Actions 顯示綠勾。

---

## P10 README、圖表與發佈

### 目標
讓履歷上的連結點進來 30 秒內看到亮點。

### 步驟

#### 10.1 README.md 結構

```markdown
# MS MARCO BM25 Search Engine

> Built an inverted index over 8.8M MS MARCO passages with BM25 ranking and
> parallel query execution. VarByte compression cut posting size by __%,
> a slab arena cut peak RSS by __%.

## At a Glance
| Metric                          | Value           |
| ------------------------------- | --------------- |
| Documents indexed               | 8,841,823       |
| Index size (VarByte)            | __ GB           |
| Compression vs. Raw32           | __% reduction   |
| Build peak RSS (default → arena)| __ GB → __ GB   |
| Query latency P50 / P95 / P99   | __ / __ / __ ms |
| QPS @ 8 threads                 | __              |
| MRR@10 (MS MARCO dev)           | __              |
| nDCG@10 (TREC DL19 / DL20)      | __ / __         |
| Recall@1000 (DL19)              | __              |

## Architecture
<!-- mermaid 架構圖 -->

## Reproduce in 5 commands
1. `bash eval/prepare_msmarco.sh`
2. `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
3. `./build/build_index data/collection.tsv`
4. `bash scripts/eval_all.sh`
5. `python -m rag_demo.rag_demo --q "what is bm25"`

## Design Highlights
- VarByte vs Raw32 ablation
- SlabArena allocator
- DAAT BM25 with parallel batch
- TREC eval pipeline

## Known Limitations
```

#### 10.2 `docs/benchmark_results.md`

收完整數字 + 4 張 png：
- `latency_cdf.png`
- `qps_vs_threads.png`
- `compression_bar.png`
- `rss_compare.png`

#### 10.3 GitHub release v1.0

- tag：`v1.0.0`
- release notes：直接複製 README At a Glance 表 + 兩張關鍵圖。

#### 10.4 履歷連結

把 GitHub repo URL 放在履歷 *MS MARCO BM25 Search Engine* 條目下方；面試官點進來首屏即看到三條 bullet 對應的證據。

### 注意事項
- README 不要寫超過 250 行；想寫長放 `docs/`。
- 所有數字都要附測試環境（CPU 型號、RAM、OS、編譯器版本），否則 latency 數字會被質疑。
- 如果某個指標達不到履歷宣稱（例如壓縮比 50%、RSS 降 30%），**改履歷實際數字**比寫不實在的數字安全得多。

### 驗收
- README 在 GitHub 渲染正常（mermaid、png、表格）。
- 履歷可以直接連結；recruiter 30 秒可看完亮點。

---

## 深度技術附錄

> 這些附錄是「為什麼這樣寫」與「踩雷紀錄」的集合，實作前先把對應附錄看一遍，可以省下大量 debug 時間。

### Appendix A — Index 檔案格式規格

#### A.1 四個落地檔
```
final_sorted_index.bin       # postings (binary)
final_sorted_block_info.bin  # block boundaries (binary)
final_sorted_lexicon.txt     # term -> {term_id, df, start_pos, bytes_size}
document_info.txt            # doc_id -> {doc_length, line_position}
```

#### A.2 `final_sorted_index.bin` byte layout

對每個 term，連續輸出 `ceil(df / 128)` 個 block：

```
[ block_0 ][ block_1 ] ... [ block_{n-1} ]
```

每個 block 內部是「先 doc_id 段、後 freq 段」分離存放：

```
+-------------------------------+
| doc_id_section (varbyte)      |  <- block_info.doc_id_size bytes
+-------------------------------+
| freq_section   (varbyte)      |  <- block_info.freq_size  bytes
+-------------------------------+
```

`doc_id` 段內所存的是 **delta**（與前一筆的差值，跨 block 不重置；block 內第一筆對 term 的全域 last_doc_id 取 delta，而非 block 內 0）。

> 為什麼分離 doc_id / freq？因為 query path 在 score < threshold 時可以**只 decode doc_id 段做 skip**，freq 段直到要算分時才 decode（lazy decode），降低 cache footprint。MaxScore / WAND 全靠這個。

#### A.3 `final_sorted_block_info.bin` layout

每個 entry 為 `(int32 last_doc_id, int64 doc_id_size, int64 freq_size)`，連續記錄所有 block：

```
struct BlockMeta {
    int32_t last_doc_id;   // absolute, NOT delta
    int64_t doc_id_size;   // bytes
    int64_t freq_size;     // bytes
};
```

block 在 `final_sorted_index.bin` 內的起始位置 = 之前所有 block 的 `(doc_id_size + freq_size)` 累加；因此 block_info 不存 offset，省 8 bytes/block。

> 注意：目前 `build_index.cpp` 的 block_info 寫了 `std::pair<int, std::pair<int64_t,int64_t>>` 的 raw memory dump，會被 padding 影響跨平台相容性。**建議改成 `__attribute__((packed))` struct 或顯式 little-endian 序列化**。

#### A.4 `final_sorted_lexicon.txt`

ASCII，每行：
```
<term> <term_id> <df> <start_position> <bytes_size>
```

例如：
```
machine 12345 4321 123456789 8762
```

> 之後若加入 MaxScore，需在每行追加 `<f_max>`（block-local 也可以另存 binary）。

#### A.5 `document_info.txt`

每行對應 doc_id i：
```
<doc_length> <line_position>
```

`line_position` 是 doc 在 `collection.tsv` 的起始 byte offset（用於 server 模式取原文）。

---

### Appendix B — VarByte Codec 規格與參考實作

#### B.1 編碼規則
- **LSB-first**：第一個 byte 攜帶最低 7 bits。
- **continuation bit = MSB**：MSB=1 表示「下一個 byte 還是同一個 number」，MSB=0 為終止。
- **零值**：用 1 byte `0x00` 表示。
- **uint32_t 上限**：5 bytes（因為 ⌈32/7⌉ = 5）。
- 與 Protobuf varint、Lucene VInt 相容（互通）。

| number    | bytes (hex)              |
| --------- | ------------------------ |
| 0         | `00`                     |
| 127       | `7F`                     |
| 128       | `80 01`                  |
| 255       | `FF 01`                  |
| 16383     | `FF 7F`                  |
| 16384     | `80 80 01`               |
| 2097151   | `FF FF 7F`               |
| UINT32_MAX| `FF FF FF FF 0F`         |

#### B.2 參考實作（`include/varbyte.hpp`）

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace idx::varbyte {

// LSB-first variable-length integer encoding.
inline void encode(std::uint32_t v, std::vector<std::uint8_t>& out) {
    while (v >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// Decode and advance `consumed` by number of bytes read.
// `data` must have at least 5 bytes available even if the value is small,
// or callers must guarantee sufficient bytes via the index format.
inline std::uint32_t decode(const std::uint8_t* data, std::size_t& consumed) {
    std::uint32_t v = 0;
    std::uint32_t shift = 0;
    consumed = 0;
    while (true) {
        std::uint8_t b = data[consumed++];
        v |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) return v;
        shift += 7;
        if (shift >= 35) throw std::runtime_error("varbyte: decode overflow");
    }
}

} // namespace idx::varbyte
```

#### B.3 既有 codebase 的 bug

[src/build_index.cpp:373](src/build_index.cpp) 的 `varbyteDecode(const std::vector<uint8_t>&)` 是「**MSB-first**」（從尾巴往頭組），但 [src/search_engine.cpp:529](src/search_engine.cpp) 的 `varbyteDecode(const uint8_t*, size_t&)` 是「**LSB-first**」（從頭往尾組）。**兩者編碼互不相容**，目前能勉強工作只是因為 build_index 在 encode 與 decode 都用同一個 broken function。重構後一律走 LSB-first（業界慣例 + 可串流 decode）。

#### B.4 為什麼不用 SIMD-BP128 或 PFOR

VarByte 雖簡單，decode 速度約 200–500 MB/s。如果要追求 GB/s 級，可改用 PForDelta、SIMD-BP128、Roaring bitmap。但本專案要保留「實作可讀性」與「壓縮比好說明」，VarByte 仍是最佳選擇。要量化分析時，可在附錄一句話帶過：「If switching to SIMD-BP128, query latency would drop further but at the cost of ~5% compression ratio.」

---

### Appendix C — DAAT / MaxScore / WAND 演算法

#### C.1 詞彙
- DAAT (Document-At-A-Time)：每個迭代鎖定一個候選 doc_id，跨所有 list 一起前進。
- TAAT (Term-At-A-Time)：一次處理一個 term 的整條 list，把分數累加到 score map。Top-k 時 DAAT 較省記憶體。
- pivot：當前候選 doc_id。

#### C.2 Disjunctive DAAT（OR）

```cpp
struct Cursor {
    int did = INT_MAX;
    int freq = 0;
    InvertedList* list;
};

auto cmp = [](const Cursor* a, const Cursor* b) { return a->did > b->did; };
std::priority_queue<Cursor*, std::vector<Cursor*>, decltype(cmp)> pq(cmp);

for (auto& c : cursors) if (c.list->next(c.did, c.freq)) pq.push(&c);

TopK heap(k);
while (!pq.empty()) {
    int did = pq.top()->did;
    double score = 0.0;
    while (!pq.empty() && pq.top()->did == did) {
        Cursor* c = pq.top(); pq.pop();
        score += bm25(c->list->idf, c->freq, doc_lengths[did], avgdl);
        if (c->list->next(c->did, c->freq)) pq.push(c);
    }
    heap.offer(did, score);
}
```

複雜度：O(P log Q)，P = 所有 posting 總和，Q = query term 數。

#### C.3 Conjunctive DAAT（AND）— max-pivot

```cpp
int pivot = 0;
while (true) {
    bool aligned = true;
    int max_did = pivot;
    for (auto& c : cursors) {
        if (!c.list->nextGEQ(pivot, c.did, c.freq)) return;
        if (c.did != pivot) aligned = false;
        max_did = std::max(max_did, c.did);
    }
    if (aligned) {
        heap.offer(pivot, sum_bm25(cursors, pivot));
        ++pivot;
    } else {
        pivot = max_did;
    }
}
```

`nextGEQ(t, did, freq)`：在當前 list 中前進，直到 `did >= t`；應用 block_info 的 `last_doc_id` 跳過整塊（block-skipping）。

#### C.4 MaxScore（top-k 早終止）

預先計算每個 term 的 BM25 上界 `UB(t)`（見 Appendix H.5）。query 階段：
1. 把 cursors 依 UB(t) 升序排列。
2. 維護「essential」前綴：一旦 `sum_{i<j} UB(t_i) < threshold`，後綴 j..n 的所有 term 才有可能讓 doc 進 top-k；前綴 0..j-1 是 non-essential。
3. 主迴圈只在 essential lists 上做 disjunctive DAAT；non-essential 用 `nextGEQ` 配合，省去無望 doc 的 score 計算。

對 BM25 + top-10 約可加速 3–10×。

#### C.5 Block-Max WAND (BMW)

更激進：在每個 block 紀錄 block-local `max_freq` 與 `min_doc_length`，可推 block-local 上界。query 時若 pivot block 的上界 < threshold，整 block skip。

本專案先實作 C.2 + C.3；MaxScore 與 BMW 列在 README 的 scope 外限制即可。

---

### Appendix D — External Merge Sort 細節

#### D.1 為何需要

build 階段 `MEMORY_LIMIT = 500 MB` 不足以容納 8.8M passages 的全 in-memory index（粗估 lexicon 0.5 GB + posting 4–6 GB）。所以分批 spill 到 `temp_index_<i>.bin`，最後 k-way merge。

#### D.2 spill 觸發點與 batch size

當前邏輯：`current_memory_usage > MEMORY_LIMIT` 即 dump in-memory `index` 到 temp file 並 clear。
- spill 後 `lexicon` 與 `term_id_to_word` **保留**（因為 term_id 必須跨 batch 一致）。
- spill 不會 reset `term_id`（otherwise 同一個 term 跨 batch 會分到不同 id）。

#### D.3 k-way merge 比較器最佳化

當前 [src/build_index.cpp:91](src/build_index.cpp) 的 `CompareIndexEntry::operator()`：

```cpp
return term_id_to_word->at(a.term_id) > term_id_to_word->at(b.term_id);
```

兩次 hash lookup + 兩次 string compare。merge 期間每 push/pop 都呼叫 → 高頻熱點。

**優化方案**：spill 階段直接以 string-sorted 寫 temp file，並把 term **字面字串**（而非 term_id）存入 `IndexEntry`：

```cpp
struct IndexEntry {
    std::string term;   // owned, sorted key
    int file_index;
    std::streamoff file_position;
    std::vector<std::pair<int,int>> postings;
};

struct CompareIndexEntry {
    bool operator()(const IndexEntry& a, const IndexEntry& b) const {
        return a.term > b.term;  // direct string compare, no hash
    }
};
```

進階：把 term 前綴 8 byte 以 uint64_t pack，先比 prefix，相同才比完整 string，cache-friendly。

#### D.4 IO buffer 與 fanout

- 預設 `std::ifstream` buffer 約 4 KB，對 N=20 個 temp file 的隨機 IO 很慢。
  ```cpp
  char buf[64 * 1024];
  file.rdbuf()->pubsetbuf(buf, sizeof(buf));
  ```
- macOS APFS / Linux ext4 對 64 KB sequential read 都有 good prefetch。
- fanout > 256 時 PQ 比較成本上升，可分兩階段 merge（先 16-way 合成中間 group，再合 group）。實務上 N < 50 直接 1-pass merge 即可。

#### D.5 Loser tree（題外話）

K 大時 loser tree 比較次數 O(log K) 比 heap 的 O(log K) 一樣，但常數低；本專案 K < 30 不必導入。

---

### Appendix E — SlabArena / PMR 細節

#### E.1 完整實作（`include/allocator.hpp`）

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

namespace idx::mem {

class SlabArena {
public:
    explicit SlabArena(std::size_t slab_bytes = 8 * 1024 * 1024)
        : slab_bytes_(slab_bytes) { grow(); }

    SlabArena(const SlabArena&) = delete;
    SlabArena& operator=(const SlabArena&) = delete;
    SlabArena(SlabArena&&) noexcept = default;
    SlabArena& operator=(SlabArena&&) noexcept = default;

    void* allocate(std::size_t bytes, std::size_t align) {
        auto cur = reinterpret_cast<std::uintptr_t>(cursor_);
        auto aligned = (cur + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        std::size_t pad = aligned - cur;
        if (pad + bytes > remaining_) {
            if (bytes + align > slab_bytes_) {
                // Oversized: dedicated slab so we don't waste current slab.
                slabs_.emplace_back(std::make_unique<std::byte[]>(bytes + align));
                auto base = reinterpret_cast<std::uintptr_t>(slabs_.back().get());
                auto a = (base + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
                bytes_in_use_ += bytes;
                return reinterpret_cast<void*>(a);
            }
            grow();
            return allocate(bytes, align);
        }
        cursor_ = reinterpret_cast<std::byte*>(aligned + bytes);
        remaining_ -= (pad + bytes);
        bytes_in_use_ += bytes;
        return reinterpret_cast<void*>(aligned);
    }

    // Reset to first slab; keep the slab to avoid free/alloc churn.
    void reset() noexcept {
        if (slabs_.size() > 1) slabs_.resize(1);
        cursor_ = slabs_.front().get();
        remaining_ = slab_bytes_;
        bytes_in_use_ = 0;
    }

    std::size_t bytes_in_use() const noexcept { return bytes_in_use_; }

private:
    void grow() {
        slabs_.emplace_back(std::make_unique<std::byte[]>(slab_bytes_));
        cursor_ = slabs_.back().get();
        remaining_ = slab_bytes_;
    }

    std::vector<std::unique_ptr<std::byte[]>> slabs_;
    std::byte* cursor_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t slab_bytes_;
    std::size_t bytes_in_use_ = 0;
};

class SlabResource final : public std::pmr::memory_resource {
public:
    explicit SlabResource(SlabArena* arena) : arena_(arena) {}

protected:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        return arena_->allocate(bytes, align);
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {
        // Monotonic; no per-allocation free.
    }
    bool do_is_equal(const memory_resource& other) const noexcept override {
        auto* o = dynamic_cast<const SlabResource*>(&other);
        return o && o->arena_ == arena_;
    }

private:
    SlabArena* arena_;
};

} // namespace idx::mem
```

#### E.2 PMR 容器接法

```cpp
idx::mem::SlabArena arena;
idx::mem::SlabResource res(&arena);

std::pmr::unordered_map<int, std::pmr::vector<std::pair<int,int>>> index{ &res };

auto& v = index[term_id];
if (v.get_allocator().resource() != &res) {
    // Sanity: PMR allocator should propagate. Use std::pmr::polymorphic_allocator
    // explicitly when constructing inner vectors.
}
```

陷阱：`std::pmr::unordered_map::operator[]` 預設**不會**把 outer allocator 傳給 inner vector；要用 `try_emplace` + `std::piecewise_construct` 或預先 `reserve` 然後手動 `vector(allocator)`：

```cpp
index.try_emplace(term_id, std::pmr::vector<std::pair<int,int>>(&res));
```

否則 inner vector 走 default new，arena 完全失效。**這是新手最常見的踩雷**。

#### E.3 Reset 時機

- **build 階段**：每次 spill 完 `index.clear()` 後 `arena.reset()`，下一輪 batch 全新 cursor。
- **query 階段**：每筆 query 開頭 reset thread-local arena；query 結束時 `score_acc.clear()` 但不 reset arena（讓下筆 query 重用記憶體）。

#### E.4 Thread safety

`SlabArena` 本實作**非** thread-safe（cursor / remaining 沒有 atomic）。多 thread 場景：
- 每個 worker 持有 thread-local arena（推薦）。
- 共享 arena 必加 `std::mutex`，在熱路徑會嚴重影響 latency。

#### E.5 alignment 邊界案例

- 對 8-byte align 的型別（如 `int64_t`），`std::align` 會 padding 0..7 bytes；slab 實際使用率約 95%，正常。
- `std::pmr::unordered_map` 內部 hash node 通常 alignment 8 或 16，要確保 `slab_bytes_` 是 16 的倍數（`8*1024*1024` 滿足）。

---

### Appendix F — mmap 與 I/O 策略

#### F.1 mmap 實作（POSIX）

```cpp
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <system_error>

namespace idx::io {

class MmapFile {
public:
    static MmapFile open_readonly(const char* path) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "open");
        struct stat st{};
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            throw std::system_error(errno, std::generic_category(), "fstat");
        }
        void* p = ::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            ::close(fd);
            throw std::system_error(errno, std::generic_category(), "mmap");
        }
        ::madvise(p, st.st_size, MADV_RANDOM);
        return MmapFile{fd, p, static_cast<std::size_t>(st.st_size)};
    }

    ~MmapFile() {
        if (data_ && data_ != MAP_FAILED) ::munmap(data_, size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MmapFile(MmapFile&& o) noexcept
        : fd_(o.fd_), data_(o.data_), size_(o.size_) {
        o.fd_ = -1; o.data_ = nullptr; o.size_ = 0;
    }

    const std::uint8_t* data() const noexcept {
        return static_cast<const std::uint8_t*>(data_);
    }
    std::size_t size() const noexcept { return size_; }

private:
    MmapFile(int fd, void* p, std::size_t s) : fd_(fd), data_(p), size_(s) {}
    int fd_ = -1;
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace idx::io
```

#### F.2 madvise 政策表

| 場景                            | 建議 madvise         |
| ------------------------------- | -------------------- |
| build 階段順序掃 collection     | `MADV_SEQUENTIAL`    |
| query 階段隨機跳 posting block  | `MADV_RANDOM`        |
| 啟動暖機（一次性 prefetch）     | `MADV_WILLNEED`      |
| 釋放冷區                        | `MADV_DONTNEED`      |

`MAP_POPULATE`（Linux 限定）會在 mmap 當下就 fault 全檔頁面進記憶體；對 8 GB index + 64 GB RAM 機器最爽，可 +30% startup 但避免第一筆 query 抖動。macOS 沒有，需手動 `madvise(MADV_WILLNEED)` 或 read-touch 一遍（不推薦）。

#### F.3 cold vs warm cache benchmark

兩者數字差距常 5–10×。報告 latency 時：
- **cold**：先 `purge`（macOS）或 `echo 3 > /proc/sys/vm/drop_caches`（Linux），跑 1 筆。
- **warm**：跑 100 筆暖機後再量。
- README 兩者都列，註明哪個是 steady-state（warm）。

#### F.4 Huge Pages（option）

Linux 上 `madvise(MADV_HUGEPAGE)` 可拿 2 MB 大頁，TLB miss 大降。對 8 GB index 約 +5–10% throughput；macOS 不適用。stretch goal。

---

### Appendix G — ThreadPool & 並行細節

#### G.1 完整實作（`include/thread_pool.hpp`）

```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace idx::concurrent {

class ThreadPool {
public:
    explicit ThreadPool(unsigned n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 1;
        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            workers_.emplace_back([this](std::stop_token st) { run(st); });
        }
    }

    ~ThreadPool() {
        for (auto& w : workers_) w.request_stop();
        cv_.notify_all();
    }

    template <class F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        {
            std::lock_guard lk(m_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    std::size_t size() const noexcept { return workers_.size(); }

private:
    void run(std::stop_token st) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return st.stop_requested() || !tasks_.empty(); });
                if (st.stop_requested() && tasks_.empty()) return;
                job = std::move(tasks_.front());
                tasks_.pop();
            }
            job();
        }
    }

    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable_any cv_; // works with stop_token
};

} // namespace idx::concurrent
```

#### G.2 為何 query level 並行而非 term level

- term level 並行：把一筆 query 的多個 term 散到多個 thread 同時 decode，再合併分數。問題：
  1. 通常 query 只有 2–6 term，並行收益小於 spawn cost。
  2. score map 跨 thread 合併需 lock 或 reduce。
  3. cache 競爭嚴重（每 thread 抓不同 list 區塊）。
- query level 並行：把 1000 筆 query 分散到 8 thread，每 thread 處理 ~125 筆。
  1. thread 間零共享狀態。
  2. throughput 接近線性。
  3. 適合 RAG batch 場景。

#### G.3 false sharing 防範

worker 共享的進度計數器，每個 atomic 用 `alignas` 隔到不同 cache line：

```cpp
struct alignas(std::hardware_destructive_interference_size) Counter {
    std::atomic<std::size_t> value{0};
};
```

#### G.4 thread-local 狀態

每個 worker 建構一個 `WorkerCtx`（thread_local 或於 worker lambda capture）：
```cpp
struct WorkerCtx {
    idx::mem::SlabArena arena;
    std::pmr::unordered_map<int, double> score_acc;
    std::vector<std::uint8_t> decode_buf;  // sized for one block
    // Optional: per-thread ifstream if not using mmap.
};
```

WorkerCtx 跟著 thread 一輩子，不要每次 query 重建。

#### G.5 為何選 single MPMC queue 而非 work-stealing

- 實作簡潔，~50 LOC。
- query 工作量基本 uniform，沒有「某 thread 卡長 query」的尾部問題。
- work-stealing 在不均勻 workload（例如混雜 1ms 與 1s task）才必要；對 retrieval 場景 over-engineering。

---

### Appendix H — BM25 詳細推導與上界

#### H.1 完整公式

對 query Q、doc d：

\[
\mathrm{BM25}(Q, d) = \sum_{t \in Q} \mathrm{IDF}(t) \cdot \frac{f(t, d) \cdot (k_1 + 1)}{f(t, d) + k_1 \cdot \big(1 - b + b \cdot \frac{|d|}{\mathrm{avgdl}}\big)}
\]

- \(f(t, d)\)：term t 在 doc d 出現次數
- \(|d|\)：doc d 的 token 總數
- \(\mathrm{avgdl}\)：collection 平均 doc 長度
- \(k_1\)：term frequency 飽和速度，常用 1.2
- \(b\)：document length normalization 強度，常用 0.75

#### H.2 IDF (Robertson-Spärck Jones, smoothed)

\[
\mathrm{IDF}(t) = \ln \left( \frac{N - \mathrm{df}(t) + 0.5}{\mathrm{df}(t) + 0.5} + 1 \right)
\]

`+1` 把可能的負值（理論上 df > N/2 時內項小於 1，log 為負）抬到 0 以上，是 Lucene 採用的 smoothed 版本；標準 BM25 paper 並沒有 +1，但 IR 業界 baseline 都 +1。

#### H.3 在 codebase 內對映

[src/search_engine.cpp:377](src/search_engine.cpp)：
```cpp
double computeIDF(int term_freq) {
    return std::log((total_docs - term_freq + 0.5) / (term_freq + 0.5) + 1.0);
}
```
- `term_freq` 名稱誤導，實際傳的是 df（`postings_num`）。重構時改名 `df`。
- 變數 `total_docs` 在 [src/search_engine.cpp:325](src/search_engine.cpp) 是區域變數，沒寫回 class 成員 → 主成員永遠 0 → IDF 會崩壞為 `log(0.5/(df+0.5)+1)` 這種非零但無意義的值。這是 P1 必修 bug 之一（與 `term_id_to_word` 同一系列）。

#### H.4 TF normalisation

```cpp
double computeTF(int freq, int doc_length) {
    return (freq * (k1 + 1)) / (freq + k1 * (1 - b + b * (doc_length / avg_doc_length)));
}
```
注意 `doc_length / avg_doc_length` 是整數除浮點，因為 `avg_doc_length` 是 double，整體會走 implicit conversion，OK。但在程式碼可讀性上建議顯式 `static_cast<double>(doc_length) / avg_doc_length`。

#### H.5 Term 上界（給 MaxScore / WAND）

\[
\mathrm{UB}(t) = \mathrm{IDF}(t) \cdot \frac{f_\max(t) \cdot (k_1 + 1)}{f_\max(t) + k_1 \cdot \big(1 - b + b \cdot \frac{|d|_\min}{\mathrm{avgdl}}\big)}
\]

- \(f_\max(t)\)：term t 在 collection 內出現次數最多的那筆 posting freq。
- \(|d|_\min\)：collection 最短 doc 長度（為了保守估計）。

build_index 階段順手算 `f_max` 寫入 lexicon 額外欄位（也可進一步算 block-local f_max → BMW）。

#### H.6 數值校驗

寫一支 [tests/test_bm25.cpp](tests/test_bm25.cpp)：

```cpp
// Reference values from Lucene 9.10 BM25 with k1=1.2, b=0.75.
// avgdl=10, |d|=5, df=2, N=10, freq=3
// expected score = idf * tf
//   idf = ln((10 - 2 + 0.5) / (2 + 0.5) + 1) = ln(4.4) ≈ 1.4816
//   tf  = 3 * 2.2 / (3 + 1.2 * (1 - 0.75 + 0.75 * 0.5))
//       = 6.6 / (3 + 1.2 * 0.625) = 6.6 / 3.75 = 1.76
//   score ≈ 2.6076
```

跟 Lucene 對齊比起跟自家 reference impl 對齊更可信。

---

### Appendix I — MS MARCO Passage v1 格式坑

#### I.1 collection.tsv

- 行數 8,841,823；總 size 約 2.9 GB
- 格式 `<pid>\t<passage>\n`，pid 連續從 0 到 8841822
- 平均 passage 約 56 token，最長 700+ token
- 殘留 HTML：`&amp;`、`&lt;`、`<p>`，當前 tokenizer 用 `isalpha` + `isdigit` 自動丟棄
- 少量非 ASCII 字元（歐語、emoji）；目前用 `std::isalpha(unsigned char)` 可能因 locale 把這些算成 alpha → token 變亂

**建議**：tokenizer 改 ASCII fast-path：
```cpp
inline bool is_token_char(char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}
```

#### I.2 dev queries / qrels

- `queries.dev.small.tsv`：6980 行，`<qid>\t<query>`
- `qrels.dev.small.tsv`：每行 `<qid> 0 <pid> 1`（binary）
- dev 平均每筆 query 只有 1.06 個 relevant doc → MRR@10 是主要指標

#### I.3 TREC DL19 / DL20

- DL19 queries: 43 筆；qrels: graded 0/1/2/3
- DL20 queries: 54 筆；qrels: graded 0/1/2/3
- 慣例：`>=2` 為 relevant 計算 recall；nDCG 直接吃 graded
- 雖然 query 很少，但 qrels 標注密集（每 query 數百筆 graded），nDCG 結果穩定可比

#### I.4 baseline 預期數字（Anserini, BM25 k1=0.82, b=0.68）

| Set      | MRR@10 | nDCG@10 | Recall@1000 |
| -------- | ------ | ------- | ----------- |
| Dev      | 0.187  | —       | 0.857       |
| DL19     | —      | 0.506   | 0.745       |
| DL20     | —      | 0.480   | 0.786       |

我們用 `(k1=1.2, b=0.75)`、tokenizer 較簡單（沒 stemming）、沒 stop-word：MRR@10 可能落 0.165–0.180、Recall@1000 落 0.80–0.86。**只要在這範圍內就算正確**。落到 0.10 以下就是 bug（典型：IDF/avgdl/df 算錯）。

---

### Appendix J — TREC Eval 格式與 pytrec_eval

#### J.1 Run file 格式（whitespace separated，必須 6 欄）

```
<qid> Q0 <docid> <rank> <score> <tag>
```

- `Q0` 是歷史遺跡，固定字串；不可省略
- `rank` 從 1 開始，同 qid 內遞增
- `score` 同 qid 內須單調**遞減**（pytrec_eval 不依賴 rank，但若 rank 與 score 不一致會被警告）
- `tag` 任意字串（如 `IDX_BM25`）；允許 . 與 _，**不可有空白**

範例：
```
1048585 Q0 7187158 1 18.4257 IDX_BM25
1048585 Q0 1782337 2 17.9214 IDX_BM25
1048585 Q0 39449   3 17.6642 IDX_BM25
```

#### J.2 Qrels 格式

```
<qid> 0 <docid> <rel>
```

- 第二欄 `0` 也是歷史遺跡
- `rel`：dev 是 0/1；DL 是 0/1/2/3
- 同 qid 同 docid 不可重複

#### J.3 pytrec_eval metric keys

精確字串（不同版本略有差異）：
- `recip_rank` — MRR over all ranks
- `recip_rank_cut.10` — MRR@10
- `ndcg_cut_10` — nDCG@10
- `recall_1000` — Recall@1000
- `map` — MAP
- `success_10` — Success@10

Python：
```python
import pytrec_eval
qrels = pytrec_eval.parse_qrel(open(qrels_path))
run   = pytrec_eval.parse_run(open(run_path))
ev = pytrec_eval.RelevanceEvaluator(
    qrels, {'recip_rank_cut.10', 'ndcg_cut_10', 'recall_1000'})
results = ev.evaluate(run)
# results: {qid: {metric: value}}
```

聚合方式：對所有 qid 取算術平均（per-query，不是 micro-average）。

#### J.4 常見錯誤

- qid / docid 全部當字串。`"00123"` 與 `"123"` 是不同 qid → 用 `int(...)` 統一是常見手法。
- 把 dev qrels（binary）與 DL qrels（graded）混用 → MRR@10 會莫名變低或變 0。
- run file 缺 `Q0` 欄 → pytrec_eval 拋 `IndexError`。

---

### Appendix K — Latency 量測方法論

#### K.1 Coordinated Omission

closed-loop 客戶端（送一筆等回應再送下一筆）遇到 server 卡 100ms 時，會錯過 100 筆「應該抖一下」的 query → P99 嚴重低估。
**正確做法**：open-loop 固定 issue rate，每 1ms 投一筆，無論前一筆完成；總延遲 = service-time + queue-time。

#### K.2 std::chrono 選哪個

| Clock                         | 適用              |
| ----------------------------- | ----------------- |
| `system_clock`                | 顯示牆上時間，**不要用** |
| `steady_clock`                | latency 量測       |
| `high_resolution_clock`       | 平台依賴，**不要用** |

#### K.3 測量 boilerplate

```cpp
auto t0 = std::chrono::steady_clock::now();
auto results = engine.search(query, opts);
auto t1 = std::chrono::steady_clock::now();
auto us = std::chrono::duration<double, std::micro>(t1 - t0).count();
latencies.push_back(us);
```

#### K.4 分位數計算

```cpp
auto pct = [&](double p) {
    auto idx = static_cast<size_t>(p * latencies.size());
    std::nth_element(latencies.begin(), latencies.begin() + idx, latencies.end());
    return latencies[idx];
};
double p50 = pct(0.50), p95 = pct(0.95), p99 = pct(0.99);
```

要 P99.9：用 `std::sort` 全排序，或導入 [HdrHistogram_c](https://github.com/HdrHistogram/HdrHistogram_c)。本專案 P99 即夠。

#### K.5 Warm-up 數量

- mmap index：cold 第一筆 100–500 ms，warm 1–10 ms。
- 跑 100–200 筆 dummy query 即可暖好 lexicon hash + 常用 posting page。
- warm-up 數字**絕對不能**算進 P50/P95/P99。

#### K.6 throughput 量測

固定總 query 數（如 10000），用 thread pool 灌滿：
```
QPS = N / total_wall_time
```
multi-thread 時 wall_time 應從第一個 task submit 到最後一個 task done。

---

### Appendix L — Memory 量測方法論

#### L.1 平台差異總覽

| 工具                       | 平台   | 單位 | 量測語意           |
| -------------------------- | ------ | ---- | ------------------ |
| `/usr/bin/time -l`         | macOS  | bytes| peak RSS (rusage)  |
| `/usr/bin/time -v`         | Linux  | KB   | peak RSS (rusage)  |
| `/proc/self/status` VmHWM  | Linux  | KB   | peak RSS           |
| `/proc/self/status` VmPeak | Linux  | KB   | peak virtual mem   |
| `/proc/self/smaps_rollup`  | Linux  | KB   | PSS（共享頁公平分） |
| `mach_task_basic_info`     | macOS  | bytes| live + peak RSS    |

#### L.2 macOS 即時 RSS（自家進程內）

```cpp
#include <mach/mach.h>

std::size_t resident_max_bytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &cnt) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size_max;
}
```

#### L.3 mmap 對 RSS 的污染

Linux：mmap shared page 計入 RSS，但是「pageable / discardable」的；**對比實驗時要兩個 binary 都走 mmap**，否則差值會被 mmap 入帳放大。

更精確：用 `/proc/self/smaps_rollup` 的 `Pss_Anon`（只算 anonymous heap pages）：

```bash
grep ^Pss_Anon /proc/self/smaps_rollup
```

#### L.4 jemalloc / tcmalloc 干擾

- glibc malloc 預設 hold per-thread arena，RSS 通常虛高 5–15%。
- 做履歷數字時建議 `LD_PRELOAD=$(brew --prefix jemalloc)/lib/libjemalloc.dylib`（macOS）統一兩組實驗。
- 重要：**baseline 與 experimental 必須同樣 malloc**，否則差值不可信。

#### L.5 量測腳本骨架（`bench/run_memory.sh`）

```bash
#!/usr/bin/env bash
set -euo pipefail
RES_DIR=bench_results
mkdir -p "$RES_DIR"

run_one() {
  local label="$1"; local bin="$2"
  echo "=== $label ==="
  /usr/bin/time -l "$bin" data/collection.tsv 2> "$RES_DIR/${label}.time"
  awk '/maximum resident set size/ { print $1 }' "$RES_DIR/${label}.time"
}

baseline_rss=$(run_one baseline ./build/baseline/build_index)
arena_rss=$(run_one arena    ./build/arena/build_index)

python3 - <<EOF
b=$baseline_rss; a=$arena_rss
saved = (b - a) / b * 100
print(f"| Build path | Peak RSS | Reduction |")
print(f"| ---------- | -------- | --------- |")
print(f"| Default allocator | {b/2**30:.2f} GB | baseline |")
print(f"| SlabArena + move  | {a/2**30:.2f} GB | {saved:.1f}% |")
EOF
```

---

### Appendix M — JSON / subprocess 死鎖防範

#### M.1 nlohmann/json drop-in

下載 `single_include/nlohmann/json.hpp` 放 `include/third_party/json.hpp`，CMake 不需 link，header-only。

#### M.2 server 模式骨架

```cpp
#include "third_party/json.hpp"
using nlohmann::json;

int main_server() {
    std::ios::sync_with_stdio(false);
    std::cout.tie(nullptr);

    SearchEngine engine{...};
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req, resp;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            resp = {{"error", std::string("bad json: ") + e.what()}};
            std::cout << resp.dump() << '\n' << std::flush;
            continue;
        }
        std::string q = req.value("q", "");
        int k = req.value("k", 10);
        bool conj = req.value("mode", "disjunctive") == "conjunctive";

        auto results = engine.search(q, {.top_k = k, .conjunctive = conj});
        resp["q"] = q;
        resp["results"] = json::array();
        for (auto& r : results) {
            resp["results"].push_back({
                {"rank", r.rank}, {"doc_id", r.doc_id},
                {"score", r.score}, {"passage", r.passage},
            });
        }
        std::cout << resp.dump() << '\n' << std::flush;
    }
    return 0;
}
```

#### M.3 subprocess 死鎖三大原因

1. **C++ 端沒 flush**：每筆 response 必須 `std::cout << '\n' << std::flush;`，不要 `std::endl`（雖然 endl 也 flush，可讀性差）。
2. **Python 端沒 drain stderr**：child 寫 stderr 滿 64 KB pipe buffer 後 block；Python 端必須有 thread / asyncio 持續抽 stderr。
3. **緩衝區交叉飽和**：response 過大（>64 KB）+ Python 端在處理 → 限制 passage 截斷至 512 chars，或改 framing 協議（每 message 前綴長度）。

#### M.4 Python client 安全寫法

```python
import json, subprocess, threading

class SearchClient:
    def __init__(self, cli_args: list[str]):
        self.p = subprocess.Popen(
            cli_args,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)  # line-buffered text mode
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._drain, daemon=True)
        self._t.start()

    def _drain(self):
        for line in self.p.stderr:
            if not self._stop.is_set():
                print("[search_cli stderr]", line, end="")

    def search(self, q: str, k: int = 10) -> list[dict]:
        self.p.stdin.write(json.dumps({"q": q, "k": k}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("search_cli died")
        return json.loads(line)["results"]

    def close(self):
        self._stop.set()
        try:
            self.p.stdin.close()
        finally:
            self.p.terminate()
            self.p.wait(timeout=2)
```

---

### Appendix N — CMake 完整範本

#### N.1 推薦 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(idx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Codec selection: VarByte (default) | Raw32
set(CODEC "VarByte" CACHE STRING "Posting codec")
set_property(CACHE CODEC PROPERTY STRINGS VarByte Raw32)

# Optimisation flags
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fno-omit-frame-pointer)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_compile_options(-mcpu=apple-m1)
    else()
        add_compile_options(-march=native)
    endif()
endif()

# Sanitizer build
option(IDX_ASAN "Enable AddressSanitizer + UBSan" OFF)
if(IDX_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Dependencies
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
set(LibArchive_INCLUDE_DIR "/opt/homebrew/opt/libarchive/include")
set(LibArchive_LIBRARY     "/opt/homebrew/opt/libarchive/lib/libarchive.dylib")

# Common include path
include_directories(${CMAKE_SOURCE_DIR}/include)

# Targets
add_executable(build_index src/build_index.cpp)
target_compile_definitions(build_index PRIVATE IDX_CODEC_${CODEC})
target_include_directories(build_index PRIVATE ${LibArchive_INCLUDE_DIR})
target_link_libraries(build_index PRIVATE ${LibArchive_LIBRARY} ZLIB::ZLIB Threads::Threads)

add_executable(search_cli src/search_cli.cpp src/search_engine.cpp)
target_compile_definitions(search_cli PRIVATE IDX_CODEC_${CODEC})
target_link_libraries(search_cli PRIVATE Threads::Threads ZLIB::ZLIB)

# Benchmarks
add_executable(bench_compression bench/bench_compression.cpp)
add_executable(bench_latency     bench/bench_latency.cpp     src/search_engine.cpp)
add_executable(bench_index_size  bench/bench_index_size.cpp)
target_link_libraries(bench_latency PRIVATE Threads::Threads ZLIB::ZLIB)

# Eval
add_executable(trec_run_writer eval/trec_run_writer.cpp src/search_engine.cpp)
target_link_libraries(trec_run_writer PRIVATE Threads::Threads ZLIB::ZLIB)

# Tests
enable_testing()
add_executable(test_varbyte       tests/test_varbyte.cpp)
add_executable(test_inverted_list tests/test_inverted_list.cpp)
add_executable(test_bm25          tests/test_bm25.cpp)
add_executable(test_thread_pool   tests/test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE Threads::Threads)
add_test(NAME varbyte       COMMAND test_varbyte)
add_test(NAME inverted_list COMMAND test_inverted_list)
add_test(NAME bm25          COMMAND test_bm25)
add_test(NAME thread_pool   COMMAND test_thread_pool)
```

#### N.2 多 codec 並行 build

```bash
cmake -B build/varbyte -DCODEC=VarByte -DCMAKE_BUILD_TYPE=Release
cmake -B build/raw32   -DCODEC=Raw32   -DCMAKE_BUILD_TYPE=Release
cmake --build build/varbyte -j
cmake --build build/raw32   -j
```

兩個 build directory 完全隔離，可並行使用。

#### N.3 sanitizer build

```bash
cmake -B build/asan -DIDX_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/asan -j
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    ./build/asan/test_thread_pool
```

#### N.4 GitHub Actions

```yaml
name: ci
on: [push, pull_request]
jobs:
  build-test:
    strategy:
      matrix:
        os: [ubuntu-latest]
        codec: [VarByte, Raw32]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y libarchive-dev zlib1g-dev cmake g++-13
      - run: cmake -B build -DCMAKE_CXX_COMPILER=g++-13 -DCODEC=${{ matrix.codec }} -DCMAKE_BUILD_TYPE=Release
      - run: cmake --build build -j
      - run: ctest --test-dir build --output-on-failure
```

---

## 跨階段共用注意事項

### 程式風格
- 所有 code comment 使用英文。
- C++ 標準：C++23（已用 `std::jthread`、`std::pmr`）。
- Naming：類別 `PascalCase`、函式/變數 `snake_case`、常數 `kCamelCase` 或 `UPPER_SNAKE` 二擇一保持一致。
- 例外：盡量不用 exception；錯誤回傳 `std::expected<T,E>`（C++23）或 `std::optional<T>`。

### Build
- 預設 `-DCMAKE_BUILD_TYPE=Release` + `-O3`，benchmark 用此模式跑。
- 額外 target：`Debug` 開 `-fsanitize=address,undefined`、`RelWithDebInfo` 給 perf。
- macOS：避免 `-march=native`，改用 `-mcpu=apple-m1` 或讓 CMake 偵測。
- Linker：libarchive、zlib 已知；新增 nlohmann/json 的 single-header 不需額外 link。

### 資料一致性
- 整個 pipeline 對 doc_id 的定義必須一致：MS MARCO 的 `pid`，0-indexed，連續至 8841822。
- `document_info.txt` 第 i 行對應 doc_id i；若中斷需重 build。
- `final_sorted_lexicon.txt` 的 `posting_number` 即為 df，不可寫成 cf。

### Git
- 在 `main` 上開分支：`feat/p1-refactor`、`feat/p2-codec` …，每個 phase 一個 PR；PR 描述貼上 phase 的「驗收」清單，全勾才合併。
- 不要把 `data/`、`build/`、`final_sorted_*`、`temp_index_*` commit 進去。

### 履歷對齊檢查清單
- [x] *Built an inverted index over 8 million documents* → P0 + P7 跑通 8841823 docs
- [x] *BM25 ranking* → P1 修好 BM25
- [x] *parallel query execution* → P4 thread pool + search_batch
- [x] *RAG-style retrieval workflows* → P5 search_cli server + P8 rag_demo
- [x] *Programmatically evaluated retrieval via latency* → P6 bench_latency
- [x] *... index size, compression ratio* → P2 + P6 bench_index_size
- [x] *... and ranking metrics* → P7 trec_run_writer + run_eval.py
- [x] *automated LLM and retrieval evaluation for grounded agent responses* → P8 rag_demo + P7 metrics
- [x] *Reduced index memory by 60% via VarByte compression* → P2 表格產出（實測 posting-store 省 69.0%）
- [ ] *lowered peak memory by 40% with move semantics and custom allocation* → P3 + P6 bench_memory（本次實測為 +3.5% RSS，不能宣稱降低）

只有 100% 勾完，履歷描述才完全有實證。
