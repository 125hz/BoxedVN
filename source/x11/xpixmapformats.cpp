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
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  See xpixmapformats.h for why this list is not the same as the visual list.
 */

#include "xpixmapformats.h"

#include <algorithm>

namespace {

// X pads every scanline to a 32-bit boundary. Boxedwine has always reported
// this and nothing depends on it varying by depth.
const uint32_t SCANLINE_PAD = 32;

// Depths an X server provides for off-screen drawables whether or not it has
// a matching visual, with the bits per pixel a real server reports for them.
// Depth 4 is padded out to a whole byte exactly as Xorg does.
const XPixmapFormatEntry NON_VISUAL_FORMATS[] = {
    {1, 1, SCANLINE_PAD},
    {4, 8, SCANLINE_PAD},
};

}  // namespace

std::vector<XPixmapFormatEntry> xBuildPixmapFormats(
    const std::vector<XPixmapFormatEntry>& visualFormats) {
    std::vector<XPixmapFormatEntry> formats;
    formats.reserve(visualFormats.size() +
                    sizeof(NON_VISUAL_FORMATS) / sizeof(NON_VISUAL_FORMATS[0]));

    // A screen may list the same depth more than once when it has several
    // visuals at that depth. XListPixmapFormats reports formats, not visuals,
    // so one entry per depth is correct - and a duplicate would make a client
    // that walks the list index the same slot twice.
    for (const XPixmapFormatEntry& format : visualFormats) {
        const bool alreadyPresent = std::any_of(
            formats.begin(), formats.end(),
            [&format](const XPixmapFormatEntry& existing) {
                return existing.depth == format.depth;
            });
        if (!alreadyPresent) {
            formats.push_back(format);
        }
    }

    for (const XPixmapFormatEntry& format : NON_VISUAL_FORMATS) {
        const bool alreadyPresent = std::any_of(
            formats.begin(), formats.end(),
            [&format](const XPixmapFormatEntry& existing) {
                return existing.depth == format.depth;
            });
        if (!alreadyPresent) {
            formats.push_back(format);
        }
    }

    std::sort(formats.begin(), formats.end(),
              [](const XPixmapFormatEntry& a, const XPixmapFormatEntry& b) {
                  return a.depth < b.depth;
              });
    return formats;
}
