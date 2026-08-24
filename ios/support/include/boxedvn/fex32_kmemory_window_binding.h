/*
 * BoxedVN - KMemory binding for the sparse FEX32 guest window.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX32_KMEMORY_WINDOW_BINDING_H
#define BOXEDVN_FEX32_KMEMORY_WINDOW_BINDING_H

#include "boxedvn/fex32_guest_window.h"
#include "fex32kmemorybinding.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace boxedvn {

class Fex32WindowKMemoryBinding final : public Fex32KMemoryBinding {
public:
    explicit Fex32WindowKMemoryBinding(Fex32GuestWindow& window);

    bool mapAnonymous(std::uint32_t firstPage, std::uint32_t pageCount,
                      std::uint8_t access) noexcept override;
    bool protectAnonymous(std::uint32_t firstPage, std::uint32_t pageCount,
                          std::uint8_t access) noexcept override;
    bool unmapAnonymous(std::uint32_t firstPage,
                        std::uint32_t pageCount) noexcept override;
    bool clear() noexcept override;

    bool isMapped(std::uint32_t page) const noexcept override;
    bool isCommitted(std::uint32_t page) const noexcept override;
    std::uint8_t pageAccess(std::uint32_t page) const noexcept override;
    std::optional<std::uintptr_t> hostPageAddress(
        std::uint32_t page) const noexcept override;
    bool healthy() const noexcept override;

private:
    struct PageSnapshot {
        std::uint32_t page;
        bool committed;
        std::uint8_t access;
    };

    static bool validRange(std::uint32_t firstPage,
                           std::uint32_t pageCount) noexcept;
    static bool validAccess(std::uint8_t access) noexcept;
    bool restorePage(const PageSnapshot& snapshot) noexcept;
    bool unmapAnonymousLocked(std::uint32_t firstPage,
                              std::uint32_t pageCount) noexcept;

    Fex32GuestWindow& window_;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> mapped_;
    bool healthy_ = true;
};

}  // namespace boxedvn

#endif  // BOXEDVN_FEX32_KMEMORY_WINDOW_BINDING_H
