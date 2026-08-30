/*
 * BoxedVN - the phase order a 64-bit exec has to complete.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * A fork child that execs unwinds the whole inherited process before it can
 * load its replacement image: close-on-exec descriptors, shared memory,
 * inherited mapped-file leases, signal and timer state, sibling threads, the
 * thread's own reset, and only then the loader. A child that stalls anywhere
 * in that sequence produces no output at all, and its parent waits in wait4
 * forever -- which is the correct thing for the parent to do.
 *
 * The ordering, the exactly-once retirement of inherited state, and the
 * requirement that the replacement image is installed before the transition is
 * called complete are all pure bookkeeping. They live here so they can be
 * exercised on any host, with a step budget instead of a wall clock, without a
 * process, an address space or a device.
 */

#ifndef BOXEDVN_EXEC_TRANSITION_H
#define BOXEDVN_EXEC_TRANSITION_H

#include <cstdint>

namespace boxedvn {

enum class ExecPhase : std::uint8_t {
    ThreadReset = 0,
    CloseOnExec = 1,
    SharedMemory = 2,
    RetireMappedFiles = 3,
    SignalsAndTimers = 4,
    SiblingThreads = 5,
    Loader = 6,
    Count = 7,
};

inline constexpr const char* execPhaseName(ExecPhase phase) noexcept {
    switch (phase) {
        case ExecPhase::ThreadReset: return "thread-reset";
        case ExecPhase::CloseOnExec: return "cloexec";
        case ExecPhase::SharedMemory: return "shm";
        case ExecPhase::RetireMappedFiles: return "retire-mapped";
        case ExecPhase::SignalsAndTimers: return "signals";
        case ExecPhase::SiblingThreads: return "siblings";
        case ExecPhase::Loader: return "loader";
        case ExecPhase::Count: return "?";
    }
    return "?";
}

enum class ExecTransitionResult : std::uint8_t {
    // Every phase began and ended, and the replacement image is installed.
    Completed = 0,
    // A phase began and never ended within the step budget. `stalledPhase`
    // names it; this is the shape the device log showed.
    Stalled = 1,
    // A phase ran out of order, or one ran twice.
    OutOfOrder = 2,
    // The transition finished without a usable replacement image.
    ImageNotInstalled = 3,
};

// The state a 64-bit exec must leave behind. The replacement address space and
// CPU have to reference each other, and the entry point and stack have to come
// from the new image rather than the inherited one.
struct ExecReplacementImage {
    std::uint64_t memoryToken = 0;
    std::uint64_t cpuMemoryToken = 0;
    std::uint64_t entryRip = 0;
    std::uint64_t stackPointer = 0;
    std::uint64_t imageBase = 0;
    std::uint64_t imageEnd = 0;

    bool crossLinked() const noexcept {
        return memoryToken != 0 && memoryToken == cpuMemoryToken;
    }
    bool belongsToImage(std::uint64_t address) const noexcept {
        return imageEnd > imageBase && address >= imageBase && address < imageEnd;
    }
    bool usable() const noexcept {
        return crossLinked() && belongsToImage(entryRip) && stackPointer != 0;
    }
};

class ExecTransition {
public:
    void begin() noexcept {
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ExecPhase::Count); ++i) {
            began_[i] = false;
            ended_[i] = false;
        }
        next_ = 0;
        cloexecRetirements_ = 0;
        leaseRetirements_ = 0;
        image_ = ExecReplacementImage {};
        result_ = ExecTransitionResult::Stalled;
        stalled_ = ExecPhase::ThreadReset;
    }

    // Returns false when the phase is out of order or repeated; the caller
    // must not proceed.
    bool beginPhase(ExecPhase phase) noexcept {
        const std::uint8_t index = static_cast<std::uint8_t>(phase);
        if (index != next_ || began_[index]) {
            result_ = ExecTransitionResult::OutOfOrder;
            return false;
        }
        began_[index] = true;
        stalled_ = phase;
        return true;
    }

    bool endPhase(ExecPhase phase) noexcept {
        const std::uint8_t index = static_cast<std::uint8_t>(phase);
        if (!began_[index] || ended_[index]) {
            result_ = ExecTransitionResult::OutOfOrder;
            return false;
        }
        ended_[index] = true;
        next_ = static_cast<std::uint8_t>(index + 1);
        return true;
    }

    // Inherited state is retired once, by the child, and never again.
    void retireCloseOnExecDescriptor() noexcept { ++cloexecRetirements_; }
    void retireMappedFileLease() noexcept { ++leaseRetirements_; }
    std::uint32_t closeOnExecRetirements() const noexcept { return cloexecRetirements_; }
    std::uint32_t mappedFileRetirements() const noexcept { return leaseRetirements_; }

    void installReplacementImage(const ExecReplacementImage& image) noexcept {
        image_ = image;
    }
    const ExecReplacementImage& replacementImage() const noexcept { return image_; }

    // Called once the caller believes the transition is done. A phase that
    // never ended leaves the transition Stalled and names that phase, which is
    // exactly what a device log has to be able to say.
    ExecTransitionResult finish() noexcept {
        if (result_ == ExecTransitionResult::OutOfOrder) {
            return result_;
        }
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ExecPhase::Count); ++i) {
            if (!began_[i] || !ended_[i]) {
                stalled_ = static_cast<ExecPhase>(i);
                result_ = ExecTransitionResult::Stalled;
                return result_;
            }
        }
        if (!image_.usable()) {
            result_ = ExecTransitionResult::ImageNotInstalled;
            return result_;
        }
        result_ = ExecTransitionResult::Completed;
        return result_;
    }

    ExecTransitionResult result() const noexcept { return result_; }
    ExecPhase stalledPhase() const noexcept { return stalled_; }
    bool phaseCompleted(ExecPhase phase) const noexcept {
        const std::uint8_t index = static_cast<std::uint8_t>(phase);
        return began_[index] && ended_[index];
    }

private:
    bool began_[static_cast<std::uint8_t>(ExecPhase::Count)] {};
    bool ended_[static_cast<std::uint8_t>(ExecPhase::Count)] {};
    std::uint8_t next_ = 0;
    std::uint32_t cloexecRetirements_ = 0;
    std::uint32_t leaseRetirements_ = 0;
    ExecReplacementImage image_ {};
    ExecTransitionResult result_ = ExecTransitionResult::Stalled;
    ExecPhase stalled_ = ExecPhase::ThreadReset;
};

} // namespace boxedvn

#endif // BOXEDVN_EXEC_TRANSITION_H
