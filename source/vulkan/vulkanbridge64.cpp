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
// The 32-bit lane's `int 0x9A` path exists for two reasons: a 32-bit guest
// pointer is not a host address, and a 32-bit guest's Vulkan structures are
// not laid out the way the host's are. source/vulkan/vk_host*.cpp is 2.7 MB of
// generated code that closes both gaps at once.
//
// Only the second gap is absent on this lane. Every Vulkan structure has an
// identical layout on x86-64 System V and on arm64 AAPCS64: both are LP64,
// every Vulkan scalar takes its natural alignment on both, and Vulkan declares
// no bitfields and no packed members. So no field ever moves, and a byte copy
// of a structure is a valid structure on the other side.
//
// The first gap is very much present, and a device run proved it. The lane is
// served only to the process holding the native map -- the one process per
// session FEX translates, the same rule the DXMT unix call enforces and
// docs/KNOWN_LIMITATIONS_IOS.md section 4 records -- but that map is the
// identity only for the high lane. Canonical low addresses are served through
// the alias at K64_NATIVE_LOW_ALIAS_BASE, and Wine's top-down arena at
// 0x7ffffe000000, where every thread stack lives and therefore where DXVK
// builds its create-infos, is served from the relocated block at
// K64_NATIVE_TOP_HOST_BASE. An earlier revision of this file handed the
// guest's own pointers to the driver; MoltenVK followed a VkInstanceCreateInfo
// at 0x7ffffe0fe968 into unmapped host memory and the process died inside
// vkCreateInstance with the fault declined.
//
// So this file marshals. Every structure a command names is copied into a
// host-side shadow through the guest page table, every pointer inside it is
// marshalled the same way -- pNext chains, pApplicationInfo,
// ppEnabled{Layer,Extension}Names, pQueueCreateInfos, pEnabledFeatures, the
// handle and index arrays in VkSubmitInfo and VkPresentInfoKHR -- and the
// output structures are copied back into guest memory afterwards. The shadows
// are freed when the call returns, so nothing the driver was given outlives
// the dispatch that built it.
//
// Three rules bound what crosses:
//
//   1. A pointer the guest page table cannot account for is never
//      dereferenced. It becomes VK_ERROR_INITIALIZATION_FAILED and a
//      `status=bad-pointer` line, which is the whole point: an application
//      that passes a bad pointer gets a Vulkan error, not a crash in Metal.
//   2. A guest pointer is handed to the driver as an address only where the
//      driver must reach the guest's own memory rather than a copy of it
//      (VK_EXT_external_memory_host). Those go through
//      boxedvn::guestToHostAddress -- the same OR/AND the ARM64 translator
//      emits for a guest dereference -- and are additionally checked against
//      the host ranges this address space actually tracks.
//   3. Guest code is never called. `pAllocator` is forced to NULL on every
//      command that takes one (which is what the IA-32 marshal does too), no
//      command whose parameters include a callback is in the table, and a
//      pNext node that carries one is dropped from the chain rather than
//      forwarded -- DXVK attaches a VkDebugUtilsMessengerCreateInfoEXT to its
//      VkInstanceCreateInfo whenever validation is on, and its
//      pfnUserCallback is x86-64 code.
//
// A pNext node whose sType this file has no size for is dropped as well, and
// named in the log. Dropping is a defined state -- it is what the driver sees
// when the extension was never enabled -- whereas forwarding a node of unknown
// length means guessing how many bytes to copy and leaving the guest's own
// pNext inside it. VK_KHR_portability_subset is the notable absentee: its two
// structures live in vulkan_beta.h, which is not vendored under
// source/vulkan/vk, so MoltenVK's portability feature bits are reported to
// DXVK as zero until that header arrives. The log names the sType each time.

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "vulkanbridge64.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "guest_low_alias.h"
#include "boxedwine_x64_vulkan_bridge.h"

#ifdef BOXEDWINE_VULKAN
#include "knativesystem.h"
#include "kvulkan.h"
#include "../x11/x11.h"
#include "vk_host.h"
#include <SDL_vulkan.h>
#endif

#ifdef BOXEDWINE_IOS
#include "bvnhostpresent.h"
#include <chrono>
#endif
#include <atomic>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <vector>

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
std::atomic<U32> gDroppedNodes{0};

// Budgeted PER COMMAND, not globally, and the difference is not cosmetic.
//
// A global budget is spent by whatever the program does most. A device run
// reached exactly 64 dispatched calls before the budget closed, and 24 of
// those were eight repetitions of the same three pipeline-creation commands;
// everything after -- the command pool, the command buffer, the surface, both
// swapchains, and whatever the presenter did next -- was invisible. The
// conclusion drawn from that log ("no command pool, no submit, no present at
// all") was an artifact of the budget rather than a fact about the run, and it
// pointed the next investigation at the wrong half of the system.
//
// So each command carries its own small budget, a failure is named whatever
// the budget says, and a global ceiling still keeps a program that renders
// from filling the log. A command that starts and then stops is now visible as
// one whose first calls are present and whose later ones are not, instead of
// being indistinguishable from a command that was never called.
const U32 kPerCommandBudget = 4;
const U32 kNamedCallCeiling = 4096;
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

// How many times each command has been dispatched this session. The budget
// above reads it; a run's own log also reports it as `seen=`, which turns the
// line into a per-command call counter at no extra cost.
std::atomic<U32> gCommandCalls[VKB_COUNT];

int commandIndexForOp(U64 op) {
    for (int i = 0; i < VKB_COUNT; ++i) {
        if (kCommandOp[i] == op) {
            return i;
        }
    }
    return -1;
}

// The KHR spelling of each command promoted from an extension into Vulkan 1.1,
// indexed by command, nullptr for a command that has only one name. A driver
// may expose only one of the two depending on the API version the instance was
// created with, so resolution tries the core name first and this second.
//
// Filled at first use rather than declared as an initialiser because
// BOXEDWINE_X64_VK_ALIASES is keyed by command name, not by index. Every write
// stores the same constant, so a race between two threads reaching this first
// is harmless, and the critical section only keeps that honest.
const char* gCommandAlias[VKB_COUNT] = {};
bool gAliasesReady = false;

void buildAliasTable() {
    if (gAliasesReady) {
        return;
    }
    BOXEDWINE_CRITICAL_SECTION;
    if (gAliasesReady) {
        return;
    }
#define VKB_ALIAS_FILL(alias, core) gCommandAlias[VKB_##core] = "vk" #alias;
    BOXEDWINE_X64_VK_ALIASES(VKB_ALIAS_FILL)
#undef VKB_ALIAS_FILL
    gAliasesReady = true;
}

// The command an alias spelling resolves to, or -1. Separate from the exact
// match so a caller asking for `vkGetPhysicalDeviceProperties2KHR` on a 1.0
// instance is served rather than told the driver has no such command --
// which is how DXVK concludes it cannot use a device at all.
int commandIndexForAlias(const char* name) {
    buildAliasTable();
    for (int i = 0; i < VKB_COUNT; ++i) {
        if (gCommandAlias[i] && !strcmp(gCommandAlias[i], name)) {
            return i;
        }
    }
    return -1;
}

// ---- The host driver --------------------------------------------------------

PFN_vkGetInstanceProcAddr gGetInstanceProcAddr = nullptr;
bool gDriverTried = false;
PFN_vkVoidFunction gResolved[VKB_COUNT] = {};
VkInstance gResolvedFor[VKB_COUNT] = {};

// ---- Live instances ---------------------------------------------------------
//
// MoltenVK is the driver directly, with no Khronos loader in front of it, so
// vkGetInstanceProcAddr(VK_NULL_HANDLE, name) answers only for the four global
// commands the Vulkan specification names -- vkEnumerateInstanceVersion,
// vkEnumerateInstance{Layer,Extension}Properties and vkCreateInstance. Every
// other command needs a live VkInstance to be looked up through.
//
// That is why this list exists rather than a single "current instance". A
// device run showed Wine's adapter probe creating an instance, enumerating the
// physical device, and then calling vkDestroyInstance twice: the first call
// destroyed the instance and cleared the current one, and the second had
// nothing left to resolve `vkDestroyInstance` through, so the bridge answered
// BOXEDWINE_X64_VK_E_NOPROC to a command that returns void. The same hole
// swallowed every instance-level command in the window between one instance
// being destroyed and the next being created, and would have leaked a second
// live instance whose destroy arrived after the first.
//
// A host Vulkan handle is a host pointer, so this list is also the only thing
// standing between a stale handle and a dereference inside Metal: a destroy
// for a handle that is not here does nothing at all.
const U32 kMaxLiveInstances = 8;
VkInstance gLiveInstances[kMaxLiveInstances] = {};

// The instance instance-level resolution goes through by default: any live
// one, most recently created first. Commands that carry their own VkInstance
// resolve through that instead (see resolutionInstance).
VkInstance gInstance = VK_NULL_HANDLE;

bool instanceIsLive(VkInstance instance) {
    if (instance == VK_NULL_HANDLE) {
        return false;
    }
    for (U32 i = 0; i < kMaxLiveInstances; ++i) {
        if (gLiveInstances[i] == instance) {
            return true;
        }
    }
    return false;
}

void noteInstanceCreated(VkInstance instance) {
    BOXEDWINE_CRITICAL_SECTION;
    gInstance = instance;
    for (U32 i = 0; i < kMaxLiveInstances; ++i) {
        if (gLiveInstances[i] == VK_NULL_HANDLE) {
            gLiveInstances[i] = instance;
            return;
        }
    }
    // Nothing in the D3D9 path holds eight instances at once; a build that
    // manages it would leak the ninth rather than risk destroying a handle it
    // cannot vouch for, and this line is how a log would say so.
    klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkCreateInstance "
             "status=instance-table-full live=%u",
             kMaxLiveInstances);
}

// Remove an instance from the list, returning false when it was not there --
// which is what a second vkDestroyInstance for the same handle looks like.
bool forgetInstance(VkInstance instance) {
    BOXEDWINE_CRITICAL_SECTION;
    bool found = false;
    for (U32 i = 0; i < kMaxLiveInstances; ++i) {
        if (gLiveInstances[i] == instance) {
            gLiveInstances[i] = VK_NULL_HANDLE;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    if (gInstance == instance) {
        // Re-point at whatever is still alive, so the commands that have no
        // instance argument of their own keep resolving.
        gInstance = VK_NULL_HANDLE;
        for (U32 i = 0; i < kMaxLiveInstances; ++i) {
            if (gLiveInstances[i] != VK_NULL_HANDLE) {
                gInstance = gLiveInstances[i];
                break;
            }
        }
    }
    return true;
}

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
//
// `resolveWith` is the instance to look the command up through. It is a
// parameter rather than a read of gInstance because a command that carries its
// own VkInstance must resolve through THAT one: vkDestroyInstance has to
// resolve while the instance it is about to destroy is still the only live one.
PFN_vkVoidFunction hostProc(int index, VkInstance resolveWith) {
    PFN_vkGetInstanceProcAddr gipa = hostDriver();
    if (!gipa || index < 0 || index >= VKB_COUNT) {
        return nullptr;
    }
    if (gResolved[index] && gResolvedFor[index] == resolveWith) {
        return gResolved[index];
    }
    PFN_vkVoidFunction fn = gipa(resolveWith, kCommandName[index]);
    if (!fn && resolveWith != VK_NULL_HANDLE) {
        fn = gipa(VK_NULL_HANDLE, kCommandName[index]);
    }
    if (!fn) {
        // The KHR spelling, for a driver that exposes the extension form but
        // not the core one because the instance is Vulkan 1.0.
        buildAliasTable();
        if (gCommandAlias[index]) {
            fn = gipa(resolveWith, gCommandAlias[index]);
            if (!fn && resolveWith != VK_NULL_HANDLE) {
                fn = gipa(VK_NULL_HANDLE, gCommandAlias[index]);
            }
        }
    }
    gResolved[index] = fn;
    gResolvedFor[index] = resolveWith;
    return fn;
}

PFN_vkVoidFunction hostProc(int index) {
    return hostProc(index, gInstance);
}

// ---- The extensible-structure table -----------------------------------------
//
// Every structure that can appear as a pNext node, or as the head of a chain a
// command names, paired with the sType that identifies it. The size comes from
// the host's own headers, so a structure that grows in a later Vulkan header
// grows here with no edit.
//
// This is a whitelist on purpose. A node whose sType is absent is dropped from
// the chain and named in the log, because the alternative -- copying a guess
// at the node's length and leaving whatever pointers it holds pointing at
// guest addresses -- is exactly the fault this file exists to prevent. Adding
// a structure is one line.

#define BOXEDWINE_X64_VK_CHAIN_STRUCTS(X) \
    X(VK_STRUCTURE_TYPE_APPLICATION_INFO,                                  VkApplicationInfo) \
    X(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,                              VkInstanceCreateInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,                                VkDeviceCreateInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,                          VkDeviceQueueCreateInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,                               VkDeviceQueueInfo2) \
    X(VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO,          VkDeviceQueueGlobalPriorityCreateInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,                   VkDeviceGroupDeviceCreateInfo) \
    X(VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,                           VkValidationFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT,                              VkValidationFlagsEXT) \
    X(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,                              VkMemoryAllocateInfo) \
    X(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,                        VkMemoryAllocateFlagsInfo) \
    X(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,                    VkMemoryDedicatedAllocateInfo) \
    X(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,                     VkMemoryDedicatedRequirements) \
    X(VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,                 VkMemoryPriorityAllocateInfoEXT) \
    X(VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,                       VkExportMemoryAllocateInfo) \
    X(VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,               VkImportMemoryHostPointerInfoEXT) \
    X(VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,                VkMemoryHostPointerPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,                               VkMappedMemoryRange) \
    X(VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,                             VkMemoryRequirements2) \
    X(VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,                 VkBufferMemoryRequirementsInfo2) \
    X(VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,                  VkImageMemoryRequirementsInfo2) \
    X(VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,                           VkBindBufferMemoryInfo) \
    X(VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,                            VkBindImageMemoryInfo) \
    X(VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR,              VkBindImageMemorySwapchainInfoKHR) \
    X(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,                                VkBufferCreateInfo) \
    X(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,                VkExternalMemoryBufferCreateInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,                                 VkImageCreateInfo) \
    X(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,                 VkExternalMemoryImageCreateInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,                     VkImageFormatListCreateInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,                   VkImageStencilUsageCreateInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR,                   VkImageSwapchainCreateInfoKHR) \
    X(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,                            VkImageViewCreateInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,                      VkImageViewUsageCreateInfo) \
    X(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,                                 VkFenceCreateInfo) \
    X(VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,                          VkExportFenceCreateInfo) \
    X(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,                             VkSemaphoreCreateInfo) \
    X(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,                        VkSemaphoreTypeCreateInfo) \
    X(VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,                      VkExportSemaphoreCreateInfo) \
    X(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,                               VkSemaphoreWaitInfo) \
    X(VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,                             VkSemaphoreSignalInfo) \
    X(VK_STRUCTURE_TYPE_SUBMIT_INFO,                                       VkSubmitInfo) \
    X(VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,                    VkTimelineSemaphoreSubmitInfo) \
    X(VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO,                             VkProtectedSubmitInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO,                          VkDeviceGroupSubmitInfo) \
    X(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,                                  VkPresentInfoKHR) \
    X(VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,                     VkDeviceGroupPresentInfoKHR) \
    X(VK_STRUCTURE_TYPE_PRESENT_ID_KHR,                                    VkPresentIdKHR) \
    X(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,                         VkSwapchainCreateInfoKHR) \
    X(VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR,            VkDeviceGroupSwapchainCreateInfoKHR) \
    X(VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,                       VkAcquireNextImageInfoKHR) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,                VkPhysicalDeviceSurfaceInfo2KHR) \
    X(VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,                        VkSurfaceCapabilities2KHR) \
    X(VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,                              VkSurfaceFormat2KHR) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,               VkPhysicalDeviceImageFormatInfo2) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,        VkPhysicalDeviceExternalImageFormatInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,                         VkImageFormatProperties2) \
    X(VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,                  VkExternalImageFormatProperties) \
    X(VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES,  VkSamplerYcbcrConversionImageFormatProperties) \
    X(VK_STRUCTURE_TYPE_TEXTURE_LOD_GATHER_FORMAT_PROPERTIES_AMD,          VkTextureLODGatherFormatPropertiesAMD) \
    X(VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,                               VkFormatProperties2) \
    X(VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,                               VkFormatProperties3) \
    X(VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2,                         VkQueueFamilyProperties2) \
    X(VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES,           VkQueueFamilyGlobalPriorityProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,               VkPhysicalDeviceMemoryProperties2) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,      VkPhysicalDeviceMemoryBudgetPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,                      VkPhysicalDeviceProperties2) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,                        VkPhysicalDeviceFeatures2) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,              VkPhysicalDeviceExternalBufferInfo) \
    X(VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,                        VkExternalBufferProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,           VkPhysicalDeviceExternalSemaphoreInfo) \
    X(VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,                     VkExternalSemaphoreProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,               VkPhysicalDeviceExternalFenceInfo) \
    X(VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,                         VkExternalFenceProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,               VkPhysicalDeviceVulkan11Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,               VkPhysicalDeviceVulkan12Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,               VkPhysicalDeviceVulkan13Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,             VkPhysicalDeviceVulkan11Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,             VkPhysicalDeviceVulkan12Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,             VkPhysicalDeviceVulkan13Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,                     VkPhysicalDeviceIDProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,                 VkPhysicalDeviceDriverProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,               VkPhysicalDeviceSubgroupProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES,  VkPhysicalDeviceSubgroupSizeControlProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,         VkPhysicalDevicePointClippingProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,              VkPhysicalDeviceMultiviewProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,          VkPhysicalDeviceMaintenance3Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES,          VkPhysicalDeviceMaintenance4Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES,         VkPhysicalDeviceFloatControlsProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,    VkPhysicalDeviceDescriptorIndexingProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,  VkPhysicalDeviceDepthStencilResolveProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES,  VkPhysicalDeviceSamplerFilterMinmaxProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES,     VkPhysicalDeviceTimelineSemaphoreProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES,   VkPhysicalDeviceInlineUniformBlockProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES, VkPhysicalDeviceTexelBufferAlignmentProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, VkPhysicalDeviceExternalMemoryHostPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT, VkPhysicalDeviceTransformFeedbackPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT, VkPhysicalDeviceConservativeRasterizationPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT,       VkPhysicalDeviceRobustness2PropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT, VkPhysicalDeviceCustomBorderColorPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT, VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT, VkPhysicalDeviceLineRasterizationPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,       VkPhysicalDevicePCIBusInfoPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT, VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT, VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,            VkPhysicalDevice16BitStorageFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,             VkPhysicalDevice8BitStorageFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,      VkPhysicalDeviceShaderFloat16Int8Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,      VkPhysicalDeviceDescriptorIndexingFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,       VkPhysicalDeviceTimelineSemaphoreFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,    VkPhysicalDeviceBufferDeviceAddressFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,      VkPhysicalDeviceVulkanMemoryModelFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,         VkPhysicalDeviceHostQueryResetFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,      VkPhysicalDeviceScalarBlockLayoutFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES,    VkPhysicalDeviceImagelessFramebufferFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,                VkPhysicalDeviceMultiviewFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,        VkPhysicalDeviceVariablePointersFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,         VkPhysicalDeviceProtectedMemoryFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,   VkPhysicalDeviceShaderDrawParametersFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,        VkPhysicalDeviceDynamicRenderingFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,        VkPhysicalDeviceSynchronization2Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES,             VkPhysicalDevicePrivateDataFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES,         VkPhysicalDeviceImageRobustnessFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES,     VkPhysicalDeviceInlineUniformBlockFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,            VkPhysicalDeviceMaintenance4Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,         VkPhysicalDeviceRobustness2FeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,  VkPhysicalDeviceCustomBorderColorFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,    VkPhysicalDeviceDepthClipEnableFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,   VkPhysicalDeviceDepthClipControlFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,   VkPhysicalDeviceTransformFeedbackFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT,      VkPhysicalDeviceMemoryPriorityFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT,         VkPhysicalDevice4444FormatsFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT,  VkPhysicalDevicePipelineRobustnessFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,           VkPhysicalDevicePresentIdFeaturesKHR) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,         VkPhysicalDevicePresentWaitFeaturesKHR) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT,     VkPhysicalDeviceIndexTypeUint8FeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT,   VkPhysicalDeviceYcbcrImageArraysFeaturesEXT) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,    VkPhysicalDeviceDescriptorBufferFeaturesEXT) \
    /* command pools, command buffers and the "2" submit path */ \
    X(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,                          VkCommandPoolCreateInfo) \
    X(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,                      VkCommandBufferAllocateInfo) \
    X(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,                         VkCommandBufferBeginInfo) \
    X(VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,                   VkCommandBufferInheritanceInfo) \
    X(VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,         VkCommandBufferInheritanceRenderingInfo) \
    X(VK_STRUCTURE_TYPE_SUBMIT_INFO_2,                                     VkSubmitInfo2) \
    X(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,                             VkSemaphoreSubmitInfo) \
    X(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,                        VkCommandBufferSubmitInfo) \
    /* events and query pools */ \
    X(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,                                 VkEventCreateInfo) \
    X(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,                            VkQueryPoolCreateInfo) \
    /* buffer views and samplers */ \
    X(VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,                           VkBufferViewCreateInfo) \
    X(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,                               VkSamplerCreateInfo) \
    X(VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,                VkSamplerReductionModeCreateInfo) \
    X(VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,       VkSamplerCustomBorderColorCreateInfoEXT) \
    /* shader modules and pipeline caches */ \
    X(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,                         VkShaderModuleCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,                        VkPipelineCacheCreateInfo) \
    /* layouts, descriptor pools and descriptor writes */ \
    X(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,                       VkPipelineLayoutCreateInfo) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,                 VkDescriptorSetLayoutCreateInfo) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,   VkDescriptorSetLayoutBindingFlagsCreateInfo) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,                     VkDescriptorSetLayoutSupport) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT, VkDescriptorSetVariableDescriptorCountLayoutSupport) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,                       VkDescriptorPoolCreateInfo) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,                      VkDescriptorSetAllocateInfo) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, VkDescriptorSetVariableDescriptorCountAllocateInfo) \
    X(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,                              VkWriteDescriptorSet) \
    X(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,                               VkCopyDescriptorSet) \
    X(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,         VkWriteDescriptorSetInlineUniformBlock) \
    X(VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,            VkDescriptorUpdateTemplateCreateInfo) \
    /* render passes, both forms, and framebuffers */ \
    X(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,                           VkRenderPassCreateInfo) \
    X(VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,                 VkRenderPassMultiviewCreateInfo) \
    X(VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,   VkRenderPassInputAttachmentAspectCreateInfo) \
    X(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,                         VkRenderPassCreateInfo2) \
    X(VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,                          VkAttachmentDescription2) \
    X(VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,                            VkAttachmentReference2) \
    X(VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,                             VkSubpassDescription2) \
    X(VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,                              VkSubpassDependency2) \
    X(VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,         VkSubpassDescriptionDepthStencilResolve) \
    X(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,                           VkFramebufferCreateInfo) \
    X(VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO,               VkFramebufferAttachmentsCreateInfo) \
    X(VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO,                 VkFramebufferAttachmentImageInfo) \
    /* pipelines and every state structure they nest */ \
    X(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,                     VkGraphicsPipelineCreateInfo) \
    X(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,                      VkComputePipelineCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,                 VkPipelineShaderStageCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,           VkPipelineVertexInputStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,         VkPipelineInputAssemblyStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,           VkPipelineTessellationStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,               VkPipelineViewportStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,          VkPipelineRasterizationStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT, VkPipelineRasterizationStateStreamCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT, VkPipelineRasterizationConservativeStateCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT, VkPipelineRasterizationDepthClipStateCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO,     VkPipelineRasterizationLineStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,            VkPipelineMultisampleStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,          VkPipelineDepthStencilStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,            VkPipelineColorBlendStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT,              VkPipelineColorWriteCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,                VkPipelineDynamicStateCreateInfo) \
    X(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,                    VkPipelineRenderingCreateInfo) \
    X(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,         VkGraphicsPipelineLibraryCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,                  VkPipelineLibraryCreateInfoKHR) \
    /* recording: render pass and dynamic rendering scopes */ \
    X(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,                            VkRenderPassBeginInfo) \
    X(VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,                 VkRenderPassAttachmentBeginInfo) \
    X(VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,                                VkSubpassBeginInfo) \
    X(VK_STRUCTURE_TYPE_SUBPASS_END_INFO,                                  VkSubpassEndInfo) \
    X(VK_STRUCTURE_TYPE_RENDERING_INFO,                                    VkRenderingInfo) \
    X(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,                         VkRenderingAttachmentInfo) \
    /* recording: barriers, in both the 1.0 and the synchronization2 form */ \
    X(VK_STRUCTURE_TYPE_MEMORY_BARRIER,                                    VkMemoryBarrier) \
    X(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,                             VkBufferMemoryBarrier) \
    X(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,                              VkImageMemoryBarrier) \
    X(VK_STRUCTURE_TYPE_DEPENDENCY_INFO,                                   VkDependencyInfo) \
    X(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,                                  VkMemoryBarrier2) \
    X(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,                           VkBufferMemoryBarrier2) \
    X(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,                            VkImageMemoryBarrier2) \
    /* recording: the copy_commands2 forms and their region structures */ \
    X(VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,                                VkCopyBufferInfo2) \
    X(VK_STRUCTURE_TYPE_BUFFER_COPY_2,                                     VkBufferCopy2) \
    X(VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,                                 VkCopyImageInfo2) \
    X(VK_STRUCTURE_TYPE_IMAGE_COPY_2,                                      VkImageCopy2) \
    X(VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,                                 VkBlitImageInfo2) \
    X(VK_STRUCTURE_TYPE_IMAGE_BLIT_2,                                      VkImageBlit2) \
    X(VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,                       VkCopyBufferToImageInfo2) \
    X(VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,                       VkCopyImageToBufferInfo2) \
    X(VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,                               VkBufferImageCopy2) \
    X(VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,                              VkResolveImageInfo2) \
    X(VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,                                   VkImageResolve2) \
    /* the core commands the audit against Vulkan 1.4 added */ \
    X(VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,                                  VkBindSparseInfo) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES,                  VkPhysicalDeviceGroupProperties) \
    X(VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2,           VkImageSparseMemoryRequirementsInfo2) \
    X(VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2,                VkSparseImageMemoryRequirements2) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,        VkPhysicalDeviceSparseImageFormatInfo2) \
    X(VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2,                  VkSparseImageFormatProperties2) \
    X(VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,              VkSamplerYcbcrConversionCreateInfo) \
    X(VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,                     VkSamplerYcbcrConversionInfo) \
    X(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,                        VkBufferDeviceAddressInfo) \
    X(VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO,         VkBufferOpaqueCaptureAddressCreateInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO,         VkDeviceMemoryOpaqueCaptureAddressInfo) \
    X(VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO,                     VkPrivateDataSlotCreateInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,                 VkDeviceBufferMemoryRequirements) \
    X(VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,                  VkDeviceImageMemoryRequirements) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES,                   VkPhysicalDeviceToolProperties) \
    X(VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,                                   VkMemoryMapInfo) \
    X(VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,                                 VkMemoryUnmapInfo) \
    X(VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2,                               VkImageSubresource2) \
    X(VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2,                              VkSubresourceLayout2) \
    X(VK_STRUCTURE_TYPE_RENDERING_AREA_INFO,                               VkRenderingAreaInfo) \
    X(VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO,                     VkDeviceImageSubresourceInfo) \
    X(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO,                VkRenderingAttachmentLocationInfo) \
    X(VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,             VkRenderingInputAttachmentIndexInfo) \
    X(VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO,                         VkBindDescriptorSetsInfo) \
    X(VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,                               VkPushConstantsInfo) \
    X(VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO,                          VkPushDescriptorSetInfo) \
    X(VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,            VkPushDescriptorSetWithTemplateInfo) \
    X(VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,                   VkHostImageLayoutTransitionInfo) \
    /* VK_EXT_surface_maintenance1 and VK_EXT_swapchain_maintenance1. DXVK \
     * enables both and chains them onto the surface capability query, the \
     * swapchain create info and every present; a device run dropped all \
     * three it used, and the presenter's own log line said so -- it \
     * reported "Present mode: VK_PRESENT_MODE_FIFO_KHR (dynamic: no)", \
     * which is what DXVK concludes when the compatibility node comes back \
     * carrying the zero it put there itself. */ \
    X(VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT,                            VkSurfacePresentModeEXT) \
    X(VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT,            VkSurfacePresentScalingCapabilitiesEXT) \
    X(VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT,              VkSurfacePresentModeCompatibilityEXT) \
    X(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,             VkSwapchainPresentModesCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,                     VkSwapchainPresentModeInfoEXT) \
    X(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,                    VkSwapchainPresentFenceInfoEXT) \
    X(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT,           VkSwapchainPresentScalingCreateInfoEXT) \
    X(VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,                   VkReleaseSwapchainImagesInfoEXT) \
    /* The other four nodes the same run dropped. All are feature or \
     * property queries whose absence reads to DXVK as "not supported", \
     * which is the safe direction -- but the drop also reached \
     * vkCreateDevice, where a feature DXVK asked to enable then silently \
     * was not. */ \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES,         VkPhysicalDeviceLineRasterizationFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES,              VkPhysicalDeviceMaintenance5Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES,            VkPhysicalDeviceMaintenance5Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT, VkPhysicalDeviceExtendedDynamicState3PropertiesEXT)

struct ChainStructInfo {
    U32 sType;
    U32 size;
};

const ChainStructInfo kChainStructs[] = {
#define VKB_CHAIN_ENTRY(sType, Struct) { (U32)(sType), (U32)sizeof(Struct) },
    BOXEDWINE_X64_VK_CHAIN_STRUCTS(VKB_CHAIN_ENTRY)
#undef VKB_CHAIN_ENTRY
};

const U32 kChainStructCount =
    (U32)(sizeof(kChainStructs) / sizeof(kChainStructs[0]));

// The size of the structure this sType names, or 0 when the table has none.
// Every extensible structure begins with a 4-byte sType and an 8-byte pNext at
// offset 8, so the two fields can be read before the size is known.
U32 chainStructSize(U32 sType) {
    for (U32 i = 0; i < kChainStructCount; ++i) {
        if (kChainStructs[i].sType == sType) {
            return kChainStructs[i].size;
        }
    }
    return 0;
}

// A node whose body holds an x86-64 function pointer. Named separately from
// the unknown ones so the log says which of the two reasons applied.
bool nodeCarriesGuestCallback(U32 sType) {
    switch (sType) {
    case (U32)VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT:
    case (U32)VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT:
    case (U32)VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT:
        return true;
    default:
        return false;
    }
}

// ---- Descriptor update templates --------------------------------------------
//
// vkUpdateDescriptorSetWithTemplate is handed a bare `const void* pData` whose
// length appears nowhere in the call: it is described entirely by the entries
// the template was created with. So the bridge has to remember, per template,
// how many bytes the driver is going to read out of that block, or it cannot
// copy it at all.
//
// What it remembers is only a length, and that is enough. Every descriptor a
// template entry can name is a VkDescriptorImageInfo, a VkDescriptorBufferInfo
// or a VkBufferView, and none of the three holds a pointer -- they are
// handles, offsets, enums and sizes -- so once the span is known, a flat copy
// of it is a complete marshal. The gaps between entries are copied too; they
// are inside the caller's own block by construction, and the driver does not
// read them.
//
// A template naming a descriptor type this bridge cannot size (a future one,
// or an acceleration structure) is recorded as unsized, and every update
// through it is then a named refusal rather than a guess at the length.

U64 descriptorElementSize(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return sizeof(VkDescriptorImageInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return sizeof(VkDescriptorBufferInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return sizeof(VkBufferView);
    default:
        return 0;
    }
}

// The byte the driver stops reading at for one entry, measured from the start
// of the caller's block. VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK is the
// exception the specification carves out: its descriptorCount is a byte count
// and its stride is not used.
U64 templateEntryEnd(const VkDescriptorUpdateTemplateEntry& entry, bool* sized) {
    if (entry.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
        return (U64)entry.offset + (U64)entry.descriptorCount;
    }
    const U64 element = descriptorElementSize(entry.descriptorType);
    if (!element) {
        *sized = false;
        return 0;
    }
    if (!entry.descriptorCount) {
        return (U64)entry.offset;
    }
    return (U64)entry.offset +
           (U64)(entry.descriptorCount - 1) * (U64)entry.stride + element;
}

// The span of a whole template: the furthest byte any of its entries reaches.
U64 templateDataSpan(const VkDescriptorUpdateTemplateCreateInfo* info,
                     bool* sized) {
    *sized = true;
    U64 span = 0;
    if (!info || !info->pDescriptorUpdateEntries) {
        return 0;
    }
    for (uint32_t i = 0; i < info->descriptorUpdateEntryCount; ++i) {
        const U64 end =
            templateEntryEnd(info->pDescriptorUpdateEntries[i], sized);
        if (!*sized) {
            return 0;
        }
        if (end > span) {
            span = end;
        }
    }
    return span;
}

// Nothing in the D3D9 chain holds anything like this many templates at once:
// DXVK creates one per pipeline layout. A build that exceeded the table would
// record nothing for the surplus, and every update through one of those is
// refused by name rather than served with a length nobody knows.
const U32 kMaxDescriptorTemplates = 256;

struct TemplateRecord {
    VkDescriptorUpdateTemplate handle;
    U64 bytes;
    bool sized;
};

TemplateRecord gTemplates[kMaxDescriptorTemplates] = {};

void noteTemplateCreated(VkDescriptorUpdateTemplate handle, U64 bytes,
                         bool sized) {
    BOXEDWINE_CRITICAL_SECTION;
    for (U32 i = 0; i < kMaxDescriptorTemplates; ++i) {
        if (gTemplates[i].handle == VK_NULL_HANDLE ||
            gTemplates[i].handle == handle) {
            gTemplates[i].handle = handle;
            gTemplates[i].bytes = bytes;
            gTemplates[i].sized = sized;
            return;
        }
    }
    klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkCreateDescriptorUpdateTemplate "
             "status=template-table-full live=%u",
             kMaxDescriptorTemplates);
}

// The recorded span, and whether one was recorded at all. A template the table
// never saw answers false, which is what an update through it needs to become
// a refusal instead of a copy of an unknown number of bytes.
bool templateDataBytes(VkDescriptorUpdateTemplate handle, U64* bytes) {
    BOXEDWINE_CRITICAL_SECTION;
    for (U32 i = 0; i < kMaxDescriptorTemplates; ++i) {
        if (gTemplates[i].handle == handle) {
            *bytes = gTemplates[i].bytes;
            return gTemplates[i].sized;
        }
    }
    return false;
}

void forgetTemplate(VkDescriptorUpdateTemplate handle) {
    BOXEDWINE_CRITICAL_SECTION;
    for (U32 i = 0; i < kMaxDescriptorTemplates; ++i) {
        if (gTemplates[i].handle == handle) {
            gTemplates[i].handle = VK_NULL_HANDLE;
            gTemplates[i].bytes = 0;
            gTemplates[i].sized = false;
            return;
        }
    }
}

// ---- The marshal ------------------------------------------------------------
//
// One instance per dispatched command. It owns every shadow allocation it
// makes and every write-back it owes the guest; `flush()` performs the
// write-backs after the driver has returned, and the destructor releases the
// shadows. A failure anywhere latches, so a caller may chain marshalling calls
// and test once at the end -- every method returns nullptr while latched.

const U64 kMaxShadowBytes = 8u * 1024u * 1024u;
const U32 kMaxChainDepth = 32;
const U32 kMaxStringBytes = 4096;
const U32 kMaxArrayElements = 1u << 20;

class Marshal {
public:
    Marshal(KMemory64* memory, const char* command)
        : memory(memory), command(command) {}

    ~Marshal() {
        for (size_t i = 0; i < blocks.size(); ++i) {
            ::free(blocks[i]);
        }
    }

    bool ok() const { return !failed; }

    // What a command returns when a pointer could not be marshalled. The two
    // Vulkan errors every caller already handles: a create-shaped command has
    // failed to initialise, and everything else has failed for a reason the
    // caller is not told.
    S64 error(bool initialisation = true) const {
        return (S64)(initialisation ? VK_ERROR_INITIALIZATION_FAILED
                                    : VK_ERROR_UNKNOWN);
    }

    // ---- Flat buffers -------------------------------------------------------

    // A read-only block. A null guest pointer stays null: an optional
    // parameter is not an error.
    void* in(U64 guest, U64 bytes) { return block(guest, bytes, true, false, 0); }

    // A block the driver writes and the guest reads back. Copied in as well as
    // out, because the "2" structures require the caller's own sType and
    // because copying in makes the write-back value-preserving for any byte
    // the driver leaves alone.
    void* out(U64 guest, U64 bytes) { return block(guest, bytes, true, true, 0); }

    // The same two, for a pointer the command cannot do without. A null here
    // is the guest's own bug, and naming it is better than handing the driver
    // a null it will dereference.
    void* inRequired(U64 guest, U64 bytes) {
        if (failed) {
            return nullptr;
        }
        if (!guest) {
            fail(0, bytes);
            return nullptr;
        }
        return in(guest, bytes);
    }

    void* outRequired(U64 guest, U64 bytes) {
        if (failed) {
            return nullptr;
        }
        if (!guest) {
            fail(0, bytes);
            return nullptr;
        }
        return out(guest, bytes);
    }

    // An input array is required whenever its count is non-zero.
    template <typename T>
    const T* inArray(const T* guestPointer, U64 count) {
        if (failed || !count) {
            return nullptr;
        }
        return (const T*)inRequired(address(guestPointer), count * sizeof(T));
    }

    // The same, for an array named by a command's own argument word rather
    // than by a member of a structure already copied in.
    void* inArrayAt(U64 guest, U64 count, U64 elementSize) {
        if (failed || !count) {
            return nullptr;
        }
        return inRequired(guest, count * elementSize);
    }

    // An input array Vulkan allows to be null even when its count is not zero.
    // The pipeline state structures are full of these -- pViewports is null
    // whenever VK_DYNAMIC_STATE_VIEWPORT is set, and VkSubpassDescription's
    // pResolveAttachments is null whenever the subpass does not resolve --
    // and inArray would call each of them a bad pointer.
    template <typename T>
    const T* inArrayOptional(const T* guestPointer, U64 count) {
        if (failed || !count || !guestPointer) {
            return nullptr;
        }
        return (const T*)inRequired(address(guestPointer), count * sizeof(T));
    }

    // An input array of NON-extensible structures whose own members hold
    // pointers: VkDescriptorSetLayoutBinding::pImmutableSamplers and
    // VkSubpassDescription's five attachment arrays are the two the recording
    // half needs. structArray cannot serve them -- it reads an sType they do
    // not have -- so the block comes back writable and the caller walks it.
    template <typename T>
    T* inArrayWritable(U64 guest, U64 count) {
        if (failed || !count) {
            return nullptr;
        }
        return (T*)inRequired(guest, count * sizeof(T));
    }

    // An extensible structure that was copied as part of an enclosing one:
    // VkComputePipelineCreateInfo::stage, which is a whole
    // VkPipelineShaderStageCreateInfo by value. The shadow still holds the
    // guest's own pNext, because block() copied the bytes verbatim, so the
    // chain is mirrored from there and then the structure's own pointer
    // members are marshalled. `sType` is the statically known type rather
    // than the field the guest wrote: the member's type is fixed by the
    // enclosing structure, and a guest that mis-set the field would otherwise
    // steer the marshal.
    void embedded(void* host, U32 sType) {
        if (failed || !host) {
            return;
        }
        VkBaseOutStructure* node = (VkBaseOutStructure*)host;
        node->pNext = (VkBaseOutStructure*)chainOptional(address(node->pNext),
                                                         false);
        if (failed) {
            return;
        }
        fixup(sType, (U8*)host);
    }

    // An output array stays optional: VkPresentInfoKHR::pResults is the only
    // one, and a caller that does not want the per-swapchain results leaves
    // it null.
    template <typename T>
    T* outArray(T* guestPointer, U64 count) {
        return (T*)out(address(guestPointer), count * sizeof(T));
    }

    // ---- Strings ------------------------------------------------------------

    const char* string(U64 guest) {
        if (failed || !guest) {
            return nullptr;
        }
        // Scanned in chunks rather than a byte at a time: every guestRange is
        // a page-table lookup under a mutex, and an extension-name array is
        // dozens of strings long.
        char probe[64];
        U32 length = 0;
        bool terminated = false;
        while (!terminated && length < kMaxStringBytes) {
            U32 chunk = (U32)sizeof(probe);
            if (length + chunk > kMaxStringBytes) {
                chunk = kMaxStringBytes - length;
            }
            if (!guestRange(memory, guest + length, chunk, false)) {
                // The string may simply end near the end of its mapping.
                chunk = 1;
                if (!guestRange(memory, guest + length, 1, false)) {
                    fail(guest + length, 1);
                    return nullptr;
                }
            }
            memory->memcpyFromGuest(probe, guest + length, chunk);
            for (U32 i = 0; i < chunk; ++i) {
                ++length;
                if (!probe[i]) {
                    terminated = true;
                    break;
                }
            }
        }
        if (!terminated) {
            // Unterminated inside the cap. No Vulkan string is this long, and
            // copying a prefix would hand the driver a name it never meant.
            fail(guest, length);
            return nullptr;
        }
        char* host = (char*)allocate(length);
        if (!host) {
            fail(guest, length);
            return nullptr;
        }
        memory->memcpyFromGuest(host, guest, length);
        host[length - 1] = 0;
        return host;
    }

    // An array of guest `char*`, as ppEnabledExtensionNames is. A null entry
    // is a failure rather than a null in the host array: the driver strcmps
    // every one of them.
    const char* const* stringArray(U64 guest, U32 count) {
        if (failed || !count) {
            return nullptr;
        }
        if (!guest) {
            // A non-zero count with no array is not an optional parameter.
            fail(0, count);
            return nullptr;
        }
        if (count > kMaxArrayElements) {
            fail(guest, count);
            return nullptr;
        }
        const U64* guestPointers =
            (const U64*)in(guest, (U64)count * sizeof(U64));
        if (!guestPointers) {
            return nullptr;
        }
        const char** host =
            (const char**)allocate((U64)count * sizeof(const char*));
        if (!host) {
            fail(guest, count);
            return nullptr;
        }
        for (U32 i = 0; i < count; ++i) {
            if (!guestPointers[i]) {
                fail(guest, count);
                return nullptr;
            }
            host[i] = string(guestPointers[i]);
            if (failed) {
                return nullptr;
            }
        }
        return host;
    }

    // ---- Extensible structures ---------------------------------------------

    // The head of a chain the command cannot do without.
    void* chain(U64 guest, bool writeBack) {
        if (failed) {
            return nullptr;
        }
        if (!guest) {
            fail(0, 0);
            return nullptr;
        }
        return node(guest, writeBack, 0);
    }

    // The same, for a parameter Vulkan allows to be null.
    void* chainOptional(U64 guest, bool writeBack) {
        if (failed || !guest) {
            return nullptr;
        }
        return node(guest, writeBack, 0);
    }

    // An array of extensible structures, laid out contiguously as the driver
    // expects, with each element's own pNext chain mirrored and its own
    // pointer members marshalled.
    void* structArray(U64 guest, U32 count, U32 elementSize, bool writeBack) {
        if (failed || !count || !elementSize) {
            return nullptr;
        }
        if (!guest) {
            // An input array is required whenever its count is non-zero, so a
            // null there is a bad pointer rather than an omitted optional
            // parameter. An OUTPUT array is the opposite: passing null is how
            // a caller asks for the count, and the count slot it hands over
            // need not have been initialised at all, so its value says
            // nothing.
            if (!writeBack) {
                fail(0, (U64)count * elementSize);
            }
            return nullptr;
        }
        if (count > kMaxArrayElements) {
            fail(guest, count);
            return nullptr;
        }
        const U64 bytes = (U64)count * elementSize;
        U8* host = (U8*)block(guest, bytes, true, writeBack, elementSize);
        if (!host) {
            return nullptr;
        }
        for (U32 i = 0; i < count; ++i) {
            U8* element = host + (U64)i * elementSize;
            const U64 elementGuest = guest + (U64)i * elementSize;
            if (!prepare(element, elementGuest, writeBack, 0)) {
                return nullptr;
            }
        }
        return host;
    }

    template <typename T>
    T* structArrayTyped(U64 guest, U32 count, bool writeBack) {
        return (T*)structArray(guest, count, (U32)sizeof(T), writeBack);
    }

    // ---- Addresses ----------------------------------------------------------

    // The one path that hands a guest address to the driver instead of a copy
    // of the bytes behind it: memory the driver must reach where it lives,
    // which is what VK_EXT_external_memory_host asks for. Translated with the
    // same OR/AND the ARM64 translator emits for a guest dereference, and
    // accepted only when this address space tracks the resulting host range.
    void* direct(U64 guest, U64 bytes, bool write) {
        if (failed || !guest) {
            return nullptr;
        }
        if (!bytes || !boxedvn::guestRangeHostable(guest, bytes) ||
            !guestRange(memory, guest, bytes, write)) {
            fail(guest, bytes);
            return nullptr;
        }
        const U64 host = boxedvn::guestToHostAddress(guest);
        if (!memory->nativeRangeCoversForPlan(host, host + bytes)) {
            fail(guest, bytes);
            return nullptr;
        }
        return (void*)(uintptr_t)host;
    }

    // ---- Completion ---------------------------------------------------------

    // A pointer member of a structure the driver WRITES INTO. The shadow
    // holds the host address the driver was given; the guest must get its own
    // pointer back, because flush() copies the whole body of a written-back
    // node and would otherwise store a host address in guest memory -- the
    // exact fault this file exists to prevent, in the one direction that is
    // easy to miss, since it travels outward rather than inward.
    //
    // Until VK_EXT_surface_maintenance1 arrived, no structure in the table was
    // both written back and holder of a pointer, so the hazard was latent.
    // VkSurfacePresentModeCompatibilityEXT is the first, and the contract test
    // holds every future one to the same rule.
    void restorePointer(void* field, U64 guest) {
        if (failed || !field) {
            return;
        }
        Restore record = { field, guest };
        restores.push_back(record);
    }

    void flush() {
        // Before anything is copied out: a pointer member the driver was
        // handed a shadow for goes back to being the guest's own pointer.
        for (size_t i = 0; i < restores.size(); ++i) {
            ::memcpy(restores[i].field, &restores[i].guest, sizeof(U64));
        }
        restores.clear();
        for (size_t i = 0; i < records.size(); ++i) {
            const WriteBack& record = records[i];
            if (!record.stride) {
                memory->memcpyToGuest(record.guest, record.host, record.bytes);
                continue;
            }
            // An extensible structure's pNext holds a host pointer in the
            // shadow and the guest's own next node in guest memory. Everything
            // but those eight bytes goes back.
            for (U64 offset = 0; offset + record.stride <= record.bytes;
                 offset += record.stride) {
                const U8* host = (const U8*)record.host + offset;
                memory->memcpyToGuest(record.guest + offset, host, 4);
                if (record.stride > 16) {
                    memory->memcpyToGuest(record.guest + offset + 16, host + 16,
                                          record.stride - 16);
                }
            }
        }
        records.clear();
    }

    void fail(U64 guest, U64 bytes) {
        if (failed) {
            return;
        }
        failed = true;
        const U32 refusals = gRefusals.fetch_add(1, std::memory_order_relaxed);
        if (refusals < kRefusalBudget) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s status=bad-pointer "
                     "guest=0x%llx bytes=%llu",
                     command, (unsigned long long)guest,
                     (unsigned long long)bytes);
        }
    }

    static U64 address(const void* pointer) {
        return (U64)(uintptr_t)pointer;
    }

private:
    struct WriteBack {
        U64 guest;
        const void* host;
        U64 bytes;
        // 0 for a flat block; otherwise the size of one extensible structure,
        // whose pNext must not be written back.
        U32 stride;
    };

    // One pointer member of a written-back structure, and the guest value that
    // has to be standing in the shadow by the time flush() copies it out.
    struct Restore {
        void* field;
        U64 guest;
    };

    void* allocate(U64 bytes) {
        if (!bytes || bytes > kMaxShadowBytes) {
            return nullptr;
        }
        void* host = ::calloc(1, (size_t)bytes);
        if (host) {
            blocks.push_back(host);
        }
        return host;
    }

    void* block(U64 guest, U64 bytes, bool copyIn, bool writeBack, U32 stride) {
        if (failed || !guest || !bytes) {
            return nullptr;
        }
        if (bytes > kMaxShadowBytes) {
            fail(guest, bytes);
            return nullptr;
        }
        if (!guestRange(memory, guest, bytes, writeBack)) {
            fail(guest, bytes);
            return nullptr;
        }
        void* host = allocate(bytes);
        if (!host) {
            fail(guest, bytes);
            return nullptr;
        }
        if (copyIn) {
            memory->memcpyFromGuest(host, guest, bytes);
        }
        if (writeBack) {
            WriteBack record = { guest, host, bytes, stride };
            records.push_back(record);
        }
        return host;
    }

    // Copy one extensible structure and everything it points at.
    void* node(U64 guest, bool writeBack, U32 depth) {
        if (failed) {
            return nullptr;
        }
        if (depth >= kMaxChainDepth) {
            fail(guest, 0);
            return nullptr;
        }
        if (!guestRange(memory, guest, 16, false)) {
            fail(guest, 16);
            return nullptr;
        }
        U32 sType = 0;
        memory->memcpyFromGuest(&sType, guest, sizeof(sType));
        const U32 size = chainStructSize(sType);
        if (!size || nodeCarriesGuestCallback(sType)) {
            drop(sType, size ? "guest-callback" : "unknown-stype");
            U64 next = 0;
            memory->memcpyFromGuest(&next, guest + 8, sizeof(next));
            return next ? node(next, writeBack, depth + 1) : nullptr;
        }
        U8* host = (U8*)block(guest, size, true, writeBack, size);
        if (!host) {
            return nullptr;
        }
        return prepare(host, guest, writeBack, depth) ? host : nullptr;
    }

    // Fix an already-copied structure in place: replace its pNext with the
    // mirror of the guest's chain, then marshal its own pointer members.
    bool prepare(U8* host, U64 guest, bool writeBack, U32 depth) {
        U64 next = 0;
        memory->memcpyFromGuest(&next, guest + 8, sizeof(next));
        ((VkBaseOutStructure*)host)->pNext =
            (VkBaseOutStructure*)(next ? node(next, writeBack, depth + 1)
                                       : nullptr);
        if (failed) {
            return false;
        }
        U32 sType = 0;
        memory->memcpyFromGuest(&sType, guest, sizeof(sType));
        fixup(sType, host);
        return !failed;
    }

    void drop(U32 sType, const char* reason) {
        const U32 seen = gDroppedNodes.fetch_add(1, std::memory_order_relaxed);
        if (seen < kRefusalBudget) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s status=dropped-chain "
                     "sType=%u reason=%s",
                     command, sType, reason);
        }
    }

    // Every structure in the table that holds a pointer of its own. A
    // structure absent from this switch holds none, which is what makes the
    // byte copy above sufficient for it.
    void fixup(U32 sType, U8* host);

    KMemory64* memory;
    const char* command;
    std::vector<void*> blocks;
    std::vector<WriteBack> records;
    std::vector<Restore> restores;
    bool failed = false;
};

void Marshal::fixup(U32 sType, U8* host) {
    switch (sType) {
    case (U32)VK_STRUCTURE_TYPE_APPLICATION_INFO: {
        VkApplicationInfo* info = (VkApplicationInfo*)host;
        info->pApplicationName = string(address(info->pApplicationName));
        info->pEngineName = string(address(info->pEngineName));
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO: {
        VkInstanceCreateInfo* info = (VkInstanceCreateInfo*)host;
        info->pApplicationInfo = (const VkApplicationInfo*)chainOptional(
            address(info->pApplicationInfo), false);
        info->ppEnabledLayerNames = stringArray(
            address(info->ppEnabledLayerNames), info->enabledLayerCount);
        info->ppEnabledExtensionNames = stringArray(
            address(info->ppEnabledExtensionNames),
            info->enabledExtensionCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO: {
        VkDeviceCreateInfo* info = (VkDeviceCreateInfo*)host;
        info->pQueueCreateInfos = structArrayTyped<VkDeviceQueueCreateInfo>(
            address(info->pQueueCreateInfos), info->queueCreateInfoCount,
            false);
        info->ppEnabledLayerNames = stringArray(
            address(info->ppEnabledLayerNames), info->enabledLayerCount);
        info->ppEnabledExtensionNames = stringArray(
            address(info->ppEnabledExtensionNames),
            info->enabledExtensionCount);
        info->pEnabledFeatures = (const VkPhysicalDeviceFeatures*)in(
            address(info->pEnabledFeatures), sizeof(VkPhysicalDeviceFeatures));
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO: {
        VkDeviceQueueCreateInfo* info = (VkDeviceQueueCreateInfo*)host;
        info->pQueuePriorities =
            inArray(info->pQueuePriorities, info->queueCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO: {
        VkDeviceGroupDeviceCreateInfo* info =
            (VkDeviceGroupDeviceCreateInfo*)host;
        // Physical-device handles are host pointers already; only the array
        // that holds them lives in guest memory.
        info->pPhysicalDevices =
            inArray(info->pPhysicalDevices, info->physicalDeviceCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT: {
        VkValidationFeaturesEXT* info = (VkValidationFeaturesEXT*)host;
        info->pEnabledValidationFeatures =
            inArray(info->pEnabledValidationFeatures,
                    info->enabledValidationFeatureCount);
        info->pDisabledValidationFeatures =
            inArray(info->pDisabledValidationFeatures,
                    info->disabledValidationFeatureCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT: {
        VkValidationFlagsEXT* info = (VkValidationFlagsEXT*)host;
        info->pDisabledValidationChecks =
            inArray(info->pDisabledValidationChecks,
                    info->disabledValidationCheckCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT: {
        VkImportMemoryHostPointerInfoEXT* info =
            (VkImportMemoryHostPointerInfoEXT*)host;
        // The one import that is genuinely an address rather than data: Wine
        // places the allocation itself, below 4 GiB so a 32-bit caller can
        // hold the mapping, and asks the driver to adopt it. The length is
        // the enclosing VkMemoryAllocateInfo's allocationSize, which this
        // node does not carry; one guest page is what gets proved here, and
        // the driver's own alignment requirement covers the rest.
        info->pHostPointer =
            direct(address(info->pHostPointer), K64_PAGE_SIZE, true);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
        VkImageFormatListCreateInfo* info = (VkImageFormatListCreateInfo*)host;
        info->pViewFormats = inArray(info->pViewFormats, info->viewFormatCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO: {
        VkBufferCreateInfo* info = (VkBufferCreateInfo*)host;
        info->pQueueFamilyIndices =
            inArray(info->pQueueFamilyIndices, info->queueFamilyIndexCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO: {
        VkImageCreateInfo* info = (VkImageCreateInfo*)host;
        info->pQueueFamilyIndices =
            inArray(info->pQueueFamilyIndices, info->queueFamilyIndexCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR: {
        VkSwapchainCreateInfoKHR* info = (VkSwapchainCreateInfoKHR*)host;
        info->pQueueFamilyIndices =
            inArray(info->pQueueFamilyIndices, info->queueFamilyIndexCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO: {
        VkSemaphoreWaitInfo* info = (VkSemaphoreWaitInfo*)host;
        info->pSemaphores = inArray(info->pSemaphores, info->semaphoreCount);
        info->pValues = inArray(info->pValues, info->semaphoreCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SUBMIT_INFO: {
        VkSubmitInfo* info = (VkSubmitInfo*)host;
        info->pWaitSemaphores =
            inArray(info->pWaitSemaphores, info->waitSemaphoreCount);
        info->pWaitDstStageMask =
            inArray(info->pWaitDstStageMask, info->waitSemaphoreCount);
        info->pCommandBuffers =
            inArray(info->pCommandBuffers, info->commandBufferCount);
        info->pSignalSemaphores =
            inArray(info->pSignalSemaphores, info->signalSemaphoreCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO: {
        VkTimelineSemaphoreSubmitInfo* info =
            (VkTimelineSemaphoreSubmitInfo*)host;
        info->pWaitSemaphoreValues = inArray(info->pWaitSemaphoreValues,
                                             info->waitSemaphoreValueCount);
        info->pSignalSemaphoreValues = inArray(info->pSignalSemaphoreValues,
                                               info->signalSemaphoreValueCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO: {
        VkDeviceGroupSubmitInfo* info = (VkDeviceGroupSubmitInfo*)host;
        info->pWaitSemaphoreDeviceIndices = inArray(
            info->pWaitSemaphoreDeviceIndices, info->waitSemaphoreCount);
        info->pCommandBufferDeviceMasks = inArray(
            info->pCommandBufferDeviceMasks, info->commandBufferCount);
        info->pSignalSemaphoreDeviceIndices = inArray(
            info->pSignalSemaphoreDeviceIndices, info->signalSemaphoreCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PRESENT_INFO_KHR: {
        VkPresentInfoKHR* info = (VkPresentInfoKHR*)host;
        info->pWaitSemaphores =
            inArray(info->pWaitSemaphores, info->waitSemaphoreCount);
        info->pSwapchains = inArray(info->pSwapchains, info->swapchainCount);
        info->pImageIndices =
            inArray(info->pImageIndices, info->swapchainCount);
        // The one output array inside an input structure: a per-swapchain
        // VkResult the caller reads after the present returns.
        info->pResults = outArray(info->pResults, info->swapchainCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR: {
        VkDeviceGroupPresentInfoKHR* info = (VkDeviceGroupPresentInfoKHR*)host;
        info->pDeviceMasks = inArray(info->pDeviceMasks, info->swapchainCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
        VkPresentIdKHR* info = (VkPresentIdKHR*)host;
        info->pPresentIds = inArray(info->pPresentIds, info->swapchainCount);
        break;
    }

    // ---- Command buffers and the "2" submit path ---------------------------

    case (U32)VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO: {
        VkCommandBufferBeginInfo* info = (VkCommandBufferBeginInfo*)host;
        // Ignored for a primary command buffer, and a caller that begins one
        // may leave whatever it likes here; null is what a primary begin
        // normally carries and chainOptional keeps it null.
        info->pInheritanceInfo = (const VkCommandBufferInheritanceInfo*)
            chainOptional(address(info->pInheritanceInfo), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO: {
        VkCommandBufferInheritanceRenderingInfo* info =
            (VkCommandBufferInheritanceRenderingInfo*)host;
        info->pColorAttachmentFormats = inArrayOptional(
            info->pColorAttachmentFormats, info->colorAttachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SUBMIT_INFO_2: {
        VkSubmitInfo2* info = (VkSubmitInfo2*)host;
        info->pWaitSemaphoreInfos =
            structArrayTyped<VkSemaphoreSubmitInfo>(
                address(info->pWaitSemaphoreInfos),
                info->waitSemaphoreInfoCount, false);
        info->pCommandBufferInfos =
            structArrayTyped<VkCommandBufferSubmitInfo>(
                address(info->pCommandBufferInfos),
                info->commandBufferInfoCount, false);
        info->pSignalSemaphoreInfos =
            structArrayTyped<VkSemaphoreSubmitInfo>(
                address(info->pSignalSemaphoreInfos),
                info->signalSemaphoreInfoCount, false);
        break;
    }

    // ---- Shader modules, pipeline caches and layouts -----------------------

    case (U32)VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO: {
        VkShaderModuleCreateInfo* info = (VkShaderModuleCreateInfo*)host;
        // SPIR-V, sized in BYTES by codeSize however it is spelled as
        // uint32_t*. inArrayAt with an element size of one is the byte count.
        info->pCode = (const uint32_t*)inArrayAt(address(info->pCode),
                                                 (U64)info->codeSize, 1);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO: {
        VkPipelineCacheCreateInfo* info = (VkPipelineCacheCreateInfo*)host;
        info->pInitialData = inArrayAt(address(info->pInitialData),
                                       (U64)info->initialDataSize, 1);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO: {
        VkPipelineLayoutCreateInfo* info = (VkPipelineLayoutCreateInfo*)host;
        info->pSetLayouts = inArray(info->pSetLayouts, info->setLayoutCount);
        info->pPushConstantRanges = inArray(info->pPushConstantRanges,
                                            info->pushConstantRangeCount);
        break;
    }

    // ---- Descriptor set layouts, pools, sets and writes --------------------

    case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO: {
        VkDescriptorSetLayoutCreateInfo* info =
            (VkDescriptorSetLayoutCreateInfo*)host;
        VkDescriptorSetLayoutBinding* bindings =
            inArrayWritable<VkDescriptorSetLayoutBinding>(
                address(info->pBindings), info->bindingCount);
        info->pBindings = bindings;
        for (U32 i = 0; bindings && i < info->bindingCount && !failed; ++i) {
            VkDescriptorSetLayoutBinding* binding = &bindings[i];
            // pImmutableSamplers is read only for the two sampler-bearing
            // descriptor types and is ignored -- and therefore may hold
            // anything at all -- for every other one. Following it there
            // would be dereferencing a value the guest never set.
            if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                binding->descriptorType ==
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                binding->pImmutableSamplers = inArrayOptional(
                    binding->pImmutableSamplers, binding->descriptorCount);
            } else {
                binding->pImmutableSamplers = nullptr;
            }
        }
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO: {
        VkDescriptorSetLayoutBindingFlagsCreateInfo* info =
            (VkDescriptorSetLayoutBindingFlagsCreateInfo*)host;
        info->pBindingFlags =
            inArrayOptional(info->pBindingFlags, info->bindingCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO: {
        VkDescriptorPoolCreateInfo* info = (VkDescriptorPoolCreateInfo*)host;
        info->pPoolSizes = inArray(info->pPoolSizes, info->poolSizeCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO: {
        VkDescriptorSetAllocateInfo* info = (VkDescriptorSetAllocateInfo*)host;
        info->pSetLayouts =
            inArray(info->pSetLayouts, info->descriptorSetCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO: {
        VkDescriptorSetVariableDescriptorCountAllocateInfo* info =
            (VkDescriptorSetVariableDescriptorCountAllocateInfo*)host;
        info->pDescriptorCounts =
            inArrayOptional(info->pDescriptorCounts, info->descriptorSetCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET: {
        // The awkward one. Exactly one of the three arrays is read, chosen by
        // descriptorType, and the other two are ignored -- so they hold
        // whatever the caller left there, which for DXVK is a stale pointer
        // from the previous write in the same scratch array. Marshalling the
        // chosen one and NULLing the rest is the only reading of this
        // structure that hands the driver nothing it did not mean.
        VkWriteDescriptorSet* info = (VkWriteDescriptorSet*)host;
        const VkDescriptorImageInfo* images = nullptr;
        const VkDescriptorBufferInfo* buffers = nullptr;
        const VkBufferView* views = nullptr;
        switch (info->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            images = inArray(info->pImageInfo, info->descriptorCount);
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            buffers = inArray(info->pBufferInfo, info->descriptorCount);
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            views = inArray(info->pTexelBufferView, info->descriptorCount);
            break;
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
            // The data is in a VkWriteDescriptorSetInlineUniformBlock on the
            // pNext chain, which the walker has already handled; all three
            // arrays are ignored.
            break;
        default:
            // A descriptor type whose payload this bridge cannot size -- a
            // future one, or an acceleration structure. Refuse rather than
            // pick one of the three arrays and hope; the type is named
            // because that is the only way a run says which one arrived.
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s "
                     "status=unsized-descriptor-type descriptorType=%d "
                     "count=%u",
                     command, (int)info->descriptorType,
                     info->descriptorCount);
            fail(address(info->pImageInfo), info->descriptorCount);
            return;
        }
        info->pImageInfo = images;
        info->pBufferInfo = buffers;
        info->pTexelBufferView = views;
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK: {
        VkWriteDescriptorSetInlineUniformBlock* info =
            (VkWriteDescriptorSetInlineUniformBlock*)host;
        info->pData = inArrayAt(address(info->pData), info->dataSize, 1);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO: {
        VkDescriptorUpdateTemplateCreateInfo* info =
            (VkDescriptorUpdateTemplateCreateInfo*)host;
        info->pDescriptorUpdateEntries =
            inArray(info->pDescriptorUpdateEntries,
                    info->descriptorUpdateEntryCount);
        break;
    }

    // ---- Render passes, both forms, and framebuffers -----------------------

    case (U32)VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO: {
        VkRenderPassCreateInfo* info = (VkRenderPassCreateInfo*)host;
        info->pAttachments = inArray(info->pAttachments, info->attachmentCount);
        VkSubpassDescription* subpasses = inArrayWritable<VkSubpassDescription>(
            address(info->pSubpasses), info->subpassCount);
        info->pSubpasses = subpasses;
        for (U32 i = 0; subpasses && i < info->subpassCount && !failed; ++i) {
            VkSubpassDescription* subpass = &subpasses[i];
            subpass->pInputAttachments = inArrayOptional(
                subpass->pInputAttachments, subpass->inputAttachmentCount);
            subpass->pColorAttachments = inArrayOptional(
                subpass->pColorAttachments, subpass->colorAttachmentCount);
            // Optional even with a non-zero colour count: a subpass that does
            // not resolve leaves it null, and its length is the COLOUR count.
            subpass->pResolveAttachments = inArrayOptional(
                subpass->pResolveAttachments, subpass->colorAttachmentCount);
            subpass->pDepthStencilAttachment =
                (const VkAttachmentReference*)in(
                    address(subpass->pDepthStencilAttachment),
                    sizeof(VkAttachmentReference));
            subpass->pPreserveAttachments = inArrayOptional(
                subpass->pPreserveAttachments,
                subpass->preserveAttachmentCount);
        }
        info->pDependencies =
            inArray(info->pDependencies, info->dependencyCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO: {
        VkRenderPassMultiviewCreateInfo* info =
            (VkRenderPassMultiviewCreateInfo*)host;
        info->pViewMasks = inArrayOptional(info->pViewMasks, info->subpassCount);
        info->pViewOffsets =
            inArrayOptional(info->pViewOffsets, info->dependencyCount);
        info->pCorrelationMasks = inArrayOptional(info->pCorrelationMasks,
                                                  info->correlationMaskCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO: {
        VkRenderPassInputAttachmentAspectCreateInfo* info =
            (VkRenderPassInputAttachmentAspectCreateInfo*)host;
        info->pAspectReferences =
            inArray(info->pAspectReferences, info->aspectReferenceCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2: {
        VkRenderPassCreateInfo2* info = (VkRenderPassCreateInfo2*)host;
        info->pAttachments = structArrayTyped<VkAttachmentDescription2>(
            address(info->pAttachments), info->attachmentCount, false);
        info->pSubpasses = structArrayTyped<VkSubpassDescription2>(
            address(info->pSubpasses), info->subpassCount, false);
        info->pDependencies = structArrayTyped<VkSubpassDependency2>(
            address(info->pDependencies), info->dependencyCount, false);
        info->pCorrelatedViewMasks = inArrayOptional(
            info->pCorrelatedViewMasks, info->correlatedViewMaskCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2: {
        VkSubpassDescription2* info = (VkSubpassDescription2*)host;
        info->pInputAttachments = structArrayTyped<VkAttachmentReference2>(
            address(info->pInputAttachments), info->inputAttachmentCount,
            false);
        info->pColorAttachments = structArrayTyped<VkAttachmentReference2>(
            address(info->pColorAttachments), info->colorAttachmentCount,
            false);
        info->pResolveAttachments =
            info->pResolveAttachments
                ? structArrayTyped<VkAttachmentReference2>(
                      address(info->pResolveAttachments),
                      info->colorAttachmentCount, false)
                : nullptr;
        info->pDepthStencilAttachment = (const VkAttachmentReference2*)
            chainOptional(address(info->pDepthStencilAttachment), false);
        info->pPreserveAttachments = inArrayOptional(
            info->pPreserveAttachments, info->preserveAttachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE: {
        VkSubpassDescriptionDepthStencilResolve* info =
            (VkSubpassDescriptionDepthStencilResolve*)host;
        info->pDepthStencilResolveAttachment = (const VkAttachmentReference2*)
            chainOptional(address(info->pDepthStencilResolveAttachment), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO: {
        VkFramebufferCreateInfo* info = (VkFramebufferCreateInfo*)host;
        // Null with a non-zero count for an imageless framebuffer, whose
        // attachments come from VkFramebufferAttachmentsCreateInfo instead.
        info->pAttachments =
            inArrayOptional(info->pAttachments, info->attachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO: {
        VkFramebufferAttachmentsCreateInfo* info =
            (VkFramebufferAttachmentsCreateInfo*)host;
        info->pAttachmentImageInfos =
            structArrayTyped<VkFramebufferAttachmentImageInfo>(
                address(info->pAttachmentImageInfos),
                info->attachmentImageInfoCount, false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO: {
        VkFramebufferAttachmentImageInfo* info =
            (VkFramebufferAttachmentImageInfo*)host;
        info->pViewFormats =
            inArray(info->pViewFormats, info->viewFormatCount);
        break;
    }

    // ---- Pipelines ---------------------------------------------------------

    case (U32)VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO: {
        VkGraphicsPipelineCreateInfo* info =
            (VkGraphicsPipelineCreateInfo*)host;
        info->pStages = structArrayTyped<VkPipelineShaderStageCreateInfo>(
            address(info->pStages), info->stageCount, false);
        // Every state pointer is optional: a pipeline library provides only
        // the states in its own subset, and a pipeline with rasterization
        // discarded omits the fragment-output ones.
        info->pVertexInputState = (const VkPipelineVertexInputStateCreateInfo*)
            chainOptional(address(info->pVertexInputState), false);
        info->pInputAssemblyState =
            (const VkPipelineInputAssemblyStateCreateInfo*)chainOptional(
                address(info->pInputAssemblyState), false);
        info->pTessellationState =
            (const VkPipelineTessellationStateCreateInfo*)chainOptional(
                address(info->pTessellationState), false);
        info->pViewportState = (const VkPipelineViewportStateCreateInfo*)
            chainOptional(address(info->pViewportState), false);
        info->pRasterizationState =
            (const VkPipelineRasterizationStateCreateInfo*)chainOptional(
                address(info->pRasterizationState), false);
        info->pMultisampleState =
            (const VkPipelineMultisampleStateCreateInfo*)chainOptional(
                address(info->pMultisampleState), false);
        info->pDepthStencilState =
            (const VkPipelineDepthStencilStateCreateInfo*)chainOptional(
                address(info->pDepthStencilState), false);
        info->pColorBlendState = (const VkPipelineColorBlendStateCreateInfo*)
            chainOptional(address(info->pColorBlendState), false);
        info->pDynamicState = (const VkPipelineDynamicStateCreateInfo*)
            chainOptional(address(info->pDynamicState), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO: {
        VkComputePipelineCreateInfo* info = (VkComputePipelineCreateInfo*)host;
        // The one by-value extensible member in the whole table.
        embedded(&info->stage,
                 (U32)VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO: {
        VkPipelineShaderStageCreateInfo* info =
            (VkPipelineShaderStageCreateInfo*)host;
        info->pName = string(address(info->pName));
        // VkSpecializationInfo has no sType, so it cannot go through the
        // chain walker; its two pointers are marshalled by hand. pData is
        // opaque bytes sized by dataSize, which is exactly what the driver
        // reads.
        VkSpecializationInfo* specialization = (VkSpecializationInfo*)in(
            address(info->pSpecializationInfo), sizeof(VkSpecializationInfo));
        if (specialization) {
            specialization->pMapEntries = inArray(
                specialization->pMapEntries, specialization->mapEntryCount);
            specialization->pData =
                inArrayAt(address(specialization->pData),
                          (U64)specialization->dataSize, 1);
        }
        info->pSpecializationInfo = specialization;
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO: {
        VkPipelineVertexInputStateCreateInfo* info =
            (VkPipelineVertexInputStateCreateInfo*)host;
        info->pVertexBindingDescriptions =
            inArrayOptional(info->pVertexBindingDescriptions,
                            info->vertexBindingDescriptionCount);
        info->pVertexAttributeDescriptions =
            inArrayOptional(info->pVertexAttributeDescriptions,
                            info->vertexAttributeDescriptionCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO: {
        VkPipelineViewportStateCreateInfo* info =
            (VkPipelineViewportStateCreateInfo*)host;
        // Null with a non-zero count whenever the matching dynamic state is
        // enabled, which is how DXVK builds every graphics pipeline.
        info->pViewports =
            inArrayOptional(info->pViewports, info->viewportCount);
        info->pScissors = inArrayOptional(info->pScissors, info->scissorCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO: {
        VkPipelineMultisampleStateCreateInfo* info =
            (VkPipelineMultisampleStateCreateInfo*)host;
        // ceil(rasterizationSamples / 32) VkSampleMask words, which is the
        // length the specification gives it. rasterizationSamples is a single
        // bit of VkSampleCountFlagBits, so its numeric value is the sample
        // count; a zero (which is invalid) yields zero words and a null.
        const U64 words = ((U64)info->rasterizationSamples + 31u) / 32u;
        info->pSampleMask = (const VkSampleMask*)inArrayOptional(
            info->pSampleMask, words);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO: {
        VkPipelineColorBlendStateCreateInfo* info =
            (VkPipelineColorBlendStateCreateInfo*)host;
        info->pAttachments =
            inArrayOptional(info->pAttachments, info->attachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT: {
        VkPipelineColorWriteCreateInfoEXT* info =
            (VkPipelineColorWriteCreateInfoEXT*)host;
        info->pColorWriteEnables =
            inArrayOptional(info->pColorWriteEnables, info->attachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO: {
        VkPipelineDynamicStateCreateInfo* info =
            (VkPipelineDynamicStateCreateInfo*)host;
        info->pDynamicStates =
            inArray(info->pDynamicStates, info->dynamicStateCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: {
        VkPipelineRenderingCreateInfo* info =
            (VkPipelineRenderingCreateInfo*)host;
        info->pColorAttachmentFormats = inArrayOptional(
            info->pColorAttachmentFormats, info->colorAttachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR: {
        VkPipelineLibraryCreateInfoKHR* info =
            (VkPipelineLibraryCreateInfoKHR*)host;
        info->pLibraries = inArray(info->pLibraries, info->libraryCount);
        break;
    }

    // ---- Recording ---------------------------------------------------------

    case (U32)VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO: {
        VkRenderPassBeginInfo* info = (VkRenderPassBeginInfo*)host;
        info->pClearValues =
            inArrayOptional(info->pClearValues, info->clearValueCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO: {
        VkRenderPassAttachmentBeginInfo* info =
            (VkRenderPassAttachmentBeginInfo*)host;
        info->pAttachments = inArray(info->pAttachments, info->attachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDERING_INFO: {
        VkRenderingInfo* info = (VkRenderingInfo*)host;
        info->pColorAttachments = structArrayTyped<VkRenderingAttachmentInfo>(
            address(info->pColorAttachments), info->colorAttachmentCount,
            false);
        info->pDepthAttachment = (const VkRenderingAttachmentInfo*)
            chainOptional(address(info->pDepthAttachment), false);
        info->pStencilAttachment = (const VkRenderingAttachmentInfo*)
            chainOptional(address(info->pStencilAttachment), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEPENDENCY_INFO: {
        VkDependencyInfo* info = (VkDependencyInfo*)host;
        info->pMemoryBarriers = structArrayTyped<VkMemoryBarrier2>(
            address(info->pMemoryBarriers), info->memoryBarrierCount, false);
        info->pBufferMemoryBarriers =
            structArrayTyped<VkBufferMemoryBarrier2>(
                address(info->pBufferMemoryBarriers),
                info->bufferMemoryBarrierCount, false);
        info->pImageMemoryBarriers = structArrayTyped<VkImageMemoryBarrier2>(
            address(info->pImageMemoryBarriers), info->imageMemoryBarrierCount,
            false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2: {
        VkCopyBufferInfo2* info = (VkCopyBufferInfo2*)host;
        info->pRegions = structArrayTyped<VkBufferCopy2>(
            address(info->pRegions), info->regionCount, false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2: {
        VkCopyImageInfo2* info = (VkCopyImageInfo2*)host;
        info->pRegions = structArrayTyped<VkImageCopy2>(
            address(info->pRegions), info->regionCount, false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2: {
        VkBlitImageInfo2* info = (VkBlitImageInfo2*)host;
        info->pRegions = structArrayTyped<VkImageBlit2>(
            address(info->pRegions), info->regionCount, false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2: {
        VkCopyBufferToImageInfo2* info = (VkCopyBufferToImageInfo2*)host;
        info->pRegions = structArrayTyped<VkBufferImageCopy2>(
            address(info->pRegions), info->regionCount, false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2: {
        VkCopyImageToBufferInfo2* info = (VkCopyImageToBufferInfo2*)host;
        info->pRegions = structArrayTyped<VkBufferImageCopy2>(
            address(info->pRegions), info->regionCount, false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2: {
        VkResolveImageInfo2* info = (VkResolveImageInfo2*)host;
        info->pRegions = structArrayTyped<VkImageResolve2>(
            address(info->pRegions), info->regionCount, false);
        break;
    }

    // ---- The core commands the Vulkan 1.4 audit added ----------------------

    case (U32)VK_STRUCTURE_TYPE_BIND_SPARSE_INFO: {
        // Three arrays of non-extensible structures, each of which holds one
        // further array of flat bind records. MoltenVK reports no sparse
        // residency, so nothing should ever arrive here -- but the command is
        // core 1.0, so winevulkan puts a slot for it in its dispatch table
        // either way, and a slot is better filled than null.
        VkBindSparseInfo* info = (VkBindSparseInfo*)host;
        info->pWaitSemaphores =
            inArray(info->pWaitSemaphores, info->waitSemaphoreCount);
        VkSparseBufferMemoryBindInfo* buffers =
            inArrayWritable<VkSparseBufferMemoryBindInfo>(
                address(info->pBufferBinds), info->bufferBindCount);
        info->pBufferBinds = buffers;
        for (U32 i = 0; buffers && i < info->bufferBindCount && !failed; ++i) {
            buffers[i].pBinds =
                inArray(buffers[i].pBinds, buffers[i].bindCount);
        }
        VkSparseImageOpaqueMemoryBindInfo* opaque =
            inArrayWritable<VkSparseImageOpaqueMemoryBindInfo>(
                address(info->pImageOpaqueBinds), info->imageOpaqueBindCount);
        info->pImageOpaqueBinds = opaque;
        for (U32 i = 0;
             opaque && i < info->imageOpaqueBindCount && !failed; ++i) {
            opaque[i].pBinds = inArray(opaque[i].pBinds, opaque[i].bindCount);
        }
        VkSparseImageMemoryBindInfo* images =
            inArrayWritable<VkSparseImageMemoryBindInfo>(
                address(info->pImageBinds), info->imageBindCount);
        info->pImageBinds = images;
        for (U32 i = 0; images && i < info->imageBindCount && !failed; ++i) {
            images[i].pBinds = inArray(images[i].pBinds, images[i].bindCount);
        }
        info->pSignalSemaphores =
            inArray(info->pSignalSemaphores, info->signalSemaphoreCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS: {
        VkDeviceBufferMemoryRequirements* info =
            (VkDeviceBufferMemoryRequirements*)host;
        info->pCreateInfo = (const VkBufferCreateInfo*)chain(
            address(info->pCreateInfo), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS: {
        VkDeviceImageMemoryRequirements* info =
            (VkDeviceImageMemoryRequirements*)host;
        info->pCreateInfo = (const VkImageCreateInfo*)chain(
            address(info->pCreateInfo), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO: {
        VkDeviceImageSubresourceInfo* info =
            (VkDeviceImageSubresourceInfo*)host;
        info->pCreateInfo = (const VkImageCreateInfo*)chain(
            address(info->pCreateInfo), false);
        info->pSubresource = (const VkImageSubresource2*)chain(
            address(info->pSubresource), false);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDERING_AREA_INFO: {
        VkRenderingAreaInfo* info = (VkRenderingAreaInfo*)host;
        info->pColorAttachmentFormats = inArrayOptional(
            info->pColorAttachmentFormats, info->colorAttachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO: {
        VkRenderingAttachmentLocationInfo* info =
            (VkRenderingAttachmentLocationInfo*)host;
        info->pColorAttachmentLocations = inArrayOptional(
            info->pColorAttachmentLocations, info->colorAttachmentCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO: {
        VkRenderingInputAttachmentIndexInfo* info =
            (VkRenderingInputAttachmentIndexInfo*)host;
        info->pColorAttachmentInputIndices = inArrayOptional(
            info->pColorAttachmentInputIndices, info->colorAttachmentCount);
        // Both of these are single optional uint32_t, not arrays: null means
        // the attachment keeps its default index.
        info->pDepthInputAttachmentIndex =
            inArrayOptional(info->pDepthInputAttachmentIndex, 1);
        info->pStencilInputAttachmentIndex =
            inArrayOptional(info->pStencilInputAttachmentIndex, 1);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO: {
        VkBindDescriptorSetsInfo* info = (VkBindDescriptorSetsInfo*)host;
        info->pDescriptorSets =
            inArray(info->pDescriptorSets, info->descriptorSetCount);
        info->pDynamicOffsets =
            inArrayOptional(info->pDynamicOffsets, info->dynamicOffsetCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO: {
        VkPushConstantsInfo* info = (VkPushConstantsInfo*)host;
        info->pValues = inArrayAt(address(info->pValues), info->size, 1);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO: {
        VkPushDescriptorSetInfo* info = (VkPushDescriptorSetInfo*)host;
        info->pDescriptorWrites = structArrayTyped<VkWriteDescriptorSet>(
            address(info->pDescriptorWrites), info->descriptorWriteCount,
            false);
        break;
    }
    // ---- Surface and swapchain maintenance1 --------------------------------

    case (U32)VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT: {
        // The one node in the table that the driver WRITES INTO and that holds
        // a pointer, so it is the one place both halves matter. DXVK runs the
        // two-call idiom through it: the first call has presentModeCount 0 and
        // pPresentModes null and collects the count, the second passes an
        // array of that size and collects the modes.
        VkSurfacePresentModeCompatibilityEXT* info =
            (VkSurfacePresentModeCompatibilityEXT*)host;
        const U64 guestModes = address(info->pPresentModes);
        info->pPresentModes = outArray(info->pPresentModes,
                                       info->presentModeCount);
        // Without this the write-back would hand the guest the shadow's own
        // host address in place of its array pointer.
        restorePointer(&info->pPresentModes, guestModes);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT: {
        VkSwapchainPresentModesCreateInfoEXT* info =
            (VkSwapchainPresentModesCreateInfoEXT*)host;
        info->pPresentModes =
            inArray(info->pPresentModes, info->presentModeCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
        VkSwapchainPresentModeInfoEXT* info =
            (VkSwapchainPresentModeInfoEXT*)host;
        info->pPresentModes =
            inArray(info->pPresentModes, info->swapchainCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
        // One fence per swapchain, attached to a present and signalled when
        // the presentation engine is done with the image. DXVK waits on it
        // before reusing that image, so a dropped node here is a wait on a
        // fence nothing will ever signal.
        VkSwapchainPresentFenceInfoEXT* info =
            (VkSwapchainPresentFenceInfoEXT*)host;
        info->pFences = inArray(info->pFences, info->swapchainCount);
        break;
    }
    case (U32)VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT: {
        VkReleaseSwapchainImagesInfoEXT* info =
            (VkReleaseSwapchainImagesInfoEXT*)host;
        info->pImageIndices =
            inArray(info->pImageIndices, info->imageIndexCount);
        break;
    }

    case (U32)VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO: {
        // The same problem vkUpdateDescriptorSetWithTemplate has, with the
        // template handle and the data in one structure rather than in two
        // argument words: the length of pData is the span recorded when the
        // template was created.
        VkPushDescriptorSetWithTemplateInfo* info =
            (VkPushDescriptorSetWithTemplateInfo*)host;
        U64 bytes = 0;
        if (!templateDataBytes(info->descriptorUpdateTemplate, &bytes)) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s "
                     "status=no-span template=0x%llx",
                     command,
                     (unsigned long long)(uintptr_t)
                         info->descriptorUpdateTemplate);
            fail(address(info->pData), 0);
            return;
        }
        info->pData = inArrayAt(address(info->pData), bytes, 1);
        break;
    }
    default:
        break;
    }
}

// ---- The surface registry ---------------------------------------------------
//
// Which X11 window a VkSurfaceKHR was made from, and how big that window was
// when it was made.
//
// The native presentation backend already holds this (KVulkdanSDLImpl keeps a
// VulkanSurfaceRecord per surface), but it holds it behind its own lock and
// exposes only isPresentationSurface(). This bridge needs the size to answer a
// question the backend never asks: whether the extent the host reports for a
// surface is the size of the guest window that surface was made from.
//
// A device run said it is not. The guest window was 804x585 for the whole run,
// and the host's presentation layer briefly re-parented its view during the
// first frame; the capability query that DXVK made inside that window returned
// 740x555, DXVK built a swapchain at 740x555, discovered the disagreement, and
// tore the whole swapchain down and rebuilt it at 804x585. Nothing in the log
// said that the extent had moved -- only that two swapchains were created with
// different "Buffer size" lines, which is a symptom several unrelated causes
// share. Recording the window size here makes the next log say it outright.
struct SurfaceWindow {
    U64 surface = 0;
    U32 windowId = 0;
    U32 width = 0;
    U32 height = 0;
};

std::mutex gSurfaceWindowsMutex;
std::vector<SurfaceWindow> gSurfaceWindows;

void rememberSurfaceWindow(U64 surface, U32 windowId, U32 width, U32 height) {
    if (!surface) {
        return;
    }
    std::lock_guard<std::mutex> lock(gSurfaceWindowsMutex);
    for (SurfaceWindow& record : gSurfaceWindows) {
        if (record.surface == surface) {
            record.windowId = windowId;
            record.width = width;
            record.height = height;
            return;
        }
    }
    gSurfaceWindows.push_back({surface, windowId, width, height});
}

// Returns false for a surface this bridge did not create, which is not an
// error: Wine builds surfaces through other paths and the witness simply has
// nothing to compare against for those.
bool surfaceWindow(U64 surface, SurfaceWindow* out) {
    if (!surface) {
        return false;
    }
    std::lock_guard<std::mutex> lock(gSurfaceWindowsMutex);
    for (const SurfaceWindow& record : gSurfaceWindows) {
        if (record.surface == surface) {
            *out = record;
            return true;
        }
    }
    return false;
}

// ---- The native presentation backend ----------------------------------------
//
// KVulkan carries five notifications the presentation backend uses to tell a
// created Metal view from a guest that actually presented a frame:
// registerVulkanSwapchain, destroyVulkanSwapchain, acquireVulkanSwapchain,
// submitVulkanWorkload and presentVulkanSwapchain. Until this revision NO lane
// called any of them, so every one of the diagnostics built on top of them was
// reasoning from an empty table:
//
//   - the first-frame watchdog fires on !firstPresentObserved, and
//     firstPresentObserved is set only by presentVulkanSwapchain, so it could
//     never be cleared. "Produced no frame for 12 seconds" was true of every
//     run that ever reached a surface, whether or not the guest presented.
//   - registerVulkanSwapchain is the swapchain-rebuild-storm detector, and
//     with an empty swapchainSurfaces map acquireVulkanSwapchain and
//     presentVulkanSwapchain return at their first lookup.
//   - "iOS guest performance: N Vulkan frames/sec" counts presents through
//     bvnReportPresentRate(), which only presentVulkanSwapchain calls.
//
// So the wiring below is not new instrumentation; it is the connection those
// three witnesses were always written against. It also means the watchdog's
// verdict becomes evidence: after this, "no frame for 12 seconds" says the
// guest made no successful vkQueuePresentKHR, rather than saying nothing.
KVulkanPtr presentationBackend() {
    return KNativeSystem::getVulkan();
}

// ---- The surface extent witness ---------------------------------------------
//
// Budgeted while the extent agrees with the guest window, unbudgeted while it
// does not, because a disagreement is the interesting event and a program that
// queries capabilities every frame would otherwise spend the budget on the
// boring case. A disagreement that repeats is rate-limited to one line a
// second by the count, not silenced: a storm of them IS the finding.
std::atomic<U32> gSurfaceExtentReports{0};
std::atomic<U32> gSurfaceExtentMismatches{0};

void noteSurfaceExtent(const char* call, U32 tid, U64 surface,
                       const VkSurfaceCapabilitiesKHR* capabilities,
                       VkResult result) {
    if (!capabilities) {
        return;
    }
    SurfaceWindow window;
    const bool known = surfaceWindow(surface, &window);
    // 0xffffffff in both fields is the "the extent is whatever the swapchain
    // asks for" encoding, which is not a disagreement with anything.
    const bool undefined = capabilities->currentExtent.width == 0xffffffffu &&
                           capabilities->currentExtent.height == 0xffffffffu;
    const bool mismatch = known && !undefined &&
        (capabilities->currentExtent.width != window.width ||
         capabilities->currentExtent.height != window.height);
    const U32 seen = gSurfaceExtentReports.fetch_add(
        1, std::memory_order_relaxed);
    U32 mismatches = 0;
    if (mismatch) {
        mismatches = gSurfaceExtentMismatches.fetch_add(
            1, std::memory_order_relaxed) + 1;
    }
    if (!mismatch && seen >= 6 && result >= 0) {
        return;
    }
    if (mismatch && mismatches > 8 && (mismatches % 60) != 0) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_VULKAN_SURFACE_EXTENT call=%s tid=%04X "
             "surface=0x%llx window=0x%x window_size=%ux%u current=%ux%u "
             "min=%ux%u max=%ux%u images=%u..%u agrees=%d mismatches=%u "
             "status=%d",
             call, tid, (unsigned long long)surface,
             known ? window.windowId : 0, window.width, window.height,
             capabilities->currentExtent.width,
             capabilities->currentExtent.height,
             capabilities->minImageExtent.width,
             capabilities->minImageExtent.height,
             capabilities->maxImageExtent.width,
             capabilities->maxImageExtent.height,
             capabilities->minImageCount, capabilities->maxImageCount,
             known ? (mismatch ? 0 : 1) : -1, mismatches, (int)result);
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
// pointer. VkXlibSurfaceCreateInfoKHR lives in vulkan_xlib.h, which is not
// vendored under source/vulkan/vk, so its two fields are read by offset.
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
    const U32 windowWidth = xWindow ? xWindow->width() : 0;
    const U32 windowHeight = xWindow ? xWindow->height() : 0;
    rememberSurfaceWindow(handle, (U32)windowId, windowWidth, windowHeight);
    // Wine builds throwaway surfaces purely to probe adapter capabilities;
    // only a real presentation surface may take over the fake-fullscreen
    // target, which is the rule the IA-32 lane already follows.
    if (vulkanWnd->isPresentationSurface(surface)) {
        XServer::getServer()->setFakeFullScreenWindow(xWindow);
    }
    klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkCreateXlibSurfaceKHR "
             "window=0x%x surface=0x%llx window_size=%ux%u status=0",
             (U32)windowId, (unsigned long long)handle, windowWidth,
             windowHeight);
    return VK_SUCCESS;
}

// ---- The platform surface extension -----------------------------------------
//
// Wine's winevulkan asks the display driver which host surface extension to
// use and substitutes that name for the application's VK_KHR_win32_surface
// before the call reaches this bridge. The display driver here is winex11.drv,
// which answers VK_KHR_xlib_surface unconditionally because it is an X11
// driver. MoltenVK has no Xlib surface extension -- it has
// VK_EXT_metal_surface -- so a bridge that forwarded the names verbatim handed
// MoltenVK a name it does not have, and a device run showed exactly that: Wine's
// own adapter probe succeeded (it enables no surface extension at all) and
// DXVK's vkCreateInstance answered VK_ERROR_EXTENSION_NOT_PRESENT the moment it
// asked for a surface.
//
// The IA-32 lane has always translated in both directions
// (vk_EnumerateInstanceExtensionProperties and vk_CreateInstance in
// source/vulkan/vulkancommon.cpp). This is the same translation:
//
//   enumeration: the host's platform surface name -> VK_KHR_xlib_surface
//   creation:    VK_KHR_xlib_surface -> the host's platform surface name
//
// A rename rather than an addition, so the property count is identical in both
// halves of the two-call idiom and a caller that sized its array from the first
// call is still right on the second.
//
// There is no device-level equivalent. Every platform surface extension is
// instance-level; VK_KHR_swapchain, the device-level half of presentation, is
// spelled the same on both sides. The IA-32 lane translates nothing in
// vk_EnumerateDeviceExtensionProperties or vk_CreateDevice either.
const char* const kGuestSurfaceExtension = "VK_KHR_xlib_surface";

#if defined(BOXEDWINE_IOS)
// SDL's UIKit Vulkan backend creates a CAMetalLayer-backed VkSurfaceKHR
// through VK_EXT_metal_surface, and createXlibSurface() below reaches that
// same path through KNativeSystem::getVulkan().
const char* const kHostSurfaceExtension = "VK_EXT_metal_surface";
#elif defined(__MACH__)
const char* const kHostSurfaceExtension = "VK_MVK_macos_surface";
#elif defined(_WIN32)
const char* const kHostSurfaceExtension = "VK_KHR_win32_surface";
#else
// A host whose driver already reports the name Wine expects. Spelled out
// rather than left to the IA-32 lane's #else, which names
// VK_KHR_win32_surface unconditionally and would rewrite a Linux host's own
// Xlib surface extension away. Both directions are the identity here.
const char* const kHostSurfaceExtension = "VK_KHR_xlib_surface";
#endif

// Every name the enumeration will accept as "the host's platform surface".
// More than one, because a build may meet a driver that reports a platform
// surface it was not compiled for -- MoltenVK reports both
// VK_EXT_metal_surface and VK_MVK_ios_surface -- and the answer Wine needs is
// the same in every case. Exactly ONE entry is ever renamed, and the one this
// build can actually deliver is preferred, so the reported list never carries
// VK_KHR_xlib_surface twice.
const char* const kHostSurfaceAliases[] = {
    "VK_EXT_metal_surface",
    "VK_MVK_macos_surface",
    "VK_MVK_ios_surface",
    "VK_KHR_win32_surface",
    "VK_KHR_xcb_surface",
    "VK_KHR_wayland_surface",
};

bool isHostSurfaceAlias(const char* name) {
    for (U32 i = 0;
         i < (U32)(sizeof(kHostSurfaceAliases) / sizeof(kHostSurfaceAliases[0]));
         ++i) {
        if (!strcmp(name, kHostSurfaceAliases[i])) {
            return true;
        }
    }
    return false;
}

std::atomic<U32> gSurfaceRenameReports{0};

// Rewrite the host's platform surface extension in an enumerated property
// array to the name Wine's X11 driver expects. Returns the number of entries
// rewritten, which is 0 or 1 -- never more, so the count the guest was given
// stays exactly what the driver reported.
U32 renameHostSurfaceExtension(VkExtensionProperties* properties, U32 count) {
    if (!properties || !count) {
        return 0;
    }
    int hostChoice = -1;
    int aliasChoice = -1;
    for (U32 i = 0; i < count; ++i) {
        const char* name = properties[i].extensionName;
        if (!strcmp(name, kGuestSurfaceExtension)) {
            // The driver already reports the name Wine wants. Renaming a
            // second entry onto it would report it twice.
            return 0;
        }
        if (hostChoice < 0 && !strcmp(name, kHostSurfaceExtension)) {
            hostChoice = (int)i;
        }
        if (aliasChoice < 0 && isHostSurfaceAlias(name)) {
            aliasChoice = (int)i;
        }
    }
    const int chosen = hostChoice >= 0 ? hostChoice : aliasChoice;
    const U32 reported =
        gSurfaceRenameReports.fetch_add(1, std::memory_order_relaxed);
    if (chosen < 0) {
        // Loud, and deliberately so. Without a platform surface in the list
        // winevulkan has nothing to map VK_KHR_win32_surface onto, DXVK is
        // told the surface extension it requires is absent, and the failure
        // that follows says nothing about why.
        if (reported < 4) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE "
                     "call=vkEnumerateInstanceExtensionProperties "
                     "surface=none count=%u status=no-platform-surface",
                     count);
        }
        return 0;
    }
    char* slot = properties[chosen].extensionName;
    if (reported < 4) {
        klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE "
                 "call=vkEnumerateInstanceExtensionProperties surface=%s "
                 "reported=%s preferred=%d count=%u",
                 slot, kGuestSurfaceExtension, hostChoice >= 0 ? 1 : 0, count);
    }
    size_t length = ::strlen(kGuestSurfaceExtension);
    if (length > VK_MAX_EXTENSION_NAME_SIZE - 1) {
        length = VK_MAX_EXTENSION_NAME_SIZE - 1;
    }
    ::memset(slot, 0, VK_MAX_EXTENSION_NAME_SIZE);
    ::memcpy(slot, kGuestSurfaceExtension, length);
    return 1;
}

std::atomic<U32> gSurfaceSubstituteReports{0};

// The other direction: put the host's own platform surface name back before
// the create info reaches the driver. The array and its strings are the
// marshal's own host allocations, so the entry is repointed at a static
// literal rather than rewritten in place.
U32 substituteHostSurfaceExtension(const char* const* names, U32 count) {
    if (!names || !count || !strcmp(kHostSurfaceExtension,
                                    kGuestSurfaceExtension)) {
        return 0;
    }
    const char** writable = const_cast<const char**>(names);
    U32 substituted = 0;
    for (U32 i = 0; i < count; ++i) {
        if (names[i] && !strcmp(names[i], kGuestSurfaceExtension)) {
            writable[i] = kHostSurfaceExtension;
            ++substituted;
        }
    }
    if (substituted) {
        const U32 reported =
            gSurfaceSubstituteReports.fetch_add(1, std::memory_order_relaxed);
        if (reported < 4) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkCreateInstance "
                     "requested=%s forwarded=%s count=%u",
                     kGuestSurfaceExtension, kHostSurfaceExtension,
                     substituted);
        }
    }
    return substituted;
}

// ---- The dispatcher ---------------------------------------------------------

#define A(n) args[n]
#define H(n) ((void*)(uintptr_t)args[n])
#define U32A(n) ((uint32_t)args[n])

// The mirror image of bw_f32() in tools/vulkan-64/vulkan.c. A float parameter
// crosses as the bit pattern of its IEEE-754 binary32 value in the low 32 bits
// of an argument word, so it is read back out of the object representation
// rather than converted: casting the word 0x3fc00000 to float would give
// 1069547520.0f where the guest passed 1.5f.
float f32(U64 word) {
    const uint32_t bits = (uint32_t)word;
    float value = 0.0f;
    ::memcpy(&value, &bits, sizeof(value));
    return value;
}

#define F32A(n) f32(args[n])

// H(n) is for a Vulkan HANDLE, which is a host value the driver gave out and
// never a guest address. Everything else that is a pointer goes through the
// marshal.
//
// The argument array reaching here is always BOXEDWINE_X64_VK_MAX_ARGS words
// wide and zero beyond `count`, so a command reached with too few arguments
// reads zeros: a null pointer the marshal declines rather than an out-of-bounds
// read. No per-command arity table is needed for safety.
//
// Every command below is called through its real prototype, so the compiler
// checks each cast. Nothing here is generated; a command is in the table
// because it was added deliberately.
// The instance a command is looked up through. A command that carries its own
// VkInstance uses that one: it is alive by the caller's own contract, whereas
// the bridge's current instance may be a different one, or none at all when
// the last instance is in the middle of being destroyed.
VkInstance resolutionInstance(int index, const U64* args) {
    switch (index) {
    case VKB_DestroyInstance:
    case VKB_EnumeratePhysicalDevices:
    case VKB_DestroySurfaceKHR:
        if (args[0]) {
            return (VkInstance)(uintptr_t)args[0];
        }
        break;
    default:
        break;
    }
    return gInstance;
}

// `tid` is the guest thread that made the call. It is carried in only so the
// two swapchain witnesses can name it: an acquire and a present that come from
// different threads is normal, an acquire from a thread that then leaves the
// process is not, and neither statement could be made from a line that named
// only the process.
S64 dispatchCommand(int index, Marshal& m, const U64* args, U64 count, U32 tid) {
    (void)count;
    (void)tid;
    if (index == VKB_DestroyInstance &&
        !instanceIsLive((VkInstance)(uintptr_t)args[0])) {
        // Settled before resolution, because resolution would dereference the
        // handle. A host Vulkan handle is a host pointer: a destroy for one
        // this bridge did not hand out, or already destroyed, can only be
        // answered by doing nothing. vkDestroyInstance returns void, so there
        // is no error to report and none is invented -- an earlier revision
        // reported BOXEDWINE_X64_VK_E_NOPROC here, which is what a device run
        // saw when Wine's adapter probe destroyed its instance twice.
        static std::atomic<U32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkDestroyInstance "
                     "instance=0x%llx status=not-live",
                     (unsigned long long)args[0]);
        }
        return 0;
    }
    PFN_vkVoidFunction raw = hostProc(index, resolutionInstance(index, args));
    if (!raw) {
        return BOXEDWINE_X64_VK_E_NOPROC;
    }
    switch (index) {
    case VKB_EnumerateInstanceVersion: {
        uint32_t* version = (uint32_t*)m.outRequired(A(0), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumerateInstanceVersion)raw)(version);
    }
    case VKB_EnumerateInstanceLayerProperties: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(0), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkLayerProperties* properties = (VkLayerProperties*)m.out(
            A(1), (U64)*countSlot * sizeof(VkLayerProperties));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumerateInstanceLayerProperties)raw)(countSlot,
                                                                  properties);
    }
    case VKB_EnumerateInstanceExtensionProperties: {
        const char* layer = m.string(A(0));
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        const U32 capacity = *countSlot;
        VkExtensionProperties* properties = (VkExtensionProperties*)m.out(
            A(2), (U64)capacity * sizeof(VkExtensionProperties));
        if (!m.ok()) {
            return m.error();
        }
        const VkResult result =
            ((PFN_vkEnumerateInstanceExtensionProperties)raw)(layer, countSlot,
                                                              properties);
        // Before the write-back, and only over what the driver actually
        // wrote: on the first call of the two-call idiom `properties` is null
        // and only the count comes back, and the rename cannot change a count
        // it never touches.
        if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
            const U32 written = *countSlot < capacity ? *countSlot : capacity;
            renameHostSurfaceExtension(properties, written);
        }
        return (S64)result;
    }
    case VKB_CreateInstance: {
        const VkInstanceCreateInfo* info =
            (const VkInstanceCreateInfo*)m.chain(A(0), false);
        VkInstance* out = (VkInstance*)m.outRequired(A(2), sizeof(VkInstance));
        if (!m.ok()) {
            return m.error();
        }
        // The other half of the surface-extension translation. The shadow
        // create info is this marshal's own memory, so the name Wine handed
        // down is replaced with the one the driver actually has.
        substituteHostSurfaceExtension(info->ppEnabledExtensionNames,
                                       info->enabledExtensionCount);
        const VkResult result =
            ((PFN_vkCreateInstance)raw)(info, nullptr, out);
        if (result == VK_SUCCESS && out && *out) {
            noteInstanceCreated(*out);
        }
        return (S64)result;
    }
    case VKB_DestroyInstance: {
        // Liveness was settled above, before resolution. Dropping it from the
        // list first re-points the bridge's current instance at whatever is
        // still alive, so a second live instance keeps working.
        VkInstance instance = (VkInstance)H(0);
        forgetInstance(instance);
        ((PFN_vkDestroyInstance)raw)(instance, nullptr);
        return 0;
    }
    case VKB_EnumeratePhysicalDevices: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkPhysicalDevice* devices = (VkPhysicalDevice*)m.out(
            A(2), (U64)*countSlot * sizeof(VkPhysicalDevice));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumeratePhysicalDevices)raw)(
            (VkInstance)H(0), countSlot, devices);
    }

    case VKB_GetPhysicalDeviceProperties: {
        VkPhysicalDeviceProperties* properties =
            (VkPhysicalDeviceProperties*)m.outRequired(
                A(1), sizeof(VkPhysicalDeviceProperties));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceProperties)raw)((VkPhysicalDevice)H(0),
                                                 properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceProperties2: {
        VkPhysicalDeviceProperties2* properties =
            (VkPhysicalDeviceProperties2*)m.chain(A(1), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceProperties2)raw)((VkPhysicalDevice)H(0),
                                                  properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceFeatures: {
        VkPhysicalDeviceFeatures* features =
            (VkPhysicalDeviceFeatures*)m.outRequired(
                A(1), sizeof(VkPhysicalDeviceFeatures));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceFeatures)raw)((VkPhysicalDevice)H(0),
                                               features);
        return 0;
    }
    case VKB_GetPhysicalDeviceFeatures2: {
        VkPhysicalDeviceFeatures2* features =
            (VkPhysicalDeviceFeatures2*)m.chain(A(1), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceFeatures2)raw)((VkPhysicalDevice)H(0),
                                                features);
        return 0;
    }
    case VKB_GetPhysicalDeviceFormatProperties: {
        VkFormatProperties* properties = (VkFormatProperties*)m.outRequired(
            A(2), sizeof(VkFormatProperties));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceFormatProperties)raw)(
            (VkPhysicalDevice)H(0), (VkFormat)U32A(1), properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceFormatProperties2: {
        VkFormatProperties2* properties =
            (VkFormatProperties2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceFormatProperties2)raw)(
            (VkPhysicalDevice)H(0), (VkFormat)U32A(1), properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceImageFormatProperties: {
        VkImageFormatProperties* properties =
            (VkImageFormatProperties*)m.outRequired(
                A(6), sizeof(VkImageFormatProperties));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkGetPhysicalDeviceImageFormatProperties)raw)(
            (VkPhysicalDevice)H(0), (VkFormat)U32A(1), (VkImageType)U32A(2),
            (VkImageTiling)U32A(3), (VkImageUsageFlags)U32A(4),
            (VkImageCreateFlags)U32A(5), properties);
    }
    case VKB_GetPhysicalDeviceImageFormatProperties2: {
        const VkPhysicalDeviceImageFormatInfo2* info =
            (const VkPhysicalDeviceImageFormatInfo2*)m.chain(A(1), false);
        VkImageFormatProperties2* properties =
            (VkImageFormatProperties2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkGetPhysicalDeviceImageFormatProperties2)raw)(
            (VkPhysicalDevice)H(0), info, properties);
    }
    case VKB_GetPhysicalDeviceQueueFamilyProperties: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkQueueFamilyProperties* properties = (VkQueueFamilyProperties*)m.out(
            A(2), (U64)*countSlot * sizeof(VkQueueFamilyProperties));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)raw)(
            (VkPhysicalDevice)H(0), countSlot, properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceQueueFamilyProperties2: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkQueueFamilyProperties2* properties =
            m.structArrayTyped<VkQueueFamilyProperties2>(A(2), *countSlot,
                                                         true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties2)raw)(
            (VkPhysicalDevice)H(0), countSlot, properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceMemoryProperties: {
        VkPhysicalDeviceMemoryProperties* properties =
            (VkPhysicalDeviceMemoryProperties*)m.outRequired(
                A(1), sizeof(VkPhysicalDeviceMemoryProperties));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceMemoryProperties)raw)((VkPhysicalDevice)H(0),
                                                       properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceMemoryProperties2: {
        VkPhysicalDeviceMemoryProperties2* properties =
            (VkPhysicalDeviceMemoryProperties2*)m.chain(A(1), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceMemoryProperties2)raw)((VkPhysicalDevice)H(0),
                                                        properties);
        return 0;
    }
    case VKB_EnumerateDeviceLayerProperties: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkLayerProperties* properties = (VkLayerProperties*)m.out(
            A(2), (U64)*countSlot * sizeof(VkLayerProperties));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumerateDeviceLayerProperties)raw)(
            (VkPhysicalDevice)H(0), countSlot, properties);
    }
    case VKB_EnumerateDeviceExtensionProperties: {
        const char* layer = m.string(A(1));
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkExtensionProperties* properties = (VkExtensionProperties*)m.out(
            A(3), (U64)*countSlot * sizeof(VkExtensionProperties));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumerateDeviceExtensionProperties)raw)(
            (VkPhysicalDevice)H(0), layer, countSlot, properties);
    }
    case VKB_GetPhysicalDeviceExternalBufferProperties: {
        const VkPhysicalDeviceExternalBufferInfo* info =
            (const VkPhysicalDeviceExternalBufferInfo*)m.chain(A(1), false);
        VkExternalBufferProperties* properties =
            (VkExternalBufferProperties*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceExternalBufferProperties)raw)(
            (VkPhysicalDevice)H(0), info, properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceExternalSemaphoreProperties: {
        const VkPhysicalDeviceExternalSemaphoreInfo* info =
            (const VkPhysicalDeviceExternalSemaphoreInfo*)m.chain(A(1), false);
        VkExternalSemaphoreProperties* properties =
            (VkExternalSemaphoreProperties*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)raw)(
            (VkPhysicalDevice)H(0), info, properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceExternalFenceProperties: {
        const VkPhysicalDeviceExternalFenceInfo* info =
            (const VkPhysicalDeviceExternalFenceInfo*)m.chain(A(1), false);
        VkExternalFenceProperties* properties =
            (VkExternalFenceProperties*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceExternalFenceProperties)raw)(
            (VkPhysicalDevice)H(0), info, properties);
        return 0;
    }

    case VKB_CreateDevice: {
        const VkDeviceCreateInfo* info =
            (const VkDeviceCreateInfo*)m.chain(A(1), false);
        VkDevice* out = (VkDevice*)m.outRequired(A(3), sizeof(VkDevice));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateDevice)raw)((VkPhysicalDevice)H(0), info,
                                              nullptr, out);
    }
    case VKB_DestroyDevice:
        ((PFN_vkDestroyDevice)raw)((VkDevice)H(0), nullptr);
        return 0;
    case VKB_GetDeviceQueue: {
        VkQueue* out = (VkQueue*)m.outRequired(A(3), sizeof(VkQueue));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceQueue)raw)((VkDevice)H(0), U32A(1), U32A(2), out);
        return 0;
    }
    case VKB_GetDeviceQueue2: {
        const VkDeviceQueueInfo2* info =
            (const VkDeviceQueueInfo2*)m.chain(A(1), false);
        VkQueue* out = (VkQueue*)m.outRequired(A(2), sizeof(VkQueue));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceQueue2)raw)((VkDevice)H(0), info, out);
        return 0;
    }
    case VKB_DeviceWaitIdle:
        return (S64)((PFN_vkDeviceWaitIdle)raw)((VkDevice)H(0));
    case VKB_QueueWaitIdle:
        return (S64)((PFN_vkQueueWaitIdle)raw)((VkQueue)H(0));
    case VKB_QueueSubmit: {
        const VkSubmitInfo* submits =
            m.structArrayTyped<VkSubmitInfo>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result = ((PFN_vkQueueSubmit)raw)(
            (VkQueue)H(0), U32A(1), submits, (VkFence)A(3));
        if (KVulkanPtr backend = presentationBackend()) {
            backend->submitVulkanWorkload((int)result, U32A(1),
                                          "vkQueueSubmit");
        }
        return (S64)result;
    }

    case VKB_AllocateMemory: {
        const VkMemoryAllocateInfo* info =
            (const VkMemoryAllocateInfo*)m.chain(A(1), false);
        VkDeviceMemory* out =
            (VkDeviceMemory*)m.outRequired(A(3), sizeof(VkDeviceMemory));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkAllocateMemory)raw)((VkDevice)H(0), info, nullptr,
                                                out);
    }
    case VKB_FreeMemory:
        ((PFN_vkFreeMemory)raw)((VkDevice)H(0), (VkDeviceMemory)A(1), nullptr);
        return 0;
    case VKB_MapMemory: {
        U64* slot = (U64*)m.outRequired(A(5), sizeof(U64));
        if (!m.ok()) {
            return m.error();
        }
        void* mapped = nullptr;
        const VkResult result = ((PFN_vkMapMemory)raw)(
            (VkDevice)H(0), (VkDeviceMemory)A(1), (VkDeviceSize)A(2),
            (VkDeviceSize)A(3), (VkMemoryMapFlags)U32A(4), &mapped);
        if (result == VK_SUCCESS) {
            // MoltenVK hands back a host address. When the allocation was
            // imported from the guest -- VK_EXT_external_memory_host, which
            // is how Wine's WoW64 layer gives a 32-bit caller a mapping it
            // can hold, because wine_vkMapMemory2KHR refuses anything above
            // 4 GiB -- that address is the alias of a guest page, and the
            // guest must be handed the canonical guest address rather than
            // the alias. An address outside both alias windows is not guest
            // memory and comes back unchanged.
            const U64 handed =
                boxedvn::hostToGuestAddress((U64)(uintptr_t)mapped);
            *slot = handed;
            static std::atomic<U32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                klog_fmt("BOXEDWINE_X64_VULKAN_MAP memory=0x%llx offset=0x%llx "
                         "size=0x%llx host=0x%llx guest=0x%llx low4g=%d",
                         (unsigned long long)A(1), (unsigned long long)A(2),
                         (unsigned long long)A(3),
                         (unsigned long long)(uintptr_t)mapped,
                         (unsigned long long)handed,
                         (handed >> 32) == 0 ? 1 : 0);
            }
        }
        return (S64)result;
    }
    case VKB_UnmapMemory:
        ((PFN_vkUnmapMemory)raw)((VkDevice)H(0), (VkDeviceMemory)A(1));
        return 0;
    case VKB_FlushMappedMemoryRanges: {
        const VkMappedMemoryRange* ranges =
            m.structArrayTyped<VkMappedMemoryRange>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkFlushMappedMemoryRanges)raw)((VkDevice)H(0),
                                                         U32A(1), ranges);
    }
    case VKB_InvalidateMappedMemoryRanges: {
        const VkMappedMemoryRange* ranges =
            m.structArrayTyped<VkMappedMemoryRange>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkInvalidateMappedMemoryRanges)raw)((VkDevice)H(0),
                                                              U32A(1), ranges);
    }
    case VKB_GetBufferMemoryRequirements: {
        VkMemoryRequirements* requirements =
            (VkMemoryRequirements*)m.outRequired(A(2),
                                                 sizeof(VkMemoryRequirements));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetBufferMemoryRequirements)raw)((VkDevice)H(0),
                                                 (VkBuffer)A(1), requirements);
        return 0;
    }
    case VKB_GetBufferMemoryRequirements2: {
        const VkBufferMemoryRequirementsInfo2* info =
            (const VkBufferMemoryRequirementsInfo2*)m.chain(A(1), false);
        VkMemoryRequirements2* requirements =
            (VkMemoryRequirements2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetBufferMemoryRequirements2)raw)((VkDevice)H(0), info,
                                                  requirements);
        return 0;
    }
    case VKB_BindBufferMemory:
        return (S64)((PFN_vkBindBufferMemory)raw)(
            (VkDevice)H(0), (VkBuffer)A(1), (VkDeviceMemory)A(2),
            (VkDeviceSize)A(3));
    case VKB_BindBufferMemory2: {
        const VkBindBufferMemoryInfo* infos =
            m.structArrayTyped<VkBindBufferMemoryInfo>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkBindBufferMemory2)raw)((VkDevice)H(0), U32A(1),
                                                   infos);
    }
    case VKB_GetImageMemoryRequirements: {
        VkMemoryRequirements* requirements =
            (VkMemoryRequirements*)m.outRequired(A(2),
                                                 sizeof(VkMemoryRequirements));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetImageMemoryRequirements)raw)((VkDevice)H(0), (VkImage)A(1),
                                                requirements);
        return 0;
    }
    case VKB_GetImageMemoryRequirements2: {
        const VkImageMemoryRequirementsInfo2* info =
            (const VkImageMemoryRequirementsInfo2*)m.chain(A(1), false);
        VkMemoryRequirements2* requirements =
            (VkMemoryRequirements2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetImageMemoryRequirements2)raw)((VkDevice)H(0), info,
                                                 requirements);
        return 0;
    }
    case VKB_BindImageMemory:
        return (S64)((PFN_vkBindImageMemory)raw)(
            (VkDevice)H(0), (VkImage)A(1), (VkDeviceMemory)A(2),
            (VkDeviceSize)A(3));
    case VKB_BindImageMemory2: {
        const VkBindImageMemoryInfo* infos =
            m.structArrayTyped<VkBindImageMemoryInfo>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkBindImageMemory2)raw)((VkDevice)H(0), U32A(1),
                                                  infos);
    }
    case VKB_GetMemoryHostPointerPropertiesEXT: {
        // The guest asks about memory it placed itself, so this pointer is a
        // guest address the driver has to be shown at its host location.
        void* pointer = m.direct(A(2), K64_PAGE_SIZE, true);
        VkMemoryHostPointerPropertiesEXT* properties =
            (VkMemoryHostPointerPropertiesEXT*)m.chain(A(3), true);
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkGetMemoryHostPointerPropertiesEXT)raw)(
            (VkDevice)H(0), (VkExternalMemoryHandleTypeFlagBits)U32A(1),
            pointer, properties);
    }

    case VKB_CreateBuffer: {
        const VkBufferCreateInfo* info =
            (const VkBufferCreateInfo*)m.chain(A(1), false);
        VkBuffer* out = (VkBuffer*)m.outRequired(A(3), sizeof(VkBuffer));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateBuffer)raw)((VkDevice)H(0), info, nullptr,
                                              out);
    }
    case VKB_DestroyBuffer:
        ((PFN_vkDestroyBuffer)raw)((VkDevice)H(0), (VkBuffer)A(1), nullptr);
        return 0;
    case VKB_CreateImage: {
        const VkImageCreateInfo* info =
            (const VkImageCreateInfo*)m.chain(A(1), false);
        VkImage* out = (VkImage*)m.outRequired(A(3), sizeof(VkImage));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateImage)raw)((VkDevice)H(0), info, nullptr,
                                             out);
    }
    case VKB_DestroyImage:
        ((PFN_vkDestroyImage)raw)((VkDevice)H(0), (VkImage)A(1), nullptr);
        return 0;
    case VKB_GetImageSubresourceLayout: {
        const VkImageSubresource* subresource =
            (const VkImageSubresource*)m.inRequired(
                A(2), sizeof(VkImageSubresource));
        VkSubresourceLayout* layout = (VkSubresourceLayout*)m.outRequired(
            A(3), sizeof(VkSubresourceLayout));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetImageSubresourceLayout)raw)((VkDevice)H(0), (VkImage)A(1),
                                               subresource, layout);
        return 0;
    }
    case VKB_CreateImageView: {
        const VkImageViewCreateInfo* info =
            (const VkImageViewCreateInfo*)m.chain(A(1), false);
        VkImageView* out =
            (VkImageView*)m.outRequired(A(3), sizeof(VkImageView));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateImageView)raw)((VkDevice)H(0), info, nullptr,
                                                 out);
    }
    case VKB_DestroyImageView:
        ((PFN_vkDestroyImageView)raw)((VkDevice)H(0), (VkImageView)A(1),
                                      nullptr);
        return 0;

    case VKB_CreateFence: {
        const VkFenceCreateInfo* info =
            (const VkFenceCreateInfo*)m.chain(A(1), false);
        VkFence* out = (VkFence*)m.outRequired(A(3), sizeof(VkFence));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateFence)raw)((VkDevice)H(0), info, nullptr,
                                             out);
    }
    case VKB_DestroyFence:
        ((PFN_vkDestroyFence)raw)((VkDevice)H(0), (VkFence)A(1), nullptr);
        return 0;
    case VKB_ResetFences: {
        const VkFence* fences = (const VkFence*)m.inArrayAt(
            A(2), U32A(1), sizeof(VkFence));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkResetFences)raw)((VkDevice)H(0), U32A(1), fences);
    }
    case VKB_GetFenceStatus:
        return (S64)((PFN_vkGetFenceStatus)raw)((VkDevice)H(0), (VkFence)A(1));
    case VKB_WaitForFences: {
        const VkFence* fences = (const VkFence*)m.inArrayAt(
            A(2), U32A(1), sizeof(VkFence));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkWaitForFences)raw)((VkDevice)H(0), U32A(1), fences,
                                               (VkBool32)U32A(3),
                                               (uint64_t)A(4));
    }
    case VKB_CreateSemaphore: {
        const VkSemaphoreCreateInfo* info =
            (const VkSemaphoreCreateInfo*)m.chain(A(1), false);
        VkSemaphore* out =
            (VkSemaphore*)m.outRequired(A(3), sizeof(VkSemaphore));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateSemaphore)raw)((VkDevice)H(0), info, nullptr,
                                                 out);
    }
    case VKB_DestroySemaphore:
        ((PFN_vkDestroySemaphore)raw)((VkDevice)H(0), (VkSemaphore)A(1),
                                      nullptr);
        return 0;
    case VKB_GetSemaphoreCounterValue: {
        uint64_t* value = (uint64_t*)m.outRequired(A(2), sizeof(uint64_t));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetSemaphoreCounterValue)raw)(
            (VkDevice)H(0), (VkSemaphore)A(1), value);
    }
    case VKB_WaitSemaphores: {
        const VkSemaphoreWaitInfo* info =
            (const VkSemaphoreWaitInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkWaitSemaphores)raw)((VkDevice)H(0), info,
                                                (uint64_t)A(2));
    }
    case VKB_SignalSemaphore: {
        const VkSemaphoreSignalInfo* info =
            (const VkSemaphoreSignalInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkSignalSemaphore)raw)((VkDevice)H(0), info);
    }

    case VKB_DestroySurfaceKHR:
        ((PFN_vkDestroySurfaceKHR)raw)((VkInstance)H(0), (VkSurfaceKHR)A(1),
                                       nullptr);
        return 0;
    case VKB_GetPhysicalDeviceSurfaceSupportKHR: {
        VkBool32* supported =
            (VkBool32*)m.outRequired(A(3), sizeof(VkBool32));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceSupportKHR)raw)(
            (VkPhysicalDevice)H(0), U32A(1), (VkSurfaceKHR)A(2), supported);
    }
    case VKB_GetPhysicalDeviceSurfaceCapabilitiesKHR: {
        VkSurfaceCapabilitiesKHR* capabilities =
            (VkSurfaceCapabilitiesKHR*)m.outRequired(
                A(2), sizeof(VkSurfaceCapabilitiesKHR));
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result =
            ((PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)raw)(
                (VkPhysicalDevice)H(0), (VkSurfaceKHR)A(1), capabilities);
        noteSurfaceExtent("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", tid,
                          A(1), capabilities, result);
        return (S64)result;
    }
    case VKB_GetPhysicalDeviceSurfaceFormatsKHR: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        VkSurfaceFormatKHR* formats = (VkSurfaceFormatKHR*)m.out(
            A(3), (U64)*countSlot * sizeof(VkSurfaceFormatKHR));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)raw)(
            (VkPhysicalDevice)H(0), (VkSurfaceKHR)A(1), countSlot, formats);
    }
    case VKB_GetPhysicalDeviceSurfacePresentModesKHR: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        VkPresentModeKHR* modes = (VkPresentModeKHR*)m.out(
            A(3), (U64)*countSlot * sizeof(VkPresentModeKHR));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)raw)(
            (VkPhysicalDevice)H(0), (VkSurfaceKHR)A(1), countSlot, modes);
    }
    case VKB_GetPhysicalDeviceSurfaceCapabilities2KHR: {
        const VkPhysicalDeviceSurfaceInfo2KHR* info =
            (const VkPhysicalDeviceSurfaceInfo2KHR*)m.chain(A(1), false);
        VkSurfaceCapabilities2KHR* capabilities =
            (VkSurfaceCapabilities2KHR*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result =
            ((PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)raw)(
                (VkPhysicalDevice)H(0), info, capabilities);
        // The surface is named inside the info structure here rather than in
        // an argument slot, and the capabilities arrive inside a wrapper. Both
        // are read out of the SHADOWS -- the copies the driver was given and
        // wrote into -- because the guest's own memory does not carry the
        // driver's answer until the marshal flushes.
        if (info && capabilities) {
            noteSurfaceExtent("vkGetPhysicalDeviceSurfaceCapabilities2KHR",
                              tid, (U64)info->surface,
                              &capabilities->surfaceCapabilities, result);
        }
        return (S64)result;
    }
    case VKB_GetPhysicalDeviceSurfaceFormats2KHR: {
        const VkPhysicalDeviceSurfaceInfo2KHR* info =
            (const VkPhysicalDeviceSurfaceInfo2KHR*)m.chain(A(1), false);
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        VkSurfaceFormat2KHR* formats =
            m.structArrayTyped<VkSurfaceFormat2KHR>(A(3), *countSlot, true);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)raw)(
            (VkPhysicalDevice)H(0), info, countSlot, formats);
    }
    case VKB_GetPhysicalDevicePresentRectanglesKHR: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        VkRect2D* rects =
            (VkRect2D*)m.out(A(3), (U64)*countSlot * sizeof(VkRect2D));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetPhysicalDevicePresentRectanglesKHR)raw)(
            (VkPhysicalDevice)H(0), (VkSurfaceKHR)A(1), countSlot, rects);
    }
    case VKB_GetDeviceGroupSurfacePresentModesKHR: {
        VkDeviceGroupPresentModeFlagsKHR* modes =
            (VkDeviceGroupPresentModeFlagsKHR*)m.outRequired(
                A(2), sizeof(VkDeviceGroupPresentModeFlagsKHR));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetDeviceGroupSurfacePresentModesKHR)raw)(
            (VkDevice)H(0), (VkSurfaceKHR)A(1), modes);
    }
    case VKB_CreateSwapchainKHR: {
        const VkSwapchainCreateInfoKHR* info =
            (const VkSwapchainCreateInfoKHR*)m.chain(A(1), false);
        VkSwapchainKHR* out =
            (VkSwapchainKHR*)m.outRequired(A(3), sizeof(VkSwapchainKHR));
        if (!m.ok()) {
            return m.error();
        }
        const VkResult result = ((PFN_vkCreateSwapchainKHR)raw)(
            (VkDevice)H(0), info, nullptr, out);
        if (result == VK_SUCCESS && info && out && *out) {
            // The pairing the presentation backend was always written to
            // receive: without it swapchainSurfaces stays empty, and every
            // acquire and present notification below returns at its first
            // lookup.
            if (KVulkanPtr backend = presentationBackend()) {
                backend->registerVulkanSwapchain((void*)*out,
                                                 (void*)info->surface);
            }
        }
        // What the guest asked for, beside the window it asked for it on. A
        // device run built one swapchain at 740x555 and a second at 804x585
        // on a window that was 804x585 throughout; this line and the extent
        // witness above are what say which of the two the host had reported.
        if (info) {
            SurfaceWindow window;
            const bool known = surfaceWindow((U64)info->surface, &window);
            static std::atomic<U32> reported{0};
            const U32 seen = reported.fetch_add(1, std::memory_order_relaxed);
            if (seen < 8 || (seen % 60) == 0 || result != VK_SUCCESS) {
                klog_fmt("BOXEDWINE_X64_VULKAN_SWAPCHAIN n=%u tid=%04X "
                         "surface=0x%llx window=0x%x window_size=%ux%u "
                         "requested=%ux%u images=%u mode=%d old=0x%llx "
                         "swapchain=0x%llx status=%d",
                         seen, tid, (unsigned long long)info->surface,
                         known ? window.windowId : 0, window.width,
                         window.height, info->imageExtent.width,
                         info->imageExtent.height, info->minImageCount,
                         (int)info->presentMode,
                         (unsigned long long)info->oldSwapchain,
                         (unsigned long long)(out ? *out : 0), (int)result);
            }
        }
        return (S64)result;
    }
    case VKB_DestroySwapchainKHR:
        if (A(1)) {
            if (KVulkanPtr backend = presentationBackend()) {
                backend->destroyVulkanSwapchain((void*)A(1));
            }
        }
        ((PFN_vkDestroySwapchainKHR)raw)((VkDevice)H(0), (VkSwapchainKHR)A(1),
                                         nullptr);
        return 0;
    case VKB_GetSwapchainImagesKHR: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        VkImage* images =
            (VkImage*)m.out(A(3), (U64)*countSlot * sizeof(VkImage));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetSwapchainImagesKHR)raw)(
            (VkDevice)H(0), (VkSwapchainKHR)A(1), countSlot, images);
    }
    case VKB_AcquireNextImageKHR: {
        uint32_t* imageIndex = (uint32_t*)m.outRequired(A(5), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result = ((PFN_vkAcquireNextImageKHR)raw)(
            (VkDevice)H(0), (VkSwapchainKHR)A(1), (uint64_t)A(2),
            (VkSemaphore)A(3), (VkFence)A(4), imageIndex);
        // Unbudgeted, and paired with the present witness, because an acquire
        // that succeeds and is never followed by a present is the shape of the
        // hang this lane is in. What it prints is exactly what a reader needs
        // to tell "the bridge lost the signal" from "the caller never came
        // back": which objects the acquire was asked to signal (either may be
        // null, and a null one must STAY null), the full 64-bit timeout, and
        // the image index that goes back to the guest. It also names the
        // calling thread, because the next question after "the acquire
        // succeeded and no present followed" is "did the thread that acquired
        // survive to present", and only a tid can answer that against the
        // thread-exit witness.
        static std::atomic<U32> reported{0};
        const U32 seen = reported.fetch_add(1, std::memory_order_relaxed);
        if (seen < 8 || (seen % 60) == 0) {
            klog_fmt("BOXEDWINE_X64_VULKAN_ACQUIRE n=%u tid=%04X "
                     "swapchain=0x%llx "
                     "timeout=0x%llx semaphore=0x%llx fence=0x%llx "
                     "index=%u status=%d",
                     seen, tid, (unsigned long long)A(1),
                     (unsigned long long)A(2), (unsigned long long)A(3),
                     (unsigned long long)A(4),
                     (result == VK_SUCCESS && imageIndex) ? *imageIndex : 0xffffffffu,
                     (int)result);
        }
        if (KVulkanPtr backend = presentationBackend()) {
            backend->acquireVulkanSwapchain(
                (void*)A(1), (int)result,
                (result == VK_SUCCESS && imageIndex) ? *imageIndex
                                                     : 0xffffffffu);
        }
        return (S64)result;
    }
    case VKB_AcquireNextImage2KHR: {
        const VkAcquireNextImageInfoKHR* info =
            (const VkAcquireNextImageInfoKHR*)m.chain(A(1), false);
        uint32_t* imageIndex = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result = ((PFN_vkAcquireNextImage2KHR)raw)(
            (VkDevice)H(0), info, imageIndex);
        // Same notification as the one-argument form, reading the swapchain
        // out of the shadow the driver was handed rather than an argument
        // slot. Both forms have to report, or "the guest never acquired" is a
        // claim about only one of the two ways of acquiring.
        if (info) {
            if (KVulkanPtr backend = presentationBackend()) {
                backend->acquireVulkanSwapchain(
                    (void*)info->swapchain, (int)result,
                    (result == VK_SUCCESS && imageIndex) ? *imageIndex
                                                         : 0xffffffffu);
            }
        }
        return (S64)result;
    }
    case VKB_QueuePresentKHR: {
        const VkPresentInfoKHR* info =
            (const VkPresentInfoKHR*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result =
            ((PFN_vkQueuePresentKHR)raw)((VkQueue)H(0), info);
        const U32 presents = gPresents.fetch_add(1, std::memory_order_relaxed);
        // One line per sixty presents, plus the first: enough to say the lane
        // is presenting and at what rate, cheap enough to leave on.
        if (presents < 3 || (presents % 60) == 0) {
            // The image index and wait semaphore are named for the same reason
            // the acquire names them: the pair of lines is what says whether
            // the image an acquire handed the guest is the image the guest
            // presented, and whether it waited on the semaphore the acquire
            // signalled. The thread is named for the same reason the acquire
            // names it.
            klog_fmt("BOXEDWINE_X64_VULKAN_PRESENT frame=%u tid=%04X "
                     "swapchains=%u "
                     "index=%u waits=%u status=%d",
                     presents, tid, info ? info->swapchainCount : 0,
                     (info && info->pImageIndices && info->swapchainCount)
                         ? info->pImageIndices[0] : 0xffffffffu,
                     info ? info->waitSemaphoreCount : 0, (int)result);
        }
        // One notification per swapchain the present names, because a present
        // may carry several and the backend keys its per-surface frame state
        // by swapchain. The array is the marshalled shadow, so it is safe to
        // walk it here.
        if (info && info->pSwapchains) {
            if (KVulkanPtr backend = presentationBackend()) {
                for (uint32_t i = 0; i < info->swapchainCount; ++i) {
                    // Per-swapchain results, when the caller asked for them,
                    // are what say which swapchain failed; without pResults
                    // every swapchain in the batch shares the call's result.
                    const int status = info->pResults ? (int)info->pResults[i]
                                                      : (int)result;
                    backend->presentVulkanSwapchain(
                        (void*)info->pSwapchains[i], status);
                }
            }
        }
        return (S64)result;
    }
    // ---- Command pools and command buffers ---------------------------------

    case VKB_CreateCommandPool: {
        const VkCommandPoolCreateInfo* info =
            (const VkCommandPoolCreateInfo*)m.chain(A(1), false);
        VkCommandPool* out =
            (VkCommandPool*)m.outRequired(A(3), sizeof(VkCommandPool));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateCommandPool)raw)((VkDevice)H(0), info,
                                                   nullptr, out);
    }
    case VKB_DestroyCommandPool:
        ((PFN_vkDestroyCommandPool)raw)((VkDevice)H(0), (VkCommandPool)A(1),
                                        nullptr);
        return 0;
    case VKB_ResetCommandPool:
        return (S64)((PFN_vkResetCommandPool)raw)(
            (VkDevice)H(0), (VkCommandPool)A(1),
            (VkCommandPoolResetFlags)U32A(2));
    case VKB_AllocateCommandBuffers: {
        const VkCommandBufferAllocateInfo* info =
            (const VkCommandBufferAllocateInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error();
        }
        // The output array's length lives inside the create info, which is why
        // it is sized only after the info has been copied.
        VkCommandBuffer* out = (VkCommandBuffer*)m.outRequired(
            A(2), (U64)info->commandBufferCount * sizeof(VkCommandBuffer));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkAllocateCommandBuffers)raw)((VkDevice)H(0), info,
                                                        out);
    }
    case VKB_FreeCommandBuffers: {
        const VkCommandBuffer* buffers = (const VkCommandBuffer*)m.inArrayAt(
            A(3), U32A(2), sizeof(VkCommandBuffer));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkFreeCommandBuffers)raw)((VkDevice)H(0), (VkCommandPool)A(1),
                                        U32A(2), buffers);
        return 0;
    }
    case VKB_BeginCommandBuffer: {
        const VkCommandBufferBeginInfo* info =
            (const VkCommandBufferBeginInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkBeginCommandBuffer)raw)((VkCommandBuffer)H(0),
                                                    info);
    }
    case VKB_EndCommandBuffer:
        return (S64)((PFN_vkEndCommandBuffer)raw)((VkCommandBuffer)H(0));
    case VKB_ResetCommandBuffer:
        return (S64)((PFN_vkResetCommandBuffer)raw)(
            (VkCommandBuffer)H(0), (VkCommandBufferResetFlags)U32A(1));
    case VKB_QueueSubmit2: {
        const VkSubmitInfo2* submits =
            m.structArrayTyped<VkSubmitInfo2>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        const VkResult result = ((PFN_vkQueueSubmit2)raw)(
            (VkQueue)H(0), U32A(1), submits, (VkFence)A(3));
        if (KVulkanPtr backend = presentationBackend()) {
            backend->submitVulkanWorkload((int)result, U32A(1),
                                          "vkQueueSubmit2");
        }
        return (S64)result;
    }

    // ---- Events and query pools --------------------------------------------

    case VKB_CreateEvent: {
        const VkEventCreateInfo* info =
            (const VkEventCreateInfo*)m.chain(A(1), false);
        VkEvent* out = (VkEvent*)m.outRequired(A(3), sizeof(VkEvent));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateEvent)raw)((VkDevice)H(0), info, nullptr,
                                             out);
    }
    case VKB_DestroyEvent:
        ((PFN_vkDestroyEvent)raw)((VkDevice)H(0), (VkEvent)A(1), nullptr);
        return 0;
    case VKB_GetEventStatus:
        return (S64)((PFN_vkGetEventStatus)raw)((VkDevice)H(0), (VkEvent)A(1));
    case VKB_SetEvent:
        return (S64)((PFN_vkSetEvent)raw)((VkDevice)H(0), (VkEvent)A(1));
    case VKB_ResetEvent:
        return (S64)((PFN_vkResetEvent)raw)((VkDevice)H(0), (VkEvent)A(1));
    case VKB_CreateQueryPool: {
        const VkQueryPoolCreateInfo* info =
            (const VkQueryPoolCreateInfo*)m.chain(A(1), false);
        VkQueryPool* out =
            (VkQueryPool*)m.outRequired(A(3), sizeof(VkQueryPool));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateQueryPool)raw)((VkDevice)H(0), info, nullptr,
                                                 out);
    }
    case VKB_DestroyQueryPool:
        ((PFN_vkDestroyQueryPool)raw)((VkDevice)H(0), (VkQueryPool)A(1),
                                      nullptr);
        return 0;
    case VKB_GetQueryPoolResults: {
        // dataSize is the caller's own byte count for pData; the driver writes
        // at most that many.
        void* data = m.outRequired(A(5), A(4));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetQueryPoolResults)raw)(
            (VkDevice)H(0), (VkQueryPool)A(1), U32A(2), U32A(3), (size_t)A(4),
            data, (VkDeviceSize)A(6), (VkQueryResultFlags)U32A(7));
    }
    case VKB_ResetQueryPool:
        ((PFN_vkResetQueryPool)raw)((VkDevice)H(0), (VkQueryPool)A(1), U32A(2),
                                    U32A(3));
        return 0;

    // ---- Buffer views and samplers -----------------------------------------

    case VKB_CreateBufferView: {
        const VkBufferViewCreateInfo* info =
            (const VkBufferViewCreateInfo*)m.chain(A(1), false);
        VkBufferView* out =
            (VkBufferView*)m.outRequired(A(3), sizeof(VkBufferView));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateBufferView)raw)((VkDevice)H(0), info, nullptr,
                                                  out);
    }
    case VKB_DestroyBufferView:
        ((PFN_vkDestroyBufferView)raw)((VkDevice)H(0), (VkBufferView)A(1),
                                       nullptr);
        return 0;
    case VKB_CreateSampler: {
        const VkSamplerCreateInfo* info =
            (const VkSamplerCreateInfo*)m.chain(A(1), false);
        VkSampler* out = (VkSampler*)m.outRequired(A(3), sizeof(VkSampler));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateSampler)raw)((VkDevice)H(0), info, nullptr,
                                               out);
    }
    case VKB_DestroySampler:
        ((PFN_vkDestroySampler)raw)((VkDevice)H(0), (VkSampler)A(1), nullptr);
        return 0;

    // ---- Shader modules and pipeline caches --------------------------------

    case VKB_CreateShaderModule: {
        const VkShaderModuleCreateInfo* info =
            (const VkShaderModuleCreateInfo*)m.chain(A(1), false);
        VkShaderModule* out =
            (VkShaderModule*)m.outRequired(A(3), sizeof(VkShaderModule));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateShaderModule)raw)((VkDevice)H(0), info,
                                                    nullptr, out);
    }
    case VKB_DestroyShaderModule:
        ((PFN_vkDestroyShaderModule)raw)((VkDevice)H(0), (VkShaderModule)A(1),
                                         nullptr);
        return 0;
    case VKB_CreatePipelineCache: {
        const VkPipelineCacheCreateInfo* info =
            (const VkPipelineCacheCreateInfo*)m.chain(A(1), false);
        VkPipelineCache* out =
            (VkPipelineCache*)m.outRequired(A(3), sizeof(VkPipelineCache));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreatePipelineCache)raw)((VkDevice)H(0), info,
                                                     nullptr, out);
    }
    case VKB_DestroyPipelineCache:
        ((PFN_vkDestroyPipelineCache)raw)((VkDevice)H(0), (VkPipelineCache)A(1),
                                          nullptr);
        return 0;
    case VKB_GetPipelineCacheData: {
        // The two-call idiom again, with a size_t rather than a uint32_t
        // capacity slot.
        size_t* sizeSlot = (size_t*)m.outRequired(A(2), sizeof(size_t));
        if (!m.ok()) {
            return m.error(false);
        }
        void* data = m.out(A(3), (U64)*sizeSlot);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkGetPipelineCacheData)raw)(
            (VkDevice)H(0), (VkPipelineCache)A(1), sizeSlot, data);
    }
    case VKB_MergePipelineCaches: {
        const VkPipelineCache* sources = (const VkPipelineCache*)m.inArrayAt(
            A(3), U32A(2), sizeof(VkPipelineCache));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkMergePipelineCaches)raw)(
            (VkDevice)H(0), (VkPipelineCache)A(1), U32A(2), sources);
    }

    // ---- Pipeline and descriptor set layouts -------------------------------

    case VKB_CreatePipelineLayout: {
        const VkPipelineLayoutCreateInfo* info =
            (const VkPipelineLayoutCreateInfo*)m.chain(A(1), false);
        VkPipelineLayout* out =
            (VkPipelineLayout*)m.outRequired(A(3), sizeof(VkPipelineLayout));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreatePipelineLayout)raw)((VkDevice)H(0), info,
                                                      nullptr, out);
    }
    case VKB_DestroyPipelineLayout:
        ((PFN_vkDestroyPipelineLayout)raw)((VkDevice)H(0),
                                           (VkPipelineLayout)A(1), nullptr);
        return 0;
    case VKB_CreateDescriptorSetLayout: {
        const VkDescriptorSetLayoutCreateInfo* info =
            (const VkDescriptorSetLayoutCreateInfo*)m.chain(A(1), false);
        VkDescriptorSetLayout* out = (VkDescriptorSetLayout*)m.outRequired(
            A(3), sizeof(VkDescriptorSetLayout));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateDescriptorSetLayout)raw)((VkDevice)H(0), info,
                                                           nullptr, out);
    }
    case VKB_DestroyDescriptorSetLayout:
        ((PFN_vkDestroyDescriptorSetLayout)raw)(
            (VkDevice)H(0), (VkDescriptorSetLayout)A(1), nullptr);
        return 0;
    case VKB_GetDescriptorSetLayoutSupport: {
        const VkDescriptorSetLayoutCreateInfo* info =
            (const VkDescriptorSetLayoutCreateInfo*)m.chain(A(1), false);
        VkDescriptorSetLayoutSupport* support =
            (VkDescriptorSetLayoutSupport*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDescriptorSetLayoutSupport)raw)((VkDevice)H(0), info,
                                                   support);
        return 0;
    }

    // ---- Descriptor pools, sets and updates --------------------------------

    case VKB_CreateDescriptorPool: {
        const VkDescriptorPoolCreateInfo* info =
            (const VkDescriptorPoolCreateInfo*)m.chain(A(1), false);
        VkDescriptorPool* out =
            (VkDescriptorPool*)m.outRequired(A(3), sizeof(VkDescriptorPool));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateDescriptorPool)raw)((VkDevice)H(0), info,
                                                      nullptr, out);
    }
    case VKB_DestroyDescriptorPool:
        ((PFN_vkDestroyDescriptorPool)raw)((VkDevice)H(0),
                                           (VkDescriptorPool)A(1), nullptr);
        return 0;
    case VKB_ResetDescriptorPool:
        return (S64)((PFN_vkResetDescriptorPool)raw)(
            (VkDevice)H(0), (VkDescriptorPool)A(1),
            (VkDescriptorPoolResetFlags)U32A(2));
    case VKB_AllocateDescriptorSets: {
        const VkDescriptorSetAllocateInfo* info =
            (const VkDescriptorSetAllocateInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error();
        }
        VkDescriptorSet* out = (VkDescriptorSet*)m.outRequired(
            A(2), (U64)info->descriptorSetCount * sizeof(VkDescriptorSet));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkAllocateDescriptorSets)raw)((VkDevice)H(0), info,
                                                        out);
    }
    case VKB_FreeDescriptorSets: {
        const VkDescriptorSet* sets = (const VkDescriptorSet*)m.inArrayAt(
            A(3), U32A(2), sizeof(VkDescriptorSet));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkFreeDescriptorSets)raw)(
            (VkDevice)H(0), (VkDescriptorPool)A(1), U32A(2), sets);
    }
    case VKB_UpdateDescriptorSets: {
        const VkWriteDescriptorSet* writes =
            m.structArrayTyped<VkWriteDescriptorSet>(A(2), U32A(1), false);
        const VkCopyDescriptorSet* copies =
            m.structArrayTyped<VkCopyDescriptorSet>(A(4), U32A(3), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkUpdateDescriptorSets)raw)((VkDevice)H(0), U32A(1), writes,
                                          U32A(3), copies);
        return 0;
    }
    case VKB_CreateDescriptorUpdateTemplate: {
        const VkDescriptorUpdateTemplateCreateInfo* info =
            (const VkDescriptorUpdateTemplateCreateInfo*)m.chain(A(1), false);
        VkDescriptorUpdateTemplate* out =
            (VkDescriptorUpdateTemplate*)m.outRequired(
                A(3), sizeof(VkDescriptorUpdateTemplate));
        if (!m.ok()) {
            return m.error();
        }
        const VkResult result =
            ((PFN_vkCreateDescriptorUpdateTemplate)raw)((VkDevice)H(0), info,
                                                        nullptr, out);
        if (result == VK_SUCCESS && out && *out) {
            // Measured from the marshalled entries, which are this marshal's
            // own memory and therefore safe to walk.
            bool sized = true;
            const U64 bytes = templateDataSpan(info, &sized);
            noteTemplateCreated(*out, bytes, sized);
            if (!sized) {
                klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE "
                         "call=vkCreateDescriptorUpdateTemplate "
                         "status=unsized-entries entries=%u",
                         info->descriptorUpdateEntryCount);
            }
        }
        return (S64)result;
    }
    case VKB_DestroyDescriptorUpdateTemplate: {
        VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)A(1);
        forgetTemplate(handle);
        ((PFN_vkDestroyDescriptorUpdateTemplate)raw)((VkDevice)H(0), handle,
                                                     nullptr);
        return 0;
    }
    case VKB_UpdateDescriptorSetWithTemplate: {
        VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)A(2);
        U64 bytes = 0;
        if (!templateDataBytes(handle, &bytes)) {
            // A template this bridge has no span for. The command returns
            // void, so the refusal is a log line and a skipped update rather
            // than an invented error -- but it is named, because a descriptor
            // set that silently kept its old contents is otherwise a rendering
            // bug with no trace.
            static std::atomic<U32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE "
                         "call=vkUpdateDescriptorSetWithTemplate "
                         "template=0x%llx status=no-span",
                         (unsigned long long)A(2));
            }
            return 0;
        }
        // Flat: every descriptor a template entry names is handles, offsets
        // and enums, so the span copies whole.
        const void* data = m.inArrayAt(A(3), bytes, 1);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkUpdateDescriptorSetWithTemplate)raw)(
            (VkDevice)H(0), (VkDescriptorSet)A(1), handle, data);
        return 0;
    }

    // ---- Render passes and framebuffers ------------------------------------

    case VKB_CreateRenderPass: {
        const VkRenderPassCreateInfo* info =
            (const VkRenderPassCreateInfo*)m.chain(A(1), false);
        VkRenderPass* out =
            (VkRenderPass*)m.outRequired(A(3), sizeof(VkRenderPass));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateRenderPass)raw)((VkDevice)H(0), info, nullptr,
                                                  out);
    }
    case VKB_CreateRenderPass2: {
        const VkRenderPassCreateInfo2* info =
            (const VkRenderPassCreateInfo2*)m.chain(A(1), false);
        VkRenderPass* out =
            (VkRenderPass*)m.outRequired(A(3), sizeof(VkRenderPass));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateRenderPass2)raw)((VkDevice)H(0), info,
                                                   nullptr, out);
    }
    case VKB_DestroyRenderPass:
        ((PFN_vkDestroyRenderPass)raw)((VkDevice)H(0), (VkRenderPass)A(1),
                                       nullptr);
        return 0;
    case VKB_CreateFramebuffer: {
        const VkFramebufferCreateInfo* info =
            (const VkFramebufferCreateInfo*)m.chain(A(1), false);
        VkFramebuffer* out =
            (VkFramebuffer*)m.outRequired(A(3), sizeof(VkFramebuffer));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateFramebuffer)raw)((VkDevice)H(0), info,
                                                   nullptr, out);
    }
    case VKB_DestroyFramebuffer:
        ((PFN_vkDestroyFramebuffer)raw)((VkDevice)H(0), (VkFramebuffer)A(1),
                                        nullptr);
        return 0;

    // ---- Pipelines ----------------------------------------------------------

    case VKB_CreateGraphicsPipelines: {
        const VkGraphicsPipelineCreateInfo* infos =
            m.structArrayTyped<VkGraphicsPipelineCreateInfo>(A(3), U32A(2),
                                                             false);
        VkPipeline* out = (VkPipeline*)m.outRequired(
            A(5), (U64)U32A(2) * sizeof(VkPipeline));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateGraphicsPipelines)raw)(
            (VkDevice)H(0), (VkPipelineCache)A(1), U32A(2), infos, nullptr,
            out);
    }
    case VKB_CreateComputePipelines: {
        const VkComputePipelineCreateInfo* infos =
            m.structArrayTyped<VkComputePipelineCreateInfo>(A(3), U32A(2),
                                                            false);
        VkPipeline* out = (VkPipeline*)m.outRequired(
            A(5), (U64)U32A(2) * sizeof(VkPipeline));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateComputePipelines)raw)(
            (VkDevice)H(0), (VkPipelineCache)A(1), U32A(2), infos, nullptr,
            out);
    }
    case VKB_DestroyPipeline:
        ((PFN_vkDestroyPipeline)raw)((VkDevice)H(0), (VkPipeline)A(1), nullptr);
        return 0;

    // ---- Recording: render pass and dynamic rendering scopes ---------------

    case VKB_CmdBeginRenderPass: {
        const VkRenderPassBeginInfo* info =
            (const VkRenderPassBeginInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBeginRenderPass)raw)((VkCommandBuffer)H(0), info,
                                        (VkSubpassContents)U32A(2));
        return 0;
    }
    case VKB_CmdNextSubpass:
        ((PFN_vkCmdNextSubpass)raw)((VkCommandBuffer)H(0),
                                    (VkSubpassContents)U32A(1));
        return 0;
    case VKB_CmdEndRenderPass:
        ((PFN_vkCmdEndRenderPass)raw)((VkCommandBuffer)H(0));
        return 0;
    case VKB_CmdBeginRenderPass2: {
        const VkRenderPassBeginInfo* info =
            (const VkRenderPassBeginInfo*)m.chain(A(1), false);
        const VkSubpassBeginInfo* begin =
            (const VkSubpassBeginInfo*)m.chain(A(2), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBeginRenderPass2)raw)((VkCommandBuffer)H(0), info, begin);
        return 0;
    }
    case VKB_CmdNextSubpass2: {
        const VkSubpassBeginInfo* begin =
            (const VkSubpassBeginInfo*)m.chain(A(1), false);
        const VkSubpassEndInfo* end =
            (const VkSubpassEndInfo*)m.chain(A(2), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdNextSubpass2)raw)((VkCommandBuffer)H(0), begin, end);
        return 0;
    }
    case VKB_CmdEndRenderPass2: {
        const VkSubpassEndInfo* end =
            (const VkSubpassEndInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdEndRenderPass2)raw)((VkCommandBuffer)H(0), end);
        return 0;
    }
    case VKB_CmdBeginRendering: {
        const VkRenderingInfo* info =
            (const VkRenderingInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBeginRendering)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdEndRendering:
        ((PFN_vkCmdEndRendering)raw)((VkCommandBuffer)H(0));
        return 0;

    // ---- Recording: binding -------------------------------------------------

    case VKB_CmdBindPipeline:
        ((PFN_vkCmdBindPipeline)raw)((VkCommandBuffer)H(0),
                                     (VkPipelineBindPoint)U32A(1),
                                     (VkPipeline)A(2));
        return 0;
    case VKB_CmdBindDescriptorSets: {
        const VkDescriptorSet* sets = (const VkDescriptorSet*)m.inArrayAt(
            A(5), U32A(4), sizeof(VkDescriptorSet));
        const uint32_t* offsets = (const uint32_t*)m.inArrayAt(
            A(7), U32A(6), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBindDescriptorSets)raw)(
            (VkCommandBuffer)H(0), (VkPipelineBindPoint)U32A(1),
            (VkPipelineLayout)A(2), U32A(3), U32A(4), sets, U32A(6), offsets);
        return 0;
    }
    case VKB_CmdBindIndexBuffer:
        ((PFN_vkCmdBindIndexBuffer)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                        (VkDeviceSize)A(2),
                                        (VkIndexType)U32A(3));
        return 0;
    case VKB_CmdBindVertexBuffers: {
        const VkBuffer* buffers =
            (const VkBuffer*)m.inArrayAt(A(3), U32A(2), sizeof(VkBuffer));
        const VkDeviceSize* offsets = (const VkDeviceSize*)m.inArrayAt(
            A(4), U32A(2), sizeof(VkDeviceSize));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBindVertexBuffers)raw)((VkCommandBuffer)H(0), U32A(1),
                                          U32A(2), buffers, offsets);
        return 0;
    }
    case VKB_CmdBindVertexBuffers2: {
        const VkBuffer* buffers =
            (const VkBuffer*)m.inArrayAt(A(3), U32A(2), sizeof(VkBuffer));
        const VkDeviceSize* offsets = (const VkDeviceSize*)m.inArrayAt(
            A(4), U32A(2), sizeof(VkDeviceSize));
        // pSizes and pStrides are optional even with a non-zero count.
        const VkDeviceSize* sizes =
            A(5) ? (const VkDeviceSize*)m.inArrayAt(A(5), U32A(2),
                                                    sizeof(VkDeviceSize))
                 : nullptr;
        const VkDeviceSize* strides =
            A(6) ? (const VkDeviceSize*)m.inArrayAt(A(6), U32A(2),
                                                    sizeof(VkDeviceSize))
                 : nullptr;
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBindVertexBuffers2)raw)((VkCommandBuffer)H(0), U32A(1),
                                           U32A(2), buffers, offsets, sizes,
                                           strides);
        return 0;
    }
    case VKB_CmdPushConstants: {
        // `size` bytes of opaque data, which is exactly what the driver reads.
        const void* values = m.inArrayAt(A(5), A(4), 1);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPushConstants)raw)((VkCommandBuffer)H(0),
                                      (VkPipelineLayout)A(1),
                                      (VkShaderStageFlags)U32A(2), U32A(3),
                                      U32A(4), values);
        return 0;
    }

    // ---- Recording: dynamic state ------------------------------------------

    case VKB_CmdSetViewport: {
        const VkViewport* viewports =
            (const VkViewport*)m.inArrayAt(A(3), U32A(2), sizeof(VkViewport));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetViewport)raw)((VkCommandBuffer)H(0), U32A(1), U32A(2),
                                    viewports);
        return 0;
    }
    case VKB_CmdSetScissor: {
        const VkRect2D* scissors =
            (const VkRect2D*)m.inArrayAt(A(3), U32A(2), sizeof(VkRect2D));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetScissor)raw)((VkCommandBuffer)H(0), U32A(1), U32A(2),
                                   scissors);
        return 0;
    }
    case VKB_CmdSetLineWidth:
        ((PFN_vkCmdSetLineWidth)raw)((VkCommandBuffer)H(0), F32A(1));
        return 0;
    case VKB_CmdSetDepthBias:
        ((PFN_vkCmdSetDepthBias)raw)((VkCommandBuffer)H(0), F32A(1), F32A(2),
                                     F32A(3));
        return 0;
    case VKB_CmdSetBlendConstants: {
        // A four-float array, not four float parameters.
        const float* constants =
            (const float*)m.inArrayAt(A(1), 4, sizeof(float));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetBlendConstants)raw)((VkCommandBuffer)H(0), constants);
        return 0;
    }
    case VKB_CmdSetDepthBounds:
        ((PFN_vkCmdSetDepthBounds)raw)((VkCommandBuffer)H(0), F32A(1), F32A(2));
        return 0;
    case VKB_CmdSetStencilCompareMask:
        ((PFN_vkCmdSetStencilCompareMask)raw)(
            (VkCommandBuffer)H(0), (VkStencilFaceFlags)U32A(1), U32A(2));
        return 0;
    case VKB_CmdSetStencilWriteMask:
        ((PFN_vkCmdSetStencilWriteMask)raw)(
            (VkCommandBuffer)H(0), (VkStencilFaceFlags)U32A(1), U32A(2));
        return 0;
    case VKB_CmdSetStencilReference:
        ((PFN_vkCmdSetStencilReference)raw)(
            (VkCommandBuffer)H(0), (VkStencilFaceFlags)U32A(1), U32A(2));
        return 0;
    case VKB_CmdSetViewportWithCount: {
        const VkViewport* viewports =
            (const VkViewport*)m.inArrayAt(A(2), U32A(1), sizeof(VkViewport));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetViewportWithCount)raw)((VkCommandBuffer)H(0), U32A(1),
                                             viewports);
        return 0;
    }
    case VKB_CmdSetScissorWithCount: {
        const VkRect2D* scissors =
            (const VkRect2D*)m.inArrayAt(A(2), U32A(1), sizeof(VkRect2D));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetScissorWithCount)raw)((VkCommandBuffer)H(0), U32A(1),
                                            scissors);
        return 0;
    }
    case VKB_CmdSetCullMode:
        ((PFN_vkCmdSetCullMode)raw)((VkCommandBuffer)H(0),
                                    (VkCullModeFlags)U32A(1));
        return 0;
    case VKB_CmdSetFrontFace:
        ((PFN_vkCmdSetFrontFace)raw)((VkCommandBuffer)H(0),
                                     (VkFrontFace)U32A(1));
        return 0;
    case VKB_CmdSetPrimitiveTopology:
        ((PFN_vkCmdSetPrimitiveTopology)raw)((VkCommandBuffer)H(0),
                                             (VkPrimitiveTopology)U32A(1));
        return 0;
    case VKB_CmdSetDepthTestEnable:
        ((PFN_vkCmdSetDepthTestEnable)raw)((VkCommandBuffer)H(0),
                                           (VkBool32)U32A(1));
        return 0;
    case VKB_CmdSetDepthWriteEnable:
        ((PFN_vkCmdSetDepthWriteEnable)raw)((VkCommandBuffer)H(0),
                                            (VkBool32)U32A(1));
        return 0;
    case VKB_CmdSetDepthCompareOp:
        ((PFN_vkCmdSetDepthCompareOp)raw)((VkCommandBuffer)H(0),
                                          (VkCompareOp)U32A(1));
        return 0;
    case VKB_CmdSetDepthBoundsTestEnable:
        ((PFN_vkCmdSetDepthBoundsTestEnable)raw)((VkCommandBuffer)H(0),
                                                 (VkBool32)U32A(1));
        return 0;
    case VKB_CmdSetStencilTestEnable:
        ((PFN_vkCmdSetStencilTestEnable)raw)((VkCommandBuffer)H(0),
                                             (VkBool32)U32A(1));
        return 0;
    case VKB_CmdSetStencilOp:
        ((PFN_vkCmdSetStencilOp)raw)(
            (VkCommandBuffer)H(0), (VkStencilFaceFlags)U32A(1),
            (VkStencilOp)U32A(2), (VkStencilOp)U32A(3), (VkStencilOp)U32A(4),
            (VkCompareOp)U32A(5));
        return 0;
    case VKB_CmdSetRasterizerDiscardEnable:
        ((PFN_vkCmdSetRasterizerDiscardEnable)raw)((VkCommandBuffer)H(0),
                                                   (VkBool32)U32A(1));
        return 0;
    case VKB_CmdSetDepthBiasEnable:
        ((PFN_vkCmdSetDepthBiasEnable)raw)((VkCommandBuffer)H(0),
                                           (VkBool32)U32A(1));
        return 0;
    case VKB_CmdSetPrimitiveRestartEnable:
        ((PFN_vkCmdSetPrimitiveRestartEnable)raw)((VkCommandBuffer)H(0),
                                                  (VkBool32)U32A(1));
        return 0;

    // ---- Recording: draw and dispatch --------------------------------------

    case VKB_CmdDraw:
        ((PFN_vkCmdDraw)raw)((VkCommandBuffer)H(0), U32A(1), U32A(2), U32A(3),
                             U32A(4));
        return 0;
    case VKB_CmdDrawIndexed:
        ((PFN_vkCmdDrawIndexed)raw)((VkCommandBuffer)H(0), U32A(1), U32A(2),
                                    U32A(3), (int32_t)U32A(4), U32A(5));
        return 0;
    case VKB_CmdDrawIndirect:
        ((PFN_vkCmdDrawIndirect)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                     (VkDeviceSize)A(2), U32A(3), U32A(4));
        return 0;
    case VKB_CmdDrawIndexedIndirect:
        ((PFN_vkCmdDrawIndexedIndirect)raw)((VkCommandBuffer)H(0),
                                            (VkBuffer)A(1), (VkDeviceSize)A(2),
                                            U32A(3), U32A(4));
        return 0;
    case VKB_CmdDispatch:
        ((PFN_vkCmdDispatch)raw)((VkCommandBuffer)H(0), U32A(1), U32A(2),
                                 U32A(3));
        return 0;
    case VKB_CmdDispatchIndirect:
        ((PFN_vkCmdDispatchIndirect)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                         (VkDeviceSize)A(2));
        return 0;
    case VKB_CmdDispatchBase:
        ((PFN_vkCmdDispatchBase)raw)((VkCommandBuffer)H(0), U32A(1), U32A(2),
                                     U32A(3), U32A(4), U32A(5), U32A(6));
        return 0;

    // ---- Recording: transfer -----------------------------------------------

    case VKB_CmdCopyBuffer: {
        const VkBufferCopy* regions = (const VkBufferCopy*)m.inArrayAt(
            A(4), U32A(3), sizeof(VkBufferCopy));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyBuffer)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                   (VkBuffer)A(2), U32A(3), regions);
        return 0;
    }
    case VKB_CmdCopyImage: {
        const VkImageCopy* regions =
            (const VkImageCopy*)m.inArrayAt(A(6), U32A(5), sizeof(VkImageCopy));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyImage)raw)((VkCommandBuffer)H(0), (VkImage)A(1),
                                  (VkImageLayout)U32A(2), (VkImage)A(3),
                                  (VkImageLayout)U32A(4), U32A(5), regions);
        return 0;
    }
    case VKB_CmdBlitImage: {
        const VkImageBlit* regions =
            (const VkImageBlit*)m.inArrayAt(A(6), U32A(5), sizeof(VkImageBlit));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBlitImage)raw)((VkCommandBuffer)H(0), (VkImage)A(1),
                                  (VkImageLayout)U32A(2), (VkImage)A(3),
                                  (VkImageLayout)U32A(4), U32A(5), regions,
                                  (VkFilter)U32A(7));
        return 0;
    }
    case VKB_CmdCopyBufferToImage: {
        const VkBufferImageCopy* regions =
            (const VkBufferImageCopy*)m.inArrayAt(A(5), U32A(4),
                                                  sizeof(VkBufferImageCopy));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyBufferToImage)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                          (VkImage)A(2),
                                          (VkImageLayout)U32A(3), U32A(4),
                                          regions);
        return 0;
    }
    case VKB_CmdCopyImageToBuffer: {
        const VkBufferImageCopy* regions =
            (const VkBufferImageCopy*)m.inArrayAt(A(5), U32A(4),
                                                  sizeof(VkBufferImageCopy));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyImageToBuffer)raw)((VkCommandBuffer)H(0), (VkImage)A(1),
                                          (VkImageLayout)U32A(2),
                                          (VkBuffer)A(3), U32A(4), regions);
        return 0;
    }
    case VKB_CmdUpdateBuffer: {
        // dataSize bytes of opaque data inlined into the command buffer.
        const void* data = m.inArrayAt(A(4), A(3), 1);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdUpdateBuffer)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                     (VkDeviceSize)A(2), (VkDeviceSize)A(3),
                                     data);
        return 0;
    }
    case VKB_CmdFillBuffer:
        ((PFN_vkCmdFillBuffer)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                   (VkDeviceSize)A(2), (VkDeviceSize)A(3),
                                   U32A(4));
        return 0;
    case VKB_CmdResolveImage: {
        const VkImageResolve* regions = (const VkImageResolve*)m.inArrayAt(
            A(6), U32A(5), sizeof(VkImageResolve));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdResolveImage)raw)((VkCommandBuffer)H(0), (VkImage)A(1),
                                     (VkImageLayout)U32A(2), (VkImage)A(3),
                                     (VkImageLayout)U32A(4), U32A(5), regions);
        return 0;
    }
    case VKB_CmdCopyBuffer2: {
        const VkCopyBufferInfo2* info =
            (const VkCopyBufferInfo2*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyBuffer2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdCopyImage2: {
        const VkCopyImageInfo2* info =
            (const VkCopyImageInfo2*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyImage2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdBlitImage2: {
        const VkBlitImageInfo2* info =
            (const VkBlitImageInfo2*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBlitImage2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdCopyBufferToImage2: {
        const VkCopyBufferToImageInfo2* info =
            (const VkCopyBufferToImageInfo2*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyBufferToImage2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdCopyImageToBuffer2: {
        const VkCopyImageToBufferInfo2* info =
            (const VkCopyImageToBufferInfo2*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdCopyImageToBuffer2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdResolveImage2: {
        const VkResolveImageInfo2* info =
            (const VkResolveImageInfo2*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdResolveImage2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }

    // ---- Recording: clears --------------------------------------------------

    case VKB_CmdClearColorImage: {
        const VkClearColorValue* colour =
            (const VkClearColorValue*)m.inRequired(A(3),
                                                   sizeof(VkClearColorValue));
        const VkImageSubresourceRange* ranges =
            (const VkImageSubresourceRange*)m.inArrayAt(
                A(5), U32A(4), sizeof(VkImageSubresourceRange));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdClearColorImage)raw)((VkCommandBuffer)H(0), (VkImage)A(1),
                                        (VkImageLayout)U32A(2), colour,
                                        U32A(4), ranges);
        return 0;
    }
    case VKB_CmdClearDepthStencilImage: {
        const VkClearDepthStencilValue* value =
            (const VkClearDepthStencilValue*)m.inRequired(
                A(3), sizeof(VkClearDepthStencilValue));
        const VkImageSubresourceRange* ranges =
            (const VkImageSubresourceRange*)m.inArrayAt(
                A(5), U32A(4), sizeof(VkImageSubresourceRange));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdClearDepthStencilImage)raw)(
            (VkCommandBuffer)H(0), (VkImage)A(1), (VkImageLayout)U32A(2), value,
            U32A(4), ranges);
        return 0;
    }
    case VKB_CmdClearAttachments: {
        const VkClearAttachment* attachments =
            (const VkClearAttachment*)m.inArrayAt(A(2), U32A(1),
                                                  sizeof(VkClearAttachment));
        const VkClearRect* rects =
            (const VkClearRect*)m.inArrayAt(A(4), U32A(3), sizeof(VkClearRect));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdClearAttachments)raw)((VkCommandBuffer)H(0), U32A(1),
                                         attachments, U32A(3), rects);
        return 0;
    }

    // ---- Recording: synchronisation ----------------------------------------

    case VKB_CmdPipelineBarrier: {
        const VkMemoryBarrier* memory =
            m.structArrayTyped<VkMemoryBarrier>(A(5), U32A(4), false);
        const VkBufferMemoryBarrier* buffers =
            m.structArrayTyped<VkBufferMemoryBarrier>(A(7), U32A(6), false);
        const VkImageMemoryBarrier* images =
            m.structArrayTyped<VkImageMemoryBarrier>(A(9), U32A(8), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPipelineBarrier)raw)(
            (VkCommandBuffer)H(0), (VkPipelineStageFlags)U32A(1),
            (VkPipelineStageFlags)U32A(2), (VkDependencyFlags)U32A(3), U32A(4),
            memory, U32A(6), buffers, U32A(8), images);
        return 0;
    }
    case VKB_CmdPipelineBarrier2: {
        const VkDependencyInfo* info =
            (const VkDependencyInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPipelineBarrier2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdSetEvent:
        ((PFN_vkCmdSetEvent)raw)((VkCommandBuffer)H(0), (VkEvent)A(1),
                                 (VkPipelineStageFlags)U32A(2));
        return 0;
    case VKB_CmdResetEvent:
        ((PFN_vkCmdResetEvent)raw)((VkCommandBuffer)H(0), (VkEvent)A(1),
                                   (VkPipelineStageFlags)U32A(2));
        return 0;
    case VKB_CmdWaitEvents: {
        const VkEvent* events =
            (const VkEvent*)m.inArrayAt(A(2), U32A(1), sizeof(VkEvent));
        const VkMemoryBarrier* memory =
            m.structArrayTyped<VkMemoryBarrier>(A(6), U32A(5), false);
        const VkBufferMemoryBarrier* buffers =
            m.structArrayTyped<VkBufferMemoryBarrier>(A(8), U32A(7), false);
        const VkImageMemoryBarrier* images =
            m.structArrayTyped<VkImageMemoryBarrier>(A(10), U32A(9), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdWaitEvents)raw)(
            (VkCommandBuffer)H(0), U32A(1), events,
            (VkPipelineStageFlags)U32A(3), (VkPipelineStageFlags)U32A(4),
            U32A(5), memory, U32A(7), buffers, U32A(9), images);
        return 0;
    }
    case VKB_CmdSetEvent2: {
        const VkDependencyInfo* info =
            (const VkDependencyInfo*)m.chain(A(2), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetEvent2)raw)((VkCommandBuffer)H(0), (VkEvent)A(1), info);
        return 0;
    }
    case VKB_CmdResetEvent2:
        // VkPipelineStageFlags2 is 64 bits wide, so the whole argument word
        // is the mask rather than its low half.
        ((PFN_vkCmdResetEvent2)raw)((VkCommandBuffer)H(0), (VkEvent)A(1),
                                    (VkPipelineStageFlags2)A(2));
        return 0;
    case VKB_CmdWaitEvents2: {
        const VkEvent* events =
            (const VkEvent*)m.inArrayAt(A(2), U32A(1), sizeof(VkEvent));
        const VkDependencyInfo* infos =
            m.structArrayTyped<VkDependencyInfo>(A(3), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdWaitEvents2)raw)((VkCommandBuffer)H(0), U32A(1), events,
                                    infos);
        return 0;
    }

    // ---- Recording: queries and secondary command buffers ------------------

    case VKB_CmdBeginQuery:
        ((PFN_vkCmdBeginQuery)raw)((VkCommandBuffer)H(0), (VkQueryPool)A(1),
                                   U32A(2), (VkQueryControlFlags)U32A(3));
        return 0;
    case VKB_CmdEndQuery:
        ((PFN_vkCmdEndQuery)raw)((VkCommandBuffer)H(0), (VkQueryPool)A(1),
                                 U32A(2));
        return 0;
    case VKB_CmdResetQueryPool:
        ((PFN_vkCmdResetQueryPool)raw)((VkCommandBuffer)H(0),
                                       (VkQueryPool)A(1), U32A(2), U32A(3));
        return 0;
    case VKB_CmdWriteTimestamp:
        ((PFN_vkCmdWriteTimestamp)raw)((VkCommandBuffer)H(0),
                                       (VkPipelineStageFlagBits)U32A(1),
                                       (VkQueryPool)A(2), U32A(3));
        return 0;
    case VKB_CmdWriteTimestamp2:
        ((PFN_vkCmdWriteTimestamp2)raw)((VkCommandBuffer)H(0),
                                        (VkPipelineStageFlags2)A(1),
                                        (VkQueryPool)A(2), U32A(3));
        return 0;
    case VKB_CmdCopyQueryPoolResults:
        ((PFN_vkCmdCopyQueryPoolResults)raw)(
            (VkCommandBuffer)H(0), (VkQueryPool)A(1), U32A(2), U32A(3),
            (VkBuffer)A(4), (VkDeviceSize)A(5), (VkDeviceSize)A(6),
            (VkQueryResultFlags)U32A(7));
        return 0;
    case VKB_CmdExecuteCommands: {
        const VkCommandBuffer* buffers = (const VkCommandBuffer*)m.inArrayAt(
            A(2), U32A(1), sizeof(VkCommandBuffer));
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdExecuteCommands)raw)((VkCommandBuffer)H(0), U32A(1),
                                        buffers);
        return 0;
    }

    // ---- Core commands the Vulkan 1.4 audit added --------------------------
    //
    // These are here because they are CORE, not because the D3D9 path calls
    // them. winevulkan fills a dispatch-table slot for every core command by
    // name; a name this bridge cannot resolve leaves that slot null, and a
    // caller that does not check calls through the hole. A device run ended
    // exactly that way: a 32-bit indirect call through a mapped table whose
    // entry was zero.

    case VKB_GetDeviceMemoryCommitment: {
        VkDeviceSize* committed =
            (VkDeviceSize*)m.outRequired(A(2), sizeof(VkDeviceSize));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceMemoryCommitment)raw)((VkDevice)H(0),
                                               (VkDeviceMemory)A(1), committed);
        return 0;
    }
    case VKB_GetImageSparseMemoryRequirements: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkSparseImageMemoryRequirements* requirements =
            (VkSparseImageMemoryRequirements*)m.out(
                A(3),
                (U64)*countSlot * sizeof(VkSparseImageMemoryRequirements));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetImageSparseMemoryRequirements)raw)(
            (VkDevice)H(0), (VkImage)A(1), countSlot, requirements);
        return 0;
    }
    case VKB_GetImageSparseMemoryRequirements2: {
        const VkImageSparseMemoryRequirementsInfo2* info =
            (const VkImageSparseMemoryRequirementsInfo2*)m.chain(A(1), false);
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkSparseImageMemoryRequirements2* requirements =
            m.structArrayTyped<VkSparseImageMemoryRequirements2>(
                A(3), *countSlot, true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetImageSparseMemoryRequirements2)raw)((VkDevice)H(0), info,
                                                       countSlot, requirements);
        return 0;
    }
    case VKB_GetPhysicalDeviceSparseImageFormatProperties: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(6), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkSparseImageFormatProperties* properties =
            (VkSparseImageFormatProperties*)m.out(
                A(7), (U64)*countSlot * sizeof(VkSparseImageFormatProperties));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceSparseImageFormatProperties)raw)(
            (VkPhysicalDevice)H(0), (VkFormat)U32A(1), (VkImageType)U32A(2),
            (VkSampleCountFlagBits)U32A(3), (VkImageUsageFlags)U32A(4),
            (VkImageTiling)U32A(5), countSlot, properties);
        return 0;
    }
    case VKB_GetPhysicalDeviceSparseImageFormatProperties2: {
        const VkPhysicalDeviceSparseImageFormatInfo2* info =
            (const VkPhysicalDeviceSparseImageFormatInfo2*)m.chain(A(1), false);
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkSparseImageFormatProperties2* properties =
            m.structArrayTyped<VkSparseImageFormatProperties2>(A(3), *countSlot,
                                                               true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPhysicalDeviceSparseImageFormatProperties2)raw)(
            (VkPhysicalDevice)H(0), info, countSlot, properties);
        return 0;
    }
    case VKB_QueueBindSparse: {
        const VkBindSparseInfo* infos =
            m.structArrayTyped<VkBindSparseInfo>(A(2), U32A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkQueueBindSparse)raw)((VkQueue)H(0), U32A(1), infos,
                                                 (VkFence)A(3));
    }
    case VKB_GetRenderAreaGranularity: {
        VkExtent2D* granularity =
            (VkExtent2D*)m.outRequired(A(2), sizeof(VkExtent2D));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetRenderAreaGranularity)raw)((VkDevice)H(0),
                                              (VkRenderPass)A(1), granularity);
        return 0;
    }
    case VKB_GetRenderingAreaGranularity: {
        const VkRenderingAreaInfo* info =
            (const VkRenderingAreaInfo*)m.chain(A(1), false);
        VkExtent2D* granularity =
            (VkExtent2D*)m.outRequired(A(2), sizeof(VkExtent2D));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetRenderingAreaGranularity)raw)((VkDevice)H(0), info,
                                                 granularity);
        return 0;
    }
    case VKB_TrimCommandPool:
        ((PFN_vkTrimCommandPool)raw)((VkDevice)H(0), (VkCommandPool)A(1),
                                     (VkCommandPoolTrimFlags)U32A(2));
        return 0;
    case VKB_CmdSetDeviceMask:
        ((PFN_vkCmdSetDeviceMask)raw)((VkCommandBuffer)H(0), U32A(1));
        return 0;
    case VKB_EnumeratePhysicalDeviceGroups: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkPhysicalDeviceGroupProperties* properties =
            m.structArrayTyped<VkPhysicalDeviceGroupProperties>(A(2),
                                                                *countSlot,
                                                                true);
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumeratePhysicalDeviceGroups)raw)(
            (VkInstance)H(0), countSlot, properties);
    }
    case VKB_GetDeviceGroupPeerMemoryFeatures: {
        VkPeerMemoryFeatureFlags* features =
            (VkPeerMemoryFeatureFlags*)m.outRequired(
                A(4), sizeof(VkPeerMemoryFeatureFlags));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceGroupPeerMemoryFeatures)raw)(
            (VkDevice)H(0), U32A(1), U32A(2), U32A(3), features);
        return 0;
    }
    case VKB_CreateSamplerYcbcrConversion: {
        const VkSamplerYcbcrConversionCreateInfo* info =
            (const VkSamplerYcbcrConversionCreateInfo*)m.chain(A(1), false);
        VkSamplerYcbcrConversion* out = (VkSamplerYcbcrConversion*)m.outRequired(
            A(3), sizeof(VkSamplerYcbcrConversion));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreateSamplerYcbcrConversion)raw)(
            (VkDevice)H(0), info, nullptr, out);
    }
    case VKB_DestroySamplerYcbcrConversion:
        ((PFN_vkDestroySamplerYcbcrConversion)raw)(
            (VkDevice)H(0), (VkSamplerYcbcrConversion)A(1), nullptr);
        return 0;
    case VKB_GetPhysicalDeviceToolProperties: {
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(1), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkPhysicalDeviceToolProperties* properties =
            m.structArrayTyped<VkPhysicalDeviceToolProperties>(A(2), *countSlot,
                                                               true);
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkGetPhysicalDeviceToolProperties)raw)(
            (VkPhysicalDevice)H(0), countSlot, properties);
    }
    case VKB_CmdDrawIndirectCount:
        ((PFN_vkCmdDrawIndirectCount)raw)(
            (VkCommandBuffer)H(0), (VkBuffer)A(1), (VkDeviceSize)A(2),
            (VkBuffer)A(3), (VkDeviceSize)A(4), U32A(5), U32A(6));
        return 0;
    case VKB_CmdDrawIndexedIndirectCount:
        ((PFN_vkCmdDrawIndexedIndirectCount)raw)(
            (VkCommandBuffer)H(0), (VkBuffer)A(1), (VkDeviceSize)A(2),
            (VkBuffer)A(3), (VkDeviceSize)A(4), U32A(5), U32A(6));
        return 0;

    // The three commands that return a 64-bit value rather than a VkResult.
    // A bridge result is a 64-bit word whose top half is how a refusal is
    // told apart from an answer, so the address cannot be the return value:
    // a legitimate device address with the high bits set would read as a
    // refusal. The guest shim therefore passes the address of a uint64_t on
    // its own stack as one extra argument word, and the answer is written
    // there. See BW_A in tools/vulkan-64/vulkan.c.
    case VKB_GetBufferDeviceAddress: {
        const VkBufferDeviceAddressInfo* info =
            (const VkBufferDeviceAddressInfo*)m.chain(A(1), false);
        uint64_t* out = (uint64_t*)m.outRequired(A(2), sizeof(uint64_t));
        if (!m.ok()) {
            return m.error(false);
        }
        *out = (uint64_t)((PFN_vkGetBufferDeviceAddress)raw)((VkDevice)H(0),
                                                             info);
        return 0;
    }
    case VKB_GetBufferOpaqueCaptureAddress: {
        const VkBufferDeviceAddressInfo* info =
            (const VkBufferDeviceAddressInfo*)m.chain(A(1), false);
        uint64_t* out = (uint64_t*)m.outRequired(A(2), sizeof(uint64_t));
        if (!m.ok()) {
            return m.error(false);
        }
        *out = ((PFN_vkGetBufferOpaqueCaptureAddress)raw)((VkDevice)H(0), info);
        return 0;
    }
    case VKB_GetDeviceMemoryOpaqueCaptureAddress: {
        const VkDeviceMemoryOpaqueCaptureAddressInfo* info =
            (const VkDeviceMemoryOpaqueCaptureAddressInfo*)m.chain(A(1), false);
        uint64_t* out = (uint64_t*)m.outRequired(A(2), sizeof(uint64_t));
        if (!m.ok()) {
            return m.error(false);
        }
        *out = ((PFN_vkGetDeviceMemoryOpaqueCaptureAddress)raw)((VkDevice)H(0),
                                                                info);
        return 0;
    }

    case VKB_CreatePrivateDataSlot: {
        const VkPrivateDataSlotCreateInfo* info =
            (const VkPrivateDataSlotCreateInfo*)m.chain(A(1), false);
        VkPrivateDataSlot* out =
            (VkPrivateDataSlot*)m.outRequired(A(3), sizeof(VkPrivateDataSlot));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkCreatePrivateDataSlot)raw)((VkDevice)H(0), info,
                                                       nullptr, out);
    }
    case VKB_DestroyPrivateDataSlot:
        ((PFN_vkDestroyPrivateDataSlot)raw)((VkDevice)H(0),
                                            (VkPrivateDataSlot)A(1), nullptr);
        return 0;
    case VKB_SetPrivateData:
        return (S64)((PFN_vkSetPrivateData)raw)(
            (VkDevice)H(0), (VkObjectType)U32A(1), (uint64_t)A(2),
            (VkPrivateDataSlot)A(3), (uint64_t)A(4));
    case VKB_GetPrivateData: {
        uint64_t* data = (uint64_t*)m.outRequired(A(4), sizeof(uint64_t));
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetPrivateData)raw)((VkDevice)H(0), (VkObjectType)U32A(1),
                                    (uint64_t)A(2), (VkPrivateDataSlot)A(3),
                                    data);
        return 0;
    }
    case VKB_GetDeviceBufferMemoryRequirements: {
        const VkDeviceBufferMemoryRequirements* info =
            (const VkDeviceBufferMemoryRequirements*)m.chain(A(1), false);
        VkMemoryRequirements2* requirements =
            (VkMemoryRequirements2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceBufferMemoryRequirements)raw)((VkDevice)H(0), info,
                                                       requirements);
        return 0;
    }
    case VKB_GetDeviceImageMemoryRequirements: {
        const VkDeviceImageMemoryRequirements* info =
            (const VkDeviceImageMemoryRequirements*)m.chain(A(1), false);
        VkMemoryRequirements2* requirements =
            (VkMemoryRequirements2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceImageMemoryRequirements)raw)((VkDevice)H(0), info,
                                                      requirements);
        return 0;
    }
    case VKB_GetDeviceImageSparseMemoryRequirements: {
        const VkDeviceImageMemoryRequirements* info =
            (const VkDeviceImageMemoryRequirements*)m.chain(A(1), false);
        uint32_t* countSlot = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error();
        }
        VkSparseImageMemoryRequirements2* requirements =
            m.structArrayTyped<VkSparseImageMemoryRequirements2>(
                A(3), *countSlot, true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceImageSparseMemoryRequirements)raw)(
            (VkDevice)H(0), info, countSlot, requirements);
        return 0;
    }
    case VKB_GetImageSubresourceLayout2: {
        const VkImageSubresource2* subresource =
            (const VkImageSubresource2*)m.chain(A(2), false);
        VkSubresourceLayout2* layout =
            (VkSubresourceLayout2*)m.chain(A(3), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetImageSubresourceLayout2)raw)((VkDevice)H(0), (VkImage)A(1),
                                                subresource, layout);
        return 0;
    }
    case VKB_GetDeviceImageSubresourceLayout: {
        const VkDeviceImageSubresourceInfo* info =
            (const VkDeviceImageSubresourceInfo*)m.chain(A(1), false);
        VkSubresourceLayout2* layout =
            (VkSubresourceLayout2*)m.chain(A(2), true);
        if (!m.ok()) {
            return m.error();
        }
        ((PFN_vkGetDeviceImageSubresourceLayout)raw)((VkDevice)H(0), info,
                                                     layout);
        return 0;
    }
    case VKB_MapMemory2: {
        const VkMemoryMapInfo* info =
            (const VkMemoryMapInfo*)m.chain(A(1), false);
        U64* slot = (U64*)m.outRequired(A(2), sizeof(U64));
        if (!m.ok()) {
            return m.error(false);
        }
        void* mapped = nullptr;
        const VkResult result =
            ((PFN_vkMapMemory2)raw)((VkDevice)H(0), info, &mapped);
        if (result == VK_SUCCESS) {
            // Same rule as vkMapMemory: the driver hands back a host address,
            // and an address inside an alias window is a guest page that the
            // guest can only hold in its canonical form.
            const U64 handed =
                boxedvn::hostToGuestAddress((U64)(uintptr_t)mapped);
            *slot = handed;
            // The same witness vkMapMemory prints, and for the same reason: a
            // run that maps no memory has not begun to upload anything, and
            // that is a fact worth being able to read off a log directly.
            static std::atomic<U32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                klog_fmt("BOXEDWINE_X64_VULKAN_MAP call=vkMapMemory2 "
                         "host=0x%llx guest=0x%llx low4g=%d",
                         (unsigned long long)(uintptr_t)mapped,
                         (unsigned long long)handed,
                         (handed >> 32) == 0 ? 1 : 0);
            }
        }
        return (S64)result;
    }
    case VKB_UnmapMemory2: {
        const VkMemoryUnmapInfo* info =
            (const VkMemoryUnmapInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkUnmapMemory2)raw)((VkDevice)H(0), info);
    }
    case VKB_TransitionImageLayout: {
        const VkHostImageLayoutTransitionInfo* transitions =
            m.structArrayTyped<VkHostImageLayoutTransitionInfo>(A(2), U32A(1),
                                                                false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkTransitionImageLayout)raw)((VkDevice)H(0), U32A(1),
                                                       transitions);
    }
    case VKB_CmdBindIndexBuffer2:
        ((PFN_vkCmdBindIndexBuffer2)raw)((VkCommandBuffer)H(0), (VkBuffer)A(1),
                                         (VkDeviceSize)A(2), (VkDeviceSize)A(3),
                                         (VkIndexType)U32A(4));
        return 0;
    case VKB_CmdSetLineStipple:
        ((PFN_vkCmdSetLineStipple)raw)((VkCommandBuffer)H(0), U32A(1),
                                       (uint16_t)A(2));
        return 0;
    case VKB_CmdPushDescriptorSet: {
        const VkWriteDescriptorSet* writes =
            m.structArrayTyped<VkWriteDescriptorSet>(A(5), U32A(4), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPushDescriptorSet)raw)(
            (VkCommandBuffer)H(0), (VkPipelineBindPoint)U32A(1),
            (VkPipelineLayout)A(2), U32A(3), U32A(4), writes);
        return 0;
    }
    case VKB_CmdPushDescriptorSetWithTemplate: {
        VkDescriptorUpdateTemplate handle = (VkDescriptorUpdateTemplate)A(1);
        U64 bytes = 0;
        if (!templateDataBytes(handle, &bytes)) {
            static std::atomic<U32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE "
                         "call=vkCmdPushDescriptorSetWithTemplate "
                         "template=0x%llx status=no-span",
                         (unsigned long long)A(1));
            }
            return 0;
        }
        const void* data = m.inArrayAt(A(4), bytes, 1);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPushDescriptorSetWithTemplate)raw)(
            (VkCommandBuffer)H(0), handle, (VkPipelineLayout)A(2), U32A(3),
            data);
        return 0;
    }
    case VKB_CmdSetRenderingAttachmentLocations: {
        const VkRenderingAttachmentLocationInfo* info =
            (const VkRenderingAttachmentLocationInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetRenderingAttachmentLocations)raw)((VkCommandBuffer)H(0),
                                                        info);
        return 0;
    }
    case VKB_CmdSetRenderingInputAttachmentIndices: {
        const VkRenderingInputAttachmentIndexInfo* info =
            (const VkRenderingInputAttachmentIndexInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdSetRenderingInputAttachmentIndices)raw)(
            (VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdBindDescriptorSets2: {
        const VkBindDescriptorSetsInfo* info =
            (const VkBindDescriptorSetsInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdBindDescriptorSets2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdPushConstants2: {
        const VkPushConstantsInfo* info =
            (const VkPushConstantsInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPushConstants2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdPushDescriptorSet2: {
        const VkPushDescriptorSetInfo* info =
            (const VkPushDescriptorSetInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPushDescriptorSet2)raw)((VkCommandBuffer)H(0), info);
        return 0;
    }
    case VKB_CmdPushDescriptorSetWithTemplate2: {
        // The template span is resolved inside the chain fixup, which is where
        // the template handle and the data pointer are in the same structure.
        const VkPushDescriptorSetWithTemplateInfo* info =
            (const VkPushDescriptorSetWithTemplateInfo*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        ((PFN_vkCmdPushDescriptorSetWithTemplate2)raw)((VkCommandBuffer)H(0),
                                                       info);
        return 0;
    }

    case VKB_ReleaseSwapchainImagesEXT: {
        const VkReleaseSwapchainImagesInfoEXT* info =
            (const VkReleaseSwapchainImagesInfoEXT*)m.chain(A(1), false);
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkReleaseSwapchainImagesEXT)raw)((VkDevice)H(0),
                                                           info);
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
#undef H
#undef U32A
#undef F32A

// ---- vkGetInstanceProcAddr / vkGetDeviceProcAddr ----------------------------

// Every author tag Vulkan uses as a command-name suffix. A command whose name
// ends in one of these came from an extension; a command whose name ends in
// none of them is core, and every driver that claims the API version it was
// promoted in must expose it.
//
// The distinction is the whole point of the witness below. Wine's winevulkan
// builds a dispatch table with one slot per command it knows, filled from
// vkGetDeviceProcAddr by name, and a caller reaching a slot the lookup could
// not fill calls through a null. A device run ended exactly there: a 32-bit
// indirect call through a table that was mapped and readable with a zero at
// the entry it wanted. An extension the driver does not have is the ordinary
// case and leaves a null nobody dereferences, because a caller only reaches
// those slots after the extension was enabled. A CORE miss is the dangerous
// one, and until this line existed the two were indistinguishable in a log.
const char* const kVendorSuffixes[] = {
    "KHR", "EXT", "AMD", "AMDX", "NV", "NVX", "INTEL", "ARM", "IMG", "QCOM",
    "VALVE", "HUAWEI", "GOOGLE", "MVK", "FUCHSIA", "ANDROID", "SEC", "NN",
    "MESA", "LUNARG", "QNX", "GGP", "KHX",
};

bool hasVendorSuffix(const char* name) {
    const size_t length = ::strlen(name);
    for (U32 i = 0;
         i < (U32)(sizeof(kVendorSuffixes) / sizeof(kVendorSuffixes[0])); ++i) {
        const size_t tag = ::strlen(kVendorSuffixes[i]);
        if (length > tag && !strcmp(name + length - tag, kVendorSuffixes[i])) {
            return true;
        }
    }
    return false;
}

// `core-miss` or `extension-miss`, so a log can be filtered down to the
// dangerous half without knowing the Vulkan registry.
const char* procAddrMissKind(const char* name) {
    return hasVendorSuffix(name) ? "extension-miss" : "core-miss";
}

std::atomic<U32> gCoreMisses{0};

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
        index = commandIndexForAlias(name);
    }
    if (index < 0) {
        // The point of the whole operation: a run says by name what the
        // caller wanted that this bridge does not carry, which is a far
        // cheaper way to learn DXVK's real requirement list than guessing it.
        //
        // A core miss gets its own status and its own budget, because it is a
        // different kind of event: an extension the bridge does not carry is
        // expected and harmless, while a CORE command it does not carry is how
        // a null gets into winevulkan's dispatch table.
        const char* kind = procAddrMissKind(name);
        const bool core = !hasVendorSuffix(name);
        const U32 seen = core
            ? gCoreMisses.fetch_add(1, std::memory_order_relaxed)
            : gRefusals.fetch_add(1, std::memory_order_relaxed);
        if (seen < kRefusalBudget) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkGetProcAddr "
                     "missing=%s handle=0x%llx status=%lld kind=%s",
                     name, (unsigned long long)handle,
                     (long long)BOXEDWINE_X64_VK_E_BADOP, kind);
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
        // The bridge carries the command and the DRIVER does not. Same split:
        // a core command MoltenVK cannot resolve is a hole this bridge has no
        // way to fill, and saying so by name is the only thing that separates
        // it from an extension nobody enabled.
        const char* kind = procAddrMissKind(name);
        const bool core = !hasVendorSuffix(name);
        const U32 seen = core
            ? gCoreMisses.fetch_add(1, std::memory_order_relaxed)
            : gRefusals.fetch_add(1, std::memory_order_relaxed);
        if (seen < kRefusalBudget) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=vkGetProcAddr "
                     "unsupported=%s handle=0x%llx status=%lld kind=%s",
                     name, (unsigned long long)handle,
                     (long long)BOXEDWINE_X64_VK_E_NOPROC, kind);
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

const char* vulkanBridge64CommandName(U32 callPlusOne) {
#ifdef BOXEDWINE_VULKAN
    if (!callPlusOne || callPlusOne > (U32)VKB_COUNT) {
        return nullptr;
    }
    return kCommandName[callPlusOne - 1];
#else
    (void)callPlusOne;
    return nullptr;
#endif
}

U64 vulkanBridge64(CPU64* cpu, U64 op, U64 argsAddress, U64 count) {
    if (!cpu || !cpu->memory || !cpu->thread || !cpu->thread->process) {
        return (U64)(S64)BOXEDWINE_X64_VK_E_FAULT;
    }
    KMemory64* memory = cpu->memory;
    const U32 pid = (U32)cpu->thread->process->id;
    const U32 tid = (U32)cpu->thread->id;
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
        // Not an error: the guest shim compares this against its own
        // BOXEDWINE_X64_VK_ABI_VERSION and refuses to run when they differ.
        name = "abi";
        result = (S64)BOXEDWINE_X64_VK_ABI_VERSION;
        break;
    case BOXEDWINE_X64_VK_OP_PROBE:
        // Not an error either: a bitmask of BOXEDWINE_X64_VK_CAP_*, so 7 is
        // marshal + identity memory + driver, which is every bit there is.
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
                     "status=%lld",
                     pid, (unsigned long long)op,
                     (long long)BOXEDWINE_X64_VK_E_BADOP);
            return (U64)(S64)BOXEDWINE_X64_VK_E_BADOP;
        }
        name = kCommandName[index];
        // Recorded on the thread before the call, and never cleared. This is
        // what lets a witness fired somewhere else entirely -- the thread-exit
        // marker in sys_exit64 -- say that the thread that just left the
        // process was the one that had been driving Vulkan, which no line
        // printed from inside this file can say about a thread that is gone.
        cpu->thread->diagnosticVulkanBridgeCall.store(
            (U32)index + 1, std::memory_order_relaxed);
        cpu->thread->diagnosticVulkanBridgeCalls.fetch_add(
            1, std::memory_order_relaxed);
        if (!(capabilities & BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY)) {
            // The DXMT rule: a forked child has a sparse KMemory64 and cannot
            // hold a host Vulkan handle. Refuse by name rather than fault
            // inside Metal (docs/KNOWN_LIMITATIONS_IOS.md section 4).
            const U32 refusals =
                gRefusals.fetch_add(1, std::memory_order_relaxed);
            if (refusals < kRefusalBudget) {
                klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s pid=%u "
                         "status=%lld reason=native-memory",
                         name, pid, (long long)BOXEDWINE_X64_VK_E_MEMORY);
            }
            return (U64)(S64)BOXEDWINE_X64_VK_E_MEMORY;
        }
        if (index == VKB_CreateXlibSurfaceKHR) {
            result = count < 4 ? BOXEDWINE_X64_VK_E_ARGS
                               : createXlibSurface(memory, args[0], args[1],
                                                   args[3]);
        } else {
            // One marshal per dispatch. It owns the shadows the driver was
            // given and the write-backs the guest is owed; both end here.
#ifdef BOXEDWINE_IOS
            // The IA-32 lane has timed these two since build 72; this lane
            // never did, so "iOS guest present path: N ms inside
            // vkQueuePresentKHR" reported zero for every 64-bit run no matter
            // how long the compositor held the drawable. Two clock reads on
            // two commands, exactly as the other lane does it.
            const bool timed = (index == VKB_QueuePresentKHR ||
                                index == VKB_AcquireNextImageKHR);
            const auto started = timed
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point();
#endif
            Marshal marshal(memory, name);
            result = dispatchCommand(index, marshal, args, count, tid);
            if (marshal.ok()) {
                marshal.flush();
            }
#ifdef BOXEDWINE_IOS
            if (timed) {
                const std::uint64_t us = (std::uint64_t)
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started).count();
                if (index == VKB_QueuePresentKHR) {
                    bvnHostPresent::recordPresent(us);
                } else {
                    bvnHostPresent::recordAcquire(us);
                }
            }
#endif
        }
        const U32 seen =
            gCommandCalls[index].fetch_add(1, std::memory_order_relaxed);
        const U32 named = gNamedCalls.fetch_add(1, std::memory_order_relaxed);
        if (result < 0 ||
            (seen < kPerCommandBudget && named < kNamedCallCeiling)) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s pid=%u tid=%04X "
                     "args=%llu status=%lld seen=%u",
                     name, pid, tid, (unsigned long long)count,
                     (long long)result, seen + 1);
        }
        writeBack = false; // the marshal wrote the results back itself
#else
        klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE pid=%u op=%llu name=? "
                 "status=%lld",
                 pid, (unsigned long long)op,
                 (long long)BOXEDWINE_X64_VK_E_NOHOST);
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
