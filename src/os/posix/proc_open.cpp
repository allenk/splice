// ─── open(2) — implementation, with no splice namespace in sight ──────────
//
// See proc_open.h for why this is its own translation unit.
#include "proc_open.h"

#include <fcntl.h>

extern "C" int splice_os_open_readonly(const char* path) {
    return ::open(path, O_RDONLY | O_CLOEXEC);
}
