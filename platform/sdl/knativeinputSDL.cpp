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
#include <SDL.h>
#include "../../source/x11/x11.h"
#include "sdlcallback.h"
#include "devinput.h"
#include "knativeinputSDL.h"
#include "knativesystem.h"
#include "kdspaudio.h"

#ifdef BOXEDWINE_IOS
// Implemented in ios/runtime/src/BVNAppDelegate.mm and
// platform/sdl/knativescreenSDL.cpp respectively.
extern "C" bool BVNSyncGuestPresentationGeometry(void);
extern "C" void BVNGuestPresentationGeometryChanged(void);
extern "C" void BVNGuestOverlayApplyPendingState(void);
#endif

U32 sdlCustomEvent;

#ifdef BOXEDWINE_MULTI_THREADED
static bool handleSdlCallbackEvent(SDL_Event* event) {
    if (event->type != sdlCustomEvent) {
        return false;
    }

    SdlCallback* callback = (SdlCallback*)event->user.data1;
    if (!callback || !callback->pfn) {
        // A malformed/stale callback event must never turn into an uncaught
        // std::bad_function_call on the application's main thread.
        klog("Ignored an empty SDL main-thread callback event");
        return true;
    }

    const U32 result = (U32)callback->pfn();
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(callback->cond);
    callback->result = result;
    callback->completed = true;
    BOXEDWINE_CONDITION_SIGNAL(callback->cond);
    return true;
}
#endif

KNativeInputSDL::KNativeInputSDL(U32 cx, U32 cy, int scaleX, int scaleY) {
    if (!sdlCustomEvent) {
        sdlCustomEvent = SDL_RegisterEvents(1);
    }
    
    this->width = cx;
    this->height = cy;
    this->scaleX = scaleX;
    this->scaleY = scaleY;
    this->scaleXOffset = 0;
    this->scaleYOffset = 0;    
}

void KNativeInputSDL::runOnUiThread(std::function<void()> callback) {
    DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN
        callback();
    DISPATCH_MAIN_THREAD_BLOCK_END
}


void KNativeInputSDL::setScreenSize(U32 cx, U32 cy) {
    width = cx;
    height = cy;
}

U32 KNativeInputSDL::screenWidth() {
    return width;
}

U32 KNativeInputSDL::screenHeight() {
    return height;
}

bool KNativeInputSDL::mouseMove(int x, int y, bool relative) {
    XServer* server = XServer::getServer(true);

    x = xFromScreen(x);
    y = yFromScreen(y);

#ifdef BOXEDWINE_IOS
    // Remember where the pointer now is, so getMousePos can answer with it.
    // See the field declaration in knativeinputSDL.h: SDL_GetMouseState is
    // blind to the overlay's injected touches.
    if (!relative) {
        injectedX = x;
        injectedY = y;
        hasInjectedPointer = true;
    }
#endif

#ifdef BOXEDWINE_RECORDER
    if (Player::instance) {
        lastX = x;
        lastY = y;
    }
#endif

    if (server) {
        server->mouseMove(x, y, relative);
        return true;
    }
    return false;
}

bool KNativeInputSDL::mouseWheel(int amount, int x, int y) {
    x = xFromScreen(x);
    y = yFromScreen(y);

    checkMousePos(x, y, true);

    XServer* server = XServer::getServer(true);
    if (server) {
        U32 btn = amount > 0 ? 4 : 5;
        server->mouseButton(btn, x, y, true);
        server->mouseButton(btn, x, y, false);
        return true;
    }
    return false;
}

bool KNativeInputSDL::mouseButton(U32 down, U32 button, int x, int y) {
    if (KSystem::enableSoundAfterMouseClick) {
        KSystem::enableSoundAfterMouseClick = false;
        KSystem::soundEnabled = true;
        KDspAudio::iterateOpenAudio([](KDspAudioPtr& audio) {
            audio->soundEnabled();
            });
    }
    x = xFromScreen(x);
    y = yFromScreen(y);

    checkMousePos(x, y, true);

#ifdef BOXEDWINE_IOS
    injectedX = x;
    injectedY = y;
    hasInjectedPointer = true;
#endif

    XServer* server = XServer::getServer(true);
    if (server) {
        U32 btn = button + 1;
        if (btn == 2) {
            btn = 3;
        } else if (btn == 3) {
            btn = 2;
        } else if (btn == 4) {
            btn = 8;
        } else if (btn == 5) {
            btn = 9;
        }
        server->mouseButton(btn, x, y, down ? true : false);
        return true;
    }
    return false;
}

bool KNativeInputSDL::getMousePos(int* x, int* y, bool allowWarp) {
#ifdef BOXEDWINE_RECORDER
    if (Player::instance) {
        *x = lastX;
        *y = lastY;
        return checkMousePos(*x, *y, false);
    }
#endif
#ifdef BOXEDWINE_IOS
    // Already guest coordinates - mouseMove/mouseButton put them through
    // xFromScreen on the way in, so they must not go through it again.
    if (hasInjectedPointer) {
        *x = injectedX;
        *y = injectedY;
        if (XServer::getServer(true) && XServer::getServer()->fakeFullScreenWnd) {
            XServer::getServer()->fakeFullScreenWnd->screenToWindow(*x, *y);
        }
        return checkMousePos(*x, *y, false);
    }
#endif
    SDL_GetMouseState(x, y);

    *x = xFromScreen(*x);
    *y = yFromScreen(*y);

    if (XServer::getServer(true) && XServer::getServer()->fakeFullScreenWnd) {
        XServer::getServer()->fakeFullScreenWnd->screenToWindow(*x, *y);
    }
    return checkMousePos(*x, *y, false);
}

void KNativeInputSDL::setMousePos(int x, int y) {
#ifdef BOXEDWINE_RECORDER
    if (Player::instance) {
        lastX = x;
        lastY = y;
        return;
    }
#endif

#ifdef BOXEDWINE_IOS
    // A guest that warps its own cursor - SetCursorPos, and every full-screen
    // engine that recentres the pointer - must be believed afterwards, or
    // getMousePos would keep answering with the last place the player touched.
    injectedX = x;
    injectedY = y;
    hasInjectedPointer = true;
#endif

    if (XServer::getServer(true) && XServer::getServer()->fakeFullScreenWnd) {
        XServer::getServer()->fakeFullScreenWnd->screenToWindow(x, y);
    }
    x = xToScreen(x);
    y = yToScreen(y);

    KNativeSystem::warpMouse(x, y);
}

int KNativeInputSDL::xToScreen(int x) {
    return x * (int)scaleX / 100 + (int)scaleXOffset;
}

int KNativeInputSDL::xFromScreen(int x) {
    return (x - (int)scaleXOffset) * 100 / (int)scaleX;
}

int KNativeInputSDL::yToScreen(int y) {
    return y * (int)scaleY / 100 + (int)scaleYOffset;
}

int KNativeInputSDL::yFromScreen(int y) {
    return (y - (int)scaleYOffset) * 100 / (int)scaleY;
}

bool KNativeInputSDL::checkMousePos(int& x, int& y, bool allowWarp) {
    bool warp = false;
    if (x < 0) {
        x = 0;
        warp = true;
    }
    if (x >= (int)width) {
        x = (int)width - 1;
        warp = true;
    }
    if (y < 0) {
        y = 0;
        warp = true;
    }
    if (y >= (int)height) {
        y = (int)height;
        warp = true;
    }
    if (allowWarp && warp) {
        int scaledX = xToScreen(x);
        int scaledY = yToScreen(y);
        KNativeSystem::getScreen()->warpMouse(scaledX, scaledY);
    }
    return allowWarp || !warp;
}

#ifndef KMOD_SCROLL
#define KMOD_SCROLL KMOD_RESERVED
#endif

U32 KNativeInputSDL::getInputModifiers() {
#ifdef BOXEDWINE_RECORDER
    if (Player::instance) {
        return Player::instance->currentInputModifiers;
    }
#endif
    int x, y;
    unsigned int result = SDL_GetMouseState(&x, &y);
    U32 modifiers = 0;
    if (result & SDL_BUTTON_LMASK) {
        modifiers |= NATIVE_LEFT_BUTTON_MASK;
    }
    if (result & SDL_BUTTON_RMASK) {
        modifiers |= NATIVE_RIGHT_BUTTON_MASK;
    }
    if (result & SDL_BUTTON_MMASK) {
        modifiers |= NATIVE_MIDDLE_BUTTON_MASK;
    }
    if (result & SDL_BUTTON_X1MASK) {
        modifiers |= NATIVE_BUTTON_4_MASK;
    }
    if (result & SDL_BUTTON_X2MASK) {
        modifiers |= NATIVE_BUTTON_5_MASK;
    }
    SDL_Keymod mods = SDL_GetModState();
    if (mods & KMOD_SHIFT) {
        modifiers |= NATIVE_SHIFT_MASK;
    }
    if (mods & KMOD_CAPS) {
        modifiers |= NATIVE_CAPS_MASK;
    }
    if (mods & KMOD_CTRL) {
        modifiers |= NATIVE_CONTROL_MASK;
    }
    if (mods & KMOD_ALT) {
        modifiers |= NATIVE_CONTROL_MASK;
    }
    if (mods & KMOD_NUM) {
        modifiers |= NATIVE_NUM_MASK;
    }
    if (mods & KMOD_SCROLL) {
        modifiers |= NATIVE_SCROLL_MASK;
    }
    return modifiers;
}

bool KNativeInputSDL::key(U32 sdlScanCode, U32 key, U32 down) {
    XServer* server = XServer::getServer(true);
    if (server) {
        U32 x11Key = XKeyboard::sdl2x11(sdlScanCode);
        if (x11Key) {
            server->key(x11Key, down ? true : false);
        }
        return true;
    }
    return false;
}

bool KNativeInputSDL::waitForEvent(U32 ms) {
    SDL_Event e = { 0 };
    if (SDL_WaitEventTimeout(&e, ms) == 1) {
#ifdef BOXEDWINE_MULTI_THREADED
        if (handleSdlCallbackEvent(&e)) {
            return true;
        }
#endif
        handlSdlEvent(&e);
        return true;
    }
    return false;
}

bool KNativeInputSDL::processEvents() {
    SDL_Event e = {};

#ifdef BOXEDWINE_IOS
    // Keep the guest picture matched to the window from the emulator's own
    // loop rather than from a UIKit layout callback.
    //
    // Build 65 re-fitted only from the overlay's -layoutSubviews, and on
    // device that callback stopped arriving after the first rotation: turning
    // back to landscape produced no re-fit at all, leaving the picture with
    // its portrait geometry, off-centre and untappable. This loop runs on the
    // main thread for as long as the guest does, so it cannot be skipped the
    // same way. Two struct comparisons every 200 ms, and it only does work
    // when the window has genuinely changed shape.
    static U32 lastGeometryPoll = 0;
    const U32 nowTicks = SDL_GetTicks();
    if (nowTicks - lastGeometryPoll >= 200) {
        lastGeometryPoll = nowTicks;
        if (BVNSyncGuestPresentationGeometry()) {
            BVNGuestPresentationGeometryChanged();
        }
        // Also keeps the overlay above SDL's views, and applies anything the
        // X server's thread asked for. Unlike the geometry check this must run
        // even with no Vulkan surface: the software-rendered Wine desktop is
        // exactly where the overlay was being buried.
        BVNGuestOverlayApplyPendingState();
    }
#endif

    while (SDL_PollEvent(&e) == 1) {
#ifdef BOXEDWINE_MULTI_THREADED
        if (handleSdlCallbackEvent(&e)) {
            continue;
        } else
#endif
            if (!handlSdlEvent(&e)) {
                return false;
            }
    }
    return true;
}

#ifndef SDLK_NUMLOCK
#define SDLK_NUMLOCK SDL_SCANCODE_NUMLOCKCLEAR
#endif
#ifndef SDLK_SCROLLOCK
#define SDLK_SCROLLOCK SDLK_SCROLLLOCK
#endif

static U32 translate(U32 key) {
    switch (key) {
    case SDLK_ESCAPE:
        return K_KEY_ESC;
    case SDLK_1:
        return K_KEY_1;
    case SDLK_2:
        return K_KEY_2;
    case SDLK_3:
        return K_KEY_3;
    case SDLK_4:
        return K_KEY_4;
    case SDLK_5:
        return K_KEY_5;
    case SDLK_6:
        return K_KEY_6;
    case SDLK_7:
        return K_KEY_7;
    case SDLK_8:
        return K_KEY_8;
    case SDLK_9:
        return K_KEY_9;
    case SDLK_0:
        return K_KEY_0;
    case SDLK_MINUS:
        return K_KEY_MINUS;
    case SDLK_EQUALS:
        return K_KEY_EQUAL;
    case SDLK_BACKSPACE:
        return K_KEY_BACKSPACE;
    case SDLK_TAB:
        return K_KEY_TAB;
    case SDLK_q:
        return K_KEY_Q;
    case SDLK_w:
        return K_KEY_W;
    case SDLK_e:
        return K_KEY_E;
    case SDLK_r:
        return K_KEY_R;
    case SDLK_t:
        return K_KEY_T;
    case SDLK_y:
        return K_KEY_Y;
    case SDLK_u:
        return K_KEY_U;
    case SDLK_i:
        return K_KEY_I;
    case SDLK_o:
        return K_KEY_O;
    case SDLK_p:
        return K_KEY_P;
    case SDLK_LEFTBRACKET:
        return K_KEY_LEFTBRACE;
    case SDLK_RIGHTBRACKET:
        return K_KEY_RIGHTBRACE;
    case SDLK_RETURN:
        return K_KEY_ENTER;
    case SDLK_LCTRL:
        return K_KEY_LEFTCTRL;
    case SDLK_RCTRL:
        return K_KEY_RIGHTCTRL;
    case SDLK_a:
        return K_KEY_A;
    case SDLK_s:
        return K_KEY_S;
    case SDLK_d:
        return K_KEY_D;
    case SDLK_f:
        return K_KEY_F;
    case SDLK_g:
        return K_KEY_G;
    case SDLK_h:
        return K_KEY_H;
    case SDLK_j:
        return K_KEY_J;
    case SDLK_k:
        return K_KEY_K;
    case SDLK_l:
        return K_KEY_L;
    case SDLK_SEMICOLON:
        return K_KEY_SEMICOLON;
    case SDLK_QUOTE:
        return K_KEY_APOSTROPHE;
    case SDLK_BACKQUOTE:
        return K_KEY_GRAVE;
    case SDLK_LSHIFT:
        return K_KEY_LEFTSHIFT;
    case SDLK_RSHIFT:
        return K_KEY_RIGHTSHIFT;
    case SDLK_BACKSLASH:
        return K_KEY_BACKSLASH;
    case SDLK_z:
        return K_KEY_Z;
    case SDLK_x:
        return K_KEY_X;
    case SDLK_c:
        return K_KEY_C;
    case SDLK_v:
        return K_KEY_V;
    case SDLK_b:
        return K_KEY_B;
    case SDLK_n:
        return K_KEY_N;
    case SDLK_m:
        return K_KEY_M;
    case SDLK_COMMA:
        return K_KEY_COMMA;
    case SDLK_PERIOD:
        return K_KEY_DOT;
    case SDLK_SLASH:
        return K_KEY_SLASH;
    case SDLK_LALT:
        return K_KEY_LEFTALT;
    case SDLK_RALT:
        return K_KEY_RIGHTALT;
    case SDLK_SPACE:
        return K_KEY_SPACE;
    case SDLK_CAPSLOCK:
        return K_KEY_CAPSLOCK;
    case SDLK_F1:
        return K_KEY_F1;
    case SDLK_F2:
        return K_KEY_F2;
    case SDLK_F3:
        return K_KEY_F3;
    case SDLK_F4:
        return K_KEY_F4;
    case SDLK_F5:
        return K_KEY_F5;
    case SDLK_F6:
        return K_KEY_F6;
    case SDLK_F7:
        return K_KEY_F7;
    case SDLK_F8:
        return K_KEY_F8;
    case SDLK_F9:
        return K_KEY_F9;
    case SDLK_F10:
        return K_KEY_F10;
    case SDLK_NUMLOCK:
        return K_KEY_NUMLOCK;
    case SDLK_SCROLLOCK:
        return K_KEY_SCROLLLOCK;
    case SDLK_F11:
        return K_KEY_F11;
    case SDLK_F12:
        return K_KEY_F12;
    case SDLK_HOME:
        return K_KEY_HOME;
    case SDLK_UP:
        return K_KEY_UP;
    case SDLK_PAGEUP:
        return K_KEY_PAGEUP;
    case SDLK_LEFT:
        return K_KEY_LEFT;
    case SDLK_RIGHT:
        return K_KEY_RIGHT;
    case SDLK_END:
        return K_KEY_END;
    case SDLK_DOWN:
        return K_KEY_DOWN;
    case SDLK_PAGEDOWN:
        return K_KEY_PAGEDOWN;
    case SDLK_INSERT:
        return K_KEY_INSERT;
    case SDLK_DELETE:
        return K_KEY_DELETE;
    case SDLK_PAUSE:
        return K_KEY_PAUSE;
    default:
        kdebug("Unhandled key: %d", key);
        return 0;
    }
}

static int getMouseButtonFromEvent(SDL_Event* e) {
    if (e->button.button == SDL_BUTTON_LEFT) {
        return 0;
    } else if (e->button.button == SDL_BUTTON_MIDDLE) {
        return 2;
    } else if (e->button.button == SDL_BUTTON_RIGHT) {
        return 1;
    }
    return 0;
}

#ifdef BOXEDWINE_IOS
static bool bvnIOSCharacterKey(char character, SDL_Keycode& keycode,
                               bool& shift) {
    shift = false;
    if (character >= 'a' && character <= 'z') {
        keycode = (SDL_Keycode)character;
        return true;
    }
    if (character >= 'A' && character <= 'Z') {
        keycode = (SDL_Keycode)(character - 'A' + 'a');
        shift = true;
        return true;
    }
    if (character >= '0' && character <= '9') {
        keycode = (SDL_Keycode)character;
        return true;
    }

    switch (character) {
    case ' ': keycode = SDLK_SPACE; return true;
    case '-': keycode = SDLK_MINUS; return true;
    case '_': keycode = SDLK_MINUS; shift = true; return true;
    case '=': keycode = SDLK_EQUALS; return true;
    case '+': keycode = SDLK_EQUALS; shift = true; return true;
    case '[': keycode = SDLK_LEFTBRACKET; return true;
    case '{': keycode = SDLK_LEFTBRACKET; shift = true; return true;
    case ']': keycode = SDLK_RIGHTBRACKET; return true;
    case '}': keycode = SDLK_RIGHTBRACKET; shift = true; return true;
    case '\\': keycode = SDLK_BACKSLASH; return true;
    case '|': keycode = SDLK_BACKSLASH; shift = true; return true;
    case ';': keycode = SDLK_SEMICOLON; return true;
    case ':': keycode = SDLK_SEMICOLON; shift = true; return true;
    case '\'': keycode = SDLK_QUOTE; return true;
    case '"': keycode = SDLK_QUOTE; shift = true; return true;
    case ',': keycode = SDLK_COMMA; return true;
    case '<': keycode = SDLK_COMMA; shift = true; return true;
    case '.': keycode = SDLK_PERIOD; return true;
    case '>': keycode = SDLK_PERIOD; shift = true; return true;
    case '/': keycode = SDLK_SLASH; return true;
    case '?': keycode = SDLK_SLASH; shift = true; return true;
    case '`': keycode = SDLK_BACKQUOTE; return true;
    case '~': keycode = SDLK_BACKQUOTE; shift = true; return true;
    case '!': keycode = SDLK_1; shift = true; return true;
    case '@': keycode = SDLK_2; shift = true; return true;
    case '#': keycode = SDLK_3; shift = true; return true;
    case '$': keycode = SDLK_4; shift = true; return true;
    case '%': keycode = SDLK_5; shift = true; return true;
    case '^': keycode = SDLK_6; shift = true; return true;
    case '&': keycode = SDLK_7; shift = true; return true;
    case '*': keycode = SDLK_8; shift = true; return true;
    case '(': keycode = SDLK_9; shift = true; return true;
    case ')': keycode = SDLK_0; shift = true; return true;
    default: return false;
    }
}
#endif

// return true to continue processing for custom handlers
//
// should only call on main thread
void KNativeInputSDL::processCustomEvents(std::function<bool(bool isKeyDown, int key, bool isF11)> onKey, std::function<bool(bool isButtonDown, int button, int x, int y)> onMouseButton, std::function<bool(int x, int y)> onMouseMove) {
    SDL_Event e = {};

#ifdef _DEBUG
    if (!isMainthread()) {
        kpanic("KNativeInputSDL::processCustomEvents should only be called on main thread");
    }
#endif
    while (SDL_WaitEvent(&e)) {
        if (e.type == SDL_KEYUP) {
            if (!onKey(false, e.key.keysym.sym, e.key.keysym.sym == SDLK_F11)) {
                return;
            }
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
            if (!onMouseButton(true, getMouseButtonFromEvent(&e), e.motion.x, e.motion.y)) {
                return;
            }
        } else if (e.type == SDL_MOUSEBUTTONUP) {
            if (!onMouseButton(false, getMouseButtonFromEvent(&e), e.motion.x, e.motion.y)) {
                return;
            }
        } else if (e.type == SDL_MOUSEMOTION) {
            if (!onMouseMove(e.motion.x, e.motion.y)) {
                return;
            }
        }
#ifdef BOXEDWINE_MULTI_THREADED
        else if (e.type == sdlCustomEvent) {
            handleSdlCallbackEvent(&e);
        }
#endif
    }
}

bool KNativeInputSDL::handlSdlEvent(SDL_Event* e) {
#ifdef BOXEDWINE_RECORDER
    if (Player::instance) {
        if (e->type == SDL_QUIT) {
            return false;
        }
        return true;
    }
#endif    
    if (e->type == SDL_QUIT) {
        KThread::setCurrentThread(nullptr);
#ifdef BOXEDWINE_IOS
        // Upstream kills pid 10 - the first process the session created - and
        // returns true, letting that death cascade through the rest.
        //
        // On iOS pid 10 is whatever the launcher chain left behind, and by the
        // time the player taps "Quit to library" it is normally an empty husk:
        // build 72's thread snapshot shows `pid=000A name=BootMenu.exe
        // threads=0` for Grisaia, and desktop mode's pid 10 is the /bin/wine
        // stub that exits as soon as it has spawned explorer. Killing a
        // process with no threads succeeds, so this returned true, the loop
        // kept running, and quitting did nothing at all - the desktop log has
        // "shutdown requested; posting SDL_QUIT" at 00:17:42 followed by the
        // player still tapping around the guest at 00:18:11.
        //
        // Quit means quit. Tear down every process that still has threads and
        // then end the loop whatever happened.
        if (!KSystem::shutingDown) {
            const std::vector<U32> ids = KSystem::getProcessIdsWithThreads();
            klog_fmt("iOS quit requested: terminating %u guest process(es)",
                     (U32)ids.size());
            for (U32 id : ids) {
                KProcessPtr process = KSystem::getProcess(id);
                if (process) {
                    process->killAllThreads();
                    KSystem::eraseProcess(id);
                }
            }
        }
        return false;
#else
        KProcessPtr p = KSystem::getProcess(10);
        if (p && !KSystem::shutingDown) {
            p->killAllThreads();
            KSystem::eraseProcess(p->id);
            return true;
        }
        return false;
#endif
    } else if (e->type == SDL_MOUSEMOTION) {
        BOXEDWINE_RECORDER_HANDLE_MOUSE_MOVE(e->motion.x, e->motion.y);
        if (!mouseMove(e->motion.x, e->motion.y, false)) {
            onMouseMove(e->motion.x, e->motion.y, false);
        }
    } else if (e->type == SDL_MOUSEBUTTONDOWN) {    
        U32 button = getMouseButtonFromEvent(e);
#ifdef BOXEDWINE_IOS
        // The in-game menu and keyboard are UIKit views above SDL's, so
        // UIKit hit-testing already claimed their touches before SDL saw
        // them; there is nothing left here to intercept.
        klog_fmt("iOS SDL mouse down: button %u at logical %d,%d",
                 button, e->button.x, e->button.y);
#endif
        BOXEDWINE_RECORDER_HANDLE_MOUSE_BUTTON_DOWN(button, e->motion.x, e->motion.y);
        if (!mouseButton(1, button, e->motion.x, e->motion.y)) {
            onMouseButtonDown(button);
        }
    } else if (e->type == SDL_MOUSEBUTTONUP) {      
        U32 button = getMouseButtonFromEvent(e);
#ifdef BOXEDWINE_IOS
        klog_fmt("iOS SDL mouse up: button %u at logical %d,%d",
                 button, e->button.x, e->button.y);
#endif
        BOXEDWINE_RECORDER_HANDLE_MOUSE_BUTTON_UP(button, e->motion.x, e->motion.y);
        if (!mouseButton(0, button, e->motion.x, e->motion.y)) {
            onMouseButtonUp(button);
        }
    } else if (e->type == SDL_MOUSEWHEEL) {
        // Handle up/down mouse wheel movements
        int x, y;
        SDL_GetMouseState(&x, &y);
        if (!mouseWheel(e->wheel.y * 80, x, y)) {
            onMouseWheel(e->wheel.y);
        }
    } else if (e->type == SDL_KEYDOWN) {       
        if (!BOXEDWINE_RECORDER_HANDLE_KEY_DOWN(e->key.keysym.scancode, e->key.keysym.sym == SDLK_F11)) {
            if (e->key.keysym.sym == SDLK_SCROLLOCK) {
                KSystem::printStacks();
            } else if (!key(e->key.keysym.scancode, e->key.keysym.sym, 1)) {
                onKeyDown(translate(e->key.keysym.sym));
            }
        }
    } else if (e->type == SDL_KEYUP) {
        if (!BOXEDWINE_RECORDER_HANDLE_KEY_UP(e->key.keysym.scancode, e->key.keysym.sym == SDLK_F11)) {
            if (!key(e->key.keysym.scancode, e->key.keysym.sym, 0)) {
                onKeyUp(translate(e->key.keysym.sym));
            }
        }
#ifdef BOXEDWINE_IOS
    } else if (e->type == SDL_TEXTINPUT &&
               SDL_IsTextInputActive() == SDL_TRUE) {
        // SDL's UIKit software keyboard emits committed characters as
        // SDL_TEXTINPUT, whereas Boxedwine historically consumed only
        // physical SDL_KEYDOWN/UP events. Synthesize the matching X11 key
        // transitions for the ASCII input used by Notepad and game launchers.
        for (const char* cursor = e->text.text; *cursor; ++cursor) {
            SDL_Keycode keycode = SDLK_UNKNOWN;
            bool shift = false;
            if (!bvnIOSCharacterKey(*cursor, keycode, shift)) {
                continue;
            }
            const SDL_Scancode scancode = SDL_GetScancodeFromKey(keycode);
            if (scancode == SDL_SCANCODE_UNKNOWN) {
                continue;
            }
            if (shift) {
                key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, 1);
            }
            key(scancode, keycode, 1);
            key(scancode, keycode, 0);
            if (shift) {
                key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, 0);
            }
        }
#endif
    } else if (e->type == SDL_WINDOWEVENT) {
#ifdef BOXEDWINE_IOS
        if (e->window.event == SDL_WINDOWEVENT_RESIZED ||
            e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            klog_fmt("iOS SDL window %s: %dx%d",
                     e->window.event == SDL_WINDOWEVENT_RESIZED
                         ? "resized" : "size changed",
                     e->window.data1, e->window.data2);
            // SDL reports its window size as the Metal view's bounds, so this
            // event IS the presentation surface changing shape - and the
            // pointer transform is derived from that size. Build 66 computed
            // the transform immediately after the fit, before SDL had seen
            // the resize, and then never recomputed it: the log shows the
            // guest still mapping taps at 109%x67% while SDL was delivering
            // 800x600 coordinates.
            BVNGuestPresentationGeometryChanged();
        }
#endif
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            for (auto& callback : onFocusGained) {
                callback();
            }
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            for (auto& callback : onFocusLost) {
                callback();
            }
        }
    }
    return true;
}
