// ─── open(2), in a file that cannot see namespace splice ──────────────────
//
// This exists for a name clash, and the clash is worth knowing about before
// the next person reaches for <fcntl.h>:
//
//   glibc declares `ssize_t splice(int, loff_t*, int, loff_t*, size_t,
//   unsigned)` -- the zero-copy pipe syscall -- in <fcntl.h>. A translation
//   unit that has both that declaration and `namespace splice` is ill-formed,
//   in EITHER order: a namespace name and a function name cannot share a
//   scope. Measured both ways on gcc 13; the error names whichever came
//   second as "redeclared as different kind of entity".
//
// So no translation unit inside namespace splice may include <fcntl.h>, and
// the one call that needs it lives here instead, in a file that declares no
// namespace at all. Nothing else belongs in this file.
//
// It never came up before because nothing in the library had opened a file.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

extern "C" {

// open(path, O_RDONLY | O_CLOEXEC), or -1 with errno set. Read and close it
// with the ordinary <unistd.h> calls, which declare no `splice`.
int splice_os_open_readonly(const char* path);

}
