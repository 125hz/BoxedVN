/*
 * BoxedVN - FEX guest-mode admission policy.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX_GUEST_MODE_POLICY_H
#define BOXEDVN_FEX_GUEST_MODE_POLICY_H

#include <cstdint>

namespace boxedvn {

// FEX's global bitness setting is process-wide. Keep the guest mode explicit
// at every context boundary so adding the 32-bit syscall adapter later cannot
// silently reuse the CPU64 one.
enum class FexGuestMode : std::uint8_t {
    X86_32,
    X86_64,
};

enum class FexGuestModeAdmission : std::uint8_t {
    RejectedNoLinux32Adapter,
    Admitted,
};

// The BoxedWine bridge currently implements only the Linux x86-64 register
// and syscall ABI. IA-32 remains an explicit future mode, never an accidental
// fallback to the 64-bit handler.
constexpr FexGuestModeAdmission fexGuestModeAdmission(
    FexGuestMode mode) noexcept {
    return mode == FexGuestMode::X86_64
        ? FexGuestModeAdmission::Admitted
        : FexGuestModeAdmission::RejectedNoLinux32Adapter;
}

constexpr bool fexGuestModeAdmitted(FexGuestMode mode) noexcept {
    return fexGuestModeAdmission(mode) == FexGuestModeAdmission::Admitted;
}

constexpr const char* fexGuestModeName(FexGuestMode mode) noexcept {
    return mode == FexGuestMode::X86_64 ? "x86-64" : "x86-32";
}

}  // namespace boxedvn

#endif  // BOXEDVN_FEX_GUEST_MODE_POLICY_H
