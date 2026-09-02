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
 * A null pointer stays null.
 *
 * Host pointers pass through unchanged. The guest occupies three ranges
 * (below 4 GiB, the identity lane from 0x7800000000, and the top arena above
 * the clear mask) and the host's own allocations sit between 4 GiB and the
 * identity lane, where no guest address can be. That matters because DXMT's
 * shader translator hands the guest a host pointer to its compiled bitcode
 * and the guest passes it straight back to create a Metal library: a
 * translation that treated it as a guest address would corrupt it.
 */
#ifndef BOXEDWINE_DXMT_GUEST_POINTER_H
#define BOXEDWINE_DXMT_GUEST_POINTER_H

#include <stdint.h>

#define BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE 0x7800000000ULL
#define BOXEDWINE_DXMT_GUEST_TOP_CLEAR_MASK 0x7F8000000000ULL
/* Guest addresses below this are the canonical low range (aliased high);
 * host allocations never sit below it on the arm64 iOS process layout. */
#define BOXEDWINE_DXMT_GUEST_LOW_END 0x100000000ULL

static inline int boxedwine_dxmt_is_host_pointer(uint64_t address)
{
    return address >= BOXEDWINE_DXMT_GUEST_LOW_END &&
           address < BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE;
}

static inline uintptr_t boxedwine_dxmt_host_pointer(uintptr_t guest)
{
    if (!guest) {
        return 0;
    }
    if (boxedwine_dxmt_is_host_pointer((uint64_t)guest)) {
        return guest;
    }
    return (uintptr_t)(((uint64_t)guest | BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE) &
                       ~BOXEDWINE_DXMT_GUEST_TOP_CLEAR_MASK);
}

/*
 * DXMT's shader translator receives a linked chain of compilation-argument
 * structures whose `next` links and `elements` arrays are guest pointers,
 * and walks them natively. This returns a host-side copy of the chain with
 * every nested pointer translated; the copy lives in thread-local storage
 * and is valid until the calling thread's next chain. Implemented in
 * tools/dxmt/boxedwine_dxmt_sm50_arguments.c, compiled beside the unix side.
 */
#if defined(__cplusplus)
extern "C" {
#endif
const void* boxedwine_dxmt_sm50_arguments(const void* guest_chain);
#if defined(__cplusplus)
}
#endif

/* Keeps the pointer's own type: works for void*, const void*, char* and the
 * raw uint64_t fields DXMT casts itself. C++ (the host test suite) spells the
 * type query differently from the C the unix sources are compiled as. */
#if defined(__cplusplus)
#define BOXEDWINE_GUEST_PTR(p) \
    ((decltype(p))boxedwine_dxmt_host_pointer((uintptr_t)(p)))
#define BOXEDWINE_SM50_ARGS(p) \
    ((decltype(p))boxedwine_dxmt_sm50_arguments((const void*)(p)))
#else
#define BOXEDWINE_GUEST_PTR(p) \
    ((__typeof__(p))boxedwine_dxmt_host_pointer((uintptr_t)(p)))
#define BOXEDWINE_SM50_ARGS(p) \
    ((__typeof__(p))boxedwine_dxmt_sm50_arguments((const void*)(p)))
#endif

#endif /* BOXEDWINE_DXMT_GUEST_POINTER_H */
