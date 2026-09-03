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
#include "knativesystem.h"

U32 XrrConfigCurrentRate() {
    return KNativeSystem::getScreen()->screenRate();
}

// The sizes a full-screen program expects to find when it enumerates display
// modes. A list with one entry -- which is what winex11's `nores` handler
// reports, and what the XRandR 1.1 path below narrows to at a small desktop
// -- makes such a program refuse to start or pick the only thing it is
// offered. These are the modes any Windows driver would report.
static const U32 kStandardModeSizes[][2] = {
    { 640, 480 },
    { 800, 600 },
    { 1024, 768 },
    { 1280, 720 },
    { 1280, 800 },
    { 1366, 768 },
    { 1600, 900 },
    { 1920, 1080 },
};

U32 XrrStandardModeList(U32 desktopCx, U32 desktopCy, U32 currentRate, XrrModeEntry* out, U32 maxOut) {
    if (!out || !maxOut) {
        return 0;
    }
    const U32 maxSizes = (U32)(sizeof(kStandardModeSizes) / sizeof(kStandardModeSizes[0])) + 1;
    U32 sizes[(sizeof(kStandardModeSizes) / sizeof(kStandardModeSizes[0])) + 1][2];
    U32 sizeCount = 0;

    // The desktop size first, so it survives a full list; the rest follow and
    // duplicates of it are dropped.
    if (desktopCx && desktopCy) {
        sizes[sizeCount][0] = desktopCx;
        sizes[sizeCount][1] = desktopCy;
        sizeCount++;
    }
    for (U32 i = 0; i < sizeof(kStandardModeSizes) / sizeof(kStandardModeSizes[0]); i++) {
        const U32 cx = kStandardModeSizes[i][0];
        const U32 cy = kStandardModeSizes[i][1];
        bool duplicate = false;
        for (U32 j = 0; j < sizeCount; j++) {
            if (sizes[j][0] == cx && sizes[j][1] == cy) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && sizeCount < maxSizes) {
            sizes[sizeCount][0] = cx;
            sizes[sizeCount][1] = cy;
            sizeCount++;
        }
    }

    U32 rates[3];
    U32 rateCount = 0;
    rates[rateCount++] = XRR_MODE_RATE_PRIMARY;
    rates[rateCount++] = XRR_MODE_RATE_HIGH;
    if (currentRate && currentRate != XRR_MODE_RATE_PRIMARY && currentRate != XRR_MODE_RATE_HIGH) {
        rates[rateCount++] = currentRate;
    }

    // Ascending by width, then height: a caller that takes the first match
    // for a size gets the same answer every time, and the list reads the way
    // an EnumDisplaySettings caller expects.
    for (U32 i = 0; i + 1 < sizeCount; i++) {
        for (U32 j = 0; j + 1 < sizeCount - i; j++) {
            if (sizes[j][0] > sizes[j + 1][0] ||
                (sizes[j][0] == sizes[j + 1][0] && sizes[j][1] > sizes[j + 1][1])) {
                const U32 cx = sizes[j][0];
                const U32 cy = sizes[j][1];
                sizes[j][0] = sizes[j + 1][0];
                sizes[j][1] = sizes[j + 1][1];
                sizes[j + 1][0] = cx;
                sizes[j + 1][1] = cy;
            }
        }
    }
    for (U32 i = 0; i + 1 < rateCount; i++) {
        for (U32 j = 0; j + 1 < rateCount - i; j++) {
            if (rates[j] > rates[j + 1]) {
                const U32 rate = rates[j];
                rates[j] = rates[j + 1];
                rates[j + 1] = rate;
            }
        }
    }

    U32 count = 0;
    for (U32 i = 0; i < sizeCount; i++) {
        for (U32 j = 0; j < rateCount; j++) {
            if (count >= maxOut) {
                return count;
            }
            out[count].width = sizes[i][0];
            out[count].height = sizes[i][1];
            out[count].rate = rates[j];
            count++;
        }
    }
    return count;
}

bool XrrGetSize(KThread* thread, const DisplayDataPtr& displayData, U32 sizeIndex, U32& cx, U32& cy) {
    KMemory* memory = thread->memory;
    XrrData* data = displayData->xrrData;

    if (!data || !data->sizesAddress) {
        XrrGetSizes(thread, displayData, 0, 0);
        data = displayData->xrrData;
    }
    if (sizeIndex >= data->sizesCount) {
        return false;
    }

    cx = memory->readd(data->sizesAddress + sizeIndex * 16);
    cy = memory->readd(data->sizesAddress + sizeIndex * 16 + 4);
    return true;
}

U32 XrrRates(KThread* thread, const DisplayDataPtr& displayData, U32 screen, U32 sizeIndex, U32 rateCountAddress) {
    KMemory* memory = thread->memory;
    XrrData* data = displayData->xrrData;

    memory->writed(rateCountAddress, 1);
    if (data->ratesAddress) {
        return data->ratesAddress;
    }
    data->ratesAddress = thread->process->alloc(thread, 2);
    memory->writew(data->ratesAddress, XrrConfigCurrentRate());
    return data->ratesAddress;
}

U32 XrrConfigCurrentConfiguration(KThread* thread, const DisplayDataPtr& displayData, U32 rotationAddress) {
    KMemory* memory = thread->memory;
    XrrData* data = displayData->xrrData;

    if (!data || !data->sizesAddress) {
        XrrGetSizes(thread, displayData, 0, 0);
        data = displayData->xrrData;
    }
    KNativeScreenPtr screen = KNativeSystem::getScreen();
    U32 cx = screen->screenWidth();
    U32 cy = screen->screenHeight();

    memory->writed(rotationAddress, 0);
    for (U32 i = 0; i < data->sizesCount; i++) {
        if (memory->readd(data->sizesAddress + i * 16) == cx && memory->readd(data->sizesAddress + i * 16 + 4) == cy) {
            return i;
        }
    }
    return 0;
}

U32 XrrGetSizes(KThread* thread, const DisplayDataPtr& data, U32 screen, U32 countAddress) {
    KMemory* memory = thread->memory;

    if (!data->xrrData) {
        data->xrrData = new XrrData();
    }
    if (data->xrrData->sizesAddress) {
        if (countAddress) {
            memory->writed(countAddress, data->xrrData->sizesCount);
        }
        return data->xrrData->sizesAddress;
    }
    U32 desktopCx = 0;
    U32 desktopCy = 0;
    U32 count = 3;
    KNativeSystem::getScreenDimensions(&desktopCx, &desktopCy);

    if (desktopCx > 1600) {
        count++;
    }
    if (desktopCx && desktopCx > 1600 && desktopCy > 1200) {
        count++;
    }
    if (desktopCx && desktopCx > 1280 && desktopCy > 1024) {
        count++;
    }
    U32 sizes = thread->process->alloc(thread, sizeof(XRRScreenSize) * count);
    
    data->xrrData->sizesAddress = sizes;
    data->xrrData->sizesCount = count;
    if (desktopCx > 1600) {
        memory->writed(sizes, desktopCx); sizes += 4;
        memory->writed(sizes, desktopCy); sizes += 4;
        memory->writed(sizes, 0); sizes += 4;
        memory->writed(sizes, 0); sizes += 4;
    }
    if (desktopCx && desktopCx > 1600 && desktopCy > 1200) {
        memory->writed(sizes, 1600); sizes += 4;
        memory->writed(sizes, 1200); sizes += 4;
        memory->writed(sizes, 0); sizes += 4;
        memory->writed(sizes, 0); sizes += 4;
    }
    if (desktopCx && desktopCx > 1280 && desktopCy > 1024) {
        memory->writed(sizes, 1280); sizes += 4;
        memory->writed(sizes, 1024); sizes += 4;
        memory->writed(sizes, 0); sizes += 4;
        memory->writed(sizes, 0); sizes += 4;
    }
    memory->writed(sizes, 1024); sizes += 4;
    memory->writed(sizes, 768); sizes += 4;
    memory->writed(sizes, 0); sizes += 4;
    memory->writed(sizes, 0); sizes += 4;
    memory->writed(sizes, 800); sizes += 4;
    memory->writed(sizes, 600); sizes += 4;
    memory->writed(sizes, 0); sizes += 4;
    memory->writed(sizes, 0); sizes += 4;
    memory->writed(sizes, 640); sizes += 4;
    memory->writed(sizes, 480); sizes += 4;
    memory->writed(sizes, 0); sizes += 4;
    memory->writed(sizes, 0);
    if (countAddress) {
        memory->writed(countAddress, data->xrrData->sizesCount);
    }
    return data->xrrData->sizesAddress;
}
