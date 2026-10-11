// ─── Pointer-to-virtual-member → vtable slot index — implementation ───────
#include "vtable_slot.h"

#include <splice/log.h>

#include <cstdint>
#include <cstring>

namespace splice::engine {

namespace {

// An interface with more slots than this is not something splice has seen, and
// a bogus displacement decoding to a plausible-looking index is worse than a
// refusal. DXGI's largest hooked index today is 39.
constexpr int kMaxPlausibleSlot = 4096;

[[nodiscard]] bool read_word(const void* bytes, unsigned int size, unsigned int offset,
                             std::uintptr_t& out) {
    if (offset + sizeof(std::uintptr_t) > size) return false;
    std::memcpy(&out, static_cast<const char*>(bytes) + offset, sizeof(out));
    return true;
}

#if defined(_MSC_VER)

// Microsoft ABI. A pointer to a virtual member is the address of a thunk that
// loads the vtable and tail-jumps through one of its slots. Three encodings
// occur on x64, all three measured against cl 19.44 at /Od and /O2:
//
//   48 8B 01  FF 20            mov rax,[rcx]; jmp [rax]            slot 0
//   48 8B 01  FF 60 d8         mov rax,[rcx]; jmp [rax+d8]         slots 1..15
//   48 8B 01  FF A0 d32        mov rax,[rcx]; jmp [rax+d32]        slots 16+
//
// The disp8 form stops at 0x78 and the compiler switches to disp32 at 0x80,
// which is exactly where a decoder that treated the byte as unsigned and never
// tested the boundary would start reporting slot 16 as slot -16.
//
// In an incremental-linked build the thunk's address is an ILT entry -- a bare
// `E9 rel32` -- so a bounded follow comes first. That follow deliberately does
// not handle `FF 25`, and deliberately does not consult the registry of live
// patch sites the way engine.cpp's unwrap_jump_stub must: this is not an
// arbitrary function address that might already be hooked, it is a thunk the
// compiler emitted in this binary moments ago, and anything else here is
// something to refuse rather than chase.
constexpr int kMaxThunkJumps = 4;

const std::uint8_t* follow_near_jumps(const std::uint8_t* code) {
    for (int depth = 0; depth < kMaxThunkJumps; ++depth) {
        if (code == nullptr || code[0] != 0xE9) break;
        std::int32_t rel32 = 0;
        std::memcpy(&rel32, code + 1, sizeof(rel32));
        code = code + 5 + rel32;
    }
    return code;
}

// One asymmetry between the two ABIs is worth stating plainly, because it is
// the limit of what this function can promise. On the Itanium ABI a bit in the
// pointer-to-member says "this is virtual", so a non-virtual member is REFUSED
// on the evidence. Under MSVC there is no such bit: a non-virtual member's
// pointer is its code address, and the only way to tell is that the code there
// does not look like a dispatch thunk. So the refusal is by pattern, and a
// non-virtual function whose body began with exactly `mov rax,[rcx]` followed
// by a jump through [rax+disp] would be misread as a vtable slot. That shape is
// a hand-written virtual dispatch, so it does not occur by accident -- but it
// is a pattern match, not a proof, and nothing above here should be written as
// though it were.
int decode_ms_thunk(std::uintptr_t first_word) {
#if defined(_M_X64)
    if (first_word == 0) return -1;
    const std::uint8_t* code =
        follow_near_jumps(reinterpret_cast<const std::uint8_t*>(first_word));
    if (code == nullptr) return -1;

    // mov rax, [rcx] -- the vtable load. An adjustor thunk for a secondary
    // base starts by changing rcx instead, and falls out here.
    if (!(code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01)) {
        SPLICE_LOGV("decode_ms_thunk: %p does not begin with mov rax,[rcx] "
                    "(%02X %02X %02X)", code, code[0], code[1], code[2]);
        return -1;
    }
    code += 3;

    std::int64_t displacement = 0;
    if (code[0] == 0xFF && code[1] == 0x20) {
        displacement = 0;
    } else if (code[0] == 0xFF && code[1] == 0x60) {
        displacement = static_cast<std::int8_t>(code[2]);   // signed: see above
    } else if (code[0] == 0xFF && code[1] == 0xA0) {
        std::int32_t disp32 = 0;
        std::memcpy(&disp32, code + 2, sizeof(disp32));
        displacement = disp32;
    } else {
        SPLICE_LOGV("decode_ms_thunk: unrecognised jump form %02X %02X", code[0], code[1]);
        return -1;
    }

    if (displacement < 0 ||
        displacement % static_cast<std::int64_t>(sizeof(void*)) != 0) {
        SPLICE_LOGV("decode_ms_thunk: implausible displacement %lld",
                    static_cast<long long>(displacement));
        return -1;
    }
    const std::int64_t index = displacement / static_cast<std::int64_t>(sizeof(void*));
    if (index > kMaxPlausibleSlot) return -1;
    return static_cast<int>(index);
#else
    (void)first_word;
    SPLICE_LOGW_ONCE("slot_of_member_pointer: the Microsoft ABI thunk form is "
                     "decoded for x64 only; name the slot index by hand");
    return -1;
#endif
}

#endif  // _MSC_VER

} // namespace

int slot_of_member_pointer(const void* pmf_bytes, unsigned int pmf_size) {
    if (pmf_bytes == nullptr) return -1;

    std::uintptr_t first = 0;
    if (!read_word(pmf_bytes, pmf_size, 0, first)) return -1;

#if defined(_MSC_VER)
    // A Microsoft-ABI pointer-to-member is pointer-sized only for single
    // inheritance. Anything wider carries a this-adjustment or a virtual-base
    // index, which a slot number alone cannot express.
    if (pmf_size != sizeof(void*)) {
        SPLICE_LOGV("slot_of_member_pointer: pointer-to-member is %u bytes, "
                    "so the class is not single-inheritance", pmf_size);
        return -1;
    }
    return decode_ms_thunk(first);
#else
    std::uintptr_t adjustment = 0;
    if (!read_word(pmf_bytes, pmf_size, sizeof(std::uintptr_t), adjustment)) return -1;

#   if defined(__aarch64__) || defined(__arm__)
    if ((adjustment & 1u) == 0) {
        SPLICE_LOGV("slot_of_member_pointer: not a virtual member (arm adj=%llu)",
                    static_cast<unsigned long long>(adjustment));
        return -1;
    }
    // adj is (this-offset << 1) | 1. A non-zero offset means the member comes
    // from a secondary base, and the call needs the pointer adjusted first.
    if ((adjustment >> 1) != 0) return -1;
    const std::uintptr_t byte_offset = first;
#   else
    // This refusal and the alignment check below cannot be told apart on this
    // ABI, and the reason is arithmetic rather than a gap in the tests: if the
    // low bit is clear then first - 1 is odd, so it is never a whole number of
    // slots either. Removing this check survives mutation for that reason, and
    // it stays because it states the rule the next reader needs. The arm
    // branch above is different -- the marker lives in the adjustment word
    // there, so nothing else is checking it, and a fabricated-pointer test
    // covers it on that architecture.
    if ((first & 1u) == 0) {
        SPLICE_LOGV("slot_of_member_pointer: not a virtual member (itanium ptr=%llu)",
                    static_cast<unsigned long long>(first));
        return -1;
    }
    if (adjustment != 0) return -1;      // same reason as the arm branch above
    const std::uintptr_t byte_offset = first - 1u;
#   endif

    if (byte_offset % sizeof(void*) != 0) return -1;
    const std::uintptr_t index = byte_offset / sizeof(void*);
    if (index > static_cast<std::uintptr_t>(kMaxPlausibleSlot)) return -1;
    return static_cast<int>(index);
#endif
}

} // namespace splice::engine
