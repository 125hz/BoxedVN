/* BoxedVN - X11 pixmap-format list regression tests. GPLv2. */

#include "boxedvn_test.h"

#include "xpixmapformats.h"

namespace {

const XPixmapFormatEntry* find(const std::vector<XPixmapFormatEntry>& formats,
                               uint32_t depth) {
    for (const XPixmapFormatEntry& format : formats) {
        if (format.depth == depth) {
            return &format;
        }
    }
    return nullptr;
}

// What XServer::initDepths/initVisuals produce for a 32-bit screen.
std::vector<XPixmapFormatEntry> boxedwineVisualFormats() {
    return {{32, 32, 32}, {24, 32, 32}, {16, 16, 32}, {8, 8, 32}};
}

}  // namespace

BOXEDVN_TEST(pixmap_formats_always_include_depth_one) {
    // Wine indexes pixmap_formats[] by depth and dereferences the entry when
    // it fills in a BITMAPINFOHEADER. Depth 1 has no visual on any server but
    // every monochrome mask is a depth-1 pixmap, so a missing entry is a null
    // dereference inside winex11 rather than a graceful failure.
    const std::vector<XPixmapFormatEntry> formats =
        xBuildPixmapFormats(boxedwineVisualFormats());

    const XPixmapFormatEntry* monochrome = find(formats, 1);
    CHECK(monochrome != nullptr);
    if (monochrome != nullptr) {
        CHECK_EQ(monochrome->bitsPerPixel, 1U);
        CHECK_EQ(monochrome->scanlinePad, 32U);
    }
}

BOXEDVN_TEST(pixmap_formats_keep_every_visual_depth) {
    const std::vector<XPixmapFormatEntry> formats =
        xBuildPixmapFormats(boxedwineVisualFormats());

    for (const XPixmapFormatEntry& expected : boxedwineVisualFormats()) {
        const XPixmapFormatEntry* actual = find(formats, expected.depth);
        CHECK(actual != nullptr);
        if (actual != nullptr) {
            CHECK_EQ(actual->bitsPerPixel, expected.bitsPerPixel);
        }
    }
}

BOXEDVN_TEST(pixmap_formats_pad_four_bit_depth_to_a_byte) {
    const std::vector<XPixmapFormatEntry> formats =
        xBuildPixmapFormats(boxedwineVisualFormats());
    const XPixmapFormatEntry* nibble = find(formats, 4);
    CHECK(nibble != nullptr);
    if (nibble != nullptr) {
        CHECK_EQ(nibble->bitsPerPixel, 8U);
    }
}

BOXEDVN_TEST(pixmap_formats_never_repeat_a_depth) {
    // Several visuals can share a depth. The format list describes formats,
    // not visuals, so a client that walks it must not see the same depth
    // twice.
    const std::vector<XPixmapFormatEntry> formats = xBuildPixmapFormats(
        {{32, 32, 32}, {32, 32, 32}, {24, 32, 32}, {24, 32, 32}});

    unsigned int count32 = 0;
    unsigned int count24 = 0;
    for (const XPixmapFormatEntry& format : formats) {
        count32 += (format.depth == 32) ? 1 : 0;
        count24 += (format.depth == 24) ? 1 : 0;
    }
    CHECK_EQ(count32, 1U);
    CHECK_EQ(count24, 1U);
}

BOXEDVN_TEST(pixmap_formats_do_not_override_a_depth_the_server_describes) {
    // A server that really does have a depth-1 visual keeps its own numbers.
    const std::vector<XPixmapFormatEntry> formats =
        xBuildPixmapFormats({{1, 8, 32}});
    const XPixmapFormatEntry* monochrome = find(formats, 1);
    CHECK(monochrome != nullptr);
    if (monochrome != nullptr) {
        CHECK_EQ(monochrome->bitsPerPixel, 8U);
    }
}

BOXEDVN_TEST(pixmap_formats_are_sorted_by_depth) {
    const std::vector<XPixmapFormatEntry> formats =
        xBuildPixmapFormats(boxedwineVisualFormats());
    CHECK(formats.size() >= 6);
    for (std::size_t i = 1; i < formats.size(); ++i) {
        CHECK(formats[i - 1].depth < formats[i].depth);
    }
}
