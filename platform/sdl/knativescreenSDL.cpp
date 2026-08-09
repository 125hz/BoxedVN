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

enum class BVNVirtualKeyAction {
    key,
    shift,
    hide,
};

struct BVNVirtualKey {
    const char* label;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
    BVNVirtualKeyAction action;
    int weight;
};

#define BVN_KEY(label, scan, code) \
    {label, scan, code, BVNVirtualKeyAction::key, 2}

constexpr BVNVirtualKey kBVNKeyboardRow0[] = {
    BVN_KEY("1", SDL_SCANCODE_1, SDLK_1),
    BVN_KEY("2", SDL_SCANCODE_2, SDLK_2),
    BVN_KEY("3", SDL_SCANCODE_3, SDLK_3),
    BVN_KEY("4", SDL_SCANCODE_4, SDLK_4),
    BVN_KEY("5", SDL_SCANCODE_5, SDLK_5),
    BVN_KEY("6", SDL_SCANCODE_6, SDLK_6),
    BVN_KEY("7", SDL_SCANCODE_7, SDLK_7),
    BVN_KEY("8", SDL_SCANCODE_8, SDLK_8),
    BVN_KEY("9", SDL_SCANCODE_9, SDLK_9),
    BVN_KEY("0", SDL_SCANCODE_0, SDLK_0),
};
constexpr BVNVirtualKey kBVNKeyboardRow1[] = {
    BVN_KEY("Q", SDL_SCANCODE_Q, SDLK_q),
    BVN_KEY("W", SDL_SCANCODE_W, SDLK_w),
    BVN_KEY("E", SDL_SCANCODE_E, SDLK_e),
    BVN_KEY("R", SDL_SCANCODE_R, SDLK_r),
    BVN_KEY("T", SDL_SCANCODE_T, SDLK_t),
    BVN_KEY("Y", SDL_SCANCODE_Y, SDLK_y),
    BVN_KEY("U", SDL_SCANCODE_U, SDLK_u),
    BVN_KEY("I", SDL_SCANCODE_I, SDLK_i),
    BVN_KEY("O", SDL_SCANCODE_O, SDLK_o),
    BVN_KEY("P", SDL_SCANCODE_P, SDLK_p),
};
constexpr BVNVirtualKey kBVNKeyboardRow2[] = {
    BVN_KEY("A", SDL_SCANCODE_A, SDLK_a),
    BVN_KEY("S", SDL_SCANCODE_S, SDLK_s),
    BVN_KEY("D", SDL_SCANCODE_D, SDLK_d),
    BVN_KEY("F", SDL_SCANCODE_F, SDLK_f),
    BVN_KEY("G", SDL_SCANCODE_G, SDLK_g),
    BVN_KEY("H", SDL_SCANCODE_H, SDLK_h),
    BVN_KEY("J", SDL_SCANCODE_J, SDLK_j),
    BVN_KEY("K", SDL_SCANCODE_K, SDLK_k),
    BVN_KEY("L", SDL_SCANCODE_L, SDLK_l),
    {"BACK", SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE,
     BVNVirtualKeyAction::key, 3},
};
constexpr BVNVirtualKey kBVNKeyboardRow3[] = {
    {"SHIFT", SDL_SCANCODE_UNKNOWN, SDLK_UNKNOWN,
     BVNVirtualKeyAction::shift, 3},
    BVN_KEY("Z", SDL_SCANCODE_Z, SDLK_z),
    BVN_KEY("X", SDL_SCANCODE_X, SDLK_x),
    BVN_KEY("C", SDL_SCANCODE_C, SDLK_c),
    BVN_KEY("V", SDL_SCANCODE_V, SDLK_v),
    BVN_KEY("B", SDL_SCANCODE_B, SDLK_b),
    BVN_KEY("N", SDL_SCANCODE_N, SDLK_n),
    BVN_KEY("M", SDL_SCANCODE_M, SDLK_m),
    {"ENTER", SDL_SCANCODE_RETURN, SDLK_RETURN,
     BVNVirtualKeyAction::key, 3},
};
constexpr BVNVirtualKey kBVNKeyboardRow4[] = {
    {"HIDE", SDL_SCANCODE_UNKNOWN, SDLK_UNKNOWN,
     BVNVirtualKeyAction::hide, 3},
    {"SPACE", SDL_SCANCODE_SPACE, SDLK_SPACE,
     BVNVirtualKeyAction::key, 10},
    {"LEFT", SDL_SCANCODE_LEFT, SDLK_LEFT,
     BVNVirtualKeyAction::key, 2},
    {"RIGHT", SDL_SCANCODE_RIGHT, SDLK_RIGHT,
     BVNVirtualKeyAction::key, 2},
    {"ENTER", SDL_SCANCODE_RETURN, SDLK_RETURN,
     BVNVirtualKeyAction::key, 3},
};

#undef BVN_KEY

template <typename Callback>
bool bvnForEachVirtualKey(int width, int height, Callback callback) {
    struct Row {
        const BVNVirtualKey* keys;
        size_t count;
    };
    constexpr Row rows[] = {
        {kBVNKeyboardRow0, sizeof(kBVNKeyboardRow0) / sizeof(BVNVirtualKey)},
        {kBVNKeyboardRow1, sizeof(kBVNKeyboardRow1) / sizeof(BVNVirtualKey)},
        {kBVNKeyboardRow2, sizeof(kBVNKeyboardRow2) / sizeof(BVNVirtualKey)},
        {kBVNKeyboardRow3, sizeof(kBVNKeyboardRow3) / sizeof(BVNVirtualKey)},
        {kBVNKeyboardRow4, sizeof(kBVNKeyboardRow4) / sizeof(BVNVirtualKey)},
    };
    constexpr int margin = 8;
    constexpr int gap = 4;
    constexpr int keyHeight = 44;
    const int keyboardHeight = margin * 2 +
        (int)(sizeof(rows) / sizeof(Row)) * keyHeight +
        ((int)(sizeof(rows) / sizeof(Row)) - 1) * gap;
    int rowY = height - keyboardHeight;

    for (const Row& row : rows) {
        int totalWeight = 0;
        for (size_t index = 0; index < row.count; ++index) {
            totalWeight += row.keys[index].weight;
        }
        const int usableWidth = width - margin * 2 -
            ((int)row.count - 1) * gap;
        int keyX = margin;
        int consumedWeight = 0;
        for (size_t index = 0; index < row.count; ++index) {
            consumedWeight += row.keys[index].weight;
            const int nextX = margin +
                usableWidth * consumedWeight / totalWeight +
                (int)index * gap;
            SDL_Rect rect = {keyX, rowY,
                             nextX - keyX, keyHeight};
            if (callback(row.keys[index], rect)) {
                return true;
            }
            keyX = nextX + gap;
        }
        rowY += keyHeight + gap;
    }
    return false;
}

} // namespace

extern "C" void BVNGuestLoadingUpdateJITProgress(size_t allocationCount) {
    gIOSJITAllocationCount.store(allocationCount, std::memory_order_relaxed);
}

extern "C" bool BVNGuestControlsHandleMouseButton(bool down, U32 button,
                                                    int x, int y) {
    return gIOSActiveScreen &&
           gIOSActiveScreen->handleGuestControlMouseButton(down, button, x, y);
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
    input->scaleX = 100;
    input->scaleY = 100;
    input->scaleXOffset = 0;
    input->scaleYOffset = 0;
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
        if (guestLoadingVisible) {
            drawIOSGuestLoading();
        } else if (guestVirtualKeyboardVisible) {
            drawIOSGuestVirtualKeyboard();
        } else {
            drawIOSGuestKeyboardButton();
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

SDL_Rect KNativeScreenSDL::guestKeyboardButtonRect() const {
    const int margin = 12;
    const int height = 48;
    const int width = 170;
    return {(int)input->width - width - margin,
            (int)input->height - height - margin, width, height};
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

void KNativeScreenSDL::drawIOSGuestKeyboardButton() {
    if (!renderer) {
        return;
    }

    const SDL_Rect button = guestKeyboardButtonRect();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 16, 25, 39, 222);
    SDL_RenderFillRect(renderer, &button);
    SDL_SetRenderDrawColor(renderer, 94, 234, 146, 255);
    SDL_RenderDrawRect(renderer, &button);
    bvnDrawPixelText(renderer, "KEYBOARD",
                     button.x + (button.w - bvnPixelTextWidth("KEYBOARD", 3)) / 2,
                     button.y + 14, 3, 238, 245, 255);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void KNativeScreenSDL::drawIOSGuestVirtualKeyboard() {
    if (!renderer) {
        return;
    }

    const int width = (int)input->width;
    const int height = (int)input->height;
    const int top = height - 252;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 8, 13, 22, 242);
    SDL_Rect background = {0, top, width, height - top};
    SDL_RenderFillRect(renderer, &background);

    bvnForEachVirtualKey(width, height,
        [this](const BVNVirtualKey& key, const SDL_Rect& rect) {
            const bool activeShift =
                key.action == BVNVirtualKeyAction::shift &&
                guestVirtualKeyboardShift;
            SDL_SetRenderDrawColor(renderer,
                                   activeShift ? 31 : 28,
                                   activeShift ? 108 : 42,
                                   activeShift ? 65 : 60,
                                   255);
            SDL_RenderFillRect(renderer, &rect);
            SDL_SetRenderDrawColor(renderer,
                                   activeShift ? 120 : 88,
                                   activeShift ? 255 : 105,
                                   activeShift ? 164 : 128,
                                   255);
            SDL_RenderDrawRect(renderer, &rect);
            const int textScale = strlen(key.label) <= 2 ? 3 : 2;
            bvnDrawPixelText(
                renderer, key.label,
                rect.x + (rect.w - bvnPixelTextWidth(key.label, textScale)) / 2,
                rect.y + (rect.h - 7 * textScale) / 2,
                textScale, 238, 245, 255);
            return false;
        });
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

bool KNativeScreenSDL::handleIOSGuestVirtualKeyboardTap(int x, int y) {
    bool handled = false;
    bvnForEachVirtualKey((int)input->width, (int)input->height,
        [this, x, y, &handled](const BVNVirtualKey& key,
                              const SDL_Rect& rect) {
            if (x < rect.x || y < rect.y ||
                x >= rect.x + rect.w || y >= rect.y + rect.h) {
                return false;
            }

            handled = true;
            if (key.action == BVNVirtualKeyAction::hide) {
                guestVirtualKeyboardVisible = false;
                guestVirtualKeyboardShift = false;
                klog("iOS guest built-in keyboard hidden");
                return true;
            }
            if (key.action == BVNVirtualKeyAction::shift) {
                guestVirtualKeyboardShift = !guestVirtualKeyboardShift;
                klog_fmt("iOS guest built-in keyboard shift %s",
                         guestVirtualKeyboardShift ? "on" : "off");
                return true;
            }

            if (guestVirtualKeyboardShift) {
                input->key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, 1);
            }
            input->key(key.scancode, key.keycode, 1);
            input->key(key.scancode, key.keycode, 0);
            if (guestVirtualKeyboardShift) {
                input->key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, 0);
                guestVirtualKeyboardShift = false;
            }
            klog_fmt("iOS guest built-in keyboard key %s", key.label);
            return true;
        });
    return handled;
}

bool KNativeScreenSDL::handleGuestControlMouseButton(bool down, U32 button,
                                                      int x, int y) {
    if (guestLoadingVisible || button != 0 || !window) {
        return false;
    }

    if (guestVirtualKeyboardVisible) {
        // The keyboard occupies the bottom 252 logical pixels. Consume both
        // halves of the click so a key never also clicks the Wine window.
        const bool inKeyboard = y >= (int)input->height - 252;
        if (down) {
            guestVirtualKeyboardPressed = inKeyboard;
            return inKeyboard;
        }
        if (guestVirtualKeyboardPressed) {
            guestVirtualKeyboardPressed = false;
            if (inKeyboard) {
                handleIOSGuestVirtualKeyboardTap(x, y);
            }
            if (guestVirtualKeyboardVisible) {
                drawIOSGuestVirtualKeyboard();
                SDL_RenderPresent(renderer);
            }
            return true;
        }
        return false;
    }

    const SDL_Rect rect = guestKeyboardButtonRect();
    const bool inside = x >= rect.x && y >= rect.y &&
                        x < rect.x + rect.w && y < rect.y + rect.h;
    if (down) {
        keyboardButtonPressed = inside;
        return inside;
    }
    if (!keyboardButtonPressed) {
        return false;
    }

    keyboardButtonPressed = false;
    if (inside) {
        guestVirtualKeyboardVisible = true;
        guestVirtualKeyboardShift = false;
        // This keyboard sends X11 key events directly. Starting SDL text
        // input also asks UIKit for its software keyboard; doing both made
        // the guest view resize/hide twice and caused the visible glitches.
        klog("iOS guest built-in keyboard shown");
        drawIOSGuestVirtualKeyboard();
        SDL_RenderPresent(renderer);
    }
    return true;
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
    keyboardButtonPressed = false;
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
        flags |= SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
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
            keyboardButtonPressed = false;
            guestVirtualKeyboardVisible = false;
            guestVirtualKeyboardShift = false;
            guestVirtualKeyboardPressed = false;
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
