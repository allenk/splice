// ─── Splice logging — out-of-line helpers ──────────────────────────────────
//
// Keeps the header (<splice/log.h>) macro-heavy and inline-friendly, while
// parking the two pieces that genuinely need out-of-line storage here:
//   - splice_log_hex_impl  : per-row formatted hex dump
//   - splice::log::{set,get}_level / is_enabled : runtime level switch
// ───────────────────────────────────────────────────────────────────────────

#include <splice/log.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// Default: at-or-above the compile-time PLOG_LEVEL.
// Users can lower it further at runtime via splice::log::set_level().
std::atomic<int> g_splice_log_level{PLOG_LEVEL};

// Emit a hex dump row via the chosen PLOG level. Keeps formatting in one
// place so every row looks identical regardless of backend.
void emit_hex_row(const char* level_tag, const void* base, const unsigned char* p,
                  unsigned long row_len) {
    // Format: "0x<addr>: xx xx xx xx xx xx xx xx  xx xx xx xx xx xx xx xx"
    char buf[96];
    int off = std::snprintf(buf, sizeof(buf), "%p:", base);
    for (unsigned long i = 0; i < row_len && off < static_cast<int>(sizeof(buf)) - 4; ++i) {
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                             "%s%02x", (i == 8 ? "  " : " "),
                             static_cast<unsigned>(p[i]));
    }

    // Route through the compile-time-selected backend.
    switch (level_tag[0]) {
        case 'E': PLOG_E("%s", buf); break;
        case 'W': PLOG_W("%s", buf); break;
        case 'I': PLOG_I("%s", buf); break;
        case 'V': PLOG_V("%s", buf); break;
        case 'D':
        default:  PLOG_D("%s", buf); break;
    }
}

} // namespace

// ─── C ABI: splice_log_hex_impl ─────────────────────────────────────────────
extern "C" void splice_log_hex_impl(const char* level_tag, const void* ptr,
                                    unsigned long len) {
    if (ptr == nullptr || len == 0) return;

    // Safety cap — hooks often log trampoline bytes (16 or 32), never huge regions.
    constexpr unsigned long kMaxDump = 4096;
    if (len > kMaxDump) len = kMaxDump;

    // Runtime level gate: skip the work entirely if suppressed.
    if (!splice::log::is_enabled(splice::log::Level::Debug)) return;

    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (unsigned long i = 0; i < len; i += 16) {
        const unsigned long row_len = (len - i < 16) ? len - i : 16;
        emit_hex_row(level_tag, bytes + i, bytes + i, row_len);
    }
}

// ─── C++ runtime level switch ───────────────────────────────────────────────
namespace splice::log {

void set_level(Level level) noexcept {
    g_splice_log_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level get_level() noexcept {
    return static_cast<Level>(g_splice_log_level.load(std::memory_order_relaxed));
}

bool is_enabled(Level level) noexcept {
#if defined(SPLICE_LOG_RUNTIME_LEVEL_DISABLED)
    // Compile-time only: trust PLOG_LEVEL.
    return static_cast<int>(level) >= PLOG_LEVEL;
#else
    return static_cast<int>(level) >= g_splice_log_level.load(std::memory_order_relaxed);
#endif
}

} // namespace splice::log
