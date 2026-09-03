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
typedef uint64_t VkDeviceSize;
typedef int32_t  VkEnum;      /* every Vulkan enum is a 32-bit signed int */

/* Dispatchable handles are pointers. */
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;

/* Non-dispatchable handles are 64 bits wide on a 64-bit platform. */
typedef uint64_t VkNonDispatchable;
typedef VkNonDispatchable VkDeviceMemory;
typedef VkNonDispatchable VkBuffer;
typedef VkNonDispatchable VkImage;
typedef VkNonDispatchable VkImageView;
typedef VkNonDispatchable VkFence;
typedef VkNonDispatchable VkSemaphore;
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

static int64_t bw_call(uint64_t op, uint64_t* args, uint64_t count)
{
    if (!bw_usable()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return boxedwine_x64_vulkan_call(op, args, count);
}

#define U(x) ((uint64_t)(uintptr_t)(x))

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
