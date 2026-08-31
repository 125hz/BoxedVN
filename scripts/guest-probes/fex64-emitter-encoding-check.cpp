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
    // Unqualified names here would find this namespace's own declarations, so
    // reach for the standard allocator explicitly.
    const size_t effective = alignment < alignof(void*) ? alignof(void*) : alignment;
    const size_t rounded = (size + effective - 1) & ~(effective - 1);
    return std::aligned_alloc(effective, rounded);
}
void aligned_free(void* pointer) {
    std::free(pointer);
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

// BoxedWine's guest address translation is two ARM64 logical-immediate
// instructions:
//
//     orr Xd, Xn, #kGuestLowAliasBase     -- canonical low -> host alias
//     bic Xd, Xd, #kGuestTopClearMask     -- Wine's top arena -> its own block
//
// Both constants have to be encodable as logical immediates or the emitter
// gives up at translation time, on device, with no way to see it coming. This
// drives the real emitter with the exact constants the runtime publishes.
void checkGuestAddressTranslationEncodings() {
    // Kept as literals rather than including the runtime header: this tool
    // links only the emitter, and a copy that silently drifted would be worse
    // than one that fails loudly. The contract test pins them together.
    constexpr uint64_t kGuestLowAliasBase = 0x7800000000ULL;
    constexpr uint64_t kGuestTopClearMask = 0x7F8000000000ULL;

    // ORR (immediate), 64-bit: sf=1 opc=01 100100.
    constexpr uint32_t kLogicalImmMask = 0xFF800000u;
    constexpr uint32_t kOrrImmOpcode = 0xB2000000u;
    // AND (immediate), 64-bit; `bic` is emitted as AND with the complement.
    constexpr uint32_t kAndImmOpcode = 0x92000000u;

    const uint32_t aliasWord = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.orr(ARMEmitter::Size::i64Bit, ARMEmitter::Register(13),
                    ARMEmitter::Register(11), kGuestLowAliasBase);
    });
    if ((aliasWord & kLogicalImmMask) != kOrrImmOpcode) {
        fail("the low alias base did not encode as ORR (immediate)",
             kOrrImmOpcode, aliasWord & kLogicalImmMask);
    }
    if ((aliasWord & 0x1Fu) != 13u || ((aliasWord >> 5) & 0x1Fu) != 11u) {
        fail("the low alias ORR named the wrong registers", 13u,
             aliasWord & 0x1Fu);
    }

    const uint32_t clearWord = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.bic(ARMEmitter::Size::i64Bit, ARMEmitter::Register(13),
                    ARMEmitter::Register(13), kGuestTopClearMask);
    });
    if ((clearWord & kLogicalImmMask) != kAndImmOpcode) {
        fail("the top clear mask did not encode as AND (immediate)",
             kAndImmOpcode, clearWord & kLogicalImmMask);
    }
    if ((clearWord & 0x1Fu) != 13u || ((clearWord >> 5) & 0x1Fu) != 13u) {
        fail("the top relocation BIC named the wrong registers", 13u,
             clearWord & 0x1Fu);
    }
    // Two distinct instructions: a relocation that collapsed into the alias
    // ORR would leave the arena pointing at an address the host cannot map.
    if (aliasWord == clearWord) {
        fail("the alias and relocation emitted the same word", aliasWord,
             clearWord);
    }
}

// The aliased stack paths must issue their accesses with NO writeback: the
// pre/post-indexed forms update the address register, and with a translated
// address in it that would put a host pointer into the guest's RSP. This
// drives the real emitter with the exact calls those paths make, so both the
// overload resolution and the addressing mode are proven rather than assumed.
//
// It also exists because stp/ldp are IndexType templates with no plain
// overload -- a fact that is invisible in the signature and cost a build.
void checkStackAccessAddressingModes() {
    // LDP/STP encode the addressing mode in bits 24:23; 0b10 is the
    // no-writeback offset form.
    constexpr uint32_t kPairModeShift = 23;
    constexpr uint32_t kPairModeOffset = 0b10;
    // LDUR/STUR are the unscaled-immediate form: bits 11:10 are zero, where
    // the pre- and post-indexed forms carry 0b11 and 0b01.
    constexpr uint32_t kUnscaledModeShift = 10;

    const uint32_t pairStore = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.stp<ARMEmitter::IndexType::OFFSET>(ARMEmitter::XRegister(4), ARMEmitter::XRegister(5),
                                                   ARMEmitter::Register(13), 0);
    });
    if (((pairStore >> kPairModeShift) & 0b11) != kPairModeOffset) {
        fail("PushTwo's aliased store is not the no-writeback pair form",
             kPairModeOffset, (pairStore >> kPairModeShift) & 0b11);
    }

    const uint32_t pairLoad = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.ldp<ARMEmitter::IndexType::OFFSET>(ARMEmitter::XRegister(4), ARMEmitter::XRegister(5),
                                                   ARMEmitter::Register(13), 0);
    });
    if (((pairLoad >> kPairModeShift) & 0b11) != kPairModeOffset) {
        fail("PopTwo's aliased load is not the no-writeback pair form",
             kPairModeOffset, (pairLoad >> kPairModeShift) & 0b11);
    }

    const uint32_t store = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.stur(ARMEmitter::XRegister(4), ARMEmitter::Register(13), 0);
    });
    if (((store >> kUnscaledModeShift) & 0b11) != 0) {
        fail("Push's aliased store is not the unscaled no-writeback form", 0,
             (store >> kUnscaledModeShift) & 0b11);
    }

    const uint32_t load = emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.ldur(ARMEmitter::XRegister(4), ARMEmitter::Register(13), 0);
    });
    if (((load >> kUnscaledModeShift) & 0b11) != 0) {
        fail("Pop's aliased load is not the unscaled no-writeback form", 0,
             (load >> kUnscaledModeShift) & 0b11);
    }

    // The sub-word forms the aliased Push and Pop also use.
    (void)emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.sturb(ARMEmitter::Register(4), ARMEmitter::Register(13), 0);
    });
    (void)emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.sturh(ARMEmitter::Register(4), ARMEmitter::Register(13), 0);
    });
    (void)emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.ldurb(ARMEmitter::Register(4), ARMEmitter::Register(13), 0);
    });
    (void)emitLastWord([](ARMEmitter::Emitter& emitter) {
        emitter.ldurh(ARMEmitter::Register(4), ARMEmitter::Register(13), 0);
    });
}

} // namespace

int main() {
    checkDeviceFaultingInstruction();
    checkMaterializedAddressForm();
    checkFullImmediateRange();
    checkGuestAddressTranslationEncodings();
    checkStackAccessAddressingModes();

    if (gFailures != 0) {
        std::fprintf(stderr, "emitter-encoding-check: FAIL (%d checks)\n", gFailures);
        return 1;
    }
    std::printf("[fex-vixl] emitter encoding check: unscaled Q load/store immediates stay inside imm9\n");
    return 0;
}
