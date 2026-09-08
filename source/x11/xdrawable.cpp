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
#include "x11.h"

// Resolve a bitmap once per operation, not once per pixel. GC rectangle and
// bitmap clips are alternatives; their origins are in destination coordinates.
class XPixelClip {
public:
    explicit XPixelClip(const std::shared_ptr<XGC>& gc) : gc(gc) {
        if (gc && gc->values.clip_mask)
            mask = XServer::getServer()->getDrawable(gc->values.clip_mask);
    }
    bool active() const { return gc && (gc->clipRectsSet || gc->values.clip_mask); }
    bool contains(S32 x, S32 y) const {
        if (!active()) return true;
        const S64 cx = (S64)x - gc->values.clip_x_origin;
        const S64 cy = (S64)y - gc->values.clip_y_origin;
        if (gc->clipRectsSet) {
            for (const auto& r : gc->clip_rects)
                if (cx >= r.x && cy >= r.y && cx < (S64)r.x+r.width && cy < (S64)r.y+r.height)
                    return true;
            return false;
        }
        if (!mask || cx < 0 || cy < 0 || cx >= mask->width() || cy >= mask->height()) return false;
        const U8* row = mask->getData() + cy * mask->getBytesPerLine();
        const U32 bits = mask->getBitsPerPixel();
        if (bits == 1) return (row[cx / 8] & (1u << (cx % 8))) != 0;
        // BoxedWine also represents expanded one-bit masks in color pixmaps.
        U32 pixel = 0;
        memcpy(&pixel, row + cx * (bits / 8), std::min<U32>(4, bits / 8));
        return pixel != 0;
    }
private:
    std::shared_ptr<XGC> gc;
    XDrawablePtr mask;
};

U32 x11ApplyRasterOperation(U32 source, U32 destination, S32 function,
	U32 planeMask) {
	U32 result;
	switch (function) {
	case GXclear: result = 0; break;
	case GXand: result = source & destination; break;
	case GXandReverse: result = source & ~destination; break;
	case GXcopy: result = source; break;
	case GXandInverted: result = ~source & destination; break;
	case GXnoop: result = destination; break;
	case GXxor: result = source ^ destination; break;
	case GXor: result = source | destination; break;
	case GXnor: result = ~(source | destination); break;
	case GXequiv: result = ~(source ^ destination); break;
	case GXinvert: result = ~destination; break;
	case GXorReverse: result = source | ~destination; break;
	case GXcopyInverted: result = ~source; break;
	case GXorInverted: result = ~source | destination; break;
	case GXnand: result = ~(source & destination); break;
	case GXset: result = 0xffffffff; break;
	default:
		// XChangeGC rejects values outside the protocol range. Preserve the
		// drawable if a malformed client nevertheless reaches this path.
		result = destination;
		break;
	}
	return (destination & ~planeMask) | (result & planeMask);
}

XDrawable::XDrawable(U32 width, U32 height, U32 depth, const VisualPtr& visual, bool isWindow, bool isPBuffer) : id(XServer::getNextId()), isWindow(isWindow), isPBuffer(isPBuffer), depth(depth), visual(visual), w(width), h(height) {
	data = nullptr;
	setSize(width, height);
}

XPBuffer::XPBuffer(U32 width, U32 height, U32 depth, const VisualPtr& visual, U32 fbConfigId, bool preservedContents, U32 eventMask)
	: XDrawable(width, height, depth, visual, false, true), fbConfigId(fbConfigId), preservedContents(preservedContents), eventMask(eventMask) {
	isOpenGL = true;
}

XDrawable::~XDrawable() {
	delete[] data;
}

U32 XDrawable::getImage(KThread* thread, S32 x, S32 y, U32 width, U32 height, U32 planeMask, U32 format, U32 redMask, U32 greenMask, U32 blueMask) {
	U32 image = thread->process->alloc(thread, sizeof(XImage));
	if (planeMask != AllPlanes) {
		kpanic_fmt("XDrawable::createXImage wasn't expecting planeMask = %x", planeMask);
	}
	if (format != ZPixmap) {
		kpanic_fmt("XDrawable::createXImage wasn't expecting format = %x", format);
	}
	U32 bytesPerLine = calculateBytesPerLine(width, visual->bits_per_rgb);
	U32 len = bytesPerLine * height;
	U32 data = thread->process->alloc(thread, len);

	U32 dst = data;
	U8* src = this->data + this->bytes_per_line * y + (visual->bits_per_rgb * x + 7) / 8;
	for (U32 y = 0; y < height; y++) {
		thread->memory->memcpy(dst, src, bytesPerLine);
		src += this->bytes_per_line;
		dst += bytesPerLine;
	}

	XImage::set(thread->memory, image, width, height, 0, format, data, 32, depth, bytesPerLine, visual->bits_per_rgb, redMask, greenMask, blueMask);
	return image;
}

U32 XDrawable::calculateBytesPerLine(U32 bitsPerPixel, U32 width) {
	U32 result = width * bitsPerPixel / 8;
	result = (result + 3) / 4 * 4;
	return result;
}

void XDrawable::lockData() {
	BOXEDWINE_MUTEX_LOCK(mutex);
}

void XDrawable::unlockData() {
	BOXEDWINE_MUTEX_UNLOCK(mutex);
}

void XDrawable::setSize(U32 width, U32 height) {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	w = width;
	h = height;
	bytes_per_line = calculateBytesPerLine(visual?visual->bits_per_rgb:32, width);
	size = isPBuffer ? 0 : height * bytes_per_line;
	if (data) {
		delete[] data;
		data = nullptr;
	}
	if (size) {
		data = new U8[size];
		memset(data, 0, size);
	}
}

int XDrawable::putImage(KThread* thread, const std::shared_ptr<XGC>& gc, XImage* image, S32 src_x, S32 src_y, S32 dest_x, S32 dest_y, U32 width, U32 height) {
	return copyImageData(thread, gc, image->data, image->bytes_per_line, image->bits_per_pixel, src_x, src_y, dest_x, dest_y, width, height);
}

int XDrawable::copyHostImageData(const std::shared_ptr<XGC>& gc, const U8* srcBase, U32 srcLength, U32 bytes_per_line, S32 bits_per_pixel, S32 src_x, S32 src_y, S32 dst_x, S32 dst_y, U32 width, U32 height) {
	if (bits_per_pixel != this->visual->bits_per_rgb) {
		return BadMatch;
	}
	if (!srcBase || src_x < 0 || src_y < 0) {
		return BadValue;
	}
	if (dst_x < 0) {
		const U64 skip = -(S64)dst_x;
		if (skip >= width) return Success;
		src_x += (S32)skip; width -= (U32)skip; dst_x = 0;
	}
	if (dst_y < 0) {
		const U64 skip = -(S64)dst_y;
		if (skip >= height) return Success;
		src_y += (S32)skip; height -= (U32)skip; dst_y = 0;
	}
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	if (dst_x + width > w) {
		if ((S32)w < dst_x) {
			return Success;
		}
		width = w - dst_x;
	}
	if (dst_y + (S32)height > (S32)h) {
		if ((S32)h < dst_y) {
			return Success;
		}
		height = h - dst_y;
	}
	const U32 copyPerLine = (bits_per_pixel * width + 7) / 8;
	U64 srcOffset = (U64)bytes_per_line * (U64)src_y + (U64)(bits_per_pixel * src_x + 7) / 8;
	U8* dst = this->data + this->bytes_per_line * dst_y + (bits_per_pixel * dst_x + 7) / 8;
	const S32 rasterFunction = gc ? gc->values.function : GXcopy;
	const U32 planeMask = gc ? gc->values.plane_mask : 0xffffffff;
	const XPixelClip clip(gc);
	if ((clip.active() || rasterFunction != GXcopy || planeMask != 0xffffffff) && bits_per_pixel != 32)
		return BadImplementation;
	for (U32 y = 0; y < height; y++) {
		if (srcOffset + copyPerLine > srcLength) {
			return BadValue;
		}
		if (!clip.active() && rasterFunction == GXcopy && planeMask == 0xffffffff) {
			memcpy(dst, srcBase + srcOffset, copyPerLine);
		} else {
			for (U32 x = 0; x < width; ++x) {
				if (!clip.contains(dst_x + x, dst_y + y)) continue;
				U32 source, target;
				memcpy(&source, srcBase + srcOffset + x*4, 4);
				memcpy(&target, dst + x*4, 4);
				target = x11ApplyRasterOperation(source, target, rasterFunction, planeMask);
				memcpy(dst + x*4, &target, 4);
			}
		}
		srcOffset += bytes_per_line;
		dst += this->bytes_per_line;
	}
	setDirtyRect(dst_x, dst_y, width, height);
	return Success;
}

int XDrawable::copyImageData(KThread* thread, const std::shared_ptr<XGC>& gc, U32 data, U32 bytes_per_line, S32 bits_per_pixel, S32 src_x, S32 src_y, S32 dst_x, S32 dst_y, U32 width, U32 height) {
	if (bits_per_pixel != this->visual->bits_per_rgb) {
		return BadMatch;
	}
	if (gc && (gc->clip_rects.size() || gc->values.clip_mask || gc->values.clip_x_origin || gc->values.clip_y_origin)) {
		//klog("XDrawable::copyImageData clipping not implemented");
	}
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	U32 src = data + bytes_per_line * src_y + (bits_per_pixel * src_x + 7) / 8;
	U8* dst = this->data + this->bytes_per_line * dst_y + (bits_per_pixel * dst_x + 7) / 8;
	KMemory* memory = thread->memory;
	if (dst_x + width > w) {
		if ((S32)w < dst_x) {
			return Success;
		}
		width = w - dst_x;
	}
	if (dst_y + (S32)height > (S32)h) {
		if ((S32)h < dst_y) {
			return Success;
		}
		height = h - dst_y;
	}
	U32 copyPerLine = (bits_per_pixel * width + 7) / 8;

	const S32 rasterFunction = gc ? gc->values.function : GXcopy;
	const U32 planeMask = gc ? gc->values.plane_mask : 0xffffffff;
	if (rasterFunction == GXcopy && planeMask == 0xffffffff) {
		for (U32 y = 0; y < height; y++) {
			if (!memory->canRead(src, copyPerLine)) {
				return BadValue;
			}
			memory->memcpy(dst, src, copyPerLine);
			src += bytes_per_line;
			dst += this->bytes_per_line;
		}
	} else if (bits_per_pixel == 32) {
		for (U32 y = 0; y < height; y++) {
			if (!memory->canRead(src, copyPerLine)) {
				return BadValue;
			}
			U32* dstPixel = (U32*)dst;
			for (U32 x = 0; x < width; x++) {
				dstPixel[x] = x11ApplyRasterOperation(
					memory->readd(src + x * 4), dstPixel[x],
					rasterFunction, planeMask);
			}			
			src += bytes_per_line;
			dst += this->bytes_per_line;
		}
	} else {
		// Boxedwine's current X11 visual is 32-bit, but keep the operation
		// correct for byte-aligned fallback visuals as well. Applying the
		// boolean operation bytewise is equivalent to applying it per pixel.
		const U32 bytesPerPixel = (U32)bits_per_pixel / 8;
		if (!bytesPerPixel || bits_per_pixel % 8) {
			return BadMatch;
		}
		for (U32 y = 0; y < height; y++) {
			if (!memory->canRead(src, copyPerLine)) {
				return BadValue;
			}
			for (U32 x = 0; x < width; x++) {
				for (U32 byte = 0; byte < bytesPerPixel; byte++) {
					const U32 shift = byte * 8;
					const U8 sourceByte = memory->readb(
						src + x * bytesPerPixel + byte);
					const U8 maskByte = (U8)(planeMask >> shift);
					dst[x * bytesPerPixel + byte] = (U8)
						x11ApplyRasterOperation(sourceByte,
							dst[x * bytesPerPixel + byte],
							rasterFunction, maskByte);
				}
			}
			src += bytes_per_line;
			dst += this->bytes_per_line;
		}
	}
	setDirtyRect(dst_x, dst_y, width, height);
	return Success;
}

int XDrawable::copy(KThread* thread, const std::shared_ptr<XGC>& gc, const std::shared_ptr<XDrawable>& srcDrawable, S32 srcX, S32 srcY, U32 width, U32 height, S32 dstX, S32 dstY) {
    if (srcDrawable->visual->bits_per_rgb != visual->bits_per_rgb) return BadMatch;
    if (srcX < 0) {
        const U64 skip = -(S64)srcX;
        if (skip >= width) return Success;
        width -= (U32)skip; dstX += (S32)skip; srcX = 0;
    }
    if (srcY < 0) {
        const U64 skip = -(S64)srcY;
        if (skip >= height) return Success;
        height -= (U32)skip; dstY += (S32)skip; srcY = 0;
    }
    if ((U32)srcX >= srcDrawable->w || (U32)srcY >= srcDrawable->h) return Success;
    width = std::min(width, srcDrawable->w-(U32)srcX);
    height = std::min(height, srcDrawable->h-(U32)srcY);
    if (!width || !height) return Success;
    const U32 bits = visual->bits_per_rgb;
    if (bits % 8) return BadImplementation;
    const U32 stride = width * (bits/8);
    // Snapshot before writing, including self-overlapping scroll copies. The
    // destination path applies raster operations, plane masks and GC clipping.
    std::vector<U8> pixels((size_t)stride * height);
    srcDrawable->lockData();
    for (U32 y = 0; y < height; ++y)
        memcpy(pixels.data() + (size_t)y*stride,
            srcDrawable->data + (size_t)(srcY+y)*srcDrawable->bytes_per_line + (size_t)srcX*(bits/8), stride);
    srcDrawable->unlockData();
    return copyHostImageData(gc, pixels.data(), (U32)pixels.size(), stride, bits,
                             0, 0, dstX, dstY, width, height);
}

int XDrawable::drawLine(KThread* thread, const std::shared_ptr<XGC>& gc, S32 x1, S32 y1, S32 x2, S32 y2) {
    if (visual->bits_per_rgb != 32) return BadMatch;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
    const XPixelClip clip(gc);
    const S64 dx = std::abs((S64)x2-x1), dy = -std::abs((S64)y2-y1);
    const S32 sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    S64 error = dx + dy;
    const S32 left = std::min(x1,x2), top = std::min(y1,y2);
    for (;;) {
        const bool last = x1 == x2 && y1 == y2;
        if ((!last || gc->values.cap_style != CapNotLast) && x1 >= 0 && y1 >= 0 &&
            (U32)x1 < w && (U32)y1 < h && clip.contains(x1,y1)) {
            U32* pixel = (U32*)(data + (size_t)y1*bytes_per_line + (size_t)x1*4);
            *pixel = x11ApplyRasterOperation(gc->values.foreground, *pixel, gc->values.function, gc->values.plane_mask);
        }
        if (last) break;
        const S64 twice = error*2;
        if (twice >= dy) { error += dy; x1 += sx; }
        if (twice <= dx) { error += dx; y1 += sy; }
    }
    setDirtyRect(left, top, (U32)dx+1, (U32)-dy+1);
    return Success;
}

int XDrawable::fillRectangle(KThread* thread, const std::shared_ptr<XGC>& gc, S32 x, S32 y, U32 width, U32 height) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
    if (x < 0) {
        const U64 skip = -(S64)x;
        if (skip >= width) return Success;
        width -= (U32)skip; x = 0;
    }
    if (y < 0) {
        const U64 skip = -(S64)y;
        if (skip >= height) return Success;
        height -= (U32)skip; y = 0;
    }
    if ((U32)x >= w || (U32)y >= h) return Success;
    width = std::min(width, w-(U32)x);
    height = std::min(height, h-(U32)y);
    if (gc->values.tile) return BadImplementation;

	// A stippled fill paints a pattern, not a colour. This port cannot hold
	// the pattern: op_CREATE_PIXMAP builds every pixmap with the source
	// drawable's visual and ignores the depth the client asked for, so a
	// stipple is a 32-bit pixmap, and copyImageData above refuses a 1-bit
	// XPutImage into it (bits_per_pixel != visual->bits_per_rgb -> BadMatch).
	// The stipple therefore stays as setSize left it, all zeroes, and every
	// bit of the pattern is unknown rather than clear. Painting the
	// foreground over the whole rectangle - what this did - is a guess, and
	// on the Wine desktop it is the guess that decides an 800x600 erase:
	// explorer's WM_ERASEBKGND reaches X as XChangeGC(GCStipple|GCFillStyle|
	// GCForeground|GCBackground) followed by XFillRectangle over the whole
	// desktop window, so one flat colour replaced everything underneath.
	// Leaving the pixels alone keeps whatever the surface already shows,
	// which is the window's background rather than half of a pattern.
	const S32 fillStyle = gc->values.fill_style;
	const bool patterned = fillStyle != FillSolid;
	if (patterned) {
		bool patternReadable = false;
		if (fillStyle == FillStippled || fillStyle == FillOpaqueStippled) {
			XDrawablePtr stipple = gc->values.stipple ?
				XServer::getServer()->getDrawable(gc->values.stipple) : nullptr;
			if (stipple && stipple->getData() && stipple->width() &&
				stipple->height() && stipple->getBitsPerPixel() == 32) {
				const U8* bits = stipple->getData();
				const U32 stipplePitch = stipple->getBytesPerLine();
				for (U32 sy = 0; sy < stipple->height() && !patternReadable;
					sy++) {
					const U32* line = (const U32*)(bits + (U64)stipplePitch * sy);
					for (U32 sx = 0; sx < stipple->width(); sx++) {
						if (line[sx]) {
							patternReadable = true;
							break;
						}
					}
				}
			}
		}
		static U32 reportedPatternFill = 0;
		if (reportedPatternFill < 8) {
			reportedPatternFill++;
			klog_fmt("BOXEDWINE_X11_FILL_RECT drawable=0x%x style=%d "
				"fg=0x%06x bg=0x%06x stipple=0x%x tile=0x%x rect=%d,%d %ux%u "
				"painted=%d",
				id, (int)fillStyle, gc->values.foreground,
				gc->values.background, gc->values.stipple, gc->values.tile,
				(int)x, (int)y, width, height, patternReadable ? 1 : 0);
		}
		if (!patternReadable) {
			return Success;
		}
	}

	if (visual->bits_per_rgb == 32) {
		U32* p = (U32*)data;
		const U32 color = gc->values.foreground;
		const U32 background = gc->values.background;
		const XPixelClip clip(gc);
		const XDrawablePtr stipple = patterned && gc->values.stipple ?
			XServer::getServer()->getDrawable(gc->values.stipple) : nullptr;
		p += bytes_per_line / 4 * y;
		for (U32 dstY = 0; dstY < height; dstY++) {
			for (U32 dstX = 0; dstX < width; dstX++) {
				if (!clip.contains(x + dstX, y + dstY)) continue;
				if (stipple) {
					// The stipple repeats from the tile/stipple origin.
					const U32 sx = (U32)(((S64)x + dstX - gc->values.ts_x_origin) %
						(S64)stipple->width() + stipple->width()) % stipple->width();
					const U32 sy = (U32)(((S64)y + dstY - gc->values.ts_y_origin) %
						(S64)stipple->height() + stipple->height()) % stipple->height();
					const U32* line = (const U32*)(stipple->getData() +
						(U64)stipple->getBytesPerLine() * sy);
					if (line[sx]) {
						p[x + dstX] = x11ApplyRasterOperation(color, p[x + dstX], gc->values.function, gc->values.plane_mask);
					} else if (fillStyle == FillOpaqueStippled) {
						p[x + dstX] = x11ApplyRasterOperation(background, p[x + dstX], gc->values.function, gc->values.plane_mask);
					}
					continue;
				}
				p[x + dstX] = x11ApplyRasterOperation(color, p[x + dstX], gc->values.function, gc->values.plane_mask);
			}
			p += bytes_per_line/4;
		}
	} else {
        kwarn_fmt("XDrawable::fillRectangle only %d-bit not handled", visual->bits_per_rgb);
	}
	setDirtyRect(x, y, width, height);
	return Success;
}

int XDrawable::drawRectangle(KThread* thread, const std::shared_ptr<XGC>& gc, S32 x, S32 y, U32 width, U32 height) {
	if (gc->clip_rects.size() || gc->values.clip_mask || gc->values.clip_x_origin || gc->values.clip_y_origin) {
		klog("XDrawable::drawRectangle clipping not implemented");
	}
	// draw rectangle with no overlapping pixels
	drawLine(thread, gc, x, y, x + width, y); // top (includes left and right)
	drawLine(thread, gc, x + width, y + 1, x + width, y + height); // right	( includes bottom but not top)
	drawLine(thread, gc, x, y + 1, x, y + height); // left (includes bottom but not top)
	drawLine(thread, gc, x + 1, y + height, x + width - 1, y + height); // bottom (does not include left and right)
	return Success;
}
