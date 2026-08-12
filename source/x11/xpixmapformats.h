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
 *  ---------------------------------------------------------------------
 *  What XListPixmapFormats has to return.
 *
 *  A screen's *visual* depths and its *pixmap* formats are different lists,
 *  and conflating them crashes X clients. A visual describes what a window
 *  can be created with; a pixmap format describes what an off-screen
 *  drawable can be created with. Every X server supports depth-1 pixmaps -
 *  that is what a bitmap is - even though no server offers a depth-1 visual.
 *
 *  Wine's X11 driver builds `pixmap_formats[]` indexed directly by depth from
 *  whatever XListPixmapFormats returns, and then dereferences
 *  `pixmap_formats[depth]->bits_per_pixel` when it fills in a
 *  BITMAPINFOHEADER. A depth missing from the list is a NULL entry and a null
 *  dereference inside the driver, not a graceful failure. Monochrome bitmaps
 *  are everywhere in Windows - cursor and icon masks, region masks,
 *  ImageList masks, any CreateBitmap(w, h, 1, 1, NULL) - so a server that
 *  omits depth 1 breaks a large amount of ordinary drawing.
 *  ---------------------------------------------------------------------
 */

#ifndef __X_PIXMAP_FORMATS_H__
#define __X_PIXMAP_FORMATS_H__

#include <stdint.h>
#include <vector>

struct XPixmapFormatEntry {
    uint32_t depth;
    uint32_t bitsPerPixel;
    uint32_t scanlinePad;

    bool operator==(const XPixmapFormatEntry& other) const {
        return depth == other.depth && bitsPerPixel == other.bitsPerPixel &&
               scanlinePad == other.scanlinePad;
    }
};

// Returns the complete pixmap-format list for a screen whose visuals cover
// `visualFormats`. Entries are returned in ascending depth order, which is
// what a real server does and what makes the list readable in a client trace.
//
// The caller passes only the depths it has visuals for. This adds the formats
// an X server must support for off-screen drawables regardless of its
// visuals, and never replaces or duplicates a depth the caller already
// described.
std::vector<XPixmapFormatEntry> xBuildPixmapFormats(
    const std::vector<XPixmapFormatEntry>& visualFormats);

#endif  // __X_PIXMAP_FORMATS_H__
