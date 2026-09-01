/* BoxedVN - x86-64 Xlib layout contract for the guest X11 bridge. GPLv2. */

#include "boxedvn_test.h"

#include "x11layout64.h"

#define BOXEDWINE_X64_X11_BRIDGE_HOST 1
#include "boxedwine_x64_x11_bridge.h"

#include <cstring>

namespace L = x11layout64;

// The 64-bit driver reads these through Xlib's macros: ConnectionNumber,
// DefaultScreen, ScreenOfDisplay, RootWindowOfScreen, DefaultVisual and
// friends. The offsets are the SysV x86-64 ABI's; the guest shim asserts the
// same values against the real headers in tools/x11-64/layout_check.c.
BOXEDVN_TEST(x11_layout64_display_fields_match_the_x86_64_abi) {
    CHECK_EQ(L::Display::fd, 16U);
    CHECK_EQ(L::Display::vendor, 32U);
    CHECK_EQ(L::Display::nformats, 96U);
    CHECK_EQ(L::Display::pixmap_format, 104U);
    CHECK_EQ(L::Display::release, 116U);
    CHECK_EQ(L::Display::request, 152U);
    CHECK_EQ(L::Display::display_name, 216U);
    CHECK_EQ(L::Display::default_screen, 224U);
    CHECK_EQ(L::Display::nscreens, 228U);
    CHECK_EQ(L::Display::screens, 232U);
    CHECK_EQ(L::Display::min_keycode, 256U);
    CHECK_EQ(L::Display::max_keycode, 260U);
    CHECK_EQ(L::Display::size, 296U);
    // The host's private id follows the public structure and stays inside
    // the tail the shim reserves.
    CHECK(BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET >= L::Display::size);
    CHECK(BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET + 8 <= BOXEDWINE_X64_X11_DISPLAY_PRIVATE_BYTES);
}

BOXEDVN_TEST(x11_layout64_screen_and_visual_fields_are_eight_byte_aligned) {
    CHECK_EQ(L::Screen::display, 8U);
    CHECK_EQ(L::Screen::root, 16U);
    CHECK_EQ(L::Screen::width, 24U);
    CHECK_EQ(L::Screen::depths, 48U);
    CHECK_EQ(L::Screen::root_visual, 64U);
    CHECK_EQ(L::Screen::cmap, 80U);
    CHECK_EQ(L::Screen::white_pixel, 88U);
    CHECK_EQ(L::Screen::root_input_mask, 120U);
    CHECK_EQ(L::Screen::size, 128U);
    CHECK_EQ(L::Depth::visuals, 8U);
    CHECK_EQ(L::Depth::size, 16U);
    CHECK_EQ(L::Visual::visualid, 8U);
    CHECK_EQ(L::Visual::c_class, 16U);
    CHECK_EQ(L::Visual::red_mask, 24U);
    CHECK_EQ(L::Visual::map_entries, 52U);
    CHECK_EQ(L::Visual::size, 56U);
    CHECK_EQ(L::ScreenFormat::size, 24U);
}

BOXEDVN_TEST(x11_layout64_never_reuses_the_ia32_sizes) {
    // The 32-bit emulation models Display as 0xB4 bytes, XEvent as 96 and
    // XVisualInfo as 40. A bridge that handed those to a 64-bit Wine would
    // shift every field; none of the 64-bit sizes may collapse to them.
    CHECK(L::Display::size != 0xB4U);
    CHECK_EQ(L::XEvent::size, 192U);
    CHECK_EQ(L::XVisualInfo::size, 64U);
    CHECK_EQ(L::XSetWindowAttributes::size, 112U);
    CHECK_EQ(L::XWindowAttributes::size, 136U);
    CHECK_EQ(L::XSizeHints::size, 80U);
    CHECK_EQ(L::XWMHints::size, 56U);
    CHECK_EQ(L::XGCValues::size, 128U);
    CHECK_EQ(L::XImage::size, 136U);
    CHECK_EQ(L::XTextProperty::size, 32U);
    CHECK_EQ(L::XColor::size, 16U);
}

BOXEDVN_TEST(x11_layout64_event_prefix_and_client_message_data) {
    CHECK_EQ(L::XEvent::any::serial, 8U);
    CHECK_EQ(L::XEvent::any::send_event, 16U);
    CHECK_EQ(L::XEvent::any::display, 24U);
    CHECK_EQ(L::XEvent::any::window, 32U);
    CHECK_EQ(L::XEvent::key::time, 56U);
    CHECK_EQ(L::XEvent::key::state, 80U);
    CHECK_EQ(L::XEvent::key::keycode, 84U);
    CHECK_EQ(L::XEvent::configure::above, 72U);
    CHECK_EQ(L::XEvent::client::message_type, 40U);
    CHECK_EQ(L::XEvent::client::format, 48U);
    CHECK_EQ(L::XEvent::client::data, 56U);
    // Five longs of client data fit inside the union.
    CHECK(L::XEvent::client::data + 5 * 8 <= L::XEvent::size);
}

BOXEDVN_TEST(x11_layout64_field_helpers_keep_full_width_values) {
    uint8_t buffer[64];
    std::memset(buffer, 0xAA, sizeof(buffer));
    const uint64_t pointer = 0x00007a4001234568ULL;
    L::put64(buffer, L::Visual::visualid, pointer);
    L::put32(buffer, L::Visual::c_class, 0x80000004U);
    L::put16(buffer, 2, 0xBEEF);
    CHECK_EQ(L::get64(buffer, L::Visual::visualid), pointer);
    CHECK_EQ(L::get32(buffer, L::Visual::c_class), 0x80000004U);
    CHECK_EQ(L::get16(buffer, 2), 0xBEEFU);
    // Little-endian, byte-exact.
    CHECK_EQ(buffer[L::Visual::visualid], 0x68U);
    CHECK_EQ(buffer[L::Visual::visualid + 5], 0x7aU);
}
