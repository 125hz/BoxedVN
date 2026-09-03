/*
 * BoxedWine x86-64 guest Vulkan bridge ABI.
 *
 * C-compatible and free of BoxedWine types: it is meant to be included both
 * by the native dispatcher (source/kernel/syscall64.cpp) and, when it is
 * written, by the 64-bit guest `libvulkan.so.1` shim, exactly as
 * include/boxedwine_x64_x11_bridge.h is shared with tools/x11-64.
 *
 * Why this exists
 * ---------------
 * The IA-32 lane reaches the host's Vulkan (MoltenVK on iOS) with `int 0x9A`
 * and stack arguments: the guest's 32-bit `/lib/libvulkan.so.1` traps, and
 * CPU decoders route it to `callVulkan()` in source/vulkan/vulkancommon.cpp,
 * which dispatches through `int9ACallback[]` into the generated host marshal
 * in source/vulkan/vk_host*.cpp.
 *
 * None of that is reachable from the 64-bit lane. CPU64 does not decode
 * `int 0x9A` as a host trap, and the shim in the root filesystem is an i386
 * ELF, so a 64-bit guest that dlopens `libvulkan.so.1` opens that file, has
 * it rejected for its ELF class, and continues its search. A device run of a
 * 32-bit Direct3D 9 program under WoW64 shows exactly that: seven candidate
 * paths tried, `/lib/libvulkan.so.1` opened, the search continuing past it,
 * and then the same story for `/lib/libGL.so.1`. wined3d therefore has
 * neither a Vulkan nor a GL adapter and `Direct3DCreate9` fails.
 *
 * So the 64-bit lane needs its own guest entry point. CPU64 already reserves
 * syscall numbers above the Linux table for host bridges (the DXMT unix call
 * and the X11 bridge); this is the third.
 *
 *   RAX = BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE
 *   RDI = operation index (BOXEDWINE_X64_VK_OP_*)
 *   RSI = guest pointer to an array of uint64_t arguments (may be 0 when
 *         RDX is 0)
 *   RDX = argument count, 0..BOXEDWINE_X64_VK_MAX_ARGS
 *   RAX = signed 64-bit result on return
 *
 * The argument array is IN/OUT, as in the X11 bridge: an operation that
 * produces a variable-size result writes the size it needs back into the
 * capacity slot and returns BOXEDWINE_X64_VK_E_BUFFER. Every guest pointer
 * the host reads or writes through is validated against the guest page table
 * first; the host never dereferences a guest address directly.
 *
 * Status: plumbing only. The operations below are the bootstrap set that
 * proves the trap reaches the host and reports what the host can do. No
 * Vulkan entry point is dispatched yet; see docs/PLAN_WOW64_D3D9.md for the
 * order the rest lands in and for what each step has to print.
 */
#ifndef BOXEDWINE_X64_VULKAN_BRIDGE_H
#define BOXEDWINE_X64_VULKAN_BRIDGE_H

#include <stdint.h>

/* Above the Linux x86-64 syscall table, beside the DXMT unix-call number
 * (0x7fff0001) and the X11 bridge (0x7fff0002). */
#define BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE 0x7fff0003ULL

/* Zero through fifteen arguments, as for the X11 bridge. A Vulkan entry point
 * with more parameters than this passes a guest pointer to a packed block. */
#define BOXEDWINE_X64_VK_MAX_ARGS 16U

/* Bumped whenever the meaning of an existing operation or of the argument
 * block changes. The guest shim reads it through BOXEDWINE_X64_VK_OP_ABI and
 * refuses to run against a host it does not understand, so a stale shim in a
 * container is a named refusal rather than a crash inside Metal. */
#define BOXEDWINE_X64_VK_ABI_VERSION 1U

/* Results below zero never collide with a VkResult (VK_SUCCESS is 0 and the
 * Vulkan error codes are negative but far from these), a count, or a handle.
 * They are deliberately the same shape and spelling as the X11 bridge's. */
#define BOXEDWINE_X64_VK_E_BADOP   (-1)  /* operation index outside the table */
#define BOXEDWINE_X64_VK_E_BUFFER  (-2)  /* capacity slot rewritten with the size needed */
#define BOXEDWINE_X64_VK_E_FAULT   (-3)  /* a guest pointer was not readable/writable */
#define BOXEDWINE_X64_VK_E_ARGS    (-4)  /* argument count outside 0..16 or too few */
#define BOXEDWINE_X64_VK_E_UNIMPL  (-5)  /* known operation, not implemented yet */
#define BOXEDWINE_X64_VK_E_NOHOST  (-6)  /* the host has no Vulkan implementation */
#define BOXEDWINE_X64_VK_E_MEMORY  (-7)  /* caller is not the identity-mapped process */

/* Operation table. The string is what diagnostics print; the index is ABI and
 * may only ever be appended to.
 *
 * The three below are the bootstrap set:
 *   ABI     - no arguments; returns BOXEDWINE_X64_VK_ABI_VERSION.
 *   PROBE   - no arguments; returns a bitmask of BOXEDWINE_X64_VK_CAP_*, so a
 *             shim can decline cleanly on a build with no host Vulkan rather
 *             than failing at the first real call.
 *   ECHO    - args[0] in, args[0] out incremented; the smallest possible
 *             proof that the guest argument array was read and written back
 *             through the page table.
 * Everything a real ICD needs (instance creation, physical-device
 * enumeration, the per-instance and per-device procedure tables, the surface
 * and swapchain calls) is appended after these, in the order
 * docs/PLAN_WOW64_D3D9.md sets out.
 */
#define BOXEDWINE_X64_VK_OPS(X) \
    X(ABI,   0, "abi") \
    X(PROBE, 1, "probe") \
    X(ECHO,  2, "echo")

#define BOXEDWINE_X64_VK_OP_ENUM(name, value, text) \
    BOXEDWINE_X64_VK_OP_##name = (value),
enum {
    BOXEDWINE_X64_VK_OPS(BOXEDWINE_X64_VK_OP_ENUM)
    BOXEDWINE_X64_VK_OP_COUNT
};
#undef BOXEDWINE_X64_VK_OP_ENUM

/* Capability bits returned by BOXEDWINE_X64_VK_OP_PROBE. */
/* The core was built with the host Vulkan marshal compiled in
 * (BOXEDWINE_VULKAN). Without this the bridge can only answer ABI and PROBE. */
#define BOXEDWINE_X64_VK_CAP_HOST_MARSHAL 0x1ULL
/* The calling process holds the identity map, i.e. it is the one process per
 * session that FEX translates. Host Vulkan handles are host pointers, so a
 * forked child with a sparse KMemory64 can never be served - the same
 * constraint the DXMT unix call already enforces and records in
 * docs/KNOWN_LIMITATIONS_IOS.md section 4. */
#define BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY 0x2ULL

/* The soname the guest loader looks for, and the only name Wine's
 * winevulkan.so dlopens (SONAME_LIBVULKAN). Kept here so the shim, the
 * packaging and the diagnostics cannot disagree about it. */
#define BOXEDWINE_X64_VK_GUEST_SONAME "libvulkan.so.1"

#if defined(__x86_64__) && !defined(BOXEDWINE_X64_VULKAN_BRIDGE_HOST)
/* Guest side. Independent of the guest libc: FEX executes the x86-64 SYSCALL
 * instruction and BoxedWine consumes the reserved number. */
static inline int64_t boxedwine_x64_vulkan_call(uint64_t op, uint64_t *args,
                                                uint64_t count)
{
    uint64_t number = BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE;
    uint64_t argsAddress = (uint64_t)(uintptr_t)args;
    __asm__ volatile("syscall"
                     : "+a"(number)
                     : "D"(op), "S"(argsAddress), "d"(count)
                     : "rcx", "r11", "memory");
    return (int64_t)number;
}
#endif

#endif /* BOXEDWINE_X64_VULKAN_BRIDGE_H */
