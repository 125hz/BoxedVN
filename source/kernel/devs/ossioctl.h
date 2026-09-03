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
 */

#ifndef __OSSIOCTL_H__
#define __OSSIOCTL_H__

#include "oss.h"
#include <string.h>

// A width-agnostic view of the memory an OSS ioctl argument points at.
//
// The IA-32 lane reaches its argument through the calling thread's 32-bit
// KMemory at the address the guest left in EDX (IOCTL_ARG1). A 64-bit guest's
// argument is a 64-bit address in its own KMemory64, and reading it through
// the 32-bit KMemory would truncate the pointer and scribble an unrelated
// page -- which is exactly why source/kernel/syscall64.cpp routes a device
// ioctl through a separate ioctl64 entry point instead of reusing ioctl().
//
// Everything that fills an OSS structure is written once against this
// interface so the two lanes cannot drift in the field layout they answer
// with.
class OssIoctlArg {
public:
    virtual ~OssIoctlArg() {}
    virtual U32 readd(U32 offset) = 0;
    virtual void writed(U32 offset, U32 value) = 0;
    virtual void writeb(U32 offset, U8 value) = 0;
    virtual void writeString(U32 offset, const char* value) = 0;
    virtual U64 address() = 0;
};

class OssIoctlArg32 : public OssIoctlArg {
public:
    OssIoctlArg32(KMemory* memory, U32 base) : memory(memory), base(base) {}
    U32 readd(U32 offset) override {
        return this->memory->readd(this->base + offset);
    }
    void writed(U32 offset, U32 value) override {
        this->memory->writed(this->base + offset, value);
    }
    void writeb(U32 offset, U8 value) override {
        this->memory->writeb(this->base + offset, value);
    }
    void writeString(U32 offset, const char* value) override {
        this->memory->strcpy(this->base + offset, value);
    }
    U64 address() override { return (U64)this->base; }

private:
    KMemory* memory;
    U32 base;
};

#ifdef BOXEDWINE_GUEST_X64
#include "kmemory64.h"

class OssIoctlArg64 : public OssIoctlArg {
public:
    OssIoctlArg64(KMemory64* memory, U64 base) : memory(memory), base(base) {}
    U32 readd(U32 offset) override {
        return this->memory->readd(this->base + offset);
    }
    void writed(U32 offset, U32 value) override {
        this->memory->writed(this->base + offset, value);
    }
    void writeb(U32 offset, U8 value) override {
        this->memory->writeb(this->base + offset, value);
    }
    void writeString(U32 offset, const char* value) override {
        // memcpyToGuest, not a byte loop: KMemory64 has no strcpy, and the
        // trailing NUL has to reach the guest or an OSSv4 client reads the
        // name off the end of the field.
        this->memory->memcpyToGuest(this->base + offset, value,
                                    (U64)::strlen(value) + 1);
    }
    U64 address() override { return this->base; }

private:
    KMemory64* memory;
    U64 base;
};
#endif

// The oss_audioinfo an OSSv4 client reads back from SNDCTL_ENGINEINFO on
// /dev/dsp and from SNDCTL_AUDIOINFO on /dev/mixer. Both devices answered
// this structure with their own hand-walked offsets before, and both walks
// were wrong in compensating ways (handle written as 64 bytes and song_name
// as 32 in one, devnode as 16 in both), which left next_play_engine and
// next_rec_engine landing inside devnode's tail. Nothing Wine reads sat past
// the damage, but the two copies could not both be right, so the walk lives
// here once and follows source/kernel/devs/oss.h field for field.
//
// Wine 9.0's dlls/wineoss.drv/oss.c reads exactly these fields out of it:
// enabled and caps and devnode during endpoint enumeration, devnode again in
// get_default_device, and oformats/min_rate/max_rate/max_channels in
// oss_get_mix_format. Every one of those sits before offset 204.
inline void ossWriteAudioInfo(OssIoctlArg& arg, S32 dev, const char* name,
                              U32 caps, U32 oformats, U32 minRate, U32 maxRate,
                              U32 minChannels, U32 maxChannels,
                              const char* devnode) {
    U32 p = 0;
    arg.writed(p, (U32)dev); p += 4;                     // int dev
    arg.writeString(p, name); p += OSS_DEVNAME_SIZE;     // oss_devname_t name
    arg.writed(p, 0); p += 4;                            // int busy
    arg.writed(p, (U32)-1); p += 4;                      // int pid
    arg.writed(p, caps); p += 4;                         // int caps
    arg.writed(p, 0); p += 4;                            // int iformats
    arg.writed(p, oformats); p += 4;                     // int oformats
    arg.writed(p, 0); p += 4;                            // int magic
    arg.writeString(p, ""); p += OSS_CMD_SIZE;           // oss_cmd_t cmd
    arg.writed(p, 0); p += 4;                            // int card_number
    arg.writed(p, 0); p += 4;                            // int port_number
    arg.writed(p, 0); p += 4;                            // int mixer_dev
    arg.writed(p, 0); p += 4;                            // int legacy_device
    arg.writed(p, 1); p += 4;                            // int enabled
    arg.writed(p, 0); p += 4;                            // int flags
    arg.writed(p, minRate); p += 4;                      // int min_rate
    arg.writed(p, maxRate); p += 4;                      // int max_rate
    arg.writed(p, minChannels); p += 4;                  // int min_channels
    arg.writed(p, maxChannels); p += 4;                  // int max_channels
    arg.writed(p, 0); p += 4;                            // int binding
    arg.writed(p, 0); p += 4;                            // int rate_source
    arg.writeString(p, ""); p += OSS_HANDLE_SIZE;        // oss_handle_t handle
    arg.writed(p, 0); p += 4;                            // unsigned int nrates
    for (U32 i = 0; i < OSS_MAX_SAMPLE_RATES; i++) {
        arg.writed(p, 0); p += 4;                        // rates[20]
    }
    arg.writeString(p, ""); p += OSS_LONGNAME_SIZE;      // oss_longname_t song_name
    arg.writeString(p, ""); p += OSS_LABEL_SIZE;         // oss_label_t label
    arg.writed(p, (U32)-1); p += 4;                      // int latency
    arg.writeString(p, devnode); p += OSS_DEVNODE_SIZE;  // oss_devnode_t devnode
    arg.writed(p, 0); p += 4;                            // int next_play_engine
    arg.writed(p, 0);                                    // int next_rec_engine
}

// The oss_sysinfo a client reads back from SNDCTL_SYSINFO on /dev/mixer.
//
// Wine 9.0's oss_test_connect is the gate this has to pass before mmdevapi
// will pick the driver at all: it sets version[0] to 0xFF and versionnum to
// ~0, issues SNDCTL_SYSINFO, and demotes the driver to Priority_Low unless
// version[0] lands in '4'..'9' and versionnum comes back without its top bit.
// oss_get_endpoint_ids then loops numaudios times over SNDCTL_AUDIOINFO, so
// numaudios has to be at least 1 or the driver reports no endpoints and Wine
// falls through to the next backend.
//
// The whole structure is zeroed first. The previous fill wrote 2000+i into
// every dword before overwriting the head, which left revision_info and
// filler holding a counting pattern that no reader has any use for.
inline void ossWriteSysInfo(OssIoctlArg& arg, U32 structLen) {
    for (U32 i = 0; i < structLen / 4; i++) {
        arg.writed(i * 4, 0);
    }
    U32 p = 0;
    arg.writeString(p, "OSS/Linux"); p += 32;   // char product[32]
    arg.writeString(p, "4.0.0a"); p += 32;      // char version[32]
    arg.writed(p, 0x040000); p += 4;            // int versionnum
    p += 128;                                   // char options[128]
    arg.writed(p, 1); p += 4;                   // int numaudios
    p += 32;                                    // int openedaudio[8]
    arg.writed(p, 1); p += 4;                   // int numsynths
    arg.writed(p, 1); p += 4;                   // int nummidis
    arg.writed(p, 1); p += 4;                   // int numtimers
    arg.writed(p, 1); p += 4;                   // int nummixers
    p += 32;                                    // int openedmidi[8]
    arg.writed(p, 1); p += 4;                   // int numcards
    arg.writed(p, 1); p += 4;                   // int numaudioengines
    arg.writeString(p, "GPL");                  // char license[16]
}

#endif
