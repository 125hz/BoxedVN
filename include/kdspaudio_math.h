/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __KDSPAUDIO_MATH_H__
#define __KDSPAUDIO_MATH_H__

namespace KDspAudioMath {
    // Cover two host callbacks per default fragment. A translated producer
    // can miss a callback while compiling shaders/CPU blocks. Wine queues
    // three fragments ahead, and the capacity below must permit all three.
    inline U32 getDefaultFragmentSize(U32 guestBytesPerSecond, U32 hostRate, U32 hostFrames) {
        if (!hostRate) return 4096;
        U64 needed = ((U64)guestBytesPerSecond * hostFrames * 2 + hostRate - 1) / hostRate;
        U32 size = 4096;
        while (size < needed && size < 16384) size *= 2;
        return size;
    }
	inline U32 getRequestedSdlPeriodFrames() {
#ifdef __EMSCRIPTEN__
		return 4096;
#else
		return 1024;
#endif
	}

	inline U32 getAlignedDurationBytes(U32 bytesPerSecond, U32 durationMs, U32 frameSize) {
		if (!frameSize) {
			return 0;
		}
		U32 bytes = (U32)((U64)bytesPerSecond * durationMs / 1000);
		return bytes & ~(frameSize - 1);
	}

	inline U32 getWriteCapacity(U32 bytesPerSecond, U32 fragmentSize, U32 bufferSize) {
		U32 capacity = bytesPerSecond / 8;
		if (capacity < (U64)fragmentSize * 3) {
			const U64 fragments = (U64)fragmentSize * 3;
			capacity = fragments > bufferSize ? bufferSize : (U32)fragments;
		}
		if (capacity > bufferSize) {
			capacity = bufferSize;
		}
		return capacity;
	}

	inline U32 getAvailableWriteBytes(U32 capacity, U32 queued) {
		return queued >= capacity ? 0 : capacity - queued;
	}

	inline U32 getOutputSpaceAvailable(U32 capacity, U32 used, bool accountForQueuedAudio) {
		return accountForQueuedAudio ? getAvailableWriteBytes(capacity, used) : capacity;
	}

	inline U32 getQueuedAfterElapsed(U32 queued, U32 bytesPerSecond, U32 elapsedMs) {
		U64 consumed = (U64)bytesPerSecond * elapsedMs / 1000;
		return consumed >= queued ? 0 : queued - (U32)consumed;
	}

	inline U32 alignWriteBytes(U32 bytes, U32 blockSize) {
		if (!blockSize) {
			return 0;
		}
		return bytes & ~(blockSize - 1);
	}

	inline U32 getWritableBytes(U32 requested, U32 capacity, U32 queued, U32 blockSize) {
		U32 available = getAvailableWriteBytes(capacity, queued);
		U32 writable = requested < available ? requested : available;
		return alignWriteBytes(writable, blockSize);
	}
}

#endif
