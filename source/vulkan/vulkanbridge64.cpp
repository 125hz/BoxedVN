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

#include <atomic>
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
    if (!fn) {
        // The KHR spelling, for a driver that exposes the extension form but
        // not the core one because the instance is Vulkan 1.0.
        buildAliasTable();
        if (gCommandAlias[index]) {
            fn = gipa(gInstance, gCommandAlias[index]);
            if (!fn && gInstance != VK_NULL_HANDLE) {
                fn = gipa(VK_NULL_HANDLE, gCommandAlias[index]);
            }
        }
    }
    gResolved[index] = fn;
    gResolvedFor[index] = gInstance;
    return fn;
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
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,    VkPhysicalDeviceDescriptorBufferFeaturesEXT)

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

    void flush() {
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
    default:
        break;
    }
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
#define H(n) ((void*)(uintptr_t)args[n])
#define U32A(n) ((uint32_t)args[n])

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
S64 dispatchCommand(int index, Marshal& m, const U64* args, U64 count) {
    (void)count;
    PFN_vkVoidFunction raw = hostProc(index);
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
        VkExtensionProperties* properties = (VkExtensionProperties*)m.out(
            A(2), (U64)*countSlot * sizeof(VkExtensionProperties));
        if (!m.ok()) {
            return m.error();
        }
        return (S64)((PFN_vkEnumerateInstanceExtensionProperties)raw)(
            layer, countSlot, properties);
    }
    case VKB_CreateInstance: {
        const VkInstanceCreateInfo* info =
            (const VkInstanceCreateInfo*)m.chain(A(0), false);
        VkInstance* out = (VkInstance*)m.outRequired(A(2), sizeof(VkInstance));
        if (!m.ok()) {
            return m.error();
        }
        const VkResult result =
            ((PFN_vkCreateInstance)raw)(info, nullptr, out);
        if (result == VK_SUCCESS && out && *out) {
            // Every later resolution goes through this instance; a second
            // instance simply replaces it and the per-command cache is keyed
            // on which instance resolved it.
            gInstance = *out;
        }
        return (S64)result;
    }
    case VKB_DestroyInstance: {
        VkInstance instance = (VkInstance)H(0);
        ((PFN_vkDestroyInstance)raw)(instance, nullptr);
        if (gInstance == instance) {
            gInstance = VK_NULL_HANDLE;
        }
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
        return (S64)((PFN_vkQueueSubmit)raw)((VkQueue)H(0), U32A(1), submits,
                                             (VkFence)A(3));
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
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)raw)(
            (VkPhysicalDevice)H(0), (VkSurfaceKHR)A(1), capabilities);
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
        return (S64)((PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)raw)(
            (VkPhysicalDevice)H(0), info, capabilities);
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
        return (S64)((PFN_vkCreateSwapchainKHR)raw)((VkDevice)H(0), info,
                                                    nullptr, out);
    }
    case VKB_DestroySwapchainKHR:
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
        return (S64)((PFN_vkAcquireNextImageKHR)raw)(
            (VkDevice)H(0), (VkSwapchainKHR)A(1), (uint64_t)A(2),
            (VkSemaphore)A(3), (VkFence)A(4), imageIndex);
    }
    case VKB_AcquireNextImage2KHR: {
        const VkAcquireNextImageInfoKHR* info =
            (const VkAcquireNextImageInfoKHR*)m.chain(A(1), false);
        uint32_t* imageIndex = (uint32_t*)m.outRequired(A(2), sizeof(uint32_t));
        if (!m.ok()) {
            return m.error(false);
        }
        return (S64)((PFN_vkAcquireNextImage2KHR)raw)((VkDevice)H(0), info,
                                                      imageIndex);
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
#undef H
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
        index = commandIndexForAlias(name);
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
            // One marshal per dispatch. It owns the shadows the driver was
            // given and the write-backs the guest is owed; both end here.
            Marshal marshal(memory, name);
            result = dispatchCommand(index, marshal, args, count);
            if (marshal.ok()) {
                marshal.flush();
            }
        }
        const U32 named = gNamedCalls.fetch_add(1, std::memory_order_relaxed);
        if (named < kNamedCallBudget || result < 0) {
            klog_fmt("BOXEDWINE_X64_VULKAN_BRIDGE call=%s pid=%u args=%llu "
                     "status=%lld",
                     name, pid, (unsigned long long)count,
                     (long long)result);
        }
        writeBack = false; // the marshal wrote the results back itself
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
