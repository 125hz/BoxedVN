/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

// The x86-64 guest Vulkan bridge: the host half of hostcall 0x7fff0003.
//
// The 32-bit lane's `int 0x9A` path exists because a 32-bit guest pointer is
// not a host address and a 32-bit guest's Vulkan structures are not laid out
// the way the host's are; source/vulkan/vk_host*.cpp is 2.7 MB of generated
// code that copies every structure across that gap. Neither problem exists on
// this lane:
//
//   1. The bridge is served only to the process holding the identity map --
//      the one process per session FEX translates -- where a guest address is
//      the host address. That is the same rule the DXMT unix call enforces
//      and docs/KNOWN_LIMITATIONS_IOS.md section 4 records, and for the same
//      reason: a host Vulkan handle is a host pointer.
//   2. Every Vulkan structure has an identical layout on x86-64 System V and
//      on arm64 AAPCS64. Both are LP64, every Vulkan scalar takes its natural
//      alignment on both, and Vulkan declares no bitfields and no packed
//      members. There is nothing to translate.
//
// So this file marshals nothing. It validates the guest's argument array,
// casts the words to the real Vulkan types, and calls the driver. What it
// must never do is call *through* a guest pointer, because guest code is
// x86-64 and the host is arm64: `pAllocator` is forced to NULL on every
// command that takes one -- which is what the IA-32 marshal does too -- and a
// command whose parameters include a guest callback is not in the table.
//
// The trust boundary is the identity-map check. Beyond it the caller is the
// session's own Wine, the same caller the DXMT unix call already accepts, and
// the pointers it passes are dereferenced by MoltenVK rather than by
// BoxedWine. The argument array itself is always validated first, and the one
// structure this file reads for itself (the Xlib surface create info) is read
// through the page table rather than dereferenced.

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "vulkanbridge64.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "boxedwine_x64_vulkan_bridge.h"

#ifdef BOXEDWINE_VULKAN
#include "knativesystem.h"
#include "kvulkan.h"
#include "../x11/x11.h"
#include "vk_host.h"
#include <SDL_vulkan.h>
#endif

#include <atomic>
#include <string.h>

namespace {

// ---- Guest memory -----------------------------------------------------------

bool guestRange(KMemory64* memory, U64 address, U64 bytes, bool write) {
    if (!memory || address == 0 || bytes == 0) {
        return false;
    }
    const U64 first = address >> K64_PAGE_SHIFT;
    const U64 last = (address + bytes - 1) >> K64_PAGE_SHIFT;
    if (last < first) {
        return false;
    }
    for (U64 page = first; page <= last; ++page) {
        const U32 flags = memory->getPageFlags(page);
        if (!(flags & K64_PAGE_MAPPED) || !(flags & K64_PAGE_READ)) {
            return false;
        }
        if (write && !(flags & K64_PAGE_WRITE)) {
            return false;
        }
    }
    return true;
}

// ---- Diagnostics ------------------------------------------------------------

std::atomic<U32> gCallOrdinal{0};

#ifdef BOXEDWINE_VULKAN

std::atomic<U32> gNamedCalls{0};
std::atomic<U32> gPresents{0};
std::atomic<U32> gRefusals{0};

// Budgeted so a program that renders does not drown the log: the first 64
// dispatched commands are named, and after that only the ones that fail, the
// ones the table does not carry, and one line every 60 presents.
const U32 kNamedCallBudget = 64;
const U32 kRefusalBudget = 64;

// ---- The command table ------------------------------------------------------
//
// One dense index per command in BOXEDWINE_X64_VK_COMMANDS, so the operation
// number (which is the IA-32 lane's ordinal plus a base, and therefore sparse)
// can be turned into an array slot for the resolved function pointer.

enum VulkanBridgeCommand {
#define VKB_ENUM(name, ordinal) VKB_##name,
    BOXEDWINE_X64_VK_COMMANDS(VKB_ENUM)
#undef VKB_ENUM
    VKB_COUNT
};

const char* const kCommandName[VKB_COUNT] = {
#define VKB_NAME(name, ordinal) "vk" #name,
    BOXEDWINE_X64_VK_COMMANDS(VKB_NAME)
#undef VKB_NAME
};

const U64 kCommandOp[VKB_COUNT] = {
#define VKB_OP(name, ordinal) (U64)(BOXEDWINE_X64_VK_OP_VK_BASE + (ordinal)),
    BOXEDWINE_X64_VK_COMMANDS(VKB_OP)
#undef VKB_OP
};

int commandIndexForOp(U64 op) {
    for (int i = 0; i < VKB_COUNT; ++i) {
        if (kCommandOp[i] == op) {
            return i;
        }
    }
    return -1;
}

// ---- The host driver --------------------------------------------------------

PFN_vkGetInstanceProcAddr gGetInstanceProcAddr = nullptr;
bool gDriverTried = false;
VkInstance gInstance = VK_NULL_HANDLE;
PFN_vkVoidFunction gResolved[VKB_COUNT] = {};
VkInstance gResolvedFor[VKB_COUNT] = {};

PFN_vkGetInstanceProcAddr hostDriver() {
    if (!gDriverTried) {
        BOXEDWINE_CRITICAL_SECTION;
        if (!gDriverTried) {
            gDriverTried = true;
            if (SDL_Vulkan_LoadLibrary(NULL) == 0) {
                gGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
                    SDL_Vulkan_GetVkGetInstanceProcAddr();
            }
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE driver=%s gipa=%s",
                     gGetInstanceProcAddr ? "loaded" : "none",
                     gGetInstanceProcAddr ? "ok" : SDL_GetError());
        }
    }
    return gGetInstanceProcAddr;
}

// MoltenVK is used as the driver directly, with no Khronos loader in front of
// it, so its instance-level vkGetInstanceProcAddr answers for the device-level
// commands as well. Resolving through the instance rather than through
// vkGetDeviceProcAddr keeps one lookup path for commands whose first argument
// is a VkQueue or a VkCommandBuffer, which carry no VkDevice to look up with.
PFN_vkVoidFunction hostProc(int index) {
    PFN_vkGetInstanceProcAddr gipa = hostDriver();
    if (!gipa || index < 0 || index >= VKB_COUNT) {
        return nullptr;
    }
    if (gResolved[index] && gResolvedFor[index] == gInstance) {
        return gResolved[index];
    }
    PFN_vkVoidFunction fn = gipa(gInstance, kCommandName[index]);
    if (!fn && gInstance != VK_NULL_HANDLE) {
        fn = gipa(VK_NULL_HANDLE, kCommandName[index]);
    }
    gResolved[index] = fn;
    gResolvedFor[index] = gInstance;
    return fn;
}

// ---- vkCreateXlibSurfaceKHR -------------------------------------------------
//
// The one command that is not forwarded. The guest hands a
// VkXlibSurfaceCreateInfoKHR naming a Display and a Window from BoxedWine's
// own X server, and no such X server exists for MoltenVK to talk to; the
// IA-32 lane special-cases the same command (BOXED_vkCreateXlibSurfaceKHR in
// vulkancommon.cpp) and creates the surface from the XWindow through
// KNativeSystem::getVulkan(), which is the host presentation path the 64-bit
// lane already uses for everything else. This does the same, reading the
// window id through the page table rather than dereferencing the guest
// pointer.
//
// x86-64 VkXlibSurfaceCreateInfoKHR: sType 0, pad 4, pNext 8, flags 16,
// pad 20, dpy 24, window 32. The IA-32 lane reads the window as the fifth
// 32-bit word for the same reason.
const U64 kXlibSurfaceWindowOffset = 32;

S64 createXlibSurface(KMemory64* memory, U64 instance, U64 createInfo,
                      U64 surfaceOut) {
    if (!guestRange(memory, createInfo, kXlibSurfaceWindowOffset + 8, false) ||
        !guestRange(memory, surfaceOut, 8, true)) {
        return BOXEDWINE_X64_VK_E_FAULT;
    }
    U64 windowId = 0;
    memory->memcpyFromGuest(&windowId, createInfo + kXlibSurfaceWindowOffset,
                            sizeof(windowId));
    KVulkanPtr vulkanWnd = KNativeSystem::getVulkan();
    if (!vulkanWnd) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    XWindowPtr xWindow = XServer::getServer()->getWindow((U32)windowId);
    void* surface = vulkanWnd->createVulkanSurface(xWindow, (void*)instance);
    if (!surface) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const U64 handle = (U64)surface;
    memory->memcpyToGuest(surfaceOut, &handle, sizeof(handle));
    // Wine builds throwaway surfaces purely to probe adapter capabilities;
    // only a real presentation surface may take over the fake-fullscreen
    // target, which is the rule the IA-32 lane already follows.
    if (vulkanWnd->isPresentationSurface(surface)) {
        XServer::getServer()->setFakeFullScreenWindow(xWindow);
    }
    klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkCreateXlibSurfaceKHR "
             "window=0x%x surface=0x%llx status=0",
             (U32)windowId, (unsigned long long)handle);
    return VK_SUCCESS;
}

// ---- The dispatcher ---------------------------------------------------------

#define A(n) args[n]
#define P(n) ((void*)(uintptr_t)args[n])
#define CP(n) ((const void*)(uintptr_t)args[n])
#define U32A(n) ((uint32_t)args[n])

// Every command below is called through its real prototype, so the compiler
// checks each cast. Nothing here is generated; a command is in the table
// because it was added deliberately.
S64 dispatchCommand(int index, KMemory64* memory, const U64* args, U64 count) {
    (void)count;
    PFN_vkVoidFunction raw = hostProc(index);
    if (!raw) {
        return BOXEDWINE_X64_VK_E_NOPROC;
    }
    switch (index) {
    case VKB_EnumerateInstanceVersion:
        return (S64)((PFN_vkEnumerateInstanceVersion)raw)((uint32_t*)P(0));
    case VKB_EnumerateInstanceLayerProperties:
        return (S64)((PFN_vkEnumerateInstanceLayerProperties)raw)(
            (uint32_t*)P(0), (VkLayerProperties*)P(1));
    case VKB_EnumerateInstanceExtensionProperties:
        return (S64)((PFN_vkEnumerateInstanceExtensionProperties)raw)(
            (const char*)CP(0), (uint32_t*)P(1), (VkExtensionProperties*)P(2));
    case VKB_CreateInstance: {
        VkInstance* out = (VkInstance*)P(2);
        const VkResult result = ((PFN_vkCreateInstance)raw)(
            (const VkInstanceCreateInfo*)CP(0), nullptr, out);
        if (result == VK_SUCCESS && out && *out) {
            // Every later resolution goes through this instance; a second
            // instance simply replaces it and the per-command cache is keyed
            // on which instance resolved it.
            gInstance = *out;
        }
        return (S64)result;
    }
    case VKB_DestroyInstance: {
        VkInstance instance = (VkInstance)P(0);
        ((PFN_vkDestroyInstance)raw)(instance, nullptr);
        if (gInstance == instance) {
            gInstance = VK_NULL_HANDLE;
        }
        return 0;
    }
    case VKB_EnumeratePhysicalDevices:
        return (S64)((PFN_vkEnumeratePhysicalDevices)raw)(
            (VkInstance)P(0), (uint32_t*)P(1), (VkPhysicalDevice*)P(2));

    case VKB_GetPhysicalDeviceProperties:
        ((PFN_vkGetPhysicalDeviceProperties)raw)(
            (VkPhysicalDevice)P(0), (VkPhysicalDeviceProperties*)P(1));
        return 0;
    case VKB_GetPhysicalDeviceProperties2:
        ((PFN_vkGetPhysicalDeviceProperties2)raw)(
            (VkPhysicalDevice)P(0), (VkPhysicalDeviceProperties2*)P(1));
        return 0;
    case VKB_GetPhysicalDeviceFeatures:
        ((PFN_vkGetPhysicalDeviceFeatures)raw)(
            (VkPhysicalDevice)P(0), (VkPhysicalDeviceFeatures*)P(1));
        return 0;
    case VKB_GetPhysicalDeviceFeatures2:
        ((PFN_vkGetPhysicalDeviceFeatures2)raw)(
            (VkPhysicalDevice)P(0), (VkPhysicalDeviceFeatures2*)P(1));
        return 0;
    case VKB_GetPhysicalDeviceFormatProperties:
        ((PFN_vkGetPhysicalDeviceFormatProperties)raw)(
            (VkPhysicalDevice)P(0), (VkFormat)U32A(1),
            (VkFormatProperties*)P(2));
        return 0;
    case VKB_GetPhysicalDeviceFormatProperties2:
        ((PFN_vkGetPhysicalDeviceFormatProperties2)raw)(
            (VkPhysicalDevice)P(0), (VkFormat)U32A(1),
            (VkFormatProperties2*)P(2));
        return 0;
    case VKB_GetPhysicalDeviceImageFormatProperties:
        return (S64)((PFN_vkGetPhysicalDeviceImageFormatProperties)raw)(
            (VkPhysicalDevice)P(0), (VkFormat)U32A(1), (VkImageType)U32A(2),
            (VkImageTiling)U32A(3), (VkImageUsageFlags)U32A(4),
            (VkImageCreateFlags)U32A(5), (VkImageFormatProperties*)P(6));
    case VKB_GetPhysicalDeviceImageFormatProperties2:
        return (S64)((PFN_vkGetPhysicalDeviceImageFormatProperties2)raw)(
            (VkPhysicalDevice)P(0),
            (const VkPhysicalDeviceImageFormatInfo2*)CP(1),
            (VkImageFormatProperties2*)P(2));
    case VKB_GetPhysicalDeviceQueueFamilyProperties:
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)raw)(
            (VkPhysicalDevice)P(0), (uint32_t*)P(1),
            (VkQueueFamilyProperties*)P(2));
        return 0;
    case VKB_GetPhysicalDeviceQueueFamilyProperties2:
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties2)raw)(
            (VkPhysicalDevice)P(0), (uint32_t*)P(1),
            (VkQueueFamilyProperties2*)P(2));
        return 0;
    case VKB_GetPhysicalDeviceMemoryProperties:
        ((PFN_vkGetPhysicalDeviceMemoryProperties)raw)(
            (VkPhysicalDevice)P(0), (VkPhysicalDeviceMemoryProperties*)P(1));
        return 0;
    case VKB_GetPhysicalDeviceMemoryProperties2:
        ((PFN_vkGetPhysicalDeviceMemoryProperties2)raw)(
            (VkPhysicalDevice)P(0), (VkPhysicalDeviceMemoryProperties2*)P(1));
        return 0;
    case VKB_EnumerateDeviceLayerProperties:
        return (S64)((PFN_vkEnumerateDeviceLayerProperties)raw)(
            (VkPhysicalDevice)P(0), (uint32_t*)P(1), (VkLayerProperties*)P(2));
    case VKB_EnumerateDeviceExtensionProperties:
        return (S64)((PFN_vkEnumerateDeviceExtensionProperties)raw)(
            (VkPhysicalDevice)P(0), (const char*)CP(1), (uint32_t*)P(2),
            (VkExtensionProperties*)P(3));

    case VKB_CreateDevice:
        return (S64)((PFN_vkCreateDevice)raw)(
            (VkPhysicalDevice)P(0), (const VkDeviceCreateInfo*)CP(1), nullptr,
            (VkDevice*)P(3));
    case VKB_DestroyDevice:
        ((PFN_vkDestroyDevice)raw)((VkDevice)P(0), nullptr);
        return 0;
    case VKB_GetDeviceQueue:
        ((PFN_vkGetDeviceQueue)raw)((VkDevice)P(0), U32A(1), U32A(2),
                                    (VkQueue*)P(3));
        return 0;
    case VKB_DeviceWaitIdle:
        return (S64)((PFN_vkDeviceWaitIdle)raw)((VkDevice)P(0));
    case VKB_QueueWaitIdle:
        return (S64)((PFN_vkQueueWaitIdle)raw)((VkQueue)P(0));
    case VKB_QueueSubmit:
        return (S64)((PFN_vkQueueSubmit)raw)(
            (VkQueue)P(0), U32A(1), (const VkSubmitInfo*)CP(2),
            (VkFence)A(3));

    case VKB_AllocateMemory:
        return (S64)((PFN_vkAllocateMemory)raw)(
            (VkDevice)P(0), (const VkMemoryAllocateInfo*)CP(1), nullptr,
            (VkDeviceMemory*)P(3));
    case VKB_FreeMemory:
        ((PFN_vkFreeMemory)raw)((VkDevice)P(0), (VkDeviceMemory)A(1), nullptr);
        return 0;
    case VKB_MapMemory: {
        // The returned pointer is a host address. Under the identity map that
        // is also a guest address, so the guest can dereference it -- but only
        // a 64-bit guest can hold it. Wine's WoW64 layer refuses a mapping
        // above 4 GiB for a 32-bit caller ("returned mapping %p does not fit
        // 32-bit pointer" in wine_vkMapMemory2KHR), which is why the 32-bit
        // path depends on VK_EXT_external_memory_host: Wine places the memory
        // itself, below 4 GiB, and imports it. Nothing here has to move the
        // pointer; the extension has to be present on the driver, and the
        // witness below is what says whether the address that came back was
        // usable by the caller that asked for it.
        const VkResult result = ((PFN_vkMapMemory)raw)(
            (VkDevice)P(0), (VkDeviceMemory)A(1), (VkDeviceSize)A(2),
            (VkDeviceSize)A(3), (VkMemoryMapFlags)U32A(4), (void**)P(5));
        if (result == VK_SUCCESS && guestRange(memory, A(5), 8, true)) {
            U64 mapped = 0;
            memory->memcpyFromGuest(&mapped, A(5), sizeof(mapped));
            static std::atomic<U32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                klog_fmt("BOXEDWINE_X64_VULKAN_MAP memory=0x%llx offset=0x%llx "
                         "size=0x%llx address=0x%llx low4g=%d",
                         (unsigned long long)A(1), (unsigned long long)A(2),
                         (unsigned long long)A(3), (unsigned long long)mapped,
                         (mapped >> 32) == 0 ? 1 : 0);
            }
        }
        return (S64)result;
    }
    case VKB_UnmapMemory:
        ((PFN_vkUnmapMemory)raw)((VkDevice)P(0), (VkDeviceMemory)A(1));
        return 0;
    case VKB_FlushMappedMemoryRanges:
        return (S64)((PFN_vkFlushMappedMemoryRanges)raw)(
            (VkDevice)P(0), U32A(1), (const VkMappedMemoryRange*)CP(2));
    case VKB_InvalidateMappedMemoryRanges:
        return (S64)((PFN_vkInvalidateMappedMemoryRanges)raw)(
            (VkDevice)P(0), U32A(1), (const VkMappedMemoryRange*)CP(2));
    case VKB_GetBufferMemoryRequirements:
        ((PFN_vkGetBufferMemoryRequirements)raw)(
            (VkDevice)P(0), (VkBuffer)A(1), (VkMemoryRequirements*)P(2));
        return 0;
    case VKB_BindBufferMemory:
        return (S64)((PFN_vkBindBufferMemory)raw)(
            (VkDevice)P(0), (VkBuffer)A(1), (VkDeviceMemory)A(2),
            (VkDeviceSize)A(3));
    case VKB_GetImageMemoryRequirements:
        ((PFN_vkGetImageMemoryRequirements)raw)(
            (VkDevice)P(0), (VkImage)A(1), (VkMemoryRequirements*)P(2));
        return 0;
    case VKB_BindImageMemory:
        return (S64)((PFN_vkBindImageMemory)raw)(
            (VkDevice)P(0), (VkImage)A(1), (VkDeviceMemory)A(2),
            (VkDeviceSize)A(3));
    case VKB_GetMemoryHostPointerPropertiesEXT:
        return (S64)((PFN_vkGetMemoryHostPointerPropertiesEXT)raw)(
            (VkDevice)P(0), (VkExternalMemoryHandleTypeFlagBits)U32A(1),
            CP(2), (VkMemoryHostPointerPropertiesEXT*)P(3));

    case VKB_CreateBuffer:
        return (S64)((PFN_vkCreateBuffer)raw)(
            (VkDevice)P(0), (const VkBufferCreateInfo*)CP(1), nullptr,
            (VkBuffer*)P(3));
    case VKB_DestroyBuffer:
        ((PFN_vkDestroyBuffer)raw)((VkDevice)P(0), (VkBuffer)A(1), nullptr);
        return 0;
    case VKB_CreateImage:
        return (S64)((PFN_vkCreateImage)raw)(
            (VkDevice)P(0), (const VkImageCreateInfo*)CP(1), nullptr,
            (VkImage*)P(3));
    case VKB_DestroyImage:
        ((PFN_vkDestroyImage)raw)((VkDevice)P(0), (VkImage)A(1), nullptr);
        return 0;
    case VKB_CreateImageView:
        return (S64)((PFN_vkCreateImageView)raw)(
            (VkDevice)P(0), (const VkImageViewCreateInfo*)CP(1), nullptr,
            (VkImageView*)P(3));
    case VKB_DestroyImageView:
        ((PFN_vkDestroyImageView)raw)((VkDevice)P(0), (VkImageView)A(1),
                                      nullptr);
        return 0;

    case VKB_CreateFence:
        return (S64)((PFN_vkCreateFence)raw)(
            (VkDevice)P(0), (const VkFenceCreateInfo*)CP(1), nullptr,
            (VkFence*)P(3));
    case VKB_DestroyFence:
        ((PFN_vkDestroyFence)raw)((VkDevice)P(0), (VkFence)A(1), nullptr);
        return 0;
    case VKB_ResetFences:
        return (S64)((PFN_vkResetFences)raw)(
            (VkDevice)P(0), U32A(1), (const VkFence*)CP(2));
    case VKB_GetFenceStatus:
        return (S64)((PFN_vkGetFenceStatus)raw)((VkDevice)P(0), (VkFence)A(1));
    case VKB_WaitForFences:
        return (S64)((PFN_vkWaitForFences)raw)(
            (VkDevice)P(0), U32A(1), (const VkFence*)CP(2), (VkBool32)U32A(3),
            (uint64_t)A(4));
    case VKB_CreateSemaphore:
        return (S64)((PFN_vkCreateSemaphore)raw)(
            (VkDevice)P(0), (const VkSemaphoreCreateInfo*)CP(1), nullptr,
            (VkSemaphore*)P(3));
    case VKB_DestroySemaphore:
        ((PFN_vkDestroySemaphore)raw)((VkDevice)P(0), (VkSemaphore)A(1),
                                      nullptr);
        return 0;

    case VKB_DestroySurfaceKHR:
        ((PFN_vkDestroySurfaceKHR)raw)((VkInstance)P(0), (VkSurfaceKHR)A(1),
                                       nullptr);
        return 0;
    case VKB_GetPhysicalDeviceSurfaceSupportKHR:
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceSupportKHR)raw)(
            (VkPhysicalDevice)P(0), U32A(1), (VkSurfaceKHR)A(2),
            (VkBool32*)P(3));
    case VKB_GetPhysicalDeviceSurfaceCapabilitiesKHR:
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)raw)(
            (VkPhysicalDevice)P(0), (VkSurfaceKHR)A(1),
            (VkSurfaceCapabilitiesKHR*)P(2));
    case VKB_GetPhysicalDeviceSurfaceFormatsKHR:
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)raw)(
            (VkPhysicalDevice)P(0), (VkSurfaceKHR)A(1), (uint32_t*)P(2),
            (VkSurfaceFormatKHR*)P(3));
    case VKB_GetPhysicalDeviceSurfacePresentModesKHR:
        return (S64)((PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)raw)(
            (VkPhysicalDevice)P(0), (VkSurfaceKHR)A(1), (uint32_t*)P(2),
            (VkPresentModeKHR*)P(3));
    case VKB_GetPhysicalDeviceSurfaceCapabilities2KHR:
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)raw)(
            (VkPhysicalDevice)P(0),
            (const VkPhysicalDeviceSurfaceInfo2KHR*)CP(1),
            (VkSurfaceCapabilities2KHR*)P(2));
    case VKB_GetPhysicalDeviceSurfaceFormats2KHR:
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)raw)(
            (VkPhysicalDevice)P(0),
            (const VkPhysicalDeviceSurfaceInfo2KHR*)CP(1), (uint32_t*)P(2),
            (VkSurfaceFormat2KHR*)P(3));
    case VKB_GetPhysicalDevicePresentRectanglesKHR:
        return (S64)((PFN_vkGetPhysicalDevicePresentRectanglesKHR)raw)(
            (VkPhysicalDevice)P(0), (VkSurfaceKHR)A(1), (uint32_t*)P(2),
            (VkRect2D*)P(3));
    case VKB_GetDeviceGroupSurfacePresentModesKHR:
        return (S64)((PFN_vkGetDeviceGroupSurfacePresentModesKHR)raw)(
            (VkDevice)P(0), (VkSurfaceKHR)A(1),
            (VkDeviceGroupPresentModeFlagsKHR*)P(2));
    case VKB_CreateSwapchainKHR:
        return (S64)((PFN_vkCreateSwapchainKHR)raw)(
            (VkDevice)P(0), (const VkSwapchainCreateInfoKHR*)CP(1), nullptr,
            (VkSwapchainKHR*)P(3));
    case VKB_DestroySwapchainKHR:
        ((PFN_vkDestroySwapchainKHR)raw)((VkDevice)P(0), (VkSwapchainKHR)A(1),
                                         nullptr);
        return 0;
    case VKB_GetSwapchainImagesKHR:
        return (S64)((PFN_vkGetSwapchainImagesKHR)raw)(
            (VkDevice)P(0), (VkSwapchainKHR)A(1), (uint32_t*)P(2),
            (VkImage*)P(3));
    case VKB_AcquireNextImageKHR:
        return (S64)((PFN_vkAcquireNextImageKHR)raw)(
            (VkDevice)P(0), (VkSwapchainKHR)A(1), (uint64_t)A(2),
            (VkSemaphore)A(3), (VkFence)A(4), (uint32_t*)P(5));
    case VKB_QueuePresentKHR: {
        const VkResult result = ((PFN_vkQueuePresentKHR)raw)(
            (VkQueue)P(0), (const VkPresentInfoKHR*)CP(1));
        const U32 presents = gPresents.fetch_add(1, std::memory_order_relaxed);
        // One line per sixty presents, plus the first: enough to say the lane
        // is presenting and at what rate, cheap enough to leave on.
        if (presents < 3 || (presents % 60) == 0) {
            klog_fmt("BOXEDWINE_X64_VULKAN_PRESENT frame=%u status=%d",
                     presents, (int)result);
        }
        return (S64)result;
    }
    case VKB_CreateXlibSurfaceKHR:
        // Handled before dispatch; unreachable.
        return BOXEDWINE_X64_VK_E_UNIMPL;
    case VKB_GetPhysicalDeviceXlibPresentationSupportKHR:
        // BoxedWine's X server is the only display this lane has, and the
        // host presents every window of it through the same Metal layer, so
        // every queue family that can present at all can present here. There
        // is no host vkGetPhysicalDeviceXlibPresentationSupportKHR to ask:
        // MoltenVK has no Xlib surface extension.
        return 1;
    default:
        break;
    }
    return BOXEDWINE_X64_VK_E_UNIMPL;
}

#undef A
#undef P
#undef CP
#undef U32A

// ---- vkGetInstanceProcAddr / vkGetDeviceProcAddr ----------------------------

S64 procAddr(KMemory64* memory, U64 handle, U64 nameAddress, U64 inShimTable) {
    char name[128] = {};
    if (!guestRange(memory, nameAddress, 1, false)) {
        return BOXEDWINE_X64_VK_E_FAULT;
    }
    for (U32 i = 0; i + 1 < sizeof(name); ++i) {
        if (!guestRange(memory, nameAddress + i, 1, false)) {
            break;
        }
        memory->memcpyFromGuest(&name[i], nameAddress + i, 1);
        if (!name[i]) {
            break;
        }
    }
    name[sizeof(name) - 1] = 0;

    int index = -1;
    for (int i = 0; i < VKB_COUNT; ++i) {
        if (!strcmp(kCommandName[i], name)) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        // The point of the whole operation: a run says by name what the
        // caller wanted that this bridge does not carry, which is a far
        // cheaper way to learn DXVK's real requirement list than guessing it.
        const U32 refusals = gRefusals.fetch_add(1, std::memory_order_relaxed);
        if (refusals < kRefusalBudget) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkGetProcAddr "
                     "missing=%s handle=0x%llx status=%d",
                     name, (unsigned long long)handle,
                     BOXEDWINE_X64_VK_E_BADOP);
        }
        return 0;
    }
    if (!inShimTable) {
        // The shim and the host disagree about the table, which only happens
        // with a stale shim in a container.
        klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkGetProcAddr stale=%s",
                 name);
        return 0;
    }
    // vkCreateXlibSurfaceKHR is served by this file rather than by the driver,
    // so it resolves whether or not MoltenVK has an Xlib surface extension --
    // which it does not.
    if (index == VKB_CreateXlibSurfaceKHR ||
        index == VKB_GetPhysicalDeviceXlibPresentationSupportKHR) {
        return hostDriver() ? 1 : 0;
    }
    if (!hostProc(index)) {
        const U32 refusals = gRefusals.fetch_add(1, std::memory_order_relaxed);
        if (refusals < kRefusalBudget) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkGetProcAddr "
                     "unsupported=%s handle=0x%llx status=%d",
                     name, (unsigned long long)handle,
                     BOXEDWINE_X64_VK_E_NOPROC);
        }
        return 0;
    }
    return 1;
}

#endif // BOXEDWINE_VULKAN

} // namespace

U64 vulkanBridge64Capabilities(CPU64* cpu) {
    U64 capabilities = 0;
#ifdef BOXEDWINE_VULKAN
    capabilities |= BOXEDWINE_X64_VK_CAP_HOST_MARSHAL;
    if (hostDriver()) {
        capabilities |= BOXEDWINE_X64_VK_CAP_DRIVER;
    }
#endif
    if (cpu && cpu->memory && cpu->memory->nativeIdentityMode()) {
        capabilities |= BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY;
    }
    return capabilities;
}

U64 vulkanBridge64(CPU64* cpu, U64 op, U64 argsAddress, U64 count) {
    if (!cpu || !cpu->memory || !cpu->thread || !cpu->thread->process) {
        return (U64)(S64)BOXEDWINE_X64_VK_E_FAULT;
    }
    KMemory64* memory = cpu->memory;
    const U32 pid = (U32)cpu->thread->process->id;
    const U64 capabilities = vulkanBridge64Capabilities(cpu);

    if (count > BOXEDWINE_X64_VK_MAX_ARGS) {
        return (U64)(S64)BOXEDWINE_X64_VK_E_ARGS;
    }
    U64 args[BOXEDWINE_X64_VK_MAX_ARGS] = {};
    if (count) {
        // The array is IN/OUT, so it has to be writable as well as readable
        // before a single slot is trusted.
        if (!guestRange(memory, argsAddress, count * sizeof(U64), true)) {
            return (U64)(S64)BOXEDWINE_X64_VK_E_FAULT;
        }
        memory->memcpyFromGuest(args, argsAddress, count * sizeof(U64));
    }

    S64 result = BOXEDWINE_X64_VK_E_UNIMPL;
    const char* name = nullptr;
    bool writeBack = true;

    switch (op) {
    case BOXEDWINE_X64_VK_OP_ABI:
        name = "abi";
        result = (S64)BOXEDWINE_X64_VK_ABI_VERSION;
        break;
    case BOXEDWINE_X64_VK_OP_PROBE:
        name = "probe";
        result = (S64)capabilities;
        break;
    case BOXEDWINE_X64_VK_OP_ECHO:
        name = "echo";
        if (count < 1) {
            result = BOXEDWINE_X64_VK_E_ARGS;
        } else {
            args[0] = args[0] + 1;
            result = 0;
        }
        break;
    case BOXEDWINE_X64_VK_OP_PROC_ADDR:
        name = "proc-addr";
#ifdef BOXEDWINE_VULKAN
        if (count < 3) {
            result = BOXEDWINE_X64_VK_E_ARGS;
        } else if (!(capabilities & BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY)) {
            result = BOXEDWINE_X64_VK_E_MEMORY;
        } else {
            result = procAddr(memory, args[0], args[1], args[2]);
        }
#else
        result = BOXEDWINE_X64_VK_E_NOHOST;
#endif
        break;
    default:
        break;
    }

    if (!name) {
#ifdef BOXEDWINE_VULKAN
        const int index = commandIndexForOp(op);
        if (index < 0) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE pid=%u op=%llu name=? "
                     "status=%d",
                     pid, (unsigned long long)op, BOXEDWINE_X64_VK_E_BADOP);
            return (U64)(S64)BOXEDWINE_X64_VK_E_BADOP;
        }
        name = kCommandName[index];
        if (!(capabilities & BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY)) {
            // The DXMT rule: a forked child has a sparse KMemory64 and cannot
            // hold a host Vulkan handle. Refuse by name rather than fault
            // inside Metal (docs/KNOWN_LIMITATIONS_IOS.md section 4).
            const U32 refusals =
                gRefusals.fetch_add(1, std::memory_order_relaxed);
            if (refusals < kRefusalBudget) {
                klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s pid=%u "
                         "status=%d reason=native-memory",
                         name, pid, BOXEDWINE_X64_VK_E_MEMORY);
            }
            return (U64)(S64)BOXEDWINE_X64_VK_E_MEMORY;
        }
        if (index == VKB_CreateXlibSurfaceKHR) {
            result = count < 4 ? BOXEDWINE_X64_VK_E_ARGS
                               : createXlibSurface(memory, args[0], args[1],
                                                   args[3]);
        } else {
            result = dispatchCommand(index, memory, args, count);
        }
        const U32 named = gNamedCalls.fetch_add(1, std::memory_order_relaxed);
        if (named < kNamedCallBudget || result < 0) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s pid=%u args=%llu "
                     "status=%lld",
                     name, pid, (unsigned long long)count,
                     (long long)result);
        }
        writeBack = false; // the driver wrote through the guest pointers
#else
        klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE pid=%u op=%llu name=? status=%d",
                 pid, (unsigned long long)op, BOXEDWINE_X64_VK_E_NOHOST);
        return (U64)(S64)BOXEDWINE_X64_VK_E_NOHOST;
#endif
    } else {
        // One line the first time a bootstrap operation is used, which is how
        // a log says the guest shim exists and reached the host at all.
        const U32 ordinal = gCallOrdinal.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 16) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE ordinal=%u pid=%u op=%llu "
                     "name=%s args=0x%llx count=%llu caps=0x%llx status=%lld",
                     ordinal, pid, (unsigned long long)op, name,
                     (unsigned long long)argsAddress,
                     (unsigned long long)count,
                     (unsigned long long)capabilities, (long long)result);
        }
    }

    if (count && writeBack) {
        memory->memcpyToGuest(argsAddress, args, count * sizeof(U64));
    }
    return (U64)result;
}

#endif // BOXEDWINE_GUEST_X64
