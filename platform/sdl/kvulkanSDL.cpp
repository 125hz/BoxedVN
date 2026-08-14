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
#include <sys/resource.h>
#include "getrusagefairness.h"
#include "bvnhostpresent.h"
#endif

#ifdef BOXEDWINE_IOS
extern "C" void BVNRegisterGuestVulkanSurface(void* surface);
extern "C" void BVNApplyGuestPresentationAspect(void* surface,
                                                U32 guestWidth,
                                                U32 guestHeight,
                                                int presentationMode);
extern "C" int BVNGuestPreferredPresentationMode(void);
extern "C" void BVNUnregisterGuestVulkanSurface(void* surface);
extern "C" void BVNGuestPresentationNaturalDrawableSize(int* width,
                                                        int* height);
extern "C" void BVNGuestVulkanSurfaceDidPresent(void* surface);
extern "C" bool BVNGuestTakeX11PatchClearRequest(void);
extern "C" void BVNGuestClearX11Patches(void);
extern "C" void BVNGuestPerformanceFramePresented(void);
extern "C" uint64_t BVNGuestX11PatchCount(void);
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
            if (presentation) {
                // Letterbox rather than stretch, and do it HERE, inside the
                // main-thread block, rather than after it.
                //
                // This used to be called from the guest thread further down,
                // and BVNApplyGuestPresentationAspect forwarded itself to the
                // main *dispatch queue*. The build-63 device log proves those
                // blocks never ran: both were still queued when the session
                // ended 2.5 minutes later, and executed only after "Boxedwine
                // shutdown", by which point their surfaces had already been
                // unregistered ("no registered view for this surface"). While
                // boxedmain owns the main thread, SDL's pump services UIKit
                // events but the main dispatch queue is not drained.
                // DISPATCH_MAIN_THREAD_BLOCK is an SDL user event, which is,
                // and it is what every other UIKit call on this path uses.
                //
                // KNativeScreenSDL then re-derives the pointer transform from
                // the rectangle the presenter measured, so the picture and the
                // touch target cannot disagree. setScreenSize above ran before
                // the surface existed and could only see a stretched window.
                BVNApplyGuestPresentationAspect(
                    (void*)result, wnd->width(), wnd->height(),
                    BVNGuestPreferredPresentationMode());
                screen->refreshIOSGuestPointerTransform();
            }
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
            S32 rootX = 0;
            S32 rootY = 0;
            wnd->windowToScreen(rootX, rootY);
            klog_fmt("Vulkan %s surface %p registered for X11 window 0x%x "
                     "(%ux%u at root %d,%d); %zu surface(s) active",
                     presentation ? "presentation" : "offscreen helper",
                     (void*)result, wnd->id, wnd->width(), wnd->height(),
                     rootX, rootY, surfaces.size());
        }
#ifdef BOXEDWINE_IOS
        // The aspect fit is applied at surface creation, inside the
        // main-thread block above - see the comment there for why it cannot
        // be done from this thread.
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
    const bool isPresentation =
        found != surfaces.end() && found->presentation;

#ifdef BOXEDWINE_IOS
    // This used to log one line per call. The build-63 device session produced
    // 17,879 of them in 150 seconds - DXVK was recreating the presentation
    // swapchain on essentially every frame, because acquire and present kept
    // returning VK_SUBOPTIMAL_KHR. MoltenVK raises that whenever the layer's
    // natural drawable size (bounds x contentsScale) differs from the extent
    // the swapchain was actually created at, and with SDL's Metal view filling
    // the window while the guest renders at 800x600 they could never agree.
    //
    // The first few are logged individually, because a healthy session creates
    // one or two and those are worth seeing. After that the log reports the
    // rate instead of the event, so the next device log answers in one line
    // what took counting 17,879 of them to see.
    static constexpr U32 kIndividuallyLogged = 4;
    static U32 totalCreations = 0;
    static U32 windowStart = 0;
    static U32 windowCount = 0;
    if (isPresentation) {
        const U32 now = SDL_GetTicks();
        ++totalCreations;
        if (totalCreations <= kIndividuallyLogged) {
            int naturalWidth = 0;
            int naturalHeight = 0;
            BVNGuestPresentationNaturalDrawableSize(&naturalWidth,
                                                    &naturalHeight);
            klog_fmt("Vulkan presentation swapchain %p created (#%u); the "
                     "presenting layer's natural drawable size is %dx%d. If "
                     "MoltenVK's next \"Created N swapchain images with size\" "
                     "line disagrees with that, every acquire will return "
                     "VK_SUBOPTIMAL_KHR and DXVK will rebuild this swapchain "
                     "on every frame",
                     swapchain, totalCreations, naturalWidth, naturalHeight);
            windowStart = now;
            windowCount = 0;
            return;
        }
        ++windowCount;
        if (now - windowStart >= 1000) {
            int naturalWidth = 0;
            int naturalHeight = 0;
            BVNGuestPresentationNaturalDrawableSize(&naturalWidth,
                                                    &naturalHeight);
            klog_fmt("Vulkan presentation swapchain rebuilt %u time(s) in "
                     "%u ms (%u total); layer natural drawable %dx%d. This is "
                     "the VK_SUBOPTIMAL_KHR recreation storm, not real work",
                     windowCount, now - windowStart, totalCreations,
                     naturalWidth, naturalHeight);
            windowStart = now;
            windowCount = 0;
        }
        return;
    }
#endif

    klog_fmt("Vulkan swapchain %p registered to %s surface %p",
             swapchain, isPresentation ? "presentation" : "helper/unknown",
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

#ifdef BOXEDWINE_IOS
// Report the presented frame rate and how much host CPU produced it.
//
// "The frame rate is not good" cannot be acted on, and neither can any change
// made in response to it, until there is a number. This is that number, and it
// costs one getrusage and two comparisons every five seconds.
//
// Cores busy is the useful half. Emulating x86 on ARM is CPU-bound by nature,
// so a low frame rate at ~1 core says one guest thread is the bottleneck and
// the work is to make that thread's translated code faster; a low frame rate
// at several cores says the work is spread and the ceiling is throughput.
void bvnReportPresentRate(void) {
    static U32 windowStart = 0;
    static U32 frames = 0;
    static double lastCpuSeconds = 0.0;
    static uint64_t lastX11PatchCount = 0;

    ++frames;
    const U32 now = SDL_GetTicks();
    if (windowStart == 0) {
        windowStart = now;
        lastX11PatchCount = BVNGuestX11PatchCount();
        return;
    }
    const U32 elapsed = now - windowStart;
    if (elapsed < 5000) {
        return;
    }

    rusage usage = {};
    double cpuSeconds = 0.0;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        cpuSeconds = (double)usage.ru_utime.tv_sec +
                     (double)usage.ru_utime.tv_usec / 1000000.0 +
                     (double)usage.ru_stime.tv_sec +
                     (double)usage.ru_stime.tv_usec / 1000000.0;
    }
    const double wallSeconds = (double)elapsed / 1000.0;
    const double coresBusy = lastCpuSeconds > 0.0
        ? (cpuSeconds - lastCpuSeconds) / wallSeconds : 0.0;
    const uint64_t x11PatchCount = BVNGuestX11PatchCount();
    const uint64_t x11PatchDelta = x11PatchCount >= lastX11PatchCount
        ? x11PatchCount - lastX11PatchCount : 0;

    // How much of that window the fairness throttle took from the guest. If
    // this approaches the window length the mitigation is the bottleneck, not
    // the emulation - which is exactly the question Grisaia's 0.4 frames/sec
    // at 0.65 cores busy raised.
    static U64 lastThrottleUs = 0;
    static U64 lastThrottleCount = 0;
    static U64 lastGetrusage = 0;
    static U64 lastSchedYield = 0;
    const U64 throttleUs =
        bvnFairness::throttleMicroseconds.load(std::memory_order_relaxed);
    const U64 throttleCount =
        bvnFairness::throttleCount.load(std::memory_order_relaxed);
    const U64 getrusage =
        bvnFairness::getrusageCalls.load(std::memory_order_relaxed);
    const U64 schedYield =
        bvnFairness::schedYieldCalls.load(std::memory_order_relaxed);

    klog_fmt("iOS guest performance: %.1f Vulkan frames/sec and %.1f X11 "
             "patches/sec over %u ms; "
             "host CPU %.2f cores busy; fairness throttle %llu ms across "
             "%llu scheduling points; guest polled getrusage %llu and "
             "sched_yield %llu times",
             (double)frames / wallSeconds,
             (double)x11PatchDelta / wallSeconds, elapsed, coresBusy,
             (unsigned long long)((throttleUs - lastThrottleUs) / 1000),
             (unsigned long long)(throttleCount - lastThrottleCount),
             (unsigned long long)(getrusage - lastGetrusage),
             (unsigned long long)(schedYield - lastSchedYield));

    // What the guest spent inside the host's present path over the same
    // window. Build 72's thread snapshot showed the whole guest parked on the
    // X11 socket behind one thread inside vkQueuePresentKHR; this says how
    // much of the wall clock that call actually consumed, which separates
    // "the GPU is slow" from "the compositor is not returning a drawable".
    static U64 lastPresentUs = 0;
    static U64 lastPresentCalls = 0;
    static U64 lastAcquireUs = 0;
    const U64 presentUs =
        bvnHostPresent::presentMicroseconds.load(std::memory_order_relaxed);
    const U64 presentCalls =
        bvnHostPresent::presentCalls.load(std::memory_order_relaxed);
    const U64 acquireUs =
        bvnHostPresent::acquireMicroseconds.load(std::memory_order_relaxed);
    const U64 worstPresentUs =
        bvnHostPresent::presentWorstMicroseconds.exchange(
            0, std::memory_order_relaxed);
    const U64 worstAcquireUs =
        bvnHostPresent::acquireWorstMicroseconds.exchange(
            0, std::memory_order_relaxed);
    const U64 presentDeltaUs = presentUs - lastPresentUs;
    klog_fmt("iOS guest present path: %llu ms of %u ms inside "
             "vkQueuePresentKHR across %llu call(s), worst single call "
             "%llu ms; vkAcquireNextImageKHR %llu ms, worst %llu ms",
             (unsigned long long)(presentDeltaUs / 1000), elapsed,
             (unsigned long long)(presentCalls - lastPresentCalls),
             (unsigned long long)(worstPresentUs / 1000),
             (unsigned long long)((acquireUs - lastAcquireUs) / 1000),
             (unsigned long long)(worstAcquireUs / 1000));
    lastPresentUs = presentUs;
    lastPresentCalls = presentCalls;
    lastAcquireUs = acquireUs;

    // Do not take a full guest thread snapshot merely because Vulkan is idle.
    // Mixed-rendered visual novels intentionally advance static text through
    // X11 patches with almost no full presents. Build 84 mistook that healthy
    // state for a hang every 30 seconds; snapshotting every process stopped
    // the emulator long enough to produce recurring ~1 second present stalls.

    lastThrottleUs = throttleUs;
    lastThrottleCount = throttleCount;
    lastGetrusage = getrusage;
    lastSchedYield = schedYield;
    lastX11PatchCount = x11PatchCount;
    lastCpuSeconds = cpuSeconds;
    windowStart = now;
    frames = 0;
}
#endif

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
    if (presentation && (result == 0 || result == 1000001003)) {
        bvnReportPresentRate();
        BVNGuestPerformanceFramePresented();
        // WineD3D uses X11/GDI for partial COPY presents. Those patches sit
        // transparently above the last Vulkan frame. A sparse Vulkan present
        // is not necessarily newer than those asynchronously published GDI
        // patches: Grisaia drops to one Vulkan present every 2-5 seconds while
        // its text is still updating through X11, and clearing at that cadence
        // made the visible dialogue jump backwards until the next patch.
        // Clear only after two nearby Vulkan presents establish that the game
        // is actively animating again and the full-frame stream supersedes
        // the patch layer.
        static U32 previousSuccessfulPresent = 0;
        const U32 presentNow = SDL_GetTicks();
        const bool fullFrameStreamActive = previousSuccessfulPresent != 0 &&
            presentNow - previousSuccessfulPresent <= 250;
        previousSuccessfulPresent = presentNow;
        if (fullFrameStreamActive && BVNGuestTakeX11PatchClearRequest()) {
            DISPATCH_MAIN_THREAD_BLOCK_BEGIN
            BVNGuestClearX11Patches();
            DISPATCH_MAIN_THREAD_BLOCK_END
        }
    }
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
