# Template Callback 取代 `std::function` — 評估文件

**狀態：** 評估中 / 待決議
**對應：** FR-010 Step 5
**目標讀者：** Allen（決策者）
**撰寫日期：** 2026-05-18

---

## 問題陳述

目前 Splice 的 hook 熱路徑用 `std::function` 儲存 callback：

```cpp
// include/splice/context.h:125
using HookFn = std::function<Ret(FuncType, Args...)>;
```

這是你最初 the predecessor framework 的設計選擇，**運作完全正確**。但 FR-010 的效能目標
要求單線程 hook < 20 ns，目前 23.7 ns，剩下的 ~4 ns 大部分卡在 `std::function`
的型別擦除開銷上：

1. **vtable hop**：每次 `invoke` 都會走一次 `std::function` 內部的 manager 函式
   間接呼叫（類似虛擬函式調用）
2. **SBO 邊界**：lambda capture > ~16 bytes 會 heap 分配（雖然只在 `store` 時，
   但 cache miss 機率上升）
3. **編譯器無法 inline**：`std::function::operator()` 跨越型別擦除邊界，跨翻譯
   單元的內聯被切斷

預估收益：**23.7 ns → 16-18 ns（-25% to -30%）**，清掉 < 20 ns gate。

---

## 核心問題：使用者 API 會變嗎？

**簡短答案：99% 不變。寫法完全一樣，只是型別檢查時機從執行時挪到編譯時。**

詳細答案：分三種使用場景。

### 場景 A — 直接寫 lambda（90% 的用法）

#### 改動前（現在）
```cpp
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
        ++frame_count;
        return orig(d, s);
    });
```

#### 改動後（Template Callback）
```cpp
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
        ++frame_count;
        return orig(d, s);
    });
```

**一字不改。** 編譯器把 lambda 的型別推導出來，在 template `onInvoke` 內以
原始型別存起來。沒有型別擦除。

---

### 場景 B — 帶 capture 的 lambda（5% 的用法）

#### 改動前
```cpp
int counter = 0;
std::string label = "frame";
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([&counter, label](auto orig, EGLDisplay d, EGLSurface s) {
        ++counter;
        SPLICE_LOGD("%s=%d", label.c_str(), counter);
        return orig(d, s);
    });
```

#### 改動後
```cpp
int counter = 0;
std::string label = "frame";
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([&counter, label](auto orig, EGLDisplay d, EGLSurface s) {
        ++counter;
        SPLICE_LOGD("%s=%d", label.c_str(), counter);
        return orig(d, s);
    });
```

**一字不改。** capture 跟著 lambda 一起被儲存進 `HookStorage`，內部佈局從
`std::function` 的 SBO buffer 變成直接 in-place 存放。

**唯一注意點：** lambda 物件在 `onInvoke` 內被 **複製** 一份（與現在 `std::function`
行為相同）。`&counter` 的參考依然指向你 caller scope 的變數，所以
`++counter` 仍然會修改原本的 `counter`。語意完全等價。

---

### 場景 C — 把 `std::function` 變數塞進去（< 1% 的用法，可能你完全沒用）

這是 **唯一** 會有差別的場景。

#### 改動前
```cpp
std::function<int(int(*)(int), int)> stored_callback = make_my_callback();
SPLICE_HOOK_ADDR(&some_func)
    .onInvoke(stored_callback);   // OK — std::function copy-init from std::function
```

#### 改動後（兩種方案）

**方案 1：完全相容**
```cpp
// 這行也照樣編譯通過 — std::function 自己就是 callable，會被當成
// 另一個 callable type 存進 HookStorage。但效能會回到 std::function 等級
// （因為 HookStorage 內部呼叫 std::function::operator()）。
std::function<int(int(*)(int), int)> stored_callback = make_my_callback();
SPLICE_HOOK_ADDR(&some_func).onInvoke(stored_callback);
```

**方案 2：嚴格但更快**
```cpp
// 拒絕 std::function — 編譯期報錯，逼使用者用 lambda 或函式指標
SPLICE_HOOK_ADDR(&some_func).onInvoke(stored_callback); // ❌ static_assert
```

**建議：方案 1（無聲相容）**。Splice 不該主動敲使用者既有的 `std::function`
寫法。

---

## 內部設計變動（不影響使用者）

### Before — `HookStorage<rcu_writeonce>`

```cpp
template <typename Ret, typename... Args>
struct HookStorage<policy::rcu_writeonce, Ret, Args...> {
    using HookFn = std::function<Ret(FuncType, Args...)>;
    std::atomic<HookFn*> m_fn{nullptr};

    void store(HookFn fn) {
        m_fn.store(new HookFn(std::move(fn)), std::memory_order_release);
    }
    Ret invoke(FuncType orig, Args... args) {
        if (auto* fn = m_fn.load(std::memory_order_acquire))
            return (*fn)(orig, args...);       // ← std::function vtable hop
        return orig(args...);
    }
};
```

### After — type-erased thunk + raw state

```cpp
template <typename Ret, typename... Args>
struct HookStorage<policy::rcu_writeonce, Ret, Args...> {
    using FuncType = Ret(*)(Args...);
    using Thunk    = Ret(*)(void* state, FuncType, Args...);

    std::atomic<Thunk> m_thunk{nullptr};
    std::atomic<void*> m_state{nullptr};

    template <typename Lambda>
    void store(Lambda fn) {
        // Static-storage instance per (call-site, Lambda type). One write
        // per hook over program lifetime — same leak semantics as before.
        static Lambda* s_fn = new Lambda(std::move(fn));
        m_state.store(s_fn, std::memory_order_relaxed);
        m_thunk.store(
            +[](void* st, FuncType orig, Args... args) -> Ret {
                return (*static_cast<Lambda*>(st))(orig, args...);
            },
            std::memory_order_release);
    }

    Ret invoke(FuncType orig, Args... args) {
        if (auto th = m_thunk.load(std::memory_order_acquire)) {
            return th(m_state.load(std::memory_order_relaxed), orig, args...);
        }
        return orig(args...);
    }
};
```

熱路徑差異：
- **舊**：load `HookFn*` → vtable 查 `operator()` → call manager → call 真正 lambda
- **新**：load `Thunk` 函式指標 → 一次間接呼叫 → lambda inline 在 thunk 內

少一層 vtable hop、少一次 manager pointer 查表。
（且 `Thunk` 是普通函式指標，編譯器可以猜分支預測）

### `HookAs::set_invoke` 的簽章變動

#### Before
```cpp
template <typename Policy, typename Ret, typename... Args>
class HookAs : public HookBase {
public:
    using HookFn = std::function<Ret(FuncType, Args...)>;
    void set_invoke(HookFn fn) { m_storage.store(std::move(fn)); }
    // ...
};
```

#### After
```cpp
template <typename Policy, typename Ret, typename... Args>
class HookAs : public HookBase {
public:
    template <typename Lambda>
    void set_invoke(Lambda&& fn) {
        m_storage.store(std::forward<Lambda>(fn));
    }
    // ...
};
```

`InterceptorEntry::onInvoke` 跟著一起變 template：

#### Before
```cpp
InterceptorEntry& onInvoke(std::function<Ret(FuncType, Args...)> fn) {
    HookManager::get_hook_as<Policy, Ret, Args...>(m_unique_id)
        .set_invoke(std::move(fn));
    return *this;
}
```

#### After
```cpp
template <typename Lambda>
InterceptorEntry& onInvoke(Lambda&& fn) {
    HookManager::get_hook_as<Policy, Ret, Args...>(m_unique_id)
        .set_invoke(std::forward<Lambda>(fn));
    return *this;
}
```

---

## 取捨表

| 軸線              | std::function (現況) | Template Callback (提案)        |
|-------------------|---------------------|--------------------------------|
| **單線程延遲**     | 23.7 ns             | **預估 16-18 ns**（清 < 20 ns gate） |
| **使用者 API 變動** | —                   | **零變動**（場景 A/B 一字不改）   |
| **型別擦除**       | 執行期（vtable）     | 編譯期（template）              |
| **編譯時間**       | 基準                | **每個 call site +template 實例化**（每處增 ~50-100ms） |
| **二進位大小**     | 基準                | **每個 hook 點 +一份 thunk 函式**（~50 bytes/site） |
| **lambda capture 限制** | 任何大小       | 任何大小（但每 type 一份 static） |
| **執行期換 callback** | 自由           | **rcu_writeonce 下不能換到不同型別**；shared_mutex 仍可（保留 `std::function`） |
| **debugger 體驗**  | 跨型別擦除邊界，stack trace 跳兩次 | lambda 直接出現在 trace |
| **錯誤訊息**       | runtime（`bad_function_call`） | **編譯時**（lambda 簽章不對直接報錯） |

---

## 主要風險點

### 1. 編譯時間爆炸（中等風險）

每個 `SPLICE_HOOK*` call site 會 instantiate 一份獨立的 `HookStorage::store<Lambda>`
+ thunk 函式。如果一個 binary 有 200+ 個 hook 點，編譯時間可能 +20%。

**緩解：** Splice 內部 hook 點通常 < 50 個（OpenGL/EGL/syscall）。實測再決定要不要
做 explicit instantiation。

### 2. lambda 型別重綁定（低風險）

```cpp
SPLICE_HOOK_ADDR(&foo).onInvoke(lambda_a);  // store as Lambda_A
// ...later, 同一個 hook id...
SPLICE_HOOK_ADDR(&foo).onInvoke(lambda_b);  // 嘗試 store Lambda_B
```

在 rcu_writeonce 下，第二次呼叫會 **靜默覆蓋** 第一個 lambda，但因為兩個 lambda
型別不同，會各自 instantiate 一份 `static Lambda* s_fn`，第二次呼叫的 thunk
指向新型別的 storage。沒有 UB，但有意外行為（兩個 lambda 都活著）。

**緩解：** `SPLICE_HOOK_ADDR(&foo)` 透過 `__COUNTER__` 已經是不同 call site，
所以實務上不會撞型別。文件加註：onInvoke 只該被呼叫一次（rcu_writeonce 語意）。
shared_mutex policy 仍保留 std::function 以支援動態換。

### 3. 與既有 `std::function` 傳參相容（已透過方案 1 解決）

見上面場景 C。

---

## 兩階段實作方案（建議）

### 階段 5a — 僅換 `rcu_writeonce` policy（小改動，大收益）

- `HookStorage<rcu_writeonce>` 換成 thunk + state 設計
- `HookStorage<shared_mutex>` **不變**，繼續用 `std::function`
- `HookAs::set_invoke` 變 template
- 預期：23.7 → 17-18 ns

理由：99% 使用者跑的是 rcu_writeonce（預設）。shared_mutex 是 escape hatch，
本來就是慢路徑，沒必要動。

### 階段 5b — 進階：thunk 路徑全部 inline（可選）

把 thunk 函式從外部函式變成 `[[gnu::always_inline]]` 的成員 template。再省 ~2 ns
但動到 ABI（trampoline 路徑會直接持有 thunk，沒有 `HookContext::get_hook`）。

**這階段超出 Step 5 範圍**，留給未來。

---

## 不做的選項（已排除）

### Option X：強制 captureless lambda → 純 function pointer
```cpp
void onInvoke(Ret(*)(FuncType, Args...));  // 拒絕 lambda capture
```

- ✅ 最快（理論可達 10-12 ns）
- ❌ **破壞既有 API**：你目前所有 `[&state]` capture 寫法全部要重寫
- ❌ 違反「保留 friendly fluent API feel」原則（CLAUDE.md §rules）

**不採用。**

---

## 決議要點

請決定：

1. **要不要做 Step 5a？**（rcu_writeonce 換 template，shared_mutex 不動）
   - 收益：清 < 20 ns gate，benchmark 再下一個里程碑
   - 成本：~200 行 hot-path 改動 + benchmark 驗證 + 文件更新

2. **若做，是否接受場景 C 的「方案 1」相容路徑？**（`std::function` 變數仍可傳入，
   只是不享受加速）
   - 推薦 yes — 不破壞任何既有用法

3. **編譯時間是否為硬性限制？**（若 < 5 秒上限不能突破，需先測 baseline）

---

## 附錄：和 benchmark-baseline.md 的關係

目前的 benchmark 數據：
- 1-thread hooked: 23.7 ns（目標 < 20 ns，**未達**）
- 8-thread hooked: 688 ns，8t/1t = 26.9×（目標 < 5×，**未達**）

Step 5 攻擊的是 1-thread 數字。8-thread ratio 是 Step 6（真正 RCU registry）
的工作，不在這份評估範圍。

---

## 附錄 B：實證 microbench（2026-05-19）

在拍板 Step 5a 之前，先寫了一個獨立 microbench `benchmark/bench_callback_storage.cpp`，
**不碰 Splice 內部**，直接比三條路徑：
- `BM_DirectCall` — `orig(x) + 1`，無 storage 間接層（floor）
- `BM_StdFunctionPath` — `std::atomic<std::function*>` load + invoke（鏡像現況）
- `BM_ThunkPath` — `std::atomic<thunk_fn>` + `std::atomic<void*>` load + invoke（鏡像 Step 5）

三條路徑共用同一個 captureless lambda body（`orig(x) + 1`），同一個 noinline target。

### 平台 1：Windows x86_64（Ryzen 9 9950X3D, MSVC /O2, Release）

| 路徑                | mean        | median       | cv     | delta vs Direct |
|---------------------|-------------|--------------|--------|-----------------|
| BM_DirectCall       | 2.60 ns     | 2.60 ns      | 0.63 % | 0               |
| BM_StdFunctionPath  | 3.99 ns     | 3.99 ns      | 0.23 % | **+1.39 ns**    |
| BM_ThunkPath        | 3.09 ns     | 3.10 ns      | 1.77 % | **+0.49 ns**    |

**Thunk 相對 std::function 省下 0.90 ns/call（−23% 的 dispatch overhead）**。

⚠️ **重要修正：原本估的「~4 ns std::function 開銷」過於樂觀**。x64 編譯器對
`std::function` 的 SBO + devirt 已經做得非常好。實際 dispatch overhead ~1.4 ns，
換 thunk 後 ~0.5 ns。

### 平台 2：Android ARM64（Snapdragon 8 Gen 3, Clang -O2, Release）

_待測量 — 設備接上 USB 後跑：_
```bash
adb push out/build/android-arm64-bench/benchmark/bench_callback_storage /data/local/tmp/
adb shell /data/local/tmp/bench_callback_storage --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

歷史經驗 ARM64 Clang 對 std::function 的優化不如 x64 MSVC，預期 delta 會大於
x86_64 的 0.90 ns。**這是 Step 5a 的決策關鍵點。**

### 重新校準的 Step 5a 預期收益

把 Windows microbench 結果套回 Splice 端對端：

| | 現況 | Step 5a 後（預估） | gate (< 20 ns) |
|---|---|---|---|
| Windows x86_64 1-thread | 23.7 ns | **~22.8 ns**（−0.9） | ❌ 仍未達 |
| Android ARM64 1-thread | （尚無 baseline，待測） | TBD | TBD |

**結論：x86_64 上 Step 5a 不足以單獨清掉 < 20 ns gate。** 還需要其它優化（trampoline
路徑減少 atomic load、`get_hook` 走更快的 thread-local 快取等）。

如果 ARM64 microbench 顯示 std::function 開銷 > 5 ns（高機率），Step 5a 在
ARM64 上會非常有效——而 ARM64 才是 Splice 的主戰場（Android 遊戲增強）。

**決策路徑：**
1. 跑完 ARM64 microbench
2. 若 ARM64 delta > 3 ns → 強烈建議 Step 5a
3. 若 ARM64 delta < 1 ns → 考慮跳過 Step 5，直接做 Step 6（RCU registry，攻擊 8t ratio）

### microbench 在哪？

- 原始碼：`benchmark/bench_callback_storage.cpp`
- Windows preset：`windows-x64-bench`
- Android preset：`android-arm64-bench`（本次新增）
- 可重跑驗證，不污染 Splice 主路徑。
