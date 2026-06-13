# FR-010 Step 5 — `std::function` vs Thunk callback storage 實證 microbench 報告

**狀態：** 證據已備齊 / 待 Allen 決議是否執行 Step 5a
**對應 FR：** FR-010 Step 5（Template Callback — kill `std::function` on hot path）
**測試日期：** 2026-05-19
**作者：** Claude + Allen
**搭配讀物：** [`template-callback-evaluation.md`](./template-callback-evaluation.md) — 設計評估、API 影響、取捨表

---

## TL;DR — 給沒空看完的你

**Thunk 比 `std::function` 在 hot path 上節省 0.9–1.2 ns/call（−23% 至 −27%）。
但 Splice 端對端是 23.7 ns，這 1 ns 不足以單獨清掉 FR-010 的 < 20 ns gate。**

| 平台 | `std::function` 開銷 | Thunk 開銷 | 節省 | 端對端推估 |
|---|---|---|---|---|
| Windows x86_64 (Ryzen 9 9950X3D, MSVC /O2) | +1.39 ns | +0.49 ns | **0.90 ns** (−23%) | 23.7 → ~22.8 ns |
| Android ARM64 (Snapdragon 8 Gen 3, Clang -O2) | +1.52 ns | +0.31 ns | **1.21 ns** (−27%) | 待端對端 baseline |

**意外發現**：我原本估 std::function 開銷 ~4-5 ns（vtable hop + state pointer），
實測 1.4–1.5 ns。現代編譯器（MSVC 19.44 / Android Clang 19）對 std::function
的 SBO + devirtualization 已經做得相當積極。

**建議行動：先看「決議」一節**。

---

## 為何要先 benchmark？

Allen 在看完 [`template-callback-evaluation.md`](./template-callback-evaluation.md)
後提出的關鍵質疑：

> 「但是我擔心的效能是否真如你說的那樣。我們是否應該先做個小 bench
> 在 PC 與 Android 實現這個驗證。然後有數據為基礎再來展開這個改動。」

這是正確的工程紀律。一個觸及 hot path、會牽動 100+ 個 hook call site 編譯時間
的重構，**沒有實證數據不該動手**。Step 5 的「預估 −4 ns、清 < 20 ns gate」
推論建立在我對 `std::function` 開銷的 **估算**，而不是 **量測**。

這份報告把「估算」換成「量測」。

---

## 實驗設計

### 為什麼不直接改 HookStorage 然後 bench？

因為那會：
1. 改動 200+ 行 hot-path 程式碼
2. 必須通過全套單元測試
3. 必須跨 Windows/ARM64 雙平台驗證
4. 一旦數字不好看，要回滾全部改動

**先寫獨立 microbench 隔離出 dispatch overhead**，0 行 Splice 內部變動。

### 三條測量路徑

獨立 microbench 在 `benchmark/bench_callback_storage.cpp`，**不引用 Splice 任何東西**。
直接在 microbench 內定義三個本地 storage struct，鏡像 Splice 的 hot path 結構：

#### 路徑 1：`BM_DirectCall`（floor）

```cpp
for (auto _ : state) {
    x = splice::bench::hot_target(x) + 1;   // no indirection at all
    benchmark::DoNotOptimize(x);
}
```

目的：建立量測下限。`hot_target` 是 noinline 的 `int hot_target(int x)`
（位於獨立 TU，編譯器無法 inline 穿透）。

#### 路徑 2：`BM_StdFunctionPath`（現況鏡像）

```cpp
struct StdFunctionStorage {
    using HookFn = std::function<int(OrigFn, int)>;
    std::atomic<HookFn*> m_fn{nullptr};

    int invoke(OrigFn orig, int x) noexcept {
        if (auto* fn = m_fn.load(std::memory_order_acquire)) {
            return (*fn)(orig, x);          // std::function manager vtable hop
        }
        return orig(x);
    }
};
```

完全鏡像 `include/splice/context.h:128` 的 `HookStorage<rcu_writeonce>` —
`std::atomic<std::function*>` load + `operator()` 調用。

#### 路徑 3：`BM_ThunkPath`（Step 5 提案）

```cpp
struct ThunkStorage {
    using Thunk = int (*)(void* state, OrigFn orig, int x);

    std::atomic<Thunk> m_thunk{nullptr};
    std::atomic<void*> m_state{nullptr};

    template <typename Lambda>
    void store(Lambda fn) {
        static Lambda* s_fn = new Lambda(std::move(fn));
        m_state.store(s_fn, std::memory_order_relaxed);
        m_thunk.store(
            +[](void* st, OrigFn orig, int x) noexcept -> int {
                return (*static_cast<Lambda*>(st))(orig, x);
            },
            std::memory_order_release);
    }

    int invoke(OrigFn orig, int x) noexcept {
        if (auto th = m_thunk.load(std::memory_order_acquire)) {
            return th(m_state.load(std::memory_order_relaxed), orig, x);
        }
        return orig(x);
    }
};
```

熱路徑差異：
- `std::function` 路徑：load HookFn\* → vtable 查 `operator()` → call manager → call lambda
- Thunk 路徑：load thunk function pointer → 一次間接呼叫 → lambda inline 在 thunk 內

### 公平性控制

- ✅ 三條路徑共用同一個 callback body：`[](OrigFn orig, int x){ return orig(x) + 1; }`
- ✅ 同一個 `splice::bench::hot_target`（noinline，獨立 TU）
- ✅ 同一個 `benchmark::DoNotOptimize(x)` 防 dead-code elimination
- ✅ 同一個 Release build / -O2
- ✅ Google Benchmark `--benchmark_repetitions=5` + `--benchmark_report_aggregates_only=true`

---

## 量測結果

### 平台 1：Windows x86_64

**硬體**：AMD Ryzen 9 9950X3D（4.3 GHz, 32 thread, 96 MiB L3）
**OS**：Windows 11 Pro 26200
**編譯器**：MSVC 19.44.35211 (cl.exe 14.44.35211)
**Build preset**：`windows-x64-bench`（Release, /O2, `-DNDEBUG`）
**Google Benchmark**：v1.9.5
**執行**：`out/build/windows-x64-bench/benchmark/bench_callback_storage.exe --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

```
--------------------------------------------------------------------
Benchmark                          Time             CPU   Iterations
--------------------------------------------------------------------
BM_DirectCall_mean              2.59 ns         2.60 ns            5
BM_DirectCall_median            2.60 ns         2.61 ns            5
BM_DirectCall_stddev           0.016 ns        0.027 ns            5
BM_DirectCall_cv                0.63 %          1.02 %             5

BM_StdFunctionPath_mean         3.99 ns         3.91 ns            5
BM_StdFunctionPath_median       3.99 ns         3.92 ns            5
BM_StdFunctionPath_stddev      0.009 ns        0.114 ns            5
BM_StdFunctionPath_cv           0.23 %          2.91 %             5

BM_ThunkPath_mean               3.09 ns         3.09 ns            5
BM_ThunkPath_median             3.10 ns         3.08 ns            5
BM_ThunkPath_stddev            0.055 ns        0.069 ns            5
BM_ThunkPath_cv                 1.77 %          2.23 %             5
```

| 路徑 | mean | CV | delta vs Direct |
|---|---|---|---|
| BM_DirectCall | 2.60 ns | 0.63 % | floor |
| BM_StdFunctionPath | 3.99 ns | 0.23 % | **+1.39 ns** |
| BM_ThunkPath | 3.09 ns | 1.77 % | **+0.49 ns** |

**Δ(StdFunction − Thunk) = 0.90 ns/call（−23% dispatch overhead）**

### 平台 2：Android ARM64

**硬體**：Snapdragon 8 Gen 3（Cortex-X4 prime @ 3.3 GHz; bench 跑在背景的 mid 群集
2.27 GHz — CPU scaling 沒鎖定）
**OS**：Android 14 (One UI 6.1)
**編譯器**：Android Clang 19（NDK r29 beta1, target `aarch64-linux-android30`）
**Build preset**：`android-arm64-bench`（Release, -O2, `-DNDEBUG`，新增的 preset）
**Google Benchmark**：v1.9.5（arm64-android triplet）
**部署**：`adb push ... /data/local/tmp/`
**執行**：`adb shell /data/local/tmp/bench_callback_storage --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

```
***WARNING*** CPU scaling is enabled, the benchmark real time measurements may be noisy and will incur extra overhead.
--------------------------------------------------------------------
Benchmark                          Time             CPU   Iterations
--------------------------------------------------------------------
BM_DirectCall_mean              3.04 ns         3.03 ns            5
BM_DirectCall_median            3.04 ns         3.03 ns            5
BM_DirectCall_stddev           0.001 ns        0.001 ns            5
BM_DirectCall_cv                0.03 %          0.02 %             5

BM_StdFunctionPath_mean         4.56 ns         4.55 ns            5
BM_StdFunctionPath_median       4.56 ns         4.55 ns            5
BM_StdFunctionPath_stddev      0.004 ns        0.004 ns            5
BM_StdFunctionPath_cv           0.09 %          0.09 %             5

BM_ThunkPath_mean               3.35 ns         3.34 ns            5
BM_ThunkPath_median             3.34 ns         3.34 ns            5
BM_ThunkPath_stddev            0.002 ns        0.002 ns            5
BM_ThunkPath_cv                 0.06 %          0.06 %             5
```

| 路徑 | mean | CV | delta vs Direct |
|---|---|---|---|
| BM_DirectCall | 3.04 ns | 0.03 % | floor |
| BM_StdFunctionPath | 4.56 ns | 0.09 % | **+1.52 ns** |
| BM_ThunkPath | 3.35 ns | 0.06 % | **+0.31 ns** |

**Δ(StdFunction − Thunk) = 1.21 ns/call（−27% dispatch overhead）**

⚠️ CV 在 ARM64 上極低（< 0.1 %）— 因為手機 CPU 雖然有 scaling 但執行緒被綁在
某一群集，比 PC 多核浮動還穩定。

---

## 分析

### 跨平台一致性

| 量測 | x86_64 | ARM64 | 比例 |
|---|---|---|---|
| Direct call | 2.60 ns | 3.04 ns | ARM64 慢 17 % |
| std::function 開銷 | 1.39 ns | 1.52 ns | ARM64 慢 9 % |
| Thunk 開銷 | 0.49 ns | 0.31 ns | ARM64 **快** 37 % |
| std::function vs Thunk 節省 | 0.90 ns (−23 %) | 1.21 ns (−27 %) | ARM64 略好 |

兩個平台給出 **內部一致** 的故事：
1. std::function 開銷 ~1.5 ns（不是 ~5 ns）
2. Thunk 開銷 ~0.3-0.5 ns（兩個 atomic load + 一次間接呼叫）
3. 兩者差距 ~1 ns

### 為什麼比我原本估的少這麼多？

我的預設模型是 **教科書版 std::function**：
- 虛擬函式呼叫（vtable hop）
- manager pointer 二次間接層
- heap-allocated capture（cache miss）

實際發生：
1. **SBO 起作用**：captureless lambda（測試 case）完全裝得進 std::function 的內建 buffer，沒 heap 分配
2. **MSVC + Clang 都有 devirtualization**：因為 lambda 型別在 `store` 時可見，編譯器在內聯點知道實際 manager 型別
3. **CPU 預測器**：function pointer 間接呼叫被 BTB 預測命中後成本接近直接呼叫

### Thunk 為什麼還是更快？

剩下的 ~1 ns 來自：
- 少一層 manager indirection（thunk 直接 dispatch 到 lambda）
- 更少的 cache footprint（一個 std::function 在 x64 上 64 bytes，thunk + state 16 bytes）
- 編譯器能在 thunk 內 inline lambda body（thunk 是 captureless，body 顯式可見）

### Splice 端對端推估

Windows x86_64 baseline（FR-010 Step 3.5 後）= **23.7 ns/call**

組成估算：
- Trampoline entry / register save: ~2 ns
- Atomic load original pointer: ~1 ns
- HookContext shared_lock + map lookup: ~10 ns（hot path 已用 shared_lock）
- HookStorage::invoke: **~2 ns**（其中 ~1.4 ns 是 std::function dispatch）
- Lambda body: ~1 ns（trivial passthrough）
- Trampoline exit: ~2 ns
- 雜項 (atomic counters, branch mispredicts): ~5 ns

換成 thunk 後：HookStorage::invoke 變 ~1 ns，節省 ~1 ns/call。
**預估端對端：23.7 → ~22.7 ns/call**。

**結論：Step 5a 單獨無法清掉 < 20 ns gate。** 還需要：
- HookContext::get_hook 的快取（thread-local first-call cache）
- 或減少 shared_lock 競爭（atomic snapshot）
- 或 Step 6 的 RCU registry

---

## 決議

把實證證據對應回 `template-callback-evaluation.md` 的決策矩陣：

| ARM64 std::function 開銷 | 原訂建議 | 實測結果 |
|---|---|---|
| > 5 ns | 強烈建議 Step 5a | — |
| 2–5 ns | 做 Step 5a（折衷收益）| — |
| **1–2 ns** | （未原訂） | **實測落在這裡：1.52 ns** |
| < 1 ns | 跳過 Step 5，直接 Step 6 | — |

實測在原矩陣的邊界。重新框定決議：

### 選項 A — 做 Step 5a（建議）

**論據**：
- 雖然單獨收益小（~1 ns / 4%），但 **沒有負面**：
  - API 完全不變（場景 A/B/C 都相容）
  - 編譯時間預估 +5-10%（< 5 秒上限有餘裕）
  - 二進位大小 +50 bytes/site × 50 sites = +2.5 KB（可忽略）
- 把錯誤從 runtime（`bad_function_call`）推到 compile-time（簽章不符直接報錯）
- 把 hot path 從教科書版 `std::function` → 透明 thunk，**debugger 體驗更好**
- 與後續 Step 6 / inline 進階優化 **可疊加**：每個小改動加總，最終清 gate

**成本**：~200 行 hot-path 改動 + 4 個 HookStorage 測試 + 雙平台 bench 驗證。

### 選項 B — 跳過 Step 5，直接 Step 6（也合理）

**論據**：
- 1 ns 的端對端收益在 hooks-per-frame 的場景（60 fps × 100 hooks = 6000 calls/sec）
  累積成 6 µs/sec，**實務上感知不到**
- Step 6（RCU registry）攻擊 8t/1t = 26.9× 的更大問題，收益空間更高
- 縮短交付時程

**成本**：放棄 ~1 ns，承擔 Step 6 設計風險（atomic<shared_ptr> AOSP 工具鏈支援不均）。

### 選項 C — 兩個都做，先 Step 6 再 Step 5a

**論據**：先處理 8t 瓶頸（影響更大），Step 5a 當作 hygiene 改動最後做。

---

## 附錄 A：bench 程式碼位置與重跑指令

### 程式碼

- **microbench**：`benchmark/bench_callback_storage.cpp`
- **共用 noinline target**：`benchmark/bench_targets.h` / `.cpp`（`splice::bench::hot_target`）
- **CMake 整合**：`benchmark/CMakeLists.txt`

### Windows 重跑

```bash
# 在 VS2022 dev shell 中：
cmake --preset=windows-x64-bench
cmake --build --preset=windows-x64-bench --target bench_callback_storage
out/build/windows-x64-bench/benchmark/bench_callback_storage.exe \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

### Android ARM64 重跑

```bash
# 任何 host（NDK 已在 PATH）：
cmake --preset=android-arm64-bench
cmake --build --preset=android-arm64-bench --target bench_callback_storage

# Push + 跑：
adb push out/build/android-arm64-bench/benchmark/bench_callback_storage /data/local/tmp/
adb shell chmod 0755 /data/local/tmp/bench_callback_storage
adb shell /data/local/tmp/bench_callback_storage \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

### 新增的 CMake preset

`android-arm64-bench`（本實驗新增）— Release build, benchmarks ON, `VCPKG_MANIFEST_FEATURES=benchmarks`。
位於 `CMakePresets.json`，繼承 `_android_arm64`。

---

## 附錄 B：為什麼這份報告值得留下

1. **未來 contributor 的真相備忘**：下個改 hot path 的人可以從這份報告知道
   「std::function 開銷在 captureless / SBO-fit lambda 上 ~1.5 ns，不是傳說中
   的 5–10 ns」，避免重複做同樣的估算錯誤。

2. **方法論模板**：「先 microbench 隔離 hypothesis、再決定要不要動 hot path」是
   值得固化的工程紀律。下次 Step 6 / 進階優化也該照辦。

3. **跨平台 baseline 紀錄**：x86_64 / ARM64 兩個平台的 callback-dispatch 絕對
   數字，未來若工具鏈升級（MSVC 19.50 / Clang 20）造成迴歸，可以用這份對照。

4. **決策可追溯**：無論最終選 A/B/C，後人看到的不只是「Splice 為什麼這麼設計」
   的結果，還有「在什麼數據下做的決定」的過程。

---

## 變更紀錄

- 2026-05-19：初版。Windows + ARM64 數據齊備，等待決議。
