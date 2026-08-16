/*
 * BoxedVN fex64 - symbols needed by the headless Wine bootstrap before the
 * optional graphics, networking, crypto and text unix libraries are linked.
 * Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 */

#include <pthread.h>
#include <setjmp.h>

#if BVN_WINE_BOOT_ENABLED

// These must be C TLS symbols. The iPhoneOS ntdll exit shim jumps back to the
// thread that entered __wine_main instead of terminating the whole app.
_Thread_local jmp_buf wine_ios_exit_jmpbuf;
_Thread_local volatile int wine_ios_exit_code;
_Thread_local pthread_t wine_ios_main_thread;
_Thread_local int wine_ios_exit_initialized;

const char wine_build[] = "wine-ios-fex64";

void winios_phase(const char *name)
{
    (void)name;
}

// Native first boot does not call these unix libraries. Publishing one-entry
// null tables satisfies ntdll's static registry without claiming that a
// subsystem exists; loading one before its real archive lands will fail at
// its first unix call instead of silently invoking the wrong implementation.
const void *dxmt_winemetal_unix_call_funcs[1] = {0};
const void *bcrypt_unix_call_funcs[1] = {0};
const void *crypt32_unix_call_funcs[1] = {0};
const void *dwrite_unix_call_funcs[1] = {0};
const void *nsi_unix_call_funcs[1] = {0};
const void *secur32_unix_call_funcs[1] = {0};
const void *ws2_32_unix_call_funcs[1] = {0};

int win32u_unix_lib_init(void)
{
    return (int)0xc00000bb; /* STATUS_NOT_SUPPORTED */
}

#endif
