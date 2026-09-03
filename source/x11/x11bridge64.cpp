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

// The x86-64 guest X11 bridge.
//
// The 32-bit guest libX11 pushes its arguments and executes int 0x9b;
// x11common.cpp reads them back as 32-bit stack words and writes 32-bit Xlib
// structures into guest memory the host allocated. A 64-bit Wine cannot use
// any of that: CPU64 decodes 0x9b as FWAIT, pointers and longs are eight
// bytes, and no host-side allocator exists for a 64-bit address space.
//
// So the 64-bit shim (tools/x11-64) owns every byte of guest memory. It
// hands the host caller-provided buffers through a private syscall carrying
// an operation index and an array of 64-bit arguments, and the host fills
// those buffers in the exact x86-64 Xlib layout after validating each range
// against the guest page table. Nothing here dereferences a guest pointer.
//
// The operations run against the same XServer, XWindow, DisplayData and
// XKeyboard objects the 32-bit path uses. The 32-bit path is untouched.

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "x11bridge64.h"
#include "x11.h"
#include "x11layout64.h"
#include "xpixmapformats.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "knativesystem.h"
#include "x11_bridge64_policy.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

namespace L = x11layout64;

// ---- Guest memory access ----------------------------------------------------

class GuestPageProbe : public boxedvn::X11Bridge64PageProbe {
public:
    explicit GuestPageProbe(KMemory64* memory) : memory(memory) {}
    bool accessible(uint64_t address, bool write) const override {
        const U32 flags = memory->getPageFlags(address >> K64_PAGE_SHIFT);
        if (!(flags & K64_PAGE_MAPPED) || !(flags & K64_PAGE_READ)) {
            return false;
        }
        return !write || (flags & K64_PAGE_WRITE);
    }
private:
    KMemory64* memory;
};

struct Call {
    CPU64* cpu = nullptr;
    KMemory64* memory = nullptr;
    KThread* thread = nullptr;
    U32 pid = 0;
    U64 op = 0;
    U64 count = 0;
    U64 args[BOXEDWINE_X64_X11_MAX_ARGS] = {};
    // Set when any guest range failed validation; the result becomes a
    // controlled fault rather than a partial success.
    bool faulted = false;

    U64 arg(U32 index) const {
        return index < count ? args[index] : 0;
    }
    S64 sarg(U32 index) const {
        return (S64)arg(index);
    }
    S32 iarg(U32 index) const {
        return (S32)(U32)arg(index);
    }
    void setArg(U32 index, U64 value) {
        if (index < BOXEDWINE_X64_X11_MAX_ARGS) {
            args[index] = value;
        }
    }
    bool readable(U64 address, U64 length) const {
        GuestPageProbe probe(memory);
        return boxedvn::x11Bridge64RangeAccessible(probe, address, length, false);
    }
    bool writable(U64 address, U64 length) const {
        GuestPageProbe probe(memory);
        return boxedvn::x11Bridge64RangeAccessible(probe, address, length, true);
    }
    bool read(U64 address, void* destination, U64 length) {
        if (length == 0) {
            return true;
        }
        if (!readable(address, length)) {
            faulted = true;
            return false;
        }
        memory->memcpyFromGuest(destination, address, length);
        return true;
    }
    bool write(U64 address, const void* source, U64 length) {
        if (length == 0) {
            return true;
        }
        if (!writable(address, length)) {
            faulted = true;
            return false;
        }
        memory->memcpyToGuest(address, source, length);
        return true;
    }
    bool write32(U64 address, U32 value) { return write(address, &value, 4); }
    bool write64(U64 address, U64 value) { return write(address, &value, 8); }
    bool read32(U64 address, U32& value) { return read(address, &value, 4); }
    bool read64(U64 address, U64& value) { return read(address, &value, 8); }

    // A NUL-terminated guest string, bounded, validated page by page.
    BString readString(U64 address, U64 maxLength = 64 * 1024) {
        std::string result;
        if (!address) {
            return BString();
        }
        while (result.size() < maxLength) {
            const U64 pageRemaining = K64_PAGE_SIZE - (address & K64_PAGE_MASK);
            const U64 chunkLength = pageRemaining < 256 ? pageRemaining : 256;
            char chunk[256];
            if (!read(address, chunk, chunkLength)) {
                return BString();
            }
            for (U64 i = 0; i < chunkLength; i++) {
                if (chunk[i] == 0) {
                    result.append(chunk, (size_t)i);
                    return BString::copy(result.c_str());
                }
            }
            result.append(chunk, (size_t)chunkLength);
            address += chunkLength;
        }
        return BString::copy(result.c_str());
    }

    // Variable-size results: the caller passes {buffer, capacity} in two
    // consecutive slots. When the capacity is short, the needed size is
    // written back into the capacity slot and E_BUFFER is returned so the
    // shim can allocate and call again.
    bool sizedResult(U32 bufferSlot, const void* source, U64 length, S64& status) {
        const U64 buffer = arg(bufferSlot);
        const U64 capacity = arg(bufferSlot + 1);
        setArg(bufferSlot + 1, length);
        if (!buffer || capacity < length) {
            status = BOXEDWINE_X64_X11_E_BUFFER;
            return false;
        }
        if (!write(buffer, source, length)) {
            status = BOXEDWINE_X64_X11_E_FAULT;
            return false;
        }
        return true;
    }
};

// ---- Bounded diagnostics ----------------------------------------------------

std::mutex& reportMutex() {
    static std::mutex mutex;
    return mutex;
}

// Which (process, marker) pairs have already been printed.
std::set<std::pair<U32, std::string>>& reportedMarkers() {
    static std::set<std::pair<U32, std::string>> markers;
    return markers;
}

bool firstTime(U32 pid, const std::string& key) {
    std::lock_guard<std::mutex> lock(reportMutex());
    std::set<std::pair<U32, std::string>>& markers = reportedMarkers();
    if (markers.size() > 4096) {
        return false;
    }
    return markers.insert(std::make_pair(pid, key)).second;
}

// The first call of each operation in each process, so a device log shows
// the sequence Wine's driver takes without printing every call.
void traceFirstCall(const Call& call, const char* name) {
    if (firstTime(call.pid, std::string("first:") + name)) {
        klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=%s first-call count=%u "
                 "a0=0x%llx a1=0x%llx a2=0x%llx",
                 call.pid, name, (unsigned)call.count,
                 (unsigned long long)call.arg(0), (unsigned long long)call.arg(1),
                 (unsigned long long)call.arg(2));
    }
}

void reportUnimplemented(U32 pid, const char* name, U64 index) {
    if (firstTime(pid, std::string("unimpl:") + name)) {
        klog_fmt("BOXEDWINE_X64_X11_UNIMPLEMENTED pid=%u op=%s index=%llu count=1",
                 pid, name, (unsigned long long)index);
    }
}

// Acceptance markers for the operations a device log is judged on. A small
// per-process budget keeps a busy window loop from flooding the log.
void reportResult(const Call& call, const char* name, S64 result, U64 detail) {
    static std::atomic<U32> budget {0};
    if (budget.fetch_add(1, std::memory_order_relaxed) >= 256) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=%s result=%lld window=0x%llx",
             call.pid, name, (long long)result, (unsigned long long)detail);
}

// ---- Display lookup ---------------------------------------------------------

DisplayDataPtr displayFor(Call& call, U64 displayAddress) {
    if (!displayAddress) {
        return nullptr;
    }
    U32 id = 0;
    if (!call.read(displayAddress + BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET, &id, 4)) {
        return nullptr;
    }
    DisplayDataPtr data = XServer::getServer()->getDisplayDataById(id);
    if (!data || data->displayAddress64 != displayAddress) {
        return nullptr;
    }
    return data;
}

// The guest address of the Visual whose id is `visualid`, found by walking
// the Screen/Depth/Visual chain the bridge wrote at open time.
U64 visualAddressFor(Call& call, U64 displayAddress, U32 visualid) {
    U64 screenAddress = 0;
    if (!call.read64(displayAddress + L::Display::screens, screenAddress) || !screenAddress) {
        return 0;
    }
    U32 ndepths = 0;
    U64 depthsAddress = 0;
    if (!call.read32(screenAddress + L::Screen::ndepths, ndepths) ||
        !call.read64(screenAddress + L::Screen::depths, depthsAddress)) {
        return 0;
    }
    if (ndepths > 64) {
        return 0;
    }
    for (U32 d = 0; d < ndepths; d++) {
        const U64 depthAddress = depthsAddress + (U64)d * L::Depth::size;
        U32 nvisuals = 0;
        U64 visualsAddress = 0;
        if (!call.read32(depthAddress + L::Depth::nvisuals, nvisuals) ||
            !call.read64(depthAddress + L::Depth::visuals, visualsAddress)) {
            return 0;
        }
        if (nvisuals > 256) {
            return 0;
        }
        for (U32 v = 0; v < nvisuals; v++) {
            const U64 visualAddress = visualsAddress + (U64)v * L::Visual::size;
            U64 id = 0;
            if (!call.read64(visualAddress + L::Visual::visualid, id)) {
                return 0;
            }
            if ((U32)id == visualid) {
                return visualAddress;
            }
        }
    }
    return 0;
}

U64 screenAddressFor(Call& call, U64 displayAddress) {
    U64 screenAddress = 0;
    call.read64(displayAddress + L::Display::screens, screenAddress);
    return screenAddress;
}

// ---- Event conversion -------------------------------------------------------

// Host events are stored in the IA-32 layout (see x11.h). Write the same
// event in the x86-64 layout. `display` is the guest Display pointer.
void writeEvent64(const XEvent& event, U64 display, U8* out) {
    using namespace L::XEvent;
    memset(out, 0, L::XEvent::size);
    L::put32(out, any::type, (U32)event.type);
    L::put64(out, any::serial, event.xany.serial);
    L::put32(out, any::send_event, (U32)event.xany.send_event);
    L::put64(out, any::display, display);
    switch (event.type) {
    case KeyPress:
    case KeyRelease:
        L::put64(out, any::window, event.xkey.window);
        L::put64(out, key::root, event.xkey.root);
        L::put64(out, key::subwindow, event.xkey.subwindow);
        L::put64(out, key::time, event.xkey.time);
        L::put32(out, key::x, (U32)event.xkey.x);
        L::put32(out, key::y, (U32)event.xkey.y);
        L::put32(out, key::x_root, (U32)event.xkey.x_root);
        L::put32(out, key::y_root, (U32)event.xkey.y_root);
        L::put32(out, key::state, event.xkey.state);
        L::put32(out, key::keycode, event.xkey.keycode);
        L::put32(out, key::same_screen, (U32)event.xkey.same_screen);
        break;
    case ButtonPress:
    case ButtonRelease:
        // The first 64-bit desktop run reported double-clicks that opened
        // nothing while single clicks worked. Wine derives its double-click
        // from consecutive button events' time, position and button, all
        // of which cross here; the first few are recorded so a stale time,
        // a moving position, or a missing release can be seen in the log.
        {
            static std::atomic<U32> reported {0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                klog_fmt("BOXEDWINE_X64_X11_BUTTON %s window=0x%llx time=%llu "
                         "x=%d y=%d root=%d,%d button=%u state=0x%x",
                         event.type == ButtonPress ? "press" : "release",
                         (unsigned long long)event.xbutton.window,
                         (unsigned long long)event.xbutton.time,
                         event.xbutton.x, event.xbutton.y,
                         event.xbutton.x_root, event.xbutton.y_root,
                         event.xbutton.button, event.xbutton.state);
            }
        }
        L::put64(out, any::window, event.xbutton.window);
        L::put64(out, key::root, event.xbutton.root);
        L::put64(out, key::subwindow, event.xbutton.subwindow);
        L::put64(out, key::time, event.xbutton.time);
        L::put32(out, key::x, (U32)event.xbutton.x);
        L::put32(out, key::y, (U32)event.xbutton.y);
        L::put32(out, key::x_root, (U32)event.xbutton.x_root);
        L::put32(out, key::y_root, (U32)event.xbutton.y_root);
        L::put32(out, key::state, event.xbutton.state);
        L::put32(out, key::keycode, event.xbutton.button);
        L::put32(out, key::same_screen, (U32)event.xbutton.same_screen);
        break;
    case MotionNotify:
        L::put64(out, any::window, event.xmotion.window);
        L::put64(out, key::root, event.xmotion.root);
        L::put64(out, key::subwindow, event.xmotion.subwindow);
        L::put64(out, key::time, event.xmotion.time);
        L::put32(out, key::x, (U32)event.xmotion.x);
        L::put32(out, key::y, (U32)event.xmotion.y);
        L::put32(out, key::x_root, (U32)event.xmotion.x_root);
        L::put32(out, key::y_root, (U32)event.xmotion.y_root);
        L::put32(out, key::state, event.xmotion.state);
        out[key::is_hint] = (U8)event.xmotion.is_hint;
        L::put32(out, key::same_screen, (U32)event.xmotion.same_screen);
        break;
    case EnterNotify:
    case LeaveNotify:
        L::put64(out, any::window, event.xcrossing.window);
        L::put64(out, key::root, event.xcrossing.root);
        L::put64(out, key::subwindow, event.xcrossing.subwindow);
        L::put64(out, key::time, event.xcrossing.time);
        L::put32(out, key::x, (U32)event.xcrossing.x);
        L::put32(out, key::y, (U32)event.xcrossing.y);
        L::put32(out, key::x_root, (U32)event.xcrossing.x_root);
        L::put32(out, key::y_root, (U32)event.xcrossing.y_root);
        L::put32(out, crossing::mode, (U32)event.xcrossing.mode);
        L::put32(out, crossing::detail, (U32)event.xcrossing.detail);
        L::put32(out, crossing::same_screen, (U32)event.xcrossing.same_screen);
        L::put32(out, crossing::focus, (U32)event.xcrossing.focus);
        L::put32(out, crossing::state, event.xcrossing.state);
        break;
    case FocusIn:
    case FocusOut:
        L::put64(out, any::window, event.xfocus.window);
        L::put32(out, focus::mode, (U32)event.xfocus.mode);
        L::put32(out, focus::detail, (U32)event.xfocus.detail);
        break;
    case KeymapNotify:
        L::put64(out, any::window, event.xkeymap.window);
        memcpy(out + keymap::key_vector, event.xkeymap.key_vector, 32);
        break;
    case Expose:
        L::put64(out, any::window, event.xexpose.window);
        L::put32(out, expose::x, (U32)event.xexpose.x);
        L::put32(out, expose::y, (U32)event.xexpose.y);
        L::put32(out, expose::width, (U32)event.xexpose.width);
        L::put32(out, expose::height, (U32)event.xexpose.height);
        L::put32(out, expose::count, (U32)event.xexpose.count);
        break;
    case GraphicsExpose:
        L::put64(out, any::window, event.xgraphicsexpose.drawable);
        L::put32(out, expose::x, (U32)event.xgraphicsexpose.x);
        L::put32(out, expose::y, (U32)event.xgraphicsexpose.y);
        L::put32(out, expose::width, (U32)event.xgraphicsexpose.width);
        L::put32(out, expose::height, (U32)event.xgraphicsexpose.height);
        L::put32(out, expose::count, (U32)event.xgraphicsexpose.count);
        L::put32(out, expose::major_code, (U32)event.xgraphicsexpose.major_code);
        L::put32(out, expose::minor_code, (U32)event.xgraphicsexpose.minor_code);
        break;
    case NoExpose:
        L::put64(out, any::window, event.xnoexpose.drawable);
        L::put32(out, noexpose::major_code, (U32)event.xnoexpose.major_code);
        L::put32(out, noexpose::minor_code, (U32)event.xnoexpose.minor_code);
        break;
    case VisibilityNotify:
        L::put64(out, any::window, event.xvisibility.window);
        L::put32(out, visibility::state, (U32)event.xvisibility.state);
        break;
    case CreateNotify:
        L::put64(out, createwindow::parent, event.xcreatewindow.parent);
        L::put64(out, createwindow::window, event.xcreatewindow.window);
        L::put32(out, createwindow::x, (U32)event.xcreatewindow.x);
        L::put32(out, createwindow::y, (U32)event.xcreatewindow.y);
        L::put32(out, createwindow::width, (U32)event.xcreatewindow.width);
        L::put32(out, createwindow::height, (U32)event.xcreatewindow.height);
        L::put32(out, createwindow::border_width, (U32)event.xcreatewindow.border_width);
        L::put32(out, createwindow::override_redirect, (U32)event.xcreatewindow.override_redirect);
        break;
    case DestroyNotify:
        L::put64(out, destroywindow::event, event.xdestroywindow.event);
        L::put64(out, destroywindow::window, event.xdestroywindow.window);
        break;
    case UnmapNotify:
        L::put64(out, destroywindow::event, event.xunmap.event);
        L::put64(out, destroywindow::window, event.xunmap.window);
        L::put32(out, destroywindow::from_configure, (U32)event.xunmap.from_configure);
        break;
    case MapNotify:
        L::put64(out, destroywindow::event, event.xmap.event);
        L::put64(out, destroywindow::window, event.xmap.window);
        L::put32(out, destroywindow::override_redirect, (U32)event.xmap.override_redirect);
        break;
    case MapRequest:
        L::put64(out, maprequest::parent, event.xmaprequest.parent);
        L::put64(out, maprequest::window, event.xmaprequest.window);
        break;
    case ReparentNotify:
        L::put64(out, reparent::event, event.xreparent.event);
        L::put64(out, reparent::window, event.xreparent.window);
        L::put64(out, reparent::parent, event.xreparent.parent);
        L::put32(out, reparent::x, (U32)event.xreparent.x);
        L::put32(out, reparent::y, (U32)event.xreparent.y);
        L::put32(out, reparent::override_redirect, (U32)event.xreparent.override_redirect);
        break;
    case ConfigureNotify:
        L::put64(out, configure::event, event.xconfigure.event);
        L::put64(out, configure::window, event.xconfigure.window);
        L::put32(out, configure::x, (U32)event.xconfigure.x);
        L::put32(out, configure::y, (U32)event.xconfigure.y);
        L::put32(out, configure::width, (U32)event.xconfigure.width);
        L::put32(out, configure::height, (U32)event.xconfigure.height);
        L::put32(out, configure::border_width, (U32)event.xconfigure.border_width);
        L::put64(out, configure::above, event.xconfigure.above);
        L::put32(out, configure::override_redirect, (U32)event.xconfigure.override_redirect);
        break;
    case ConfigureRequest:
        L::put64(out, configurerequest::parent, event.xconfigurerequest.parent);
        L::put64(out, configurerequest::window, event.xconfigurerequest.window);
        L::put32(out, configurerequest::x, (U32)event.xconfigurerequest.x);
        L::put32(out, configurerequest::y, (U32)event.xconfigurerequest.y);
        L::put32(out, configurerequest::width, (U32)event.xconfigurerequest.width);
        L::put32(out, configurerequest::height, (U32)event.xconfigurerequest.height);
        L::put32(out, configurerequest::border_width, (U32)event.xconfigurerequest.border_width);
        L::put64(out, configurerequest::above, event.xconfigurerequest.above);
        L::put32(out, configurerequest::detail, (U32)event.xconfigurerequest.detail);
        L::put64(out, configurerequest::value_mask, event.xconfigurerequest.value_mask);
        break;
    case GravityNotify:
        L::put64(out, gravity::event, event.xgravity.event);
        L::put64(out, gravity::window, event.xgravity.window);
        L::put32(out, gravity::x, (U32)event.xgravity.x);
        L::put32(out, gravity::y, (U32)event.xgravity.y);
        break;
    case ResizeRequest:
        L::put64(out, any::window, event.xresizerequest.window);
        L::put32(out, resizerequest::width, (U32)event.xresizerequest.width);
        L::put32(out, resizerequest::height, (U32)event.xresizerequest.height);
        break;
    case CirculateNotify:
        L::put64(out, circulate::event, event.xcirculate.event);
        L::put64(out, circulate::window, event.xcirculate.window);
        L::put32(out, circulate::place, (U32)event.xcirculate.place);
        break;
    case CirculateRequest:
        L::put64(out, maprequest::parent, event.xcirculaterequest.parent);
        L::put64(out, maprequest::window, event.xcirculaterequest.window);
        L::put32(out, maprequest::place, (U32)event.xcirculaterequest.place);
        break;
    case PropertyNotify:
        L::put64(out, any::window, event.xproperty.window);
        L::put64(out, property::atom, event.xproperty.atom);
        L::put64(out, property::time, event.xproperty.time);
        L::put32(out, property::state, (U32)event.xproperty.state);
        break;
    case SelectionClear:
        L::put64(out, any::window, event.xselectionclear.window);
        L::put64(out, selectionclear::selection, event.xselectionclear.selection);
        L::put64(out, selectionclear::time, event.xselectionclear.time);
        break;
    case SelectionRequest:
        L::put64(out, selectionrequest::owner, event.xselectionrequest.owner);
        L::put64(out, selectionrequest::requestor, event.xselectionrequest.requestor);
        L::put64(out, selectionrequest::selection, event.xselectionrequest.selection);
        L::put64(out, selectionrequest::target, event.xselectionrequest.target);
        L::put64(out, selectionrequest::property, event.xselectionrequest.property);
        L::put64(out, selectionrequest::time, event.xselectionrequest.time);
        break;
    case SelectionNotify:
        L::put64(out, selection::requestor, event.xselection.requestor);
        L::put64(out, selection::selection, event.xselection.selection);
        L::put64(out, selection::target, event.xselection.target);
        L::put64(out, selection::property, event.xselection.property);
        L::put64(out, selection::time, event.xselection.time);
        break;
    case ColormapNotify:
        L::put64(out, any::window, event.xcolormap.window);
        L::put64(out, colormap::colormap, event.xcolormap.colormap);
        L::put32(out, colormap::c_new, (U32)event.xcolormap.c_new);
        L::put32(out, colormap::state, (U32)event.xcolormap.state);
        break;
    case ClientMessage:
        L::put64(out, any::window, event.xclient.window);
        L::put64(out, client::message_type, event.xclient.message_type);
        L::put32(out, client::format, (U32)event.xclient.format);
        if (event.xclient.format == 32) {
            for (int i = 0; i < 5; i++) {
                L::put64(out, client::data + 8 * i, (U64)(S64)event.xclient.data.l[i]);
            }
        } else {
            memcpy(out + client::data, event.xclient.data.b, 20);
        }
        break;
    case MappingNotify:
        L::put64(out, any::window, event.xmapping.window);
        L::put32(out, mapping::request, (U32)event.xmapping.request);
        L::put32(out, mapping::first_keycode, (U32)event.xmapping.first_keycode);
        L::put32(out, mapping::count, (U32)event.xmapping.count);
        break;
    case GenericEvent:
        L::put32(out, generic::extension, (U32)event.xgeneric.extension);
        L::put32(out, generic::evtype, (U32)event.xgeneric.evtype);
        L::put32(out, generic::cookie, event.xcookie.cookie);
        break;
    default:
        L::put64(out, any::window, event.xany.window);
        break;
    }
}

// The reverse direction for the events Wine hands back (XSendEvent for
// _NET_WM_STATE client messages, XPutBackEvent for what it read earlier).
void readEvent64(const U8* in, XEvent& event) {
    using namespace L::XEvent;
    memset(&event, 0, sizeof(event));
    event.type = (S32)L::get32(in, any::type);
    event.xany.serial = (U32)L::get64(in, any::serial);
    event.xany.send_event = (S32)L::get32(in, any::send_event);
    event.xany.display = 0;
    switch (event.type) {
    case ClientMessage:
        event.xclient.window = (U32)L::get64(in, any::window);
        event.xclient.message_type = (U32)L::get64(in, client::message_type);
        event.xclient.format = (S32)L::get32(in, client::format);
        if (event.xclient.format == 32) {
            for (int i = 0; i < 5; i++) {
                event.xclient.data.l[i] = (S32)L::get64(in, client::data + 8 * i);
            }
        } else {
            memcpy(event.xclient.data.b, in + client::data, 20);
        }
        break;
    case ConfigureNotify:
        event.xconfigure.event = (U32)L::get64(in, configure::event);
        event.xconfigure.window = (U32)L::get64(in, configure::window);
        event.xconfigure.x = (S32)L::get32(in, configure::x);
        event.xconfigure.y = (S32)L::get32(in, configure::y);
        event.xconfigure.width = (S32)L::get32(in, configure::width);
        event.xconfigure.height = (S32)L::get32(in, configure::height);
        event.xconfigure.border_width = (S32)L::get32(in, configure::border_width);
        event.xconfigure.above = (U32)L::get64(in, configure::above);
        event.xconfigure.override_redirect = (S32)L::get32(in, configure::override_redirect);
        break;
    case Expose:
        event.xexpose.window = (U32)L::get64(in, any::window);
        event.xexpose.x = (S32)L::get32(in, expose::x);
        event.xexpose.y = (S32)L::get32(in, expose::y);
        event.xexpose.width = (S32)L::get32(in, expose::width);
        event.xexpose.height = (S32)L::get32(in, expose::height);
        event.xexpose.count = (S32)L::get32(in, expose::count);
        break;
    case PropertyNotify:
        event.xproperty.window = (U32)L::get64(in, any::window);
        event.xproperty.atom = (U32)L::get64(in, property::atom);
        event.xproperty.time = (U32)L::get64(in, property::time);
        event.xproperty.state = (S32)L::get32(in, property::state);
        break;
    case SelectionNotify:
        event.xselection.requestor = (U32)L::get64(in, selection::requestor);
        event.xselection.selection = (U32)L::get64(in, selection::selection);
        event.xselection.target = (U32)L::get64(in, selection::target);
        event.xselection.property = (U32)L::get64(in, selection::property);
        event.xselection.time = (U32)L::get64(in, selection::time);
        break;
    default:
        event.xany.window = (U32)L::get64(in, any::window);
        break;
    }
}

// ---- Structure readers ------------------------------------------------------

bool readSetWindowAttributes(Call& call, U64 address, XSetWindowAttributes& out) {
    U8 raw[L::XSetWindowAttributes::size];
    if (!call.read(address, raw, sizeof(raw))) {
        return false;
    }
    using namespace L::XSetWindowAttributes;
    out.background_pixmap = (U32)L::get64(raw, background_pixmap);
    out.background_pixel = (U32)L::get64(raw, background_pixel);
    out.border_pixmap = (U32)L::get64(raw, border_pixmap);
    out.border_pixel = (U32)L::get64(raw, border_pixel);
    out.bit_gravity = (S32)L::get32(raw, bit_gravity);
    out.win_gravity = (S32)L::get32(raw, win_gravity);
    out.backing_store = (S32)L::get32(raw, backing_store);
    out.backing_planes = (U32)L::get64(raw, backing_planes);
    out.backing_pixel = (U32)L::get64(raw, backing_pixel);
    out.save_under = (S32)L::get32(raw, save_under);
    out.event_mask = (S32)L::get64(raw, event_mask);
    out.do_not_propagate_mask = (S32)L::get64(raw, do_not_propagate_mask);
    out.override_redirect = (S32)L::get32(raw, override_redirect);
    out.colormap = (U32)L::get64(raw, colormap);
    out.cursor = (U32)L::get64(raw, cursor);
    return true;
}

bool readWindowChanges(Call& call, U64 address, XWindowChanges& out) {
    U8 raw[L::XWindowChanges::size];
    if (!call.read(address, raw, sizeof(raw))) {
        return false;
    }
    using namespace L::XWindowChanges;
    out.x = (S32)L::get32(raw, x);
    out.y = (S32)L::get32(raw, y);
    out.width = (S32)L::get32(raw, width);
    out.height = (S32)L::get32(raw, height);
    out.border_width = (S32)L::get32(raw, border_width);
    out.sibling = (U32)L::get64(raw, sibling);
    out.stack_mode = (S32)L::get32(raw, stack_mode);
    return true;
}

bool readGCValues(Call& call, U64 address, XGCValues& out) {
    U8 raw[L::XGCValues::size];
    if (!call.read(address, raw, sizeof(raw))) {
        return false;
    }
    using namespace L::XGCValues;
    out.function = (S32)L::get32(raw, function);
    out.plane_mask = (U32)L::get64(raw, plane_mask);
    out.foreground = (U32)L::get64(raw, foreground);
    out.background = (U32)L::get64(raw, background);
    out.line_width = (S32)L::get32(raw, line_width);
    out.line_style = (S32)L::get32(raw, line_style);
    out.cap_style = (S32)L::get32(raw, cap_style);
    out.join_style = (S32)L::get32(raw, join_style);
    out.fill_style = (S32)L::get32(raw, fill_style);
    out.fill_rule = (S32)L::get32(raw, fill_rule);
    out.arc_mode = (S32)L::get32(raw, arc_mode);
    out.tile = (U32)L::get64(raw, tile);
    out.stipple = (U32)L::get64(raw, stipple);
    out.ts_x_origin = (S32)L::get32(raw, ts_x_origin);
    out.ts_y_origin = (S32)L::get32(raw, ts_y_origin);
    out.font = (U32)L::get64(raw, font);
    out.subwindow_mode = (S32)L::get32(raw, subwindow_mode);
    out.graphics_exposures = (S32)L::get32(raw, graphics_exposures);
    out.clip_x_origin = (S32)L::get32(raw, clip_x_origin);
    out.clip_y_origin = (S32)L::get32(raw, clip_y_origin);
    out.clip_mask = (U32)L::get64(raw, clip_mask);
    out.dash_offset = (S32)L::get32(raw, dash_offset);
    out.dashes = (S8)raw[dashes];
    return true;
}

bool readColor(Call& call, U64 address, XColor& out) {
    U8 raw[L::XColor::size];
    if (!call.read(address, raw, sizeof(raw))) {
        return false;
    }
    out.pixel = (U32)L::get64(raw, L::XColor::pixel);
    out.red = L::get16(raw, L::XColor::red);
    out.green = L::get16(raw, L::XColor::green);
    out.blue = L::get16(raw, L::XColor::blue);
    out.flags = raw[L::XColor::flags];
    out.pad = 0;
    return true;
}

bool writeColorRgb(Call& call, U64 address, U16 red, U16 green, U16 blue) {
    U8 raw[6];
    L::put16(raw, 0, red);
    L::put16(raw, 2, green);
    L::put16(raw, 4, blue);
    return call.write(address + L::XColor::red, raw, sizeof(raw));
}

// A 64-bit XTextProperty's bytes, packed the way the host stores them:
// format 8/16 bytes as-is, format 32 items packed from long to 32 bits.
bool readTextPropertyBytes(Call& call, U64 address, U32& encoding, U32& format, std::vector<U8>& bytes) {
    U8 raw[L::XTextProperty::size];
    if (!call.read(address, raw, sizeof(raw))) {
        return false;
    }
    const U64 value = L::get64(raw, L::XTextProperty::value);
    encoding = (U32)L::get64(raw, L::XTextProperty::encoding);
    format = L::get32(raw, L::XTextProperty::format);
    const U64 nitems = L::get64(raw, L::XTextProperty::nitems);
    if (nitems > 1024 * 1024) {
        call.faulted = true;
        return false;
    }
    if (format == 32) {
        std::vector<U8> longs((size_t)nitems * 8);
        if (nitems && !call.read(value, longs.data(), longs.size())) {
            return false;
        }
        bytes.resize((size_t)nitems * 4);
        for (U64 i = 0; i < nitems; i++) {
            L::put32(bytes.data(), (U32)(i * 4), (U32)L::get64(longs.data(), (U32)(i * 8)));
        }
    } else {
        const U64 itemBytes = format == 16 ? 2 : 1;
        bytes.resize((size_t)(nitems * itemBytes));
        if (nitems && !call.read(value, bytes.data(), bytes.size())) {
            return false;
        }
    }
    return true;
}

// ---- Operations -------------------------------------------------------------

typedef S64 (*OpHandler)(Call& call);

#define REQUIRE_ARGS(n) \
    if (call.count < (n)) { return BOXEDWINE_X64_X11_E_ARGS; }

#define REQUIRE_DISPLAY(var, slot) \
    DisplayDataPtr var = displayFor(call, call.arg(slot)); \
    if (!var) { return BOXEDWINE_X64_X11_E_DISPLAY; }

S64 op_INIT_THREADS(Call& call) {
    reportResult(call, "init-threads", 1, 0);
    return 1;
}

// { arena, arenaBytes, clientFd, serverFd }
S64 op_OPEN_DISPLAY(Call& call) {
    REQUIRE_ARGS(4);
    const U64 arena = call.arg(0);
    const U64 arenaBytes = call.arg(1);
    const U32 clientFd = (U32)call.arg(2);
    const U32 serverFd = (U32)call.arg(3);
    if (arenaBytes < BOXEDWINE_X64_X11_DISPLAY_ARENA_BYTES || !call.writable(arena, arenaBytes)) {
        reportResult(call, "open-display", BOXEDWINE_X64_X11_E_FAULT, arena);
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    XServer* server = XServer::getServer();
    KNativeScreenPtr screen = KNativeSystem::getScreen();

    // Lay out the arena: Display, Screen, Depth[], Visual[], ScreenFormat[],
    // then the strings. Every pointer field is an absolute guest address.
    std::vector<U8> bytes((size_t)BOXEDWINE_X64_X11_DISPLAY_ARENA_BYTES, 0);
    U64 cursor = BOXEDWINE_X64_X11_DISPLAY_PRIVATE_BYTES;
    const U64 screenOffset = cursor;
    cursor += L::Screen::size;

    std::vector<std::pair<U32, std::vector<VisualPtr>>> depthVisuals;
    server->iterateHostVisuals([&depthVisuals](U32 depth, const VisualPtr& visual) {
        if (depthVisuals.empty() || depthVisuals.back().first != depth) {
            depthVisuals.push_back(std::make_pair(depth, std::vector<VisualPtr>()));
        }
        depthVisuals.back().second.push_back(visual);
    });
    U32 visualTotal = 0;
    for (auto& entry : depthVisuals) {
        visualTotal += (U32)entry.second.size();
    }
    const U64 depthsOffset = cursor;
    cursor += (U64)depthVisuals.size() * L::Depth::size;
    const U64 visualsOffset = cursor;
    cursor += (U64)visualTotal * L::Visual::size;

    std::vector<XPixmapFormatEntry> visualFormats;
    for (auto& entry : depthVisuals) {
        if (!entry.second.empty()) {
            visualFormats.push_back({entry.first, (U32)entry.second.front()->bits_per_rgb, 32u});
        }
    }
    const std::vector<XPixmapFormatEntry> formats = xBuildPixmapFormats(visualFormats);
    const U64 formatsOffset = cursor;
    cursor += (U64)formats.size() * L::ScreenFormat::size;

    const char vendor[] = "Boxedwine.org";
    const char displayName[] = ":0";
    const U64 vendorOffset = cursor;
    cursor += sizeof(vendor);
    const U64 nameOffset = cursor;
    cursor += sizeof(displayName);
    if (cursor > bytes.size()) {
        klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=open-display result=arena-too-small need=%llu",
                 call.pid, (unsigned long long)cursor);
        return 0;
    }
    memcpy(bytes.data() + vendorOffset, vendor, sizeof(vendor));
    memcpy(bytes.data() + nameOffset, displayName, sizeof(displayName));

    // Visuals and depths.
    U64 visualCursor = visualsOffset;
    U64 rootVisualAddress = 0;
    U32 rootDepth = 0;
    for (size_t d = 0; d < depthVisuals.size(); d++) {
        const U64 depthOffset = depthsOffset + d * L::Depth::size;
        L::put32(bytes.data(), (U32)(depthOffset + L::Depth::depth), depthVisuals[d].first);
        L::put32(bytes.data(), (U32)(depthOffset + L::Depth::nvisuals), (U32)depthVisuals[d].second.size());
        L::put64(bytes.data(), (U32)(depthOffset + L::Depth::visuals), arena + visualCursor);
        if (d == 0) {
            rootDepth = depthVisuals[d].first;
            rootVisualAddress = arena + visualCursor;
        }
        for (const VisualPtr& visual : depthVisuals[d].second) {
            U8* v = bytes.data() + visualCursor;
            L::put64(v, L::Visual::ext_data, 0);
            L::put64(v, L::Visual::visualid, visual->visualid);
            L::put32(v, L::Visual::c_class, (U32)visual->c_class);
            L::put64(v, L::Visual::red_mask, visual->red_mask);
            L::put64(v, L::Visual::green_mask, visual->green_mask);
            L::put64(v, L::Visual::blue_mask, visual->blue_mask);
            L::put32(v, L::Visual::bits_per_rgb, (U32)visual->bits_per_rgb);
            L::put32(v, L::Visual::map_entries, (U32)visual->map_entries);
            visualCursor += L::Visual::size;
        }
    }
    // Pixmap formats.
    for (size_t i = 0; i < formats.size(); i++) {
        U8* f = bytes.data() + formatsOffset + i * L::ScreenFormat::size;
        L::put64(f, L::ScreenFormat::ext_data, 0);
        L::put32(f, L::ScreenFormat::depth, formats[i].depth);
        L::put32(f, L::ScreenFormat::bits_per_pixel, formats[i].bitsPerPixel);
        L::put32(f, L::ScreenFormat::scanline_pad, formats[i].scanlinePad);
    }
    // Screen.
    {
        U8* s = bytes.data() + screenOffset;
        L::put64(s, L::Screen::display, arena);
        L::put64(s, L::Screen::root, server->getRoot()->id);
        L::put32(s, L::Screen::width, screen->screenWidth());
        L::put32(s, L::Screen::height, screen->screenHeight());
        L::put32(s, L::Screen::mwidth, (U32)(screen->screenWidth() * 0.2646));
        L::put32(s, L::Screen::mheight, (U32)(screen->screenHeight() * 0.2646));
        L::put32(s, L::Screen::ndepths, (U32)depthVisuals.size());
        L::put64(s, L::Screen::depths, arena + depthsOffset);
        L::put32(s, L::Screen::root_depth, rootDepth);
        L::put64(s, L::Screen::root_visual, rootVisualAddress);
        L::put64(s, L::Screen::default_gc, 0);
        L::put64(s, L::Screen::cmap, server->getDefaultColorMap()->id);
        L::put64(s, L::Screen::white_pixel, 0x00FFFFFF);
        L::put64(s, L::Screen::black_pixel, 0);
        L::put32(s, L::Screen::max_maps, 1);
        L::put32(s, L::Screen::min_maps, 1);
        L::put32(s, L::Screen::backing_store, 0);
        L::put32(s, L::Screen::save_unders, 0);
        L::put64(s, L::Screen::root_input_mask, 0);
    }
    // Display.
    S32 minKeycode = 0;
    S32 maxKeycode = 0;
    XKeyboard::getMinMaxKeycodes(minKeycode, maxKeycode);
    DisplayDataPtr data = server->registerDisplay64(call.thread, arena, clientFd, serverFd);
    {
        U8* dpy = bytes.data();
        L::put32(dpy, L::Display::fd, clientFd);
        L::put32(dpy, L::Display::proto_major_version, 11);
        L::put32(dpy, L::Display::proto_minor_version, 4);
        L::put64(dpy, L::Display::vendor, arena + vendorOffset);
        L::put32(dpy, L::Display::byte_order, LSBFirst);
        L::put32(dpy, L::Display::bitmap_unit, 32);
        L::put32(dpy, L::Display::bitmap_pad, 32);
        L::put32(dpy, L::Display::bitmap_bit_order, LSBFirst);
        L::put32(dpy, L::Display::nformats, (U32)formats.size());
        L::put64(dpy, L::Display::pixmap_format, arena + formatsOffset);
        L::put32(dpy, L::Display::release, 1);
        L::put64(dpy, L::Display::request, 1);
        L::put32(dpy, L::Display::max_request_size, 65535);
        L::put64(dpy, L::Display::display_name, arena + nameOffset);
        L::put32(dpy, L::Display::default_screen, 0);
        L::put32(dpy, L::Display::nscreens, 1);
        L::put64(dpy, L::Display::screens, arena + screenOffset);
        L::put32(dpy, L::Display::min_keycode, (U32)minKeycode);
        L::put32(dpy, L::Display::max_keycode, (U32)maxKeycode);
        L::put32(dpy, BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET, data->displayId);
    }
    if (!call.write(arena, bytes.data(), bytes.size())) {
        server->unregisterDisplay64(data);
        reportResult(call, "open-display", BOXEDWINE_X64_X11_E_FAULT, arena);
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=open-display result=ok display=0x%llx id=%u "
             "root=0x%x screen=%ux%u depths=%u visuals=%u formats=%u fds=%u/%u",
             call.pid, (unsigned long long)arena, data->displayId, server->getRoot()->id,
             screen->screenWidth(), screen->screenHeight(), (unsigned)depthVisuals.size(),
             visualTotal, (unsigned)formats.size(), clientFd, serverFd);
    return (S64)data->displayId;
}

S64 op_CLOSE_DISPLAY(Call& call) {
    REQUIRE_ARGS(1);
    REQUIRE_DISPLAY(data, 0);
    const S64 result = XServer::getServer()->unregisterDisplay64(data);
    reportResult(call, "close-display", result, call.arg(0));
    return result;
}

S64 op_GRAB_SERVER(Call& call) {
    XServer* server = XServer::getServer();
#ifdef BOXEDWINE_MULTI_THREADED
    BOXEDWINE_MUTEX_LOCK(server->mutex);
#endif
    server->isLocked = true;
    return Success;
}

S64 op_UNGRAB_SERVER(Call& call) {
    XServer* server = XServer::getServer();
    server->isLocked = false;
#ifdef BOXEDWINE_MULTI_THREADED
    BOXEDWINE_MUTEX_UNLOCK(server->mutex);
#endif
    return Success;
}

S64 op_SYNC(Call& call) {
    // Everything is synchronous already; a sync still presents what was drawn.
    XServer::getServer()->draw(true);
    return Success;
}

S64 op_FLUSH(Call& call) {
    XServer::getServer()->draw(true);
    return Success;
}

// { display, name, onlyIfExists }
S64 op_INTERN_ATOM(Call& call) {
    REQUIRE_ARGS(3);
    BString name = call.readString(call.arg(1));
    if (call.faulted) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return XServer::getServer()->internAtom(name, call.arg(2) != 0);
}

// { display, names (char**), count, onlyIfExists, atoms_return (Atom*) }
S64 op_INTERN_ATOMS(Call& call) {
    REQUIRE_ARGS(5);
    const U64 names = call.arg(1);
    const U64 count = call.arg(2);
    const bool onlyIfExists = call.arg(3) != 0;
    const U64 atomsReturn = call.arg(4);
    if (count > 4096) {
        return BOXEDWINE_X64_X11_E_ARGS;
    }
    XServer* server = XServer::getServer();
    for (U64 i = 0; i < count; i++) {
        U64 nameAddress = 0;
        if (!call.read64(names + i * 8, nameAddress)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        BString name = call.readString(nameAddress);
        if (call.faulted) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        const U64 atom = server->internAtom(name, onlyIfExists);
        if (!call.write64(atomsReturn + i * 8, atom)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
    }
    return 1;
}

// { display, atom, buffer, capacity } -> length including the terminator
S64 op_GET_ATOM_NAME(Call& call) {
    REQUIRE_ARGS(4);
    BString name;
    if (!XServer::getServer()->getAtom((U32)call.arg(1), name)) {
        return 0;
    }
    S64 status = 0;
    if (!call.sizedResult(2, name.c_str(), (U64)name.length() + 1, status)) {
        return status;
    }
    return (S64)name.length() + 1;
}

// { display, parent, x, y, width, height, border_width, depth, class,
//   visual (Visual*), valuemask, attributes (XSetWindowAttributes*) }
S64 op_CREATE_WINDOW(Call& call) {
    REQUIRE_ARGS(12);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    XWindowPtr parent = server->getWindow((U32)call.arg(1));
    if (!parent) {
        reportResult(call, "create-window", BadWindow, call.arg(1));
        return BadWindow;
    }
    const S32 x = call.iarg(2);
    const S32 y = call.iarg(3);
    const U32 width = (U32)call.arg(4);
    const U32 height = (U32)call.arg(5);
    const U32 borderWidth = (U32)call.arg(6);
    U32 depth = (U32)call.arg(7);
    U32 c_class = (U32)call.arg(8);
    const U64 visualAddress = call.arg(9);
    const U64 valuemask = call.arg(10);
    const U64 attributesAddress = call.arg(11);

    VisualPtr visual;
    if (visualAddress == CopyFromParent) {
        visual = parent->getVisual();
    } else {
        U64 visualId = 0;
        if (!call.read64(visualAddress + L::Visual::visualid, visualId)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        visual = server->getVisual((U32)visualId);
        if (!visual) {
            reportResult(call, "create-window", BadMatch, visualId);
            return BadMatch;
        }
    }
    if (c_class == CopyFromParent) {
        c_class = parent->c_class;
    }
    if (depth == CopyFromParent) {
        depth = parent->getDepth();
    }
    XSetWindowAttributes attributes;
    bool hasAttributes = false;
    if (attributesAddress && valuemask) {
        if (!readSetWindowAttributes(call, attributesAddress, attributes)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        hasAttributes = true;
    }
    XWindowPtr result = server->createNewWindow(data->displayId, parent, width, height, depth, (U32)x, (U32)y, c_class, borderWidth, visual);
    if (!result) {
        reportResult(call, "create-window", BadAlloc, 0);
        return BadAlloc;
    }
    if (hasAttributes) {
        result->setAttributes(data, &attributes, (U32)valuemask);
    }
    klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=create-window result=ok window=0x%x parent=0x%x "
             "geometry=%d,%d %ux%u depth=%u class=%u mask=0x%llx",
             call.pid, result->id, parent->id, x, y, width, height, depth, c_class,
             (unsigned long long)valuemask);
    return (S64)result->id;
}

S64 op_DESTROY_WINDOW(Call& call) {
    REQUIRE_ARGS(2);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    return server->destroyWindow(w->id);
}

S64 op_MAP_WINDOW(Call& call) {
    REQUIRE_ARGS(2);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        reportResult(call, "map-window", BadWindow, call.arg(1));
        return BadWindow;
    }
    const S64 result = server->mapWindow(data, w);
    reportResult(call, "map-window", result, w->id);
    return result;
}

S64 op_UNMAP_WINDOW(Call& call) {
    REQUIRE_ARGS(2);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    return server->unmapWindow(data, w);
}

S64 op_SELECT_INPUT(Call& call) {
    REQUIRE_ARGS(3);
    REQUIRE_DISPLAY(data, 0);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    data->setEventMask(w->id, (U32)call.arg(2));
    return Success;
}

S64 op_MOVE_RESIZE_WINDOW(Call& call) {
    REQUIRE_ARGS(6);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    return w->moveResize(call.iarg(2), call.iarg(3), (U32)call.arg(4), (U32)call.arg(5));
}

// { display, window, mask, changes (XWindowChanges*) }
S64 op_CONFIGURE_WINDOW(Call& call) {
    REQUIRE_ARGS(4);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    XWindowChanges changes = {};
    if (!readWindowChanges(call, call.arg(3), changes)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return w->configure((U32)call.arg(2), &changes);
}

// { display, window, valuemask, attributes }
S64 op_CHANGE_WINDOW_ATTRIBUTES(Call& call) {
    REQUIRE_ARGS(4);
    REQUIRE_DISPLAY(data, 0);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    XSetWindowAttributes attributes;
    if (!readSetWindowAttributes(call, call.arg(3), attributes)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return w->setAttributes(data, &attributes, (U32)call.arg(2));
}

// { display, window, attributes_return (XWindowAttributes*) }
S64 op_GET_WINDOW_ATTRIBUTES(Call& call) {
    REQUIRE_ARGS(3);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    const XSetWindowAttributes& a = w->getAttributes();
    U8 raw[L::XWindowAttributes::size] = {};
    using namespace L::XWindowAttributes;
    L::put32(raw, x, (U32)w->x());
    L::put32(raw, y, (U32)w->y());
    L::put32(raw, width, w->width());
    L::put32(raw, height, w->height());
    L::put32(raw, border_width, w->borderWidth());
    L::put32(raw, depth, w->getDepth());
    L::put64(raw, visual, w->getVisual() ? visualAddressFor(call, call.arg(0), w->getVisual()->visualid) : 0);
    L::put64(raw, root, server->getRoot()->id);
    L::put32(raw, c_class, w->c_class);
    L::put32(raw, bit_gravity, (U32)a.bit_gravity);
    L::put32(raw, win_gravity, (U32)a.win_gravity);
    L::put32(raw, backing_store, (U32)a.backing_store);
    L::put64(raw, backing_planes, a.backing_planes);
    L::put64(raw, backing_pixel, a.backing_pixel);
    L::put32(raw, save_under, (U32)a.save_under);
    L::put64(raw, colormap, w->colorMap ? w->colorMap->id : server->getDefaultColorMap()->id);
    L::put32(raw, map_installed, 1);
    L::put32(raw, map_state, w->mapped() ? (w->isThisAndAncestorsMapped() ? 2 : 1) : 0);
    const U32 eventMask = data->getEventMask(w->id);
    L::put64(raw, all_event_masks, eventMask);
    L::put64(raw, your_event_mask, eventMask);
    L::put64(raw, do_not_propagate_mask, (U64)(S64)a.do_not_propagate_mask);
    L::put32(raw, override_redirect, (U32)a.override_redirect);
    L::put64(raw, screen, screenAddressFor(call, call.arg(0)));
    if (!call.write(call.arg(2), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

S64 op_REPARENT_WINDOW(Call& call) {
    REQUIRE_ARGS(5);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    XWindowPtr parent = server->getWindow((U32)call.arg(2));
    if (!w || !parent) {
        return BadWindow;
    }
    return w->reparentWindow(parent, call.iarg(3), call.iarg(4));
}

// { display, src_w, dest_w, src_x, src_y, dest_x_return, dest_y_return, child_return }
S64 op_TRANSLATE_COORDINATES(Call& call) {
    REQUIRE_ARGS(8);
    XServer* server = XServer::getServer();
    XWindowPtr src = server->getWindow((U32)call.arg(1));
    XWindowPtr dest = server->getWindow((U32)call.arg(2));
    if (!src || !dest) {
        return 0;
    }
    S32 x = call.iarg(3);
    S32 y = call.iarg(4);
    src->windowToScreen(x, y);
    XWindowPtr child = server->getRoot()->getWindowFromPoint(x, y);
    dest->screenToWindow(x, y);
    if (!call.write32(call.arg(5), (U32)x) || !call.write32(call.arg(6), (U32)y)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (call.arg(7) && !call.write64(call.arg(7), child ? child->id : 0)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

// { display, drawable, root_return, x_return, y_return, width_return,
//   height_return, border_width_return, depth_return }
S64 op_GET_GEOMETRY(Call& call) {
    REQUIRE_ARGS(9);
    XServer* server = XServer::getServer();
    XDrawablePtr d = server->getDrawable((U32)call.arg(1));
    if (!d) {
        return 0;
    }
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    bool ok = call.write64(call.arg(2), server->getRoot()->id);
    ok = ok && call.write32(call.arg(3), w ? (U32)w->x() : 0);
    ok = ok && call.write32(call.arg(4), w ? (U32)w->y() : 0);
    ok = ok && call.write32(call.arg(5), d->width());
    ok = ok && call.write32(call.arg(6), d->height());
    ok = ok && call.write32(call.arg(7), w ? w->borderWidth() : 0);
    ok = ok && call.write32(call.arg(8), d->getDepth());
    return ok ? 1 : BOXEDWINE_X64_X11_E_FAULT;
}

// { display, window, root_return, parent_return, children buffer, capacity } -> child count
S64 op_QUERY_TREE(Call& call) {
    REQUIRE_ARGS(6);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        return 0;
    }
    std::vector<U32> ids;
    w->getChildIds(ids);
    std::vector<U8> children(ids.size() * 8);
    for (size_t i = 0; i < ids.size(); i++) {
        L::put64(children.data(), (U32)(i * 8), ids[i]);
    }
    if (!call.write64(call.arg(2), server->getRoot()->id) ||
        !call.write64(call.arg(3), w->getParent() ? w->getParent()->id : 0)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    S64 status = 0;
    if (!ids.empty() && !call.sizedResult(4, children.data(), children.size(), status)) {
        return status;
    }
    call.setArg(5, children.size());
    return (S64)ids.size();
}

S64 op_WITHDRAW_WINDOW(Call& call) {
    REQUIRE_ARGS(2);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    return w->unmapWindow();
}

S64 op_ICONIFY_WINDOW(Call& call) {
    return 1;
}

// { display, focus, revert_to, time }
S64 op_SET_INPUT_FOCUS(Call& call) {
    REQUIRE_ARGS(4);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    const U32 focus = (U32)call.arg(1);
    if (focus != PointerRoot && focus != None && !server->getWindow(focus)) {
        return BadWindow;
    }
    return server->setInputFocus(data, focus, (U32)call.arg(2), (U32)call.arg(3), true);
}

// { display, focus_return (Window*), revert_to_return (int*) }
S64 op_GET_INPUT_FOCUS(Call& call) {
    REQUIRE_ARGS(3);
    XServer* server = XServer::getServer();
    const U32 focus = server->inputFocusIsPointerRoot ? PointerRoot : (server->inputFocus ? server->inputFocus->id : None);
    if (!call.write64(call.arg(1), focus) || !call.write32(call.arg(2), server->inputFocusRevertTo)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return Success;
}

S64 op_SET_TRANSIENT_FOR_HINT(Call& call) {
    REQUIRE_ARGS(3);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    w->setTransient((U32)call.arg(2));
    return Success;
}

S64 op_CLEAR_AREA(Call& call) {
    reportUnimplemented(call.pid, "clear-area", call.op);
    return Success;
}

// { display, window, property, type, format, mode, data, nelements }
S64 op_CHANGE_PROPERTY(Call& call) {
    REQUIRE_ARGS(8);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    const U32 property = (U32)call.arg(2);
    const U32 type = (U32)call.arg(3);
    const U32 format = (U32)call.arg(4);
    const U32 mode = (U32)call.arg(5);
    const U64 dataAddress = call.arg(6);
    const U64 nelements = call.arg(7);
    BString name;
    if (!server->getAtom(property, name) || !server->getAtom(type, name)) {
        return BadAtom;
    }
    if (format != 8 && format != 16 && format != 32) {
        return BadValue;
    }
    if (mode != PropModeReplace) {
        reportUnimplemented(call.pid, "change-property-append", call.op);
        return BadImplementation;
    }
    if (nelements > 1024 * 1024) {
        return BadValue;
    }
    std::vector<U8> bytes;
    if (format == 32) {
        // A 64-bit client passes format-32 items as longs; the server stores
        // them as 32-bit words, which is also what a 32-bit client passes.
        std::vector<U8> longs((size_t)nelements * 8);
        if (nelements && !call.read(dataAddress, longs.data(), longs.size())) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        bytes.resize((size_t)nelements * 4);
        for (U64 i = 0; i < nelements; i++) {
            L::put32(bytes.data(), (U32)(i * 4), (U32)L::get64(longs.data(), (U32)(i * 8)));
        }
    } else {
        bytes.resize((size_t)(nelements * (format / 8)));
        if (nelements && !call.read(dataAddress, bytes.data(), bytes.size())) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
    }
    w->setProperty(property, type, format, (U32)bytes.size(), bytes.data(), true);
    return Success;
}

// { display, window, property, long_offset, long_length, delete, req_type,
//   actual_type_return, actual_format_return, nitems_return,
//   bytes_after_return, buffer, capacity }
S64 op_GET_WINDOW_PROPERTY(Call& call) {
    REQUIRE_ARGS(13);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    const U32 property = (U32)call.arg(2);
    const U64 longOffset = call.arg(3) * 4;
    const U64 longLength = call.arg(4) * 4;
    const bool deleteProperty = call.arg(5) != 0;
    const U32 reqType = (U32)call.arg(6);
    bool ok = call.write64(call.arg(7), 0);
    ok = ok && call.write32(call.arg(8), 0);
    ok = ok && call.write64(call.arg(9), 0);
    ok = ok && call.write64(call.arg(10), 0);
    if (!ok) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (!w) {
        return BadWindow;
    }
    BString name;
    if (!server->getAtom(property, name)) {
        return BadAtom;
    }
    if (reqType != AnyPropertyType && !server->getAtom(reqType, name)) {
        return BadAtom;
    }
    XPropertyPtr prop = w->getProperty(property);
    if (!prop) {
        call.setArg(12, 0);
        return Success;
    }
    if (longOffset > prop->length) {
        return BadValue;
    }
    ok = call.write64(call.arg(7), prop->type);
    ok = ok && call.write32(call.arg(8), prop->format);
    if (!ok) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (reqType != AnyPropertyType && reqType != prop->type) {
        call.setArg(12, 0);
        if (!call.write64(call.arg(10), prop->length)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        return Success;
    }
    const U64 toCopy = std::min<U64>(longLength, prop->length - longOffset);
    std::vector<U8> bytes;
    U64 nitems = 0;
    if (prop->format == 32) {
        const U64 items = toCopy / 4;
        nitems = items;
        bytes.resize((size_t)(items * 8 + 8));
        for (U64 i = 0; i < items; i++) {
            // Xlib hands format-32 data to a 64-bit client as longs, each
            // sign-extended from the 32-bit wire value.
            const U32 value = L::get32(prop->value, (U32)(longOffset + i * 4));
            L::put64(bytes.data(), (U32)(i * 8), (U64)(S64)(S32)value);
        }
    } else {
        nitems = toCopy * 8 / prop->format;
        bytes.resize((size_t)toCopy + 1);
        if (toCopy) {
            memcpy(bytes.data(), prop->value + longOffset, (size_t)toCopy);
        }
    }
    S64 status = 0;
    if (!call.sizedResult(11, bytes.data(), bytes.size(), status)) {
        return status;
    }
    ok = call.write64(call.arg(9), nitems);
    ok = ok && call.write64(call.arg(10), prop->length - longOffset - toCopy);
    if (!ok) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (deleteProperty && prop->length - longOffset - toCopy == 0) {
        w->deleteProperty(property);
    }
    return Success;
}

S64 op_DELETE_PROPERTY(Call& call) {
    REQUIRE_ARGS(3);
    XServer* server = XServer::getServer();
    XWindowPtr w = server->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    BString name;
    if (!server->getAtom((U32)call.arg(2), name)) {
        return BadAtom;
    }
    w->deleteProperty((U32)call.arg(2));
    return Success;
}

// { display, window, hints (XWMHints*) }
S64 op_SET_WM_HINTS(Call& call) {
    REQUIRE_ARGS(3);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    U8 raw[L::XWMHints::size];
    if (!call.read(call.arg(2), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    XWMHints hints = {};
    hints.flags = (U32)L::get64(raw, L::XWMHints::flags);
    hints.input = (S32)L::get32(raw, L::XWMHints::input);
    hints.initial_state = (S32)L::get32(raw, L::XWMHints::initial_state);
    hints.icon_pixmap = (U32)L::get64(raw, L::XWMHints::icon_pixmap);
    hints.icon_window = (U32)L::get64(raw, L::XWMHints::icon_window);
    hints.icon_x = (S32)L::get32(raw, L::XWMHints::icon_x);
    hints.icon_y = (S32)L::get32(raw, L::XWMHints::icon_y);
    hints.icon_mask = (U32)L::get64(raw, L::XWMHints::icon_mask);
    hints.window_group = (U32)L::get64(raw, L::XWMHints::window_group);
    w->setProperty(XA_WM_HINTS, XA_CARDINAL, 32, sizeof(XWMHints), (const U8*)&hints, true);
    return Success;
}

// { display, window, hints_return (XWMHints*, 56 bytes) } -> 1 when present
S64 op_GET_WM_HINTS(Call& call) {
    REQUIRE_ARGS(3);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return 0;
    }
    XPropertyPtr prop = w->getProperty(XA_WM_HINTS);
    if (!prop || prop->length != sizeof(XWMHints)) {
        return 0;
    }
    XWMHints hints;
    memcpy(&hints, prop->value, sizeof(hints));
    U8 raw[L::XWMHints::size] = {};
    L::put64(raw, L::XWMHints::flags, hints.flags);
    L::put32(raw, L::XWMHints::input, (U32)hints.input);
    L::put32(raw, L::XWMHints::initial_state, (U32)hints.initial_state);
    L::put64(raw, L::XWMHints::icon_pixmap, hints.icon_pixmap);
    L::put64(raw, L::XWMHints::icon_window, hints.icon_window);
    L::put32(raw, L::XWMHints::icon_x, (U32)hints.icon_x);
    L::put32(raw, L::XWMHints::icon_y, (U32)hints.icon_y);
    L::put64(raw, L::XWMHints::icon_mask, hints.icon_mask);
    L::put64(raw, L::XWMHints::window_group, hints.window_group);
    if (!call.write(call.arg(2), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

// { display, window, hints (XSizeHints*) }
S64 op_SET_WM_NORMAL_HINTS(Call& call) {
    REQUIRE_ARGS(3);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    U8 raw[L::XSizeHints::size];
    if (!call.read(call.arg(2), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    XSizeHints hints = {};
    hints.flags = (U32)L::get64(raw, L::XSizeHints::flags);
    // Every field after flags is an int at the same relative order.
    memcpy(&hints.x, raw + L::XSizeHints::x, sizeof(XSizeHints) - sizeof(U32));
    w->setProperty(XA_WM_NORMAL_HINTS, XA_CARDINAL, 32, sizeof(XSizeHints), (const U8*)&hints, true);
    return Success;
}

// { display, window, hints_return (XSizeHints*), supplied_return (long*) }
S64 op_GET_WM_NORMAL_HINTS(Call& call) {
    REQUIRE_ARGS(4);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return 0;
    }
    XPropertyPtr prop = w->getProperty(XA_WM_NORMAL_HINTS);
    if (!prop || prop->length != sizeof(XSizeHints)) {
        return 0;
    }
    XSizeHints hints;
    memcpy(&hints, prop->value, sizeof(hints));
    U8 raw[L::XSizeHints::size] = {};
    L::put64(raw, L::XSizeHints::flags, hints.flags);
    memcpy(raw + L::XSizeHints::x, &hints.x, sizeof(XSizeHints) - sizeof(U32));
    if (!call.write(call.arg(2), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (call.arg(3) && !call.write64(call.arg(3), hints.flags)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

// { display, window, text_prop (XTextProperty*), property }
S64 op_SET_TEXT_PROPERTY(Call& call) {
    REQUIRE_ARGS(4);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return BadWindow;
    }
    U32 encoding = 0;
    U32 format = 8;
    std::vector<U8> bytes;
    if (!readTextPropertyBytes(call, call.arg(2), encoding, format, bytes)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    w->setProperty((U32)call.arg(3), encoding, format, (U32)bytes.size(), bytes.data(), true);
    return Success;
}

S64 op_LOCK_EVENTS(Call& call) {
    REQUIRE_ARGS(1);
    REQUIRE_DISPLAY(data, 0);
    return (S64)data->lockEvents();
}

// { display, index, event_return (192 bytes) } -> 1 when copied
S64 op_GET_EVENT(Call& call) {
    REQUIRE_ARGS(3);
    REQUIRE_DISPLAY(data, 0);
    XEvent* event = data->getEvent((U32)call.arg(1));
    if (!event) {
        return 0;
    }
    U8 raw[L::XEvent::size];
    writeEvent64(*event, data->displayAddress64, raw);
    if (!call.write(call.arg(2), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

S64 op_REMOVE_EVENT(Call& call) {
    REQUIRE_ARGS(2);
    REQUIRE_DISPLAY(data, 0);
    data->removeEvent((U32)call.arg(1));
    return Success;
}

S64 op_UNLOCK_EVENTS(Call& call) {
    REQUIRE_ARGS(1);
    REQUIRE_DISPLAY(data, 0);
    data->unlockEvents();
    return Success;
}

// { display, window, event_type, event_return }
S64 op_CHECK_TYPED_WINDOW_EVENT(Call& call) {
    REQUIRE_ARGS(4);
    REQUIRE_DISPLAY(data, 0);
    XWindowPtr w = XServer::getServer()->getWindow((U32)call.arg(1));
    if (!w) {
        return 0;
    }
    XEvent event = {};
    if (!data->findAndRemoveEvent(w->id, call.iarg(2), event)) {
        return 0;
    }
    U8 raw[L::XEvent::size];
    writeEvent64(event, data->displayAddress64, raw);
    if (!call.write(call.arg(3), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

// { display, window, propagate, event_mask, event_send }
S64 op_SEND_EVENT(Call& call) {
    REQUIRE_ARGS(5);
    XServer* server = XServer::getServer();
    const U32 windowId = (U32)call.arg(1);
    XWindowPtr window = server->getWindow(windowId);
    const bool propagate = call.arg(2) != 0;
    const U32 eventMask = (U32)call.arg(3);
    if (server->selectionWindow && windowId == server->selectionWindow->id) {
        return 1;
    }
    if (windowId == PointerWindow || windowId == InputFocus) {
        reportUnimplemented(call.pid, "send-event-special-destination", call.op);
        return BadImplementation;
    }
    if (!window) {
        return BadWindow;
    }
    U8 raw[L::XEvent::size];
    if (!call.read(call.arg(4), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    XEvent event;
    readEvent64(raw, event);
    if (event.type == ClientMessage && event.xclient.message_type == _NET_WM_STATE) {
        return window->handleNetWmStatePropertyEvent(event);
    }
    event.xany.send_event = True;
    if (!eventMask) {
        DisplayDataPtr dst = server->getDisplayDataById(window->displayId);
        if (!dst) {
            return BadValue;
        }
        event.xany.serial = dst->getNextEventSerial();
        event.xany.display = dst->displayAddress;
        dst->putEvent(event);
    } else if (!propagate) {
        server->iterateEventMask(window->id, eventMask, [&event](const DisplayDataPtr dst) {
            event.xany.serial = dst->getNextEventSerial();
            event.xany.display = dst->displayAddress;
            dst->putEvent(event);
        });
    } else {
        reportUnimplemented(call.pid, "send-event-propagate", call.op);
        return BadImplementation;
    }
    return 1;
}

// { display, event }
S64 op_PUT_BACK_EVENT(Call& call) {
    REQUIRE_ARGS(2);
    REQUIRE_DISPLAY(data, 0);
    U8 raw[L::XEvent::size];
    if (!call.read(call.arg(1), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    XEvent event;
    readEvent64(raw, event);
    event.xany.display = data->displayAddress;
    data->putEvent(event, true);
    return Success;
}

// { display, grab_window, owner_events, event_mask, pointer_mode,
//   keyboard_mode, confine_to, cursor, time }
S64 op_GRAB_POINTER(Call& call) {
    REQUIRE_ARGS(9);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    XWindowPtr window = server->getWindow((U32)call.arg(1));
    if (!window) {
        return BadWindow;
    }
    if (!window->mapped()) {
        return GrabNotViewable;
    }
    XWindowPtr confined;
    if (call.arg(6)) {
        confined = server->getWindow((U32)call.arg(6));
        if (!confined) {
            return BadWindow;
        }
    }
    return server->grabPointer(data, window, confined, (U32)call.arg(3), (U32)call.arg(8));
}

S64 op_UNGRAB_POINTER(Call& call) {
    REQUIRE_ARGS(2);
    return XServer::getServer()->ungrabPointer((U32)call.arg(1));
}

// { display, src_w, dest_w, src_x, src_y, src_width, src_height, dest_x, dest_y }
S64 op_WARP_POINTER(Call& call) {
    REQUIRE_ARGS(9);
    S32 x = call.iarg(7);
    S32 y = call.iarg(8);
    KNativeInputPtr input = KNativeSystem::getCurrentInput();
    if (!call.arg(2)) {
        S32 currentX = 0;
        S32 currentY = 0;
        input->getMousePos(&currentX, &currentY);
        x += currentX;
        y += currentY;
    }
#ifdef BOXEDWINE_IOS
    XServer* server = XServer::getServer(true);
    if (server && server->fakeFullScreenWnd && call.arg(2) &&
        (U32)call.arg(2) != server->fakeFullScreenWnd->id) {
        XWindowPtr destination = server->getWindow((U32)call.arg(2));
        if (destination) {
            if (destination->id != server->getRoot()->id) {
                destination->windowToScreen(x, y);
            }
            server->fakeFullScreenWnd->screenToWindow(x, y);
        }
    }
#endif
    input->setMousePos(x, y);
    return Success;
}

// { display, window, root_return, child_return, root_x_return, root_y_return,
//   win_x_return, win_y_return, mask_return }
S64 op_QUERY_POINTER(Call& call) {
    REQUIRE_ARGS(9);
    XServer* server = XServer::getServer();
    XWindowPtr window = server->getWindow((U32)call.arg(1));
    XWindowPtr root = server->getRoot();
    if (!window) {
        return 0;
    }
    S32 x = 0;
    S32 y = 0;
    KNativeSystem::getCurrentInput()->getMousePos(&x, &y);
    S32 rootX = x;
    S32 rootY = y;
#ifdef BOXEDWINE_IOS
    const S32 guestX = x;
    const S32 guestY = y;
    if (server->fakeFullScreenWnd) {
        server->fakeFullScreenWnd->windowToScreen(rootX, rootY);
    }
#endif
    XWindowPtr child;
#ifdef BOXEDWINE_IOS
    if (server->fakeFullScreenWnd && x >= 0 && y >= 0 &&
        x < (S32)server->fakeFullScreenWnd->width() &&
        y < (S32)server->fakeFullScreenWnd->height()) {
        child = server->fakeFullScreenWnd;
    } else
#endif
    {
        child = root->getWindowFromPoint(x, y);
    }
#ifdef BOXEDWINE_IOS
    if (server->fakeFullScreenWnd && window->id == root->id) {
        x = rootX;
        y = rootY;
        window->screenToWindow(x, y);
    } else if (server->fakeFullScreenWnd && window->id == server->fakeFullScreenWnd->id) {
        x = guestX;
        y = guestY;
    } else if (server->fakeFullScreenWnd) {
        x = rootX;
        y = rootY;
        window->screenToWindow(x, y);
    } else {
        window->screenToWindow(x, y);
    }
#else
    window->screenToWindow(x, y);
#endif
    bool ok = call.write64(call.arg(2), root->id);
    ok = ok && call.write64(call.arg(3), child ? child->id : 0);
    ok = ok && call.write32(call.arg(4), (U32)rootX);
    ok = ok && call.write32(call.arg(5), (U32)rootY);
    ok = ok && call.write32(call.arg(6), (U32)x);
    ok = ok && call.write32(call.arg(7), (U32)y);
    ok = ok && call.write32(call.arg(8), server->getInputModifiers());
    return ok ? 1 : BOXEDWINE_X64_X11_E_FAULT;
}

// { display, window, visual (Visual*), alloc }
S64 op_CREATE_COLORMAP(Call& call) {
    REQUIRE_ARGS(4);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    const U64 visualAddress = call.arg(2);
    const U32 alloc = (U32)call.arg(3);
    Visual visual = {};
    if (visualAddress) {
        U8 raw[L::Visual::size];
        if (!call.read(visualAddress, raw, sizeof(raw))) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        visual.visualid = (U32)L::get64(raw, L::Visual::visualid);
        visual.c_class = (S32)L::get32(raw, L::Visual::c_class);
        visual.red_mask = (U32)L::get64(raw, L::Visual::red_mask);
        visual.green_mask = (U32)L::get64(raw, L::Visual::green_mask);
        visual.blue_mask = (U32)L::get64(raw, L::Visual::blue_mask);
        visual.bits_per_rgb = (S32)L::get32(raw, L::Visual::bits_per_rgb);
        visual.map_entries = (S32)L::get32(raw, L::Visual::map_entries);
    }
    if (alloc == AllocNone && (!visualAddress || visual.c_class == TrueColor)) {
        return DummyAtom;
    }
    const U32 id = server->createColorMap(&visual, (int)alloc);
    if (alloc == AllocAll) {
        XColorMapPtr colorMap = server->getColorMap(id);
        for (U32 i = 0; i < MAX_COLORMAP_SIZE; i++) {
            colorMap->colors[i].flags |= (COLOR_ALLOCATED | COLOR_WRITE);
            colorMap->colors[i].uses.set(data->displayId, 1);
        }
    }
    return id;
}

S64 op_FREE_COLORMAP(Call& call) {
    return Success;
}

// { display, colormap, color (XColor*) }
S64 op_ALLOC_COLOR(Call& call) {
    REQUIRE_ARGS(3);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    XColorMapPtr colorMap = server->getColorMap((U32)call.arg(1));
    if (!colorMap) {
        return 0;
    }
    XColor color;
    if (!readColor(call, call.arg(2), color)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    const U8 r = (U8)(color.red >> 8);
    const U8 g = (U8)(color.green >> 8);
    const U8 b = (U8)(color.blue >> 8);
    int freeIndex = -1;
    for (U32 i = 0; i < MAX_COLORMAP_SIZE; i++) {
        XColorMapColor& c = colorMap->colors[i];
        if ((c.flags & COLOR_ALLOCATED) && c.r == r && c.g == g && c.b == b) {
            c.uses.set(data->displayId, 1);
            c.flags = COLOR_ALLOCATED;
            return call.write64(call.arg(2) + L::XColor::pixel, i) ? 1 : BOXEDWINE_X64_X11_E_FAULT;
        } else if (freeIndex == -1 && !(c.flags & COLOR_ALLOCATED)) {
            freeIndex = (int)i;
        }
    }
    if (freeIndex < 0) {
        return 0;
    }
    XColorMapColor& c = colorMap->colors[freeIndex];
    c.r = r;
    c.g = g;
    c.b = b;
    c.flags = COLOR_ALLOCATED;
    c.uses.set(data->displayId, 1);
    colorMap->dirty = true;
    return call.write64(call.arg(2) + L::XColor::pixel, (U64)freeIndex) ? 1 : BOXEDWINE_X64_X11_E_FAULT;
}

// { display, colormap, contig, plane_masks_return, nplanes, pixels_return, npixels }
S64 op_ALLOC_COLOR_CELLS(Call& call) {
    REQUIRE_ARGS(7);
    REQUIRE_DISPLAY(data, 0);
    XColorMapPtr colorMap = XServer::getServer()->getColorMap((U32)call.arg(1));
    if (!colorMap) {
        return 0;
    }
    U64 npixels = call.arg(6);
    U64 pixelsAddress = call.arg(5);
    U32 available = 0;
    for (U32 i = 0; i < MAX_COLORMAP_SIZE; i++) {
        if (!(colorMap->colors[i].flags & COLOR_ALLOCATED)) {
            available++;
        }
    }
    if (npixels > available) {
        return 0;
    }
    for (U32 i = 0; i < MAX_COLORMAP_SIZE && npixels; i++) {
        XColorMapColor& c = colorMap->colors[i];
        if (c.flags & COLOR_ALLOCATED) {
            continue;
        }
        c.flags |= (COLOR_ALLOCATED | COLOR_WRITE);
        c.uses.set(data->displayId, 1);
        if (!call.write64(pixelsAddress, i)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        pixelsAddress += 8;
        npixels--;
    }
    return 1;
}

// { display, colormap, pixels (unsigned long*), npixels, planes }
S64 op_FREE_COLORS(Call& call) {
    REQUIRE_ARGS(5);
    REQUIRE_DISPLAY(data, 0);
    XColorMapPtr colorMap = XServer::getServer()->getColorMap((U32)call.arg(1));
    if (!colorMap) {
        return BadColor;
    }
    U64 pixelsAddress = call.arg(2);
    const U64 npixels = call.arg(3);
    S64 result = Success;
    for (U64 i = 0; i < npixels; i++) {
        U64 index = 0;
        if (!call.read64(pixelsAddress, index)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        if (index >= MAX_COLORMAP_SIZE) {
            return BadValue;
        }
        XColorMapColor& c = colorMap->colors[index];
        if (!(c.flags & COLOR_ALLOCATED)) {
            result = BadAccess;
        }
        c.uses.remove(data->displayId);
        if (c.uses.size() == 0) {
            c.flags = 0;
        }
        pixelsAddress += 8;
    }
    return result;
}

// { display, colormap, color (XColor*) }
S64 op_QUERY_COLOR(Call& call) {
    REQUIRE_ARGS(3);
    XColorMapPtr colorMap = XServer::getServer()->getColorMap((U32)call.arg(1));
    if (!colorMap) {
        return BadColor;
    }
    XColor color;
    if (!readColor(call, call.arg(2), color)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (color.pixel >= MAX_COLORMAP_SIZE) {
        return BadValue;
    }
    const XColorMapColor& c = colorMap->colors[color.pixel];
    return writeColorRgb(call, call.arg(2), (U16)(c.r << 8), (U16)(c.g << 8), (U16)(c.b << 8)) ? Success : BOXEDWINE_X64_X11_E_FAULT;
}

// { display, colormap, colors (XColor*), ncolors }
S64 op_QUERY_COLORS(Call& call) {
    REQUIRE_ARGS(4);
    XColorMapPtr colorMap = XServer::getServer()->getColorMap((U32)call.arg(1));
    if (!colorMap) {
        return BadColor;
    }
    const U64 count = call.arg(3);
    if (count > MAX_COLORMAP_SIZE) {
        return BadValue;
    }
    for (U64 i = 0; i < count; i++) {
        const U64 address = call.arg(2) + i * L::XColor::size;
        XColor color;
        if (!readColor(call, address, color)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        if (color.pixel >= MAX_COLORMAP_SIZE) {
            return BadValue;
        }
        const XColorMapColor& c = colorMap->colors[color.pixel];
        if (!writeColorRgb(call, address, (U16)(c.r << 8), (U16)(c.g << 8), (U16)(c.b << 8))) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
    }
    return Success;
}

// { display, colormap, color (XColor*) }
S64 op_STORE_COLOR(Call& call) {
    REQUIRE_ARGS(3);
    XColorMapPtr colorMap = XServer::getServer()->getColorMap((U32)call.arg(1));
    if (!colorMap) {
        return BadColor;
    }
    XColor color;
    if (!readColor(call, call.arg(2), color)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (color.pixel >= MAX_COLORMAP_SIZE) {
        return BadValue;
    }
    XColorMapColor& c = colorMap->colors[color.pixel];
    if (!(c.flags & COLOR_WRITE)) {
        return BadAccess;
    }
    c.r = (U8)(color.red >> 8);
    c.g = (U8)(color.green >> 8);
    c.b = (U8)(color.blue >> 8);
    colorMap->dirty = true;
    return Success;
}

// { display, drawable, valuemask, values (XGCValues*) }
S64 op_CREATE_GC(Call& call) {
    REQUIRE_ARGS(4);
    XServer* server = XServer::getServer();
    XDrawablePtr drawable = server->getDrawable((U32)call.arg(1));
    if (!drawable) {
        return 0;
    }
    XGCPtr gc = server->createGC(drawable);
    const U32 mask = (U32)call.arg(2);
    if (mask && call.arg(3)) {
        XGCValues values = {};
        if (!readGCValues(call, call.arg(3), values)) {
            server->removeGC(gc->id);
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        gc->updateValues(mask, &values);
    }
    return gc->id;
}

S64 op_FREE_GC(Call& call) {
    REQUIRE_ARGS(2);
    XServer* server = XServer::getServer();
    if (!server->getGC((U32)call.arg(1))) {
        return BadGC;
    }
    server->removeGC((U32)call.arg(1));
    return Success;
}

// { display, gc, valuemask, values }
S64 op_CHANGE_GC(Call& call) {
    REQUIRE_ARGS(4);
    XGCPtr gc = XServer::getServer()->getGC((U32)call.arg(1));
    if (!gc) {
        return BadGC;
    }
    XGCValues values = {};
    if (!readGCValues(call, call.arg(3), values)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    gc->updateValues((U32)call.arg(2), &values);
    return Success;
}

// { display, gc, selector, value }
S64 op_SET_GC_VALUE(Call& call) {
    REQUIRE_ARGS(4);
    XGCPtr gc = XServer::getServer()->getGC((U32)call.arg(1));
    if (!gc) {
        return BadGC;
    }
    const U64 value = call.arg(3);
    switch (call.arg(2)) {
    case BOXEDWINE_X64_X11_GC_FUNCTION: gc->values.function = (S32)value; break;
    case BOXEDWINE_X64_X11_GC_BACKGROUND: gc->values.background = (U32)value; break;
    case BOXEDWINE_X64_X11_GC_FOREGROUND: gc->values.foreground = (U32)value; break;
    case BOXEDWINE_X64_X11_GC_SUBWINDOW_MODE: gc->values.subwindow_mode = (S32)value; break;
    case BOXEDWINE_X64_X11_GC_GRAPHICS_EXPOSURES: gc->values.graphics_exposures = value ? True : False; break;
    case BOXEDWINE_X64_X11_GC_CLIP_MASK: gc->values.clip_mask = (U32)value; break;
    case BOXEDWINE_X64_X11_GC_FILL_STYLE: gc->values.fill_style = (S32)value; break;
    case BOXEDWINE_X64_X11_GC_ARC_MODE: gc->values.arc_mode = (S32)value; break;
    default: return BadValue;
    }
    return Success;
}

// { display, gc, clip_x_origin, clip_y_origin, rectangles, n, ordering }
S64 op_SET_CLIP_RECTANGLES(Call& call) {
    REQUIRE_ARGS(7);
    XGCPtr gc = XServer::getServer()->getGC((U32)call.arg(1));
    if (!gc) {
        return BadGC;
    }
    const U64 count = call.arg(5);
    if (count > 65536) {
        return BadValue;
    }
    std::vector<U8> raw((size_t)count * L::XRectangle::size);
    if (count && !call.read(call.arg(4), raw.data(), raw.size())) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    gc->values.clip_x_origin = call.iarg(2);
    gc->values.clip_y_origin = call.iarg(3);
    gc->clip_rects.clear();
    for (U64 i = 0; i < count; i++) {
        XRectangle r;
        const U32 offset = (U32)(i * L::XRectangle::size);
        r.x = (S16)L::get16(raw.data(), offset + L::XRectangle::x);
        r.y = (S16)L::get16(raw.data(), offset + L::XRectangle::y);
        r.width = L::get16(raw.data(), offset + L::XRectangle::width);
        r.height = L::get16(raw.data(), offset + L::XRectangle::height);
        gc->clip_rects.push_back(r);
    }
    return Success;
}

// { display, src, dest, gc, src_x, src_y, width, height, dest_x, dest_y }
S64 op_COPY_AREA(Call& call) {
    REQUIRE_ARGS(10);
    XServer* server = XServer::getServer();
    XDrawablePtr src = server->getDrawable((U32)call.arg(1));
    XDrawablePtr dest = server->getDrawable((U32)call.arg(2));
    if (!src || !dest) {
        return BadDrawable;
    }
    XGCPtr gc = server->getGC((U32)call.arg(3));
    if (!gc) {
        return BadGC;
    }
    return dest->copy(call.thread, gc, src, call.iarg(4), call.iarg(5), (U32)call.arg(6), (U32)call.arg(7), call.iarg(8), call.iarg(9));
}

// { display, drawable, gc, x1, y1, x2, y2 }
S64 op_DRAW_LINE(Call& call) {
    REQUIRE_ARGS(7);
    XServer* server = XServer::getServer();
    XDrawablePtr drawable = server->getDrawable((U32)call.arg(1));
    if (!drawable) {
        return BadDrawable;
    }
    XGCPtr gc = server->getGC((U32)call.arg(2));
    if (!gc) {
        return BadGC;
    }
    return drawable->drawLine(call.thread, gc, call.iarg(3), call.iarg(4), call.iarg(5), call.iarg(6));
}

// { display, drawable, gc, x, y, width, height }
S64 op_DRAW_RECTANGLE(Call& call) {
    REQUIRE_ARGS(7);
    XServer* server = XServer::getServer();
    XDrawablePtr drawable = server->getDrawable((U32)call.arg(1));
    if (!drawable) {
        return BadDrawable;
    }
    XGCPtr gc = server->getGC((U32)call.arg(2));
    if (!gc) {
        return BadGC;
    }
    return drawable->drawRectangle(call.thread, gc, call.iarg(3), call.iarg(4), (U32)call.arg(5), (U32)call.arg(6));
}

S64 op_FILL_RECTANGLE(Call& call) {
    REQUIRE_ARGS(7);
    XServer* server = XServer::getServer();
    XDrawablePtr drawable = server->getDrawable((U32)call.arg(1));
    if (!drawable) {
        return BadDrawable;
    }
    XGCPtr gc = server->getGC((U32)call.arg(2));
    if (!gc) {
        return BadGC;
    }
    return drawable->fillRectangle(call.thread, gc, call.iarg(3), call.iarg(4), (U32)call.arg(5), (U32)call.arg(6));
}

// { display, drawable, gc, image (XImage*), src_x, src_y, dest_x, dest_y, width, height }
S64 op_PUT_IMAGE(Call& call) {
    REQUIRE_ARGS(10);
    XServer* server = XServer::getServer();
    XDrawablePtr drawable = server->getDrawable((U32)call.arg(1));
    if (!drawable) {
        return BadDrawable;
    }
    XGCPtr gc = server->getGC((U32)call.arg(2));
    if (!gc) {
        return BadGC;
    }
    U8 raw[L::XImage::size];
    if (!call.read(call.arg(3), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    const U64 dataAddress = L::get64(raw, L::XImage::data);
    const U32 bytesPerLine = L::get32(raw, L::XImage::bytes_per_line);
    const U32 bitsPerPixel = L::get32(raw, L::XImage::bits_per_pixel);
    const S32 srcX = call.iarg(4);
    const S32 srcY = call.iarg(5);
    const U32 width = (U32)call.arg(8);
    const U32 height = (U32)call.arg(9);
    if (srcX < 0 || srcY < 0 || !bytesPerLine || height > 16384 || width > 16384) {
        return BadValue;
    }
    // Only the rows this transfer reads are copied out of the guest.
    const U64 firstRow = (U64)bytesPerLine * (U64)srcY;
    const U64 rowBytes = ((U64)bitsPerPixel * (srcX + width) + 7) / 8;
    const U64 needed = firstRow + (U64)bytesPerLine * (height ? height - 1 : 0) + rowBytes;
    if (needed > 256u * 1024u * 1024u) {
        return BadValue;
    }
    std::vector<U8> bytes((size_t)needed);
    if (needed && !call.read(dataAddress, bytes.data(), needed)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return drawable->copyHostImageData(gc, bytes.data(), (U32)bytes.size(), bytesPerLine, (S32)bitsPerPixel, srcX, srcY, call.iarg(6), call.iarg(7), width, height);
}

S64 op_GET_IMAGE(Call& call) {
    reportUnimplemented(call.pid, "get-image", call.op);
    return BOXEDWINE_X64_X11_E_UNIMPL;
}

// { display, drawable, width, height, depth }
S64 op_CREATE_PIXMAP(Call& call) {
    REQUIRE_ARGS(5);
    XServer* server = XServer::getServer();
    XDrawablePtr d = server->getDrawable((U32)call.arg(1));
    if (!d) {
        return 0;
    }
    XPixmapPtr pixmap = server->createNewPixmap((U32)call.arg(2), (U32)call.arg(3), (U32)call.arg(4), d->getVisual());
    return pixmap ? pixmap->id : 0;
}

// { display, drawable, data, width, height }
S64 op_CREATE_BITMAP_FROM_DATA(Call& call) {
    REQUIRE_ARGS(5);
    XServer* server = XServer::getServer();
    XDrawablePtr d = server->getDrawable((U32)call.arg(1));
    XWindowPtr window = server->getWindow((U32)call.arg(1));
    if (!d) {
        return 0;
    }
    const U32 width = (U32)call.arg(3);
    const U32 height = (U32)call.arg(4);
    if (width > 16384 || height > 16384) {
        return 0;
    }
    const U32 depth = window ? window->getDepth() : d->getDepth();
    XPixmapPtr pixmap = server->createNewPixmap(width, height, depth, d->getVisual());
    if (!pixmap) {
        return 0;
    }
    // The 32-bit path copies the 1-bit rows through the pixmap's own stride.
    // Read only the bitmap the guest actually holds and re-stride it here.
    const U32 sourceStride = (width + 7) / 8;
    const U32 targetStride = pixmap->getBytesPerLine();
    std::vector<U8> source((size_t)sourceStride * height);
    if (!source.empty() && !call.read(call.arg(2), source.data(), source.size())) {
        server->removePixmap(pixmap->id);
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    std::vector<U8> restrided((size_t)targetStride * height, 0);
    for (U32 y = 0; y < height; y++) {
        memcpy(restrided.data() + (size_t)y * targetStride, source.data() + (size_t)y * sourceStride,
               std::min(sourceStride, targetStride));
    }
    pixmap->copyHostImageData(nullptr, restrided.data(), (U32)restrided.size(), targetStride, pixmap->getBitsPerPixel(), 0, 0, 0, 0, width, height);
    return pixmap->id;
}

S64 op_FREE_PIXMAP(Call& call) {
    REQUIRE_ARGS(2);
    return XServer::getServer()->removePixmap((U32)call.arg(1));
}

S64 op_CREATE_FONT_CURSOR(Call& call) {
    REQUIRE_ARGS(2);
    XServer* server = XServer::getServer();
    XCursorPtr cursor = std::make_shared<XCursor>((U32)call.arg(1));
    server->addCursor(cursor);
    return cursor->id;
}

// { display, source, mask, foreground (XColor*), background (XColor*), x, y }
S64 op_CREATE_PIXMAP_CURSOR(Call& call) {
    REQUIRE_ARGS(7);
    XServer* server = XServer::getServer();
    XColor fg;
    XColor bg;
    if (!readColor(call, call.arg(3), fg) || !readColor(call, call.arg(4), bg)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    XPixmapPtr pixmap = server->getPixmap((U32)call.arg(1));
    XPixmapPtr mask = server->getPixmap((U32)call.arg(2));
    if (!pixmap) {
        return 0;
    }
    XCursorPtr cursor = std::make_shared<XCursor>(pixmap, mask, fg, bg, (U32)call.arg(5), (U32)call.arg(6));
    server->addCursor(cursor);
    return cursor->id;
}

S64 op_DEFINE_CURSOR(Call& call) {
    REQUIRE_ARGS(3);
    XServer* server = XServer::getServer();
    XWindowPtr window = server->getWindow((U32)call.arg(1));
    if (!window) {
        return BadWindow;
    }
    window->cursor = server->getCursor((U32)call.arg(2));
    server->updateCursor(window);
    return Success;
}

S64 op_FREE_CURSOR(Call& call) {
    return Success;
}

// { display, first_keycode, keycode_count, buffer, capacity } -> keysyms_per_keycode
S64 op_GET_KEYBOARD_MAPPING(Call& call) {
    REQUIRE_ARGS(5);
    S32 minKeycode = 0;
    S32 maxKeycode = 0;
    XKeyboard::getMinMaxKeycodes(minKeycode, maxKeycode);
    const U64 first = call.arg(1);
    const U64 count = call.arg(2);
    const U32 perKeycode = 6;
    if (count == 0 || count > 256 || first + count - 1 > (U64)maxKeycode) {
        return BadValue;
    }
    std::vector<U8> keysyms((size_t)count * perKeycode * 8);
    for (U64 i = 0; i < count; i++) {
        for (U32 j = 0; j < perKeycode; j++) {
            const U32 keysym = XKeyboard::keycodeToKeysym((U32)(first + i), j);
            L::put64(keysyms.data(), (U32)((i * perKeycode + j) * 8), keysym);
        }
    }
    S64 status = 0;
    if (!call.sizedResult(3, keysyms.data(), keysyms.size(), status)) {
        return status;
    }
    return perKeycode;
}

// { display, modifiermap buffer (8 bytes) } -> max_keypermod
S64 op_GET_MODIFIER_MAPPING(Call& call) {
    REQUIRE_ARGS(2);
    if (!call.write(call.arg(1), XKeyboard::getModifiers(), 8)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return 1;
}

S64 op_KEYSYM_TO_KEYCODE(Call& call) {
    REQUIRE_ARGS(2);
    return XKeyboard::keysymToKeycode((U32)call.arg(1));
}

// { display, keycode, group, level }
S64 op_KEYCODE_TO_KEYSYM(Call& call) {
    REQUIRE_ARGS(4);
    return XKeyboard::keycodeToKeysym((U32)call.arg(1), (U32)call.arg(3));
}

// { keysym, buffer, capacity } -> length including terminator, 0 when unknown
S64 op_KEYSYM_TO_STRING(Call& call) {
    REQUIRE_ARGS(3);
    const char* name = XKeyboard::getKeysymName((U32)call.arg(0));
    if (!name) {
        return 0;
    }
    const U64 length = (U64)strlen(name) + 1;
    S64 status = 0;
    if (!call.sizedResult(1, name, length, status)) {
        return status;
    }
    return (S64)length;
}

// { event (XKeyEvent*), buffer_return, bytes_buffer, keysym_return } -> length
S64 op_LOOKUP_STRING(Call& call) {
    REQUIRE_ARGS(4);
    U8 raw[L::XEvent::size];
    if (!call.read(call.arg(0), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    const U32 state = L::get32(raw, L::XEvent::key::state);
    const U32 keycode = L::get32(raw, L::XEvent::key::keycode);
    U32 keysym = 0;
    if (state & NumMask) {
        keysym = XKeyboard::keycodeToKeysym(keycode, 5);
    }
    if (!keysym) {
        keysym = XKeyboard::keycodeToKeysym(keycode, (state & ShiftMask) ? 1 : 0);
    }
    if (!keysym) {
        return 0;
    }
    char buffer[16] = {};
    U32 result = XKeyboard::translate(keysym, state, buffer, sizeof(buffer));
    if (call.arg(3) && !call.write64(call.arg(3), keysym)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (result && buffer[0] != 0) {
        const U64 capacity = call.arg(2);
        if (result > capacity) {
            result = (U32)capacity;
        }
        if (result && !call.write(call.arg(1), buffer, result)) {
            return BOXEDWINE_X64_X11_E_FAULT;
        }
        return result;
    }
    return 0;
}

// { display, sym_return (KeySym*), modifiers, buffer, nbytes, extra_rtrn }
S64 op_KB_TRANSLATE_KEYSYM(Call& call) {
    REQUIRE_ARGS(6);
    U64 keysym64 = 0;
    if (!call.read64(call.arg(1), keysym64)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    U32 keysym = (U32)keysym64;
    char buffer[16] = {};
    U32 result = XKeyboard::translate(keysym, (U32)call.arg(2), buffer, sizeof(buffer));
    if (!result) {
        return 0;
    }
    const U64 nbytes = call.arg(4);
    if (result > nbytes) {
        result = (U32)nbytes;
    }
    if (!call.write(call.arg(3), buffer, result + 1 <= nbytes ? result + 1 : result)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    if (!call.write64(call.arg(1), keysym)) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return result;
}

// { event (XKeyEvent*), index }
S64 op_LOOKUP_KEYSYM(Call& call) {
    REQUIRE_ARGS(2);
    U8 raw[L::XEvent::size];
    if (!call.read(call.arg(0), raw, sizeof(raw))) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    return XKeyboard::keycodeToKeysym(L::get32(raw, L::XEvent::key::keycode), (U32)call.arg(1));
}

// { display, name, major_opcode_return, first_event_return, first_error_return }
S64 op_QUERY_EXTENSION(Call& call) {
    REQUIRE_ARGS(5);
    BString name = call.readString(call.arg(1));
    if (call.faulted) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    // No extension is offered to the 64-bit driver yet: XInput2, RENDER,
    // MIT-SHM and the rest all report absent so Wine takes its core paths.
    if (firstTime(call.pid, std::string("ext:") + name.c_str())) {
        klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=query-extension name='%s' result=absent",
                 call.pid, name.c_str());
    }
    return 0;
}

S64 op_SET_SELECTION_OWNER(Call& call) {
    REQUIRE_ARGS(4);
    XServer::getServer()->selectionOwner = (U32)call.arg(2);
    return Success;
}

S64 op_GET_SELECTION_OWNER(Call& call) {
    return XServer::getServer()->selectionOwner;
}

// { display, selection, target, property, requestor, time }
S64 op_CONVERT_SELECTION(Call& call) {
    REQUIRE_ARGS(6);
    REQUIRE_DISPLAY(data, 0);
    XServer* server = XServer::getServer();
    const U32 selection = (U32)call.arg(1);
    const U32 target = (U32)call.arg(2);
    U32 property = (U32)call.arg(3);
    const U32 requestor = (U32)call.arg(4);
    XWindowPtr wnd = server->getWindow(requestor);
    if (!wnd) {
        return BadWindow;
    }
    if (selection == CLIPBOARD) {
        if (target == TARGETS) {
            U32 atom = UTF8_STRING;
            wnd->setProperty(property, XA_ATOM, 32, 4, (const U8*)&atom);
        } else if (target == UTF8_STRING) {
            BString text = KNativeSystem::getScreen()->clipboardGetText();
            server->sdlLastSelection = text;
            wnd->setProperty(property, UTF8_STRING, 8, text.length(), (const U8*)text.c_str());
        } else {
            property = None;
        }
    } else {
        property = None;
    }
    XEvent event = {};
    event.type = SelectionNotify;
    event.xselection.display = data->displayAddress;
    event.xselection.requestor = requestor;
    event.xselection.selection = selection;
    event.xselection.target = target;
    event.xselection.property = property;
    event.xselection.serial = data->getNextEventSerial();
    event.xselection.time = server->getEventTime();
    event.xselection.send_event = False;
    data->putEvent(event);
    return Success;
}

// { name (char*) }: a guest-side stub was reached.
S64 op_REPORT_UNIMPLEMENTED(Call& call) {
    REQUIRE_ARGS(1);
    BString name = call.readString(call.arg(0), 128);
    if (call.faulted) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    reportUnimplemented(call.pid, name.c_str(), (U64)-1);
    return Success;
}

// { text (char*) }: a bounded guest-side trace line.
S64 op_TRACE(Call& call) {
    REQUIRE_ARGS(1);
    static std::atomic<U32> budget {0};
    if (budget.fetch_add(1, std::memory_order_relaxed) >= 128) {
        return Success;
    }
    BString text = call.readString(call.arg(0), 256);
    if (call.faulted) {
        return BOXEDWINE_X64_X11_E_FAULT;
    }
    klog_fmt("BOXEDWINE_X64_X11_SHIM pid=%u %s", call.pid, text.c_str());
    return Success;
}

// ---- RandR 1.2+ -------------------------------------------------------------
//
// The 32-bit lane answers XRandR 1.1 only: a size list and one rate
// (source/x11/xrandr.cpp), and its 1.2 entry points in x11common.cpp are
// kpanics. Wine 9.0's winex11 reads its mode list through RRGetScreenResources,
// RRGetOutputInfo and RRGetCrtcInfo instead, so the modes live here, built
// once by XrrStandardModeList and served to the shim, which turns them into
// the Xrandr structures Wine's driver reads.

std::mutex& randrMutex() {
    static std::mutex mutex;
    return mutex;
}

struct RandrModeList {
    XrrModeEntry entries[BOXEDWINE_X64_X11_RANDR_MAX_MODES];
    U32 count = 0;
    bool built = false;
};

// Built once, from the desktop size the launch asked for, and fixed after
// that: a program that enumerates modes, switches, and enumerates again has
// to be shown the same list both times. Only the current mode moves.
U32 randrModeList(const XrrModeEntry** entries) {
    static RandrModeList modes;
    std::lock_guard<std::mutex> lock(randrMutex());
    if (!modes.built) {
        KNativeScreenPtr screen = KNativeSystem::getScreen();
        const U32 width = screen ? screen->screenWidth() : 0;
        const U32 height = screen ? screen->screenHeight() : 0;
        const U32 rate = screen ? screen->screenRate() : 0;
        modes.count = XrrStandardModeList(width, height, rate, modes.entries,
                                          BOXEDWINE_X64_X11_RANDR_MAX_MODES);
        modes.built = true;
    }
    *entries = modes.entries;
    return modes.count;
}

// The rate the last accepted switch asked for, and the size it asked for.
// The host panel keeps its own refresh rate whatever a guest selects, so
// reporting that rate back would tell a caller that asked for 120 and got
// RRSetConfigSuccess that it is running at 60. It is told what it chose, for
// as long as the mode it chose is the one on screen.
struct RandrRequest {
    U32 width = 0;
    U32 height = 0;
    U32 rate = 0;
};

RandrRequest& randrRequest() {
    static RandrRequest request;
    return request;
}

U32 randrCurrentRate(U32 width, U32 height) {
    {
        std::lock_guard<std::mutex> lock(randrMutex());
        const RandrRequest& request = randrRequest();
        if (request.rate && request.width == width && request.height == height) {
            return request.rate;
        }
    }
    KNativeScreenPtr screen = KNativeSystem::getScreen();
    const U32 rate = screen ? screen->screenRate() : 0;
    return rate ? rate : (U32)XRR_MODE_RATE_PRIMARY;
}

// The index of the mode the root window is showing. Wine fails
// xrandr14_get_current_mode outright when the CRTC names a mode the resource
// list does not carry, so this never answers "none": an exact match first,
// then the same size at any rate, then the first mode.
U32 randrCurrentModeIndex(const XrrModeEntry* entries, U32 count, U32 width, U32 height, U32 rate) {
    if (!count) {
        return BOXEDWINE_X64_X11_RANDR_NO_MODE;
    }
    for (U32 i = 0; i < count; i++) {
        if (entries[i].width == width && entries[i].height == height && entries[i].rate == rate) {
            return i;
        }
    }
    for (U32 i = 0; i < count; i++) {
        if (entries[i].width == width && entries[i].height == height) {
            return i;
        }
    }
    return 0;
}

// { display, buffer, capacity } -> mode count
S64 op_RANDR_GET_STATE(Call& call) {
    REQUIRE_ARGS(3);
    REQUIRE_DISPLAY(data, 0);
    const XrrModeEntry* entries = nullptr;
    const U32 count = randrModeList(&entries);
    KNativeScreenPtr screen = KNativeSystem::getScreen();
    const U32 width = screen ? screen->screenWidth() : 0;
    const U32 height = screen ? screen->screenHeight() : 0;
    const U32 rate = randrCurrentRate(width, height);
    const U32 current = randrCurrentModeIndex(entries, count, width, height, rate);

    boxedwine_x64_x11_randr_state state = {};
    state.version = BOXEDWINE_X64_X11_RANDR_STATE_VERSION;
    state.modeCount = count;
    state.currentWidth = width;
    state.currentHeight = height;
    state.currentRate = rate;
    state.currentMode = current;
    state.mmWidth = (U32)(width * 0.2646);
    state.mmHeight = (U32)(height * 0.2646);
    state.minWidth = 320;
    state.minHeight = 200;
    state.maxWidth = 8192;
    state.maxHeight = 8192;

    std::vector<U8> bytes(sizeof(state) + (size_t)count * sizeof(boxedwine_x64_x11_randr_mode), 0);
    memcpy(bytes.data(), &state, sizeof(state));
    for (U32 i = 0; i < count; i++) {
        boxedwine_x64_x11_randr_mode mode = {};
        mode.width = entries[i].width;
        mode.height = entries[i].height;
        mode.rate = entries[i].rate;
        if (i == current) {
            mode.flags |= BOXEDWINE_X64_X11_RANDR_MODE_CURRENT;
        }
        if (entries[i].width == width && entries[i].height == height &&
            entries[i].rate == (U32)XRR_MODE_RATE_PRIMARY) {
            mode.flags |= BOXEDWINE_X64_X11_RANDR_MODE_PREFERRED;
        }
        memcpy(bytes.data() + sizeof(state) + (size_t)i * sizeof(mode), &mode, sizeof(mode));
    }
    S64 status = 0;
    if (!call.sizedResult(1, bytes.data(), bytes.size(), status)) {
        return status;
    }
    if (firstTime(call.pid, "randr:modes")) {
        klog_fmt("BOXEDWINE_X64_RANDR modes=%u current=%ux%u@%u", count, width, height, rate);
    }
    return (S64)count;
}

// { display, width, height, rate } -> RRSetConfigSuccess / RRSetConfigFailed
//
// The rate is accepted and reported back but never applied: the host panel
// runs at whatever iOS gives it, and a program that asked for 60 on a 120 Hz
// panel wants the mode, not a refused call.
S64 op_RANDR_SET_MODE(Call& call) {
    REQUIRE_ARGS(4);
    REQUIRE_DISPLAY(data, 0);
    const U32 width = (U32)call.arg(1);
    const U32 height = (U32)call.arg(2);
    const U32 rate = (U32)call.arg(3);
    const XrrModeEntry* entries = nullptr;
    const U32 count = randrModeList(&entries);
    KNativeScreenPtr screen = KNativeSystem::getScreen();
    const U32 fromWidth = screen ? screen->screenWidth() : 0;
    const U32 fromHeight = screen ? screen->screenHeight() : 0;

    bool listed = false;
    for (U32 i = 0; i < count; i++) {
        if (entries[i].width == width && entries[i].height == height) {
            listed = true;
            break;
        }
    }
    if (!width || !height || !listed) {
        klog_fmt("BOXEDWINE_X64_RANDR pid=%u mode-switch %ux%u@%u refused=unlisted",
                 call.pid, width, height, rate);
        return RRSetConfigFailed;
    }
    {
        std::lock_guard<std::mutex> lock(randrMutex());
        RandrRequest& request = randrRequest();
        request.width = width;
        request.height = height;
        request.rate = rate;
    }
    if (width != fromWidth || height != fromHeight) {
        XServer::getServer()->changeScreen(width, height);
    }
    klog_fmt("BOXEDWINE_X64_RANDR pid=%u mode-switch %ux%u -> %ux%u@%u result=ok",
             call.pid, fromWidth, fromHeight, width, height, rate);
    return RRSetConfigSuccess;
}

struct OpEntry {
    const char* name;
    OpHandler handler;
};

const OpEntry kOps[] = {
#define BOXEDWINE_X64_X11_OP_ENTRY(name, text) { text, op_##name },
    BOXEDWINE_X64_X11_OPS(BOXEDWINE_X64_X11_OP_ENTRY)
#undef BOXEDWINE_X64_X11_OP_ENTRY
};

static_assert(sizeof(kOps) / sizeof(kOps[0]) == BOXEDWINE_X64_X11_OP_COUNT,
              "every bridge operation needs a handler");

} // namespace

bool x11Bridge64UpdateScreenSize(KProcess* process, U64 displayAddress, U32 width, U32 height) {
    if (!process || !process->memory64 || !displayAddress) {
        return false;
    }
    KMemory64* memory = process->memory64;
    GuestPageProbe probe(memory);
    if (!boxedvn::x11Bridge64RangeAccessible(probe, displayAddress + L::Display::screens, 8, false)) {
        return false;
    }
    U64 screenAddress = 0;
    memory->memcpyFromGuest(&screenAddress, displayAddress + L::Display::screens, 8);
    if (!screenAddress ||
        !boxedvn::x11Bridge64RangeAccessible(probe, screenAddress, L::Screen::size, true)) {
        return false;
    }
    const U32 values[4] = { width, height, (U32)(width * 0.2646), (U32)(height * 0.2646) };
    memory->memcpyToGuest(screenAddress + L::Screen::width, &values[0], 4);
    memory->memcpyToGuest(screenAddress + L::Screen::height, &values[1], 4);
    memory->memcpyToGuest(screenAddress + L::Screen::mwidth, &values[2], 4);
    memory->memcpyToGuest(screenAddress + L::Screen::mheight, &values[3], 4);
    return true;
}

U64 x11Bridge64(CPU64* cpu, U64 op, U64 argsAddress, U64 count) {
    if (!cpu || !cpu->memory || !cpu->thread || !cpu->thread->process) {
        return (U64)(S64)BOXEDWINE_X64_X11_E_FAULT;
    }
    Call call;
    call.cpu = cpu;
    call.memory = cpu->memory;
    call.thread = cpu->thread;
    call.pid = (U32)cpu->thread->process->id;
    GuestPageProbe probe(cpu->memory);
    boxedvn::X11Bridge64Call decoded = boxedvn::x11Bridge64Decode(
        probe, op, argsAddress, count,
        [cpu](uint64_t address, uint64_t* out, uint64_t n) {
            cpu->memory->memcpyFromGuest(out, address, n * sizeof(uint64_t));
        });
    if (decoded.admission != boxedvn::X11Bridge64Admission::Admitted) {
        const S64 result = boxedvn::x11Bridge64AdmissionResult(decoded.admission);
        if (firstTime(call.pid, "refused:" + std::to_string(op) + ":" + std::to_string(result))) {
            klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=%llu refused result=%lld args=0x%llx count=%llu",
                     call.pid, (unsigned long long)op, (long long)result,
                     (unsigned long long)argsAddress, (unsigned long long)count);
        }
        return (U64)result;
    }
    call.op = op;
    call.count = count;
    memcpy(call.args, decoded.args, sizeof(call.args));

    const OpEntry& entry = kOps[op];
    traceFirstCall(call, entry.name);
    S64 result = entry.handler(call);
    if (call.faulted && result >= 0) {
        result = BOXEDWINE_X64_X11_E_FAULT;
    }
    if (result == BOXEDWINE_X64_X11_E_FAULT || result == BOXEDWINE_X64_X11_E_ARGS ||
        result == BOXEDWINE_X64_X11_E_DISPLAY) {
        if (firstTime(call.pid, std::string("failed:") + entry.name + ":" + std::to_string(result))) {
            klog_fmt("BOXEDWINE_X64_X11_BRIDGE pid=%u op=%s result=%lld count=%llu a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx",
                     call.pid, entry.name, (long long)result, (unsigned long long)count,
                     (unsigned long long)call.arg(0), (unsigned long long)call.arg(1),
                     (unsigned long long)call.arg(2), (unsigned long long)call.arg(3));
        }
    }
    // The array is IN/OUT: sizes and other slot results travel back this way.
    if (count) {
        cpu->memory->memcpyToGuest(argsAddress, call.args, count * sizeof(U64));
    }
    return (U64)result;
}

#endif // BOXEDWINE_GUEST_X64
