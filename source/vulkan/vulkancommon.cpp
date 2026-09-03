/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
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
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#ifdef BOXEDWINE_VULKAN
#include "knativesystem.h"
#include "../x11/x11.h"
#include "vk_host.h"
#include "vkdef.h"
#include "kvulkan.h"
#include "boxedwine_x64_vulkan_bridge.h"
#include "guest_wine64_layout.h"
#include <SDL_vulkan.h>
#ifdef BOXEDWINE_IOS
#include "BVNFrameRate.h"
#include "bvnhostpresent.h"
#include <chrono>

namespace bvnHostPresent {
std::atomic<std::uint64_t> presentCalls{0};
std::atomic<std::uint64_t> presentMicroseconds{0};
std::atomic<std::uint64_t> presentWorstMicroseconds{0};
std::atomic<std::uint64_t> acquireCalls{0};
std::atomic<std::uint64_t> acquireMicroseconds{0};
std::atomic<std::uint64_t> acquireWorstMicroseconds{0};

static void record(std::atomic<std::uint64_t>& calls,
                   std::atomic<std::uint64_t>& total,
                   std::atomic<std::uint64_t>& worst,
                   std::uint64_t microseconds) {
    calls.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(microseconds, std::memory_order_relaxed);
    std::uint64_t previous = worst.load(std::memory_order_relaxed);
    while (microseconds > previous &&
           !worst.compare_exchange_weak(previous, microseconds,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    }
}

void recordPresent(std::uint64_t microseconds) {
    record(presentCalls, presentMicroseconds, presentWorstMicroseconds,
           microseconds);
}

void recordAcquire(std::uint64_t microseconds) {
    record(acquireCalls, acquireMicroseconds, acquireWorstMicroseconds,
           microseconds);
}
}
#endif

static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr = nullptr;

// MoltenVK advertises promoted KHR extensions but, depending on the Vulkan
// version requested by Wine, may return only the promoted core symbol from
// vkGetInstanceProcAddr. Guest WineD3D deliberately requests Vulkan 1.0 plus
// VK_KHR_get_physical_device_properties2, so refusing the KHR spelling makes
// every queried feature appear unavailable. Vulkan guarantees identical
// signatures for promoted aliases; try the core spelling before declaring an
// entry point missing.
static PFN_vkVoidFunction loadInstanceProcWithPromotedKHRFallback(
        VkInstance instance, const char* name) {
    PFN_vkVoidFunction result = pvkGetInstanceProcAddr(instance, name);
    const size_t length = strlen(name);
    if (!result && length > 3 && !strcmp(name + length - 3, "KHR")) {
        std::string coreName(name, length - 3);
        result = pvkGetInstanceProcAddr(instance, coreName.c_str());
        if (result) {
            klog_fmt("Boxedwine: resolved promoted Vulkan alias %s through %s",
                     name, coreName.c_str());
        }
    }
    return result;
}

static U32 vulkanPtrCount;
static U32 vulkanPtrHighMark;

#ifdef _DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {

    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
}
#endif

U32 createVulkanPtr(KMemory* memory, void* value, BoxedVulkanInfo* info) {
    KProcessPtr process = KThread::currentThread()->process;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(process->freeVulkanPtrMutex);
    U32 result = 0;

    if (value == nullptr) {
        return 0;
    }
    if (process->vulkanPtrMap.get(value, result)) {
        return result;
    }
    if (!process->vulkanFreePtrAddress) {
        KThread* thread = KThread::currentThread();
        U32 address = thread->memory->mmap(thread, 0, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE, K_MAP_ANONYMOUS | K_MAP_PRIVATE | K_MAP_BOXEDWINE, -1, 0);
        for (U32 i = 0; i < K_PAGE_SIZE; i += 16) {
            memory->writed(address + i, process->vulkanFreePtrAddress);
            process->vulkanFreePtrAddress = address + i;
        }
    }
    result = process->vulkanFreePtrAddress;
    process->vulkanFreePtrAddress = memory->readd(process->vulkanFreePtrAddress);
    memory->writeq(result, (U64)value);

    if (!info) {
        info = new BoxedVulkanInfo();
        if (!pvkGetInstanceProcAddr) {
            pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
        }
#undef VKFUNC
#undef VKFUNC_INSTANCE
#define VKFUNC_INSTANCE(f) info->pvk##f = (PFN_vk##f)loadInstanceProcWithPromotedKHRFallback((VkInstance)value, "vk"#f); if (!info->pvk##f) {kwarn("Boxedwine: Failed to load vk"#f);} else {info->functionAddressByName[B("vk"#f)]=1;}
#define VKFUNC(f)
#include "vkfuncs.h" 
        info->instance = (VkInstance)value;

#ifdef _DEBUG1
        KNativeSystem::getScreen()->showWindow(true);
        PFN_vkCreateDebugUtilsMessengerEXT debugFunc = (PFN_vkCreateDebugUtilsMessengerEXT)pvkGetInstanceProcAddr((VkInstance)value, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        memset(&createInfo, 0, sizeof(VkDebugUtilsMessengerCreateInfoEXT));
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr;

        if (debugFunc) {
            debugFunc(info->instance, &createInfo, nullptr, &info->debugMessenger);
        } else {
            klog("Vulkan debug function not found");
        }
#endif
    }
    memory->writeq(result + 8, (U64)info);
    vulkanPtrCount++;
    vulkanPtrHighMark = std::max(vulkanPtrHighMark, vulkanPtrCount);
    process->vulkanPtrMap.set(value, result);
    return result;
}

BoxedVulkanInfo* getInfoFromHandle(KMemory* memory, U32 address) {
    return (BoxedVulkanInfo*)memory->readq(address+8);
}

static void hasProcAddress(CPU* cpu) {
    U32 handle = cpu->peek32(1);
    if (!handle) {
        EAX = 1;
    } else {
        BoxedVulkanInfo* pBoxedInfo = getInfoFromHandle(cpu->memory, handle);
        BString name = cpu->memory->readString(cpu->peek32(2));

        if (name == "vkMapMemory2KHR" || name == "vkUnmapMemory2KHR") {
            EAX = 0;
        } else if (pBoxedInfo->functionAddressByName.count(name) || name == "vkGetDeviceProcAddr" || name == "vkCreateXlibSurfaceKHR") {
            EAX = 1;
        }
        else {
            EAX = 0;
        }
    }
}

void freeVulkanPtr(KMemory* memory, U32 p) {
    KProcessPtr process = KThread::currentThread()->process;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(process->freeVulkanPtrMutex);
    void* address = getVulkanPtr(memory, p);
    process->vulkanPtrMap.remove(address);
    memory->writed(p, process->vulkanFreePtrAddress);
    process->vulkanFreePtrAddress = p;
    vulkanPtrCount--;
}

void* getVulkanPtr(KMemory* memory, U32 address) {
    return (void*)(memory->readq(address));
}

class VMemory {
public:
    VkDeviceMemory memory;
    VkDeviceSize size;
    VkDeviceSize mappedOffset;
    VkDeviceSize mappedLen;
    U32 mappedAddress;
    U32 processId;
    void* hostAddress;
};

class RecentVulkanMapping {
public:
    VkDeviceMemory memory;
    VkDeviceSize allocationSize;
    VkDeviceSize offset;
    VkDeviceSize length;
    U32 guestAddress;
    U32 processId;
    void* hostAddress;
};

std::unordered_map<VkDeviceMemory, std::shared_ptr<VMemory>> vmemory;
static std::vector<RecentVulkanMapping> recentVulkanMappings;
static std::vector<U64> reportedVulkanFaults;
static constexpr size_t kRecentVulkanMappingLimit = 32;
static constexpr size_t kReportedVulkanFaultLimit = 64;
BOXEDWINE_MUTEX vmemoryMutex;

static U64 mappingPageEnd(U32 guestAddress, VkDeviceSize length) {
    const U64 dataStart = (U64)guestAddress & ~(U64)K_PAGE_MASK;
    const U64 dataBytes = (guestAddress & K_PAGE_MASK) + length;
    return dataStart + ((dataBytes + K_PAGE_MASK) & ~(U64)K_PAGE_MASK);
}

static const char* classifyVulkanMappingAddress(
        U32 address, U32 guestAddress, VkDeviceSize length) {
    const U64 fault = address;
    const U64 start = guestAddress;
    const U64 end = start + length;
    const U64 pageStart = start & ~(U64)K_PAGE_MASK;
    const U64 pageEnd = mappingPageEnd(guestAddress, length);
    if (fault >= start && fault < end) {
        return "mapped data";
    }
    if (pageStart >= K_PAGE_SIZE && fault >= pageStart - K_PAGE_SIZE &&
        fault < pageStart) {
        return "leading guard page";
    }
    if (fault >= pageEnd && fault < pageEnd + K_PAGE_SIZE) {
        return "trailing guard page";
    }
    return nullptr;
}

static void rememberVulkanMapping(const std::shared_ptr<VMemory>& mapping) {
    recentVulkanMappings.push_back({mapping->memory, mapping->size,
        mapping->mappedOffset, mapping->mappedLen, mapping->mappedAddress,
        mapping->processId, mapping->hostAddress});
    if (recentVulkanMappings.size() > kRecentVulkanMappingLimit) {
        recentVulkanMappings.erase(recentVulkanMappings.begin());
    }
}

std::shared_ptr<VMemory> getVMemory(VkDeviceMemory memory) {
    if (vmemory.count(memory))
        return vmemory[memory];
    return NULL;
}

void registerVkMemoryAllocation(VkDeviceMemory memory, VkDeviceSize size) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(vmemoryMutex);
    if (vmemory.empty()) {
        // A later guest commonly reuses the same small process IDs and 32-bit
        // ranges. Do not suppress or misclassify its first fault using the
        // preceding Vulkan device's bounded diagnostic history.
        recentVulkanMappings.clear();
        reportedVulkanFaults.clear();
    }
    std::shared_ptr<VMemory> m = std::make_shared<VMemory>();
    m->memory = memory;
    m->size = size;
    m->mappedOffset = 0;
    m->mappedLen = 0;
    m->mappedAddress = 0;
    m->processId = 0;
    m->hostAddress = nullptr;
    vmemory[memory] = m;
}

void unregisterVkMemoryAllocation(VkDeviceMemory memory) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(vmemoryMutex);
    std::shared_ptr<VMemory> m = getVMemory(memory);
    if (m) {
        if (m->mappedAddress) {
            rememberVulkanMapping(m);
            kwarn_fmt("Freeing still-mapped Vulkan memory %p: pid %x guest "
                      "%.8X length %llu",
                      (void*)memory, m->processId, m->mappedAddress,
                      (unsigned long long)m->mappedLen);
        }
        vmemory.erase(memory);
    }
}

U32 mapVkMemory(VkDeviceMemory memory, void* pData, VkDeviceSize offset, VkDeviceSize len) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(vmemoryMutex);
    std::shared_ptr<VMemory> m = getVMemory(memory);
    if (!m) {
        kpanic("Wasn't expecting mapVkMemory before registerVkMemoryAllocation");
    }
    if (m->mappedAddress) {
        kpanic("Wasn't expecting mapVkMemory to be called twice on the same memory");
    }
    if (offset > m->size) {
        kpanic("mapVkMemory offset exceeds allocation size");
    }
    if (len == VK_WHOLE_SIZE) {
        len = m->size - offset;
    }
    if (len > m->size - offset) {
        kpanic("mapVkMemory range exceeds allocation size");
    }
    if (len > std::numeric_limits<U32>::max()) {
        kpanic("mapVkMemory range exceeds the 32-bit guest address space");
    }
    m->mappedOffset = offset;
    m->mappedLen = len;
    m->processId = KThread::currentThread()->process->id;
    m->hostAddress = pData;
    m->mappedAddress = KThread::currentThread()->memory->mapNativeMemory(pData, (U32)len);
    if (!m->mappedAddress) {
        kwarn_fmt("Failed to reserve a 32-bit guest range for Vulkan memory "
                  "%p (pid %x, length %llu)", (void*)memory, m->processId,
                  (unsigned long long)len);
    }
    return m->mappedAddress;
}

void unmapVkMemory(VkDeviceMemory memory) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(vmemoryMutex);
    std::shared_ptr<VMemory> m = getVMemory(memory);
    if (!m) {
        kpanic("Wasn't expecting mapVkMemory before registerVkMemoryAllocation");
    }
    if (!m->mappedAddress) {
        klog("unmapVkMemory called, but no record of being mapped");
        return;
    }
    rememberVulkanMapping(m);
    KThread::currentThread()->memory->unmapNativeMemory(m->mappedAddress, (U32)m->mappedLen);
    m->mappedAddress = 0;
    m->mappedOffset = 0;
    m->mappedLen = 0;
    m->processId = 0;
    m->hostAddress = nullptr;
}

void logVkMemoryFaultContext(U32 processId, U32 address, bool writeFault) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(vmemoryMutex);
    if (vmemory.empty() && recentVulkanMappings.empty()) {
        return;
    }

    const U64 faultKey = ((U64)processId << 33) | ((U64)address << 1) |
        (writeFault ? 1 : 0);
    if (std::find(reportedVulkanFaults.begin(), reportedVulkanFaults.end(),
                  faultKey) != reportedVulkanFaults.end()) {
        return;
    }
    reportedVulkanFaults.push_back(faultKey);
    if (reportedVulkanFaults.size() > kReportedVulkanFaultLimit) {
        reportedVulkanFaults.erase(reportedVulkanFaults.begin());
    }

    for (const auto& item : vmemory) {
        const auto& mapping = item.second;
        if (!mapping->mappedAddress || mapping->processId != processId) {
            continue;
        }
        const char* classification = classifyVulkanMappingAddress(
            address, mapping->mappedAddress, mapping->mappedLen);
        if (classification) {
            klog_fmt("Guest %s fault %.8X is in Vulkan %s: memory %p "
                     "allocation %llu offset %llu host %p guest "
                     "[%.8X, %.8llX)",
                     writeFault ? "write" : "read", address,
                     classification, (void*)mapping->memory,
                     (unsigned long long)mapping->size,
                     (unsigned long long)mapping->mappedOffset,
                     mapping->hostAddress,
                     mapping->mappedAddress,
                     (unsigned long long)((U64)mapping->mappedAddress +
                                          mapping->mappedLen));
            return;
        }
    }
    for (auto iterator = recentVulkanMappings.rbegin();
         iterator != recentVulkanMappings.rend(); ++iterator) {
        if (iterator->processId != processId) {
            continue;
        }
        const char* classification = classifyVulkanMappingAddress(
            address, iterator->guestAddress, iterator->length);
        if (classification) {
            klog_fmt("Guest %s fault %.8X is in a recently unmapped Vulkan "
                     "%s: memory %p allocation %llu offset %llu host %p "
                     "guest [%.8X, %.8llX)",
                     writeFault ? "write" : "read", address,
                     classification, (void*)iterator->memory,
                     (unsigned long long)iterator->allocationSize,
                     (unsigned long long)iterator->offset,
                     iterator->hostAddress,
                     iterator->guestAddress,
                     (unsigned long long)((U64)iterator->guestAddress +
                                          iterator->length));
            return;
        }
    }
    klog_fmt("Guest %s fault %.8X (pid %x) is outside all live and the last "
             "%zu unmapped Vulkan guest ranges",
             writeFault ? "write" : "read", address, processId,
             recentVulkanMappings.size());
}

#define ARG1 cpu->peek32(1)
#define ARG2 cpu->peek32(2)
#define ARG3 cpu->peek32(3)
#define ARG4 cpu->peek32(4)

/*
typedef struct VkXlibSurfaceCreateInfoKHR {
    VkStructureType                sType;
    const void* pNext;
    VkFlags    flags;
    Display* dpy;
    Window                         window;
} VkXlibSurfaceCreateInfoKHR;
*/

// VkResult vkCreateXlibSurfaceKHR( VkInstance instance, const VkXlibSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface  ) const
static void BOXED_vkCreateXlibSurfaceKHR(CPU* cpu) {
    //VkInstance instance,
    //const VkWin32SurfaceCreateInfoKHR* create_info,
    //const VkAllocationCallbacks* allocator, 
    // VkSurfaceKHR* surface

    KVulkanPtr vulkanWnd = KNativeSystem::getVulkan();

    void* instance = getVulkanPtr(cpu->memory, cpu->peek32(1));
    // window is the 5th 32-bit variable in VkXlibSurfaceCreateInfoKHR
    U32 windowId = cpu->memory->readd(ARG2 + 4 * sizeof(U32));
    XWindowPtr xWindow = XServer::getServer()->getWindow(windowId);
    void* surface = vulkanWnd->createVulkanSurface(xWindow, instance);
    if (!surface) {
        EAX = VK_ERROR_OUT_OF_HOST_MEMORY;
    } else {
        EAX = VK_SUCCESS;
        // VK_DEFINE_NON_DISPATCHABLE_HANDLE (always 64 bit)
        cpu->memory->writeq(cpu->peek32(4), (U64)surface);
        // Wine also creates tiny Vulkan surfaces solely to probe Direct3D
        // capabilities. The native backend keeps those surfaces alive but
        // offscreen; only a real presentation surface may replace X11 as the
        // fake-fullscreen input/rendering target.
        if (vulkanWnd->isPresentationSurface(surface)) {
            XServer::getServer()->setFakeFullScreenWindow(xWindow);
        }
    }
}

#include "vk_host_marshal.h"

void initVulkan();
void vk_CreateInstance(CPU* cpu) {
    initVulkan();
    MarshalVkInstanceCreateInfo local_pCreateInfo(nullptr, cpu->memory, ARG1);
    VkInstanceCreateInfo* pCreateInfo = &local_pCreateInfo.s;
    static bool shown; if (!shown && ARG2) { klog("vkCreateInstance:VkAllocationCallbacks not implemented"); shown = true; }
    VkAllocationCallbacks* pAllocator = NULL;
    VkInstance pInstance;
    bool containsDebug = false;
    for (U32 i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        if (strstr(pCreateInfo->ppEnabledExtensionNames[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            containsDebug = true;
        } else if (strstr(pCreateInfo->ppEnabledExtensionNames[i], "VK_KHR_xlib_surface")) {
            delete[] pCreateInfo->ppEnabledExtensionNames[i];
#ifdef BOXEDWINE_IOS
            // SDL's UIKit Vulkan backend creates a CAMetalLayer-backed
            // VkSurfaceKHR through VK_EXT_metal_surface. The upstream
            // __MACH__ branch assumes AppKit/macOS and requests
            // VK_MVK_macos_surface, which MoltenVK cannot use on iOS.
            const char* platformSurface = "VK_EXT_metal_surface";
#elif defined(__MACH__)
            const char* platformSurface = "VK_MVK_macos_surface";
#else
            const char* platformSurface = "VK_KHR_win32_surface";
#endif
            char** p = new char* [pCreateInfo->enabledExtensionCount];
            memcpy(p, pCreateInfo->ppEnabledExtensionNames, sizeof(char*) * (pCreateInfo->enabledExtensionCount));
            p[i] = new char[strlen(platformSurface) + 1];
            strcpy(p[i], platformSurface);
            delete[] pCreateInfo->ppEnabledExtensionNames;
            pCreateInfo->ppEnabledExtensionNames = p;
        }
    }
#ifdef _DEBUG1
    if (!containsDebug) {
        char** p = new char*[pCreateInfo->enabledExtensionCount + 1];
        memcpy(p, pCreateInfo->ppEnabledExtensionNames, sizeof(char*) * (pCreateInfo->enabledExtensionCount));
        p[pCreateInfo->enabledExtensionCount] = new char[strlen(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) + 1];
        strcpy(p[pCreateInfo->enabledExtensionCount], VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        delete[] pCreateInfo->ppEnabledExtensionNames;
        pCreateInfo->ppEnabledExtensionNames = p;
        pCreateInfo->enabledExtensionCount++;
    }
    pCreateInfo->enabledLayerCount = 1;
    char** names = new char*[2];
    names[0] = (char*)"VK_LAYER_KHRONOS_validation";
    names[1] = (char*)"VK_LAYER_LUNARG_api_dump";
    pCreateInfo->ppEnabledLayerNames = names;
#endif
    EAX = pvkCreateInstance(pCreateInfo, pAllocator, &pInstance);
    if (EAX == VK_SUCCESS) {
        cpu->memory->writed(ARG3, createVulkanPtr(cpu->memory, pInstance, NULL));
    }
}

void vk_DestroyInstance2(CPU* cpu) {
    if (!ARG1) {
        return;
    }
    VkInstance instance = (VkInstance)getVulkanPtr(cpu->memory, ARG1);
    BoxedVulkanInfo* pBoxedInfo = getInfoFromHandle(cpu->memory, ARG1);
    static bool shown; if (!shown && ARG2) { klog("vkDestroyInstance:VkAllocationCallbacks not implemented"); shown = true; }
    VkAllocationCallbacks* pAllocator = NULL;

#ifdef _DEBUG
    PFN_vkDestroyDebugUtilsMessengerEXT debugFunc = (PFN_vkDestroyDebugUtilsMessengerEXT)pvkGetInstanceProcAddr(pBoxedInfo->instance, "vkDestroyDebugUtilsMessengerEXT");

    if (debugFunc) {
        debugFunc(pBoxedInfo->instance, pBoxedInfo->debugMessenger, nullptr);
    } else {
        klog("Vulkan debug function not found");
    }
#endif

    pBoxedInfo->pvkDestroyInstance(instance, pAllocator);
    freeVulkanPtr(cpu->memory, ARG1);
}

void vk_EnumerateInstanceExtensionProperties(CPU* cpu) {
    initVulkan();
    U32 len = 0;
    char* pLayerName = nullptr;
    if (ARG1) {
        len = cpu->memory->strlen(ARG1) + 1;
        pLayerName = new char[len];
        cpu->memory->memcpy(pLayerName, ARG1, (U32)len * sizeof(char));
    }
    uint32_t tmp_pPropertyCount = (uint32_t)cpu->memory->readd(ARG2);
    uint32_t* pPropertyCount = &tmp_pPropertyCount;
    VkExtensionProperties* pProperties = nullptr;
    if (ARG3) {
        pProperties = new VkExtensionProperties[*pPropertyCount];
        cpu->memory->memcpy(pProperties, ARG3, (U32)*pPropertyCount * sizeof(VkExtensionProperties));
    }
    EAX = pvkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
    delete[] pLayerName;
    cpu->memory->writed(ARG2, (U32)tmp_pPropertyCount);
    if (pProperties) {
        for (U32 i = 0; i < tmp_pPropertyCount; i++) {
            if (!strcmp(pProperties[i].extensionName, "VK_MVK_macos_surface") || !strcmp(pProperties[i].extensionName, "VK_EXT_metal_surface") || !strcmp(pProperties[i].extensionName, "VK_KHR_win32_surface")) {
                strcpy(pProperties[i].extensionName, "VK_KHR_xlib_surface");
            }            
        }
        cpu->memory->memcpy(ARG3, pProperties, (U32)*pPropertyCount * sizeof(VkExtensionProperties));
    }
    delete[] pProperties;
}

VkBool32 VKAPI_PTR boxed_vkDebugReportCallbackEXT(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData) {
    if (pMessage) {
        klog(pMessage);
    } else {
        klog_fmt("vkDebugReportCallbackEXT %d", messageCode);
    }
    return VK_TRUE;
}

VkBool32 VKAPI_PTR boxed_vkDebugUtilsMessengerCallbackEXT(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
    if (pCallbackData && pCallbackData->pMessage) {
        klog(pCallbackData->pMessage);
    } else {
        klog("vkDebugUtilsMessengerCallbackEXT");
    }
    return VK_TRUE;
}

// ported/inspired by https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/layers/core_checks/cc_descriptor.cpp
U32 calculateUpdateDescriptorSetWithTemplateDataSize(BoxedVulkanInfo* pBoxedInfo, VkDescriptorUpdateTemplate descriptorUpdateTemplate) {
    std::shared_ptr<MarshalVkDescriptorUpdateTemplateCreateInfo> pCreateInfo = pBoxedInfo->descriptorUpdateTemplateCreateInfo.at((U64)descriptorUpdateTemplate);
    if (!pCreateInfo) {
        kpanic("calculateUpdateDescriptorSetWithTemplateDataSize missing cached pCreateInfo");
    }

    size_t size = 0;
    for (uint32_t i = 0; i < pCreateInfo->s.descriptorUpdateEntryCount; i++) {
        uint32_t count = pCreateInfo->s.pDescriptorUpdateEntries[i].descriptorCount;
        // find the fartherest offset + count * stride
        size = std::max(size, pCreateInfo->s.pDescriptorUpdateEntries[i].offset + count * pCreateInfo->s.pDescriptorUpdateEntries[i].stride);
    }

    for (uint32_t i = 0; i < pCreateInfo->s.descriptorUpdateEntryCount; i++) {
        for (uint32_t j = 0; j < pCreateInfo->s.pDescriptorUpdateEntries[i].descriptorCount; j++) {
            switch (pCreateInfo->s.pDescriptorUpdateEntries[i].descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                if (sizeof(VkDescriptorImageInfo) > pCreateInfo->s.pDescriptorUpdateEntries[i].stride) {
                    // because of padding, this can be 20 or 24
                    kpanic_fmt("calculateUpdateDescriptorSetWithTemplateDataSize sizeof(VkDescriptorBufferInfo) %d > stride", sizeof(VkDescriptorImageInfo), pCreateInfo->s.pDescriptorUpdateEntries[i].stride);
                }
                break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                if (sizeof(VkDescriptorBufferInfo) != 24) {
                    kpanic_fmt("calculateUpdateDescriptorSetWithTemplateDataSize unexpected sizeof(VkDescriptorBufferInfo) %d", sizeof(VkDescriptorBufferInfo));
                }
                break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                // VkBufferView - VK_DEFINE_NON_DISPATCHABLE_HANDLE
                break;
            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                // size = pCreateInfo->s.pDescriptorUpdateEntries[i].descriptorCount
                // 
                // skip the rest of the array, they just represent bytes in the update
                j = pCreateInfo->s.pDescriptorUpdateEntries[i].descriptorCount;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                // VkAccelerationStructureKHR - VK_DEFINE_NON_DISPATCHABLE_HANDLE
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
                // VkAccelerationStructureNV - VK_DEFINE_NON_DISPATCHABLE_HANDLE
                break;
            default:
            {
                static bool showOnce;
                if (!showOnce) {
                    kwarn_fmt("calculateUpdateDescriptorSetWithTemplateDataSize unexpected descriptorType %x", pCreateInfo->s.pDescriptorUpdateEntries[i].descriptorType);
                    showOnce = true;
                }
            }
                break;
            }
        }
    }
    return (U32)size;
}

#include "../vulkan/vk_host.h"

static bool vulkanInitialized;

void initVulkan() {
    if (!vulkanInitialized) {
        BOXEDWINE_CRITICAL_SECTION;
        if (vulkanInitialized) {
            return;
        }        

        if (SDL_Vulkan_LoadLibrary(NULL)) {
            kpanic_fmt("Failed to load vulkan: %s\n", SDL_GetError());
        }
        pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
        pvkCreateInstance = (PFN_vkCreateInstance)pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
        pvkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
#undef VKFUNC_INSTANCE
#define VKFUNC_INSTANCE(f)
#undef VKFUNC
#define VKFUNC(f) pvk##f = (PFN_vk##f)pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vk"#f); if (!pvk##f) {kwarn("Boxedwine: Failed to load vk"#f);}
#include "../vulkan/vkfuncs.h"
#undef LOAD_FUNCPTR
        vulkanInitialized = true;
    }
}

Int99Callback int9ACallback[VK_LAST_VALUE+1];
U32 int9ACallbackSize;

void vulkan_init() {
    // One startup line that answers "can a 64-bit guest reach the host's
    // Vulkan at all", which today it cannot. The IA-32 lane traps through
    // `int 0x9A` into the callback table built below; CPU64 decodes no such
    // trap, so the 64-bit lane needs a guest `libvulkan.so.1` of its own that
    // traps through BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE, built and staged
    // the way tools/x11-64 builds the X11 client shims. Nothing in this tree
    // builds one yet, and the file a 64-bit guest does find under that name
    // is the IA-32 shim in the root filesystem, which its loader rejects for
    // its ELF class and silently walks past. `present` flips when the
    // packaging lands and defines BOXEDWINE_X64_VULKAN_GUEST_SHIM; see
    // docs/PLAN_WOW64_D3D9.md.
#if defined(BOXEDWINE_X64_VULKAN_GUEST_SHIM)
    const int guestShimPresent = 1;
#else
    const int guestShimPresent = 0;
#endif
    klog_fmt("BOXEDWINE_X64_VULKAN_SHIM present=%d icd=%s hostcall=0x%llx "
             "abi=%u soname=%s path=%s lane32_ops=%u",
             guestShimPresent,
             guestShimPresent ? BOXEDWINE_X64_VK_GUEST_SONAME : "none",
             (unsigned long long)BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE,
             (unsigned)BOXEDWINE_X64_VK_ABI_VERSION,
             BOXEDWINE_X64_VK_GUEST_SONAME,
             K_X64_GUEST_VULKAN_LIB_PATH,
             (unsigned)(VK_LAST_VALUE + 1));

    int9ACallbackSize = VK_LAST_VALUE+1;

#undef VKFUNC
#undef VKFUNC_INSTANCE
#define VKFUNC(name) int9ACallback[name] = vk_##name;
#define VKFUNC_INSTANCE(name) int9ACallback[name] = vk_##name;
#include "vkfuncs.h"      

    int9ACallback[CreateXlibSurfaceKHR] = BOXED_vkCreateXlibSurfaceKHR;
    int9ACallback[GetDeviceProcAddr] = hasProcAddress;
    int9ACallback[GetInstanceProcAddr] = hasProcAddress;
    int9ACallback[CreateInstance] = vk_CreateInstance;
    int9ACallback[DestroyInstance] = vk_DestroyInstance2;
    int9ACallback[EnumerateInstanceExtensionProperties] = vk_EnumerateInstanceExtensionProperties;
}

#endif

void callVulkan(CPU* cpu, U32 index) {
#ifdef BOXEDWINE_VULKAN
    if (index < int9ACallbackSize) {
        if (int9ACallback[index]) {
            KThread* thread = cpu->thread;
            if (thread) {
                thread->diagnosticVulkanCall.store(index + 1,
                                                   std::memory_order_relaxed);
            }
#ifdef BOXEDWINE_IOS
            // Only the two calls that can block on the compositor are timed,
            // so this costs two clock reads per presented frame rather than
            // two per Vulkan call.
            const bool timed = (index == QueuePresentKHR ||
                                index == AcquireNextImageKHR);
            const auto started = timed
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point();
            if (index == QueuePresentKHR) {
                BVNGuestFrameLimiterWait();
            }
#endif
            int9ACallback[index](cpu);
#ifdef BOXEDWINE_IOS
            if (timed) {
                const std::uint64_t us = (std::uint64_t)
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started).count();
                if (index == QueuePresentKHR) {
                    bvnHostPresent::recordPresent(us);
                } else {
                    bvnHostPresent::recordAcquire(us);
                }
            }
#endif
            if (thread) {
                thread->diagnosticVulkanCall.store(0,
                                                   std::memory_order_relaxed);
            }
        } else {
            kpanic_fmt("Vulkan tried to call missing function: %d", index);
        }
    } else 
#endif
    {
        kpanic_fmt("Vulkan not compiled into Boxedwine: %d", index);
    }
}
