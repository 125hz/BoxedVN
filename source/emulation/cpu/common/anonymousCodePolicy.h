/*
 * Boxedwine anonymous executable-code compatibility policy.
 * GPLv2; see license.txt.
 */

#ifndef BOXEDWINE_ANONYMOUS_CODE_POLICY_H
#define BOXEDWINE_ANONYMOUS_CODE_POLICY_H

#include <cstdint>

// Wine and Boxedwine themselves create small executable relay regions that do
// not have an ELF/PE name. They are stable emulator plumbing, not browser JIT
// heaps, and interpreting them puts every guest transition on the reference
// CPU. Device traces identify the Wine relay page at 0x7D400000 and the
// Boxedwine callback/stub reservation at 0xF0000000. Keep those on the ARM64
// translator while the compatibility profile interprets application-created
// anonymous heaps such as V8's 0x3xxxxxxx/0x4xxxxxxx code spaces.
inline bool shouldInterpretAnonymousExecutableAddress(std::uint32_t address) {
    const bool wineRelay = address >= 0x7D400000u && address < 0x7D410000u;
    const bool boxedwineStub = address >= 0xF0000000u && address < 0xF1000000u;
    return !wineRelay && !boxedwineStub;
}

#endif
