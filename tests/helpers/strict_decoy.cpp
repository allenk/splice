#include <splice/engine.h>
#include "os/memory.h"
#include <cstring>

// A separately linked copy of Splice: its site registry is NOT the test EXE's.
// The fixture lives until process exit, just like a resident overlay DLL.
namespace {
unsigned char* site = nullptr;
}
extern "C" __declspec(dllexport) void* splice_test_decoy_site() {
    if (site != nullptr) return site;
    site = static_cast<unsigned char*>(splice::os::allocate_executable_memory(4096));
    if (site == nullptr) return nullptr;
    std::memset(site, 0x90, 4096);
    const unsigned char code[] = {0xb8, 42, 0, 0, 0, 0xc3};
    std::memcpy(site + 64, code, sizeof(code));
    void* original = nullptr;
    if (splice_hook_address(site, site + 64, &original) == nullptr) return nullptr;
    return site;
}
