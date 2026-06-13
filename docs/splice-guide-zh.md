# Splice — Complete Capability Guide

**目標讀者：** 第一次接觸 Splice、想知道它「能做什麼 / 不能做什麼 / 怎麼用」的人
**位階：** 這份是 Splice 的 **canonical user guide**。深入主題請看：
- 設計取捨：[`v2-design-rationale.md`](./v2-design-rationale.md)
- 對 v1 對照：[`splice-vs-predecessor-summary.md`](./splice-vs-predecessor-summary.md)
- 效能旅程：[`fr-010-performance-summary.md`](./fr-010-performance-summary.md)
- 引擎內幕：[`hooking-internals.md`](./hooking-internals.md)
- 實作路線圖：[`Splice Plan.md`](./Splice%20Plan.md)

---

## 1. 一句話定位

> **Splice 是一個 type-safe / fluent / cross-platform 的 C++20 函式 hooking 函式庫。**
>
> 為 Android ARM64 遊戲增強而生（從 the predecessor framework 產品化），擴展到
> Linux ARM64/x86_64 + Windows x64。目標：寫起來像 DSL、跑起來零滯後、
> 對什麼可行什麼不可行 **誠實**。

---

## 2. 30 秒 demo

```cpp
#include <splice/splice.h>

// 攔截 eglSwapBuffers，每次 frame swap 後做事
SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers)
    .after([](EGLBoolean /*ret*/, EGLDisplay d, EGLSurface s) {
        ++g_frame_count;
    });

// 攔截並改寫返回值
SPLICE_HOOK_ADDR(&my_func)
    .onInvoke([](auto orig, int x) {
        int r = orig(x);
        return r * 2;
    });

// 只在條件成立時攔截，否則 trampoline 直接呼叫原函式（零成本）
SPLICE_HOOK(glDrawArrays)
    .when([]{ return g_capture_enabled.load(); })
    .before([](GLenum, GLint, GLsizei n) { g_total_verts += n; });

splice::install_all();   // 一次 commit 所有 hook
```

---

## 3. 能做什麼 — 能力清單

### 3.1 Hook 安裝策略（自動選最安全的）

| 策略 | 何時用 | 安全性 |
|---|---|---|
| **POINTER_SWAP**（GOT/PLT on ELF, IAT on PE）| 透過 dynamic linker import 的函式 | Tier 1：**原子、可逆**、永久 disable |
| **INLINE patch**（jmp rel32 on x86_64, indirect branch on ARM64）| 直接位址 / 本地函式 / vtable slot | Tier 2：**原子安裝**、**原子 disable**、trampoline 永久 leak |

引擎會先試 POINTER_SWAP，失敗才走 INLINE。

### 3.2 五種 hook 修飾方式（fluent API）

```cpp
// 1. 完全取代原函式 — 你決定要不要呼叫 orig
.onInvoke([](auto orig, Args... args) -> Ret { ... })

// 2. 在原函式 *之前* 跑（不用接 orig）
.before([](Args... args) { ... })

// 3. 在原函式 *之後* 跑（接到 ret + args）
.after([](Ret ret, Args... args) { ... })

// 4. 條件閘 — 為 false 時 trampoline 走原路徑
.when([]{ return condition; })

// 5. 一次性 / N 次性
.once()
.times(5)
```

`.when` / `.once` / `.times` 可跟 `.onInvoke` / `.before` / `.after` **任意組合**。`onInvoke` / `before` / `after` 三者**互斥**（最後設定的覆蓋之前）。

### 3.3 診斷一行語法糖

```cpp
SPLICE_TRACE(eglSwapBuffers);          // 每次呼叫記 args + 回傳
SPLICE_COUNT(malloc);                  // 計數器，splice::stats<malloc>() 查詢
SPLICE_TIME(glTexImage2D);             // 累計時間，avg/min/max 查詢
```

### 3.4 Hook target 表達方式

| 寫法 | 用途 |
|---|---|
| `SPLICE_HOOK(funcname)` | 連結器看得到的符號，編譯期決定位址 |
| `SPLICE_HOOK_LIB("libfoo.so", funcname)` | 透過 dlopen / LoadLibrary 解析（dlsym 時機）|
| `SPLICE_HOOK_ADDR(&funcname)` | 直接傳函式指標（vtable slot、RVA、JIT addr 都可）|

每個都有 `_STATIC` 變體（保證 entry 永久存在 — 適合 `installAll()` 模式）和 `_AS` 變體（per-call-site 切換 policy，見 §6.2）。

### 3.5 Disable 與生命週期

| 操作 | 行為 |
|---|---|
| `entry.disable()` | Tier 1：原子還原 pointer；Tier 2：原子還原 prologue bytes |
| `entry.is_installed()` | 查 hook 是否仍然 active |
| **`splice::ScopedHook`** | RAII：物件解構自動 disable |
| `splice::install_all()` | 跑所有 queued installer（每個 SPLICE_HOOK_STATIC 都會 queue 一個）|

**重要：不支援「真正的 uninstall + memory reclaim」**——這在 in-process non-priv
hook 是 **架構上不可能** 的（需要知道沒有 thread 還在 trampoline 內，需要 ptrace
或 root）。Splice 給的是 disable：行為還原、callback 保留可重啟、trampoline
記憶體 leak。詳見 §9 limitations。

### 3.6 跨函式型別、跨多引數

- 任何 `Ret(Args...)` 函式都能 hook，**arity 不限**（variadic template）
- `void` 回傳、C linkage、calling convention 都自動推導
- Lambda 可 capture（`[this]`、`[&state]`、捕值都行）
- 編譯期 `decltype(&func)` 推導 — 簽章寫錯 compile-time 報錯，**不會等到 runtime crash**

### 3.7 並行模型 — Policy framework

每個 hook 的 callback 儲存方式由 policy 決定：

| Policy | Reader 成本 | Writer 行為 | 適用 |
|---|---|---|---|
| `splice::policy::rcu_writeonce`（**預設**）| 1 個 atomic acquire-load | 寫一次後不再變；舊 callback **故意 leak**（hook 跟程式同壽）| 99% 場景 |
| `splice::policy::shared_mutex` | reader counter RMW | 寫時取 unique_lock，可動態換 callback | callback 真的會 runtime 換 |
| `splice::policy::rcu_refcounted`（v1.1）| atomic<shared_ptr> load | 寫時 swap | 零 leak 動態換（C++20）|

切換方式：
```cpp
// Per-call-site override（FR-010 Step 4）
SPLICE_HOOK_AS(splice::policy::shared_mutex, my_func)
    .onInvoke(swappable_callback);
```

### 3.8 Registry 並行模型 — Registry framework

整個 HookContext 的 lookup table 也可切換：

| Impl | Reader 成本 | 8t/1t scaling | 寫操作 |
|---|---|---|---|
| `splice::registry::shared_mutex_map`（**預設**）| shared_lock + map find | ~50× | unique_lock |
| `splice::registry::rcu_atomic_array` | pure atomic load + array index | **~4×** ✅ | copy snapshot + atomic publish + 100ms 延遲回收 |

build-time 切：
```bash
cmake -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array
```

---

## 4. 不能做什麼 — 老實話

### 4.1 不會做的

| 功能 | 為什麼不做 |
|---|---|
| 真正的 uninstall + memory reclaim | 架構上不可能（in-process non-priv）— ShadowHook / LSPlant 也辦不到 |
| `uninstallAll()` | 同上，改提供 `disable_all()` |
| Windows VEH hooking | 不需要、SEH 路徑足夠 |
| Kernel-mode hooking | 不在 scope |
| Stop-the-world thread suspend | 需要 ptrace / root |
| Hook 優先順序 | 安裝順序即執行順序，不另做排序 API |
| Python / Rust bindings | v1.0 不做 |
| Unity / Unreal 引擎整合層 | 不在 scope |

### 4.2 設計上的折衷（必須清楚的）

| 折衷 | 含意 |
|---|---|
| **`disable()` ≠ uninstall** | Tier 2 inline disable 還原 prologue 但 trampoline JIT 記憶體 leak（一輩子，bounded by hook 數量） |
| **rcu_writeonce 是 write-once-leak** | 每次 `set_invoke` allocate 一個 `std::function`，舊的不回收。Splice 預期 callback set 一次就不變 |
| **RCU registry 限 SPLICE_MAX_HOOKS** | 預設 512，硬上限。超過要 rebuild |
| **不支援 hook callback 內遞迴呼叫同一 hook** | 會 infinite loop（用 `.when(false)` gate 自己跳過）|
| **`__COUNTER__` ID 系統** | 已修為 `__LINE__ << 16 \| __COUNTER__`，理論上兩個 TU 同行 + 同 counter 仍會撞（機率極低）|
| **不保證 callback 順序** | 同 target 多次 hook（不該這樣做）行為未定義 |

---

## 5. 平台支援矩陣

| 平台 | Arch | 狀態 | 安裝策略 | 備註 |
|---|---|---|---|---|
| Android | ARM64 | ✅ Production | INLINE（無 GOT/PLT for symbol-resolved 路徑）| 真機驗證 Snapdragon 8 Gen 3 |
| Linux | ARM64 | ✅ | GOT/PLT + INLINE | CI 驗證 |
| Linux | x86_64 | ✅ | GOT/PLT + INLINE | CI 驗證 |
| Windows | x64 | ✅ | IAT + INLINE | 真機驗證 |
| Android | ARM32 | ⏸ Optional v1.1 | — | FR-006 |
| macOS | — | ❌ | — | 不在 scope |
| iOS | — | ❌ | — | 不在 scope |

**Sanitizer 覆蓋：** ASan / UBSan（CI）、TSan（Linux Phase 4.5）。

---

## 6. 詳細 API 參考

### 6.1 Public macros（user-facing）

#### Hook 安裝
| Macro | 簽章 | 行為 |
|---|---|---|
| `SPLICE_HOOK(func)` | linker-visible 函式 | thread_local static entry，自動推導型別 |
| `SPLICE_HOOK(lib, func)` | dlopen/LoadLibrary 解析 | install 時才解析符號 |
| `SPLICE_HOOK_LIB(lib, func)` | 同上 | 顯式雙引數版 |
| `SPLICE_HOOK_ADDR(func_ptr)` | 任意位址 | 直接 patch 該位址 |
| `SPLICE_HOOK_MEMBER(Class::method)` | 非虛擬 member function | 推導顯式 this 簽章；callback 第一個參數是 `Class* self` |
| `SPLICE_HOOK_STATIC(...)` | 上述各形式 | `static` 而非 `thread_local static`（entry 永存）|

#### Policy override
| Macro | 用途 |
|---|---|
| `SPLICE_HOOK_AS(Policy, func)` | per-call-site 切 policy（如 shared_mutex）|
| `SPLICE_HOOK_LIB_AS(Policy, lib, func)` | 同上 + 雙引數 |
| `SPLICE_HOOK_ADDR_AS(Policy, ptr)` | 同上 + 直接位址 |
| `SPLICE_HOOK_*_AS_STATIC(...)` | 上述各 `_AS` 形式的 static 變體 |

> Policy 寫在 macro 第一個位置（不能用 `<Policy>` 寫法，因為 C macro 不能傳模板參數）

#### 診斷
| Macro | 行為 |
|---|---|
| `SPLICE_TRACE(func)` | 每次呼叫 log args + return |
| `SPLICE_COUNT(func)` | 計數器 |
| `SPLICE_TIME(func)` | 累計 avg/min/max 時間 |
| `SPLICE_GET_ORIGINAL(lib, func)` | 拿原始函式指標 |
| `SPLICE_IS_INSTALLED(lib, func)` | 查 hook 是否裝著 |
| `SPLICE_CALL_ORIGINAL(lib, func, args...)` | 呼叫原始（若 hook 裝著），否則 no-op |

### 6.2 Fluent modifiers（`InterceptorEntry` 方法）

```cpp
auto& entry = SPLICE_HOOK(func)
    .onInvoke(lambda)          // 替換
  | .before(lambda)            // 之前
  | .after(lambda)             // 之後
  | .when(predicate)            // 條件
  | .once()                     // 一次
  | .times(n);                  // N 次

entry.is_installed();           // 查狀態
entry.disable();                // 觸發 disable
```

### 6.3 全局 API

```cpp
splice::install_all();          // 安裝 queue 中所有 hook
splice::install_count();        // 查 queue 大小
splice_is_hooked(void* addr);   // 偵測 addr 是否被 hook（C ABI）

splice::default_context();      // 取得 process-wide 預設 context
splice::HookContext ctx;        // 自建 context（test 隔離用）
ctx.reset();                    // 清空整個 context（測試 fixture）

splice::register_global_installer([]{...});  // 手動 queue installer
                                              // 回傳 InstallerToken（RAII）
```

### 6.4 RAII helpers

```cpp
splice::ScopedHook h = SPLICE_HOOK(func).onInvoke(lambda);
// h 解構時自動 disable

splice::InstallerToken t = splice::register_global_installer(fn);
// t 解構時自動從 queue 移除
```

---

## 7. Build-time 配置

| Macro / Var | 預設 | 作用 |
|---|---|---|
| `SPLICE_DEFAULT_POLICY` | `::splice::policy::rcu_writeonce` | callback storage 預設 policy |
| `SPLICE_REGISTRY_IMPL` | `::splice::registry::shared_mutex_map` | registry 實作 |
| `SPLICE_MAX_HOOKS` | 512 | rcu_atomic_array snapshot 上限 |
| `SPLICE_RCU_GRACE_PERIOD_MS` | 100 | RCU snapshot 回收 grace period |
| `SPLICE_LOG_TAG` | `"splice"` | log tag 字串 |
| `SPLICE_BUILD_TESTS` | ON | 編 unit tests |
| `SPLICE_BUILD_EXAMPLES` | ON | 編 examples |
| `SPLICE_BUILD_BENCHMARKS` | OFF | 編 microbench |
| `SPLICE_ENABLE_ASAN` | OFF | AddressSanitizer |
| `SPLICE_ENABLE_UBSAN` | OFF | UBSanitizer |

### 配置範例

```bash
# 預設（safe & well-tested）
cmake --preset=windows-x64-dev

# 8 thread heavy contention 場景 — RCU registry
cmake --preset=windows-x64-release \
    -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array

# 動態換 callback 為主的場景 — default policy 改 shared_mutex
cmake --preset=android-arm64-release \
    -DSPLICE_DEFAULT_POLICY=::splice::policy::shared_mutex

# AOSP 系統服務（hook 量大）
cmake --preset=android-arm64-release \
    -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array \
    -DSPLICE_MAX_HOOKS=4096
```

---

## 8. 效能數字（實證）

硬體：AMD Ryzen 9 9950X3D（Windows）/ Snapdragon 8 Gen 3（ARM64）

| 量測 | 數值 |
|---|---|
| Single-thread hooked call | **22.0 ns/call**（從 v1 baseline 41.1，−46%）|
| 8-thread hooked call（端對端）| 923 ns/call（從 v1 baseline 1568，−41%）|
| 8t/1t ratio（端對端）| ~42×（registry-isolated 已達 < 5×）|
| Registry-isolated 8t/1t（RCU mode） | **3.9–5.5×** ✅ |
| Trampoline overhead（理論下限）| ~2 ns（無 hook installed）|

完整 bench：[`fr-010-performance-summary.md`](./fr-010-performance-summary.md)

### Hot-path cost breakdown（Windows 22.0 ns）

| 元件 | ns | 備註 |
|---|---|---|
| Trampoline 進出 | ~2 | jmp rel32 + saved prologue + return |
| `get_original(slot)` | 1.3 | atomic load on shared_mutex_map（0.3 in RCU mode）|
| `get_hook_as<...>(slot)` | 1.0 | 同上 |
| `std::function::operator()` | 1.4 | dispatch — Step 5 microbench 證實這不是瓶頸 |
| Callback body + `orig(x)` | ~16 | depends on user lambda |

---

## 9. 限制與危險區（HARD truths）

### 9.1 In-process non-priv hooking 的物理限制

**不能：** 在 hook 已安裝後，「安全地」釋放 trampoline 記憶體並還原原 prologue
到 100% 原始狀態，**同時** 保證沒有任何 thread 還在 trampoline 內執行。

**為什麼：** 需要：
1. 列舉所有 thread — 要 `/proc/self/task` 或 Win32 `Thread32First`
2. 暫停每一個 thread — 要 `SIGSTOP`（要 ptrace 權限）或 `SuspendThread`
3. 檢查每個 thread 的 instruction pointer 是否在 trampoline 內 — 要 `GetThreadContext`

這些都需要 elevated 權限。Splice 是 in-process non-priv 庫，**做不到**。

**Splice 的折衷：** Tier 1/2 disable — 行為還原（callback 不再執行），記憶體
不還原。任何「自稱可以 in-process uninstall」的 hook library 都在說謊（或在
玩 Russian roulette — ShadowHook 偶發 SIGILL 就是這種來源）。

### 9.2 不要在 callback 內呼叫被 hook 的同一函式

```cpp
SPLICE_HOOK(printf).onInvoke([](auto orig, const char* fmt, ...) {
    printf("called %s\n", fmt);   // ← 遞迴 hook，infinite loop
    // 正確: orig("called %s\n", fmt);
});
```

如果一定要：用 thread_local 旗標 + `.when()` gate 跳過。

### 9.3 `recursive_mutex` 風險

不要在 hook callback 內持有其他鎖然後等鎖（容易 deadlock）。Splice hook
本身會 take 一個 reader-lock（shared_mutex_map mode）— 在 callback 內再去
take writer-lock 並期待其他 reader 出去，會卡死。

### 9.4 Tier 2 disable 的記憶體 leak 是 **設計上**的

不是 bug。documented 在 FR-013、CLAUDE.md、本文件。一個 hook 一輩子 leak
~4 KB（trampoline page）。100 個 hook = 400 KB。對 Android process 完全可
忽略。

### 9.5 ID system 殘留風險

`SPLICE_UNIQUE_ID = __LINE__ << 16 | __COUNTER__` 在跨 TU 同 line + 同
counter 仍會撞（機率 < 1/10⁶）。若撞了，兩個 hook 共享同一個 trampoline
function（C++ ODR），用同一個 registry slot — last writer wins，可能
infinite recursion。**對策**：跨 TU hook 同樣 signature 的函式時，分散到
不同源檔的不同 line（自然會發生）。

### 9.6 ARM64 / x86_64 disassembler 邊界

- ARM64 supports：B, BL, B.cond, ADR, ADRP, LDR literal, CBZ/CBNZ, TBZ/TBNZ, generic instructions
- x86_64 supports：call rel32, jmp rel32/rel8, Jcc rel32/rel8, RIP-relative MOV/LEA, generic 1-byte+

若 prologue 含上述以外的 PC-relative 指令，install 會回 `cannot relocate` 並
失敗（log 警告，不會 crash）。

---

## 10. 常用 recipe / cookbook

### 10.1 Frame counter（最常見場景）

```cpp
std::atomic<uint64_t> g_frames{0};

SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers)
    .after([](EGLBoolean, EGLDisplay, EGLSurface) {
        g_frames.fetch_add(1, std::memory_order_relaxed);
    });
splice::install_all();
```

### 10.2 條件 trace（debug 用，release 自動 disable）

```cpp
constexpr bool kTraceEnabled = SPLICE_DEBUG_BUILD;

if constexpr (kTraceEnabled) {
    SPLICE_TRACE_LIB("libGLESv2.so", glDrawArrays);
}
```

### 10.3 一次性檢查（first-call diagnostic）

```cpp
SPLICE_HOOK(my_init)
    .once()
    .before([](Args... args) {
        SPLICE_LOGI("my_init called for the first time");
    });
```

### 10.4 計時器 + 條件閘

```cpp
SPLICE_HOOK(expensive_func)
    .when([]{ return g_profile_mode.load(); })
    .onInvoke([](auto orig, auto... args) {
        auto start = std::chrono::steady_clock::now();
        auto ret = orig(args...);
        auto dt = std::chrono::steady_clock::now() - start;
        g_total_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
        return ret;
    });
```

### 10.5 動態 enable/disable（shared_mutex policy）

```cpp
SPLICE_HOOK_LIB_AS(splice::policy::shared_mutex,
                   "libfoo.so", swap_target)
    .onInvoke(initial_callback);

// 之後從另一 thread 換 callback
splice::HookManager::get_hook_as<splice::policy::shared_mutex, Ret, Args...>(
    splice::default_context().slot_for(trampoline_ptr))
        .set_invoke(new_callback);
```

### 10.6 ScopedHook RAII

```cpp
{
    splice::ScopedHook h = SPLICE_HOOK(target).onInvoke(lambda);
    do_something_under_hook();
} // h 出 scope，hook 自動 disable
```

### 10.7 隔離 context（單元測試）

```cpp
TEST(MyFeature, isolated_hook) {
    splice::HookContext ctx;   // local context，不污染 default_context
    // ... 在 ctx 上操作 ...
    ctx.reset();              // 測試結束清理
}
```

---

## 11. 兩種 hook 範式（可執行範例）

Splice 支援兩種本質不同的攔截方式。多數 hooking 教學只講第一種;懂兩種才能
正確 hook 像 Vulkan 這種 API。兩種各有可執行、雙平台驗證的範例在 `examples/`。

### 範式 A — 直接 patch 目標函式

覆寫目標 prologue(inline patch)或換 import-table slot(GOT/PLT/IAT)。目標
函式被呼叫就觸發。這是經典做法 —— `a production game enhancer` 對 GLES 就這樣,
[`examples/gpu_app`](../examples/gpu_app/) 示範:逐個 hook `glViewport` 類命令、
改它們的**參數**來升級解析度。

```cpp
SPLICE_HOOK_ADDR(&gpu::set_viewport)            // patch 這個函式本身
    .when([]{ return enabled; })
    .onInvoke([](auto orig, int w, int h) {
        orig(w * 4, h * 4);                      // 改參數
    });
```

何時用:目標是可直接呼叫的 exported/已知函式(libc、GLES、遊戲自家函式、
vtable slot)。

### 範式 B — hook dispatcher,改它的回傳值

有些 API 不把函式當可 patch 的符號 export —— 你透過一個 *resolver* 拿函式
指標。**Vulkan** 是典型:`vkGetDeviceProcAddr(device, "vkCmdDraw")` 回傳指標。
真實 Vulkan 工具(RenderDoc、MangoHud、validation layer)不 patch
`vkCmdDraw`,而是 hook **getter**、回傳自己的 wrapper。
[`examples/vulkan_app`](../examples/vulkan_app/) 用單一 Splice hook 改 getter 的
**回傳值**:

```cpp
SPLICE_HOOK_ADDR(&vk::vk_get_device_proc_addr)   // patch RESOLVER,一次
    .onInvoke([](auto orig, VkDevice dev, const char* name) -> PFN_vkVoidFunction {
        auto real = orig(dev, name);
        if (strcmp(name, "vkCmdDraw") == 0) {
            g_real_draw = (PFN_CmdDraw)real;     // 擷取真實指標
            return (PFN_vkVoidFunction)&my_wrapped_draw;   // 回傳 wrapper
        }
        return real;                             // 其餘原樣放行
    });
```

何時用:目標透過 dispatch/factory 函式取得 —— Vulkan proc-addr getter、
COM `QueryInterface`、plugin `dlsym` shim,任何「給我 X 的函式指標」API。

### 為什麼這個分別重要

- **一個 hook vs 多個。** 範式 B hook **單一**函式(resolver)就能影響**所有**
  命令;範式 A 每個命令都要 hook。
- **prologue 約束不同。** 範式 A inline-patch 目標,所以目標 prologue 必須可
  relocate(libc-heavy 函式可能不行 —— 見 §9.6)。範式 B 從不 patch 命令、只
  patch resolver,所以命令本體多複雜都行。(demo 裡 `vkQueuePresentKHR` 用 B
  成功計數,而等價的 `present()` 在 A 裡無法 inline-hook。)
- **展示的能力。** A 改**參數**;B 改**回傳值**(函式指標)。Splice 的
  `.onInvoke` 兩者都能。

| | 範式 A(`gpu_app`)| 範式 B(`vulkan_app`)|
|---|---|---|
| hook 目標 | 每個命令 | resolver,一次 |
| 真實對應 | GLES、libc、遊戲函式 | Vulkan、COM、plugin dlsym |
| 改寫 | 參數 | 回傳的函式指標 |
| 目標 prologue 須可 relocate | 是 | 否(命令不動)|

---

## 12. 工具鏈與整合

### 12.1 CMake 整合

```cmake
find_package(splice REQUIRED)
target_link_libraries(my_target PRIVATE splice::splice)
```

### 12.2 VCPKG manifest

```json
{
  "dependencies": ["splice"],
  "features": {
    "benchmarks": { "description": "FR-010 microbench", "dependencies": ["benchmark"] }
  }
}
```

### 12.3 編譯需求

- C++20（API surface 為 C++17-compatible，內部用 C++20 features 如 `if constexpr` + `std::atomic<shared_ptr>` (opt-in)）
- CMake 3.21+
- GTest（測試）/ Google Benchmark（bench）
- VS2022 + Ninja（Windows）/ NDK r29+（Android）/ Clang 16+ or GCC 12+（Linux）

### 12.4 vendored / 內建第三方

- `platform_log.h` — Allen 的跨平台 C log substrate（vendored，不要 inline 改）
- `nmd_assembly.h` — x86_64 disassembler

---

## 13. FAQ

### Q1. Splice vs Frida / Substrate / ShadowHook / LSPlant？

| | Splice | Frida | Substrate | ShadowHook | LSPlant |
|---|---|---|---|---|---|
| 語言 | C++20 | JS/Py | C/Obj-C | C/C++ | C++ |
| 嵌入式 | ✅ | ❌（runtime 需 1+ MB）| ✅ | ✅ | ✅ |
| 跨 ARM64/x86_64 | ✅ | ✅ | iOS only | ARM only | ARM64 only |
| Type-safe API | ✅ | ❌ | ❌ | ❌ | ❌ |
| **真 uninstall** | ❌ | ⚠ Russian roulette | ❌ | ⚠ SIGILL | ⚠ UB |
| Fluent DSL | ✅ | ❌ | ❌ | ❌ | ❌ |

Splice 的核心優勢是 **type-safe + fluent + 對限制誠實**。沒拼 raw 功能廣度。

### Q2. 為什麼 hook 設計上不支援優先順序？

the predecessor framework 時代的經驗：**hook 該被當成獨立模組組合**，不該有「我跑在他之
前」這種跨模組依賴。要排序就用單一 hook + 內部 dispatch，別讓 framework 操心。

### Q3. 為什麼預設不用最快的 RCU registry？

RCU 有 trade-off（writer 慢 10×、記憶體 ~2× 暫時、AOSP toolchain 風險）。99%
使用者只跑 1-2 個 thread，預設 `shared_mutex_map` 就夠。重度多線程場景再
opt-in `rcu_atomic_array`。這是「safe default + powerful opt-in」原則。

### Q4. 為什麼 callback 還用 std::function 而不是 template？

實證後決定不換（[`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md)）。
- 估算：std::function 開銷 ~5 ns
- 實測：1.4–1.5 ns（MSVC/Clang 已有 SBO + devirt）
- 換 thunk 只能省 ~1 ns，不值得 ~200 行 hot-path 重構

工程紀律：**動 hot path 前先 microbench 驗證**，不憑教科書直覺優化。

### Q5. 可以 hook C++ member function 嗎？

可以。**非虛擬** member 有一行語法糖：

```cpp
SPLICE_HOOK_MEMBER(Widget::scale)
    .onInvoke([](auto orig, Widget* self, int factor) {
        return orig(self, factor);   // `this` 是第一個顯式參數
    });
```

`SPLICE_HOOK_MEMBER` 透過 `member_function_traits` 把
`Ret(Class::*)(Args...)` 推導成顯式 this 的 free-function 簽章
`Ret(*)(Class*, Args...)`，並從 pointer-to-member 取出 code address。

**虛擬** member 不能用這個 macro —— 虛擬函式的 pointer-to-member 是 vtable
offset 而非 code address。請在 live instance 上解析 vtable slot，改用
`SPLICE_HOOK_ADDR(slot_addr)`。

### Q6. 跨 process / cross-binary hook？

Splice 是 in-process 庫。要跨 process 自己想辦法 inject + 啟動 Splice。

### Q7. 跟 LD_PRELOAD / SetWindowsHookEx 比？

那些是 OS-level 機制，Splice 是 process-internal 動態 patch。可以並用。

---

## 14. 進階主題

- **引擎內幕（patch flow / atomic install / disasm）**：[`hooking-internals.md`](./hooking-internals.md)
- **設計取捨深井**：[`v2-design-rationale.md`](./v2-design-rationale.md)
- **效能旅程 + 工程紀律**：[`fr-010-performance-summary.md`](./fr-010-performance-summary.md)
- **Step 6 RCU 設計**：[`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md)
- **Step 5 std::function 真相**：[`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md)
- **跟 v1 對照（短版）**：[`splice-vs-predecessor-summary.md`](./splice-vs-predecessor-summary.md)
- **vs Detours / PolyHook2 / SubHook / rcmp（spec 比較）**：[`splice-vs-hooking-libraries.md`](./splice-vs-hooking-libraries.md)
- **demo loader 對照**：`reference/a production game enhancer_Loader.cpp`（v1）vs `reference/splice_game_Loader.cpp`（v2）

---

## 15. 一句話收尾

> 寫起來像 DSL、跑起來零滯後、對什麼可行什麼不可行誠實。
>
> 99% 使用者只需要 `SPLICE_HOOK(func).onInvoke([](auto orig, ...){...})` +
> `splice::install_all()`。1% 重度場景開 build flag 切 RCU registry +
> shared_mutex policy。Splice 都讓你做。

---

## 變更紀錄

- 2026-05-31：初版。涵蓋 Phase 0–4.5 + FR-008/010/013 + FR-009 (進行中)。
