# Splice vs the predecessor framework — Quick Comparison

**作者：** Allen Kuo
**日期：** 2026-05-29
**深入版本：** [`v2-design-rationale.md`](./v2-design-rationale.md)
**績效實證：** [`fr-010-performance-summary.md`](./fr-010-performance-summary.md)

---

## 一句話

Splice 是 the predecessor framework 的 **產品化升級** — 保留 friendly fluent API、攻擊
hot-path 同步機制、補完 disable 語意、加上跨平台支援。

---

## 對照表

| 軸線 | the predecessor framework（原版）| Splice v2（現在）|
|---|---|---|
| **目標平台** | Android ARM64 only | ARM64 + x86_64，Android / Linux / Windows |
| **API 風格** | `the legacy hook macro(func).onInvoke([](orig, ...){...})` | `SPLICE_HOOK_ADDR(func).onInvoke([](orig, ...){...})` — **一字不差保留** |
| **registry map** | `std::map<int, ...>`（O(log n)）| `std::unordered_map<int, ...>`（O(1) 平均）|
| **registry 同步** | `recursive_mutex`（全序列化）| `shared_mutex` 預設、可 opt-in **RCU snapshot**（pure-load reader）|
| **callback storage** | `std::function` | 同（實測 std::function ~1.5 ns 不是瓶頸，[Step 5 報告](./fr-010-step5-microbench-report.md)）|
| **installer queue 生命週期** | `static vector` 永不清空 → 測試漏抓 dangling lambda | **InstallerToken RAII**，O(1) 釋放（Task #57）|
| **可中斷？** | 沒有 disable API | **Tier 1 / Tier 2 disable**（GOT/IAT pointer-swap + inline atomic restore）|
| **policy 切換** | 寫死 | **build-time `SPLICE_DEFAULT_POLICY`** + **per-call-site `SPLICE_HOOK_AS`** |
| **registry 實作切換** | 寫死 | **build-time `SPLICE_REGISTRY_IMPL`**（shared_mutex_map 預設 / rcu_atomic_array opt-in）|
| **ID system** | per-TU `__COUNTER__`（**跨 TU 撞 id 是潛伏 bug**）| `SPLICE_UNIQUE_ID = __LINE__ << 16 \| __COUNTER__` + runtime `slot_for(trampoline_ptr)`（全域唯一）|
| **trampoline 生成** | 7 個 explicit specialisation（0/1/2/3/4/5/10 args）| 1 個 variadic template，cover 所有 arity |
| **log 機制** | `__android_log_print` 直接呼叫 | `platform_log.h` 跨平台 substrate + hot-path throttle（`LOGD_EVERY_N`, `LOGV_ONCE`）|
| **可測試性** | 全域 singleton，測試無法 reset | `HookContext` 可獨立實例化 + `reset()` 支援 |
| **單線程效能** | 沒實測 | **22.0 ns/call**（從 41.1 改善到 22.0，−46%）|
| **8 thread reader** | 沒實測 | registry 隔離 ratio **3.9–5.5×**（從 ~50× 改善，−92%）|
| **CI / sanitizer** | 無 | ASan + UBSan + TSan、benchmark 迴歸 gate |

---

## 不變的部分（你的設計選擇被保留）

1. **friendly fluent API 的「signature 一刻」** — `.onInvoke(lambda)` 寫法、
   chainable builder、`auto orig` 推導，全部不動。
2. **`std::function` 寫 callback** — 實測證明不是瓶頸，沒必要換 thunk
   ([Step 5 報告](./fr-010-step5-microbench-report.md))。
3. **per-call-site trampoline** — 每個 hook 點一個 static function，避開 JIT thunk。
4. **C macro logging** — 用 `platform_log.h`，不引入 C++ log library。

---

## 為什麼這些改動值得做

1. **真實多執行緒場景的成本** — the predecessor framework 在 4-8 個 game render thread
   並行 hook 同函式時，`recursive_mutex` 會把所有 reader 序列化，造成 ~50× 的
   per-call 延遲爆炸。Splice 的 shared_mutex / RCU 路徑把這個拉回 3-9×。
2. **長壽行程的安全性** — 沒 disable API 就只能假設「hook 跟程式同壽」。Splice
   的 Tier 1/2 disable 讓 Android 系統服務這類「需要動態啟停 hook 」的場景變
   可行。
3. **跨平台** — 原本只能在 ARM64 Android 跑。Splice 加 x86_64 後，工具鏈本身
   可在 Windows 開發環境直接 bench + debug，不用每次都 adb push。
4. **可測試** — 全套 unit test + integration test + microbench，回歸有
   safety net；之前改 the predecessor framework 都是「祈禱 + 真機跑」。
5. **工程紀律固化** — escape hatch 原則、「優化前先 microbench 驗證」這些
   都寫進 docs，避免重複 v1 的猜測式優化。

---

## 不是替代品的部分

Splice 仍 **依賴** the predecessor framework 體系的核心 idea：
- 編譯期 trampoline 生成（`__COUNTER__` → 唯一 static function）
- C-style hooking by symbol name + lib
- ARM64 disasm logic（**逐字 port 過來**，沒重寫）

如果你的 production code 還在用 the predecessor framework，**沒必要急著換** — v1 在
單執行緒場景的效能完全夠用。Splice 是攻擊 v1 的特定痛點（多線程競爭、disable、
跨平台），不是「全面取代」。

---

## 更多閱讀

- [`v2-design-rationale.md`](./v2-design-rationale.md) — 深入設計取捨
- [`fr-010-performance-summary.md`](./fr-010-performance-summary.md) — 完整效能旅程
- [`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md) — RCU registry 設計
- [`CLAUDE.md`](../CLAUDE.md) §"Current Progress" — 最新狀態
