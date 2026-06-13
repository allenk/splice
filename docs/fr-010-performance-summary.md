# FR-010 — Splice 效能改善總覽與 `std::function` 真相

**狀態：** 知識庫（KB）/ 給未來閱讀者
**對應 FR：** FR-010 全系列（Step 3.5 / 5 / 6.1–6.4）
**撰寫日期：** 2026-05-27
**作者：** Claude + Allen
**位階：** 這份是 **總覽**。各 Step 的細節報告在：
- [`benchmark-baseline.md`](./benchmark-baseline.md) — 原始 baseline
- [`template-callback-evaluation.md`](./template-callback-evaluation.md) — Step 5 設計評估
- [`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md) — std::function vs thunk 實證
- [`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md) — Step 6 設計
- [`fr-010-step6-bench-results.md`](./fr-010-step6-bench-results.md) — Step 6 實證

---

## 一句話總結（給後人）

> **熱路徑優化的勝負關鍵在 critical section / 同步機制，不在 dispatch wrapping。
> Splice 案例：sync 層改動帶來 −42% 至 −92%，包裝層改動只有 ~5%。**

兩個量級的差距 —— 這條紀律值得固化。動手前先 microbench 驗證假設，別憑教科書
直覺攻擊 `std::function`。

---

## TL;DR

| 指標 | 起點（pre-FR-010）| Step 3.5 | Step 6 | 目標 |
|---|---|---|---|---|
| 1-thread hooked call | 41.1 ns | 23.7 ns | 22.0 ns | < 20 ns |
| 8-thread hooked call | 1568 ns | 688 ns | 923 ns † | — |
| 8t/1t ratio | 39.1× | 26.9× | 42×† | < 5× |
| Registry lookup（純隔離）8t/1t | — | — | **3.9–5.5×** ✅ | < 5× |

† Step 6 的 trampoline 數字不是退化，是不同的 bench shape（registry 已贏了，剩
下殘留 bouncer 在 `std::function` 之外的某處 —— 詳見 [`fr-010-step6-bench-results.md`](./fr-010-step6-bench-results.md)）。

**最大發現**：原本以為 `std::function` 是熱路徑頭號嫌犯，**實測證明它只佔 1.4–1.5 ns**，
換成 thunk 只能再省 ~1 ns。我們做了一個小 microbench 才搞清楚這件事，避免了一個
~200 行的無效重構。

---

## 一、效能改善的旅程

### 改善前 — baseline（2026-05-09 量測）

硬體：AMD Ryzen 9 9950X3D，MSVC /O2，Release。

```
BM_HookedCall (1 thread):       41.1 ns/call
BM_HookedCall_Contended/8t:    1568 ns/call  (ratio 39.1×)
```

組成（事後分析）：
- `std::recursive_mutex` 鎖 / 解鎖：~10 ns
- `std::unordered_map<int, shared_ptr<Hook>>` find：~10 ns
- `std::function::operator()` dispatch：~1.4 ns
- 真正 trampoline + hooked function body：~20 ns

### Step 3.5 — `shared_mutex` 取代 `recursive_mutex`（2026-05-09）

**單一改動**：`HookContext::m_mutex` 從 `std::recursive_mutex` 換成 `std::shared_mutex`，
reader path 用 `shared_lock`，writer path 用 `unique_lock`。

```cpp
// Before
std::recursive_mutex m_mutex;
std::lock_guard lock(m_mutex);
// → atomic RMW under contention; serialises ALL readers

// After
std::shared_mutex m_mutex;
std::shared_lock lock(m_mutex);   // multiple readers OK
// → reader counter atomic RMW still bounces, but no full serialisation
```

| 指標 | 改善 |
|---|---|
| 1-thread | 41.1 → **23.7 ns**（−42%）|
| 8-thread | 1568 → **688 ns**（−56%）|
| 8t/1t | 39.1× → **26.9×** |

清掉了「全串行化」這一層，但 reader counter cache-line ping-pong 仍是
8-thread 的最大殺手。

### Step 6.1 — escape hatch 框架（2026-05-23）

**動機**：要做 RCU 但 **不強迫所有使用者吃 RCU 的 trade-off**（writer 慢、記憶體
2× 暫時、AOSP toolchain 風險）。確立工程原則：「有 trade-off 的優化必須附 escape
hatch」。

**改動**：
- 新增 `include/splice/registry_impl.h`，定義 `SPLICE_REGISTRY_IMPL` macro
- 用 partial specialisation 把 `HookContext::m_hooks` 邏輯抽到 `HookRegistry<Impl>`
- 預設 `Impl = registry::shared_mutex_map`，**行為與 Step 3.5 完全一致**
- opt-in：build 時下 `-DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array`

零行為變動，零效能變動。為 Step 6.2 鋪路。

### Step 6.2 — RCU 實作（2026-05-25）

兩個新 specialisation：
- `HookRegistry<rcu_atomic_array>` — `std::atomic<Snapshot*>` 發布、`std::array<shared_ptr, SPLICE_MAX_HOOKS>` 索引。Reader = 單一 acquire load
- `OriginalsRegistry<rcu_atomic_array>` — `std::array<std::atomic<void*>, SPLICE_MAX_HOOKS>`。Reader = 單一 atomic load

`SPLICE_MAX_HOOKS = 512`（決議 2026-05-19），覆蓋 AOSP 系統服務典型 hook 量，
保持 8 KiB snapshot 在 L1d 內。

### Step 6.3 — 實證（2026-05-26）

**Registry-lookup 隔離 microbench**（`benchmark/bench_registry_lookup.cpp`，
直接驅動 HookRegistry / OriginalsRegistry，**跳過 trampoline**）：

| Op | shared_mutex_map 1t→8t | rcu_atomic_array 1t→8t | RCU 加速 |
|---|---|---|---|
| HookRegistry::get | 10.1 → 522 ns | **0.96 → 5.24 ns** | 100× @ 8t |
| OriginalsRegistry::get | 9.79 → 473 ns | **1.29 → 5.09 ns** | 93× @ 8t |

**Registry 層的 8t/1t ratio：**
- shared_mutex_map: ~50×（這就是 cache-line bouncing 的本性）
- **rcu_atomic_array: 3.9–5.5×（清掉 < 5× gate）**✅

Full trampoline 數字沒有等比例改善（923 ns @ 8t，ratio ~42×）——剩下的 bouncer
不在 registry，**也不在 `std::function`**（見下一節）。

### Step 6.4 — 時間延遲回收（2026-05-26）

RCU snapshot 不能無限累積。原設計用 EBR（Epoch-Based Reclamation），但 EBR 的
標準寫法要在 reader 進出 critical section 時做 atomic ops，每次 trampoline 加
3–9 ns，**會抹消 Step 6.2/6.3 的全部收益**。

改用 **time-deferred reclamation**：
- Writer 退役舊 snapshot 時記錄時間戳
- 下一個 writer 來時掃 retire queue，釋放 > 100 ms 老的條目
- Reader **零額外成本**

Trade-off：reader thread 被排程器搶占 > 100 ms 期間，舊 snapshot 可能被 freed。
實務上 Splice 的 trampoline 是 ns–μs 等級，100 ms 有 10–100× headroom。

---

## 二、`std::function` 真相 — 我們做錯的假設

### 一開始的假設

進入 Step 5（template callback）設計時，我預估：

> `std::function` 在熱路徑上的開銷 **~4–5 ns**：
> - vtable hop (~2 ns)
> - manager pointer 二次間接層 (~1 ns)
> - heap-allocated capture 的 cache miss (~2 ns)
> - 換成 thunk 後預估省 **3–4 ns / call**

這是 **教科書版** `std::function` 的代價估算。Step 5 的設計文件
`template-callback-evaluation.md` 一度寫著「預估端對端 23.7 → 19 ns / call（清
< 20 ns gate）」。

### Allen 的工程紀律

> 「但是我擔心的效能是否真如你說的那樣。我們是否應該先做個小 bench
> 在 PC 與 Android 實現這個驗證。然後有數據為基礎再來展開這個改動。」
> — Allen，2026-05-18

**這個提問救了我們 ~200 行的無效重構。**

### 實證 microbench 設計

`benchmark/bench_callback_storage.cpp` — **不碰 Splice 任何東西**，直接在
microbench 內定義三條本地路徑：

```cpp
// Path 1: Floor
int x = hot_target(x) + 1;   // no indirection

// Path 2: 現況鏡像
struct StdFunctionStorage {
    std::atomic<std::function<int(OrigFn, int)>*> m_fn;
    int invoke(...) { return (*m_fn.load())(orig, x); }  // std::function dispatch
};

// Path 3: Step 5 提案
struct ThunkStorage {
    std::atomic<int(*)(void*, OrigFn, int)> m_thunk;
    std::atomic<void*> m_state;
    int invoke(...) { return m_thunk.load()(m_state.load(), orig, x); }
};
```

三條路徑共用同一個 captureless lambda body、同一個 noinline target。差別純粹在
dispatch 機制。

### 雙平台實測（2026-05-19）

#### Windows x86_64（Ryzen 9 9950X3D, MSVC /O2）

| Path | mean | delta vs Direct |
|---|---|---|
| BM_DirectCall | 2.60 ns | 0 |
| BM_StdFunctionPath | 3.99 ns | **+1.39 ns** |
| BM_ThunkPath | 3.09 ns | **+0.49 ns** |

**Δ(StdFunction − Thunk) = 0.90 ns/call（−23% dispatch overhead）**

#### Android ARM64（Snapdragon 8 Gen 3, Clang 19 -O2）

| Path | mean | delta vs Direct |
|---|---|---|
| BM_DirectCall | 3.04 ns | 0 |
| BM_StdFunctionPath | 4.56 ns | **+1.52 ns** |
| BM_ThunkPath | 3.35 ns | **+0.31 ns** |

**Δ(StdFunction − Thunk) = 1.21 ns/call（−27% dispatch overhead）**

### 為什麼 `std::function` 比我以為的便宜這麼多？

我的預設模型是「教科書版」`std::function`：堆積分配、vtable hop、manager
indirection。實際發生的是：

1. **SBO（small-buffer optimisation）起作用**
   - Captureless lambda 完全裝得進 `std::function` 的內建 buffer（典型 ~16 bytes）
   - 沒有 heap 分配、沒有 cache miss
   - 跨平台 libc++/MS-STL 都實作了 SBO

2. **編譯器做了 devirtualisation**
   - MSVC 19.44 / Android Clang 19 都很積極
   - Lambda 型別在 `store` 時可見 → manager 函式型別 inline
   - vtable hop 折成直接 call

3. **CPU 分支預測 + BTB**
   - Function pointer 間接呼叫，命中 BTB 後成本接近直接 call
   - 第一次呼叫成本高，後續穩定路徑成本極低
   - Bench 跑 1.2 × 10⁹ 次迭代，所有間接呼叫早就 warm 了

### 結論

| 維度 | 假設 | 實證 |
|---|---|---|
| `std::function` 開銷 | ~4–5 ns | **~1.4–1.5 ns** |
| 換 thunk 預期省 | 3–4 ns/call | **~1 ns/call** |
| 是否清 < 20 ns gate？ | Yes | **No**（23.7 → 22.8 ns） |
| 是否值得 ~200 行重構？ | Yes | **No**（投入產出比偏低）|

**Step 5（template callback）被暫緩**。後續優先做 Step 6（RCU registry），收益
大得多（看 Step 6.3 的數字）。

---

## 三、那殘留的 bouncer 在哪？

Step 6.3 把 registry 隔離測到 < 5× 但 full trampoline 還是 ~42×。差值：

| | 1t | 8t | 8t/1t |
|---|---|---|---|
| Registry lookup（兩個）| 2.25 ns | 10.3 ns | 4.6× |
| **「其餘」trampoline** | **19.8 ns** | **913 ns** | **46×** |
| Total | 22.0 ns | 923 ns | 42× |

「其餘」候選：
- HookStorage::invoke（atomic load + `std::function` call）
- 真正 hooked function call（透過 JIT trampoline 跳回原函式）
- Google Benchmark thread harness（不太可能，BM_RawCall_Contended 已校正）
- MSVC `_Init_thread_header_safe`（Meyers singleton guard，**已 cache 為 `static HookContext* const ctx`**，只省了 10%）

由於 Step 5 microbench 證實 `std::function` 開銷只有 1.5 ns，**它不是元兇**。
真正的 bouncer 需要 VTune / perf 才能定位，**列為 Step 6.x follow-up ticket**，
不在當前 FR-010 範圍。

---

## 四、得到了什麼、失去了什麼、學到了什麼

### 得到了
- **單線程 hot path：41.1 → 22.0 ns/call（−46%）**
- **Registry 層 8t/1t ratio：~50× → 3.9–5.5×（−92%）**
- 多執行緒 8t 端對端：1568 → 923 ns/call（−41%）
- 為 RCU 設下完整 escape hatch（`SPLICE_REGISTRY_IMPL`、`SPLICE_MAX_HOOKS`、`SPLICE_RCU_GRACE_PERIOD_MS`）
- 確立工程原則：「有 trade-off 的優化必須附 escape hatch」
- 確立工程紀律：「對 hot path 出手前先寫 microbench 驗證假設」

### 失去了（沒達標）
- 1t < 20 ns gate：差 2 ns
- 8t/1t < 5× 端對端 gate：差很多，需要找出殘留 bouncer

### 學到了
1. **「教科書估算」不是 benchmark**。我給 `std::function` 估 ~4 ns，實測 1.5 ns。
   差距來自 SBO、devirt、BTB 三個現代編譯器/CPU 機制的疊加效應。
2. **每個重構動工前都該有一份「我預估這個動作會省多少 ns」的數字**。預估錯了
   就停手，不要硬幹。
3. **Splice 的真正 hot path bottleneck 是 reader contention，不是 dispatch 本身**。
   這跟 the predecessor framework v1 的直覺相反（v1 嘗試攻擊 dispatch，沒攻擊 contention）。
4. **RCU 的設計成本主要在 memory reclamation，不在發布機制**。time-deferred
   reclamation 證明：**在 reader 是 ns-μs 等級的場景**，標準 EBR 是 over-engineering。

---

## 五、給後人的話

如果你在 Splice 後續工作中想優化熱路徑：

1. **先量測，再動手**。`benchmark/` 目錄底下已有完整的 microbench 工具鏈，
   再加一個只是改 200 行內。
2. **`std::function` 不是頭號嫌疑犯**。如果你重新動 Step 5，把證據放在這份報告
   的對照欄上。
3. **8t/1t = 42× 的殘留 bouncer 還沒人解**。需要 VTune / perf record。在你有
   時間 + 工具的場合再來。
4. **任何 RCU 改動都得保有 escape hatch**。`SPLICE_REGISTRY_IMPL` 是模板。
5. **Microbench 是寫給未來的自己看的**。把組件隔離、命名清楚、留可重跑的指令。

---

## 變更紀錄

- 2026-05-27：初版。統合 Step 3.5 / 6.1–6.4 + Step 5 證據鏈。
