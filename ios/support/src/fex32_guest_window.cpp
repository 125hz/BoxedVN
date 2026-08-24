/*
 * BoxedVN - sparse high-address window for a 32-bit direct guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/fex32_guest_window.h"

namespace boxedvn {

namespace {

constexpr std::uint8_t kCommitted = 1u << 7;
constexpr std::uint8_t kAccessMask =
    Fex32WindowRead | Fex32WindowWrite | Fex32WindowExecute;

}  // namespace

std::unique_ptr<Fex32GuestWindow> Fex32GuestWindow::create(
    Fex32VirtualMemory& memory) {
    const std::uint64_t hostPageSize = memory.hostPageSize();
    if (hostPageSize < kPageSize || hostPageSize > kWindowSize ||
        (hostPageSize & (hostPageSize - 1)) != 0 ||
        (hostPageSize % kPageSize) != 0) {
        return nullptr;
    }

    const auto reserved = memory.reserve(kWindowSize, kWindowAlignment);
    if (!reserved.has_value()) {
        return nullptr;
    }

    const std::uint64_t base = reserved->value;
    const bool highAndAligned =
        base >= kWindowSize && (base % kWindowAlignment) == 0;
    const auto contract = GuestAddressContract32::create(
        GuestAddressContract32::Mode::BiasedDirect, base, kWindowSize);
    if (!highAndAligned || !contract.has_value()) {
        memory.release(*reserved, kWindowSize);
        return nullptr;
    }

    try {
        return std::unique_ptr<Fex32GuestWindow>(new Fex32GuestWindow(
            memory, *reserved, *contract, hostPageSize));
    } catch (...) {
        memory.release(*reserved, kWindowSize);
        throw;
    }
}

Fex32GuestWindow::Fex32GuestWindow(Fex32VirtualMemory& memory,
                                   HostAddress64 hostBase,
                                   GuestAddressContract32 contract,
                                   std::uint64_t hostPageSize)
    : memory_(memory),
      hostBase_(hostBase),
      contract_(contract),
      hostPageSize_(hostPageSize),
      guestPagesPerHostPage_(
          static_cast<std::uint32_t>(hostPageSize / kPageSize)),
      pageState_(kPageCount, Fex32WindowNone) {}

Fex32GuestWindow::~Fex32GuestWindow() {
    memory_.release(hostBase_, kWindowSize);
}

bool Fex32GuestWindow::validAccess(std::uint8_t access) noexcept {
    return (access & ~kAccessMask) == 0;
}

std::optional<HostAddress64> Fex32GuestWindow::pageHostAddress(
    std::uint32_t page) const noexcept {
    if (page >= kPageCount) {
        return std::nullopt;
    }
    return contract_.guestToHost(
        GuestAddress32(page * static_cast<std::uint32_t>(kPageSize)),
        kPageSize);
}

std::optional<HostAddress64> Fex32GuestWindow::hostSpanAddress(
    std::uint32_t page) const noexcept {
    if (page >= kPageCount) {
        return std::nullopt;
    }
    const std::uint32_t firstPage =
        (page / guestPagesPerHostPage_) * guestPagesPerHostPage_;
    return pageHostAddress(firstPage);
}

bool Fex32GuestWindow::isCommittedUnlocked(std::uint32_t page) const noexcept {
    return page < kPageCount && (pageState_[page] & kCommitted) != 0;
}

std::uint8_t Fex32GuestWindow::pageAccessUnlocked(
    std::uint32_t page) const noexcept {
    return page < kPageCount
               ? static_cast<std::uint8_t>(pageState_[page] & kAccessMask)
               : Fex32WindowNone;
}

std::uint8_t Fex32GuestWindow::combinedHostAccessUnlocked(
    std::uint32_t page, std::uint8_t replacementState) const noexcept {
    const std::uint32_t firstPage =
        (page / guestPagesPerHostPage_) * guestPagesPerHostPage_;
    const std::uint32_t endPage = firstPage + guestPagesPerHostPage_;
    std::uint8_t combined = Fex32WindowNone;
    for (std::uint32_t candidate = firstPage; candidate < endPage;
         ++candidate) {
        const std::uint8_t state =
            candidate == page ? replacementState : pageState_[candidate];
        if ((state & kCommitted) != 0) {
            combined |= state & kAccessMask;
        }
    }
    return combined;
}

bool Fex32GuestWindow::hostSpanCommittedUnlocked(
    std::uint32_t page, std::uint8_t replacementState) const noexcept {
    const std::uint32_t firstPage =
        (page / guestPagesPerHostPage_) * guestPagesPerHostPage_;
    const std::uint32_t endPage = firstPage + guestPagesPerHostPage_;
    for (std::uint32_t candidate = firstPage; candidate < endPage;
         ++candidate) {
        const std::uint8_t state =
            candidate == page ? replacementState : pageState_[candidate];
        if ((state & kCommitted) != 0) {
            return true;
        }
    }
    return false;
}

bool Fex32GuestWindow::commitPage(std::uint32_t page,
                                  std::uint8_t access) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto hostAddress = hostSpanAddress(page);
    if (!hostAddress.has_value() || access == Fex32WindowNone ||
        !validAccess(access)) {
        return false;
    }

    const std::uint8_t replacement = kCommitted | access;
    const std::uint8_t hostAccess =
        combinedHostAccessUnlocked(page, replacement);
    const bool hostAlreadyCommitted =
        hostSpanCommittedUnlocked(page, pageState_[page]);
    const bool changed = hostAlreadyCommitted
                             ? memory_.protect(*hostAddress, hostPageSize_,
                                               hostAccess)
                             : memory_.commit(*hostAddress, hostPageSize_,
                                              hostAccess);
    if (!changed) {
        return false;
    }
    pageState_[page] = replacement;
    return true;
}

bool Fex32GuestWindow::protectPage(std::uint32_t page,
                                   std::uint8_t access) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto hostAddress = hostSpanAddress(page);
    if (!hostAddress.has_value() || !validAccess(access) ||
        !isCommittedUnlocked(page)) {
        return false;
    }
    const std::uint8_t replacement = kCommitted | access;
    const std::uint8_t hostAccess =
        combinedHostAccessUnlocked(page, replacement);
    if (!memory_.protect(*hostAddress, hostPageSize_, hostAccess)) {
        return false;
    }
    pageState_[page] = replacement;
    return true;
}

bool Fex32GuestWindow::decommitPage(std::uint32_t page) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto hostAddress = hostSpanAddress(page);
    if (!hostAddress.has_value() || !isCommittedUnlocked(page)) {
        return false;
    }
    const bool spanRemainsCommitted =
        hostSpanCommittedUnlocked(page, Fex32WindowNone);
    const std::uint8_t hostAccess =
        combinedHostAccessUnlocked(page, Fex32WindowNone);
    const bool changed = spanRemainsCommitted
                             ? memory_.protect(*hostAddress, hostPageSize_,
                                               hostAccess)
                             : memory_.decommit(*hostAddress, hostPageSize_);
    if (!changed) {
        return false;
    }
    pageState_[page] = Fex32WindowNone;
    return true;
}

bool Fex32GuestWindow::isCommitted(std::uint32_t page) const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return isCommittedUnlocked(page);
}

std::uint8_t Fex32GuestWindow::pageAccess(std::uint32_t page) const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return pageAccessUnlocked(page);
}

std::optional<HostAddress64> Fex32GuestWindow::translate(
    GuestAddress32 guestAddress, std::uint64_t length,
    std::uint8_t requiredAccess) const noexcept {
    if (!validAccess(requiredAccess) || requiredAccess == Fex32WindowNone ||
        !GuestAddressContract32::validGuestRange(guestAddress, length)) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    const std::uint64_t firstPage = guestAddress.value / kPageSize;
    const std::uint64_t lastByte =
        std::uint64_t{guestAddress.value} + length - 1;
    const std::uint64_t lastPage = lastByte / kPageSize;
    for (std::uint64_t page = firstPage; page <= lastPage; ++page) {
        const std::uint8_t state = pageState_[static_cast<std::size_t>(page)];
        if ((state & kCommitted) == 0 ||
            (state & requiredAccess) != requiredAccess) {
            return std::nullopt;
        }
    }
    return contract_.guestToHost(guestAddress, length);
}

std::optional<GuestAddress32> Fex32GuestWindow::hostToGuest(
    HostAddress64 hostAddress, std::uint64_t length) const noexcept {
    return contract_.hostToGuest(hostAddress, length);
}

}  // namespace boxedvn
