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
#include "kdspaudio.h"
#include "kdspaudio_math.h"
#include "sdl_audio_converter.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include "../../source/kernel/devs/oss.h"

#define DSP_BUFFER_SIZE (1024*32)

// SDL's iOS backend picks the *ambient* AVAudioSession category by default,
// which is silenced by the hardware ring/silent switch and ducked by whatever
// else is playing. Nothing in ios/ configures AVAudioSession itself (a grep
// for AVAudioSession, AVFAudio and setCategory over ios/ finds nothing), so
// without this a user with the switch on hears no guest audio at all and has
// no way to tell that from a broken driver.
//
// SDL_SetHint uses SDL_HINT_NORMAL priority, and SDL_GetHint prefers the
// environment variable over a NORMAL-priority hint, so setting SDL_AUDIO_CATEGORY
// in the environment still wins. The hint is read by coreaudio's
// update_audio_session() when the device is opened, so it has to be in place
// before SDL_OpenAudioDevice and not merely before SDL_Init.
static void ensureAudioSessionCategory() {
	// KNativeSystem::cleanup calls SDL_Quit, which clears all hints. Reapply
	// before every open, including later guest sessions in the same iOS app.
	SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
}

// CoreAudio keeps a backend-wide list of queues and session reference counts.
// Serialize open/close across independent guest streams and the drain timer.
// Queueing and callbacks never take this lock; it is below pendingClosesMutex.
static std::mutex dspDeviceLifecycleMutex;
static SDL_AudioDeviceID openDspAudioDevice(const SDL_AudioSpec* requested,
                                           SDL_AudioSpec* obtained) {
    std::lock_guard<std::mutex> lock(dspDeviceLifecycleMutex);
    return SDL_OpenAudioDevice(nullptr, 0, requested, obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
}
static void closeDspAudioDevice(SDL_AudioDeviceID device) {
    std::lock_guard<std::mutex> lock(dspDeviceLifecycleMutex);
    SDL_CloseAudioDevice(device);
}

class KDspAudioSdl : public KDspAudio, public std::enable_shared_from_this<KDspAudioSdl> {
public:
	KDspAudioSdl() {
		this->want.format = AUDIO_U8;
		this->want.channels = 1;
		this->want.freq = 11025;
		this->want.samples = KDspAudioMath::getRequestedSdlPeriodFrames();
		this->got.channels = 1;
		this->got.freq = 11025;
		this->got.samples = this->want.samples;
	}

	virtual ~KDspAudioSdl() {
		this->writeWatchActive = false;
		if (this->cvtBuf) {
			SDL_free(this->cvtBuf);
		}
#ifdef __EMSCRIPTEN__
		if (this->stream) {
			SDL_FreeAudioStream(this->stream);
		}
#endif
		if (this->deviceId) {
			closeDspAudioDevice(this->deviceId);
			this->deviceId = 0;
		}
	}

	void openAudio(U32 format, U32 freq, U32 channels) override;
	void soundEnabled() override;
	bool isOpen() override { return this->open; }
	void closeAudio() override;
	U32 writeAudio(U8* data, U32 len) override;
	U32 getFragmentSize() override {return this->dspFragSize;}
	void setFragmentSize(U32 size) override;
	U32 getBufferSize() override {
#ifdef __EMSCRIPTEN__
		return this->getGuestQueuedAudioSizeWant();
#else
		return this->getQueuedAudioSizeWant();
#endif
	}
	U32 getBufferCapacity() override {
#ifdef __EMSCRIPTEN__
		return DSP_BUFFER_SIZE;
#else
		return this->getWriteCapacityWant();
#endif
	}
	U32 getPlayableBufferSize() override {
#ifdef __EMSCRIPTEN__
		return getBufferSize();
#else
		return getQueuedAudioSizeWant(false);
#endif
	}
	bool isWriteReady() override;
	void waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) override;

	U32 bytesPerSampleWant() {
		return SDL_AUDIO_BITSIZE(this->want.format) / 8;
	}

	U32 bytesPerSampleGot() {
		return SDL_AUDIO_BITSIZE(this->got.format) / 8;
	}

	U32 bytesPerSecondWant() {
		return this->want.freq * this->want.channels * bytesPerSampleWant();
	}

	U32 bytesPerSecondGot() {
		return this->got.freq * this->got.channels * bytesPerSampleGot();
	}

	U32 getWriteCapacityWant() {
		return KDspAudioMath::getWriteCapacity(bytesPerSecondWant(), getFragmentSize(), DSP_BUFFER_SIZE);
	}

	U32 getQueuedAudioSizeWant(bool includePendingInput = true) {
		if (!KSystem::soundEnabled) {
			this->drainNoSoundAudioBuffer();
			return (U32)this->audioBuffer.size();
		}
		if (!this->deviceId) {
			return 0;
		}
		U32 gotBytesPerSecond = bytesPerSecondGot();
		if (!gotBytesPerSecond) {
			return 0;
		}
		U64 queued = (U64)SDL_GetQueuedAudioSize(this->deviceId) * bytesPerSecondWant() / gotBytesPerSecond;
#ifndef __EMSCRIPTEN__
        if (includePendingInput && !this->sameFormat) queued += this->converter.bufferedInputBytes();
#endif
		return (U32)std::min<U64>(queued, 0xFFFFFFFFu);
	}

	void drainNoSoundAudioBuffer() {
		U32 now = KSystem::getMilliesSinceStart();
		if (!this->lastNoSoundDrainTime) {
			this->lastNoSoundDrainTime = now;
			return;
		}
		U32 elapsedTime = now - this->lastNoSoundDrainTime;
		this->lastNoSoundDrainTime = now;
		if (!elapsedTime || this->audioBuffer.empty()) {
			return;
		}
		U32 queued = (U32)this->audioBuffer.size();
		U32 remaining = KDspAudioMath::getQueuedAfterElapsed(queued, bytesPerSecondWant(), elapsedTime);
		if (remaining < queued) {
			this->audioBuffer.erase(this->audioBuffer.begin(), this->audioBuffer.begin() + (queued - remaining));
		}
	}

#ifdef __EMSCRIPTEN__
	U32 getGuestQueuedAudioSizeWant() {
		return this->getEstimatedRealQueuedWant();
	}
#endif

	U32 getSdlFormat(U32 format) {
		switch (format) {
		case AFMT_MU_LAW:
		case AFMT_A_LAW:
		case AFMT_IMA_ADPCM:
		case AFMT_U8:
			return AUDIO_U8;
		case AFMT_S16_LE:
			return AUDIO_S16LSB;
		case AFMT_S16_BE:
			return AUDIO_S16MSB;
		case AFMT_S8:
			return AUDIO_S8;
		case AFMT_U16_LE:
			return AUDIO_U16LSB;
		case AFMT_U16_BE:
			return AUDIO_U16MSB;
		case AFMT_MPEG:
			return AUDIO_U8;
        case AFMT_FLOAT:
                return AUDIO_F32LSB;
		default:
			kpanic_fmt("KNativeAudioSdl Unknow audio format %d", format);
			return 0;
		}
	}
	SDL_AudioSpec want = { 0 };
	SDL_AudioSpec got = { 0 };
	SDL_AudioCVT cvt = { 0 };
#ifndef __EMSCRIPTEN__
    SdlAudioConverter converter;
    std::vector<U8> convertedAudio;
#endif
#ifdef __EMSCRIPTEN__
	SDL_AudioStream* stream = nullptr;
#endif
	U32 openedFormat = 0;
	int cvtBufLen = 0;
	unsigned char* cvtBuf = nullptr;
#ifdef __EMSCRIPTEN__
	std::vector<U8> streamBuffer;
#endif
	U32 signalLastMs = 0;
	float guestPeak = 0, hostPeak = 0;
	U32 invalidSamples = 0;
	bool sameFormat = false;
	U32 dspFragSize = 4096;
    bool fragmentSizeRequested = false;
	bool open = false;
	std::deque<U8> audioBuffer; // only used when KSystem::soundEnabled is false
	U32 lastNoSoundDrainTime = 0;
	SDL_AudioDeviceID deviceId = 0;
	BOXEDWINE_CONDITION writeCond = std::make_shared<BoxedWineCondition>(B("KDspAudioSdl::writeCond"));
	bool writeWatchActive = false;
	bool writeWatchListed = false;
#ifdef __EMSCRIPTEN__
	U32 realQueuedWant = 0;
	U32 lastRealQueuedTime = 0;
	std::vector<U8> silenceBuffer;

	U32 getEstimatedRealQueuedWant() {
		U32 now = KSystem::getMilliesSinceStart();
		if (!this->lastRealQueuedTime) {
			this->lastRealQueuedTime = now;
			return this->realQueuedWant;
		}
		U32 elapsed = now - this->lastRealQueuedTime;
		if (elapsed) {
			U32 consumed = (U32)(((U64)bytesPerSecondWant() * elapsed) / 1000);
			this->realQueuedWant = consumed >= this->realQueuedWant ? 0 : this->realQueuedWant - consumed;
			this->lastRealQueuedTime = now;
		}
		return this->realQueuedWant;
	}

	void addRealQueuedWant(U32 bytes) {
		U32 queued = getEstimatedRealQueuedWant();
		this->realQueuedWant = std::min((U32)DSP_BUFFER_SIZE, queued + bytes);
	}

	U32 topUpSilence() {
		if (!this->deviceId) {
			return 0;
		}
		U32 gotBytesPerSecond = bytesPerSecondGot();
		U32 frameSize = bytesPerSampleGot() * this->got.channels;
		if (!gotBytesPerSecond || !frameSize) {
			return 0;
		}
		U32 queued = SDL_GetQueuedAudioSize(this->deviceId);
		U32 target = KDspAudioMath::getAlignedDurationBytes(gotBytesPerSecond, 48, frameSize);
		if (queued >= target) {
			return 0;
		}
		U32 bytes = (target - queued) & ~(frameSize - 1);
		if (!bytes) {
			return 0;
		}
		if (this->silenceBuffer.size() < bytes) {
			this->silenceBuffer.resize(bytes);
		}
		SDL_memset(this->silenceBuffer.data(), this->got.silence, bytes);
		SDL_QueueAudio(this->deviceId, this->silenceBuffer.data(), bytes);
		return bytes;
	}
#endif
};

// Voices whose closeAudio was called while audio was still queued to SDL.
// A timer polls these and finalizes the device close when the queue drains.
static std::list<std::shared_ptr<KDspAudioSdl>> pendingCloses;
static std::list<std::weak_ptr<KDspAudioSdl>> pendingWriteWatches;
static BOXEDWINE_MUTEX pendingClosesMutex;
static SDL_TimerID drainTimer = 0;

static Uint32 SDLCALL drainTimerCb(Uint32 interval, void* /*param*/) {
	std::vector<BOXEDWINE_CONDITION> readyConditions;

	{
		BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
		auto it = pendingCloses.begin();
		while (it != pendingCloses.end()) {
			std::shared_ptr<KDspAudioSdl> v = *it;
			if (!v->deviceId || SDL_GetQueuedAudioSize(v->deviceId) == 0) {
				if (v->deviceId) {
					closeDspAudioDevice(v->deviceId);
					v->deviceId = 0;
				}
				it = pendingCloses.erase(it);
			} else {
				++it;
			}
		}

		auto watch = pendingWriteWatches.begin();
		while (watch != pendingWriteWatches.end()) {
			std::shared_ptr<KDspAudioSdl> v = watch->lock();
			if (!v || !v->writeWatchActive || !v->writeCond->parentsCount()) {
				if (v) {
					v->writeWatchActive = false;
					v->writeWatchListed = false;
				}
				watch = pendingWriteWatches.erase(watch);
			} else {
				if (v->isWriteReady()) {
					readyConditions.push_back(v->writeCond);
				}
				++watch;
			}
		}

		if (pendingCloses.empty() && pendingWriteWatches.empty() && drainTimer) {
			interval = 0; // stop the timer
			drainTimer = 0;
		}
	}

	for (auto& cond : readyConditions) {
		BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(cond);
		BOXEDWINE_CONDITION_SIGNAL_ALL(cond);
	}
	return interval;
}

static void ensureDrainTimer() {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
	if (!drainTimer) {
		if (!SDL_WasInit(SDL_INIT_TIMER)) {
			SDL_InitSubSystem(SDL_INIT_TIMER);
		}
		drainTimer = SDL_AddTimer(50, drainTimerCb, nullptr);
	}
}

bool KDspAudioSdl::isWriteReady() {
#ifdef __EMSCRIPTEN__
	return this->getGuestQueuedAudioSizeWant() < DSP_BUFFER_SIZE;
#else
	U32 capacity = getWriteCapacityWant();
	if (!capacity) {
		return false;
	}
	if (!KSystem::soundEnabled) {
		this->drainNoSoundAudioBuffer();
		return this->audioBuffer.size() < capacity;
	}
	if (!this->open || !this->deviceId) {
		return true;
	}
	return this->getQueuedAudioSizeWant() < capacity;
#endif
}

void KDspAudioSdl::waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) {
	if (events & K_POLLOUT) {
		BOXEDWINE_CONDITION_ADD_PARENT(this->writeCond, parentCondition);
		{
			BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
			this->writeWatchActive = true;
			if (!this->writeWatchListed) {
				this->writeWatchListed = true;
				pendingWriteWatches.push_back(shared_from_this());
			}
		}
		ensureDrainTimer();
	} else {
		BOXEDWINE_CONDITION_REMOVE_PARENT(this->writeCond, parentCondition);
		if (!this->writeCond->parentsCount()) {
			BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
			this->writeWatchActive = false;
		}
	}
}

void KDspAudioSdl::soundEnabled() {
	if (this->open) {
		openAudio(this->openedFormat, this->want.freq, this->want.channels);
	}
}

void KDspAudioSdl::openAudio(U32 format, U32 freq, U32 channels) {
    this->open = false;
#ifndef __EMSCRIPTEN__
    this->converter.reset();
#endif
	this->want.callback = nullptr; // SDL_QueueAudio mode
	this->want.userdata = nullptr;
	this->want.format = getSdlFormat(format);
	this->want.freq = freq;
	this->want.channels = channels;
	this->openedFormat = format;

	if (!KSystem::soundEnabled) {
		this->open = true;
		this->lastNoSoundDrainTime = KSystem::getMilliesSinceStart();
		return;
	}

	{
		BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
		// Drop any pending-close entry; we're reusing this voice.
		std::shared_ptr<KDspAudioSdl> self = shared_from_this();
		for (auto it = pendingCloses.begin(); it != pendingCloses.end();) {
			if (*it == self) {
				it = pendingCloses.erase(it);
			} else {
				++it;
			}
		}
		// Close any prior device on this voice (reopening drops any still-queued audio).
		if (this->deviceId) {
			closeDspAudioDevice(this->deviceId);
			this->deviceId = 0;
		}
#ifdef __EMSCRIPTEN__
		if (this->stream) {
			SDL_FreeAudioStream(this->stream);
			this->stream = nullptr;
		}
#endif
	}

    SDL_AudioSpec requested = this->want;
#ifdef __MACH__
    if (requested.freq < 44100) {
        requested.freq = 44100;
    }
#endif
	ensureAudioSessionCategory();
	SDL_AudioDeviceID newId = openDspAudioDevice(&requested, &this->got);
	if (newId == 0) {
		klog_fmt("Failed to open audio: %s", SDL_GetError());
		klog_fmt("BOXEDWINE_AUDIO_DEVICE want=0x%x/%uHz/%uch got=none converted=0 "
				 "status=failed error=%s",
				 this->want.format, this->want.freq, this->want.channels,
				 SDL_GetError());
		return;
	}
	this->deviceId = newId;
#ifndef __EMSCRIPTEN__
    if (!this->fragmentSizeRequested) {
        this->dspFragSize = KDspAudioMath::getDefaultFragmentSize(
            bytesPerSecondWant(), this->got.freq, this->got.samples);
    }
#endif

	if (this->want.freq != this->got.freq || this->want.channels != this->got.channels || this->want.format != this->got.format) {
		this->sameFormat = false;
#ifdef __EMSCRIPTEN__
		this->stream = SDL_NewAudioStream(this->want.format, this->want.channels, this->want.freq, this->got.format, this->got.channels, this->got.freq);
		if (!this->stream) {
			SDL_BuildAudioCVT(&this->cvt, this->want.format, this->want.channels, this->want.freq, this->got.format, this->got.channels, this->got.freq);
		}
#else
        if (!this->converter.open(this->want, this->got)) {
            klog_fmt("BOXEDWINE_AUDIO_CONVERSION_ERROR: %s", SDL_GetError());
            closeDspAudioDevice(this->deviceId);
            this->deviceId = 0;
            return;
        }
#endif
	} else {
		this->sameFormat = true;
	}

	this->open = true;
	SDL_PauseAudioDevice(this->deviceId, 0);
	klog_fmt("openAudio: freq=%d(got %d) format=%x(got %x) channels=%d(got %d)", this->want.freq, this->got.freq, this->want.format, this->got.format, this->want.channels, this->got.channels);
	// One line that says whether a host device opened at all and whether the
	// guest's format is being converted on the way to it. "Failed to open
	// audio" above is the only thing this path used to say, and it said
	// nothing on success -- so a silent run could not be told from a run with
	// no device.
	klog_fmt("BOXEDWINE_AUDIO_DEVICE want=0x%x/%uHz/%uch got=0x%x/%uHz/%uch "
			 "converted=%d status=ok",
			 this->want.format, this->want.freq, this->want.channels,
			 this->got.format, this->got.freq, this->got.channels,
			 this->sameFormat ? 0 : 1);
}

void KDspAudioSdl::closeAudio() {
	if (!KSystem::soundEnabled) {
		this->open = false;
		this->audioBuffer.clear();
		this->lastNoSoundDrainTime = 0;
		return;
	}
	if (!this->open) {
		return;
	}

	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
	this->open = false;
	if (!this->deviceId) {
		return;
	}
#ifndef __EMSCRIPTEN__
    // Flush once at end of stream, never after individual writes: SDL documents
    // gaps when feeding more data after a flush.
    if (!this->sameFormat && this->converter.finish()) {
        int bytes = this->converter.get(this->convertedAudio);
        if (bytes > 0 && SDL_QueueAudio(this->deviceId, this->convertedAudio.data(), bytes) < 0)
            klog_fmt("BOXEDWINE_AUDIO_QUEUE_ERROR: %s", SDL_GetError());
    }
    this->converter.reset();
#endif
	if (SDL_GetQueuedAudioSize(this->deviceId) > 0) {
		// Leave the device open so the queue can drain; the poll timer will finalize.
		ensureDrainTimer();
		pendingCloses.push_back(shared_from_this());
	} else {
		closeDspAudioDevice(this->deviceId);
		this->deviceId = 0;
	}
}

void KDspAudioSdl::setFragmentSize(U32 size) {
    this->fragmentSizeRequested = true;
#ifdef __EMSCRIPTEN__
	size = std::clamp(size, (U32)256, (U32)4096);
#else
	size = std::clamp(size, (U32)512, (U32)16384);
#endif
	U32 blockSize = bytesPerSampleWant() * want.channels;
	if (blockSize) {
		size &= ~(blockSize - 1);
	}
	this->dspFragSize = size ? size : blockSize;
}

U32 KDspAudioSdl::writeAudio(U8* data, U32 len) {	
	if (!KSystem::soundEnabled) {
		U32 capacity = getWriteCapacityWant();
		this->drainNoSoundAudioBuffer();

		if (this->audioBuffer.size() >= capacity) {
			return -K_EWOULDBLOCK;
		}
		U32 blockSize = bytesPerSampleWant() * want.channels;
		len = KDspAudioMath::getWritableBytes(len, capacity, (U32)this->audioBuffer.size(), blockSize);
		audioBuffer.insert(this->audioBuffer.end(), data, data + len);
		return len;
	}

	if (!this->open || !this->deviceId) {
		return 0;
	}

#ifdef __EMSCRIPTEN__
	U32 queued = this->getGuestQueuedAudioSizeWant();
	if (queued >= DSP_BUFFER_SIZE) {
		return -K_EWOULDBLOCK;
	}
	U32 blockSize = bytesPerSampleWant() * want.channels;
	len = std::min(len, (DSP_BUFFER_SIZE - queued) & ~(blockSize - 1));
	if (!len) {
		return -K_EWOULDBLOCK;
	}
#else
	U32 queued = this->getQueuedAudioSizeWant();
	U32 capacity = getWriteCapacityWant();
	if (queued >= capacity) {
		return -K_EWOULDBLOCK;
	}
	U32 blockSize = bytesPerSampleWant() * want.channels;
	len = KDspAudioMath::getWritableBytes(len, capacity, queued, blockSize);
	if (!len) {
		return -K_EWOULDBLOCK;
	}
#endif

	// Measure a bounded subset of samples, never dump audio payloads.
	auto peak = [this](const U8* samples, U32 bytes, U32 format) {
		float result = 0;
		const U32 width = SDL_AUDIO_BITSIZE(format) / 8;
		if (!width) return result;
		const U32 count = bytes / width;
		for (U32 i = 0; i < count; i += std::max(1u, count / 128)) {
			float value = 0;
			if (format == AUDIO_F32LSB) memcpy(&value, samples + i * width, 4);
			else if (format == AUDIO_S16LSB) {
				S16 pcm; memcpy(&pcm, samples + i * width, 2); value = pcm / 32768.0f;
			}
			if (!std::isfinite(value)) this->invalidSamples++;
			else result = std::max(result, std::abs(value));
		}
		return result;
	};
	this->guestPeak = std::max(this->guestPeak, peak(data, len, want.format));

#ifdef __EMSCRIPTEN__
	if (!this->sameFormat && this->stream) {
		if (SDL_AudioStreamPut(this->stream, data, (int)len) < 0) {
			return 0;
		}
		int available = SDL_AudioStreamAvailable(this->stream);
		if (available > 0) {
			if ((int)this->streamBuffer.size() < available) {
				this->streamBuffer.resize(available);
			}
			int got = SDL_AudioStreamGet(this->stream, this->streamBuffer.data(), available);
			if (got > 0) {
				this->hostPeak = std::max(this->hostPeak, peak(this->streamBuffer.data(), got, this->got.format));
				SDL_QueueAudio(this->deviceId, this->streamBuffer.data(), got);
			}
		}
	} else if (!this->sameFormat) {
		int needed = (int)len * this->cvt.len_mult;
		if (this->cvtBufLen && this->cvtBufLen < needed) {
			SDL_free(this->cvtBuf);
			this->cvtBuf = nullptr;
			this->cvtBufLen = 0;
		}
		if (!this->cvtBufLen) {
			this->cvtBufLen = needed;
			this->cvtBuf = (Uint8*)SDL_malloc(this->cvtBufLen);
		}
		this->cvt.len = (int)len;
		this->cvt.buf = this->cvtBuf;
		memcpy(this->cvt.buf, data, len);
		if (SDL_ConvertAudio(&this->cvt) < 0) {
			klog_fmt("BOXEDWINE_AUDIO_CONVERSION_ERROR: %s", SDL_GetError());
			return -K_EIO;
		}
		this->hostPeak = std::max(this->hostPeak, peak(this->cvt.buf, this->cvt.len_cvt, got.format));
		SDL_QueueAudio(this->deviceId, this->cvt.buf, this->cvt.len_cvt);
#else
    if (!this->sameFormat) {
        if (!this->converter.put(data, len)) return -K_EIO;
        int bytes = this->converter.get(this->convertedAudio);
        if (bytes < 0) return -K_EIO;
        if (bytes > 0) {
            this->hostPeak = std::max(this->hostPeak, peak(this->convertedAudio.data(), bytes, got.format));
            if (SDL_QueueAudio(this->deviceId, this->convertedAudio.data(), bytes) < 0) {
                klog_fmt("BOXEDWINE_AUDIO_QUEUE_ERROR: %s", SDL_GetError());
                return -K_EIO;
            }
        }
#endif
	} else {
		this->hostPeak = std::max(this->hostPeak, this->guestPeak);
		if (SDL_QueueAudio(this->deviceId, data, len) < 0) return -K_EIO;
	}
	const U32 now = KSystem::getMilliesSinceStart();
	if (now - this->signalLastMs >= 5000) {
		klog_fmt("BOXEDWINE_AUDIO_SIGNAL id=%u guest_fmt=0x%x host_fmt=0x%x "
			"guest_peak=%.6f host_peak=%.6f invalid=%u queued=%u",
			this->id, want.format, got.format, this->guestPeak, this->hostPeak,
			this->invalidSamples,
			SDL_GetQueuedAudioSize(this->deviceId));
		this->signalLastMs = now;
		this->guestPeak = this->hostPeak = 0;
		this->invalidSamples = 0;
	}
#ifdef __EMSCRIPTEN__
	addRealQueuedWant(len);
	topUpSilence();
#endif
	return len;
}

static U32 nextId = 0;
BOXEDWINE_MUTEX KDspAudio::mutex;
BHashTable<U32, KDspAudioWeakPtr> KDspAudio::openAudios;

KDspAudioPtr KDspAudio::createDspAudio() {
	KDspAudioPtr result = std::make_shared<KDspAudioSdl>();
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	result->id = nextId++;
	KDspAudioWeakPtr weak = result;
	openAudios.set(result->id, weak);
	return result;
}

KDspAudio::~KDspAudio() {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	openAudios.remove(this->id);
}

void KDspAudio::iterateOpenAudio(std::function<void(KDspAudioPtr&)> callback) {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	for (auto& it : openAudios) {
		KDspAudioPtr audio = it.value.lock();
		if (audio) {
			callback(audio);
		}
	}
}

void KDspAudio::shutdown() {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pendingClosesMutex);
	if (drainTimer) {
		SDL_RemoveTimer(drainTimer);
		drainTimer = 0;
	}
	if (KSystem::soundEnabled) {
		for (auto& v : pendingCloses) {
			if (v->deviceId) {
				closeDspAudioDevice(v->deviceId);
				v->deviceId = 0;
			}
		}
	}
	pendingCloses.clear();
	for (auto& weakAudio : pendingWriteWatches) {
		std::shared_ptr<KDspAudioSdl> audio = weakAudio.lock();
		if (audio) {
			audio->writeWatchActive = false;
			audio->writeWatchListed = false;
		}
	}
	pendingWriteWatches.clear();
}
