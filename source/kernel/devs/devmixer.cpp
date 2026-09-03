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
    if (result == -K_ENOTTY) {
        // The one line to grep for when a 64-bit guest's audio stops at a
        // request this device does not model.
        klog_fmt("BOXEDWINE_X64_OSS_IOCTL node=/dev/mixer nr=0x%x status=unimplemented",
                 request);
    }
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
    } else if (code == 0x5807 || code == 0x580C) {
        // SNDCTL_AUDIOINFO ('X', 7) and SNDCTL_ENGINEINFO ('X', 12) answer the
        // same oss_audioinfo. Wine issues ENGINEINFO on the dsp node, but
        // OSSv4 accepts it on a mixer node too, and a client that asks here
        // should not be told the mixer has no engine behind it.
        op = (code == 0x5807) ? "SNDCTL_AUDIOINFO" : "SNDCTL_ENGINEINFO";
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
        // The 'M' group. Most of it is MIXER_READ(channel) /
        // MIXER_WRITE(channel), where the channel number is the low byte --
        // but six of those numbers are not channels at all, and answering
        // them with a level tells a client the mixer has 100 of whatever it
        // asked about, when what it asked for was a bit mask or a name.
        const U32 channel = code & 0xFF;
        if (channel == 101) { // SOUND_MIXER_INFO
            // mixer_info: char id[16], char name[32], int modify_counter,
            // int fillers[10]. Zeroed first, because writeString copies only
            // up to the terminator and the rest of each field would otherwise
            // be whatever the caller left there.
            op = "SOUND_MIXER_INFO";
            for (U32 i = 0; i < len / 4; i++) {
                arg.writed(i * 4, 0);
            }
            arg.writeString(0, "BoxedWine");
            arg.writeString(16, "BoxedWine OSS mixer");
        } else if (channel >= 0xFB) {
            // SOUND_MIXER_STEREODEVS (0xfb), _CAPS (0xfc), _RECMASK (0xfd),
            // _DEVMASK (0xfe) and _RECSRC (0xff): bit masks, not levels.
            op = "SOUND_MIXER_MASK";
            switch (channel) {
            case 0xFB: // SOUND_MIXER_STEREODEVS
            case 0xFE: // SOUND_MIXER_DEVMASK
                // The six channels Wine's aux device enumerates: VOLUME (0),
                // SYNTH (3), PCM (4), LINE (6), MIC (7) and CD (8). All of
                // them are stereo, because all of them are the same fixed
                // pair of levels.
                value = (1u << 0) | (1u << 3) | (1u << 4) |
                        (1u << 6) | (1u << 7) | (1u << 8);
                break;
            default:
                // No recording sources, and no mixer capabilities to claim.
                value = 0;
                break;
            }
            if (write) {
                arg.writed(0, value);
            }
        } else {
            // The SOUND_MIXER_READ_*/SOUND_MIXER_WRITE_* pair Wine's aux
            // (winmm auxiliary) device uses. There is no hardware mixer
            // behind /dev/dsp; report both channels at full scale and accept,
            // but ignore, a write. Refusing would make Wine report the aux
            // device as absent, which is a worse answer than a fixed volume.
            // Both MIXER_READ (_SIOR) and MIXER_WRITE (_SIOWR) carry the READ
            // direction bit, so both expect the resulting level written back.
            op = "SOUND_MIXER";
            value = 100 | (100 << 8);
            if (write) {
                arg.writed(0, value);
            }
        }
    } else {
        // Only the OSS groups are named in the log. Everything else reaching
        // a mixer fd -- FIONBIO and FIONREAD are the ones that do -- belongs
        // to syscall64.cpp, which is keyed on this same -ENOTTY, and would
        // otherwise fill the log with witnesses for requests that were never
        // this device's to answer.
        const U32 group = code & 0xFF00;
        if (group == 0x5000 || group == 0x5100 || group == 0x5800) {
            this->logFirstCall64(request, "unimplemented", argAddress, 0,
                                 -K_ENOTTY);
        }
        return (U32)-K_ENOTTY;
    }
    this->logFirstCall64(request, op, argAddress, value, result);
    return (U32)result;
}
#endif
