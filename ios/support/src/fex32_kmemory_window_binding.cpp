/*
 * BoxedVN - KMemory binding for the sparse FEX32 guest window.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/fex32_kmemory_window_binding.h"

#include <algorithm>

namespace boxedvn {

namespace {

constexpr std::uint8_t kAccessMask =
    Fex32KMemoryRead | Fex32KMemoryWrite | Fex32KMemoryExecute;

}  // namespace

Fex32WindowKMemoryBinding::Fex32WindowKMemoryBinding(
    Fex32GuestWindow& window)
    : window_(window), mapped_(Fex32GuestWindow::kPageCount, 0) {}

bool Fex32WindowKMemoryBinding::validRange(std::uint32_t firstPage,
                                           std::uint32_t pageCount) noexcept {
    return pageCount != 0 && firstPage < Fex32GuestWindow::kPageCount &&
           pageCount <= Fex32GuestWindow::kPageCount - firstPage;
}

bool Fex32WindowKMemoryBinding::validAccess(std::uint8_t access) noexcept {
    return (access & ~kAccessMask) == 0;
}

bool Fex32WindowKMemoryBinding::mapAnonymous(
    std::uint32_t firstPage, std::uint32_t pageCount,
    std::uint8_t access) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!healthy_ || !validRange(firstPage, pageCount) ||
        !validAccess(access)) {
        return false;
    }
    for (std::uint32_t page = firstPage; page < firstPage + pageCount;
         ++page) {
        if (mapped_[page]) return false;
    }

    std::uint32_t committed = 0;
    if (access != Fex32KMemoryNone) {
        for (; committed < pageCount; ++committed) {
            if (!window_.commitPage(firstPage + committed, access)) break;
        }
        if (committed != pageCount) {
            bool restored = true;
            while (committed != 0) {
                --committed;
                restored = window_.decommitPage(firstPage + committed) &&
                           restored;
            }
            healthy_ = restored;
            return false;
        }
    }

    std::fill(mapped_.begin() + firstPage,
              mapped_.begin() + firstPage + pageCount, std::uint8_t{1});
    return true;
}

bool Fex32WindowKMemoryBinding::restorePage(
    const PageSnapshot& snapshot) noexcept {
    const bool committed = window_.isCommitted(snapshot.page);
    if (!snapshot.committed) {
        return !committed || window_.decommitPage(snapshot.page);
    }
    if (!committed) {
        return snapshot.access != Fex32KMemoryNone &&
               window_.commitPage(snapshot.page, snapshot.access);
    }
    return window_.protectPage(snapshot.page, snapshot.access);
}

bool Fex32WindowKMemoryBinding::protectAnonymous(
    std::uint32_t firstPage, std::uint32_t pageCount,
    std::uint8_t access) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!healthy_ || !validRange(firstPage, pageCount) ||
        !validAccess(access)) {
        return false;
    }

    std::vector<PageSnapshot> snapshots;
    try {
        snapshots.reserve(pageCount);
        for (std::uint32_t page = firstPage; page < firstPage + pageCount;
             ++page) {
            if (!mapped_[page]) return false;
            snapshots.push_back(
                {page, window_.isCommitted(page), window_.pageAccess(page)});
        }
    } catch (...) {
        return false;
    }

    std::size_t applied = 0;
    for (; applied < snapshots.size(); ++applied) {
        const PageSnapshot& old = snapshots[applied];
        bool changed = true;
        if (old.committed) {
            changed = access == Fex32KMemoryNone
                          ? window_.decommitPage(old.page)
                          : window_.protectPage(old.page, access);
        } else if (access != Fex32KMemoryNone) {
            changed = window_.commitPage(old.page, access);
        }
        if (!changed) break;
    }
    if (applied == snapshots.size()) return true;

    bool restored = true;
    while (applied != 0) {
        --applied;
        restored = restorePage(snapshots[applied]) && restored;
    }
    healthy_ = restored;
    return false;
}

bool Fex32WindowKMemoryBinding::unmapAnonymousLocked(
    std::uint32_t firstPage, std::uint32_t pageCount) noexcept {
    if (!healthy_ || !validRange(firstPage, pageCount)) return false;

    std::vector<PageSnapshot> snapshots;
    try {
        snapshots.reserve(pageCount);
        for (std::uint32_t page = firstPage; page < firstPage + pageCount;
             ++page) {
            if (!mapped_[page]) continue;
            snapshots.push_back(
                {page, window_.isCommitted(page), window_.pageAccess(page)});
        }
    } catch (...) {
        return false;
    }

    std::size_t removed = snapshots.size();
    while (removed != 0) {
        const PageSnapshot& old = snapshots[removed - 1];
        if (old.committed && !window_.decommitPage(old.page)) break;
        --removed;
    }
    if (removed != 0) {
        bool restored = true;
        for (std::size_t index = removed; index < snapshots.size(); ++index) {
            restored = restorePage(snapshots[index]) && restored;
        }
        healthy_ = restored;
        return false;
    }

    for (const PageSnapshot& old : snapshots) mapped_[old.page] = 0;
    return true;
}

bool Fex32WindowKMemoryBinding::unmapAnonymous(
    std::uint32_t firstPage, std::uint32_t pageCount) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return unmapAnonymousLocked(firstPage, pageCount);
}

bool Fex32WindowKMemoryBinding::clear() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!healthy_) return false;

    std::uint32_t page = 0;
    while (page < Fex32GuestWindow::kPageCount) {
        while (page < Fex32GuestWindow::kPageCount && !mapped_[page]) ++page;
        if (page == Fex32GuestWindow::kPageCount) break;
        const std::uint32_t first = page;
        while (page < Fex32GuestWindow::kPageCount && mapped_[page]) ++page;
        if (!unmapAnonymousLocked(first, page - first)) return false;
    }
    return true;
}

bool Fex32WindowKMemoryBinding::isMapped(std::uint32_t page) const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return healthy_ && page < mapped_.size() && mapped_[page] != 0;
}

bool Fex32WindowKMemoryBinding::isCommitted(
    std::uint32_t page) const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return healthy_ && page < mapped_.size() && mapped_[page] != 0 &&
           window_.isCommitted(page);
}

std::uint8_t Fex32WindowKMemoryBinding::pageAccess(
    std::uint32_t page) const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return healthy_ && page < mapped_.size() && mapped_[page] != 0
               ? window_.pageAccess(page)
               : Fex32KMemoryNone;
}

std::optional<std::uintptr_t>
Fex32WindowKMemoryBinding::hostPageAddress(
    std::uint32_t page) const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!healthy_ || page >= mapped_.size() || !mapped_[page] ||
        !window_.isCommitted(page)) {
        return std::nullopt;
    }
    const std::uint64_t address =
        window_.hostBase().value +
        std::uint64_t{page} * Fex32GuestWindow::kPageSize;
    return static_cast<std::uintptr_t>(address);
}

bool Fex32WindowKMemoryBinding::healthy() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return healthy_;
}

}  // namespace boxedvn
