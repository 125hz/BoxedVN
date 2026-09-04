/*
 * BoxedWine x86-64 guest Vulkan ICD -- libvulkan.so.1 for the 64-bit lane.
 *
 * Wine's winex11.drv dlopens SONAME_LIBVULKAN and binds sixteen symbols by
 * name (dlls/winex11.drv/vulkan.c of Wine 9.0); everything else it and
 * winevulkan reach through vkGetInstanceProcAddr / vkGetDeviceProcAddr. The
 * file a 64-bit guest currently finds under that name is the IA-32 lane's
 * shim, an i386 ELF whose `int 0x9A` trap CPU64 does not decode, so the
 * 64-bit loader rejects it for its ELF class and walks past it. This is the
 * x86-64 replacement.
 *
 * It marshals nothing. Every entry point packs its arguments into an array of
 * 64-bit words and traps through the reserved syscall in
 * include/boxedwine_x64_vulkan_bridge.h; the host casts those words to the
 * real Vulkan types, copies every structure they point at into host-side
 * shadows with the nested pointers translated, and calls MoltenVK with those.
 * Keeping the marshal entirely on the host side is what lets this file stay
 * free of Vulkan headers: it never has to know the shape of anything it
 * passes, only that every Vulkan structure has the same layout on x86-64
 * System V as on arm64 AAPCS64, which is what makes a byte copy sufficient.
 *
 * No Vulkan headers are required to build this file. Only the widths of the
 * scalars matter, and they are fixed by the Vulkan ABI: dispatchable handles
 * are pointers, non-dispatchable handles are 64 bits on a 64-bit platform,
 * VkResult and the enums are 32-bit signed, the flag types are 32-bit
 * unsigned, VkDeviceSize is 64-bit unsigned. Pulling in the real headers
 * would make the builder depend on a Vulkan SDK for no gain, and the shim
 * never dereferences any of these structures.
 */

#include <stdint.h>
#include <string.h>

#include "boxedwine_x64_vulkan_bridge.h"

#if !defined(__x86_64__)
#error "The BoxedWine guest Vulkan ICD is an x86-64 guest library."
#endif

/* -- Vulkan ABI scalars ---------------------------------------------------- */

#define VKAPI_ATTR __attribute__((visibility("default")))
#define VKAPI_CALL
#define VKAPI_PTR

typedef int32_t  VkResult;
typedef uint32_t VkBool32;
typedef uint32_t VkFlags;
typedef uint64_t VkFlags64;   /* VkPipelineStageFlags2 and friends */
typedef uint64_t VkDeviceSize;
typedef int32_t  VkEnum;      /* every Vulkan enum is a 32-bit signed int */

/* Dispatchable handles are pointers. */
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkCommandBuffer;

/* Non-dispatchable handles are 64 bits wide on a 64-bit platform. */
typedef uint64_t VkNonDispatchable;
typedef VkNonDispatchable VkDeviceMemory;
typedef VkNonDispatchable VkBuffer;
typedef VkNonDispatchable VkBufferView;
typedef VkNonDispatchable VkImage;
typedef VkNonDispatchable VkImageView;
typedef VkNonDispatchable VkSampler;
typedef VkNonDispatchable VkFence;
typedef VkNonDispatchable VkSemaphore;
typedef VkNonDispatchable VkEvent;
typedef VkNonDispatchable VkQueryPool;
typedef VkNonDispatchable VkShaderModule;
typedef VkNonDispatchable VkPipelineCache;
typedef VkNonDispatchable VkPipeline;
typedef VkNonDispatchable VkPipelineLayout;
typedef VkNonDispatchable VkDescriptorSetLayout;
typedef VkNonDispatchable VkDescriptorPool;
typedef VkNonDispatchable VkDescriptorSet;
typedef VkNonDispatchable VkDescriptorUpdateTemplate;
typedef VkNonDispatchable VkRenderPass;
typedef VkNonDispatchable VkFramebuffer;
typedef VkNonDispatchable VkCommandPool;
typedef VkNonDispatchable VkSamplerYcbcrConversion;
typedef VkNonDispatchable VkPrivateDataSlot;
typedef VkNonDispatchable VkSurfaceKHR;
typedef VkNonDispatchable VkSwapchainKHR;

typedef void (VKAPI_PTR *PFN_vkVoidFunction)(void);

#define VK_SUCCESS                        0
#define VK_ERROR_INITIALIZATION_FAILED  (-3)
#define VK_ERROR_INCOMPATIBLE_DRIVER    (-9)

/* The Vulkan version this ICD reports when the host cannot answer. Wine only
 * calls vkEnumerateInstanceVersion when the symbol resolves, so a refusal has
 * to be a VkResult rather than a missing export. */
#define BW_VK_API_VERSION_1_0 ((uint32_t)((1u << 22) | (0u << 12) | 0u))

/* -- The trap -------------------------------------------------------------- */

/* Resolved once, before the first entry point does anything. A build of
 * BoxedWine with no host Vulkan, a host whose bridge ABI this shim does not
 * understand, or a plain Linux kernel that answers ENOSYS all land here, and
 * every entry point then fails with VK_ERROR_INITIALIZATION_FAILED instead of
 * returning a syscall errno dressed up as a VkResult. */
static int bw_ready = -1;

static void bw_init(void)
{
    int64_t abi = boxedwine_x64_vulkan_call(BOXEDWINE_X64_VK_OP_ABI, 0, 0);
    int64_t caps;

    if (abi != (int64_t)BOXEDWINE_X64_VK_ABI_VERSION) {
        bw_ready = 0;
        return;
    }
    caps = boxedwine_x64_vulkan_call(BOXEDWINE_X64_VK_OP_PROBE, 0, 0);
    if (caps < 0) {
        bw_ready = 0;
        return;
    }
    /* Both bits or nothing: without the host marshal there is no driver, and
     * without the identity map a host Vulkan handle is meaningless in this
     * address space. Declining here is what turns a structural limitation
     * into a named Vulkan error rather than a fault inside Metal. */
    bw_ready = (((uint64_t)caps & BOXEDWINE_X64_VK_CAP_HOST_MARSHAL) &&
                ((uint64_t)caps & BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY)) ? 1 : 0;
}

static int bw_usable(void)
{
    if (bw_ready < 0) {
        bw_init();
    }
    return bw_ready == 1;
}

/* A bridge refusal is not a VkResult and must never be handed to a caller as
 * one. Every entry point below narrows this 64-bit word to a 32-bit VkResult,
 * so a refusal that reached that cast would arrive at the application as
 * whatever fell out of the truncation. The codes live below INT32_MIN
 * precisely so this test is exact rather than a guess about which small
 * negative numbers the host might mean.
 *
 * Every refusal becomes VK_ERROR_INITIALIZATION_FAILED, which is the same
 * answer this shim already gives when the bridge is unusable, and is the
 * honest summary of all of them: the bridge could not serve the call. The
 * reason itself is not knowable to a Vulkan caller and does not need to be --
 * the host names it in the log, which is where it is actionable. */
static int64_t bw_call(uint64_t op, uint64_t* args, uint64_t count)
{
    int64_t result;

    if (!bw_usable()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    result = boxedwine_x64_vulkan_call(op, args, count);
    if (BOXEDWINE_X64_VK_IS_ERROR(result)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return result;
}

#define U(x) ((uint64_t)(uintptr_t)(x))

/* A float parameter crosses as the bit pattern of its IEEE-754 binary32
 * value in the low 32 bits of an argument word, which the host reads back
 * with the mirror image of this function. Casting the value to uint64_t
 * instead would round it to an integer -- a line width of 1.5 would arrive
 * as 1 -- and reinterpreting the object representation is the only way to
 * carry it through an integer word intact. Both sides are IEEE-754 binary32
 * with the same endianness, so the round trip is exact. */
static uint64_t bw_f32(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof bits);
    return (uint64_t)bits;
}

#define F(x) bw_f32(x)

/* A command that returns a 64-bit value rather than a VkResult:
 * vkGetBufferDeviceAddress and the two opaque-capture-address queries. The
 * value cannot come back in the bridge's own return word, because a refusal is
 * told from an answer by the top half of that word (BOXEDWINE_X64_VK_IS_ERROR)
 * and a legitimate device address may have those bits set. So the shim passes
 * the address of a uint64_t on its own stack as one extra argument word and
 * the host writes the answer there. A refusal yields zero, which is the value
 * Vulkan defines for "this object has no address". */
#define BW_A(op, ...)                                                            do {                                                                             uint64_t bw_out = 0;                                                         uint64_t bw_a[] = { __VA_ARGS__, U(&bw_out) };                               if (bw_call(BOXEDWINE_X64_VK_OP_##op, bw_a,                                              sizeof bw_a / sizeof bw_a[0]) != 0) {                                return 0;                                                                }                                                                            return bw_out;                                                           } while (0)

#define BW_R(op, ...)                                                        \
    do {                                                                     \
        uint64_t bw_a[] = { __VA_ARGS__ };                                   \
        return (VkResult)bw_call(BOXEDWINE_X64_VK_OP_##op, bw_a,             \
                                 sizeof bw_a / sizeof bw_a[0]);              \
    } while (0)

#define BW_V(op, ...)                                                        \
    do {                                                                     \
        uint64_t bw_a[] = { __VA_ARGS__ };                                   \
        (void)bw_call(BOXEDWINE_X64_VK_OP_##op, bw_a,                        \
                      sizeof bw_a / sizeof bw_a[0]);                         \
        return;                                                              \
    } while (0)

#define BW_B(op, ...)                                                        \
    do {                                                                     \
        uint64_t bw_a[] = { __VA_ARGS__ };                                   \
        int64_t bw_r = bw_call(BOXEDWINE_X64_VK_OP_##op, bw_a,               \
                               sizeof bw_a / sizeof bw_a[0]);                \
        return bw_r > 0 ? (VkBool32)1 : (VkBool32)0;                         \
    } while (0)

/* -- Global commands ------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* pApiVersion)
{ BW_R(EnumerateInstanceVersion, U(pApiVersion)); }

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, void* pProperties)
{ BW_R(EnumerateInstanceLayerProperties, U(pPropertyCount), U(pProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, void* pProperties)
{ BW_R(EnumerateInstanceExtensionProperties, U(pLayerName), U(pPropertyCount), U(pProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const void* pCreateInfo, const void* pAllocator, VkInstance* pInstance)
{ BW_R(CreateInstance, U(pCreateInfo), U(pAllocator), U(pInstance)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const void* pAllocator)
{ BW_V(DestroyInstance, U(instance), U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices)
{ BW_R(EnumeratePhysicalDevices, U(instance), U(pPhysicalDeviceCount), U(pPhysicalDevices)); }

/* -- Physical device ------------------------------------------------------- */

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, void* pProperties)
{ BW_V(GetPhysicalDeviceProperties, U(physicalDevice), U(pProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, void* pProperties)
{ BW_V(GetPhysicalDeviceProperties2, U(physicalDevice), U(pProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, void* pFeatures)
{ BW_V(GetPhysicalDeviceFeatures, U(physicalDevice), U(pFeatures)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, void* pFeatures)
{ BW_V(GetPhysicalDeviceFeatures2, U(physicalDevice), U(pFeatures)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkEnum format, void* pFormatProperties)
{ BW_V(GetPhysicalDeviceFormatProperties, U(physicalDevice), (uint64_t)(uint32_t)format, U(pFormatProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkEnum format, void* pFormatProperties)
{ BW_V(GetPhysicalDeviceFormatProperties2, U(physicalDevice), (uint64_t)(uint32_t)format, U(pFormatProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkEnum format, VkEnum type, VkEnum tiling, VkFlags usage, VkFlags flags, void* pImageFormatProperties)
{ BW_R(GetPhysicalDeviceImageFormatProperties, U(physicalDevice), (uint64_t)(uint32_t)format, (uint64_t)(uint32_t)type, (uint64_t)(uint32_t)tiling, (uint64_t)usage, (uint64_t)flags, U(pImageFormatProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const void* pImageFormatInfo, void* pImageFormatProperties)
{ BW_R(GetPhysicalDeviceImageFormatProperties2, U(physicalDevice), U(pImageFormatInfo), U(pImageFormatProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pCount, void* pProperties)
{ BW_V(GetPhysicalDeviceQueueFamilyProperties, U(physicalDevice), U(pCount), U(pProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pCount, void* pProperties)
{ BW_V(GetPhysicalDeviceQueueFamilyProperties2, U(physicalDevice), U(pCount), U(pProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, void* pMemoryProperties)
{ BW_V(GetPhysicalDeviceMemoryProperties, U(physicalDevice), U(pMemoryProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, void* pMemoryProperties)
{ BW_V(GetPhysicalDeviceMemoryProperties2, U(physicalDevice), U(pMemoryProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, void* pProperties)
{ BW_R(EnumerateDeviceLayerProperties, U(physicalDevice), U(pPropertyCount), U(pProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, void* pProperties)
{ BW_R(EnumerateDeviceExtensionProperties, U(physicalDevice), U(pLayerName), U(pPropertyCount), U(pProperties)); }

/* -- Device and queue ------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const void* pCreateInfo, const void* pAllocator, VkDevice* pDevice)
{ BW_R(CreateDevice, U(physicalDevice), U(pCreateInfo), U(pAllocator), U(pDevice)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const void* pAllocator)
{ BW_V(DestroyDevice, U(device), U(pAllocator)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
{ BW_V(GetDeviceQueue, U(device), (uint64_t)queueFamilyIndex, (uint64_t)queueIndex, U(pQueue)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const void* pQueueInfo, VkQueue* pQueue)
{ BW_V(GetDeviceQueue2, U(device), U(pQueueInfo), U(pQueue)); }

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{ BW_R(DeviceWaitIdle, U(device)); }

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{ BW_R(QueueWaitIdle, U(queue)); }

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const void* pSubmits, VkFence fence)
{ BW_R(QueueSubmit, U(queue), (uint64_t)submitCount, U(pSubmits), (uint64_t)fence); }

/* -- Memory ---------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const void* pAllocateInfo, const void* pAllocator, VkDeviceMemory* pMemory)
{ BW_R(AllocateMemory, U(device), U(pAllocateInfo), U(pAllocator), U(pMemory)); }

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const void* pAllocator)
{ BW_V(FreeMemory, U(device), (uint64_t)memory, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkFlags flags, void** ppData)
{ BW_R(MapMemory, U(device), (uint64_t)memory, (uint64_t)offset, (uint64_t)size, (uint64_t)flags, U(ppData)); }

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{ BW_V(UnmapMemory, U(device), (uint64_t)memory); }

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const void* pMemoryRanges)
{ BW_R(FlushMappedMemoryRanges, U(device), (uint64_t)memoryRangeCount, U(pMemoryRanges)); }

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const void* pMemoryRanges)
{ BW_R(InvalidateMappedMemoryRanges, U(device), (uint64_t)memoryRangeCount, U(pMemoryRanges)); }

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, void* pMemoryRequirements)
{ BW_V(GetBufferMemoryRequirements, U(device), (uint64_t)buffer, U(pMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice device, const void* pInfo, void* pMemoryRequirements)
{ BW_V(GetBufferMemoryRequirements2, U(device), U(pInfo), U(pMemoryRequirements)); }

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{ BW_R(BindBufferMemory, U(device), (uint64_t)buffer, (uint64_t)memory, (uint64_t)memoryOffset); }

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const void* pBindInfos)
{ BW_R(BindBufferMemory2, U(device), (uint64_t)bindInfoCount, U(pBindInfos)); }

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image, void* pMemoryRequirements)
{ BW_V(GetImageMemoryRequirements, U(device), (uint64_t)image, U(pMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice device, const void* pInfo, void* pMemoryRequirements)
{ BW_V(GetImageMemoryRequirements2, U(device), U(pInfo), U(pMemoryRequirements)); }

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{ BW_R(BindImageMemory, U(device), (uint64_t)image, (uint64_t)memory, (uint64_t)memoryOffset); }

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount, const void* pBindInfos)
{ BW_R(BindImageMemory2, U(device), (uint64_t)bindInfoCount, U(pBindInfos)); }

/* The external-object capability queries. DXVK runs all three before it
 * chooses a memory type or a sharing mode; a NULL answer for any of them is
 * read as a device it cannot use. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice, const void* pExternalBufferInfo, void* pExternalBufferProperties)
{ BW_V(GetPhysicalDeviceExternalBufferProperties, U(physicalDevice), U(pExternalBufferInfo), U(pExternalBufferProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice, const void* pExternalSemaphoreInfo, void* pExternalSemaphoreProperties)
{ BW_V(GetPhysicalDeviceExternalSemaphoreProperties, U(physicalDevice), U(pExternalSemaphoreInfo), U(pExternalSemaphoreProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice, const void* pExternalFenceInfo, void* pExternalFenceProperties)
{ BW_V(GetPhysicalDeviceExternalFenceProperties, U(physicalDevice), U(pExternalFenceInfo), U(pExternalFenceProperties)); }

/* The extension Wine's WoW64 layer needs before it can hand a 32-bit caller a
 * mapped pointer: wine_vkAllocateMemory places host memory below 4 GiB itself
 * and imports it with VkImportMemoryHostPointerInfoEXT, which is the only way
 * vkMapMemory can return an address a 32-bit program can hold. Wine reads the
 * required alignment out of vkGetPhysicalDeviceProperties2 and calls this. */
VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryHostPointerPropertiesEXT(VkDevice device, VkFlags handleType, const void* pHostPointer, void* pMemoryHostPointerProperties)
{ BW_R(GetMemoryHostPointerPropertiesEXT, U(device), (uint64_t)handleType, U(pHostPointer), U(pMemoryHostPointerProperties)); }

/* -- Buffers, images, views ------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkBuffer* pBuffer)
{ BW_R(CreateBuffer, U(device), U(pCreateInfo), U(pAllocator), U(pBuffer)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const void* pAllocator)
{ BW_V(DestroyBuffer, U(device), (uint64_t)buffer, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkImage* pImage)
{ BW_R(CreateImage, U(device), U(pCreateInfo), U(pAllocator), U(pImage)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const void* pAllocator)
{ BW_V(DestroyImage, U(device), (uint64_t)image, U(pAllocator)); }

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image, const void* pSubresource, void* pLayout)
{ BW_V(GetImageSubresourceLayout, U(device), (uint64_t)image, U(pSubresource), U(pLayout)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkImageView* pView)
{ BW_R(CreateImageView, U(device), U(pCreateInfo), U(pAllocator), U(pView)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView imageView, const void* pAllocator)
{ BW_V(DestroyImageView, U(device), (uint64_t)imageView, U(pAllocator)); }

/* -- Synchronisation ------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkFence* pFence)
{ BW_R(CreateFence, U(device), U(pCreateInfo), U(pAllocator), U(pFence)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const void* pAllocator)
{ BW_V(DestroyFence, U(device), (uint64_t)fence, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences)
{ BW_R(ResetFences, U(device), (uint64_t)fenceCount, U(pFences)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence)
{ BW_R(GetFenceStatus, U(device), (uint64_t)fence); }

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout)
{ BW_R(WaitForFences, U(device), (uint64_t)fenceCount, U(pFences), (uint64_t)waitAll, timeout); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkSemaphore* pSemaphore)
{ BW_R(CreateSemaphore, U(device), U(pCreateInfo), U(pAllocator), U(pSemaphore)); }

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const void* pAllocator)
{ BW_V(DestroySemaphore, U(device), (uint64_t)semaphore, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t* pValue)
{ BW_R(GetSemaphoreCounterValue, U(device), (uint64_t)semaphore, U(pValue)); }

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const void* pWaitInfo, uint64_t timeout)
{ BW_R(WaitSemaphores, U(device), U(pWaitInfo), timeout); }

VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const void* pSignalInfo)
{ BW_R(SignalSemaphore, U(device), U(pSignalInfo)); }

/* -- Surface and swapchain ------------------------------------------------- */

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const void* pAllocator)
{ BW_V(DestroySurfaceKHR, U(instance), (uint64_t)surface, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported)
{ BW_R(GetPhysicalDeviceSurfaceSupportKHR, U(physicalDevice), (uint64_t)queueFamilyIndex, (uint64_t)surface, U(pSupported)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, void* pSurfaceCapabilities)
{ BW_R(GetPhysicalDeviceSurfaceCapabilitiesKHR, U(physicalDevice), (uint64_t)surface, U(pSurfaceCapabilities)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, void* pSurfaceFormats)
{ BW_R(GetPhysicalDeviceSurfaceFormatsKHR, U(physicalDevice), (uint64_t)surface, U(pSurfaceFormatCount), U(pSurfaceFormats)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, void* pPresentModes)
{ BW_R(GetPhysicalDeviceSurfacePresentModesKHR, U(physicalDevice), (uint64_t)surface, U(pPresentModeCount), U(pPresentModes)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const void* pSurfaceInfo, void* pSurfaceCapabilities)
{ BW_R(GetPhysicalDeviceSurfaceCapabilities2KHR, U(physicalDevice), U(pSurfaceInfo), U(pSurfaceCapabilities)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const void* pSurfaceInfo, uint32_t* pSurfaceFormatCount, void* pSurfaceFormats)
{ BW_R(GetPhysicalDeviceSurfaceFormats2KHR, U(physicalDevice), U(pSurfaceInfo), U(pSurfaceFormatCount), U(pSurfaceFormats)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pRectCount, void* pRects)
{ BW_R(GetPhysicalDevicePresentRectanglesKHR, U(physicalDevice), (uint64_t)surface, U(pRectCount), U(pRects)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, uint32_t* pModes)
{ BW_R(GetDeviceGroupSurfacePresentModesKHR, U(device), (uint64_t)surface, U(pModes)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkSwapchainKHR* pSwapchain)
{ BW_R(CreateSwapchainKHR, U(device), U(pCreateInfo), U(pAllocator), U(pSwapchain)); }

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const void* pAllocator)
{ BW_V(DestroySwapchainKHR, U(device), (uint64_t)swapchain, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
{ BW_R(GetSwapchainImagesKHR, U(device), (uint64_t)swapchain, U(pSwapchainImageCount), U(pSwapchainImages)); }

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
{ BW_R(AcquireNextImageKHR, U(device), (uint64_t)swapchain, timeout, (uint64_t)semaphore, (uint64_t)fence, U(pImageIndex)); }

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const void* pAcquireInfo, uint32_t* pImageIndex)
{ BW_R(AcquireNextImage2KHR, U(device), U(pAcquireInfo), U(pImageIndex)); }

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const void* pPresentInfo)
{ BW_R(QueuePresentKHR, U(queue), U(pPresentInfo)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateXlibSurfaceKHR(VkInstance instance, const void* pCreateInfo, const void* pAllocator, VkSurfaceKHR* pSurface)
{ BW_R(CreateXlibSurfaceKHR, U(instance), U(pCreateInfo), U(pAllocator), U(pSurface)); }

VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, void* dpy, uint64_t visualID)
{ BW_B(GetPhysicalDeviceXlibPresentationSupportKHR, U(physicalDevice), (uint64_t)queueFamilyIndex, U(dpy), visualID); }

/* -- Command pools and command buffers ------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkCommandPool* pCommandPool)
{ BW_R(CreateCommandPool, U(device), U(pCreateInfo), U(pAllocator), U(pCommandPool)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const void* pAllocator)
{ BW_V(DestroyCommandPool, U(device), (uint64_t)commandPool, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool commandPool, VkFlags flags)
{ BW_R(ResetCommandPool, U(device), (uint64_t)commandPool, (uint64_t)flags); }

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const void* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
{ BW_R(AllocateCommandBuffers, U(device), U(pAllocateInfo), U(pCommandBuffers)); }

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
{ BW_V(FreeCommandBuffers, U(device), (uint64_t)commandPool, (uint64_t)commandBufferCount, U(pCommandBuffers)); }

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const void* pBeginInfo)
{ BW_R(BeginCommandBuffer, U(commandBuffer), U(pBeginInfo)); }

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{ BW_R(EndCommandBuffer, U(commandBuffer)); }

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkFlags flags)
{ BW_R(ResetCommandBuffer, U(commandBuffer), (uint64_t)flags); }

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const void* pSubmits, VkFence fence)
{ BW_R(QueueSubmit2, U(queue), (uint64_t)submitCount, U(pSubmits), (uint64_t)fence); }

/* -- Events and query pools ------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkEvent* pEvent)
{ BW_R(CreateEvent, U(device), U(pCreateInfo), U(pAllocator), U(pEvent)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice device, VkEvent event, const void* pAllocator)
{ BW_V(DestroyEvent, U(device), (uint64_t)event, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent event)
{ BW_R(GetEventStatus, U(device), (uint64_t)event); }

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event)
{ BW_R(SetEvent, U(device), (uint64_t)event); }

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event)
{ BW_R(ResetEvent, U(device), (uint64_t)event); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkQueryPool* pQueryPool)
{ BW_R(CreateQueryPool, U(device), U(pCreateInfo), U(pAllocator), U(pQueryPool)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const void* pAllocator)
{ BW_V(DestroyQueryPool, U(device), (uint64_t)queryPool, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, VkDeviceSize stride, VkFlags flags)
{ BW_R(GetQueryPoolResults, U(device), (uint64_t)queryPool, (uint64_t)firstQuery, (uint64_t)queryCount, (uint64_t)dataSize, U(pData), (uint64_t)stride, (uint64_t)flags); }

VKAPI_ATTR void VKAPI_CALL vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{ BW_V(ResetQueryPool, U(device), (uint64_t)queryPool, (uint64_t)firstQuery, (uint64_t)queryCount); }

/* -- Buffer views and samplers --------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkBufferView* pView)
{ BW_R(CreateBufferView, U(device), U(pCreateInfo), U(pAllocator), U(pView)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView bufferView, const void* pAllocator)
{ BW_V(DestroyBufferView, U(device), (uint64_t)bufferView, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkSampler* pSampler)
{ BW_R(CreateSampler, U(device), U(pCreateInfo), U(pAllocator), U(pSampler)); }

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const void* pAllocator)
{ BW_V(DestroySampler, U(device), (uint64_t)sampler, U(pAllocator)); }

/* -- Shader modules and pipeline caches ------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkShaderModule* pShaderModule)
{ BW_R(CreateShaderModule, U(device), U(pCreateInfo), U(pAllocator), U(pShaderModule)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const void* pAllocator)
{ BW_V(DestroyShaderModule, U(device), (uint64_t)shaderModule, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkPipelineCache* pPipelineCache)
{ BW_R(CreatePipelineCache, U(device), U(pCreateInfo), U(pAllocator), U(pPipelineCache)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const void* pAllocator)
{ BW_V(DestroyPipelineCache, U(device), (uint64_t)pipelineCache, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t* pDataSize, void* pData)
{ BW_R(GetPipelineCacheData, U(device), (uint64_t)pipelineCache, U(pDataSize), U(pData)); }

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache* pSrcCaches)
{ BW_R(MergePipelineCaches, U(device), (uint64_t)dstCache, (uint64_t)srcCacheCount, U(pSrcCaches)); }

/* -- Pipeline and descriptor set layouts ----------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkPipelineLayout* pPipelineLayout)
{ BW_R(CreatePipelineLayout, U(device), U(pCreateInfo), U(pAllocator), U(pPipelineLayout)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout, const void* pAllocator)
{ BW_V(DestroyPipelineLayout, U(device), (uint64_t)pipelineLayout, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkDescriptorSetLayout* pSetLayout)
{ BW_R(CreateDescriptorSetLayout, U(device), U(pCreateInfo), U(pAllocator), U(pSetLayout)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const void* pAllocator)
{ BW_V(DestroyDescriptorSetLayout, U(device), (uint64_t)descriptorSetLayout, U(pAllocator)); }

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSupport(VkDevice device, const void* pCreateInfo, void* pSupport)
{ BW_V(GetDescriptorSetLayoutSupport, U(device), U(pCreateInfo), U(pSupport)); }

/* -- Descriptor pools, sets and updates ------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkDescriptorPool* pDescriptorPool)
{ BW_R(CreateDescriptorPool, U(device), U(pCreateInfo), U(pAllocator), U(pDescriptorPool)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, const void* pAllocator)
{ BW_V(DestroyDescriptorPool, U(device), (uint64_t)descriptorPool, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, VkFlags flags)
{ BW_R(ResetDescriptorPool, U(device), (uint64_t)descriptorPool, (uint64_t)flags); }

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const void* pAllocateInfo, VkDescriptorSet* pDescriptorSets)
{ BW_R(AllocateDescriptorSets, U(device), U(pAllocateInfo), U(pDescriptorSets)); }

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets)
{ BW_R(FreeDescriptorSets, U(device), (uint64_t)descriptorPool, (uint64_t)descriptorSetCount, U(pDescriptorSets)); }

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const void* pDescriptorWrites, uint32_t descriptorCopyCount, const void* pDescriptorCopies)
{ BW_V(UpdateDescriptorSets, U(device), (uint64_t)descriptorWriteCount, U(pDescriptorWrites), (uint64_t)descriptorCopyCount, U(pDescriptorCopies)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate)
{ BW_R(CreateDescriptorUpdateTemplate, U(device), U(pCreateInfo), U(pAllocator), U(pDescriptorUpdateTemplate)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pAllocator)
{ BW_V(DestroyDescriptorUpdateTemplate, U(device), (uint64_t)descriptorUpdateTemplate, U(pAllocator)); }

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData)
{ BW_V(UpdateDescriptorSetWithTemplate, U(device), (uint64_t)descriptorSet, (uint64_t)descriptorUpdateTemplate, U(pData)); }

/* -- Render passes and framebuffers ---------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkRenderPass* pRenderPass)
{ BW_R(CreateRenderPass, U(device), U(pCreateInfo), U(pAllocator), U(pRenderPass)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkRenderPass* pRenderPass)
{ BW_R(CreateRenderPass2, U(device), U(pCreateInfo), U(pAllocator), U(pRenderPass)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const void* pAllocator)
{ BW_V(DestroyRenderPass, U(device), (uint64_t)renderPass, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkFramebuffer* pFramebuffer)
{ BW_R(CreateFramebuffer, U(device), U(pCreateInfo), U(pAllocator), U(pFramebuffer)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const void* pAllocator)
{ BW_V(DestroyFramebuffer, U(device), (uint64_t)framebuffer, U(pAllocator)); }

/* -- Pipelines ------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const void* pCreateInfos, const void* pAllocator, VkPipeline* pPipelines)
{ BW_R(CreateGraphicsPipelines, U(device), (uint64_t)pipelineCache, (uint64_t)createInfoCount, U(pCreateInfos), U(pAllocator), U(pPipelines)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const void* pCreateInfos, const void* pAllocator, VkPipeline* pPipelines)
{ BW_R(CreateComputePipelines, U(device), (uint64_t)pipelineCache, (uint64_t)createInfoCount, U(pCreateInfos), U(pAllocator), U(pPipelines)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const void* pAllocator)
{ BW_V(DestroyPipeline, U(device), (uint64_t)pipeline, U(pAllocator)); }

/* -- Recording: render pass and dynamic rendering scopes ------------------- */

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const void* pRenderPassBegin, VkEnum contents)
{ BW_V(CmdBeginRenderPass, U(commandBuffer), U(pRenderPassBegin), (uint64_t)(uint32_t)contents); }

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkEnum contents)
{ BW_V(CmdNextSubpass, U(commandBuffer), (uint64_t)(uint32_t)contents); }

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{ BW_V(CmdEndRenderPass, U(commandBuffer)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const void* pRenderPassBegin, const void* pSubpassBeginInfo)
{ BW_V(CmdBeginRenderPass2, U(commandBuffer), U(pRenderPassBegin), U(pSubpassBeginInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer commandBuffer, const void* pSubpassBeginInfo, const void* pSubpassEndInfo)
{ BW_V(CmdNextSubpass2, U(commandBuffer), U(pSubpassBeginInfo), U(pSubpassEndInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const void* pSubpassEndInfo)
{ BW_V(CmdEndRenderPass2, U(commandBuffer), U(pSubpassEndInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer, const void* pRenderingInfo)
{ BW_V(CmdBeginRendering, U(commandBuffer), U(pRenderingInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer)
{ BW_V(CmdEndRendering, U(commandBuffer)); }

/* -- Recording: binding ----------------------------------------------------- */

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkEnum pipelineBindPoint, VkPipeline pipeline)
{ BW_V(CmdBindPipeline, U(commandBuffer), (uint64_t)(uint32_t)pipelineBindPoint, (uint64_t)pipeline); }

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkEnum pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
{ BW_V(CmdBindDescriptorSets, U(commandBuffer), (uint64_t)(uint32_t)pipelineBindPoint, (uint64_t)layout, (uint64_t)firstSet, (uint64_t)descriptorSetCount, U(pDescriptorSets), (uint64_t)dynamicOffsetCount, U(pDynamicOffsets)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkEnum indexType)
{ BW_V(CmdBindIndexBuffer, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset, (uint64_t)(uint32_t)indexType); }

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets)
{ BW_V(CmdBindVertexBuffers, U(commandBuffer), (uint64_t)firstBinding, (uint64_t)bindingCount, U(pBuffers), U(pOffsets)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides)
{ BW_V(CmdBindVertexBuffers2, U(commandBuffer), (uint64_t)firstBinding, (uint64_t)bindingCount, U(pBuffers), U(pOffsets), U(pSizes), U(pStrides)); }

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues)
{ BW_V(CmdPushConstants, U(commandBuffer), (uint64_t)layout, (uint64_t)stageFlags, (uint64_t)offset, (uint64_t)size, U(pValues)); }

/* -- Recording: dynamic state ---------------------------------------------- */

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const void* pViewports)
{ BW_V(CmdSetViewport, U(commandBuffer), (uint64_t)firstViewport, (uint64_t)viewportCount, U(pViewports)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const void* pScissors)
{ BW_V(CmdSetScissor, U(commandBuffer), (uint64_t)firstScissor, (uint64_t)scissorCount, U(pScissors)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{ BW_V(CmdSetLineWidth, U(commandBuffer), F(lineWidth)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{ BW_V(CmdSetDepthBias, U(commandBuffer), F(depthBiasConstantFactor), F(depthBiasClamp), F(depthBiasSlopeFactor)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{ BW_V(CmdSetBlendConstants, U(commandBuffer), U(blendConstants)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds)
{ BW_V(CmdSetDepthBounds, U(commandBuffer), F(minDepthBounds), F(maxDepthBounds)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkFlags faceMask, uint32_t compareMask)
{ BW_V(CmdSetStencilCompareMask, U(commandBuffer), (uint64_t)faceMask, (uint64_t)compareMask); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkFlags faceMask, uint32_t writeMask)
{ BW_V(CmdSetStencilWriteMask, U(commandBuffer), (uint64_t)faceMask, (uint64_t)writeMask); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkFlags faceMask, uint32_t reference)
{ BW_V(CmdSetStencilReference, U(commandBuffer), (uint64_t)faceMask, (uint64_t)reference); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const void* pViewports)
{ BW_V(CmdSetViewportWithCount, U(commandBuffer), (uint64_t)viewportCount, U(pViewports)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const void* pScissors)
{ BW_V(CmdSetScissorWithCount, U(commandBuffer), (uint64_t)scissorCount, U(pScissors)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkFlags cullMode)
{ BW_V(CmdSetCullMode, U(commandBuffer), (uint64_t)cullMode); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkEnum frontFace)
{ BW_V(CmdSetFrontFace, U(commandBuffer), (uint64_t)(uint32_t)frontFace); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkEnum primitiveTopology)
{ BW_V(CmdSetPrimitiveTopology, U(commandBuffer), (uint64_t)(uint32_t)primitiveTopology); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
{ BW_V(CmdSetDepthTestEnable, U(commandBuffer), (uint64_t)depthTestEnable); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
{ BW_V(CmdSetDepthWriteEnable, U(commandBuffer), (uint64_t)depthWriteEnable); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkEnum depthCompareOp)
{ BW_V(CmdSetDepthCompareOp, U(commandBuffer), (uint64_t)(uint32_t)depthCompareOp); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable)
{ BW_V(CmdSetDepthBoundsTestEnable, U(commandBuffer), (uint64_t)depthBoundsTestEnable); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
{ BW_V(CmdSetStencilTestEnable, U(commandBuffer), (uint64_t)stencilTestEnable); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer commandBuffer, VkFlags faceMask, VkEnum failOp, VkEnum passOp, VkEnum depthFailOp, VkEnum compareOp)
{ BW_V(CmdSetStencilOp, U(commandBuffer), (uint64_t)faceMask, (uint64_t)(uint32_t)failOp, (uint64_t)(uint32_t)passOp, (uint64_t)(uint32_t)depthFailOp, (uint64_t)(uint32_t)compareOp); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable)
{ BW_V(CmdSetRasterizerDiscardEnable, U(commandBuffer), (uint64_t)rasterizerDiscardEnable); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
{ BW_V(CmdSetDepthBiasEnable, U(commandBuffer), (uint64_t)depthBiasEnable); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable)
{ BW_V(CmdSetPrimitiveRestartEnable, U(commandBuffer), (uint64_t)primitiveRestartEnable); }

/* -- Recording: draw and dispatch ------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{ BW_V(CmdDraw, U(commandBuffer), (uint64_t)vertexCount, (uint64_t)instanceCount, (uint64_t)firstVertex, (uint64_t)firstInstance); }

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{ BW_V(CmdDrawIndexed, U(commandBuffer), (uint64_t)indexCount, (uint64_t)instanceCount, (uint64_t)firstIndex, (uint64_t)(uint32_t)vertexOffset, (uint64_t)firstInstance); }

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{ BW_V(CmdDrawIndirect, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset, (uint64_t)drawCount, (uint64_t)stride); }

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{ BW_V(CmdDrawIndexedIndirect, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset, (uint64_t)drawCount, (uint64_t)stride); }

VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{ BW_V(CmdDispatch, U(commandBuffer), (uint64_t)groupCountX, (uint64_t)groupCountY, (uint64_t)groupCountZ); }

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset)
{ BW_V(CmdDispatchIndirect, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset); }

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{ BW_V(CmdDispatchBase, U(commandBuffer), (uint64_t)baseGroupX, (uint64_t)baseGroupY, (uint64_t)baseGroupZ, (uint64_t)groupCountX, (uint64_t)groupCountY, (uint64_t)groupCountZ); }

/* -- Recording: transfer ---------------------------------------------------- */

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount, const void* pRegions)
{ BW_V(CmdCopyBuffer, U(commandBuffer), (uint64_t)srcBuffer, (uint64_t)dstBuffer, (uint64_t)regionCount, U(pRegions)); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkEnum srcImageLayout, VkImage dstImage, VkEnum dstImageLayout, uint32_t regionCount, const void* pRegions)
{ BW_V(CmdCopyImage, U(commandBuffer), (uint64_t)srcImage, (uint64_t)(uint32_t)srcImageLayout, (uint64_t)dstImage, (uint64_t)(uint32_t)dstImageLayout, (uint64_t)regionCount, U(pRegions)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkEnum srcImageLayout, VkImage dstImage, VkEnum dstImageLayout, uint32_t regionCount, const void* pRegions, VkEnum filter)
{ BW_V(CmdBlitImage, U(commandBuffer), (uint64_t)srcImage, (uint64_t)(uint32_t)srcImageLayout, (uint64_t)dstImage, (uint64_t)(uint32_t)dstImageLayout, (uint64_t)regionCount, U(pRegions), (uint64_t)(uint32_t)filter); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkEnum dstImageLayout, uint32_t regionCount, const void* pRegions)
{ BW_V(CmdCopyBufferToImage, U(commandBuffer), (uint64_t)srcBuffer, (uint64_t)dstImage, (uint64_t)(uint32_t)dstImageLayout, (uint64_t)regionCount, U(pRegions)); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkEnum srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const void* pRegions)
{ BW_V(CmdCopyImageToBuffer, U(commandBuffer), (uint64_t)srcImage, (uint64_t)(uint32_t)srcImageLayout, (uint64_t)dstBuffer, (uint64_t)regionCount, U(pRegions)); }

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData)
{ BW_V(CmdUpdateBuffer, U(commandBuffer), (uint64_t)dstBuffer, (uint64_t)dstOffset, (uint64_t)dataSize, U(pData)); }

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data)
{ BW_V(CmdFillBuffer, U(commandBuffer), (uint64_t)dstBuffer, (uint64_t)dstOffset, (uint64_t)size, (uint64_t)data); }

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkEnum srcImageLayout, VkImage dstImage, VkEnum dstImageLayout, uint32_t regionCount, const void* pRegions)
{ BW_V(CmdResolveImage, U(commandBuffer), (uint64_t)srcImage, (uint64_t)(uint32_t)srcImageLayout, (uint64_t)dstImage, (uint64_t)(uint32_t)dstImageLayout, (uint64_t)regionCount, U(pRegions)); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const void* pCopyBufferInfo)
{ BW_V(CmdCopyBuffer2, U(commandBuffer), U(pCopyBufferInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer commandBuffer, const void* pCopyImageInfo)
{ BW_V(CmdCopyImage2, U(commandBuffer), U(pCopyImageInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer commandBuffer, const void* pBlitImageInfo)
{ BW_V(CmdBlitImage2, U(commandBuffer), U(pBlitImageInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const void* pCopyBufferToImageInfo)
{ BW_V(CmdCopyBufferToImage2, U(commandBuffer), U(pCopyBufferToImageInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const void* pCopyImageToBufferInfo)
{ BW_V(CmdCopyImageToBuffer2, U(commandBuffer), U(pCopyImageToBufferInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer commandBuffer, const void* pResolveImageInfo)
{ BW_V(CmdResolveImage2, U(commandBuffer), U(pResolveImageInfo)); }

/* -- Recording: clears ------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkEnum imageLayout, const void* pColor, uint32_t rangeCount, const void* pRanges)
{ BW_V(CmdClearColorImage, U(commandBuffer), (uint64_t)image, (uint64_t)(uint32_t)imageLayout, U(pColor), (uint64_t)rangeCount, U(pRanges)); }

VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkEnum imageLayout, const void* pDepthStencil, uint32_t rangeCount, const void* pRanges)
{ BW_V(CmdClearDepthStencilImage, U(commandBuffer), (uint64_t)image, (uint64_t)(uint32_t)imageLayout, U(pDepthStencil), (uint64_t)rangeCount, U(pRanges)); }

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount, const void* pAttachments, uint32_t rectCount, const void* pRects)
{ BW_V(CmdClearAttachments, U(commandBuffer), (uint64_t)attachmentCount, U(pAttachments), (uint64_t)rectCount, U(pRects)); }

/* -- Recording: synchronisation --------------------------------------------- */

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkFlags srcStageMask, VkFlags dstStageMask, VkFlags dependencyFlags, uint32_t memoryBarrierCount, const void* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const void* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const void* pImageMemoryBarriers)
{ BW_V(CmdPipelineBarrier, U(commandBuffer), (uint64_t)srcStageMask, (uint64_t)dstStageMask, (uint64_t)dependencyFlags, (uint64_t)memoryBarrierCount, U(pMemoryBarriers), (uint64_t)bufferMemoryBarrierCount, U(pBufferMemoryBarriers), (uint64_t)imageMemoryBarrierCount, U(pImageMemoryBarriers)); }

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const void* pDependencyInfo)
{ BW_V(CmdPipelineBarrier2, U(commandBuffer), U(pDependencyInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkFlags stageMask)
{ BW_V(CmdSetEvent, U(commandBuffer), (uint64_t)event, (uint64_t)stageMask); }

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkFlags stageMask)
{ BW_V(CmdResetEvent, U(commandBuffer), (uint64_t)event, (uint64_t)stageMask); }

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, VkFlags srcStageMask, VkFlags dstStageMask, uint32_t memoryBarrierCount, const void* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const void* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const void* pImageMemoryBarriers)
{ BW_V(CmdWaitEvents, U(commandBuffer), (uint64_t)eventCount, U(pEvents), (uint64_t)srcStageMask, (uint64_t)dstStageMask, (uint64_t)memoryBarrierCount, U(pMemoryBarriers), (uint64_t)bufferMemoryBarrierCount, U(pBufferMemoryBarriers), (uint64_t)imageMemoryBarrierCount, U(pImageMemoryBarriers)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const void* pDependencyInfo)
{ BW_V(CmdSetEvent2, U(commandBuffer), (uint64_t)event, U(pDependencyInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkFlags64 stageMask)
{ BW_V(CmdResetEvent2, U(commandBuffer), (uint64_t)event, (uint64_t)stageMask); }

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, const void* pDependencyInfos)
{ BW_V(CmdWaitEvents2, U(commandBuffer), (uint64_t)eventCount, U(pEvents), U(pDependencyInfos)); }

/* -- Recording: queries and secondary command buffers ---------------------- */

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkFlags flags)
{ BW_V(CmdBeginQuery, U(commandBuffer), (uint64_t)queryPool, (uint64_t)query, (uint64_t)flags); }

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{ BW_V(CmdEndQuery, U(commandBuffer), (uint64_t)queryPool, (uint64_t)query); }

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{ BW_V(CmdResetQueryPool, U(commandBuffer), (uint64_t)queryPool, (uint64_t)firstQuery, (uint64_t)queryCount); }

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, VkEnum pipelineStage, VkQueryPool queryPool, uint32_t query)
{ BW_V(CmdWriteTimestamp, U(commandBuffer), (uint64_t)(uint32_t)pipelineStage, (uint64_t)queryPool, (uint64_t)query); }

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkFlags64 stage, VkQueryPool queryPool, uint32_t query)
{ BW_V(CmdWriteTimestamp2, U(commandBuffer), (uint64_t)stage, (uint64_t)queryPool, (uint64_t)query); }

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize stride, VkFlags flags)
{ BW_V(CmdCopyQueryPoolResults, U(commandBuffer), (uint64_t)queryPool, (uint64_t)firstQuery, (uint64_t)queryCount, (uint64_t)dstBuffer, (uint64_t)dstOffset, (uint64_t)stride, (uint64_t)flags); }

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
{ BW_V(CmdExecuteCommands, U(commandBuffer), (uint64_t)commandBufferCount, U(pCommandBuffers)); }

/* -- Core commands the Vulkan 1.4 audit added ------------------------------ */
/*
 * These are here because they are CORE, not because the D3D9 path calls them.
 * Wine's winevulkan fills one dispatch-table slot per command by name out of
 * vkGetDeviceProcAddr; a name this ICD does not export leaves that slot null,
 * and a caller that does not check calls through the hole. A device run ended
 * exactly that way.
 */

VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory, VkDeviceSize* pCommittedMemoryInBytes)
{ BW_V(GetDeviceMemoryCommitment, U(device), (uint64_t)memory, U(pCommittedMemoryInBytes)); }

VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t* pSparseMemoryRequirementCount, void* pSparseMemoryRequirements)
{ BW_V(GetImageSparseMemoryRequirements, U(device), (uint64_t)image, U(pSparseMemoryRequirementCount), U(pSparseMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements2(VkDevice device, const void* pInfo, uint32_t* pSparseMemoryRequirementCount, void* pSparseMemoryRequirements)
{ BW_V(GetImageSparseMemoryRequirements2, U(device), U(pInfo), U(pSparseMemoryRequirementCount), U(pSparseMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkEnum format, VkEnum type, VkEnum samples, VkFlags usage, VkEnum tiling, uint32_t* pPropertyCount, void* pProperties)
{ BW_V(GetPhysicalDeviceSparseImageFormatProperties, U(physicalDevice), (uint64_t)(uint32_t)format, (uint64_t)(uint32_t)type, (uint64_t)(uint32_t)samples, (uint64_t)usage, (uint64_t)(uint32_t)tiling, U(pPropertyCount), U(pProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice, const void* pFormatInfo, uint32_t* pPropertyCount, void* pProperties)
{ BW_V(GetPhysicalDeviceSparseImageFormatProperties2, U(physicalDevice), U(pFormatInfo), U(pPropertyCount), U(pProperties)); }

VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const void* pBindInfo, VkFence fence)
{ BW_R(QueueBindSparse, U(queue), (uint64_t)bindInfoCount, U(pBindInfo), (uint64_t)fence); }

VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice device, VkRenderPass renderPass, void* pGranularity)
{ BW_V(GetRenderAreaGranularity, U(device), (uint64_t)renderPass, U(pGranularity)); }

VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularity(VkDevice device, const void* pRenderingAreaInfo, void* pGranularity)
{ BW_V(GetRenderingAreaGranularity, U(device), U(pRenderingAreaInfo), U(pGranularity)); }

VKAPI_ATTR void VKAPI_CALL vkTrimCommandPool(VkDevice device, VkCommandPool commandPool, VkFlags flags)
{ BW_V(TrimCommandPool, U(device), (uint64_t)commandPool, (uint64_t)flags); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
{ BW_V(CmdSetDeviceMask, U(commandBuffer), (uint64_t)deviceMask); }

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount, void* pPhysicalDeviceGroupProperties)
{ BW_R(EnumeratePhysicalDeviceGroups, U(instance), U(pPhysicalDeviceGroupCount), U(pPhysicalDeviceGroupProperties)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeatures(VkDevice device, uint32_t heapIndex, uint32_t localDeviceIndex, uint32_t remoteDeviceIndex, VkFlags* pPeerMemoryFeatures)
{ BW_V(GetDeviceGroupPeerMemoryFeatures, U(device), (uint64_t)heapIndex, (uint64_t)localDeviceIndex, (uint64_t)remoteDeviceIndex, U(pPeerMemoryFeatures)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSamplerYcbcrConversion(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkSamplerYcbcrConversion* pYcbcrConversion)
{ BW_R(CreateSamplerYcbcrConversion, U(device), U(pCreateInfo), U(pAllocator), U(pYcbcrConversion)); }

VKAPI_ATTR void VKAPI_CALL vkDestroySamplerYcbcrConversion(VkDevice device, VkSamplerYcbcrConversion ycbcrConversion, const void* pAllocator)
{ BW_V(DestroySamplerYcbcrConversion, U(device), (uint64_t)ycbcrConversion, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolProperties(VkPhysicalDevice physicalDevice, uint32_t* pToolCount, void* pToolProperties)
{ BW_R(GetPhysicalDeviceToolProperties, U(physicalDevice), U(pToolCount), U(pToolProperties)); }

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{ BW_V(CmdDrawIndirectCount, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset, (uint64_t)countBuffer, (uint64_t)countBufferOffset, (uint64_t)maxDrawCount, (uint64_t)stride); }

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{ BW_V(CmdDrawIndexedIndirectCount, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset, (uint64_t)countBuffer, (uint64_t)countBufferOffset, (uint64_t)maxDrawCount, (uint64_t)stride); }

/* The three that return a 64-bit value; see BW_A. */
VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device, const void* pInfo)
{ BW_A(GetBufferDeviceAddress, U(device), U(pInfo)); }

VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress(VkDevice device, const void* pInfo)
{ BW_A(GetBufferOpaqueCaptureAddress, U(device), U(pInfo)); }

VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice device, const void* pInfo)
{ BW_A(GetDeviceMemoryOpaqueCaptureAddress, U(device), U(pInfo)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePrivateDataSlot(VkDevice device, const void* pCreateInfo, const void* pAllocator, VkPrivateDataSlot* pPrivateDataSlot)
{ BW_R(CreatePrivateDataSlot, U(device), U(pCreateInfo), U(pAllocator), U(pPrivateDataSlot)); }

VKAPI_ATTR void VKAPI_CALL vkDestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot privateDataSlot, const void* pAllocator)
{ BW_V(DestroyPrivateDataSlot, U(device), (uint64_t)privateDataSlot, U(pAllocator)); }

VKAPI_ATTR VkResult VKAPI_CALL vkSetPrivateData(VkDevice device, VkEnum objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data)
{ BW_R(SetPrivateData, U(device), (uint64_t)(uint32_t)objectType, objectHandle, (uint64_t)privateDataSlot, data); }

VKAPI_ATTR void VKAPI_CALL vkGetPrivateData(VkDevice device, VkEnum objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t* pData)
{ BW_V(GetPrivateData, U(device), (uint64_t)(uint32_t)objectType, objectHandle, (uint64_t)privateDataSlot, U(pData)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(VkDevice device, const void* pInfo, void* pMemoryRequirements)
{ BW_V(GetDeviceBufferMemoryRequirements, U(device), U(pInfo), U(pMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device, const void* pInfo, void* pMemoryRequirements)
{ BW_V(GetDeviceImageMemoryRequirements, U(device), U(pInfo), U(pMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const void* pInfo, uint32_t* pSparseMemoryRequirementCount, void* pSparseMemoryRequirements)
{ BW_V(GetDeviceImageSparseMemoryRequirements, U(device), U(pInfo), U(pSparseMemoryRequirementCount), U(pSparseMemoryRequirements)); }

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2(VkDevice device, VkImage image, const void* pSubresource, void* pLayout)
{ BW_V(GetImageSubresourceLayout2, U(device), (uint64_t)image, U(pSubresource), U(pLayout)); }

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayout(VkDevice device, const void* pInfo, void* pLayout)
{ BW_V(GetDeviceImageSubresourceLayout, U(device), U(pInfo), U(pLayout)); }

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory2(VkDevice device, const void* pMemoryMapInfo, void** ppData)
{ BW_R(MapMemory2, U(device), U(pMemoryMapInfo), U(ppData)); }

VKAPI_ATTR VkResult VKAPI_CALL vkUnmapMemory2(VkDevice device, const void* pMemoryUnmapInfo)
{ BW_R(UnmapMemory2, U(device), U(pMemoryUnmapInfo)); }

VKAPI_ATTR VkResult VKAPI_CALL vkTransitionImageLayout(VkDevice device, uint32_t transitionCount, const void* pTransitions)
{ BW_R(TransitionImageLayout, U(device), (uint64_t)transitionCount, U(pTransitions)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkEnum indexType)
{ BW_V(CmdBindIndexBuffer2, U(commandBuffer), (uint64_t)buffer, (uint64_t)offset, (uint64_t)size, (uint64_t)(uint32_t)indexType); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStipple(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern)
{ BW_V(CmdSetLineStipple, U(commandBuffer), (uint64_t)lineStippleFactor, (uint64_t)lineStipplePattern); }

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet(VkCommandBuffer commandBuffer, VkEnum pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const void* pDescriptorWrites)
{ BW_V(CmdPushDescriptorSet, U(commandBuffer), (uint64_t)(uint32_t)pipelineBindPoint, (uint64_t)layout, (uint64_t)set, (uint64_t)descriptorWriteCount, U(pDescriptorWrites)); }

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout, uint32_t set, const void* pData)
{ BW_V(CmdPushDescriptorSetWithTemplate, U(commandBuffer), (uint64_t)descriptorUpdateTemplate, (uint64_t)layout, (uint64_t)set, U(pData)); }

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2(VkCommandBuffer commandBuffer, const void* pBindDescriptorSetsInfo)
{ BW_V(CmdBindDescriptorSets2, U(commandBuffer), U(pBindDescriptorSetsInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2(VkCommandBuffer commandBuffer, const void* pPushConstantsInfo)
{ BW_V(CmdPushConstants2, U(commandBuffer), U(pPushConstantsInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2(VkCommandBuffer commandBuffer, const void* pPushDescriptorSetInfo)
{ BW_V(CmdPushDescriptorSet2, U(commandBuffer), U(pPushDescriptorSetInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer commandBuffer, const void* pPushDescriptorSetWithTemplateInfo)
{ BW_V(CmdPushDescriptorSetWithTemplate2, U(commandBuffer), U(pPushDescriptorSetWithTemplateInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetRenderingAttachmentLocations(VkCommandBuffer commandBuffer, const void* pLocationInfo)
{ BW_V(CmdSetRenderingAttachmentLocations, U(commandBuffer), U(pLocationInfo)); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetRenderingInputAttachmentIndices(VkCommandBuffer commandBuffer, const void* pInputAttachmentIndexInfo)
{ BW_V(CmdSetRenderingInputAttachmentIndices, U(commandBuffer), U(pInputAttachmentIndexInfo)); }

/* -- Swapchain maintenance1 ------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkReleaseSwapchainImagesEXT(VkDevice device, const void* pReleaseInfo)
{ BW_R(ReleaseSwapchainImagesEXT, U(device), U(pReleaseInfo)); }

/* -- Procedure lookup ------------------------------------------------------ */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);

struct bw_proc {
    const char* name;
    PFN_vkVoidFunction fn;
};

/* Built from the same list the host dispatches, so the exported set and the
 * bridge's operation table can never disagree. */
#define BW_PROC_ENTRY(name, ordinal) { "vk" #name, (PFN_vkVoidFunction)vk##name },
/* An alias resolves to the core command's own entry point: the trap carries
 * the operation number, not the name, so there is nothing for a separate
 * function to do. */
#define BW_ALIAS_ENTRY(alias, core) { "vk" #alias, (PFN_vkVoidFunction)vk##core },
static const struct bw_proc bw_procs[] = {
    BOXEDWINE_X64_VK_COMMANDS(BW_PROC_ENTRY)
    BOXEDWINE_X64_VK_ALIASES(BW_ALIAS_ENTRY)
    { "vkGetInstanceProcAddr", (PFN_vkVoidFunction)vkGetInstanceProcAddr },
    { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)vkGetDeviceProcAddr },
};
#undef BW_ALIAS_ENTRY
#undef BW_PROC_ENTRY

static const struct bw_proc* bw_find(const char* name)
{
    unsigned i;
    if (!name) {
        return 0;
    }
    for (i = 0; i < sizeof bw_procs / sizeof bw_procs[0]; ++i) {
        if (!strcmp(bw_procs[i].name, name)) {
            return &bw_procs[i];
        }
    }
    return 0;
}

/* One lookup path for both GetProcAddr forms. The host is asked about every
 * name, including the ones this shim does not carry, because that answer --
 * printed as `missing=<name>` -- is how a device run says what DXVK wanted
 * that the bridge has yet to grow. A name the shim has and the host driver
 * does not still resolves to NULL, which is what a caller expects for an
 * extension the driver lacks. */
static PFN_vkVoidFunction bw_proc_addr(uint64_t handle, const char* pName)
{
    const struct bw_proc* proc;
    uint64_t args[3];
    int64_t answer;

    if (!pName) {
        return 0;
    }
    /* Resolving the lookup functions themselves must not need the host: the
     * Vulkan contract says vkGetInstanceProcAddr(NULL, "vkGetInstanceProcAddr")
     * works before any instance exists. */
    if (!strcmp(pName, "vkGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    }
    if (!strcmp(pName, "vkGetDeviceProcAddr")) {
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }
    proc = bw_find(pName);
    args[0] = handle;
    args[1] = U(pName);
    args[2] = proc ? 1u : 0u;
    answer = bw_call(BOXEDWINE_X64_VK_OP_PROC_ADDR, args, 3);
    if (answer != 1 || !proc) {
        return 0;
    }
    return proc->fn;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{ return bw_proc_addr(U(instance), pName); }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
{ return bw_proc_addr(U(device), pName); }

/* The loader-facing aliases. Nothing in Wine's chain uses them -- winex11.drv
 * dlopens this library directly rather than going through a Khronos loader --
 * but an ICD that omits them cannot be dropped behind one either, and they
 * cost two symbols. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName)
{ return vkGetInstanceProcAddr(instance, pName); }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* pName)
{ return vkGetInstanceProcAddr(instance, pName); }

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion)
{
    if (!pSupportedVersion) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (*pSupportedVersion > 5) {
        *pSupportedVersion = 5;
    }
    return VK_SUCCESS;
}
