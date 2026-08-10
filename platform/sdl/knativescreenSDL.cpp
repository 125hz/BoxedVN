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
#include "../../source/ui/mainui.h"
#include "../../source/sdl/startupArgs.h"

#include <SDL.h>
#include "sdlcallback.h"
#include "knativeinputSDL.h"
#include "knativescreenSDL.h"
#include "../../source/x11/x11.h"

#ifdef BOXEDWINE_IOS
extern "C" void BVNAttachGuestWindowToScene(void);
// Reports the rectangle the presenter actually placed the guest picture in, in
// window points. Returns false when it is filling the window, so a caller can
// never assume a letterbox that did not happen.
extern "C" bool BVNGuestPresentationContentRect(int* x, int* y, int* w, int* h);

namespace {

std::atomic<size_t> gIOSJITAllocationCount{0};
KNativeScreenSDL* gIOSActiveScreen = nullptr;

struct BVNPixelGlyph {
    char character;
    U8 rows[7];
};

// A deliberately tiny font keeps the startup/status UI inside SDL, where it
// is rendered by the same Metal-backed renderer as Wine. This is the subset
// needed by the guest loading screen and keyboard button.
constexpr BVNPixelGlyph kBVNPixelGlyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},
};

const U8* bvnGlyphRows(char character) {
    for (const BVNPixelGlyph& glyph : kBVNPixelGlyphs) {
        if (glyph.character == character) {
            return glyph.rows;
        }
    }
    return nullptr;
}

int bvnPixelTextWidth(const char* text, int scale) {
    return text && text[0] ? ((int)strlen(text) * 6 - 1) * scale : 0;
}

void bvnDrawPixelText(SDL_Renderer* renderer, const char* text, int x, int y,
                      int scale, U8 red, U8 green, U8 blue) {
    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
    for (const char* cursor = text; cursor && *cursor; ++cursor, x += 6 * scale) {
        const U8* rows = bvnGlyphRows(*cursor);
        if (!rows) {
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if (rows[row] & (1 << (4 - column))) {
                    SDL_Rect pixel = {x + column * scale, y + row * scale,
                                      scale, scale};
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
    }
}

void bvnDrawCenteredPixelText(SDL_Renderer* renderer, const char* text, int y,
                              int scale, int width, U8 red, U8 green, U8 blue) {
    bvnDrawPixelText(renderer, text,
                     (width - bvnPixelTextWidth(text, scale)) / 2, y, scale,
                     red, green, blue);
}

} // namespace

extern "C" void BVNGuestLoadingUpdateJITProgress(size_t allocationCount) {
    gIOSJITAllocationCount.store(allocationCount, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The bridge used by the UIKit guest overlay (ios/runtime/src/BVNGuestOverlay.mm).
//
// The overlay is a UIKit view, so it must not include SDL or Boxedwine
// headers; equally, the scancode numbers must not be duplicated on the UIKit
// side where they could silently drift from SDL's. So the overlay names keys
// the way SDL itself names them and resolves them here, through SDL.
// ---------------------------------------------------------------------------

// Returns SDL_SCANCODE_UNKNOWN (0) for a name SDL does not know, which the
// overlay logs at install time rather than discovering as a dead key later.
extern "C" U32 BVNGuestControlsScancodeForName(const char* name) {
    if (!name || !name[0]) {
        return (U32)SDL_SCANCODE_UNKNOWN;
    }
    return (U32)SDL_GetScancodeFromName(name);
}

// Runs on the main thread, from inside UIKit's touch delivery, which itself
// runs inside SDL_PollEvent's run-loop pump - exactly the context the SDL-drawn
// keyboard already injected keys from.
extern "C" void BVNGuestControlsSendKey(U32 scancode, bool down) {
    if (!gIOSActiveScreen || scancode == (U32)SDL_SCANCODE_UNKNOWN) {
        return;
    }
    gIOSActiveScreen->input->key(scancode, 0, down ? 1 : 0);
}

// A right click, from the overlay's two-finger tap. The coordinates are in
// the Metal view's bounds, which since build 65 are the guest resolution, so
// they go through the same transform as a real pointer event.
//
// Boxedwine numbers its buttons 0 = left, 1 = right, 2 = middle (see
// getMouseButtonFromEvent), which is not SDL's numbering.
extern "C" void BVNGuestControlsSendRightClick(int x, int y) {
    if (!gIOSActiveScreen) {
        return;
    }
    const KNativeInputSDLPtr& input = gIOSActiveScreen->input;
    klog_fmt("iOS overlay right click at window %d,%d -> guest %d,%d",
             x, y, input->xFromScreen(x), input->yFromScreen(y));
    input->mouseButton(1, 1, x, y);
    input->mouseButton(0, 1, x, y);
}

// The window changed shape (rotation, or the presenter re-letterboxed). The
// pointer transform is re-derived from the presenter's measured rectangle;
// it is never predicted. See refreshIOSGuestPointerTransform.
extern "C" void BVNGuestPresentationGeometryChanged(void) {
    if (gIOSActiveScreen) {
        gIOSActiveScreen->refreshIOSGuestPointerTransform();
    }
}

// SDL asks its view controller for the supported orientations on every UIKit
// rotation query, and answers from SDL_HINT_ORIENTATIONS when it is set. With
// no hint it derives the answer from the guest window's shape: an 800x600
// window is wider than it is tall, so SDL reports landscape-only and UIKit
// never even offers portrait. The app delegate's own mask is ANDed with this
// one, so unlocking rotation has to change both.
extern "C" void BVNGuestControlsSetRotationHint(bool allowPortrait) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS,
                allowPortrait ? "LandscapeLeft LandscapeRight Portrait"
                              : "LandscapeLeft LandscapeRight");
}

#endif

KNativeScreenSDL::KNativeScreenSDL(U32 cx, U32 cy, U32 bpp, int scaleX, int scaleY, const BString& scaleQuality, U32 fullScreen, U32 vsync) {
    input = std::make_shared<KNativeInputSDL>(cx, cy, scaleX, scaleY);
    this->bpp = bpp;
    this->scaleQuality = scaleQuality;
    this->fullScreen = fullScreen;
    this->vsync = vsync;

    this->defaultScreenWidth = cx;
    this->defaultScreenHeight = cy;
    this->defaultScreenBpp = bpp;

#ifdef BOXEDWINE_IOS
    gIOSActiveScreen = this;
#endif
    recreateMainWindow();
}

KNativeScreenSDL::~KNativeScreenSDL() {
    destroyMainWindow();
#ifdef BOXEDWINE_IOS
    if (gIOSActiveScreen == this) {
        gIOSActiveScreen = nullptr;
    }
#endif
}

KNativeInputPtr KNativeScreenSDL::getInput() {
    return input;
}

void KNativeScreenSDL::setScreenSize(U32 cx, U32 cy) {
    input->setScreenSize(cx, cy);

#ifdef BOXEDWINE_IOS
    // UIKit always expands an SDL window to the device's full bounds, even
    // when SDL_CreateWindow was given the guest resolution. Keep the guest
    // coordinate space logical so SDL letterboxes it instead of independently
    // stretching X and Y to a portrait (or otherwise differently shaped)
    // device. SDL also applies this transform to pointer events, keeping taps
    // aligned with the image.
    if (renderer && SDL_RenderSetLogicalSize(renderer, cx, cy) != 0) {
        klog_fmt("SDL_RenderSetLogicalSize failed after a mode change: %s",
                 SDL_GetError());
    }

    // SDL's UIKit window is always the actual device/window geometry. Do not
    // run Boxedwine's desktop-window scaling below as well: that would first
    // squash 800x600 into the portrait point size with unrelated X/Y scales,
    // then feed the already-distorted coordinates through SDL's logical-size
    // transform. The renderer alone owns presentation scaling on iOS.
    if (renderer) {
        input->scaleX = 100;
        input->scaleY = 100;
        input->scaleXOffset = 0;
        input->scaleYOffset = 0;
        return;
    }

    // No renderer means the guest is presenting through Vulkan instead, and
    // SDL_RenderSetLogicalSize above never ran - so nothing maps pointer
    // events from window space into guest space.
    refreshIOSGuestPointerTransform();
    return;
#endif

    // If full screen, then we just have to change the scale
    if (fullScreen != FULLSCREEN_NOTSET) {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) != 0) {
            if (fullScreen == FULLSCREEN_STRETCH) {
                input->scaleX = dm.w * 100 / cx;
                input->scaleY = dm.h * 100 / cy;
            } else if (fullScreen == FULLSCREEN_ASPECT) {
                input->scaleX = dm.w * 100 / cx;
                input->scaleY = dm.h * 100 / cy;
                input->scaleXOffset = 0;
                input->scaleYOffset = 0;
                if (input->scaleY > input->scaleX) {
                    input->scaleY = input->scaleX;
                    input->scaleYOffset = (dm.h - cy * input->scaleY / 100) / 2;
                } else if (input->scaleX > input->scaleY) {
                    input->scaleX = input->scaleY;
                    input->scaleXOffset = (dm.w - cx * input->scaleX / 100) / 2;
                }
            }
        }
    } else {
        if (input->scaleX != 100) {
            cx = cx * input->scaleX / 100;
            cy = cy * input->scaleY / 100;
        }
        if (window) {
            DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
                SDL_SetWindowSize(window, cx, cy);
            DISPATCH_MAIN_THREAD_BLOCK_END
        }
    }
}

U32 KNativeScreenSDL::screenWidth() {
    return input->width;
}

U32 KNativeScreenSDL::screenHeight() {
    return input->height;
}

U32 KNativeScreenSDL::screenBpp() {
    return this->bpp;
}

U32 KNativeScreenSDL::screenRate() {
    if (KSystem::videoOption == VIDEO_NO_WINDOW) {
        return 60;
    }
    SDL_DisplayMode DM;
    SDL_GetCurrentDisplayMode(0, &DM);
    return DM.refresh_rate;
}

void KNativeScreenSDL::setTitle(const BString& title) {
    if (window) {
        SDL_SetWindowTitle(window, title.c_str());
    }
}

void KNativeScreenSDL::getPos(S32& x, S32& y) {
    if (!window) {
        x = input->lastX;
        y = input->lastY;
    } else {
        SDL_GetWindowPosition(window, &x, &y);
    }
}

U32 KNativeScreenSDL::getLastUpdateTime() {
    return lastUpdateTime;
}

void KNativeScreenSDL::showWindow(bool show) {
    if (show == visible) {
        return;
    }
    if (!isMainthread()) {
        DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
            showWindow(show);
        DISPATCH_MAIN_THREAD_BLOCK_END
    } else {
        showOnDraw = false;
        if (!show) {
            SDL_HideWindow(window);
            visible = false;            
        } else {
            if (KSystem::videoOption == VIDEO_NORMAL) {
                SDL_ShowWindow(window);
                SDL_RaiseWindow(window);
            }
            visible = true;
#if !defined(BOXEDWINE_DISABLE_UI) && !defined(__TEST) && defined(BOXEDWINE_UI_LAUNCH_IN_PROCESS)
            if (uiIsRunning()) {
                uiShutdown();
            }
#else
            if (!KSystem::showWindowTimestamp.isEmpty()) {
                BWriteFile file(KSystem::showWindowTimestamp);
                file.write("Showing Window");
            }
            klog("Showing Window");
#endif
        }
    }
}

void KNativeScreenSDL::clear() {
#ifdef BOXEDWINE_RECORDER
    if (Recorder::instance) {
        BOXEDWINE_MUTEX_LOCK(drawingMutex);
    }
    if (Recorder::instance || Player::instance) {
        U32 size = screenWidth() * screenHeight() * 4;
        if (recordBufferSize < size) {
            delete[] recordBuffer;
            recordBuffer = new U8[size];
            recordBufferSize = size;
        }
        if (bpp == 32) {
            U32* pixel = (U32*)recordBuffer;
            U32 len = screenWidth() * screenHeight();
            for (U32 i = 0; i < len; i++, pixel++) {
                *pixel = 165 | (110 << 8) | (58 << 16) | (255 << 24);
            }
        } else {
            memset(recordBuffer, 0, recordBufferSize);
        }
    }
#endif
    if (KSystem::videoOption != VIDEO_NO_WINDOW && renderer) {
        SDL_SetRenderDrawColor(renderer, 58, 110, 165, 255);
        SDL_RenderClear(renderer);
    }
}

void KNativeScreenSDL::putBitsOnWnd(U32 id, U8* bits, U32 bitsPerPixel, U32 srcPitch, S32 dstX, S32 dstY, U32 width, U32 height, U32* palette, bool isDirty) {
    if (!bitsPerPixel || !srcPitch) {
        return;
    }
#ifdef BOXEDWINE_IOS
    // XServer draws only mapped InputOutput descendants of its root through
    // this method. The first call is therefore a real visible Wine window,
    // which is the deterministic point at which startup has completed.
    if (guestLoadingVisible) {
        guestLoadingVisible = false;
        klog_fmt("iOS guest startup complete: first mapped X11 window 0x%x",
                 id);
    }
#endif
    WndCachePtr wnd;
    
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(wndCacheMutex);
        wnd = wndCache.get(id);
        if (!wnd) {
            wnd = std::make_shared<WndCache>();
            wndCache.set(id, wnd);
        }
    }

    U32 bpp = 32;
    U32 dstPitch = (width * ((bpp + 7) / 8) + 3) & ~3;

    if (wnd->sdlTexture && (wnd->sdlTextureHeight != height || wnd->sdlTextureWidth != width)) {
        SDL_DestroyTexture(wnd->sdlTexture);
        wnd->sdlTexture = nullptr;
        isDirty = true;
    }
    if (isDirty) {
        lastUpdateTime = KSystem::getMilliesSinceStart();
    }
    if (!wnd->sdlTexture) {
        if (KSystem::videoOption != VIDEO_NO_WINDOW && renderer) {
            wnd->sdlTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        }
        wnd->sdlTextureHeight = height;
        wnd->sdlTextureWidth = width;
    }
    if (isDirty && bitsPerPixel == 8) {
        wnd->ensureSize(dstPitch * height);
        for (U32 y = 0; y < height; y++) {
            U8* srcLine = bits + srcPitch * y;
            U32* dstLine = (U32*)(wnd->bits + dstPitch * y);
            for (U32 x = 0; x < width; x++, dstLine++, srcLine++) {
                *dstLine = palette[*srcLine];
            }
        }
        bits = wnd->bits;
    } else if (isDirty && bitsPerPixel == 16) {
        wnd->ensureSize(dstPitch * height);
        for (U32 y = 0; y < height; y++) {
            U16* srcLine = (U16*)(bits + srcPitch * y);
            U32* dstLine = (U32*)(wnd->bits + dstPitch * y);
            for (U32 x = 0; x < width; x++, dstLine++, srcLine++) {
                U32 r = (*srcLine & 0xF800) >> 11;
                U32 g = (*srcLine & 0x07E0) >> 5;
                U32 b = *srcLine & 0x001F;

                r = (r * 255) / 31;
                g = (g * 255) / 63;
                b = (b * 255) / 31;
                *dstLine = (r << 16) | (g << 8) | b;
            }
        }
        bits = wnd->bits;
    }
#ifdef BOXEDWINE_RECORDER
    if ((Recorder::instance || Player::instance) && screenBpp() > 8) {
        int wndWidth = width;
        int wndHeight = height;
        S32 top = dstY;
        S32 left = dstX;
        S32 srcTopAdjust = 0;
        S32 srcLeftAdjust = 0;
        S32 bytesPerPixel = (this->bpp + 7) / 8;
        S32 recorderPitch = (screenWidth() * ((this->bpp + 7) / 8) + 3) & ~3;

        if (top < 0) {
            wndHeight += top;
            top = 0;
        }
        if (top >= (S32)screenHeight()) {
            return;
        }
        if (left >= (S32)screenWidth()) {
            return;
        }
        if (left < 0) {
            srcLeftAdjust = -left;
            left = 0;
        }
        if (top + wndHeight > (S32)screenHeight()) {
            wndHeight = screenHeight() - top;
        }

        int pitch = (wndWidth * ((this->bpp + 7) / 8) + 3) & ~3;
        if (dstX + wndWidth > (S32)screenWidth()) {
            wndWidth = screenWidth() - left;
        }
        int copyPitch = (wndWidth * ((this->bpp + 7) / 8) + 3) & ~3;
        for (int y = 0; y < wndHeight; y++) {
            S32 offset = recorderPitch * (y + top) + (left * bytesPerPixel);
            if (offset<0 || offset + copyPitch>(S32)recordBufferSize || copyPitch < 0) {
                kpanic("script recorder overwrote memory when copying screen");
            }
            memcpy(recordBuffer + offset, bits + pitch * (y + srcTopAdjust) + (srcLeftAdjust * bytesPerPixel), copyPitch);
        }
    }    
#endif     

    if (KSystem::videoOption != VIDEO_NO_WINDOW && renderer) {
        if (isDirty) {
            SDL_UpdateTexture(wnd->sdlTexture, nullptr, bits, dstPitch);
        }

        SDL_Rect dstrect;
        dstrect.x = dstX * (int)input->scaleX / 100 + input->scaleXOffset;
        dstrect.y = dstY * (int)input->scaleY / 100 + input->scaleYOffset;
        dstrect.w = wnd->sdlTextureWidth * (int)input->scaleX / 100;
        dstrect.h = wnd->sdlTextureHeight * (int)input->scaleY / 100;
        SDL_RenderCopy(renderer, wnd->sdlTexture, nullptr, &dstrect);
    }
}

void KNativeScreenSDL::present() {
    if (KSystem::videoOption != VIDEO_NO_WINDOW) {
#ifdef BOXEDWINE_IOS
        // UIKit can rotate or resize the Metal view after this renderer was
        // created. SDL normally updates logical presentation from a window
        // event, but the guest owns UIKit's main thread and the drawable can
        // change before that queued event is consumed. Polling the drawable
        // here makes rendering and SDL's input transform agree immediately.
        syncIOSGuestPresentation("present");
#endif
        if (showOnDraw) {
            showWindow(true);
        }
#ifdef BOXEDWINE_IOS
        // The keyboard and menu are UIKit views now (BVNGuestOverlay.mm), not
        // SDL geometry: this renderer stops existing the moment the guest
        // switches to Vulkan, which is exactly when a player needs them.
        if (guestLoadingVisible) {
            drawIOSGuestLoading();
        }
#endif
        SDL_RenderPresent(renderer);
    }
    presented = true;
#ifdef BOXEDWINE_RECORDER
    if (Recorder::instance) {
        BOXEDWINE_MUTEX_UNLOCK(drawingMutex);
    }
#endif
}

bool KNativeScreenSDL::presentedSinceLastCheck() {
    bool result = presented;
    presented = false;
    return result;
}

void KNativeScreenSDL::clearTextureCache(U32 id) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(wndCacheMutex);
    wndCache.clear();
}

bool KNativeScreenSDL::canBltToScreen() {
    return additionalSDLWindowFlags == 0;
}

void KNativeScreenSDL::warpMouse(int x, int y) {
    DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
        if (window) {
            SDL_WarpMouseInWindow(window, x, y);
        }
    DISPATCH_MAIN_THREAD_BLOCK_END
}

bool KNativeScreenSDL::isVisible() {
    return visible;
}

#ifdef BOXEDWINE_IOS
void KNativeScreenSDL::syncIOSGuestPresentation(const char* reason) {
    if (!window || !renderer) {
        return;
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight) != 0) {
        klog_fmt("SDL_GetRendererOutputSize failed: %s", SDL_GetError());
        return;
    }
    if (outputWidth <= 0 || outputHeight <= 0 ||
        (outputWidth == guestOutputWidth &&
         outputHeight == guestOutputHeight)) {
        return;
    }

    if (SDL_RenderSetLogicalSize(renderer, input->width, input->height) != 0) {
        klog_fmt("SDL_RenderSetLogicalSize failed: %s", SDL_GetError());
        return;
    }

    guestOutputWidth = outputWidth;
    guestOutputHeight = outputHeight;

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    SDL_Rect viewport = {};
    SDL_RenderGetViewport(renderer, &viewport);
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    SDL_RenderGetScale(renderer, &scaleX, &scaleY);
    klog_fmt("iOS guest presentation updated (%s): window %dx%d, output "
             "%dx%d, viewport %d,%d %dx%d, scale %.3fx%.3f",
             reason ? reason : "unknown", windowWidth, windowHeight,
             outputWidth, outputHeight, viewport.x, viewport.y, viewport.w,
             viewport.h, scaleX, scaleY);
}

// Map window coordinates into guest coordinates.
//
// SDL's window IS the presentation surface, and that is the whole basis of
// this function. SDL_uikitviewcontroller.viewDidLayoutSubviews reports the SDL
// window size as its view's `bounds.size`, and touches arrive as
// -[UITouch locationInView:] against those same bounds. So whatever
// BVNApplyGuestPresentationAspect does to the Metal view, SDL's coordinate
// space follows it automatically, and the offset is ALWAYS zero: a letterbox
// offset lives in the view's position on screen, which UIKit has already
// removed by the time SDL sees a touch.
//
// Build 64 got this wrong in the most instructive way. It subtracted the
// measured letterbox offset from coordinates SDL had already made
// content-relative, so a tap on the left edge of an 800x600 picture letterboxed
// at x=169 mapped to guest x = (0 - 169) * 100/67 = -252. The left third of the
// picture was dead and everything else was displaced. The log said
// "window 536x402" - SDL's window really had become the content rect, and that
// was the tell.
//
// Since build 65 the presenter also sets the view's bounds to the guest
// resolution and does its scaling with a transform, so this normally resolves
// to a straight 100% identity. The arithmetic is kept rather than hard-coded
// because a guest that never got a fit applied still has to map correctly.
void KNativeScreenSDL::refreshIOSGuestPointerTransform() {
    if (!window || renderer) {
        // With a renderer, SDL's logical size owns the transform instead.
        return;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    const int cx = (int)input->width;
    const int cy = (int)input->height;
    if (windowWidth <= 0 || windowHeight <= 0 || cx <= 0 || cy <= 0) {
        input->scaleX = 100;
        input->scaleY = 100;
        input->scaleXOffset = 0;
        input->scaleYOffset = 0;
        return;
    }

    // KNativeInput's transform is a whole-number percentage, so it cannot
    // express every ratio exactly. Round to nearest rather than truncating:
    // truncation biases the scale down, and the error accumulates towards the
    // far edge of the picture, which is where a visual novel puts its menu.
    input->scaleX = (U32)((windowWidth * 100 + cx / 2) / cx);
    input->scaleY = (U32)((windowHeight * 100 + cy / 2) / cy);
    input->scaleXOffset = 0;
    input->scaleYOffset = 0;

    // Cross-check, never a source of truth: the presenter's measured on-screen
    // rectangle. Its SIZE should match SDL's window once the view's bounds are
    // the guest resolution; its ORIGIN is the letterbox offset on screen, which
    // must not appear in this transform. A mismatch means SDL and the presenter
    // have diverged, which is the failure mode worth naming out loud.
    int contentX = 0;
    int contentY = 0;
    int contentWidth = 0;
    int contentHeight = 0;
    const bool letterboxed =
        BVNGuestPresentationContentRect(&contentX, &contentY,
                                        &contentWidth, &contentHeight);

    klog_fmt("iOS Vulkan pointer mapping: SDL window %dx%d, guest %dx%d, "
             "scale %u%%x%u%%, offset 0,0%s; presenter shows it at %dx%d "
             "on screen at %d,%d",
             windowWidth, windowHeight, cx, cy, input->scaleX, input->scaleY,
             windowWidth == cx && windowHeight == cy ? " (1:1)" : "",
             letterboxed ? contentWidth : windowWidth,
             letterboxed ? contentHeight : windowHeight,
             letterboxed ? contentX : 0, letterboxed ? contentY : 0);
}

void KNativeScreenSDL::drawIOSGuestLoading() {
    if (!renderer) {
        return;
    }

    const int width = (int)input->width;
    const int height = (int)input->height;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 14, 19, 29, 255);
    SDL_RenderClear(renderer);

    const int centerY = height / 2;
    bvnDrawCenteredPixelText(renderer, "STARTING WINE", centerY - 116, 5,
                             width, 245, 247, 250);

    // This activity glyph makes the surface clearly intentional. Wine's
    // pre-main-loop startup blocks SDL event processing, so it must not imply
    // smooth animation during that phase.
    const int phase = (SDL_GetTicks() / 160) % 8;
    static const int spinnerX[8] = {0, 18, 26, 18, 0, -18, -26, -18};
    static const int spinnerY[8] = {-26, -18, 0, 18, 26, 18, 0, -18};
    for (int index = 0; index < 8; ++index) {
        const U8 brightness = index == phase ? 245 : 78;
        SDL_SetRenderDrawColor(renderer, 56, 132, brightness, 255);
        SDL_Rect dot = {width / 2 + spinnerX[index] - 5,
                        centerY - 23 + spinnerY[index] - 5, 10, 10};
        SDL_RenderFillRect(renderer, &dot);
    }

    bvnDrawCenteredPixelText(renderer, "THE APP IS WORKING", centerY + 40, 3,
                             width, 185, 196, 214);
    bvnDrawCenteredPixelText(renderer,
                             "TRANSLATING X86 CODE PLEASE WAIT",
                             centerY + 78, 2, width, 134, 148, 170);

    char progress[64];
    const size_t allocationCount =
        gIOSJITAllocationCount.load(std::memory_order_relaxed);
    if (allocationCount) {
        snprintf(progress, sizeof(progress), "JIT CODE BLOCKS %zu",
                 allocationCount);
    } else {
        snprintf(progress, sizeof(progress), "WINE IS LOADING");
    }
    bvnDrawCenteredPixelText(renderer, progress, centerY + 112, 2, width,
                             80, 190, 112);
}

#endif

bool KNativeScreenSDL::clipboardIsTextAvailable() {
    return SDL_HasClipboardText() != SDL_FALSE;
}

BString KNativeScreenSDL::clipboardGetText() {
    char* result = SDL_GetClipboardText();
    if (!result) {
        return BString::empty;
    }
    BString text = BString::copy(result);
    SDL_free(result);
    return text;
}

void KNativeScreenSDL::clipboardSetText(const char* text) {
    SDL_SetClipboardText(text);
}

#ifdef BOXEDWINE_RECORDER

void KNativeScreenSDL::startRecorderScreenShot() {
    BOXEDWINE_MUTEX_LOCK(drawingMutex);
    SDL_Surface* src = SDL_GetWindowSurface(window);
    SDL_Rect r = {};

    r.x = 0;
    r.y = 0;
    r.w = src->w;
    r.h = src->h;

    screenCopyTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, src->w, src->h);
    U32 len = src->w * src->h * src->format->BytesPerPixel;
    U8* pixels = new unsigned char[len];

    if (!SDL_RenderReadPixels(renderer, &src->clip_rect, src->format->format, pixels, src->w * src->format->BytesPerPixel)) {
        SDL_UpdateTexture(screenCopyTexture, nullptr, pixels, src->w * src->format->BytesPerPixel);
    }
    delete[] pixels;
}

void KNativeScreenSDL::finishRecorderScreenShot() {
    SDL_Surface* dst = SDL_GetWindowSurface(window);
    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = dst->w;
    rect.h = dst->h;

    SDL_RenderCopy(renderer, screenCopyTexture, nullptr, &rect);
    SDL_RenderPresent(renderer);

    SDL_DestroyTexture(screenCopyTexture);
    screenCopyTexture = nullptr;
    BOXEDWINE_MUTEX_UNLOCK(drawingMutex);
}

void KNativeScreenSDL::drawRectOnPushedSurfaceAndDisplay(U32 x, U32 y, U32 w, U32 h, U8 r, U8 g, U8 b, U8 a) {
    SDL_Surface* dst = SDL_GetWindowSurface(window);
    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = dst->w;
    rect.h = dst->h;

    SDL_RenderCopy(renderer, screenCopyTexture, nullptr, &rect);

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    SDL_RenderPresent(renderer);
}

#endif

bool KNativeScreenSDL::internalScreenShot(const BString& filepath, SDL_Rect* rect, U8* buffer, U32 bufferlen) {
#ifdef BOXEDWINE_RECORDER
    if (!recordBuffer) {
        if (renderer && bpp == 32) {
            U32 pitch = screenWidth() * 4;
            U32 size = pitch * screenHeight();
            if (recordBufferSize < size) {
                delete[] recordBuffer;
                recordBuffer = new U8[size];
                recordBufferSize = size;
            }
            if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, recordBuffer, pitch)) {
                if (filepath.length()) {
                    klog_fmt("failed to save screenshot, %s, SDL_RenderReadPixels failed: %s", filepath.c_str(), SDL_GetError());
                }
                return false;
            }
        } else {
            if (filepath.length()) {
                klog_fmt("failed to save screenshot, %s, because recorderBuffer was NULL", filepath.c_str());
            }
            return false;
        }
    }
    if (bpp == 8) {
        klog("KNativeScreenSDL::internalScreenShot 8 bit screen shots not supported");
        return false;
    }
    U8* pixels = nullptr;
    SDL_Surface* s = nullptr;
    U32 rMask = 0;
    U32 gMask = 0;
    U32 bMask = 0;

    if (bpp == 32) {
        rMask = 0x00FF0000;
        gMask = 0x0000FF00;
        bMask = 0x000000FF;
    } else if (bpp == 16) {
        rMask = 0xF800;
        gMask = 0x07E0;
        bMask = 0x001F;
    } else {
        kpanic_fmt("Unhandled bpp for screen shot: %d", bpp);
    }
    if (rect) {
        int inPitch = (screenWidth() * ((bpp + 7) / 8) + 3) & ~3;
        int outPitch = (rect->w * ((bpp + 7) / 8) + 3) & ~3;
        U32 bytesPerPixel = (bpp + 7) / 8;
        U32 len = outPitch * rect->h;

        if (!buffer) {
            pixels = new unsigned char[len];
            buffer = pixels;
        } else if (bufferlen < len) {
            return false;
        }

        for (int y = 0; y < rect->h; y++) {
            memcpy(buffer + y * outPitch, recordBuffer + (y + rect->y) * inPitch + (rect->x * bytesPerPixel), outPitch);
        }
        s = SDL_CreateRGBSurfaceFrom(buffer, rect->w, rect->h, bpp, outPitch, rMask, gMask, bMask, 0);
        if (!pixels && bpp <= 16) { // buffer was passed in and needs to be filled
            SDL_PixelFormat* format = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);

            SDL_Surface* tmp = SDL_ConvertSurface(s, format, 0);
            SDL_FreeFormat(format);

            outPitch = (rect->w * ((32 + 7) / 8) + 3) & ~3;
            int outLen = outPitch * rect->h;
            memcpy(buffer, tmp->pixels, outLen);
            SDL_FreeSurface(tmp);
        }
    } else {
        int pitch = (screenWidth() * ((bpp + 7) / 8) + 3) & ~3;

        s = SDL_CreateRGBSurfaceFrom(recordBuffer, screenWidth(), screenHeight(), bpp, pitch, rMask, gMask, bMask, 0);
        if (buffer) {
            int outPitch = (screenWidth() * ((32 + 7) / 8) + 3) & ~3;
            U32 outLen = outPitch * screenHeight();

            if (bufferlen < outLen) {
                return false;
            }
            if (bpp > 16) {
                memcpy(buffer, recordBuffer, outLen);
            } else {
                SDL_PixelFormat* format = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);                

                SDL_Surface* tmp = SDL_ConvertSurface(s, format, 0);
                SDL_FreeFormat(format);
                memcpy(buffer, tmp->pixels, outLen);
                SDL_FreeSurface(tmp);
            }
        }
    }

    if (!s) {
        klog_fmt("sdlScreenshot: %s", SDL_GetError());
        if (pixels) {
            delete[] pixels;
        }
        return false;
    }
    if (filepath.length()) {
        SDL_SaveBMP(s, filepath.c_str());
    }
    if (s) {
        SDL_FreeSurface(s);
    }
    if (pixels) {
        delete[] pixels;
    }
    return true;
#else
    return false;
#endif
}

bool KNativeScreenSDL::partialScreenShot(const BString& filepath, U32 x, U32 y, U32 w, U32 h, U8* buffer, U32 bufferlen) {
    SDL_Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return internalScreenShot(filepath, &r, buffer, bufferlen);
}

bool KNativeScreenSDL::screenShot(const BString& filepath, U8* buffer, U32 bufferlen) {
    return internalScreenShot(filepath, nullptr, buffer, bufferlen);
}

bool KNativeScreenSDL::saveBmp(const BString& filepath, U8* buffer, U32 bpp, U32 w, U32 h) {
    U32 rMask = 0;
    U32 gMask = 0;
    U32 bMask = 0;

    if (bpp == 32) {
        rMask = 0x00FF0000;
        gMask = 0x0000FF00;
        bMask = 0x000000FF;
    } else if (bpp == 16) {
        rMask = 0xF800;
        gMask = 0x07E0;
        bMask = 0x001F;
    } else {
        kpanic_fmt("Unhandled bpp for screen shot: %d", bpp);
    }

    SDL_Surface* s = SDL_CreateRGBSurfaceFrom(buffer, w, h, bpp, w * 4, rMask, gMask, bMask, 0);
    if (!s) {
        return false;
    }
    if (filepath.length()) {
        SDL_SaveBMP(s, filepath.c_str());
    }
    SDL_FreeSurface(s);
    return true;
}

void KNativeScreenSDL::buildCursor(KThread* thread, const std::shared_ptr<XCursor>& cursor, U32 pixelsAddress, U32 width, U32 height, S32 xHot, S32 yHot) {
    if (KSystem::videoOption == VIDEO_NO_WINDOW) {
        return;
    }
    U8* buffer = thread->memory->lockReadOnlyMemory(pixelsAddress, width * height * 4);
    SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(buffer, width, height, 32, width * 4, 0xff0000, 0xff00, 0xff, 0xff000000);
    SDL_Cursor* sdlCursor = SDL_CreateColorCursor(surface, xHot, yHot);

    thread->memory->unlockMemory(buffer);
    SDL_FreeSurface(surface);

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(cursorsMutex);
    SDL_Cursor* prev = cursors.get(cursor->id);
    if (prev) {
        SDL_FreeCursor(prev);
    }
    cursors.set(cursor->id, sdlCursor);
}

void KNativeScreenSDL::setCursor(const std::shared_ptr<XCursor>& cursor) {
    if (KSystem::videoOption == VIDEO_NO_WINDOW) {
        return;
    }
    SDL_Cursor* sdlCursor;

    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(cursorsMutex);
        sdlCursor = cursors.get(cursor->id);
    }
    if (!sdlCursor) {
        switch (cursor->shape) {
        case 0:
            if (!KSystem::disableHideCursor && cursor->fg.red == 0 && cursor->fg.blue == 0 && cursor->fg.green == 0 && cursor->bg.red == 0 && cursor->bg.green == 0 && cursor->bg.blue == 0) {
                sdlCursor = nullptr;
            } else {
                sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            }
            break;
        case 22: // XC_center_ptr
            // :TODO:
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            break;
        case 52: // XC_fleur
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
            break;
        case 60: // XC_hand2
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
            break;
        case 64: // XC_icon
            // :TODO:
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
            break;
        case 68: // XC_left_ptr
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            break;
        case 88: // XC_pirate
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NO);
            break;
        case 108: // XC_sb_h_double_arrow
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
            break;
        case 116: // XC_sb_v_double_arrow
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
            break;
        case 130: // XC_tcross
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
            break;
        case 132: // XC_top_left_arrow
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            break;
        case 134: // XC_top_left_corner
        case 136: // XC_top_right_corner
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
            break;
        case 150: // XC_watch
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAITARROW);
            break;
        case 152: // XC_xterm
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
            break;    
        case 1000: // left_ptr_watch
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAITARROW);
            break;
        case 92: // XC_question_arrow
            // :TODO:
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            break;
        default:
            klog_fmt("KNativeScreenSDL::setCursor cursor shape not defined for %d", cursor->shape);
            sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            break;
        }
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(cursorsMutex);
        cursors.set(cursor->id, sdlCursor);
    }
    DISPATCH_MAIN_THREAD_BLOCK_BEGIN
        if (sdlCursor) {
            SDL_ShowCursor(1);
            SDL_SetCursor(sdlCursor);
        } else {
            SDL_ShowCursor(0);
        }
    DISPATCH_MAIN_THREAD_BLOCK_END
}

void KNativeScreenSDL::destroyTextureCache() {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(wndCacheMutex);
    wndCache.clear();
}

void KNativeScreenSDL::destroyMainWindow() {
#ifdef BOXEDWINE_IOS
    guestLoadingVisible = false;
    guestOutputWidth = 0;
    guestOutputHeight = 0;
    if (SDL_IsTextInputActive() == SDL_TRUE) {
        SDL_StopTextInput();
    }
#endif
    destroyTextureCache();

    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
}

void KNativeScreenSDL::recreateMainWindow() {
    if (KSystem::videoOption != VIDEO_NO_WINDOW) {
        destroyMainWindow();
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaleQuality.c_str());

#ifdef BOXEDWINE_IOS
        // UIKit settles the scene in landscape before this SDL window exists,
        // and keeps it landscape for the session. Matching that policy in
        // SDL prevents its view controller from advertising a live portrait
        // transition that Boxedwine cannot safely service while boxedmain
        // owns the main thread.
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        input->scaleX = 100;
        input->scaleY = 100;
        input->scaleXOffset = 0;
        input->scaleYOffset = 0;
#endif

        int cx = input->width * input->scaleX / 100;
        int cy = input->height * input->scaleY / 100;
        int flags = SDL_WINDOW_HIDDEN | additionalSDLWindowFlags;
#ifdef BOXEDWINE_IOS
        // UIKit otherwise creates a one-pixel-per-point Metal drawable
        // (402x874 on this 3x phone), which is visibly blurry when expanded
        // to the physical display. RESIZABLE ensures orientation-driven view
        // geometry is treated as a live window-size change by SDL.
        //
        // BORDERLESS is how SDL is told this is a full-screen game surface:
        // SDL_uikitviewcontroller answers -prefersStatusBarHidden and
        // -preferredScreenEdgesDeferringSystemGestures from exactly these two
        // flags. Without it the clock and the "< StikDebug" return breadcrumb
        // are drawn over the top of the picture, which is what they were doing
        // once the letterbox made the guest fill the screen's full height.
        flags |= SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE |
                 SDL_WINDOW_BORDERLESS;
#endif
        
        visible = false;

#ifndef BOXEDWINE_IOS
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) != 0) {
            SDL_Log("SDL_GetDesktopDisplayMode failed: %s", SDL_GetError());
            fullScreen = FULLSCREEN_NOTSET;
        } else if (dm.w <= 0 || dm.h <= 0) {
            SDL_Log("SDL_GetDesktopDisplayMode returned an invalid size: %dx%d", dm.w, dm.h);
            fullScreen = FULLSCREEN_NOTSET;
        } else {
            if (fullScreen == FULLSCREEN_STRETCH) {
                cx = dm.w;
                cy = dm.h;
                flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
                input->scaleX = dm.w * 100 / input->width;
                input->scaleY = dm.h * 100 / input->height;
            } else if (fullScreen == FULLSCREEN_ASPECT) {
                cx = dm.w;
                cy = dm.h;
                flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
                input->scaleX = dm.w * 100 / input->width;
                input->scaleY = dm.h * 100 / input->height;
                input->scaleXOffset = 0;
                input->scaleYOffset = 0;
                if (input->scaleY > input->scaleX) {
                    input->scaleY = input->scaleX;
                    input->scaleYOffset = (dm.h - input->height * input->scaleY / 100) / 2;
                } else if (input->scaleX > input->scaleY) {
                    input->scaleX = input->scaleY;
                    input->scaleXOffset = (dm.w - input->width * input->scaleX / 100) / 2;
                }
            } else if (input->width == (U32)dm.w && input->height == (U32)dm.h) {
                flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            }
            if (cx > dm.w || cy > dm.h) {
                cx = dm.w;
                cy = dm.h;
                input->scaleX = dm.w * 100 / input->width;
                input->scaleY = dm.h * 100 / input->height;
                flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            }
        }
#endif

        window = SDL_CreateWindow("BoxedWine", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, cx, cy, flags);
        if (!window) {
            klog_fmt("SDL_CreateWindow failed: %s", SDL_GetError());
        }
#ifdef BOXEDWINE_IOS
        if (window) {
            // SDL 2.32 still creates its UIWindow with the legacy
            // initWithFrame: path. On scene-based iOS that window has no
            // UIWindowScene, which is enough to render Metal content but not
            // enough for UIKit-owned services such as the software keyboard.
            BVNAttachGuestWindowToScene();
        }
#endif
        if (!(flags & SDL_WINDOW_VULKAN)) {
#if defined(BOXEDWINE_LINUX) || defined(__EMSCRIPTEN__)
            // NVidia drivers need this
            flags = SDL_RENDERER_SOFTWARE;
#else
            flags = SDL_RENDERER_ACCELERATED;
#endif
            if (this->vsync != VSYNC_DISABLED) {
                flags |= SDL_RENDERER_PRESENTVSYNC;
            }
            renderer = SDL_CreateRenderer(window, -1, flags);
            if (!renderer) {
                klog("Failed to create SDL accelerated renderer, will try software");
                flags &= ~SDL_RENDERER_ACCELERATED;
                flags |= SDL_RENDERER_SOFTWARE;
                renderer = SDL_CreateRenderer(window, -1, flags);
            }
#ifdef BOXEDWINE_IOS
            if (renderer) {
                // SDL's UIKit backend intentionally ignores the requested
                // window size and fills the screen. Without a logical size,
                // its native view stretches (for example) an 800x600 guest to
                // the whole portrait phone, which is the tall, narrow output
                // seen on device. Logical-size letterboxing preserves aspect
                // ratio and installs SDL's matching input-coordinate mapping.
                syncIOSGuestPresentation("create");
            }
#endif
        }
#ifdef BOXEDWINE_IOS
        if (window && renderer) {
            guestLoadingVisible = true;
            gIOSJITAllocationCount.store(0, std::memory_order_relaxed);

            // recreateMainWindow runs before SDL's event loop begins. Calling
            // showWindow() here can enter sdlDispatch() when Boxedwine's SDL
            // main-thread identity has not been established yet, then wait
            // forever for an event loop that cannot service the callback.
            // This path is already executing on UIKit's main queue, so show
            // the native SDL window directly for this one pre-loop operation.
            if (KSystem::videoOption == VIDEO_NORMAL) {
                SDL_ShowWindow(window);
                SDL_RaiseWindow(window);
            }
            visible = true;
            showOnDraw = false;
            klog("iOS guest window shown directly for pre-loop startup");

            // A Metal drawable presented while SDL's UIWindow is hidden may
            // be discarded. Commit the labeled loading frame after showing
            // the window so Wine startup is not an unexplained black surface.
            drawIOSGuestLoading();
            SDL_RenderPresent(renderer);
            klog("iOS guest loading screen presented");
        }
#endif
    }
}
