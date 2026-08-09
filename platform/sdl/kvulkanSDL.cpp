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
#include "../../source/x11/x11.h"
// Needed wherever the Vulkan bridge is compiled, not just on iOS: the macOS
// development host uses the same presentation code path so that DXVK reaches
// MoltenVK exactly as it does on device.
#ifdef BOXEDWINE_VULKAN
#include "../../source/vulkan/vk/vulkan_core.h"
#define NO_SDL_VULKAN_TYPEDEFS
#endif
#include <SDL.h>
#include <SDL_vulkan.h>
#include "kvulkanSDL.h"
#include "sdlcallback.h"

#ifdef BOXEDWINE_IOS
extern "C" void BVNRegisterGuestVulkanSurface(void* surface);
extern "C" void BVNUnregisterGuestVulkanSurface(void* surface);
extern "C" void BVNGuestVulkanSurfaceDidPresent(void* surface);
extern "C" void* BVNCreateOffscreenMetalLayer(U32 width, U32 height);
extern "C" void BVNDestroyOffscreenMetalLayer(void* layer);

struct BVNVkMetalSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    const void* pLayer;
};

using BVNPFN_vkCreateMetalSurfaceEXT = VkResult (VKAPI_PTR *)(
    VkInstance instance, const BVNVkMetalSurfaceCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);
#endif

namespace {
constexpr U32 kMinimumPresentationWidth = 320;
constexpr U32 kMinimumPresentationHeight = 200;

struct FirstFrameWatch {
    std::atomic<bool> active{true};
    std::atomic<bool> firstPresentObserved{false};
};

struct VulkanSurfaceRecord {
    void* surface = nullptr;
    XWindowPtr window;
    bool presentation = true;
    bool presentationVisible = true;
    bool firstPresentObserved = false;
    U64 acquireAttempts = 0;
    U64 submitAttempts = 0;
    U64 presentAttempts = 0;
    void* offscreenMetalLayer = nullptr;
    std::shared_ptr<FirstFrameWatch> firstFrameWatch;
};

#ifdef BOXEDWINE_IOS
void startFirstFrameWatchdog(const std::shared_ptr<FirstFrameWatch>& watch,
                             void* surface, U32 windowId, U32 width,
                             U32 height) {
    std::thread([watch, surface, windowId, width, height]() {
        // Twelve seconds is far beyond normal swapchain setup, while remaining
        // short enough that one exported device log contains useful evidence.
        std::this_thread::sleep_for(std::chrono::seconds(12));
        if (!watch->active.load(std::memory_order_acquire) ||
            watch->firstPresentObserved.load(std::memory_order_acquire)) {
            return;
        }
        klog_fmt("Vulkan presentation surface %p for X11 window 0x%X "
                 "(%ux%u) produced no frame for 12 seconds",
                 surface, windowId, width, height);
        KSystem::logThreadSnapshot("Vulkan first frame absent for 12 seconds");

        // A second sample distinguishes a blocked thread from slow but active
        // translation by comparing per-thread dispatch counters and futex age.
        std::this_thread::sleep_for(std::chrono::seconds(18));
        if (!watch->active.load(std::memory_order_acquire) ||
            watch->firstPresentObserved.load(std::memory_order_acquire)) {
            return;
        }
        klog_fmt("Vulkan presentation surface %p still produced no frame "
                 "after 30 seconds", surface);
        KSystem::logThreadSnapshot("Vulkan first frame absent for 30 seconds");
    }).detach();
}
#endif
}

class KVulkdanSDLImpl : public KVulkan {
public:
    KVulkdanSDLImpl(const KNativeScreenSDLPtr& screen) : screen(screen) {}
    ~KVulkdanSDLImpl() override;
    KNativeScreenSDLPtr screen;

    void* createVulkanSurface(const XWindowPtr& wnd, void* instance) override;
    void destroyVulkanSurface(void* surface) override;
    bool isPresentationSurface(void* surface) override;
    void registerVulkanSwapchain(void* swapchain, void* surface) override;
    void destroyVulkanSwapchain(void* swapchain) override;
    void acquireVulkanSwapchain(void* swapchain, int result,
                                U32 imageIndex) override;
    void submitVulkanWorkload(int result, U32 submitCount,
                              const char* api) override;
    void presentVulkanSwapchain(void* swapchain, int result) override;
    void revealX11ForDiagnostic(const XWindowPtr& wnd) override;

private:
    std::mutex surfacesMutex;
    std::vector<VulkanSurfaceRecord> surfaces;
    std::unordered_map<void*, void*> swapchainSurfaces;
};

KVulkdanSDLImpl::~KVulkdanSDLImpl() {
#ifdef BOXEDWINE_IOS
    // Wine normally destroys each VkSurfaceKHR. Forced guest shutdown can
    // skip that path, so release any retained offscreen layers as the native
    // system drops this session's Vulkan backend.
    for (const auto& record : surfaces) {
        if (record.firstFrameWatch) {
            record.firstFrameWatch->active.store(false,
                std::memory_order_release);
        }
        if (record.offscreenMetalLayer) {
            BVNDestroyOffscreenMetalLayer(record.offscreenMetalLayer);
        }
    }
#endif
}

void* KVulkdanSDLImpl::createVulkanSurface(const XWindowPtr& wnd,
                                           void* instance) {
    VkSurfaceKHR result = {0};
#ifdef BOXEDWINE_IOS
    const bool presentation = wnd->width() >= kMinimumPresentationWidth &&
        wnd->height() >= kMinimumPresentationHeight;
#else
    const bool presentation = true;
#endif
    void* offscreenMetalLayer = nullptr;

#ifdef __MACH__
    // If SDL_Vulkan_CreateSurface isn't on the main thread, it won't draw on
    // macOS/iOS. The dispatch is synchronous, so the references remain valid.
    DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN_WITH_ARG2(
        =, &result COMMA &offscreenMetalLayer)
#ifdef BOXEDWINE_IOS
    if (!presentation) {
        // A WineD3D capability probe is a legitimate VkSurfaceKHR, but it is
        // not a guest display. Creating it through SDL would install another
        // SDL_uikitmetalview on the only iOS window and cover X11 with a black
        // 113x2 drawable. Give the probe an unattached CAMetalLayer instead.
        offscreenMetalLayer = BVNCreateOffscreenMetalLayer(
            wnd->width(), wnd->height());
        auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            SDL_Vulkan_GetVkGetInstanceProcAddr());
        auto createMetalSurface = getInstanceProcAddr
            ? reinterpret_cast<BVNPFN_vkCreateMetalSurfaceEXT>(
                getInstanceProcAddr((VkInstance)instance,
                                    "vkCreateMetalSurfaceEXT"))
            : nullptr;
        if (!offscreenMetalLayer) {
            kwarn("Failed to allocate an offscreen CAMetalLayer for a Vulkan helper surface");
        } else if (!createMetalSurface) {
            kwarn("MoltenVK did not provide vkCreateMetalSurfaceEXT for an offscreen helper surface");
        }
        if (offscreenMetalLayer && createMetalSurface) {
            BVNVkMetalSurfaceCreateInfo createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
            createInfo.pLayer = offscreenMetalLayer;
            const VkResult createResult = createMetalSurface(
                (VkInstance)instance, &createInfo, nullptr, &result);
            if (createResult != VK_SUCCESS) {
                kwarn_fmt("vkCreateMetalSurfaceEXT failed for an offscreen helper surface: %d",
                          (int)createResult);
                result = 0;
            }
        }
        if (!result && offscreenMetalLayer) {
            BVNDestroyOffscreenMetalLayer(offscreenMetalLayer);
            offscreenMetalLayer = nullptr;
        }
    } else
#endif
    {
        if (!screen->additionalSDLWindowFlags) {
            screen->additionalSDLWindowFlags = SDL_WINDOW_VULKAN;
            screen->recreateMainWindow();
        }
        screen->setScreenSize(wnd->width(), wnd->height());
        screen->showWindow(true);

        if (!SDL_Vulkan_CreateSurface(screen->window, (VkInstance)instance,
                                      &result)) {
            result = 0;
        }
#ifdef BOXEDWINE_IOS
        if (result) {
            BVNRegisterGuestVulkanSurface((void*)result);
        }
#endif
    }
    DISPATCH_MAIN_THREAD_BLOCK_END
#else
    DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
    if (!screen->additionalSDLWindowFlags) {
        screen->additionalSDLWindowFlags = SDL_WINDOW_VULKAN;
        screen->recreateMainWindow();
    }
    screen->setScreenSize(wnd->width(), wnd->height());
    screen->showWindow(true);
    DISPATCH_MAIN_THREAD_BLOCK_END

    if (!SDL_Vulkan_CreateSurface(screen->window, (VkInstance)instance,
                                  &result)) {
        result = 0;
    }
#endif
    if (!result) {
        kwarn_fmt("Failed to create vulkan surface: %s\n", SDL_GetError());
    } else {
        std::shared_ptr<FirstFrameWatch> watch;
#ifdef BOXEDWINE_IOS
        if (presentation) {
            watch = std::make_shared<FirstFrameWatch>();
        }
#endif
        {
            std::lock_guard<std::mutex> lock(surfacesMutex);
            surfaces.push_back({(void*)result, wnd, presentation, presentation,
                                false, 0, 0, 0, offscreenMetalLayer, watch});
            klog_fmt("Vulkan %s surface %p registered for X11 window 0x%x "
                     "(%ux%u); %zu surface(s) active",
                     presentation ? "presentation" : "offscreen helper",
                     (void*)result, wnd->id, wnd->width(), wnd->height(),
                     surfaces.size());
        }
#ifdef BOXEDWINE_IOS
        if (watch) {
            startFirstFrameWatchdog(watch, (void*)result, wnd->id,
                                    wnd->width(), wnd->height());
        }
#endif
    }
    return (void*)result;
}

void KVulkdanSDLImpl::destroyVulkanSurface(void* surface) {
    VulkanSurfaceRecord removed;
    XWindowPtr remainingPresentationWindow;
    size_t remainingCount = 0;
    size_t remainingPresentationCount = 0;
    {
        std::lock_guard<std::mutex> lock(surfacesMutex);
        auto found = std::find_if(surfaces.begin(), surfaces.end(),
            [surface](const auto& item) { return item.surface == surface; });
        if (found == surfaces.end()) {
            kwarn_fmt("Vulkan surface %p was destroyed without a registered "
                      "native surface", surface);
            return;
        }
        removed = *found;
        if (removed.firstFrameWatch) {
            removed.firstFrameWatch->active.store(false,
                std::memory_order_release);
        }
        surfaces.erase(found);
        for (auto iterator = swapchainSurfaces.begin();
             iterator != swapchainSurfaces.end();) {
            if (iterator->second == surface) {
                iterator = swapchainSurfaces.erase(iterator);
            } else {
                ++iterator;
            }
        }
        remainingCount = surfaces.size();
        for (auto iterator = surfaces.rbegin(); iterator != surfaces.rend();
             ++iterator) {
            if (iterator->presentation && iterator->presentationVisible) {
                if (!remainingPresentationWindow) {
                    remainingPresentationWindow = iterator->window;
                }
                ++remainingPresentationCount;
            }
        }
    }

#ifdef BOXEDWINE_IOS
    DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
    if (!removed.presentation) {
        BVNDestroyOffscreenMetalLayer(removed.offscreenMetalLayer);
        klog_fmt("Offscreen Vulkan helper surface %p destroyed; %zu "
                 "surface(s) remain", surface, remainingCount);
        return 0;
    }

    if (!removed.presentationVisible) {
        klog_fmt("Detached diagnostic Vulkan surface %p destroyed normally; "
                 "%zu surface(s) remain", surface, remainingCount);
        return 0;
    }

    // SDL2 creates and installs a new UIKit Metal view for every presentation
    // surface, but has no matching surface-destroy API. Detach the exact view
    // so the prior live presentation is exposed again.
    BVNUnregisterGuestVulkanSurface(surface);

    // Helper surfaces intentionally do not participate in presentation. The
    // X11 compositor must return as soon as the last display-sized Vulkan
    // surface disappears, even when a capability probe remains alive.
    if (remainingPresentationCount == 0) {
        screen->additionalSDLWindowFlags = 0;
        screen->setScreenSize(screen->defaultScreenWidth,
                              screen->defaultScreenHeight);
        screen->recreateMainWindow();
        screen->showWindow(true);
        XServer::getServer()->setFakeFullScreenWindow(nullptr);
        klog_fmt("Last presentation Vulkan surface was destroyed; restored "
                 "the SDL X11 compositor (%zu offscreen/helper surface(s) "
                 "remain)", remainingCount);
    } else {
        XServer::getServer()->setFakeFullScreenWindow(
            remainingPresentationWindow);
        klog_fmt("Vulkan surface %p destroyed; restored the previous iOS "
                 "Metal view (%zu presentation surface(s), %zu total remain)",
                 surface, remainingPresentationCount, remainingCount);
    }
    DISPATCH_MAIN_THREAD_BLOCK_END
#else
    (void)removed;
    (void)remainingPresentationWindow;
    (void)remainingCount;
    (void)remainingPresentationCount;
#endif
}

bool KVulkdanSDLImpl::isPresentationSurface(void* surface) {
    std::lock_guard<std::mutex> lock(surfacesMutex);
    auto found = std::find_if(surfaces.begin(), surfaces.end(),
        [surface](const auto& item) { return item.surface == surface; });
    return found != surfaces.end() && found->presentation;
}

void KVulkdanSDLImpl::registerVulkanSwapchain(void* swapchain,
                                               void* surface) {
    if (!swapchain || !surface) {
        return;
    }
    std::lock_guard<std::mutex> lock(surfacesMutex);
    swapchainSurfaces[swapchain] = surface;
    auto found = std::find_if(surfaces.begin(), surfaces.end(),
        [surface](const auto& item) { return item.surface == surface; });
    klog_fmt("Vulkan swapchain %p registered to %s surface %p",
             swapchain,
             found != surfaces.end() && found->presentation
                 ? "presentation" : "helper/unknown",
             surface);
}

void KVulkdanSDLImpl::destroyVulkanSwapchain(void* swapchain) {
    if (!swapchain) {
        return;
    }
    std::lock_guard<std::mutex> lock(surfacesMutex);
    swapchainSurfaces.erase(swapchain);
}

void KVulkdanSDLImpl::acquireVulkanSwapchain(void* swapchain, int result,
                                             U32 imageIndex) {
    void* surface = nullptr;
    U64 attempt = 0;
    bool presentation = false;
    {
        std::lock_guard<std::mutex> lock(surfacesMutex);
        auto mapped = swapchainSurfaces.find(swapchain);
        if (mapped == swapchainSurfaces.end()) {
            return;
        }
        surface = mapped->second;
        auto found = std::find_if(surfaces.begin(), surfaces.end(),
            [surface](const auto& item) { return item.surface == surface; });
        if (found == surfaces.end()) {
            return;
        }
        presentation = found->presentation;
        attempt = ++found->acquireAttempts;
    }
    if (attempt == 1 || result < 0) {
        klog_fmt("Vulkan %s surface %p acquire attempt %llu returned %d "
                 "(image %u)", presentation ? "presentation" : "helper",
                 surface, (unsigned long long)attempt, result, imageIndex);
    }
}

void KVulkdanSDLImpl::submitVulkanWorkload(int result, U32 submitCount,
                                           const char* api) {
    void* surface = nullptr;
    U64 attempt = 0;
    {
        std::lock_guard<std::mutex> lock(surfacesMutex);
        // Queue submission does not name a swapchain. Attribute it to the
        // newest live presentation surface, which is the view WineD3D has
        // made current and the one covering X11 on iOS.
        for (auto iterator = surfaces.rbegin(); iterator != surfaces.rend();
             ++iterator) {
            if (iterator->presentation && iterator->presentationVisible) {
                surface = iterator->surface;
                attempt = ++iterator->submitAttempts;
                break;
            }
        }
    }
    if (surface && (attempt == 1 || result < 0)) {
        klog_fmt("Vulkan presentation surface %p %s submission attempt %llu "
                 "returned %d (%u submit(s))", surface,
                 api ? api : "queue", (unsigned long long)attempt, result,
                 submitCount);
    }
}

void KVulkdanSDLImpl::presentVulkanSwapchain(void* swapchain, int result) {
    void* surface = nullptr;
    U64 attempt = 0;
    bool firstSuccessfulPresent = false;
    bool presentation = false;
    {
        std::lock_guard<std::mutex> lock(surfacesMutex);
        auto mapped = swapchainSurfaces.find(swapchain);
        if (mapped == swapchainSurfaces.end()) {
            return;
        }
        surface = mapped->second;
        auto found = std::find_if(surfaces.begin(), surfaces.end(),
            [surface](const auto& item) { return item.surface == surface; });
        if (found == surfaces.end()) {
            return;
        }
        presentation = found->presentation;
        attempt = ++found->presentAttempts;
        if (presentation && !found->firstPresentObserved &&
            (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            found->firstPresentObserved = true;
            if (found->firstFrameWatch) {
                found->firstFrameWatch->firstPresentObserved.store(
                    true, std::memory_order_release);
            }
            firstSuccessfulPresent = true;
        }
    }

    if (attempt == 1 || result < 0) {
        klog_fmt("Vulkan %s surface %p queue-present attempt %llu returned %d",
                 presentation ? "presentation" : "helper",
                 surface, (unsigned long long)attempt, result);
    }

#ifdef BOXEDWINE_IOS
    if (firstSuccessfulPresent) {
        // UIKit is main-thread-only. Dispatch just this first transition;
        // subsequent frames remain entirely on the Vulkan/Metal path.
        DISPATCH_MAIN_THREAD_BLOCK_BEGIN_WITH_ARG(surface)
        BVNGuestVulkanSurfaceDidPresent(surface);
        DISPATCH_MAIN_THREAD_BLOCK_END
    }
#else
    (void)firstSuccessfulPresent;
#endif
}

void KVulkdanSDLImpl::revealX11ForDiagnostic(const XWindowPtr& wnd) {
#ifdef BOXEDWINE_IOS
    std::vector<void*> visibleSurfaces;
    {
        std::lock_guard<std::mutex> lock(surfacesMutex);
        for (auto& record : surfaces) {
            if (record.presentation && record.presentationVisible) {
                visibleSurfaces.push_back(record.surface);
                record.presentationVisible = false;
            }
        }
    }
    if (visibleSurfaces.empty()) {
        return;
    }

    // Detach newest-first, matching the UIKit surface stack.  The Vulkan
    // objects remain valid so Wine can finish exception handling and destroy
    // them normally; destroyVulkanSurface() knows they are no longer visible.
    DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
    for (auto iterator = visibleSurfaces.rbegin();
         iterator != visibleSurfaces.rend(); ++iterator) {
        BVNUnregisterGuestVulkanSurface(*iterator);
    }
    screen->additionalSDLWindowFlags = 0;
    screen->setScreenSize(screen->defaultScreenWidth,
                          screen->defaultScreenHeight);
    screen->recreateMainWindow();
    screen->showWindow(true);
    XServer::getServer()->setFakeFullScreenWindow(nullptr);
    klog_fmt("Wine diagnostic X11 window 0x%x replaced %zu live Vulkan "
             "presentation layer(s); the surfaces remain valid for teardown",
             wnd ? wnd->id : 0, visibleSurfaces.size());
    DISPATCH_MAIN_THREAD_BLOCK_END
#else
    (void)wnd;
#endif
}

KVulkanPtr KVulkanSDL::create(const KNativeScreenSDLPtr& screen) {
    return std::make_shared<KVulkdanSDLImpl>(screen);
}

#endif
