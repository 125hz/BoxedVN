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
    std::array<std::uint8_t, 6> indirectJump;
    std::array<std::uint8_t, 8> entrySlot;
};

// Replaces a six-byte loader handoff site with `jmp qword ptr [rip+disp32]`.
// The target slot lives in read-only padding of the loader's executable page,
// so the patch neither consumes a guest register nor changes the guest stack.
constexpr std::optional<Fex64LoaderHandoffPatch>
planFex64LoaderHandoffPatch(std::uint64_t jumpAddress,
                            std::uint64_t entrySlotAddress,
                            std::uint64_t programEntry) noexcept {
    if (programEntry == 0 ||
        jumpAddress > std::numeric_limits<std::uint64_t>::max() - 6) {
        return std::nullopt;
    }

    const std::uint64_t nextInstruction = jumpAddress + 6;
    std::int64_t displacement = 0;
    if (entrySlotAddress >= nextInstruction) {
        const std::uint64_t delta = entrySlotAddress - nextInstruction;
        if (delta > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        displacement = static_cast<std::int64_t>(delta);
    } else {
        const std::uint64_t delta = nextInstruction - entrySlotAddress;
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
        {0xff, 0x25, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0},
    };
    for (std::size_t byte = 0; byte < 4; ++byte) {
        patch.indirectJump[2 + byte] = static_cast<std::uint8_t>(
            encodedDisplacement >> (byte * 8));
    }
    for (std::size_t byte = 0; byte < 8; ++byte) {
        patch.entrySlot[byte] = static_cast<std::uint8_t>(
            programEntry >> (byte * 8));
    }
    return patch;
}

}  // namespace boxedvn

#endif  // BOXEDVN_FEX64_LOADER_HANDOFF_H
