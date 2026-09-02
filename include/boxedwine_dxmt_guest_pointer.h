/*
 * BoxedWine - guest pointer translation for DXMT's native unix side.
 *
 * C-compatible: this header is force-included into DXMT's winemetal unix
 * translation units when they are compiled into the iOS host (see
 * scripts/build-dxmt-ios-native.sh), and mirrored by the C++ test suite.
 *
 * The x86-64 guest is dereferenced by the translator through one address
 * rule (include/guest_low_alias.h):
 *
 *   host = (guest | 0x7800000000) & ~0x7F8000000000
 *
 * which is the identity for the high lane where the guest heap and images
 * live, an alias for the canonical low 2 GiB, and a relocation for Wine's
 * top-down arena at 0x7ffffe000000, where Wine places its thread stacks.
 * DXMT's PE side builds every unix-call parameter block on the calling
 * thread's stack and points at stack-resident descriptors from it, and the
 * unix side reads those pointers directly. A device run showed every call
 * refused with EFAULT because the block was at 0x7ffffe1ff4e8: mapped, but
 * not at its own address on the host.
 *
 * The dispatcher translates the parameter block itself; this macro is what
 * the rewritten unix sources apply to every nested guest pointer they read.
 * A null pointer stays null. Host object handles are never passed through
 * it: they are host pointers already.
 */
#ifndef BOXEDWINE_DXMT_GUEST_POINTER_H
#define BOXEDWINE_DXMT_GUEST_POINTER_H

#include <stdint.h>

#define BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE 0x7800000000ULL
#define BOXEDWINE_DXMT_GUEST_TOP_CLEAR_MASK 0x7F8000000000ULL

static inline uintptr_t boxedwine_dxmt_host_pointer(uintptr_t guest)
{
    if (!guest) {
        return 0;
    }
    return (uintptr_t)(((uint64_t)guest | BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE) &
                       ~BOXEDWINE_DXMT_GUEST_TOP_CLEAR_MASK);
}

/* Keeps the pointer's own type: works for void*, const void*, char* and the
 * raw uint64_t fields DXMT casts itself. C++ (the host test suite) spells the
 * type query differently from the C the unix sources are compiled as. */
#if defined(__cplusplus)
#define BOXEDWINE_GUEST_PTR(p) \
    ((decltype(p))boxedwine_dxmt_host_pointer((uintptr_t)(p)))
#else
#define BOXEDWINE_GUEST_PTR(p) \
    ((__typeof__(p))boxedwine_dxmt_host_pointer((uintptr_t)(p)))
#endif

#endif /* BOXEDWINE_DXMT_GUEST_POINTER_H */
