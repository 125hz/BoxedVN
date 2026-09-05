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
 * Why the arguments are raw guest pointers and the host marshals them
 * -------------------------------------------------------------------
 * The IA-32 marshal exists for two reasons: a 32-bit guest pointer is not a
 * host address, and a 32-bit guest's Vulkan structures are not laid out the
 * way the host's are. Only the second of those is gone here. Every Vulkan
 * structure has the same layout on x86-64 System V as on arm64 AAPCS64: both
 * are LP64, every Vulkan scalar has its natural alignment on both, and Vulkan
 * defines no bitfields or packed members. So no field ever moves, and the
 * argument words are the guest's own pointers, unwidened and unrepacked.
 *
 * The first reason has NOT gone away, and an early device run proved it. The
 * lane is served only to the process holding the native map
 * (BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY, the same rule the DXMT unix call
 * enforces), but that map is identity only for the high lane: canonical low
 * addresses are served through an alias and Wine's top-down arena -- where
 * every thread stack lives, and therefore where DXVK builds its create-infos
 * -- is served from a relocated host block. A guest pointer handed straight
 * to MoltenVK is a host address only by luck. It was not: MoltenVK followed
 * a VkInstanceCreateInfo at a 0x7ffffe... stack address into unmapped host
 * memory and the process died inside vkCreateInstance.
 *
 * So the host dispatcher marshals. Every structure a command names is copied
 * into a host-side shadow through the guest page table, every pointer inside
 * it (pNext chains, pApplicationInfo, ppEnabled*Names, pQueueCreateInfos,
 * pEnabledFeatures, the arrays in VkSubmitInfo and VkPresentInfoKHR, ...) is
 * marshalled the same way, and the results are copied back into guest memory
 * afterwards. A pointer the page table cannot account for is a named
 * VK_ERROR_INITIALIZATION_FAILED and a `status=bad-pointer` log line rather
 * than a fault inside Metal. The guest ICD is unchanged by any of this: it
 * still packs raw guest pointers, which is what keeps it free of Vulkan
 * headers.
 *
 * What the host must never do is call *through* a guest pointer. `pAllocator`
 * is forced to NULL (which is what the IA-32 marshal does too), a command
 * whose parameters include a guest callback is not in the table, and a pNext
 * node that carries one (VkDebugUtilsMessengerCreateInfoEXT, which DXVK
 * attaches to its VkInstanceCreateInfo whenever validation is on) is dropped
 * from the chain rather than forwarded.
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
/* 3: the host marshals every structure that crosses the bridge instead of
 *    handing the guest's own pointers to the driver, and the command table
 *    grew the memory-requirements-2 / bind-memory-2 / external-object /
 *    timeline-semaphore group. The wire format did not change -- a shim built
 *    for 2 would still be understood -- but such a shim has no entry point for
 *    the new commands and would answer NULL for them, which a caller reads as
 *    "the driver cannot do this" rather than as a stale container. */
/* 4: BOXEDWINE_X64_VK_E_* moved below INT32_MIN so a bridge refusal can never
 *    be read as a VkResult, in a log or by a caller. A shim built for 3
 *    truncates the new codes to 32 bits and hands the application whatever
 *    falls out, so the version gates the two apart rather than letting them
 *    mix. */
/* 5: the command-recording half. Command pools and buffers, events, query
 *    pools, buffer views, samplers, shader modules, pipeline caches, pipeline
 *    layouts, descriptor set layouts / pools / sets / update templates, render
 *    passes in both the 1.0 and the create_renderpass2 form, framebuffers,
 *    graphics and compute pipelines, and the vkCmd* family DXVK emits. A shim
 *    built for 4 has no entry point for any of them and would answer NULL,
 *    which a caller reads as a driver that cannot record a command buffer.
 *    The wire format gained one convention as well: a float parameter crosses
 *    as the bit pattern of its IEEE-754 binary32 value in the low 32 bits of
 *    an argument word (see the note above BOXEDWINE_X64_VK_COMMANDS), which a
 *    shim built for 4 does not know how to pack. */
/* 6: the core-completeness pass. A device run got through instance and device
 *    creation and then died on a 32-bit indirect call through a mapped
 *    dispatch table whose entry was zero. Wine's winevulkan fills one slot per
 *    command by name out of vkGetDeviceProcAddr; a name this bridge cannot
 *    resolve leaves that slot null, and a caller that does not check calls
 *    through the hole. So the table was audited against every command core
 *    Vulkan 1.0 through 1.4 defines, and the ones that can be marshalled
 *    correctly were added -- whether or not the D3D9 path calls them, because
 *    the slot exists either way.
 *
 *    The wire format gained one convention with it: a command that returns a
 *    64-bit value rather than a VkResult (vkGetBufferDeviceAddress and the two
 *    opaque-capture-address queries) cannot return it in RAX, because a bridge
 *    refusal is told from an answer by the top half of that word and a
 *    legitimate device address may have those bits set. The guest shim passes
 *    the address of a uint64_t on its own stack as one extra argument word
 *    instead, and the host writes the answer there. A shim built for 5 has no
 *    entry point for any of the new commands and does not know the convention,
 *    which is the whole reason this is a version and not an addition. */
/* 7: the surface / swapchain maintenance1 pass. A device run reached a
 *    swapchain and then never presented; the bridge was dropping every
 *    VK_EXT_surface_maintenance1 and VK_EXT_swapchain_maintenance1 node DXVK
 *    chained onto the capability query, the swapchain create info and the
 *    present. Those structures are now carried, and vkReleaseSwapchainImagesEXT
 *    with them -- one more command, so one more version. */
#define BOXEDWINE_X64_VK_ABI_VERSION 7U

/*
 * The bridge's own refusals, as distinct from a VkResult.
 *
 * These used to be -1 through -8, on the claim that the Vulkan error codes
 * were "far from these". That claim was simply wrong: the core VkResult errors
 * run -1 to -13, so every single bridge refusal had a VkResult sitting on top
 * of it. It cost real time. A device log is the only diagnostic channel this
 * lane has, and a run that printed `status=-7` for a bridge refusal was
 * indistinguishable from one printing VK_ERROR_EXTENSION_NOT_PRESENT, while
 * `status=-8` read as VK_ERROR_FEATURE_NOT_PRESENT. Two separate
 * investigations began by reading a bridge code as a driver answer.
 *
 * So they now live below every value a VkResult can hold. VkResult is a 32-bit
 * signed enum -- the lowest one any Vulkan header has defined is around
 * -1000483000, and the extension numbering scheme cannot reach INT32_MIN --
 * while this hostcall returns a 64-bit word in RAX. A result below INT32_MIN
 * therefore cannot be a VkResult under any present or future header, and the
 * test in scripts/test_x64_vulkan_shim.py pins exactly that.
 *
 * The historical ordinal is preserved as the offset from the base, so
 * BOXEDWINE_X64_VK_E_BASE minus a code still gives the number an old log
 * printed: -4294967303 is offset 7, which is what -7 used to mean.
 */
#define BOXEDWINE_X64_VK_E_BASE    (-0x100000000LL)  /* -4294967296 */
#define BOXEDWINE_X64_VK_E_BADOP   (BOXEDWINE_X64_VK_E_BASE - 1)  /* operation index outside the table */
#define BOXEDWINE_X64_VK_E_BUFFER  (BOXEDWINE_X64_VK_E_BASE - 2)  /* capacity slot rewritten with the size needed */
#define BOXEDWINE_X64_VK_E_FAULT   (BOXEDWINE_X64_VK_E_BASE - 3)  /* a guest pointer was not readable/writable */
#define BOXEDWINE_X64_VK_E_ARGS    (BOXEDWINE_X64_VK_E_BASE - 4)  /* argument count outside 0..16 or too few */
#define BOXEDWINE_X64_VK_E_UNIMPL  (BOXEDWINE_X64_VK_E_BASE - 5)  /* known operation, not implemented yet */
#define BOXEDWINE_X64_VK_E_NOHOST  (BOXEDWINE_X64_VK_E_BASE - 6)  /* the host has no Vulkan implementation */
#define BOXEDWINE_X64_VK_E_MEMORY  (BOXEDWINE_X64_VK_E_BASE - 7)  /* caller is not the identity-mapped process */
#define BOXEDWINE_X64_VK_E_NOPROC  (BOXEDWINE_X64_VK_E_BASE - 8)  /* the host driver does not expose the command */

/* True for any bridge refusal and false for every VkResult. The guest shim
 * uses it to keep a refusal from reaching an application as a VkResult. */
#define BOXEDWINE_X64_VK_IS_ERROR(result) ((int64_t)(result) <= BOXEDWINE_X64_VK_E_BASE)

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
 * memory / image / sync calls DXVK walks before it records a command buffer:
 * the "2" forms of the memory-requirements and bind-memory calls its
 * allocator uses, the external-object capability queries it runs before it
 * picks a memory type, and the timeline-semaphore trio -- plus, since ABI 5,
 * the recording half: command pools and buffers, events and query pools, the
 * resource and descriptor objects a pipeline is built out of, render passes in
 * both forms, graphics and compute pipelines, and the vkCmd* family DXVK
 * emits for a frame.
 *
 * Since ABI 6 the table also carries every command core Vulkan 1.0 through 1.4
 * defines that can be marshalled correctly, whether or not the D3D9 path calls
 * it. That is not completeness for its own sake: winevulkan fills one dispatch
 * slot per command by name, so a core command this bridge cannot resolve
 * leaves a null in a table a caller may walk without checking. Only three core
 * commands are refused, and each is refused because the marshal cannot be
 * proven rather than because nothing calls it:
 *
 *   - vkCopyMemoryToImage, vkCopyImageToMemory and vkCopyImageToImage
 *     (VK_VERSION_1_4, host image copy). VkMemoryToImageCopy and
 *     VkImageToMemoryCopy carry a `pHostPointer` the driver reads or writes
 *     directly, whose length is the byte footprint of an image region: it
 *     depends on the format's texel block size and on memoryRowLength and
 *     memoryImageHeight, and nothing in the call states it. Sizing it would
 *     mean carrying a format table, and getting it wrong is a read or a write
 *     past the guest's own allocation -- the exact fault this bridge exists to
 *     prevent. They resolve to NULL and the host prints `kind=core-miss`.
 *
 * Extension commands are absent in bulk and that is expected: a caller only
 * reaches an extension's dispatch slot after enabling the extension, so a null
 * there is never dereferenced. The two kinds are told apart in the log by
 * `kind=core-miss` versus `kind=extension-miss`, which is what makes the
 * dangerous half of a run's proc-address misses greppable.
 *
 * Still absent among the extensions, with the reason, because each was a
 * deliberate call rather than an oversight:
 *
 *   - vkCmdSetVertexInputEXT and the VK_EXT_extended_dynamic_state3 setters:
 *     DXVK emits them only when the matching feature bit is set, and MoltenVK
 *     does not set it.
 *   - the acceleration-structure, micromap, video and performance-query
 *     families: no D3D9 path reaches them, and none is core.
 *
 * A run that wants one of these names it through BOXEDWINE_X64_VK_OP_PROC_ADDR
 * and the host prints it, which is still the cheapest way to learn what is
 * really needed. docs/PLAN_WOW64_D3D9.md holds the order they land in.
 *
 * Every command here takes only integers, pointers and -- since ABI 5 --
 * floats; no by-value structure and no guest callback, which is what makes a
 * typed host-side call possible for each of them. Exactly three commands take
 * a float by value -- vkCmdSetLineWidth, vkCmdSetDepthBias and
 * vkCmdSetDepthBounds -- and each such parameter crosses as the bit pattern of
 * its IEEE-754 binary32 value in the low 32 bits of an argument word. Both
 * sides are IEEE-754 binary32 with the same endianness, so the round trip is
 * exact; passing the value through a C conversion to uint64_t instead would
 * round it to an integer, and a line width of 1.5 would arrive as 1.
 * vkCmdSetBlendConstants' four floats are an array, so they cross as a pointer
 * and are marshalled like any other block.
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
    X(GetDeviceQueue2,                           279) \
    X(DeviceWaitIdle,                             22) \
    X(QueueWaitIdle,                              21) \
    X(WaitForPresentKHR,                         570) \
    X(QueueSubmit,                                20) \
    /* memory */ \
    X(AllocateMemory,                             23) \
    X(FreeMemory,                                 24) \
    X(MapMemory,                                  25) \
    X(UnmapMemory,                                26) \
    X(FlushMappedMemoryRanges,                    27) \
    X(InvalidateMappedMemoryRanges,               28) \
    X(GetBufferMemoryRequirements,                30) \
    X(GetBufferMemoryRequirements2,              263) \
    X(BindBufferMemory,                           31) \
    X(BindBufferMemory2,                         230) \
    X(GetImageMemoryRequirements,                 32) \
    X(GetImageMemoryRequirements2,               265) \
    X(BindImageMemory,                            33) \
    X(BindImageMemory2,                          232) \
    X(GetMemoryHostPointerPropertiesEXT,         302) \
    /* external object capabilities, which DXVK queries before it picks a
     * memory type or a sharing mode */ \
    X(GetPhysicalDeviceExternalBufferProperties, 214) \
    X(GetPhysicalDeviceExternalSemaphoreProperties, 216) \
    X(GetPhysicalDeviceExternalFenceProperties,  218) \
    /* buffers, images, views */ \
    X(CreateBuffer,                               54) \
    X(DestroyBuffer,                              55) \
    X(CreateImage,                                58) \
    X(DestroyImage,                               59) \
    X(GetImageSubresourceLayout,                  60) \
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
    X(GetSemaphoreCounterValue,                  312) \
    X(WaitSemaphores,                            314) \
    X(SignalSemaphore,                           316) \
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
    X(AcquireNextImage2KHR,                      238) \
    X(QueuePresentKHR,                           170) \
    X(CreateXlibSurfaceKHR,                      171) \
    X(GetPhysicalDeviceXlibPresentationSupportKHR, 172) \
    /* command pools and command buffers */ \
    X(CreateCommandPool,                          97) \
    X(DestroyCommandPool,                         98) \
    X(ResetCommandPool,                           99) \
    X(AllocateCommandBuffers,                    100) \
    X(FreeCommandBuffers,                        101) \
    X(BeginCommandBuffer,                        102) \
    X(EndCommandBuffer,                          103) \
    X(ResetCommandBuffer,                        104) \
    X(QueueSubmit2,                              519) \
    /* events and query pools */ \
    X(CreateEvent,                                44) \
    X(DestroyEvent,                               45) \
    X(GetEventStatus,                             46) \
    X(SetEvent,                                   47) \
    X(ResetEvent,                                 48) \
    X(CreateQueryPool,                            49) \
    X(DestroyQueryPool,                           50) \
    X(GetQueryPoolResults,                        51) \
    X(ResetQueryPool,                             52) \
    /* buffer views and samplers */ \
    X(CreateBufferView,                           56) \
    X(DestroyBufferView,                          57) \
    X(CreateSampler,                              80) \
    X(DestroySampler,                             81) \
    /* shader modules and pipeline caches */ \
    X(CreateShaderModule,                         63) \
    X(DestroyShaderModule,                        64) \
    X(CreatePipelineCache,                        65) \
    X(DestroyPipelineCache,                       66) \
    X(GetPipelineCacheData,                       67) \
    X(MergePipelineCaches,                        68) \
    /* pipeline and descriptor set layouts */ \
    X(CreatePipelineLayout,                       78) \
    X(DestroyPipelineLayout,                      79) \
    X(CreateDescriptorSetLayout,                  82) \
    X(DestroyDescriptorSetLayout,                 83) \
    X(GetDescriptorSetLayoutSupport,             284) \
    /* descriptor pools, sets and updates */ \
    X(CreateDescriptorPool,                       84) \
    X(DestroyDescriptorPool,                      85) \
    X(ResetDescriptorPool,                        86) \
    X(AllocateDescriptorSets,                     87) \
    X(FreeDescriptorSets,                         88) \
    X(UpdateDescriptorSets,                       89) \
    X(CreateDescriptorUpdateTemplate,            242) \
    X(DestroyDescriptorUpdateTemplate,           244) \
    X(UpdateDescriptorSetWithTemplate,           246) \
    /* render passes and framebuffers */ \
    X(CreateRenderPass,                           92) \
    X(DestroyRenderPass,                          93) \
    X(CreateRenderPass2,                         304) \
    X(CreateFramebuffer,                          90) \
    X(DestroyFramebuffer,                         91) \
    /* pipelines */ \
    X(CreateGraphicsPipelines,                    74) \
    X(CreateComputePipelines,                     75) \
    X(DestroyPipeline,                            77) \
    /* recording: render pass and dynamic rendering scopes */ \
    X(CmdBeginRenderPass,                        156) \
    X(CmdNextSubpass,                            157) \
    X(CmdEndRenderPass,                          158) \
    X(CmdBeginRenderPass2,                       306) \
    X(CmdNextSubpass2,                           308) \
    X(CmdEndRenderPass2,                         310) \
    X(CmdBeginRendering,                         577) \
    X(CmdEndRendering,                           579) \
    /* recording: binding */ \
    X(CmdBindPipeline,                           105) \
    X(CmdBindDescriptorSets,                     116) \
    X(CmdBindIndexBuffer,                        117) \
    X(CmdBindVertexBuffers,                      118) \
    X(CmdBindVertexBuffers2,                     432) \
    X(CmdPushConstants,                          155) \
    /* recording: dynamic state */ \
    X(CmdSetViewport,                            107) \
    X(CmdSetScissor,                             108) \
    X(CmdSetLineWidth,                           109) \
    X(CmdSetDepthBias,                           110) \
    X(CmdSetBlendConstants,                      111) \
    X(CmdSetDepthBounds,                         112) \
    X(CmdSetStencilCompareMask,                  113) \
    X(CmdSetStencilWriteMask,                    114) \
    X(CmdSetStencilReference,                    115) \
    X(CmdSetViewportWithCount,                   426) \
    X(CmdSetScissorWithCount,                    428) \
    X(CmdSetCullMode,                            420) \
    X(CmdSetFrontFace,                           422) \
    X(CmdSetPrimitiveTopology,                   424) \
    X(CmdSetDepthTestEnable,                     434) \
    X(CmdSetDepthWriteEnable,                    436) \
    X(CmdSetDepthCompareOp,                      438) \
    X(CmdSetDepthBoundsTestEnable,               440) \
    X(CmdSetStencilTestEnable,                   442) \
    X(CmdSetStencilOp,                           444) \
    X(CmdSetRasterizerDiscardEnable,             447) \
    X(CmdSetDepthBiasEnable,                     449) \
    X(CmdSetPrimitiveRestartEnable,              452) \
    /* recording: draw and dispatch */ \
    X(CmdDraw,                                   119) \
    X(CmdDrawIndexed,                            120) \
    X(CmdDrawIndirect,                           123) \
    X(CmdDrawIndexedIndirect,                    124) \
    X(CmdDispatch,                               125) \
    X(CmdDispatchIndirect,                       126) \
    X(CmdDispatchBase,                           239) \
    /* recording: transfer */ \
    X(CmdCopyBuffer,                             131) \
    X(CmdCopyImage,                              132) \
    X(CmdBlitImage,                              133) \
    X(CmdCopyBufferToImage,                      134) \
    X(CmdCopyImageToBuffer,                      135) \
    X(CmdUpdateBuffer,                           138) \
    X(CmdFillBuffer,                             139) \
    X(CmdResolveImage,                           143) \
    X(CmdCopyBuffer2,                            493) \
    X(CmdCopyImage2,                             495) \
    X(CmdBlitImage2,                             497) \
    X(CmdCopyBufferToImage2,                     499) \
    X(CmdCopyImageToBuffer2,                     501) \
    X(CmdResolveImage2,                          503) \
    /* recording: clears */ \
    X(CmdClearColorImage,                        140) \
    X(CmdClearDepthStencilImage,                 141) \
    X(CmdClearAttachments,                       142) \
    /* recording: synchronisation */ \
    X(CmdPipelineBarrier,                        147) \
    X(CmdPipelineBarrier2,                       517) \
    X(CmdSetEvent,                               144) \
    X(CmdResetEvent,                             145) \
    X(CmdWaitEvents,                             146) \
    X(CmdSetEvent2,                              511) \
    X(CmdResetEvent2,                            513) \
    X(CmdWaitEvents2,                            515) \
    /* recording: queries and secondary command buffers */ \
    X(CmdBeginQuery,                             148) \
    X(CmdEndQuery,                               149) \
    X(CmdResetQueryPool,                         152) \
    X(CmdWriteTimestamp,                         153) \
    X(CmdWriteTimestamp2,                        521) \
    X(CmdCopyQueryPoolResults,                   154) \
    X(CmdExecuteCommands,                        159) \
    /* core 1.0 the audit found missing */ \
    X(GetDeviceMemoryCommitment,                  29) \
    X(GetImageSparseMemoryRequirements,           34) \
    X(GetPhysicalDeviceSparseImageFormatProperties, 35) \
    X(QueueBindSparse,                            36) \
    X(GetRenderAreaGranularity,                   94) \
    /* core 1.1 */ \
    X(EnumeratePhysicalDeviceGroups,             226) \
    X(GetDeviceGroupPeerMemoryFeatures,          228) \
    X(CmdSetDeviceMask,                          234) \
    X(TrimCommandPool,                           212) \
    X(GetImageSparseMemoryRequirements2,         267) \
    X(GetPhysicalDeviceSparseImageFormatProperties2, 208) \
    X(CreateSamplerYcbcrConversion,              275) \
    X(DestroySamplerYcbcrConversion,             277) \
    /* core 1.2 */ \
    X(CmdDrawIndirectCount,                      318) \
    X(CmdDrawIndexedIndirectCount,               321) \
    X(GetBufferDeviceAddress,                    384) \
    X(GetBufferOpaqueCaptureAddress,             382) \
    X(GetDeviceMemoryOpaqueCaptureAddress,       397) \
    /* core 1.3 */ \
    X(CreatePrivateDataSlot,                     485) \
    X(DestroyPrivateDataSlot,                    487) \
    X(SetPrivateData,                            489) \
    X(GetPrivateData,                            491) \
    X(GetDeviceBufferMemoryRequirements,         269) \
    X(GetDeviceImageMemoryRequirements,          271) \
    X(GetDeviceImageSparseMemoryRequirements,    273) \
    X(GetPhysicalDeviceToolProperties,           405) \
    /* core 1.4 */ \
    X(CmdBindIndexBuffer2,                       430) \
    X(CmdSetLineStipple,                         402) \
    X(CmdPushDescriptorSet,                      210) \
    X(CmdPushDescriptorSetWithTemplate,          248) \
    X(CmdBindDescriptorSets2,                    624) \
    X(CmdPushConstants2,                         626) \
    X(CmdPushDescriptorSet2,                     628) \
    X(CmdPushDescriptorSetWithTemplate2,         630) \
    X(CmdSetRenderingAttachmentLocations,        639) \
    X(CmdSetRenderingInputAttachmentIndices,     641) \
    X(MapMemory2,                                615) \
    X(UnmapMemory2,                              617) \
    X(GetImageSubresourceLayout2,                599) \
    X(GetDeviceImageSubresourceLayout,           613) \
    X(GetRenderingAreaGranularity,                95) \
    X(TransitionImageLayout,                     531) \
    /* VK_EXT_swapchain_maintenance1: DXVK calls this to give back an image it
     * acquired and decided not to present. Not core, but it is in the family
     * whose chain structures this bridge now carries, and a swapchain the
     * presenter cannot release images from is one it stops acquiring from. */ \
    X(ReleaseSwapchainImagesEXT,                 612)

/*
 * The core commands this bridge deliberately does NOT carry.
 *
 * Machine-readable on purpose: scripts/test_x64_vulkan_shim.py computes the
 * set of commands core Vulkan defines out of source/vulkan/vk/vulkan_core.h
 * and asserts that every one of them is either in the table above or named
 * here. That is what keeps the audit from rotting -- a header update that adds
 * a core command fails the test rather than quietly leaving a null in
 * winevulkan's dispatch table.
 *
 * All three are VK_VERSION_1_4 host image copy. VkMemoryToImageCopy and
 * VkImageToMemoryCopy carry a `pHostPointer` the driver reads or writes
 * directly, and its length is the byte footprint of an image region: it
 * depends on the format's texel block size and on memoryRowLength and
 * memoryImageHeight, none of which the call states. Sizing it would mean
 * carrying a format table into this bridge, and a wrong size is a read or a
 * write past the guest's own allocation -- the exact fault the marshal exists
 * to prevent. They resolve to NULL and the host prints `kind=core-miss`, which
 * is a named hole rather than a silent one.
 */
#define BOXEDWINE_X64_VK_CORE_REFUSALS(X) \
    X(CopyMemoryToImage) \
    X(CopyImageToMemory) \
    X(CopyImageToImage)

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
    X(GetPhysicalDeviceMemoryProperties2KHR,      GetPhysicalDeviceMemoryProperties2) \
    X(GetBufferMemoryRequirements2KHR,            GetBufferMemoryRequirements2) \
    X(GetImageMemoryRequirements2KHR,             GetImageMemoryRequirements2) \
    X(BindBufferMemory2KHR,                       BindBufferMemory2) \
    X(BindImageMemory2KHR,                        BindImageMemory2) \
    X(GetPhysicalDeviceExternalBufferPropertiesKHR, GetPhysicalDeviceExternalBufferProperties) \
    X(GetPhysicalDeviceExternalSemaphorePropertiesKHR, GetPhysicalDeviceExternalSemaphoreProperties) \
    X(GetPhysicalDeviceExternalFencePropertiesKHR, GetPhysicalDeviceExternalFenceProperties) \
    X(GetSemaphoreCounterValueKHR,                GetSemaphoreCounterValue) \
    X(WaitSemaphoresKHR,                          WaitSemaphores) \
    X(SignalSemaphoreKHR,                         SignalSemaphore) \
    X(ResetQueryPoolEXT,                          ResetQueryPool) \
    X(GetDescriptorSetLayoutSupportKHR,           GetDescriptorSetLayoutSupport) \
    X(CreateDescriptorUpdateTemplateKHR,          CreateDescriptorUpdateTemplate) \
    X(DestroyDescriptorUpdateTemplateKHR,         DestroyDescriptorUpdateTemplate) \
    X(UpdateDescriptorSetWithTemplateKHR,         UpdateDescriptorSetWithTemplate) \
    X(CreateRenderPass2KHR,                       CreateRenderPass2) \
    X(CmdBeginRenderPass2KHR,                     CmdBeginRenderPass2) \
    X(CmdNextSubpass2KHR,                         CmdNextSubpass2) \
    X(CmdEndRenderPass2KHR,                       CmdEndRenderPass2) \
    X(CmdBeginRenderingKHR,                       CmdBeginRendering) \
    X(CmdEndRenderingKHR,                         CmdEndRendering) \
    X(CmdDispatchBaseKHR,                         CmdDispatchBase) \
    X(CmdBindVertexBuffers2EXT,                   CmdBindVertexBuffers2) \
    X(CmdSetViewportWithCountEXT,                 CmdSetViewportWithCount) \
    X(CmdSetScissorWithCountEXT,                  CmdSetScissorWithCount) \
    X(CmdSetCullModeEXT,                          CmdSetCullMode) \
    X(CmdSetFrontFaceEXT,                         CmdSetFrontFace) \
    X(CmdSetPrimitiveTopologyEXT,                 CmdSetPrimitiveTopology) \
    X(CmdSetDepthTestEnableEXT,                   CmdSetDepthTestEnable) \
    X(CmdSetDepthWriteEnableEXT,                  CmdSetDepthWriteEnable) \
    X(CmdSetDepthCompareOpEXT,                    CmdSetDepthCompareOp) \
    X(CmdSetDepthBoundsTestEnableEXT,             CmdSetDepthBoundsTestEnable) \
    X(CmdSetStencilTestEnableEXT,                 CmdSetStencilTestEnable) \
    X(CmdSetStencilOpEXT,                         CmdSetStencilOp) \
    X(CmdSetRasterizerDiscardEnableEXT,           CmdSetRasterizerDiscardEnable) \
    X(CmdSetDepthBiasEnableEXT,                   CmdSetDepthBiasEnable) \
    X(CmdSetPrimitiveRestartEnableEXT,            CmdSetPrimitiveRestartEnable) \
    X(CmdCopyBuffer2KHR,                          CmdCopyBuffer2) \
    X(CmdCopyImage2KHR,                           CmdCopyImage2) \
    X(CmdBlitImage2KHR,                           CmdBlitImage2) \
    X(CmdCopyBufferToImage2KHR,                   CmdCopyBufferToImage2) \
    X(CmdCopyImageToBuffer2KHR,                   CmdCopyImageToBuffer2) \
    X(CmdResolveImage2KHR,                        CmdResolveImage2) \
    X(CmdPipelineBarrier2KHR,                     CmdPipelineBarrier2) \
    X(CmdSetEvent2KHR,                            CmdSetEvent2) \
    X(CmdResetEvent2KHR,                          CmdResetEvent2) \
    X(CmdWaitEvents2KHR,                          CmdWaitEvents2) \
    X(CmdWriteTimestamp2KHR,                      CmdWriteTimestamp2) \
    X(QueueSubmit2KHR,                            QueueSubmit2) \
    X(EnumeratePhysicalDeviceGroupsKHR,           EnumeratePhysicalDeviceGroups) \
    X(GetDeviceGroupPeerMemoryFeaturesKHR,        GetDeviceGroupPeerMemoryFeatures) \
    X(CmdSetDeviceMaskKHR,                        CmdSetDeviceMask) \
    X(TrimCommandPoolKHR,                         TrimCommandPool) \
    X(GetImageSparseMemoryRequirements2KHR,       GetImageSparseMemoryRequirements2) \
    X(GetPhysicalDeviceSparseImageFormatProperties2KHR, GetPhysicalDeviceSparseImageFormatProperties2) \
    X(CreateSamplerYcbcrConversionKHR,            CreateSamplerYcbcrConversion) \
    X(DestroySamplerYcbcrConversionKHR,           DestroySamplerYcbcrConversion) \
    X(CmdDrawIndirectCountKHR,                    CmdDrawIndirectCount) \
    X(CmdDrawIndirectCountAMD,                    CmdDrawIndirectCount) \
    X(CmdDrawIndexedIndirectCountKHR,             CmdDrawIndexedIndirectCount) \
    X(CmdDrawIndexedIndirectCountAMD,             CmdDrawIndexedIndirectCount) \
    X(GetBufferDeviceAddressKHR,                  GetBufferDeviceAddress) \
    X(GetBufferDeviceAddressEXT,                  GetBufferDeviceAddress) \
    X(GetBufferOpaqueCaptureAddressKHR,           GetBufferOpaqueCaptureAddress) \
    X(GetDeviceMemoryOpaqueCaptureAddressKHR,     GetDeviceMemoryOpaqueCaptureAddress) \
    X(CreatePrivateDataSlotEXT,                   CreatePrivateDataSlot) \
    X(DestroyPrivateDataSlotEXT,                  DestroyPrivateDataSlot) \
    X(SetPrivateDataEXT,                          SetPrivateData) \
    X(GetPrivateDataEXT,                          GetPrivateData) \
    X(GetDeviceBufferMemoryRequirementsKHR,       GetDeviceBufferMemoryRequirements) \
    X(GetDeviceImageMemoryRequirementsKHR,        GetDeviceImageMemoryRequirements) \
    X(GetDeviceImageSparseMemoryRequirementsKHR,  GetDeviceImageSparseMemoryRequirements) \
    X(GetPhysicalDeviceToolPropertiesEXT,         GetPhysicalDeviceToolProperties) \
    X(CmdBindIndexBuffer2KHR,                     CmdBindIndexBuffer2) \
    X(CmdSetLineStippleKHR,                       CmdSetLineStipple) \
    X(CmdSetLineStippleEXT,                       CmdSetLineStipple) \
    X(CmdPushDescriptorSetKHR,                    CmdPushDescriptorSet) \
    X(CmdPushDescriptorSetWithTemplateKHR,        CmdPushDescriptorSetWithTemplate) \
    X(CmdBindDescriptorSets2KHR,                  CmdBindDescriptorSets2) \
    X(CmdPushConstants2KHR,                       CmdPushConstants2) \
    X(CmdPushDescriptorSet2KHR,                   CmdPushDescriptorSet2) \
    X(CmdPushDescriptorSetWithTemplate2KHR,       CmdPushDescriptorSetWithTemplate2) \
    X(CmdSetRenderingAttachmentLocationsKHR,      CmdSetRenderingAttachmentLocations) \
    X(CmdSetRenderingInputAttachmentIndicesKHR,   CmdSetRenderingInputAttachmentIndices) \
    X(MapMemory2KHR,                              MapMemory2) \
    X(UnmapMemory2KHR,                            UnmapMemory2) \
    X(GetImageSubresourceLayout2KHR,              GetImageSubresourceLayout2) \
    X(GetImageSubresourceLayout2EXT,              GetImageSubresourceLayout2) \
    X(GetDeviceImageSubresourceLayoutKHR,         GetDeviceImageSubresourceLayout) \
    X(GetRenderingAreaGranularityKHR,             GetRenderingAreaGranularity) \
    X(TransitionImageLayoutEXT,                   TransitionImageLayout)

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
