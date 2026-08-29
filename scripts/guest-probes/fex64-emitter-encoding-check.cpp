// Encoding regression for FEX's ARM64 unscaled 128-bit load/store path.
//
// Device build 137 faulted with SIGILL on a host word of 0xffff0177: the low
// half-word of a well-formed STUR Q23, [X11, #-16] with the opcode replaced by
// the sign bits of a negative displacement. This drives the real
// ARMEmitter::Emitter -- the same emitter the iOS translator links -- with the
// maintained downstream patches applied, and asserts that no unscaled
// Q-register displacement can reach the opcode field.
//
// It also pins the shape the patched JIT/MemoryOps.cpp indexed-context path now
// emits: the base offset is materialized into the address register, so the
// LDUR/STUR that follows must carry a zero immediate.

#include <CodeEmitter/Emitter.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// FEX declares these out of line so host tools can pick their own allocator.
// The emitter only reaches them through fextl::vector.
namespace FEXCore::Allocator {
void* aligned_alloc(size_t alignment, size_t size) {
    void* result = nullptr;
    if (posix_memalign(&result, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0) {
        return nullptr;
    }
    return result;
}
void aligned_free(void* pointer) {
    ::free(pointer);
}
} // namespace FEXCore::Allocator

// Referenced by ERROR_AND_DIE_FMT; assertions are off in this translation unit,
// so reaching either of these means the emitter gave up on a valid request.
namespace LogMan {
namespace Throw {
    [[noreturn]] void MFmt(const char*, const fmt::format_args&) {
        std::fprintf(stderr, "emitter-encoding-check: emitter threw\n");
        std::abort();
    }
} // namespace Throw
namespace Msg {
    void MFmtImpl(DebugLevels, const char*, const fmt::format_args&) {
        std::fprintf(stderr, "emitter-encoding-check: emitter reported a fatal message\n");
        std::abort();
    }
} // namespace Msg
} // namespace LogMan

namespace {

constexpr size_t kBufferBytes = 4096;
constexpr uint32_t kSturQOpcodeMask = 0xffe00c00u;
constexpr uint32_t kSturQOpcode = 0x3c800000u;
constexpr uint32_t kLdurQOpcode = 0x3cc00000u;
constexpr uint32_t kDeviceFaultWord = 0xffff0177u;

int gFailures = 0;

void fail(const char* what, uint32_t expected, uint32_t actual) {
    std::fprintf(stderr, "emitter-encoding-check: %s: expected 0x%08x, got 0x%08x\n", what, expected, actual);
    ++gFailures;
}

// Emits through the real emitter and returns the last instruction word.
template<typename EmitFn>
uint32_t emitLastWord(EmitFn&& emitFn) {
    static uint8_t buffer[kBufferBytes];
    ARMEmitter::Emitter emitter(buffer, sizeof(buffer));
    emitFn(emitter);
    const size_t offset = emitter.GetCursorOffset();
    if (offset < sizeof(uint32_t) || (offset % sizeof(uint32_t)) != 0) {
        std::fprintf(stderr, "emitter-encoding-check: emitter produced %zu bytes\n", offset);
        ++gFailures;
        return 0;
    }
    uint32_t word = 0;
    __builtin_memcpy(&word, buffer + offset - sizeof(uint32_t), sizeof(word));
    return word;
}

void checkDeviceFaultingInstruction() {
    // The exact lowering libc's `movups [rax + 0x30], xmm0` produced on device.
    const uint32_t word = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.stur(ARMEmitter::QRegister(23), ARMEmitter::Register(11), -16);
    });
    if (word != 0x3c9f0177u) {
        fail("STUR Q23, [X11, #-16]", 0x3c9f0177u, word);
    }
    if (word == kDeviceFaultWord) {
        fail("STUR Q23, [X11, #-16] reproduced the device fault word", 0x3c9f0177u, word);
    }

    const uint32_t load = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.ldur(ARMEmitter::QRegister(23), ARMEmitter::Register(11), -16);
    });
    if (load != 0x3cdf0177u) {
        fail("LDUR Q23, [X11, #-16]", 0x3cdf0177u, load);
    }
}

// The patched indexed-context path materializes the base offset into TMP1 and
// then emits LDUR/STUR with no displacement at all.
void checkMaterializedAddressForm() {
    const uint32_t store = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.add(ARMEmitter::Size::i64Bit, ARMEmitter::Register(11), ARMEmitter::Register(11), 24);
        emitter.stur(ARMEmitter::QRegister(23), ARMEmitter::Register(11));
    });
    if (store != (kSturQOpcode | (11u << 5) | 23u)) {
        fail("STUR Q23, [X11] after materializing the offset", kSturQOpcode | (11u << 5) | 23u, store);
    }

    const uint32_t load = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.add(ARMEmitter::Size::i64Bit, ARMEmitter::Register(11), ARMEmitter::Register(11), 24);
        emitter.ldur(ARMEmitter::QRegister(23), ARMEmitter::Register(11));
    });
    if (load != (kLdurQOpcode | (11u << 5) | 23u)) {
        fail("LDUR Q23, [X11] after materializing the offset", kLdurQOpcode | (11u << 5) | 23u, load);
    }
}

// Every architecturally valid unscaled displacement must stay inside imm9.
void checkFullImmediateRange() {
    for (int32_t immediate = -256; immediate <= 255; ++immediate) {
        const uint32_t store = emitLastWord([immediate](ARMEmitter::Emitter& emitter) {
            emitter.stur(ARMEmitter::QRegister(23), ARMEmitter::Register(11), immediate);
        });
        const uint32_t expectedStore =
            kSturQOpcode | ((static_cast<uint32_t>(immediate) & 0x1ffu) << 12) | (11u << 5) | 23u;
        if (store != expectedStore) {
            fail("STUR Q23 immediate sweep", expectedStore, store);
            return;
        }
        if ((store & kSturQOpcodeMask) != kSturQOpcode) {
            fail("STUR Q23 immediate sweep corrupted the opcode", kSturQOpcode, store & kSturQOpcodeMask);
            return;
        }
        if (store == kDeviceFaultWord) {
            fail("STUR Q23 immediate sweep produced the device fault word", expectedStore, store);
            return;
        }

        const uint32_t load = emitLastWord([immediate](ARMEmitter::Emitter& emitter) {
            emitter.ldur(ARMEmitter::QRegister(23), ARMEmitter::Register(11), immediate);
        });
        const uint32_t expectedLoad =
            kLdurQOpcode | ((static_cast<uint32_t>(immediate) & 0x1ffu) << 12) | (11u << 5) | 23u;
        if (load != expectedLoad) {
            fail("LDUR Q23 immediate sweep", expectedLoad, load);
            return;
        }
        if (load == kDeviceFaultWord) {
            fail("LDUR Q23 immediate sweep produced the device fault word", expectedLoad, load);
            return;
        }
    }
}

} // namespace

int main() {
    checkDeviceFaultingInstruction();
    checkMaterializedAddressForm();
    checkFullImmediateRange();

    if (gFailures != 0) {
        std::fprintf(stderr, "emitter-encoding-check: FAIL (%d checks)\n", gFailures);
        return 1;
    }
    std::printf("[fex-vixl] emitter encoding check: unscaled Q load/store immediates stay inside imm9\n");
    return 0;
}
