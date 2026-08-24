/*
 * BoxedVN - x86-64 dynamic-loader handoff patch planning.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX64_LOADER_HANDOFF_H
#define BOXEDVN_FEX64_LOADER_HANDOFF_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace boxedvn {

struct Fex64LoaderHandoffPatch final {
    static constexpr std::size_t kBranchSize = 6;
    static constexpr std::size_t kTrampolineSize = 13;

    std::array<std::uint8_t, kBranchSize> branchToTrampoline;
    std::array<std::uint8_t, kTrampolineSize> trampoline;
};

constexpr std::optional<std::uint64_t>
validatedFex64LoaderFallbackEntry(
        std::uint64_t faultRIP, std::uint64_t interpreterBase,
        std::uint64_t programEntry,
        const std::array<std::uint8_t, 4>& faultBytes) noexcept {
    constexpr std::array<std::uint8_t, 4> elfMagic {0x7f, 'E', 'L', 'F'};
    if (faultRIP != interpreterBase || programEntry == 0 ||
        programEntry == faultRIP || faultBytes != elfMagic) {
        return std::nullopt;
    }
    return programEntry;
}

// Replaces a six-byte loader handoff site with `jmp rel32` plus padding. The
// nearby trampoline loads the validated program entry into caller-saved R11
// and jumps through it. This avoids the RIP-relative memory-indirect branch
// that did not retire correctly in the live iOS translator while preserving
// the guest stack and every register with defined ELF process-entry meaning.
constexpr std::optional<Fex64LoaderHandoffPatch>
planFex64LoaderHandoffPatch(std::uint64_t jumpAddress,
                            std::uint64_t trampolineAddress,
                            std::uint64_t programEntry) noexcept {
    if (programEntry == 0 ||
        jumpAddress > std::numeric_limits<std::uint64_t>::max() - 5) {
        return std::nullopt;
    }

    const std::uint64_t nextInstruction = jumpAddress + 5;
    std::int64_t displacement = 0;
    if (trampolineAddress >= nextInstruction) {
        const std::uint64_t delta = trampolineAddress - nextInstruction;
        if (delta > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        displacement = static_cast<std::int64_t>(delta);
    } else {
        const std::uint64_t delta = nextInstruction - trampolineAddress;
        constexpr std::uint64_t minimumMagnitude =
            std::uint64_t{1} << 31;
        if (delta > minimumMagnitude) {
            return std::nullopt;
        }
        displacement = -static_cast<std::int64_t>(delta);
    }

    const std::uint32_t encodedDisplacement = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(displacement));
    Fex64LoaderHandoffPatch patch {
        {0xe9, 0, 0, 0, 0, 0x90},
        {0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xff, 0xe3},
    };
    for (std::size_t byte = 0; byte < 4; ++byte) {
        patch.branchToTrampoline[1 + byte] = static_cast<std::uint8_t>(
            encodedDisplacement >> (byte * 8));
    }
    for (std::size_t byte = 0; byte < 8; ++byte) {
        patch.trampoline[2 + byte] = static_cast<std::uint8_t>(
            programEntry >> (byte * 8));
    }
    return patch;
}

}  // namespace boxedvn

#endif  // BOXEDVN_FEX64_LOADER_HANDOFF_H
