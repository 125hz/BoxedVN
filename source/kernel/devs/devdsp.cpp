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

#include "kscheduler.h"
#include "../../io/fsvirtualopennode.h"
#include "oss.h"
#include "ossioctl.h"
#ifdef __EMSCRIPTEN__
#include <algorithm>
#endif
#include <math.h>
#include <string.h>
#include "kdspaudio.h"
#include "kdspaudio_math.h"

#ifdef __EMSCRIPTEN__
static U32 dspMaxOutputFreq = 11025;
static const U32 DSP_DEFAULT_FRAGMENT_SIZE = 1024;
static const U32 DSP_DEFAULT_FRAGMENT_COUNT = 8;
#endif

// The output formats and the rate ceiling this device advertises, in one place
// so SNDCTL_DSP_GETFMTS and the oss_audioinfo written by SNDCTL_ENGINEINFO can
// never disagree -- an OSSv4 client that is told a format in one and refused
// it in the other has no way to recover.
static U32 devDspOutputFormats() {
#ifdef __EMSCRIPTEN__
    return AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE;
#else
    return AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE | AFMT_FLOAT;
#endif
}

static U32 devDspMaxRate() {
#ifdef __EMSCRIPTEN__
    return dspMaxOutputFreq;
#else
    return 48000;
#endif
}

class DevDsp : public FsVirtualOpenNode {
public:
    DevDsp(const std::shared_ptr<FsNode>& node, U32 flags) : FsVirtualOpenNode(node, flags) {                
        this->audio = KDspAudio::createDspAudio();
        this->freq = 11025;
        this->channels = 1;
        this->format = AFMT_U8;
#ifdef __EMSCRIPTEN__
        this->audio->setFragmentSize(DSP_DEFAULT_FRAGMENT_SIZE);
#endif
    } 
    virtual ~DevDsp() {this->audio->closeAudio();}

    // From FsOpenNode
    bool setLength(S64 length) override;
    U32 ioctl(KThread* thread, U32 request) override;
#ifdef BOXEDWINE_GUEST_X64
    // The 64-bit lane's entry point. source/kernel/syscall64.cpp's ioctl case
    // hands every device fd to this first and falls through to its own
    // FIONBIO/FIONREAD/-ENOTTY handling when it answers -ENOTTY, so anything
    // this does not model has to keep answering -ENOTTY and not -ENODEV.
    U32 ioctl64(U32 request, U64 argAddress, KMemory64* memory) override;
#endif
    U32 readNative(U8* buffer, U32 len) override;
    U32 writeNative(U8* buffer, U32 len) override;
    void waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) override;
    bool isWriteReady() override;

    std::shared_ptr<KDspAudio> audio;
    U32 freq;
    U32 channels;
    U32 format;
#ifdef BOXEDWINE_GUEST_X64
    // Everything below is the 64-bit lane's diagnostics and its lazily opened
    // host voice. None of it is read on the IA-32 path.
    U32 bytesPerFrame() const {
        U32 bytesPerSample = 1;
        switch (this->format) {
        case AFMT_S16_LE: case AFMT_S16_BE:
        case AFMT_U16_LE: case AFMT_U16_BE: bytesPerSample = 2; break;
        case AFMT_FLOAT: bytesPerSample = 4; break;
        default: bytesPerSample = 1; break;
        }
        return bytesPerSample * (this->channels ? this->channels : 1);
    }
    void ensureAudioOpen64();
    void logDspOpen64();
    void logFirstCall64(U32 request, const char* op, U64 arg, U32 value, S32 result);
    void accountWrite64(U32 requested, S32 written, U32 queuedBefore);

    bool lane64 = false;
    bool openLogged64 = false;
    U32 loggedRequests64[24] = {};
    U32 loggedRequestCount64 = 0;
    U32 statsLastMs64 = 0;
    U64 statsBytes64 = 0;
    U64 statsTotalBytes64 = 0;
    U32 statsUnderruns64 = 0;
    bool everWrote64 = false;
#endif
#ifdef __EMSCRIPTEN__
    U32 fragmentCount = DSP_DEFAULT_FRAGMENT_COUNT;
    U32 bytesWritten = 0;
    U32 lastOutputBlocks = 0;

private:
    U32 getEffectiveBufferCapacity();
    U32 getUsedBufferSize();
    U32 getAvailableBufferSize();
#endif
};


void dspShutdown() {
    KDspAudio::shutdown();
}

#ifdef __EMSCRIPTEN__
void dspSetMaxOutputFreq(U32 freq) {
    if (freq == 11025 || freq == 22050) {
        dspMaxOutputFreq = freq;
    } else {
        kwarn_fmt("Unsupported Emscripten audio frequency %d, using %d", freq, dspMaxOutputFreq);
    }
}
#endif

bool DevDsp::setLength(S64 len) {
    return false;
}

U32 DevDsp::readNative(U8* buffer, U32 len){
    return 0;
}

U32 DevDsp::writeNative(U8* buffer, U32 len) {
    if (!this->audio->isOpen()) {
        this->audio->openAudio(this->format, this->freq, this->channels);
    }
#ifdef BOXEDWINE_GUEST_X64
    if (this->lane64) {
        this->logDspOpen64();
    }
#endif
#ifdef __EMSCRIPTEN__
    U32 result = this->audio->writeAudio(buffer, len);
    if ((S32)result > 0) {
        this->bytesWritten += result;
    }
    return result;
#else
#ifdef BOXEDWINE_GUEST_X64
    // Sampled before the write, not after: an underrun is the host queue
    // having reached zero while the guest was still meant to be feeding it,
    // and after a successful write the queue is never zero.
    const U32 queuedBefore = this->lane64 ? this->audio->getBufferSize() : 0;
#endif
    U32 result = this->audio->writeAudio(buffer, len);
#ifdef BOXEDWINE_GUEST_X64
    if (this->lane64) {
        this->accountWrite64(len, (S32)result, queuedBefore);
    }
#endif
    return result;
#endif
}

#ifdef __EMSCRIPTEN__
U32 DevDsp::getEffectiveBufferCapacity() {
    U32 capacity = this->audio->getBufferCapacity();
    U32 fragmentCapacity = this->audio->getFragmentSize() * this->fragmentCount;
    return std::min(capacity, fragmentCapacity ? fragmentCapacity : capacity);
}

U32 DevDsp::getUsedBufferSize() {
    return std::min(this->audio->getBufferSize(), this->getEffectiveBufferCapacity());
}

U32 DevDsp::getAvailableBufferSize() {
    return this->getEffectiveBufferCapacity() - this->getUsedBufferSize();
}
#endif

bool DevDsp::isWriteReady() {
#ifdef __EMSCRIPTEN__
    return this->getAvailableBufferSize() >= this->audio->getFragmentSize();
#else
    return this->audio->isWriteReady();
#endif
}

U32 DevDsp::ioctl(KThread* thread, U32 request) {
    U32 len = (request >> 16) & 0x3FFF;
    CPU* cpu = thread->cpu;
    KMemory* memory = thread->memory;

    //BOOL read = request & 0x40000000;
    bool write = (request & 0x80000000)!=0;

    switch (request & 0xFFFF) {
    case 0x5000: // SNDCTL_DSP_RESET
        this->audio->closeAudio();
        this->freq = 8000;
        this->channels = 1;
        this->format = AFMT_U8;
#ifdef __EMSCRIPTEN__
        this->audio->setFragmentSize(DSP_DEFAULT_FRAGMENT_SIZE);
        this->fragmentCount = DSP_DEFAULT_FRAGMENT_COUNT;
        this->bytesWritten = 0;
        this->lastOutputBlocks = 0;
#endif
        return 0;
    case 0x5002: { // SNDCTL_DSP_SPEED 
        if (len!=4) {
            // A guest handing an OSS device a length the emulation does not
            // model must not take the emulator down with it: on this device
            // the caller is a sound driver, and the right answer to something
            // it should not have asked is an error it can fall back from.
            klog_fmt("DevDsp: SNDCTL_DSP_SPEED refused, len=%d (expected 4)", len);
            return -K_EINVAL;
        }
#ifdef __EMSCRIPTEN__
        U32 oldFreq = this->freq;
        this->freq = std::min(memory->readd(IOCTL_ARG1), dspMaxOutputFreq);
        if (oldFreq != this->freq) {
#else
		this->freq = memory->readd(IOCTL_ARG1);
        if (freq != this->freq) {
#endif
            this->audio->closeAudio();
        }
		if (write)
            memory->writed(IOCTL_ARG1, this->freq);
        return 0;
    }
    case 0x5003: { // SNDCTL_DSP_STEREO
        if (len!=4) {
            klog_fmt("DevDsp: SNDCTL_DSP_STEREO refused, len=%d (expected 4)", len);
            return -K_EINVAL;
        }
        U32 fmt = memory->readd(IOCTL_ARG1);
        if (fmt != (U32)(this->channels - 1)) {
            this->audio->closeAudio();
        }
        if (fmt == 0) {
            this->channels = 1;
        } else if (fmt == 1) {
            this->channels = 2;
        } else {
            klog_fmt("DevDsp: SNDCTL_DSP_STEREO refused, value=%d", fmt);
            return -K_EINVAL;
        }
        if (write)
            memory->writed(IOCTL_ARG1, this->channels - 1);
        return 0;
    }
    case 0x5005: { // SNDCTL_DSP_SETFMT 
        if (len!=4) {
            klog_fmt("DevDsp: SNDCTL_DSP_SETFMT refused, len=%d (expected 4)", len);
            return -K_EINVAL;
        }
        U32 fmt = memory->readd(IOCTL_ARG1);
		if (fmt != AFMT_QUERY && fmt != this->format) {
            this->audio->closeAudio();
        }
        switch (fmt) {
        case AFMT_QUERY:
            break;
        case AFMT_MU_LAW:
        case AFMT_A_LAW:
        case AFMT_IMA_ADPCM:
        case AFMT_U8:
			this->format = AFMT_U8;
            break;
        case AFMT_S16_LE:
			this->format = AFMT_S16_LE;
            break;
        case AFMT_S16_BE:
			this->format = AFMT_S16_BE;
            break;
        case AFMT_S8:
			this->format = AFMT_S8;
            break;
        case AFMT_U16_LE:
			this->format = AFMT_U16_LE;
            break;
        case AFMT_U16_BE:
			this->format = AFMT_U16_BE;
            break;
        case AFMT_MPEG:
			this->format = AFMT_U8;
            break;
        case AFMT_FLOAT:
            this->format = AFMT_FLOAT;
            break;
        }
        if (write)
            memory->writed(IOCTL_ARG1, this->format);
		else if (this->format != fmt) {
            klog_fmt("DevDsp: SNDCTL_DSP_SETFMT refused, asked 0x%x, using 0x%x, "
                     "and the request has no result slot", fmt, this->format);
            return -K_EINVAL;
        }
        return 0;
        }
    case 0x5006: {// SOUND_PCM_WRITE_CHANNELS
        U32 channels = memory->readd(IOCTL_ARG1);
		if (channels != this->channels) {
            this->audio->closeAudio();
        }
        if (channels==1) {
            this->channels = 1;
        } else if (channels == 2) {
            this->channels = 2;
        } else {
            this->channels = 2;
        }
        if (write)
            memory->writed(IOCTL_ARG1, this->channels);
        return 0;
        }
    case 0x500A: // SNDCTL_DSP_SETFRAGMENT
#ifdef __EMSCRIPTEN__
    {
        U32 value = memory->readd(IOCTL_ARG1);
        U32 shift = value & 0xFFFF;
        U32 count = value >> 16;
        if (shift > 15) {
            shift = 15;
        }
        this->audio->setFragmentSize(1 << shift);
        this->fragmentCount = std::clamp(count, (U32)2, (U32)64);
        return 0;
    }
#else
		// this->data->dspFragSize = 1 << (readd(IOCTL_ARG1) & 0xFFFF);
        klog("DevDsp::ioctl was not expecting SNDCTL_DSP_SETFRAGMENT");
        return 0;
#endif
    case 0x500B: // SNDCTL_DSP_GETFMTS
#ifdef __EMSCRIPTEN__
        memory->writed(IOCTL_ARG1, AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE);
#else
        memory->writed(IOCTL_ARG1, AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE | AFMT_FLOAT);
#endif
        return 0;

		//typedef struct audio_buf_info {
		//	int fragments;     /* # of available fragments (partially usend ones not counted) */
		//	int fragstotal;    /* Total # of fragments allocated */
		//	int fragsize;      /* Size of a fragment in bytes */
		//
		//	int bytes;         /* Available space in bytes (includes partially used fragments) */
		//	/* Note! 'bytes' could be more than fragments*fragsize */
		//} audio_buf_info;

    case 0x500C: // SNDCTL_DSP_GETOSPACE
    {
#ifdef __EMSCRIPTEN__
        U32 capacity = this->getEffectiveBufferCapacity();
        U32 used = this->getUsedBufferSize();
        U32 available = KDspAudioMath::getOutputSpaceAvailable(capacity, used, true);
        memory->writed(IOCTL_ARG1, available / this->audio->getFragmentSize()); // fragments
        memory->writed(IOCTL_ARG1 + 4, capacity / this->audio->getFragmentSize());
        memory->writed(IOCTL_ARG1 + 8, this->audio->getFragmentSize());
        memory->writed(IOCTL_ARG1 + 12, available);
#else
        U32 capacity = this->audio->getBufferCapacity();
        U32 available = KDspAudioMath::getOutputSpaceAvailable(capacity, 0, false);
        memory->writed(IOCTL_ARG1, available / this->audio->getFragmentSize()); // fragments
        memory->writed(IOCTL_ARG1 + 4, capacity / this->audio->getFragmentSize());
        memory->writed(IOCTL_ARG1 + 8, this->audio->getFragmentSize());
        memory->writed(IOCTL_ARG1 + 12, available);
#endif
        return 0;
    }
    case 0x500F: // SNDCTL_DSP_GETCAPS
        memory->writed(IOCTL_ARG1, DSP_CAP_TRIGGER);
        return 0;
    case 0x5010: // SNDCTL_DSP_SETTRIGGER
        /*
        if (readd(IOCTL_ARG1) & PCM_ENABLE_OUTPUT) {
            if (sdlSoundEnabled) {
                SDL_PauseAudio(0);
            }
			this->data->pauseAtLen = 0xFFFFFFFF;
        } else {            
			this->data->pauseAtLen = (U32)audioBuffer.size();
			if (this->data->pauseAtLen == 0) {
                if (sdlSoundEnabled) {
                    SDL_PauseAudio(0);
                }
            }
        }
        */
        klog("DevDsp::ioctl was not expecting SNDCTL_DSP_SETTRIGGER");
        return 0;
    case 0x5012: // SNDCTL_DSP_GETOPTR
#ifdef __EMSCRIPTEN__
    {
        U32 fragmentSize = this->audio->getFragmentSize();
        U32 currentBlocks = fragmentSize ? this->bytesWritten / fragmentSize : 0;
        U32 blocks = currentBlocks - this->lastOutputBlocks;
        this->lastOutputBlocks = currentBlocks;
        U32 capacity = this->getEffectiveBufferCapacity();
        memory->writed(IOCTL_ARG1, this->bytesWritten); // Total # of bytes written
        memory->writed(IOCTL_ARG1 + 4, blocks); // # of fragment transitions since last time
        memory->writed(IOCTL_ARG1 + 8, capacity ? this->bytesWritten % capacity : 0); // Current DMA pointer value
        return 0;
    }
#else
        /*
        writed(IOCTL_ARG1, 0); // Total # of bytes processed
        writed(IOCTL_ARG1 + 4, 0); // # of fragment transitions since last time
        if (data->pauseEnabled()) {
			writed(IOCTL_ARG1 + 8, this->data->pauseAtLen); // Current DMA pointer value
			if (this->data->pauseAtLen == 0) {
                if (sdlSoundEnabled) {
                    SDL_PauseAudio(0);
                }
            }
        } else {
			writed(IOCTL_ARG1 + 8, (U32)audioBuffer.size()); // Current DMA pointer value
        }
        */
        klog("DevDsp::ioctl was not expecting SNDCTL_DSP_GETOPTR");
        return 0;
#endif
    case 0x5016: // SNDCTL_DSP_SETDUPLEX
        return -K_EINVAL;
    case 0x5017: // SNDCTL_DSP_GETODELAY
#ifdef __EMSCRIPTEN__
        memory->writed(IOCTL_ARG1, this->getUsedBufferSize());
        return 0;
#else
        /*
        if (write) {
			writed(IOCTL_ARG1, (U32)audioBuffer.size());
            return 0;
        }
        */
        klog("DevDsp::ioctl was not expecting SNDCTL_DSP_GETODELAY");
        return 0;
#endif
    case 0x580C: // SNDCTL_ENGINEINFO
        if (write) {
            OssIoctlArg32 arg(memory, IOCTL_ARG1);
            ossWriteAudioInfo(arg, 0, "BoxedWine audio", PCM_CAP_OUTPUT,
                              devDspOutputFormats(), 11025, devDspMaxRate(),
                              1, 2, "/dev/dsp");
            return 0;
        }
    }
    return -K_ENODEV;
}

#ifdef BOXEDWINE_GUEST_X64
// The host voice is opened lazily on the first write in the IA-32 lane, which
// is late enough for an OSS client that writes as soon as it has set a format.
// wineoss does not: dlls/wineoss.drv/oss.c asks SNDCTL_DSP_GETOSPACE straight
// after SETFMT/SPEED/CHANNELS and takes stream->oss_bufsize_bytes from
// fragstotal * fragsize, so the numbers have to describe the format that was
// just negotiated and not KDspAudio's 11025/mono/U8 defaults.
void DevDsp::ensureAudioOpen64() {
    if (!this->audio->isOpen()) {
        this->audio->openAudio(this->format, this->freq, this->channels);
    }
    this->logDspOpen64();
}

void DevDsp::logDspOpen64() {
    if (this->openLogged64) {
        return;
    }
    this->openLogged64 = true;
    KThread* thread = KThread::currentThread();
    klog_fmt("BOXEDWINE_X64_DSP open pid=%d fmt=0x%x rate=%u channels=%u "
             "frag=%u",
             thread && thread->process ? (int)thread->process->id : -1,
             this->format, this->freq, this->channels,
             this->audio->getFragmentSize());
}

// One line per distinct request code, the way BOXEDWINE_X64_X11_BRIDGE budgets
// its first-call reports: a run that reaches sound shows SETFMT, SPEED and
// CHANNELS once each and then goes quiet, and a run that does not shows which
// request was refused and with what.
void DevDsp::logFirstCall64(U32 request, const char* op, U64 arg, U32 value,
                            S32 result) {
    const U32 code = request & 0xFFFF;
    for (U32 i = 0; i < this->loggedRequestCount64; i++) {
        if (this->loggedRequests64[i] == code) {
            return;
        }
    }
    if (this->loggedRequestCount64 >=
        (U32)(sizeof(this->loggedRequests64) / sizeof(this->loggedRequests64[0]))) {
        return;
    }
    this->loggedRequests64[this->loggedRequestCount64++] = code;
    klog_fmt("BOXEDWINE_X64_DSP op=%s request=0x%x arg=0x%llx value=%u result=%d",
             op, request, (unsigned long long)arg, value, result);
}

void DevDsp::accountWrite64(U32 requested, S32 written, U32 queuedBefore) {
    if (this->everWrote64 && queuedBefore == 0) {
        // The host queue ran dry between two writes: the guest did not keep up
        // and the device played silence. Counted rather than logged per event,
        // because an underrun storm is exactly when a log line costs the most.
        this->statsUnderruns64++;
    }
    if (written > 0) {
        this->statsBytes64 += (U64)written;
        this->statsTotalBytes64 += (U64)written;
        this->everWrote64 = true;
    }
    const U32 now = KSystem::getMilliesSinceStart();
    if (!this->statsLastMs64) {
        this->statsLastMs64 = now;
        return;
    }
    const U32 elapsed = now - this->statsLastMs64;
    if (elapsed < 5000) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_DSP_STATS bytes/s=%llu underruns=%u rate=%u "
             "channels=%u fmt=0x%x short=%d",
             (unsigned long long)(this->statsBytes64 * 1000ULL / elapsed),
             this->statsUnderruns64, this->freq, this->channels, this->format,
             written >= 0 && (U32)written < requested ? 1 : 0);
    this->statsLastMs64 = now;
    this->statsBytes64 = 0;
    this->statsUnderruns64 = 0;
}

U32 DevDsp::ioctl64(U32 request, U64 argAddress, KMemory64* memory) {
    if (!memory) {
        return (U32)-K_ENOTTY;
    }
    const U32 code = request & 0xFFFF;
    // Only the OSS group. Anything else has to reach syscall64.cpp's own
    // FIONBIO/FIONREAD handling untouched, and that is keyed on -ENOTTY.
    if ((code & 0xFF00) != 0x5000 && (code & 0xFF00) != 0x5800) {
        return (U32)-K_ENOTTY;
    }
    this->lane64 = true;
    OssIoctlArg64 arg(memory, argAddress);
    const bool write = (request & 0x80000000) != 0;
    const char* op = "unknown";
    U32 value = 0;
    S32 result = 0;

    switch (code) {
    case 0x5000: // SNDCTL_DSP_RESET
        op = "SNDCTL_DSP_RESET";
        this->audio->closeAudio();
        break;
    case 0x5001: // SNDCTL_DSP_SYNC - wait for playback to drain
        // Nothing to wait for: sys_write64 already copied the guest's bytes
        // into a host-owned queue before writeNative saw them, so a SYNC has
        // no guest-visible buffer left to flush. Closing here would drop what
        // is still queued, which is the opposite of what SYNC means.
        op = "SNDCTL_DSP_SYNC";
        break;
    case 0x5002: { // SNDCTL_DSP_SPEED
        op = "SNDCTL_DSP_SPEED";
        value = arg.readd(0);
        // Not clamped to the advertised min_rate/max_rate: KDspAudioSdl
        // converts whatever it is given to whatever the host device accepts,
        // so the rate really is honoured and OSS's contract ("SPEED returns
        // the rate you got") is met by echoing it back. Bounded only against
        // a value that would overflow the bytes-per-second arithmetic the
        // buffer geometry is computed from.
        if (value < 1000 || value > 192000) {
            klog_fmt("DevDsp: SNDCTL_DSP_SPEED refused, rate=%u out of range", value);
            result = -K_EINVAL;
            break;
        }
        if (value != this->freq) {
            this->freq = value;
            this->audio->closeAudio();
        }
        if (write) {
            arg.writed(0, this->freq);
        }
        value = this->freq;
        break;
    }
    case 0x5003: { // SNDCTL_DSP_STEREO
        op = "SNDCTL_DSP_STEREO";
        value = arg.readd(0);
        if (value > 1) {
            result = -K_EINVAL;
            break;
        }
        const U32 wanted = value + 1;
        if (wanted != this->channels) {
            this->channels = wanted;
            this->audio->closeAudio();
        }
        if (write) {
            arg.writed(0, this->channels - 1);
        }
        break;
    }
    case 0x5004: // SNDCTL_DSP_GETBLKSIZE
        op = "SNDCTL_DSP_GETBLKSIZE";
        this->ensureAudioOpen64();
        value = this->audio->getFragmentSize();
        arg.writed(0, value);
        break;
    case 0x5005: { // SNDCTL_DSP_SETFMT
        op = "SNDCTL_DSP_SETFMT";
        value = arg.readd(0);
        U32 chosen = this->format;
        switch (value) {
        case AFMT_QUERY: break;
        case AFMT_MU_LAW: case AFMT_A_LAW: case AFMT_IMA_ADPCM:
        case AFMT_U8:     chosen = AFMT_U8; break;
        case AFMT_S16_LE: chosen = AFMT_S16_LE; break;
        case AFMT_S16_BE: chosen = AFMT_S16_BE; break;
        case AFMT_S8:     chosen = AFMT_S8; break;
        case AFMT_U16_LE: chosen = AFMT_U16_LE; break;
        case AFMT_U16_BE: chosen = AFMT_U16_BE; break;
        case AFMT_MPEG:   chosen = AFMT_U8; break;
        case AFMT_FLOAT:  chosen = AFMT_FLOAT; break;
        default:
            // OSS answers an unsupported format by returning the one it will
            // use, not by failing; wineoss then retries with what it was
            // given. Refusing outright would lose it a working stream.
            chosen = AFMT_S16_LE;
            break;
        }
        if (value != AFMT_QUERY && chosen != this->format) {
            this->format = chosen;
            this->audio->closeAudio();
        }
        if (write) {
            arg.writed(0, this->format);
        } else if (this->format != value && value != AFMT_QUERY) {
            result = -K_EINVAL;
        }
        value = this->format;
        break;
    }
    case 0x5006: { // SNDCTL_DSP_CHANNELS (SOUND_PCM_WRITE_CHANNELS)
        op = "SNDCTL_DSP_CHANNELS";
        value = arg.readd(0);
        const U32 wanted = value >= 2 ? 2 : 1;
        if (wanted != this->channels) {
            this->channels = wanted;
            this->audio->closeAudio();
        }
        if (write) {
            arg.writed(0, this->channels);
        }
        value = this->channels;
        break;
    }
    case 0x5008: // SNDCTL_DSP_POST
        op = "SNDCTL_DSP_POST";
        break;
    case 0x500A: { // SNDCTL_DSP_SETFRAGMENT
        op = "SNDCTL_DSP_SETFRAGMENT";
        value = arg.readd(0);
        U32 shift = value & 0xFFFF;
        if (shift > 16) {
            shift = 16;
        }
        this->audio->setFragmentSize(1u << shift);
        if (write) {
            arg.writed(0, value);
        }
        break;
    }
    case 0x500B: // SNDCTL_DSP_GETFMTS
        op = "SNDCTL_DSP_GETFMTS";
        value = devDspOutputFormats();
        arg.writed(0, value);
        break;
    case 0x500C: { // SNDCTL_DSP_GETOSPACE
        op = "SNDCTL_DSP_GETOSPACE";
        this->ensureAudioOpen64();
        U32 fragSize = this->audio->getFragmentSize();
        if (!fragSize) {
            fragSize = this->bytesPerFrame();
        }
        if (!fragSize) {
            result = -K_EINVAL;
            break;
        }
        const U32 capacity = this->audio->getBufferCapacity();
        const U32 used = this->audio->getBufferSize();
        const U32 available =
            KDspAudioMath::getOutputSpaceAvailable(capacity, used, true);
        // Self-consistency matters more than precision here. wineoss takes
        // oss_bufsize_bytes as fragstotal * fragsize and then computes
        // (oss_bufsize_bytes - bi.bytes) as an unsigned frame count, so a
        // "bytes" larger than fragstotal * fragsize underflows into a
        // multi-gigabyte write budget. Reporting whole fragments keeps
        // bytes <= fragstotal * fragsize by construction.
        const U32 fragsTotal = capacity / fragSize;
        const U32 frags = available / fragSize;
        arg.writed(0, frags);                 // int fragments
        arg.writed(4, fragsTotal);            // int fragstotal
        arg.writed(8, fragSize);              // int fragsize
        arg.writed(12, frags * fragSize);     // int bytes
        value = frags * fragSize;
        break;
    }
    case 0x500D: { // SNDCTL_DSP_GETISPACE
        // No capture device is emulated. An empty input buffer is the honest
        // answer and is what wineoss's capture position path expects; the
        // alternative (-ENODEV) makes it log an error every period.
        op = "SNDCTL_DSP_GETISPACE";
        U32 fragSize = this->audio->getFragmentSize();
        if (!fragSize) {
            fragSize = 4096;
        }
        arg.writed(0, 0);
        arg.writed(4, 0);
        arg.writed(8, fragSize);
        arg.writed(12, 0);
        break;
    }
    case 0x500E: // SNDCTL_DSP_NONBLOCK
        // The descriptor's blocking mode is the file descriptor's business,
        // and syscall64.cpp already routes FIONBIO there. Nothing to do.
        op = "SNDCTL_DSP_NONBLOCK";
        break;
    case 0x500F: // SNDCTL_DSP_GETCAPS
        op = "SNDCTL_DSP_GETCAPS";
        value = DSP_CAP_TRIGGER;
        arg.writed(0, value);
        break;
    case 0x5010: // SNDCTL_DSP_SETTRIGGER
        op = "SNDCTL_DSP_SETTRIGGER";
        value = arg.readd(0);
        break;
    case 0x5011: // SNDCTL_DSP_GETTRIGGER
        op = "SNDCTL_DSP_GETTRIGGER";
        value = PCM_ENABLE_OUTPUT;
        arg.writed(0, value);
        break;
    case 0x5012: { // SNDCTL_DSP_GETOPTR
        op = "SNDCTL_DSP_GETOPTR";
        U32 fragSize = this->audio->getFragmentSize();
        if (!fragSize) {
            fragSize = 4096;
        }
        const U32 played = (U32)this->statsTotalBytes64;
        arg.writed(0, played);              // int bytes
        arg.writed(4, played / fragSize);   // int blocks
        arg.writed(8, this->audio->getBufferSize()); // int ptr
        value = played;
        break;
    }
    case 0x5016: // SNDCTL_DSP_SETDUPLEX
        op = "SNDCTL_DSP_SETDUPLEX";
        result = -K_EINVAL;
        break;
    case 0x5017: // SNDCTL_DSP_GETODELAY
        op = "SNDCTL_DSP_GETODELAY";
        value = this->audio->getBufferSize();
        arg.writed(0, value);
        break;
    case 0x580C: // SNDCTL_ENGINEINFO
        op = "SNDCTL_ENGINEINFO";
        if (!write) {
            result = -K_EINVAL;
            break;
        }
        ossWriteAudioInfo(arg, 0, "BoxedWine audio", PCM_CAP_OUTPUT,
                          devDspOutputFormats(), 11025, devDspMaxRate(),
                          1, 2, "/dev/dsp");
        break;
    default:
        result = -K_ENOTTY;
        op = "unhandled";
        break;
    }
    this->logFirstCall64(request, op, argAddress, value, result);
    return (U32)result;
}
#endif

void DevDsp::waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) {
    this->audio->waitForEvents(parentCondition, events);
}

FsOpenNode* openDevDsp(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
    return new DevDsp(node, flags);
}
