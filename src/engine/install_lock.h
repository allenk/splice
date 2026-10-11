// ─── The one lock every install and disable takes ──────────────────────────
//
// It was a file-local detail of engine.cpp until the group transaction needed
// it too, and the reason it has to be shared rather than duplicated is the
// reason it exists at all: **page protection is not reference counted.** Two
// installs that make the same page writable, write, and put it back can revoke
// each other's write access mid-write. A second mutex would serialise each
// kind of install against itself and neither against the other, which is the
// bug with extra steps.
//
// What it does not make safe: patching a function while other threads execute
// it. That is what the atomic install sequence is for, and what the group
// transaction's suspension is for; neither has anything to do with this lock.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <mutex>

namespace splice::engine {

std::mutex& install_mutex();

} // namespace splice::engine
