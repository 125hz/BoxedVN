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

#ifndef __KTHREAD_H__
#define __KTHREAD_H__

#include "getrusagefairness.h"

#define MAX_POLL_DATA 256

#define TLS_ENTRIES 10
#define TLS_ENTRY_START_INDEX 10

class OpenGLVetexPointer {
public:
    OpenGLVetexPointer() = default;
    U32 size = 0;
    U32 type = 0;
    U32 stride = 0;
    U32 count = 0; // used by marshalEdgeFlagPointerEXT
    U32 ptr = 0;
    U8* marshal = nullptr;
    U32 lastMarshalledPtr = 0;
    U32 marshal_size = 0;
    U32 refreshEachCall = 0;
    bool enabled = false;
    bool normalized = false;
    bool isArrayBuffer = false;
    bool isVertexAttrib = false;
};
typedef std::shared_ptr<OpenGLVetexPointer> OpenGLVetexPointerPtr;

class KProcess;
class Memory;
class Wnd;
#ifdef BOXEDWINE_GUEST_X64
class CPU64;
#endif

class KThreadGlContext {
public:
    KThreadGlContext() = default;
    KThreadGlContext(void* context):context(context) {}
    void* context = nullptr;
    bool hasBeenMadeCurrent = false;
    bool sharing = false;
    std::shared_ptr<Wnd> wnd;
};

typedef std::shared_ptr<KThreadGlContext> KThreadGlContextPtr;

class KThread {
public:
    KThread(U32 id, const KProcessPtr& process);
    ~KThread();

    static void runOnMainThread(std::function<void()> callback);

    void addCallbackOnExit(std::function<void(U32 id)> callback) {callbacksOnExit.push_back(callback);}

    void reset();

    struct user_desc* getLDT(U32 index);
    bool isLdtEmpty(struct user_desc* desc);
    U32 signal(U32 signal, bool wait);
    void queuePendingSignal(U32 signal);
    bool readyForSignal(U32 signal);
    void cleanup();

    void seg_mapper(U32 address, bool readFault, bool writeFault, bool throwException=true, bool executeFault=false);
    void seg_access(U32 address, bool readFault, bool writeFault, bool throwException=true, bool executeFault=false);
    void seg_instructionFetch(U32 address, bool throwException=true);
    bool runSignals();
    void runSignal(U32 signal, U32 trapNo, U32 errorNo);
    void signalIllegalInstruction(int code);
    void signalTrap(U32 code);
    void signalDebugTrap(U32 code, U32 dr6);
    bool debugTrapBeforeInstruction();
    bool hasHardwareBreakpointAt(U32 address) const;
    bool hasMemoryWriteBreakpointEnabled() const;
    void updateDebugTrapActive();
    void checkDebugTrapOnMemoryWrite(U32 address, U32 len);
    bool isDebugTrapActive() const;
    void setPtraceStop(U32 signal);
    void resumeFromPtraceStop();
    void waitForPtraceResume();
    void clone(KThread* from);
    void setupStack();
    void setTLS(struct user_desc* desc);

    // syscalls
    U32 futex(U32 addr, U32 op, U32 value, U32 pTime, U32 val2, U32 val3, bool time64) ;
#ifdef BOXEDWINE_GUEST_X64
    S64 futex64(U64 addr, U32 op, U32 value, U64 timeoutAddress, U32 val3);
#endif
    U32 modify_ldt(U32 func, U32 ptr, U32 count);
    U32 signalstack(U32 ss, U32 oss);
    U32 sigprocmask(U32 how, U32 set, U32 oset, U32 sigsetSize);
    U32 sigreturn();
    U32 set_robust_list(U32 head, U32 len);
    U32 get_robust_list(U32 pid, U32 head_ptr, U32 len_ptr);
    U32 rseq(U32 rseq, U32 rseq_len, U32 flags, U32 sig);
    U32 sigsuspend(U32 mask, U32 sigsetSize);
    U32 sigtimedwait(U32 set, U32 info, U32 timeout, U32 sizeofSet, bool time64);
    U32 sleep(U32 ms);
    U32 nanoSleep(U64 nano);
    U32 clockNanoSleep(U32 clock, U32 flags, U64 nano, U32 addressRemain);

    U32 id = 0;
    U64 sigMask = 0; // :TODO: what happens when this is changed while in a signal
    U64 inSigMask = 0;
    U32 alternateStack = 0;
    U32 alternateStackSize = 0;
    CPU* cpu = nullptr;
#ifdef BOXEDWINE_GUEST_X64
    CPU64* cpu64 = nullptr;
#endif
    KProcessPtr process;
    KMemory* const memory;
    bool interrupted = false;
    U32 inSignal = 0;    
#ifdef BOXEDWINE_MULTI_THREADED
    bool exited = false;	
    bool startSignal = false;    
    U64 threadStartTime = 0;
#else
    U64 userTime = 0;    
#endif
    bool terminating = false;
    U32 clear_child_tid = 0;
#ifdef BOXEDWINE_GUEST_X64
    U64 clear_child_tid64 = 0;
    // Futex storm witness: how many times in a row the 64-bit wait path
    // returned without blocking, and when that run started.
    U32 futexImmediateReturns = 0;
    U32 futexImmediateWindowStart = 0;
#endif
    U32 debugRegs[8] = {};
    bool ptraceStopPending = false;
    U32 ptraceStopSignal = 0;
    bool ptraceStopped = false;
    bool ptraceAttached = false;
    bool ptraceSingleStep = false;
    U32 ptraceTracerProcessId = 0;
    BOXEDWINE_CONDITION ptraceCond;

    U64 getThreadUserTime();

    U64 kernelTime = 0;
    U32 inSysCall = 0;
#if defined(BOXEDWINE_IOS) && defined(BOXEDWINE_MULTI_THREADED)
    // A per-thread cache of getrusage(RUSAGE_SELF).
    //
    // Grisaia's guest makes 37,000 of those calls a second as a spin-loop
    // clock, and each one takes KProcess::threadsMutex and walks every thread
    // in the process. Caching it for a millisecond removes 37,000 lock
    // acquisitions and ~370,000 per-thread time computations a second while
    // leaving the answer monotonic and accurate to 1 ms - which is far finer
    // than anything can reasonably read from cumulative CPU time. Per thread,
    // so there is no shared state and no lock of its own.
    U64 cachedSelfRusageMicroseconds = 0;
    U32 cachedSelfUserSeconds = 0;
    U32 cachedSelfUserMicroSeconds = 0;
    U32 cachedSelfKernelSeconds = 0;
    U32 cachedSelfKernelMicroSeconds = 0;
    bool hasCachedSelfRusage = false;

    GetrusageFairness getrusageFairness;
    GetrusageFairness schedYieldFairness;
#endif
    // Cross-thread hang diagnostics read only these atomics, never the live
    // CPU register file. The emulation thread refreshes them at each dispatch
    // boundary, making snapshots safe even while that thread is host-blocked.
    std::atomic<U32> diagnosticEip{0};
    std::atomic<U32> diagnosticEsp{0};
    std::atomic<U32> diagnosticEbp{0};
    std::atomic<U64> diagnosticDispatchCount{0};
    // Dispatch index of the host Vulkan function this thread is currently
    // executing, biased by one so that zero means "not inside a Vulkan call".
    // A host fault raised inside a Vulkan call does not crash cleanly:
    // Boxedwine's guest SIGSEGV handler receives it, cannot map it to guest
    // memory, and the thread wedges RUNNABLE with no further dispatches. The
    // guest EIP then names only the caller, so record the callee too. An index
    // is stored rather than a string because the watchdog reads this from
    // another thread, and a shared name buffer would be a data race.
    std::atomic<U32> diagnosticVulkanCall{0};
    // The 64-bit lane's counterpart, and it answers a different question. The
    // field above names the call a thread is INSIDE, for a watchdog sampling a
    // live process, and is cleared on the way out. This one is never cleared:
    // it names the last Vulkan command the thread dispatched over the bridge,
    // so a thread that has already left the bridge - or left the process - can
    // still be identified as the one that was driving the graphics. A device
    // capture ended with a successful vkAcquireNextImageKHR, four threads
    // parked in futex waits owed no wake, and a fifth thread exiting; nothing
    // on record said whether the thread that left was the one that had
    // acquired, and those are opposite diagnoses. Biased by one, so zero means
    // this thread never reached the bridge at all.
    std::atomic<U32> diagnosticVulkanBridgeCall{0};
    std::atomic<U64> diagnosticVulkanBridgeCalls{0};
    // Guest memory faults serviced for this thread. A fault that cannot be
    // resolved is retried on the same instruction, so the thread stays
    // RUNNABLE, executes no new dispatches, and never updates its EIP - the
    // exact signature of a wedge. Only this counter separates that from a
    // thread that is genuinely blocked, because both look identical otherwise.
    std::atomic<U64> diagnosticFaultCount{0};
    // Guest syscall this thread is currently servicing, biased by one so zero
    // means "not in a syscall". Boxedwine runs syscalls on the emulation
    // thread itself, so one that blocks leaves the thread RUNNABLE with no
    // dispatch progress and no waiting condition - indistinguishable from a
    // spin until the syscall is named.
    std::atomic<U32> diagnosticSyscall{0};
    // Wine-server witness. A thread that has sent a request to the wineserver
    // and is blocked reading the reply is indistinguishable, from outside,
    // from a thread that is legitimately parked waiting on a Windows object:
    // both sit in KUnixSocketObject::lockCond and neither says what it is
    // waiting for. A server that answers nobody and a client that is simply
    // idle look identical, and they are opposite faults. These name the
    // request: the descriptor the thread is reading, when that read began,
    // and the first word of the last message it wrote to a socket, which for
    // the wineserver protocol is the request code. Biased by one where zero
    // has to mean "not doing this", written by the thread itself on the
    // syscall path and read by the hang snapshot from another thread.
    std::atomic<U32> diagnosticSocketReadFd{0};
    std::atomic<U32> diagnosticSocketReadStartMillies{0};
    std::atomic<U32> diagnosticSocketWriteFd{0};
    std::atomic<U32> diagnosticSocketWriteCode{0};
    std::atomic<U32> diagnosticSocketWriteBytes{0};
    std::atomic<U32> diagnosticSocketWriteMillies{0};
    BOXEDWINE_CONDITION waitingForSignalToEndCond;
    BOXEDWINE_CONDITION sigWaitCond;
    U64 sigWaitMask = 0;
    U64 foundWaitSignal = 0;

    U64 waitingForSignalToEndMaskToRestore = 0;
    U64 pendingSignals = 0;
    BOXEDWINE_MUTEX pendingSignalsMutex;
    KThreadGlContextPtr getGlContextById(U32 id);
    void removeGlContextById(U32 id);
    KThreadGlContextPtr addGlContext(U32 id, void* context);
    void removeAllGlContexts();
    bool hasContextBeenMadeCurrentSinceCreation = false;

    BHashTable<U32, std::shared_ptr<KThreadGlContext>> glContext;
    BString name;

    std::vector<KPollData> pollData;
public:
    U32 currentContext = 0;
    U32 currentDrawable = 0;
    U32 currentReadDrawable = 0;
    U32 glLastError = 0;
    bool log = false; // syscalls
    OpenGLVetexPointer glVertextPointer; // 0 index
    BHashTable<U32, OpenGLVetexPointerPtr> glVertextPointersByIndex; // indexes greater than 0
    BHashTable<U32, OpenGLVetexPointerPtr> glVertexAttribPointerNVByIndex;
    OpenGLVetexPointer glNormalPointer;
    OpenGLVetexPointer glFogPointer;
    OpenGLVetexPointer glFogPointerEXT;
    OpenGLVetexPointer glTangentPointerEXT;
    OpenGLVetexPointer glColorPointer;
    OpenGLVetexPointer glSecondaryColorPointer;
    OpenGLVetexPointer glSecondaryColorPointerEXT;
    OpenGLVetexPointer glIndexPointer;
    U32 glClientActiveTexture = 0x84C0; // GL_TEXTURE0; kthread.h does not include OpenGL headers
    BHashTable<U32, OpenGLVetexPointerPtr> glTexCoordPointersByTexture;
    BHashTable<U32, OpenGLVetexPointerPtr> glMultiTexCoordPointerEXTByTexunit;
    BHashTable<U32, OpenGLVetexPointerPtr> glMultiTexCoordPointerSGISByTarget;
    OpenGLVetexPointer glEdgeFlagPointer;
    OpenGLVetexPointer glEdgeFlagPointerEXT;
    OpenGLVetexPointer glElementPointerAPPLE;
    OpenGLVetexPointer glElementPointerATI;
    BHashTable<U32, OpenGLVetexPointerPtr> glVariantPointerEXTById;
    OpenGLVetexPointer glMatrixIndexPointerARB;
    OpenGLVetexPointer glVertexWeightPointerEXT;
    OpenGLVetexPointer glWeightPointerARB;
    OpenGLVetexPointer glInterleavedArray;
    U32 glInterleavedArrayTexture = 0x84C0; // GL_TEXTURE0
    U32 marshalIndex = 0;

    inline static KThread* currentThread() {return runningThread;}
	inline static void setCurrentThread(KThread* thread) { runningThread = thread; }

    BOXEDWINE_CONDITION waitingCond = nullptr;    
    BOXEDWINE_CONDITION pollCond;
#ifdef BOXEDWINE_MULTI_THREADED
    BOXEDWINE_MUTEX waitingCondSync;
#else
    KListNode<KThread*> scheduledThreadNode;
    KListNode<KThread*> waitThreadNode;
        
    BoxedWineConditionTimer condTimer;
#endif

    U32 condStartWaitTime = 0;
    static void logFutexSnapshot();
private:
    bool isOnAlternateSignalStack(U32 stackAddress) const;
    void internalCleanup();
    void exitRobustList();
    U32 handleFutexDeath(U32 uaddr, bool pi, bool pending_op);

    U32 robustList = 0;

    std::shared_ptr<FsNode> threadNode; // in /proc/<pid>/task/<tid>
    std::shared_ptr<FsNode> commNode; // in /proc/<pid>/task/<tid>/comm

    std::vector< std::function<void(U32) > > callbacksOnExit;

    void clearFutexes();

    thread_local static KThread* runningThread;

    BOXEDWINE_CONDITION sleepCond;      

    struct user_desc tls[TLS_ENTRIES] = {};
    BOXEDWINE_MUTEX tlsMutex;

    static BOXEDWINE_MUTEX_NR futexesMutex;
};

class ChangeThread {
public:
    ChangeThread(KThread* thread);
    ~ChangeThread();
    KThread* savedThread;
};

#define SIGSUSPEND_RETURN 0xF000000000000000l
#define RESTORE_SIGNAL_MASK 0x0FFFFFFFFFFFFFFFl

void OPCALL onExitSignal(CPU* cpu, DecodedOp* op);

void common_signalIllegalInstruction(CPU* cpu, int code);

#endif
