// Exercise the actual Darwin signal/retry contract, including STP writeback.
// This runs on the macOS ARM64 build host; it does not require FEX or JIT.
#include "fex_callret_guard.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if !defined(__APPLE__) || !defined(__aarch64__)
#error This regression needs Darwin AArch64 signal contexts.
#endif

static std::uint64_t stackBase;
static constexpr std::uint64_t stackSize = 0x1000000;
static volatile sig_atomic_t recovered;

static void faultHandler(int, siginfo_t* info, void* rawContext) {
    auto* machine = static_cast<ucontext_t*>(rawContext)->uc_mcontext;
    const auto pc = machine->__ss.__pc;
    const auto reset = boxedvn::recoverFexCallRetGuard(stackBase, stackSize,
        machine->__ss.__x[25], reinterpret_cast<std::uint64_t>(info->si_addr),
        *reinterpret_cast<const std::uint32_t*>(pc));
    if (!reset || recovered >= 2) _exit(2);
    machine->__ss.__x[25] = reset;
    recovered = recovered + 1;
    // Leave PC untouched: the faulting instruction must retry successfully.
}

int main() {
    constexpr auto guard = boxedvn::fexCallRetHostGuardBytes;
    void* allocation = mmap(nullptr, stackSize + 2 * guard, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (allocation == MAP_FAILED) return 3;
    stackBase = reinterpret_cast<std::uint64_t>(allocation) + guard;
    if (mprotect(reinterpret_cast<void*>(stackBase), stackSize,
                 PROT_READ | PROT_WRITE)) return 4;
    struct sigaction action {}, oldBus {}, oldSegv {};
    action.sa_sigaction = faultHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, &oldBus) ||
        sigaction(SIGSEGV, &action, &oldSegv)) return 5;

    std::uint64_t afterPush, afterPop;
    __asm__ volatile("mov x25, %1\n stp xzr, xzr, [x25, #-16]!\n mov %0, x25"
        : "=r"(afterPush) : "r"(stackBase) : "x25", "memory");
    const auto end = stackBase + stackSize;
    __asm__ volatile("mov x25, %1\n ldp x9, x10, [x25], #16\n mov %0, x25"
        : "=r"(afterPop) : "r"(end) : "x25", "x9", "x10", "memory");

    sigaction(SIGBUS, &oldBus, nullptr);
    sigaction(SIGSEGV, &oldSegv, nullptr);
    munmap(allocation, stackSize + 2 * guard);
    const auto reset = stackBase + stackSize / 4;
    if (recovered != 2 || afterPush != reset - 16 || afterPop != reset + 16) return 6;
    std::puts("FEX callret guard: Darwin STP and LDP fault/retry passed");
    return 0;
}
