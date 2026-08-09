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

#ifndef __KVULKAN_H__
#define __KVULKAN_H__

class XWindow;

class KVulkan {
public:
    virtual ~KVulkan() {}
    virtual void* createVulkanSurface(const std::shared_ptr<XWindow>& wnd, void* instance) = 0;
    virtual void destroyVulkanSurface(void* surface) = 0;
    virtual bool isPresentationSurface(void* surface) = 0;
    // VkSurfaceKHR and VkSwapchainKHR are created by separate generated
    // bridge calls. Keep their relationship in the native presentation
    // backend so iOS can distinguish "a Metal view exists" from "the guest
    // actually presented a frame" without making UIKit understand Vulkan.
    virtual void registerVulkanSwapchain(void* swapchain, void* surface) = 0;
    virtual void destroyVulkanSwapchain(void* swapchain) = 0;
    virtual void acquireVulkanSwapchain(void* swapchain, int result,
                                        U32 imageIndex) = 0;
    virtual void submitVulkanWorkload(int result, U32 submitCount,
                                      const char* api) = 0;
    virtual void presentVulkanSwapchain(void* swapchain, int result) = 0;
    // An unhandled guest exception may leave its still-live Metal surface
    // above WineDbg's X11 window.  iOS backends can temporarily detach the
    // presentation layer so the diagnostic remains visible and interactive.
    virtual void revealX11ForDiagnostic(const std::shared_ptr<XWindow>& wnd) = 0;
};

typedef std::shared_ptr<KVulkan> KVulkanPtr;

#endif
