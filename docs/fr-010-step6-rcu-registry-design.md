# FR-010 Step 6 — RCU Registry 設計與 escape hatch 評估

**狀態：** 設計提案 / 待 Allen 拍板實作
**對應 FR：** FR-010 Step 6（true RCU registry — close 8t/1t < 5× gate）
**撰寫日期：** 2026-05-19
**作者：** Claude + Allen
**搭配讀物：**
- [`template-callback-evaluation.md`](./template-callback-evaluation.md) — Step 5 policy framework 設計
- [`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md) — Step 5 實證 bench
- [`benchmark-baseline.md`](./benchmark-baseline.md) — FR-010 整體 baseline
- [`v2-design-rationale.md`](./v2-design-rationale.md) §"Why not runtime-switchable"

---

## TL;DR

1. **問題**：8 thread reader contention 把 hooked call 從 23.7 ns 拉到 688 ns/call
   （**26.9× degradation**，gate < 5×）。元兇是 `HookContext::m_mutex` 的 reader
   counter cache-line ping-pong。
2. **解法**：把 `m_hooks` 從 `unordered_map + shared_mutex` 換成 RCU snapshot —
   reader 無鎖，writer 原子發布新 snapshot。
3. **取捨原則**（Allen 確立）：**任何 RCU 改動必須有 escape hatch**。RCU 本質是
   functional trade-off（寫者多耗、舊資料延遲回收、記憶體峰值高），不該強加於
   所有使用者。
4. **escape hatch 設計**：build-time macro `SPLICE_REGISTRY_IMPL`，平行 Step 3
   的 `SPLICE_DEFAULT_POLICY`。**預設保持現況**（`shared_mutex_map`），opt-in
   切到 `rcu_atomic_array`。零 API 變動。
5. **預期收益**：8-thread hooked **688 → ~50-80 ns/call**（gate < 5× 進入射程）。

---

## 為什麼要做 Step 6

Step 5 的 microbench 證明 `std::function → thunk` 只能省 ~1 ns 端對端。**Splice
的真正 bottleneck 不在 callback dispatch，而在 registry 查表的 reader 競爭。**

### 8t/1t = 26.9× 的證據

來自 `docs/benchmark-baseline.md`（FR-010 Step 3.5 後）：

| 量測（Ryzen 9 9950X3D） | 數值 |
|---|---|
| 1-thread hooked call | 23.7 ns/call |
| 8-thread hooked call | 688 ns/call |
| **8t/1t ratio** | **26.9×** |
| FR-010 gate | < 5× |

`shared_mutex` 看似可以「無鎖」讀取，實際上 reader 仍需要對 `m_reader_count`
做 atomic increment/decrement，在 8 個 thread 同時做這件事時，這個 cache line 在
8 個 core 的 L1 之間 ping-pong，每次 RMW 都引發 MOESI 協議的 invalidate 流量。
**這就是 reader 競爭的本質**。

### RCU 為什麼能解

RCU（Read-Copy-Update）的核心承諾：**reader 在 hot path 上完全無原子 RMW，只
讀取 immutable snapshot**。

```cpp
// shared_mutex 版（現況）
Hook& get_hook(int id) {
    std::shared_lock lock(m_mutex);          // ← atomic RMW（cache line bounce）
    return *m_hooks[id];
}

// RCU 版（提案）
Hook& get_hook(int id) {
    auto* snap = m_snapshot.load(std::memory_order_acquire);  // ← pure load
    return *snap->lookup(id);                                  //    no atomic RMW
}
```

Reader 只做純 load，零 cache line bouncing。8 個 core 各自的 L1 都能 cache 同一份
snapshot 唯讀，MOESI 走 S 狀態，零 invalidate 流量。

---

## 取捨原則 — 為什麼必須有 escape hatch

> 「RCU 優化 應該算是一種功能取捨。所以必須要有開關來控制。」
> — Allen，2026-05-19

RCU 的成本不是零，只是把成本從 reader 挪到 writer + 記憶體：

| 軸線 | shared_mutex（現況） | RCU registry |
|---|---|---|
| Reader 延遲 | 中等（shared_lock RMW）| **極低（pure load）** |
| Writer 延遲 | 中等（unique_lock）| **高（copy snapshot + atomic publish + 等 grace period）** |
| 記憶體峰值 | 1× registry size | **2× 短暫**（舊 snapshot 等 grace period 才回收）|
| 寫入可見度 | 立即（鎖釋放即可見）| **延遲**（reader 可能看到舊 snapshot 數百 ns） |
| 失敗模式 | 死鎖（已知防護）| **memory reclaim bug 極難 debug**（use-after-free） |
| AOSP toolchain | 全支援 | **`atomic<shared_ptr>` C++20，舊 NDK 部分支援** |

對只有 1-2 thread 的桌面工具或單線程遊戲，shared_mutex 已經夠用——上面那 26.9×
degradation 只發生在重度多執行緒場景。**強迫所有人吃 RCU 的記憶體 + writer 延遲
成本是不對的**。

這跟 Step 3 確立的「policy framework」精神完全一致：
- Step 3 對 **callback storage** 提供 `rcu_writeonce` / `shared_mutex` 選擇
- Step 6 對 **registry 整體** 提供 `rcu_atomic_array` / `shared_mutex_map` 選擇

兩層 escape hatch 互相獨立、互相疊加。

---

## escape hatch 設計

### Build-time 開關

新增一個 macro，**模仿 `SPLICE_DEFAULT_POLICY` 的設計**：

```cpp
// include/splice/registry_impl.h（新增）
namespace splice::registry {

// Registry implementation tag — selects how HookContext stores m_hooks.
//
// shared_mutex_map (default)  : unordered_map<int, shared_ptr<HookBase>>
//                               guarded by shared_mutex. Predictable, well-
//                               tested, AOSP-toolchain safe. Reader pays
//                               atomic RMW on the shared_mutex counter
//                               (cache-line bouncing under contention).
//
// rcu_atomic_array            : atomic<Snapshot*> publish-and-forget. Reader
//                               does a single std::memory_order_acquire load;
//                               no atomic RMW on the hot path. Writer copies
//                               the snapshot, publishes new pointer, and
//                               defers old-snapshot reclamation by one
//                               grace period.
//                               Trade-offs: higher writer latency, ~2×
//                               memory peak during grace period, requires
//                               C++20 std::atomic<shared_ptr> or hand-rolled
//                               hazard-pointer scheme on older toolchains.
struct shared_mutex_map {};
struct rcu_atomic_array {};

} // namespace splice::registry

// Build-time selection. Override per project:
//   add_compile_definitions(SPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array)
//
// Default stays shared_mutex_map — opt-in only, **no surprises for existing users**.
#ifndef SPLICE_REGISTRY_IMPL
#   define SPLICE_REGISTRY_IMPL ::splice::registry::shared_mutex_map
#endif
```

### 為什麼 build-time 而非 runtime？

跟 Step 3 一樣的紀律：

| 切換時機 | 成本 | 適不適合 |
|---|---|---|
| Build-time | 0 ns runtime（template specialization）| ✅ |
| Runtime | 每次 lookup +1 branch + 雙倉儲 | ❌ 違反 hot path 紀律 |

兩種 registry 的 **data layout 完全不同**（`unordered_map` vs `atomic<Snapshot*>`），
runtime 切換要存兩份、走 branch。Splice 的 hot path 紀律不允許這個成本。

### 兩種 registry 的 partial spec 對應

`HookContext` 內部把 `m_hooks` 抽到 template helper：

```cpp
// include/splice/context.h（改動）
template <typename Impl>
class HookRegistry;   // primary undefined

// shared_mutex_map — 現況實作（不動）
template <>
class HookRegistry<registry::shared_mutex_map> {
    std::shared_mutex                            m_mutex;
    std::unordered_map<int, std::shared_ptr<HookBase>> m_hooks;
public:
    template <typename HookT>
    HookT& get_or_create(int id) {
        { std::shared_lock l(m_mutex);
          if (auto it = m_hooks.find(id); it != m_hooks.end())
              return *static_cast<HookT*>(it->second.get());
        }
        std::unique_lock l(m_mutex);
        auto& slot = m_hooks[id];
        if (!slot) slot = std::make_shared<HookT>();
        return *static_cast<HookT*>(slot.get());
    }
    void clear() { std::unique_lock l(m_mutex); m_hooks.clear(); }
};

// rcu_atomic_array — Step 6 新實作（opt-in）
template <>
class HookRegistry<registry::rcu_atomic_array> {
    struct Snapshot {
        // Sparse array, indexed by id directly. Splice ids are __COUNTER__-
        // derived so they're dense from 0. Cap matches HookContext's
        // installer-slot bound.
        std::array<std::shared_ptr<HookBase>, SPLICE_MAX_HOOKS> slots;
    };
    std::atomic<Snapshot*> m_snapshot{nullptr};
    std::mutex             m_write_mutex;   // serialises writers only
public:
    template <typename HookT>
    HookT& get_or_create(int id) {
        // Hot path: pure load, no RMW.
        auto* snap = m_snapshot.load(std::memory_order_acquire);
        if (snap && id < (int)snap->slots.size() && snap->slots[id]) {
            return *static_cast<HookT*>(snap->slots[id].get());
        }
        return install_slot<HookT>(id);   // cold path
    }
private:
    template <typename HookT>
    HookT& install_slot(int id) {
        std::lock_guard l(m_write_mutex);
        auto* old = m_snapshot.load(std::memory_order_acquire);
        auto  next = std::make_unique<Snapshot>();
        if (old) next->slots = old->slots;
        if (!next->slots[id]) next->slots[id] = std::make_shared<HookT>();
        auto* raw = next.release();
        m_snapshot.store(raw, std::memory_order_release);
        defer_reclaim(old);                // see "memory reclamation"
        return *static_cast<HookT*>(raw->slots[id].get());
    }
};
```

`HookContext` 持有 `HookRegistry<SPLICE_REGISTRY_IMPL>`。Template 二選一在編譯期
決定，**runtime 完全零成本**。

---

## 使用者 API 完全不變

### 從使用者視角看

```cpp
// 不管用哪種 registry implementation，使用者寫的 code 一模一樣
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
        return orig(d, s);
    });
splice::install_all();
```

切換只在 build 階段：

```bash
# 預設（現況）
cmake --preset=windows-x64-release

# Opt-in RCU registry（高並行 hook 場景，例如 60fps × 8 thread）
cmake --preset=windows-x64-release \
      -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array
```

或在 consumer 的 CMakeLists.txt：

```cmake
add_compile_definitions(SPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array)
```

---

## RCU 的硬問題 — Memory Reclamation

RCU 最危險的部分**不是讀寫**，而是**回收舊 snapshot 的時機**。

### 問題本質

```
Thread A: snap = m_snapshot.load();        // 拿到 snap = 0x1000
Thread B: m_snapshot.store(0x2000);        // 發布新 snapshot
Thread B: delete (Snapshot*)0x1000;        // ❌ Thread A 還在用！UAF
```

Reader 拿到 snapshot 指標後、解參考前，writer 不可以釋放它。**怎麼知道何時所有
reader 都離開了？**

### 三個解法的取捨

| 方案 | 機制 | 優點 | 缺點 | 適合 Splice？ |
|---|---|---|---|---|
| **`atomic<shared_ptr>`** | refcount 自動回收 | 簡單、不會 UAF | reader 每次 load 都做 refcount RMW（**回到 shared_mutex 等級的 cache bounce**）| ❌ 失去 RCU 優勢 |
| **Hazard pointers** | reader 註冊「我在用」、writer 等所有 hazard 清空才回收 | reader 純 load + 註冊一次（thread-local）| 實作複雜、需要 thread-local register | ⚠️ 可行但工程量大 |
| **Epoch-based reclamation (EBR)** | 全域 epoch counter，reader 標 epoch 進入、writer 等 epoch+2 才回收 | reader 純 load + 標 epoch、實作中等 | 需要 epoch 廣播、長存活 reader 會延長 grace period | ✅ **推薦** |
| **Quiescent-state reclamation (QSBR)** | 假設 reader 有「沒在 critical section」的時點 | 最快 | 需要程式碼結構配合（Linux kernel 風格）| ❌ 不適合 hook library |

### Splice 的選擇：EBR with 2-epoch retire queue

```cpp
// 偽碼 — 完整實作會寫在 src/context_rcu.cpp
struct EBR {
    static thread_local std::atomic<int> tl_epoch;   // 0 = quiescent, 1/2 = in CS
    static std::atomic<int> global_epoch;            // alternates 1 ↔ 2
    static std::vector<Snapshot*> retire_q[3];       // indexed by epoch

    static void reader_enter() {
        tl_epoch.store(global_epoch.load(std::memory_order_acquire),
                       std::memory_order_release);
    }
    static void reader_exit() {
        tl_epoch.store(0, std::memory_order_release);
    }
    static void defer_reclaim(Snapshot* old) {
        int e = global_epoch.load();
        retire_q[e].push_back(old);
        try_advance_epoch();
    }
    // ... try_advance_epoch scans all tl_epoch, advances + drains old queue
};
```

實作複雜度大概 200 行 C++，但這是 RCU 的核心難點，沒有捷徑。**EBR 加上一個 dev-build
sanitizer hook 來 catch reader 沒 enter/exit 的 bug**，可以把風險控制住。

---

## 為什麼選 array、不選 hash map？

`unordered_map` 在 RCU 下要 copy 整個 bucket 結構，O(n) writer 成本。
**Splice 的 hook id 是 `__COUNTER__` 衍生的，本來就是密集從 0 起算的整數**——
直接用 `std::array<shared_ptr, MAX>` 索引，O(1) lookup、O(MAX) writer copy
（MAX 大約 256，可調）。

```cpp
// SPLICE_MAX_HOOKS — build-time bound (decided 2026-05-19)
// Default 512 covers AOSP system-service hooking (300-800 typical) while
// keeping the snapshot at 8 KiB — well under L1d (32-48 KiB) so reader
// lookup stays at L1-hit speed (~1 ns). Bump to 4096 for extreme cases
// (full syscall tracing) but accept the L2-hit penalty.
#ifndef SPLICE_MAX_HOOKS
#   define SPLICE_MAX_HOOKS 512
#endif
```

**512 個 hook slot 對應 512 × 16 bytes = 8 KiB snapshot**，完全裝在 L1d
（32-48 KiB），reader 的 indexed lookup 就是一次 L1 hit (~1 ns)。預設 512 的
理由（Allen 2026-05-19 拍板）：

| MAX | snapshot 大小 | L1d fit | Reader 成本 | 覆蓋場景 |
|---|---|---|---|---|
| 256 | 4 KiB | ✅ | ~1 ns | 遊戲增強（10-50 hook）|
| **512（預設）** | **8 KiB** | ✅ | **~1 ns** | **AOSP 系統服務（300-800 hook）**|
| 1024 | 16 KiB | ✅ 緊 | ~1 ns | 完整 API trace |
| 4096 | 64 KiB | ❌ → L2 | **~5 ns（退化 5×）**| 全 syscall hook |

> 超過 1024 會把 snapshot 推出 L1d，reader 退化到 L2 速度。那種規模請考慮
> hash-based fallback registry（未來工作，目前不在 v1.0 範圍）。

---

## 預期效能

### 1-thread

| 路徑 | shared_mutex_map | rcu_atomic_array |
|---|---|---|
| HookContext::get_hook | ~10 ns（shared_lock RMW + map find）| **~2 ns（atomic load + array index）**|
| 其餘 hot path | ~14 ns | ~14 ns |
| **端對端** | **23.7 ns** | **~16 ns（清 < 20 ns gate）**|

### 8-thread

| 路徑 | shared_mutex_map | rcu_atomic_array |
|---|---|---|
| HookContext::get_hook | ~80 ns × 8 因子 contention = ~640 ns | **~2 ns（reader 完全不爭用）**|
| 其餘 hot path | ~50 ns | ~50 ns |
| **端對端** | **688 ns** | **~50-80 ns** |
| **8t/1t ratio** | **26.9×** | **~3-5×（清 < 5× gate）**|

### Writer 成本

| 操作 | shared_mutex_map | rcu_atomic_array |
|---|---|---|
| onInvoke (首次安裝) | ~50 ns（unique_lock + map insert）| **~500 ns**（copy 4KiB snapshot + atomic publish）|
| 切換 callback runtime（罕見）| ~50 ns | ~500 ns + grace period（~100 µs）|

Writer 慢 10× — 但 **callback 安裝是 cold path（每個 hook 一輩子一次）**，這個
慢一點完全可以接受。

### 記憶體

| 狀態 | shared_mutex_map | rcu_atomic_array |
|---|---|---|
| 穩態 | 1× snapshot ~ 4 KiB | 1× snapshot ~ 4 KiB |
| 寫入瞬間（grace period 內）| 1× | **~2× ~ 8 KiB**（短暫） |

對嵌入式或極端記憶體敏感場景，這個雙倍峰值要評估。Splice 主要部署目標是
Android 手機（GB 級 RAM），4 KiB 浮動完全可忽略。

---

## 風險與緩解

### Risk 1：AOSP toolchain 對 `atomic<shared_ptr>` 支援不均

`std::atomic<std::shared_ptr<T>>` 是 C++20 feature，**libc++ 直到 LLVM 17 才支援**。
Android NDK r29 用 LLVM 19（我們的構建環境），但**第三方 AOSP 整合的 toolchain 可能更舊**。

**緩解**：採 EBR + raw pointer（上面的設計），**完全繞過 `atomic<shared_ptr>`**。
只用 `std::atomic<Snapshot*>` + 手動 EBR 回收，C++17 即可編譯。

### Risk 2：EBR 實作 bug 可能造成 use-after-free

EBR 的核心是「所有 reader 都離開後才回收」。如果某個 reader 忘記 exit（早退、
exception 拋出沒走 RAII），會延長 grace period 但**不會 UAF**。如果回收邏輯
有 bug（advance epoch 太早），則 UAF。

**緩解**：
1. EBR Guard 用 RAII 包裝（reader 進入 / 離開強制成對）
2. Debug build 加 `SPLICE_RCU_VERIFY` flag：每次 reclaim 前掃所有 thread_local
   epoch，confirm 全部過期才釋放（即使生產 build 因為效能原因略過）
3. ASAN + TSan 雙 sanitizer CI run（已有基礎建設）

### Risk 3：Snapshot copy 在 hook 多時變昂貴

若 SPLICE_MAX_HOOKS 拉到 4096，每次 install 都 copy 64 KiB snapshot，writer
變很慢。但這是 **cold path**，不影響 hot path。

**緩解**：SPLICE_MAX_HOOKS 預設 256；若使用者真的需要更多，build-time 改即可。

### Risk 4：兩個 registry 都要維護

Code base 多了一個實作分支。

**緩解**：
- 兩個 registry 用同一個 unit test 跑（template helper），不會 drift
- benchmark 同時測兩條，CI gate 兩條都不能迴歸
- 文件記明：**RCU registry 是 opt-in 加速，預設不變**

### Risk 5：使用者開啟 RCU 後遇到問題不知道原因

**緩解**：
- 啟動時 `SPLICE_LOGI("registry=rcu_atomic_array")` 記入 log
- `splice::diagnostics()` API 回傳目前的 registry impl 名稱
- 文件 prominently 標示 trade-off

---

## 實作計畫

依序執行，每步都有可驗證的 gate：

| Step | 工作 | Gate |
|---|---|---|
| 6.1 | 新增 `include/splice/registry_impl.h` + `SPLICE_REGISTRY_IMPL` macro。`HookRegistry<shared_mutex_map>` 把現有 `m_hooks` 邏輯封裝進去。**這步零行為變動**。 | 既有 105/105 + 94/94 全綠 |
| 6.2 | 實作 `HookRegistry<rcu_atomic_array>`，不含真正 EBR（先用 simple-leak — 永不回收舊 snapshot，跟 rcu_writeonce 策略一致）。 | 新增 RCU registry 單元測試 4 個（基本 lookup + 並行 reader） |
| 6.3 | 跑 contended bench，確認 8t/1t 數字下降到 < 5×。 | 8-thread bench 顯示 < 100 ns/call |
| 6.4 | 實作 EBR 真正回收。 | TSan + 1M-iteration stress 0 race |
| 6.5 | CI 整合：兩個 registry impl 都跑 unit + bench，gate 都不能 regress。 | CI green |
| 6.6 | 文件補完（包含 README + CLAUDE.md progress 條目）。 | — |

---

## 決議要點

請決定：

1. **整體方向**：要不要做 Step 6（含 RCU registry + escape hatch）？
2. **escape hatch 機制**：採 build-time macro `SPLICE_REGISTRY_IMPL`（**強烈推薦**）？
3. **記憶體回收**：採 EBR（推薦，無 toolchain 依賴）？或 simple-leak（先 ship，
   後續再補回收）？
4. ~~**SPLICE_MAX_HOOKS 預設值**~~ — **已決議 2026-05-19：512**
   （AOSP 系統服務涵蓋、L1d-friendly、reader 不退化）

---

## 附錄 A：和 Step 5 的關係

Step 5（callback storage thunk）與 Step 6（registry RCU）**正交**：

| | callback storage | registry |
|---|---|---|
| **影響範圍** | 每個 hook 的 invoke path | 所有 hook 的 lookup path |
| **省時關鍵** | dispatch overhead | reader contention |
| **典型省時** | ~1 ns/call | ~10 ns/call (1t), ~600 ns/call (8t) |
| **escape hatch** | `SPLICE_DEFAULT_POLICY` + `SPLICE_HOOK_AS` | `SPLICE_REGISTRY_IMPL` |
| **互動** | 完全獨立 | — |

兩者收益可疊加。但 **Step 6 收益遠大於 Step 5**（10× to 100×），優先級高。

## 附錄 B：為什麼這份文件值得留下

這份文件不只是「我們要做什麼」，更是「**為什麼預設不開、誰可以開、開了會付什麼代價**」
的完整紀錄。

未來碰到的所有情境都能從這找到答案：
- 「為什麼預設不用 RCU？」→ functional trade-off，writer 慢 10×、記憶體 2×、AOSP 風險
- 「想用 RCU 怎麼開？」→ `-DSPLICE_REGISTRY_IMPL=...`
- 「開了 RCU 為什麼還是慢？」→ 大概是 reader 沒走 EBR Guard、或熱點不在 registry
- 「為什麼不做 runtime 開關？」→ 違反 hot path 紀律，與 Step 3 policy framework 紀律一致

也固化了一條 **Splice 工程原則**：
> 凡是有「功能 trade-off」性質的優化（不只是純粹的「更快更好」），**都必須提供
> escape hatch**。預設值偏向「驚喜最少」，opt-in 留給知情使用者。

這條原則已在三個地方體現：
1. Step 3：`SPLICE_DEFAULT_POLICY` + `SPLICE_HOOK_AS`（callback storage）
2. Step 6：`SPLICE_REGISTRY_IMPL`（registry）
3. ScopedHook 將 Tier 2 disable 列為「best-effort，trampoline 永久 leak」也是
   trade-off 化身（FR-013）。

---

## 變更紀錄

- 2026-05-19：初版。完整設計 + escape hatch + EBR 策略 + 實作計畫，待 Allen 拍板。
