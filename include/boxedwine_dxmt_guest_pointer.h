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
 * One host pointer crosses into the guest and comes back: DXMT's shader
 * translator hands the guest a host pointer to its compiled bitcode and the
 * guest passes it straight back to create a Metal library. It cannot be
 * told from a guest address by value (the guest's low range runs to 8 GiB,
 * where host allocations also live), so the thunk that returns it sets a
 * tag bit no guest address carries, and the translation strips the tag and
 * returns the host pointer unchanged.
 */
#ifndef BOXEDWINE_DXMT_GUEST_POINTER_H
#define BOXEDWINE_DXMT_GUEST_POINTER_H

#include <stdint.h>

#define BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE 0x7800000000ULL
#define BOXEDWINE_DXMT_GUEST_TOP_CLEAR_MASK 0x7F8000000000ULL
/* Set on a host pointer handed to the guest so it is recognised when the
 * guest passes it back. Guest addresses are canonical and below 2^47. */
#define BOXEDWINE_DXMT_HOST_POINTER_TAG 0x4000000000000000ULL

static inline uint64_t boxedwine_dxmt_tag_host_pointer(uint64_t host)
{
    return host ? (host | BOXEDWINE_DXMT_HOST_POINTER_TAG) : 0;
}

static inline uintptr_t boxedwine_dxmt_host_pointer(uintptr_t guest)
{
    if (!guest) {
        return 0;
    }
    if ((uint64_t)guest & BOXEDWINE_DXMT_HOST_POINTER_TAG) {
        return (uintptr_t)((uint64_t)guest & ~BOXEDWINE_DXMT_HOST_POINTER_TAG);
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
/* Tags the Data pointer of a struct SM50_COMPILED_BITCODE the translator
 * just filled in, so the guest can hand it back through any translated
 * site. Same translation unit as the chain copy. */
void boxedwine_dxmt_tag_compiled_bitcode(void* compiled_bitcode);
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
