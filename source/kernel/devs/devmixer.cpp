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

#include "oss.h"
#include "ossioctl.h"
#include "../../io/fsvirtualopennode.h"

class DevMixer : public FsVirtualOpenNode {
public:
    DevMixer(const std::shared_ptr<FsNode>& node, U32 flags) : FsVirtualOpenNode(node, flags) {}

    // From FsOpenNode
    U32 ioctl(KThread* thread, U32 request) override;
#ifdef BOXEDWINE_GUEST_X64
    U32 ioctl64(U32 request, U64 argAddress, KMemory64* memory) override;
#endif
    U32 readNative(U8* buffer, U32 len) override {return 0;}
    U32 writeNative(U8* buffer, U32 len) override {return 0;}

#ifdef BOXEDWINE_GUEST_X64
private:
    void logFirstCall64(U32 request, const char* op, U64 arg, U32 value, S32 result);
    U32 loggedRequests64[16] = {};
    U32 loggedRequestCount64 = 0;
#endif
};

FsOpenNode* openDevMixer(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
    return new DevMixer(node, flags);
}

U32 DevMixer::ioctl(KThread* thread, U32 request) {
    U32 len = (request >> 16) & 0x3FFF;
    CPU* cpu = thread->cpu;
    KMemory* memory = thread->memory;
    //bool read = (request & 0x40000000) != 0;
    bool write = (request & 0x80000000) != 0;

    OssIoctlArg32 arg(memory, IOCTL_ARG1);

    switch (request & 0xFFFF) {
    case 0x5801: // SNDCTL_SYSINFO
        if (write) {
            ossWriteSysInfo(arg, len);
            return 0;
        }
        break;
    case 0x5807: // SNDCTL_AUDIOINFO
        if (write) {
            // The device index the caller asked about is the first field and
            // is echoed back. Only one audio device exists (numaudios == 1 in
            // the sysinfo above), and it is /dev/dsp.
            ossWriteAudioInfo(arg, (S32)arg.readd(0), "BoxedWine mixer",
                              PCM_CAP_OUTPUT,
                              AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_BE,
                              11025, 48000, 1, 2, "/dev/dsp");
            return 0;
        }
    }
    return -K_ENODEV;
}

#ifdef BOXEDWINE_GUEST_X64
void DevMixer::logFirstCall64(U32 request, const char* op, U64 arg, U32 value,
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
    klog_fmt("BOXEDWINE_X64_MIXER op=%s request=0x%x arg=0x%llx value=%u result=%d",
             op, request, (unsigned long long)arg, value, result);
}

// /dev/mixer is the node Wine's OSS driver opens first, and the only one it
// opens before deciding whether the backend exists at all: oss_test_connect
// opens it O_RDONLY and issues SNDCTL_SYSINFO, and a failure there is reported
// as Priority_Unavailable, which makes mmdevapi skip the driver entirely.
U32 DevMixer::ioctl64(U32 request, U64 argAddress, KMemory64* memory) {
    if (!memory) {
        return (U32)-K_ENOTTY;
    }
    const U32 code = request & 0xFFFF;
    const U32 len = (request >> 16) & 0x3FFF;
    const bool write = (request & 0x80000000) != 0;
    OssIoctlArg64 arg(memory, argAddress);
    const char* op = "unknown";
    U32 value = 0;
    S32 result = 0;

    if (code == 0x5801) { // SNDCTL_SYSINFO
        op = "SNDCTL_SYSINFO";
        if (!write) {
            return (U32)-K_ENOTTY;
        }
        ossWriteSysInfo(arg, len);
    } else if (code == 0x5807) { // SNDCTL_AUDIOINFO
        op = "SNDCTL_AUDIOINFO";
        if (!write) {
            return (U32)-K_ENOTTY;
        }
        value = arg.readd(0);
        if ((S32)value > 0) {
            // Only device 0 exists. Answering anything else with a copy of it
            // would give Wine duplicate endpoints for the same devnode.
            result = -K_EINVAL;
        } else {
            ossWriteAudioInfo(arg, (S32)value, "BoxedWine mixer", PCM_CAP_OUTPUT,
                              AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_BE,
                              11025, 48000, 1, 2, "/dev/dsp");
        }
    } else if ((code & 0xFF00) == 0x4D00) {
        // The SOUND_MIXER_READ_*/SOUND_MIXER_WRITE_* pair Wine's aux (winmm
        // auxiliary) device uses. There is no hardware mixer behind /dev/dsp;
        // report both channels at full scale and accept, but ignore, a write.
        // Refusing would make Wine report the aux device as absent, which is
        // a worse answer than a fixed volume.
        // Both MIXER_READ (_SIOR) and MIXER_WRITE (_SIOWR) carry the READ
        // direction bit, so both expect the resulting level written back.
        op = "SOUND_MIXER";
        value = 100 | (100 << 8);
        if (write) {
            arg.writed(0, value);
        }
    } else {
        return (U32)-K_ENOTTY;
    }
    this->logFirstCall64(request, op, argAddress, value, result);
    return (U32)result;
}
#endif
