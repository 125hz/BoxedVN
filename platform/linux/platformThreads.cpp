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

#ifdef BOXEDWINE_HOST_EXCEPTIONS

#include <signal.h>

void platformHandler(int sig, siginfo_t* info, void* vcontext);
void platformChainHostSignal(int sig, siginfo_t* info, void* vcontext);

#ifdef __MACH__
#include <mach/task.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#endif

// Boxedwine is not the only fault handler in this address space. A second
// translator (FEX, for the 64-bit guest) installs its own handlers for the same
// four signals and relies on them being reached, and Darwin's default action is
// what must run for a fault that belongs to neither. Remember whatever was
// installed before us so platformHandler can pass on a fault it cannot
// attribute to a Boxedwine CPU. Returning from a synchronous fault handler
// without changing the context re-runs the faulting instruction, so a silent
// return is not "ignore", it is an unbreakable fault loop.
static const int platformHandledSignals[] = { SIGBUS, SIGSEGV, SIGILL, SIGFPE };
static const unsigned platformHandledSignalCount =
    sizeof(platformHandledSignals) / sizeof(platformHandledSignals[0]);
static struct sigaction platformPreviousActions[4];

static int platformSignalIndex(int sig) {
    for (unsigned i = 0; i < platformHandledSignalCount; i++) {
        if (platformHandledSignals[i] == sig) {
            return (int)i;
        }
    }
    return -1;
}

void platformChainHostSignal(int sig, siginfo_t* info, void* vcontext) {
    const int index = platformSignalIndex(sig);
    if (index < 0) {
        return;
    }
    const struct sigaction previous = platformPreviousActions[index];
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction) {
        previous.sa_sigaction(sig, info, vcontext);
        return;
    }
    if (previous.sa_handler == SIG_IGN) {
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
        previous.sa_handler(sig);
        return;
    }
    // Nobody else claimed this fault. Restore the default action and re-raise
    // so the process dies with the real signal instead of spinning forever on
    // the faulting instruction.
    sigaction(sig, &previous, nullptr);
    raise(sig);
}

void platformInitExceptionHandling() {
    static bool initializedHandler = false;
    if (!initializedHandler) {
        struct sigaction sa {};
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = platformHandler;
        sa.sa_flags = SA_SIGINFO;
        for (unsigned i = 0; i < platformHandledSignalCount; i++) {
            struct sigaction oldsa {};
            if (sigaction(platformHandledSignals[i], &sa, &oldsa) != 0) {
                continue;
            }
            // Guard against chaining to ourselves if this ever runs twice.
            if ((oldsa.sa_flags & SA_SIGINFO) != 0 &&
                oldsa.sa_sigaction == platformHandler) {
                continue;
            }
            platformPreviousActions[i] = oldsa;
        }
        //sigaction(SIGTRAP, &sa, &oldsa);
        initializedHandler = true;
#ifdef __MACH__
        // proc hand -p true -s false SIGSEGV
        // proc hand -p true -s false SIGBUS
        // in the debug out put window, (lldb) enter the above 2 commands in order to run while debugging on Mac

        // set a break point on this line then enter the above commands.

        task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
    }
}

#endif
