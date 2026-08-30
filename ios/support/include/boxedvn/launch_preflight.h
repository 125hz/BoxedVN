/*
 * BoxedVN - launch preflight sequencing.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * The JIT and translator preflight probes are guarded by a timeout that waits
 * on a semaphore. Running that wait on the main thread stops UIKit for its
 * whole duration: the "Starting Wine" presentation never renders, scrolling
 * stops and touches are dropped. The probes therefore run on a background
 * queue and hand their result back to the main thread.
 *
 * That introduces the ordinary hazards of an asynchronous handoff -- a stale
 * completion from an abandoned launch, a cleanup running twice, a failure
 * reported after the session already started. Those are pure bookkeeping, so
 * they live here and can be tested on any host without UIKit, SDL or a device.
 */

#ifndef BOXEDVN_LAUNCH_PREFLIGHT_H
#define BOXEDVN_LAUNCH_PREFLIGHT_H

#include <cstdint>

namespace boxedvn {

enum class LaunchPreflightOutcome : std::uint8_t {
    // The probes have not reported yet.
    Pending = 0,
    // Everything the guest needs is available; the session may start.
    Ready = 1,
    // A probe reported unavailable.
    Rejected = 2,
    // A probe did not report before its timeout.
    TimedOut = 3,
};

// What the completion is allowed to do when it reaches the main thread.
enum class LaunchPreflightAction : std::uint8_t {
    // A newer launch superseded this one: do nothing at all. The launch that
    // replaced it owns the presentation and will release it.
    Ignore = 0,
    // Start the guest session on the main thread.
    StartSession = 1,
    // Report the failure and release the presentation.
    Fail = 2,
};

// One launch attempt's asynchronous bookkeeping. Not thread safe by itself:
// the owner serializes it the way the platform does -- the generation is
// captured before the background work starts and every decision is taken on
// the main thread.
class LaunchPreflight {
public:
    LaunchPreflight() = default;

    // Begin an attempt. `generation` is the launch token captured when the
    // work was scheduled.
    void begin(std::uint64_t generation) noexcept {
        generation_ = generation;
        outcome_ = LaunchPreflightOutcome::Pending;
        completed_ = false;
        cleanedUp_ = false;
        sessionStarted_ = false;
    }

    std::uint64_t generation() const noexcept { return generation_; }
    LaunchPreflightOutcome outcome() const noexcept { return outcome_; }
    bool completed() const noexcept { return completed_; }
    bool cleanedUp() const noexcept { return cleanedUp_; }
    bool sessionStarted() const noexcept { return sessionStarted_; }

    // Decide what the completion may do. `currentGeneration` is the token the
    // runtime holds now. Only the first completion for an attempt is honoured:
    // a probe that reports after its own timeout already fired must not
    // resurrect the launch or release the presentation a second time.
    LaunchPreflightAction complete(LaunchPreflightOutcome outcome,
                                   std::uint64_t currentGeneration) noexcept {
        if (completed_ || outcome == LaunchPreflightOutcome::Pending) {
            return LaunchPreflightAction::Ignore;
        }
        completed_ = true;
        outcome_ = outcome;
        if (currentGeneration != generation_) {
            // Superseded. The launch that replaced this one owns the
            // presentation, so this completion must not tear it down.
            return LaunchPreflightAction::Ignore;
        }
        if (outcome == LaunchPreflightOutcome::Ready) {
            sessionStarted_ = true;
            return LaunchPreflightAction::StartSession;
        }
        return LaunchPreflightAction::Fail;
    }

    // The presentation and event pump are released exactly once, whether the
    // attempt failed in preflight or the session ran to completion.
    bool claimCleanup() noexcept {
        if (cleanedUp_) {
            return false;
        }
        cleanedUp_ = true;
        return true;
    }

private:
    std::uint64_t generation_ = 0;
    LaunchPreflightOutcome outcome_ = LaunchPreflightOutcome::Pending;
    bool completed_ = false;
    bool cleanedUp_ = false;
    bool sessionStarted_ = false;
};

} // namespace boxedvn

#endif // BOXEDVN_LAUNCH_PREFLIGHT_H
