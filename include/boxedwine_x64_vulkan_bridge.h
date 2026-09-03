/*
 * BoxedWine x86-64 guest Vulkan bridge ABI.
 *
 * C-compatible and free of BoxedWine types: it is included both by the native
 * dispatcher (source/vulkan/vulkanbridge64.cpp, reached from
 * source/kernel/syscall64.cpp) and by the 64-bit guest `libvulkan.so.1` shim
 * in tools/vulkan-64, exactly as include/boxedwine_x64_x11_bridge.h is shared
 * with tools/x11-64.
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
 * capacity slot and returns BOXEDWINE_X64_VK_E_BUFFER. The *array itself* is
 * always validated against the guest page table before a slot is touched.
 *
 * Why the arguments are guest pointers and nothing is marshalled
 * -------------------------------------------------------------
 * The IA-32 marshal exists because a 32-bit guest pointer is not a host
 * address and a 32-bit guest's Vulkan structures are not laid out the way the
 * host's are. Neither is true here:
 *
 *   1. The bridge is served only to the one process per session that FEX
 *      translates, the process holding the identity map, where a guest
 *      address *is* the host address (BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY,
 *      the same rule the DXMT unix call enforces and
 *      docs/KNOWN_LIMITATIONS_IOS.md section 4 records).
 *   2. Every Vulkan structure has the same layout on x86-64 System V as on
 *      arm64 AAPCS64: both are LP64, every Vulkan scalar has its natural
 *      alignment on both, and Vulkan defines no bitfields or packed members.
 *
 * So the host dispatcher casts the argument words to the real Vulkan types
 * and calls MoltenVK with them. What it must never do is call *through* a
 * guest pointer: `pAllocator` is forced to NULL (which is what the IA-32
 * marshal does too) and any command whose parameters include a guest
 * callback is refused rather than dispatched.
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
#define BOXEDWINE_X64_VK_ABI_VERSION 2U

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
#define BOXEDWINE_X64_VK_E_NOPROC  (-8)  /* the host driver does not expose the command */

/* Bootstrap operations.
 *
 *   ABI       - no arguments; returns BOXEDWINE_X64_VK_ABI_VERSION.
 *   PROBE     - no arguments; returns a bitmask of BOXEDWINE_X64_VK_CAP_*, so
 *               a shim can decline cleanly on a build with no host Vulkan
 *               rather than failing at the first real call.
 *   ECHO      - args[0] in, args[0] out incremented; the smallest possible
 *               proof that the guest argument array was read and written back
 *               through the page table.
 *   PROC_ADDR - args[0] = VkInstance or VkDevice (0 for a global command),
 *               args[1] = guest pointer to the NUL-terminated command name,
 *               args[2] = 1 when the shim has a trampoline for that name and
 *                         0 when it does not.
 *               Returns 1 when the host driver exposes the command and the
 *               shim can hand out its trampoline, 0 otherwise. The host names
 *               every refusal in the log, which is how a run says what DXVK
 *               asked for that this bridge does not carry.
 */
#define BOXEDWINE_X64_VK_OP_ABI       0U
#define BOXEDWINE_X64_VK_OP_PROBE     1U
#define BOXEDWINE_X64_VK_OP_ECHO      2U
#define BOXEDWINE_X64_VK_OP_PROC_ADDR 3U
#define BOXEDWINE_X64_VK_OP_BOOTSTRAP_COUNT 4U

/* Vulkan commands start here. The index of a command is this base plus the
 * ordinal the IA-32 lane already gave it in source/vulkan/vkdef.h, so the two
 * lanes can never drift apart and a diagnostic that prints one number means
 * the same command on both. scripts/test_x64_vulkan_shim.py asserts every
 * ordinal below against vkdef.h. Append only. */
#define BOXEDWINE_X64_VK_OP_VK_BASE 0x1000U

/*
 * The commands this bridge carries, as (name, IA-32 ordinal) pairs.
 *
 * The set is the bootstrap half of the D3D9 chain: everything Wine's
 * winex11.drv binds by name out of the Vulkan library (its LOAD_FUNCPTR list
 * in dlls/winex11.drv/vulkan.c of Wine 9.0 -- sixteen required symbols and
 * four optional ones), plus the instance / physical-device / device / queue /
 * memory / image / sync calls DXVK walks before it records a command buffer.
 * The recording commands (vkCmd*, pipelines, descriptors, render passes,
 * command pools) are deliberately absent: a run that gets that far names each
 * one it wanted through BOXEDWINE_X64_VK_OP_PROC_ADDR, which is a cheaper way
 * to learn the real list than guessing it. docs/PLAN_WOW64_D3D9.md holds the
 * order they land in.
 *
 * Every command here takes only integers and pointers -- no float parameter,
 * no by-value structure, no guest callback -- which is what makes a typed
 * host-side call possible for each of them.
 */
#define BOXEDWINE_X64_VK_COMMANDS(X) \
    /* global */ \
    X(EnumerateInstanceVersion,                   14) \
    X(EnumerateInstanceLayerProperties,           15) \
    X(EnumerateInstanceExtensionProperties,       16) \
    X(CreateInstance,                              1) \
    X(DestroyInstance,                             2) \
    X(EnumeratePhysicalDevices,                    3) \
    /* physical device */ \
    X(GetPhysicalDeviceProperties,                 6) \
    X(GetPhysicalDeviceProperties2,              198) \
    X(GetPhysicalDeviceFeatures,                   9) \
    X(GetPhysicalDeviceFeatures2,                196) \
    X(GetPhysicalDeviceFormatProperties,          10) \
    X(GetPhysicalDeviceFormatProperties2,        200) \
    X(GetPhysicalDeviceImageFormatProperties,     11) \
    X(GetPhysicalDeviceImageFormatProperties2,   202) \
    X(GetPhysicalDeviceQueueFamilyProperties,      7) \
    X(GetPhysicalDeviceQueueFamilyProperties2,   204) \
    X(GetPhysicalDeviceMemoryProperties,           8) \
    X(GetPhysicalDeviceMemoryProperties2,        206) \
    X(EnumerateDeviceLayerProperties,             17) \
    X(EnumerateDeviceExtensionProperties,         18) \
    /* device and queue */ \
    X(CreateDevice,                               12) \
    X(DestroyDevice,                              13) \
    X(GetDeviceQueue,                             19) \
    X(DeviceWaitIdle,                             22) \
    X(QueueWaitIdle,                              21) \
    X(QueueSubmit,                                20) \
    /* memory */ \
    X(AllocateMemory,                             23) \
    X(FreeMemory,                                 24) \
    X(MapMemory,                                  25) \
    X(UnmapMemory,                                26) \
    X(FlushMappedMemoryRanges,                    27) \
    X(InvalidateMappedMemoryRanges,               28) \
    X(GetBufferMemoryRequirements,                30) \
    X(BindBufferMemory,                           31) \
    X(GetImageMemoryRequirements,                 32) \
    X(BindImageMemory,                            33) \
    X(GetMemoryHostPointerPropertiesEXT,         302) \
    /* buffers, images, views */ \
    X(CreateBuffer,                               54) \
    X(DestroyBuffer,                              55) \
    X(CreateImage,                                58) \
    X(DestroyImage,                               59) \
    X(CreateImageView,                            61) \
    X(DestroyImageView,                           62) \
    /* synchronisation */ \
    X(CreateFence,                                37) \
    X(DestroyFence,                               38) \
    X(ResetFences,                                39) \
    X(GetFenceStatus,                             40) \
    X(WaitForFences,                              41) \
    X(CreateSemaphore,                            42) \
    X(DestroySemaphore,                           43) \
    /* surface and swapchain */ \
    X(DestroySurfaceKHR,                         161) \
    X(GetPhysicalDeviceSurfaceSupportKHR,        162) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR,   163) \
    X(GetPhysicalDeviceSurfaceFormatsKHR,        164) \
    X(GetPhysicalDeviceSurfacePresentModesKHR,   165) \
    X(GetPhysicalDeviceSurfaceCapabilities2KHR,  257) \
    X(GetPhysicalDeviceSurfaceFormats2KHR,       258) \
    X(GetPhysicalDevicePresentRectanglesKHR,     241) \
    X(GetDeviceGroupSurfacePresentModesKHR,      237) \
    X(CreateSwapchainKHR,                        166) \
    X(DestroySwapchainKHR,                       167) \
    X(GetSwapchainImagesKHR,                     168) \
    X(AcquireNextImageKHR,                       169) \
    X(QueuePresentKHR,                           170) \
    X(CreateXlibSurfaceKHR,                      171) \
    X(GetPhysicalDeviceXlibPresentationSupportKHR, 172)

/*
 * Alternate spellings that resolve to the same operation.
 *
 * Every command here was promoted from a KHR extension into Vulkan 1.1, and
 * both spellings stayed valid. Which one a caller asks for depends on the API
 * version it created the instance with, not on what it wants: DXVK queries
 * `vkGetPhysicalDeviceProperties2KHR` and
 * `vkGetPhysicalDeviceMemoryProperties2KHR` on a 1.0 instance, and Wine's
 * winex11.drv names both as well. A lookup that only matched the core
 * spelling would hand back NULL and the caller would conclude the driver has
 * no `VK_KHR_get_physical_device_properties2` -- which is how DXVK decides it
 * cannot use a device at all.
 *
 * So the guest ICD exports the alias under the core command's own entry point
 * (the trap carries the operation number, not the name), and the host tries
 * the core spelling against the driver first and the KHR spelling second,
 * because MoltenVK may expose only one of the two depending on the instance
 * version. First is (alias suffix name, core command name).
 */
#define BOXEDWINE_X64_VK_ALIASES(X) \
    X(GetPhysicalDeviceFeatures2KHR,              GetPhysicalDeviceFeatures2) \
    X(GetPhysicalDeviceProperties2KHR,            GetPhysicalDeviceProperties2) \
    X(GetPhysicalDeviceFormatProperties2KHR,      GetPhysicalDeviceFormatProperties2) \
    X(GetPhysicalDeviceImageFormatProperties2KHR, GetPhysicalDeviceImageFormatProperties2) \
    X(GetPhysicalDeviceQueueFamilyProperties2KHR, GetPhysicalDeviceQueueFamilyProperties2) \
    X(GetPhysicalDeviceMemoryProperties2KHR,      GetPhysicalDeviceMemoryProperties2)

#define BOXEDWINE_X64_VK_OP_ENUM(name, ordinal) \
    BOXEDWINE_X64_VK_OP_##name = (BOXEDWINE_X64_VK_OP_VK_BASE + (ordinal)),
enum {
    BOXEDWINE_X64_VK_COMMANDS(BOXEDWINE_X64_VK_OP_ENUM)
    BOXEDWINE_X64_VK_OP_SENTINEL
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
/* The host loaded a real Vulkan driver and resolved vkGetInstanceProcAddr.
 * Distinct from CAP_HOST_MARSHAL, which only says the code is compiled in. */
#define BOXEDWINE_X64_VK_CAP_DRIVER 0x4ULL

/* The soname the guest loader looks for, and the only name Wine's
 * winex11.drv dlopens (SONAME_LIBVULKAN). Kept here so the shim, the
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
