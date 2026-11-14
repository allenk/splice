// ─── Every entry point the public header declares has a definition ────────
//
// The C ABI in `include/splice/engine.h` is what a consumer links against. It
// has two implementations: `src/engine/engine.cpp` for a platform with an OS
// layer and a matching architecture backend, and `src/engine/engine_stub.cpp`
// for one without -- a 32-bit ARM or x86 Android build, say, where the public
// API still has to compile and link so a consumer can build and be told at run
// time that hooking is unavailable.
//
// Three entry points were added to the header over time and only to the first
// implementation. On the stub platforms that is an unresolved external at link
// time, in a library whose header promised the function -- and it is invisible
// here, because no configuration this project builds compiles the stub. A
// consumer on `armeabi-v7a` finds out instead.
//
// So this reads the header and the stub and compares the sets. Only the stub,
// and the asymmetry is the design: the backed implementation is spread over
// `engine.cpp`, `live_install.cpp`, `slot_patcher.cpp` and `vtable_slot.cpp`,
// and it needs no check here because this binary is linked against it -- a
// declaration implemented nowhere is a link error in the test suite itself.
// The stub is the one implementation nothing links, so it is the one that has
// to be read. The first draft of this test checked both and failed, because it
// expected one file to hold what four of them do.
//
// What it proves is exactly that and no more: a name in the header has a name
// in the stub. It cannot see a signature that disagrees, and it is not a link
// check. The only real one is a build with `SPLICE_HAS_ENGINE` off, and
// nothing performs one -- that condition needs an architecture with no
// backend compiled, which no preset and no continuous-integration job here
// targets. So a stub whose signature drifts from the header has no detector
// at all, and the consumer on such a platform is the one who finds out.
//
// Reading source text from a test is unusual and is the point: the fact being
// checked is a property of the source, and both files arrive here by the same
// route a reader would take rather than through anything the build derived
// from them.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

#if !defined(SPLICE_SOURCE_ROOT)
#   error "SPLICE_SOURCE_ROOT must name the repository root; see tests/CMakeLists.txt"
#endif

std::string read_file(const std::string& relative) {
    const std::string path = std::string(SPLICE_SOURCE_ROOT) + "/" + relative;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Every `splice_...` immediately followed by `(` -- a declaration, a
// definition or a call, which is all three of the things a name can appear as
// and all three of which are evidence that the name exists in that file.
//
// The `(` is not decoration and the test was wrong without it. A plain
// substring search finds every name in the stub, because the stub names each
// one in its own log message: deleting a definition and leaving the
// `SPLICE_LOGW_ONCE` behind passed a detector that was supposed to catch
// exactly that, and red-proving it is what said so.
std::set<std::string> entry_points(const std::string& text) {
    std::set<std::string> names;
    const std::string prefix = "splice_";
    for (std::size_t at = text.find(prefix); at != std::string::npos;
         at = text.find(prefix, at + 1)) {
        if (at > 0 && name_char(text[at - 1])) continue;   // a longer word
        std::size_t end = at;
        while (end < text.size() && name_char(text[end])) ++end;
        if (end >= text.size() || text[end] != '(') continue;
        names.insert(text.substr(at, end - at));
    }
    return names;
}

} // namespace

TEST(CAbiCompleteness, the_stub_defines_every_entry_point_the_header_declares) {
    const std::string header = read_file("include/splice/engine.h");
    const std::string stub = read_file("src/engine/engine_stub.cpp");

    ASSERT_FALSE(header.empty()) << "include/splice/engine.h not readable from "
                                << SPLICE_SOURCE_ROOT;
    ASSERT_FALSE(stub.empty()) << "src/engine/engine_stub.cpp not readable";

    const std::set<std::string> declared = entry_points(header);
    // A guard against the extraction silently finding nothing, which would
    // make every comparison below pass for the wrong reason -- the failure
    // this project has caught three times now.
    ASSERT_GT(declared.size(), 10u)
        << "the header should declare more than ten entry points; the "
           "extraction found " << declared.size();

    for (const char* known : {"splice_hook_address", "splice_disable",
                              "splice_live_prepare", "splice_vtable_swap"}) {
        EXPECT_TRUE(declared.count(known) == 1)
            << known << " is missing from the extracted set, so the extraction "
                        "is wrong rather than the sources";
    }

    const std::set<std::string> defined = entry_points(stub);
    std::set<std::string> missing_from_stub;
    for (const std::string& name : declared) {
        if (defined.count(name) == 0) missing_from_stub.insert(name);
    }

    const auto joined = [](const std::set<std::string>& names) {
        std::string out;
        for (const std::string& name : names) {
            if (!out.empty()) out += ", ";
            out += name;
        }
        return out;
    };

    EXPECT_TRUE(missing_from_stub.empty())
        << "declared in include/splice/engine.h and absent from "
           "src/engine/engine_stub.cpp, which is an unresolved external on "
           "every platform with no backend compiled: "
        << joined(missing_from_stub);
}
