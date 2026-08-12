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

#ifndef __X_VISUAL_INFO_H__
#define __X_VISUAL_INFO_H__

#include <stddef.h>

struct XVisualInfo {
	VisualPtrAddress visual;
	VisualID visualid;
	S32 screen;
	S32 depth;
	S32 c_class;
	U32 red_mask;
	U32 green_mask;
	U32 blue_mask;
	S32 colormap_size;
	S32 bits_per_rgb;

	void set(S32 screenIndex, U32 visualAddress, U32 depth, Visual* visual);
	void read(KMemory* memory, U32 address);
	void write(KMemory* memory, U32 address);
	bool match(U32 mask, S32 screenIndex, const Depth* depth, const Visual* visual);
};

static_assert(sizeof(XVisualInfo) == 40, "emulation expects sizeof(XVisualInfo) to be 40");

// XGetVisualInfo hands the guest a flat array of these, so a client reads each
// field at a fixed byte offset from the pointer it was given. Getting that
// wrong does not fail loudly: the client keeps running with a visual whose
// depth, class and colour masks are all somebody else's. Pin the offsets Xlib
// clients compile against.
static_assert(offsetof(XVisualInfo, visual) == 0, "XVisualInfo.visual must be at offset 0");
static_assert(offsetof(XVisualInfo, visualid) == 4, "XVisualInfo.visualid must be at offset 4");
static_assert(offsetof(XVisualInfo, screen) == 8, "XVisualInfo.screen must be at offset 8");
static_assert(offsetof(XVisualInfo, depth) == 12, "XVisualInfo.depth must be at offset 12");
static_assert(offsetof(XVisualInfo, c_class) == 16, "XVisualInfo.class must be at offset 16");
static_assert(offsetof(XVisualInfo, red_mask) == 20, "XVisualInfo.red_mask must be at offset 20");
static_assert(offsetof(XVisualInfo, green_mask) == 24, "XVisualInfo.green_mask must be at offset 24");
static_assert(offsetof(XVisualInfo, blue_mask) == 28, "XVisualInfo.blue_mask must be at offset 28");
static_assert(offsetof(XVisualInfo, colormap_size) == 32, "XVisualInfo.colormap_size must be at offset 32");
static_assert(offsetof(XVisualInfo, bits_per_rgb) == 36, "XVisualInfo.bits_per_rgb must be at offset 36");

#endif