/*
 * BoxedVN - sparse high-address window for a 32-bit direct guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX32_GUEST_WINDOW_H
#define BOXEDVN_FEX32_GUEST_WINDOW_H

#include "boxedvn/guest_address_contract32.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace boxedvn {

enum Fex32WindowAccess : std::uint8_t {
    Fex32WindowNone = 0,
    Fex32WindowRead = 1u << 0,
    Fex32WindowWrite = 1u << 1,
    Fex32WindowExecute = 1u << 2,
};

// The platform owns reservation and protection mechanics. Keeping that seam
// injected makes the guest-address and page-state contract testable without
// reserving 4 GiB in the host test process.
class Fex32VirtualMemory {
public:
    virtual ~Fex32VirtualMemory() = default;

    virtual std::uint64_t hostPageSize() const noexcept = 0;
    virtual std::optional<HostAddress64> reserve(
        std::uint64_t size, std::uint64_t alignment) noexcept = 0;
    virtual bool commit(HostAddress64 address, std::uint64_t size,
                        std::uint8_t access) noexcept = 0;
    virtual bool protect(HostAddress64 address, std::uint64_t size,
                         std::uint8_t access) noexcept = 0;
    virtual bool decommit(HostAddress64 address,
                          std::uint64_t size) noexcept = 0;
    virtual void release(HostAddress64 address,
                         std::uint64_t size) noexcept = 0;
};

// The injected virtual-memory provider must outlive the window. The window
// owns the reservation and releases the complete 4 GiB span at destruction.
class Fex32GuestWindow final {
public:
    static constexpr std::uint64_t kWindowSize = 0x100000000ULL;
    static constexpr std::uint64_t kWindowAlignment = 0x100000000ULL;
    static constexpr std::uint64_t kPageSize = 0x1000;
    static constexpr std::uint32_t kPageCount =
        static_cast<std::uint32_t>(kWindowSize / kPageSize);

    static std::unique_ptr<Fex32GuestWindow> create(
        Fex32VirtualMemory& memory);

    ~Fex32GuestWindow();

    Fex32GuestWindow(const Fex32GuestWindow&) = delete;
    Fex32GuestWindow& operator=(const Fex32GuestWindow&) = delete;

    HostAddress64 hostBase() const noexcept { return hostBase_; }
    std::uint64_t hostPageSize() const noexcept { return hostPageSize_; }

    bool commitPage(std::uint32_t page, std::uint8_t access) noexcept;
    bool protectPage(std::uint32_t page, std::uint8_t access) noexcept;
    bool decommitPage(std::uint32_t page) noexcept;

    bool isCommitted(std::uint32_t page) const noexcept;
    std::uint8_t pageAccess(std::uint32_t page) const noexcept;

    // Translation succeeds only when every covered guest page is committed
    // with the requested permissions. The returned integer becomes a pointer
    // only inside the platform/runtime layer that owns the reservation.
    std::optional<HostAddress64> translate(
        GuestAddress32 guestAddress, std::uint64_t length,
        std::uint8_t requiredAccess) const noexcept;
    // Reverse translation is arithmetic only: an address merely has to be in
    // the reservation. KMemory must validate logical mapping and permissions.
    std::optional<GuestAddress32> hostToGuest(
        HostAddress64 hostAddress, std::uint64_t length = 1) const noexcept;

private:
    Fex32GuestWindow(Fex32VirtualMemory& memory, HostAddress64 hostBase,
                     GuestAddressContract32 contract,
                     std::uint64_t hostPageSize);

    static bool validAccess(std::uint8_t access) noexcept;
    std::optional<HostAddress64> pageHostAddress(
        std::uint32_t page) const noexcept;
    std::optional<HostAddress64> hostSpanAddress(
        std::uint32_t page) const noexcept;
    bool isCommittedUnlocked(std::uint32_t page) const noexcept;
    std::uint8_t pageAccessUnlocked(std::uint32_t page) const noexcept;
    std::uint8_t combinedHostAccessUnlocked(
        std::uint32_t page, std::uint8_t replacementState) const noexcept;
    bool hostSpanCommittedUnlocked(
        std::uint32_t page, std::uint8_t replacementState) const noexcept;

    Fex32VirtualMemory& memory_;
    HostAddress64 hostBase_;
    GuestAddressContract32 contract_;
    std::uint64_t hostPageSize_;
    std::uint32_t guestPagesPerHostPage_;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> pageState_;
};

}  // namespace boxedvn

#endif  // BOXEDVN_FEX32_GUEST_WINDOW_H
