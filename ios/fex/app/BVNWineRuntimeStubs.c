/*
 * BoxedVN fex64 - symbols needed by the headless Wine bootstrap before the
 * optional graphics, networking, crypto and text unix libraries are linked.
 * Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 */

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>

#if BVN_WINE_BOOT_ENABLED

// These must be C TLS symbols. The iPhoneOS ntdll exit shim jumps back to the
// thread that entered __wine_main instead of terminating the whole app.
_Thread_local jmp_buf wine_ios_exit_jmpbuf;
_Thread_local volatile int wine_ios_exit_code;
_Thread_local pthread_t wine_ios_main_thread;
_Thread_local int wine_ios_exit_initialized;

jmp_buf *BVNWineExitJumpBuffer(void)
{
    return &wine_ios_exit_jmpbuf;
}

void BVNWinePrepareExitTrap(void)
{
    wine_ios_main_thread = pthread_self();
    wine_ios_exit_code = 0;
    wine_ios_exit_initialized = 1;
}

int BVNWineExitCode(void)
{
    return wine_ios_exit_code;
}

void BVNWineClearExitTrap(void)
{
    wine_ios_exit_initialized = 0;
}

const char wine_build[] = "wine-ios-fex64";

// Winios.m defines this for real once the display driver is linked.
#ifndef BVN_WINE_WIN32U_ENABLED
void winios_phase(const char *name)
{
    (void)name;
}
#endif

// Native first boot does not call these unix libraries. Publishing one-entry
// null tables satisfies ntdll's static registry without claiming that a
// subsystem exists; loading one before its real archive lands will fail at
// its first unix call instead of silently invoking the wrong implementation.

// DXMT's archive defines the real table, so the stub has to step aside when it
// is linked. Without the guard the two definitions collide: the linker prefers
// this object file's copy and never loads the archive member, which is the
// silent-wrong-implementation case the note above warns about - the graphics
// target would keep branching to zero with the real code sitting in the
// binary. scripts/build-fex64-dxmt.sh verifies the archive really exports it,
// and the app build defines BVN_WINE_DXMT_ENABLED only when linking it.
#ifndef BVN_WINE_DXMT_ENABLED
const void *dxmt_winemetal_unix_call_funcs[1] = {0};
#endif
const void *bcrypt_unix_call_funcs[1] = {0};
const void *crypt32_unix_call_funcs[1] = {0};
const void *dwrite_unix_call_funcs[1] = {0};
const void *nsi_unix_call_funcs[1] = {0};
const void *secur32_unix_call_funcs[1] = {0};
const void *ws2_32_unix_call_funcs[1] = {0};

// Report the refusal rather than just returning it.
//
// The graphics target now reaches the guest's message loop: a device log shows
// cube-x64.exe calling PeekMessage and being told a message is waiting while
// the MSG it gets back is entirely zero - no window, no message id - after
// which the guest branches through the address of its own stack buffer and
// dies. A window subsystem whose unix half refuses to initialise is the
// obvious suspect for a message queue that answers like that, and this is the
// refusal.
//
// Whether it is the cause depends on something this file cannot see: whether
// win32u asks at all. If the line never appears, the PE side never got as far
// as its unixlib and the message behaviour comes from somewhere else, which is
// worth knowing before building the unix half of win32u the way DXMT's was
// built. Counted rather than printed once, because a message loop would ask
// repeatedly and the count says whether it kept trying.
// Gone once the real unix side is linked: the archive defines this symbol,
// and two definitions would let this object file win while the archive sat
// unused - the same trap the graphics unix-call table had.
#ifndef BVN_WINE_WIN32U_ENABLED
int win32u_unix_lib_init(void)
{
    static int refusals;

    refusals++;
    if (refusals <= 4 || (refusals % 256) == 0) {
        fprintf(stderr, "[win32u-stub] refusing unixlib init, call #%d "
                        "(STATUS_NOT_SUPPORTED) rev=bvn8\n", refusals);
        fflush(stderr);
    }
    return (int)0xc00000bb; /* STATUS_NOT_SUPPORTED */
}
#endif

#endif
