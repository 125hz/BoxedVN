/*
 * BoxedWine - decoding policy for the x86-64 guest X11 bridge call.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * The private syscall carries an operation index, a guest pointer to an
 * array of 64-bit arguments and a count. Everything that decides whether a
 * call is admitted lives here, against an abstract page probe, so the rules
 * can be tested on any host:
 *
 *   - the operation index must name an entry of the table;
 *   - the count is 0..16, and an operation that needs N arguments refuses a
 *     shorter array rather than reading past it;
 *   - the argument array must be canonical, non-null when the count is not
 *     zero, and every page it spans must be mapped and readable; it is also
 *     written back, so those pages must be writable too;
 *   - arguments are copied out as 64-bit values and never truncated.
 *
 * A pointer that fails the probe is reported as a controlled fault, not
 * dereferenced.
 */

#ifndef __X11_BRIDGE64_POLICY_H__
#define __X11_BRIDGE64_POLICY_H__

#include <cstdint>

#define BOXEDWINE_X64_X11_BRIDGE_HOST 1
#include "boxedwine_x64_x11_bridge.h"

namespace boxedvn {

// The highest canonical user address on x86-64 (47-bit).
constexpr uint64_t kX11Bridge64CanonicalLimit = 0x0000800000000000ULL;
constexpr uint64_t kX11Bridge64PageSize = 4096;

// What the bridge asks of the guest page table. Implemented over KMemory64
// in the emulator and over a fake in tests.
class X11Bridge64PageProbe {
public:
    virtual ~X11Bridge64PageProbe() = default;
    // True when the page holding `address` is mapped and readable, and
    // writable too when `write` is set.
    virtual bool accessible(uint64_t address, bool write) const = 0;
};

// True when [address, address+length) is canonical and every page in it
// passes the probe. A zero length is always accessible; a null address
// never is.
inline bool x11Bridge64RangeAccessible(const X11Bridge64PageProbe& probe,
                                       uint64_t address, uint64_t length,
                                       bool write) {
    if (length == 0) {
        return true;
    }
    if (address == 0) {
        return false;
    }
    if (address >= kX11Bridge64CanonicalLimit ||
        length > kX11Bridge64CanonicalLimit - address) {
        return false;
    }
    const uint64_t first = address & ~(kX11Bridge64PageSize - 1);
    const uint64_t last = (address + length - 1) & ~(kX11Bridge64PageSize - 1);
    for (uint64_t page = first;; page += kX11Bridge64PageSize) {
        if (!probe.accessible(page, write)) {
            return false;
        }
        if (page == last) {
            break;
        }
    }
    return true;
}

enum class X11Bridge64Admission {
    Admitted,
    UnknownOperation,
    BadArgumentCount,
    ArgumentArrayFault,
};

struct X11Bridge64Call {
    uint64_t op = 0;
    uint64_t argsAddress = 0;
    uint64_t count = 0;
    uint64_t args[BOXEDWINE_X64_X11_MAX_ARGS] = {};
    X11Bridge64Admission admission = X11Bridge64Admission::UnknownOperation;
};

inline const char* x11Bridge64OpName(uint64_t op) {
    switch (op) {
#define BOXEDWINE_X64_X11_OP_NAME(name, text) \
        case BOXEDWINE_X64_X11_OP_##name: return text;
        BOXEDWINE_X64_X11_OPS(BOXEDWINE_X64_X11_OP_NAME)
#undef BOXEDWINE_X64_X11_OP_NAME
        default: return nullptr;
    }
}

inline bool x11Bridge64KnownOp(uint64_t op) {
    return op < BOXEDWINE_X64_X11_OP_COUNT;
}

// Decide admission and, when admitted, copy the argument array out. The
// argument pages must be writable because results are written back into
// the array.
template <typename ReadArguments>
inline X11Bridge64Call x11Bridge64Decode(const X11Bridge64PageProbe& probe,
                                         uint64_t op, uint64_t argsAddress,
                                         uint64_t count,
                                         ReadArguments&& readArguments) {
    X11Bridge64Call call;
    call.op = op;
    call.argsAddress = argsAddress;
    call.count = count;
    if (!x11Bridge64KnownOp(op)) {
        call.admission = X11Bridge64Admission::UnknownOperation;
        return call;
    }
    if (count > BOXEDWINE_X64_X11_MAX_ARGS) {
        call.admission = X11Bridge64Admission::BadArgumentCount;
        return call;
    }
    if (count != 0) {
        if (!x11Bridge64RangeAccessible(probe, argsAddress,
                                        count * sizeof(uint64_t), true)) {
            call.admission = X11Bridge64Admission::ArgumentArrayFault;
            return call;
        }
        readArguments(argsAddress, call.args, count);
    }
    call.admission = X11Bridge64Admission::Admitted;
    return call;
}

// Maps an admission decision to the ABI's error code. Admitted calls carry
// no error and the caller dispatches them.
inline int64_t x11Bridge64AdmissionResult(X11Bridge64Admission admission) {
    switch (admission) {
        case X11Bridge64Admission::Admitted: return 0;
        case X11Bridge64Admission::UnknownOperation: return BOXEDWINE_X64_X11_E_BADOP;
        case X11Bridge64Admission::BadArgumentCount: return BOXEDWINE_X64_X11_E_ARGS;
        case X11Bridge64Admission::ArgumentArrayFault: return BOXEDWINE_X64_X11_E_FAULT;
    }
    return BOXEDWINE_X64_X11_E_BADOP;
}

// The private numbers must stay clear of each other and of the Linux table.
static_assert(BOXEDWINE_X64_HOSTCALL_X11_BRIDGE != 0x7fff0001ULL,
              "the X11 bridge and the DXMT unix call must use distinct numbers");
static_assert(BOXEDWINE_X64_HOSTCALL_X11_BRIDGE > 1024,
              "the X11 bridge number must sit above the Linux x86-64 table");

} // namespace boxedvn

#endif
