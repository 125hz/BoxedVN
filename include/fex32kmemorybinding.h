/*
 * BoxedVN - optional direct-memory contract for a 32-bit translator.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX32_KMEMORY_BINDING_H
#define BOXEDVN_FEX32_KMEMORY_BINDING_H

#include <cstdint>
#include <optional>

namespace boxedvn {

enum Fex32KMemoryAccess : std::uint8_t {
    Fex32KMemoryNone = 0,
    Fex32KMemoryRead = 1u << 0,
    Fex32KMemoryWrite = 1u << 1,
    Fex32KMemoryExecute = 1u << 2,
};

// KMemory borrows this object. The platform/runtime owner must keep it alive
// until the KMemory instance has been destroyed. Existing BoxedWine backends
// pass no binding and retain the soft-MMU implementation unchanged.
class Fex32KMemoryBinding {
public:
    virtual ~Fex32KMemoryBinding() = default;

    virtual bool mapAnonymous(std::uint32_t firstPage,
                              std::uint32_t pageCount,
                              std::uint8_t access) noexcept = 0;
    virtual bool protectAnonymous(std::uint32_t firstPage,
                                  std::uint32_t pageCount,
                                  std::uint8_t access) noexcept = 0;
    // Linux munmap accepts holes. Implementations remove every mapped page in
    // the range and treat already-unmapped pages as a no-op.
    virtual bool unmapAnonymous(std::uint32_t firstPage,
                                std::uint32_t pageCount) noexcept = 0;
    virtual bool clear() noexcept = 0;

    virtual bool isMapped(std::uint32_t page) const noexcept = 0;
    virtual bool isCommitted(std::uint32_t page) const noexcept = 0;
    virtual std::uint8_t pageAccess(std::uint32_t page) const noexcept = 0;
    virtual std::optional<std::uintptr_t> hostPageAddress(
        std::uint32_t page) const noexcept = 0;
    virtual bool healthy() const noexcept = 0;
};

}  // namespace boxedvn

#endif  // BOXEDVN_FEX32_KMEMORY_BINDING_H
