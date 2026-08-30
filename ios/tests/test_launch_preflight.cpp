#include "boxedvn_test.h"
#include "boxedvn/launch_preflight.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace boxedvn;

namespace {

// A minimal stand-in for the launch path: a main queue that only runs when
// something drains it, a background worker that may never report, and the
// bookkeeping the real runtime performs at each step. It models the ordering
// guarantees, not UIKit.
class LaunchHarness {
public:
    std::vector<std::string> mainQueue;
    std::vector<std::string> events;
    std::uint64_t currentGeneration = 0;
    bool presentationInstalled = false;
    std::string state = "idle";
    LaunchPreflight preflight;

    // BVNRuntimeRequestLaunch: accept, schedule the probes off the main
    // thread, and return. The app stays in library mode -- no scene rotation,
    // no refresh-rate hold, no SDL event pump and no DXMT layer, because none
    // of those belong to anything until a guest session actually starts.
    // There is no guest overlay to show here either: it is installed only
    // once SDL has created and attached the guest window.
    void requestLaunch() {
        state = "starting";
        ++currentGeneration;
        preflight.begin(currentGeneration);
        events.push_back("launch-returned");
    }

    // The success path, and only the success path, enters guest presentation.
    void enterGuestPresentation() {
        presentationInstalled = true;
        events.push_back("presentation");
    }

    void enqueueMain(const std::string& item) { mainQueue.push_back(item); }

    void drainMain() {
        std::vector<std::string> pending;
        pending.swap(mainQueue);
        for (const std::string& item : pending) {
            events.push_back("main:" + item);
        }
    }

    // The completion the background worker dispatches back to main.
    void deliver(LaunchPreflightOutcome outcome) {
        const LaunchPreflightAction action =
            preflight.complete(outcome, currentGeneration);
        switch (action) {
            case LaunchPreflightAction::Ignore:
                events.push_back("ignored");
                return;
            case LaunchPreflightAction::StartSession:
                enterGuestPresentation();
                events.push_back("boxedmain");
                state = "running";
                releasePresentation();
                return;
            case LaunchPreflightAction::Fail:
                // Nothing guest-owned was acquired, so nothing is released.
                state = "failed";
                events.push_back("failed");
                return;
        }
    }

    void releasePresentation() {
        if (!preflight.claimCleanup()) {
            events.push_back("double-cleanup");
            return;
        }
        presentationInstalled = false;
        events.push_back("cleanup");
    }

    int count(const std::string& what) const {
        int n = 0;
        for (const std::string& event : events) {
            if (event == what) ++n;
        }
        return n;
    }
};

} // namespace

BOXEDVN_TEST(launch_preflight_returns_before_a_probe_that_never_completes) {
    // The regression this exists for: the probes used to run inline on the
    // main thread behind a six second semaphore wait, so nothing queued on
    // main could run and the starting presentation never rendered.
    LaunchHarness harness;
    harness.requestLaunch();
    CHECK(harness.events.size() == 1);
    CHECK(harness.events[0] == "launch-returned");

    // A main-queue sentinel enqueued while the probe is still outstanding must
    // run, and it must run before any completion is delivered.
    harness.enqueueMain("sentinel");
    harness.drainMain();
    CHECK(harness.count("main:sentinel") == 1);

    // Still waiting: the state stays visible, the app is still in library
    // mode, and no guest facility has been started.
    CHECK(harness.state == "starting");
    CHECK(!harness.presentationInstalled);
    CHECK(harness.count("presentation") == 0);
    CHECK(harness.count("boxedmain") == 0);
    CHECK(harness.count("cleanup") == 0);
    CHECK(harness.preflight.outcome() == LaunchPreflightOutcome::Pending);

    // Ordering: the launch returned, then the main queue ran, and only then
    // could any completion be delivered.
    CHECK(harness.events[0] == "launch-returned");
    CHECK(harness.events[1] == "main:sentinel");
}

BOXEDVN_TEST(launch_preflight_timeout_fails_and_cleans_up_exactly_once) {
    LaunchHarness harness;
    harness.requestLaunch();
    harness.enqueueMain("sentinel");
    harness.drainMain();

    harness.deliver(LaunchPreflightOutcome::TimedOut);
    CHECK(harness.state == "failed");
    // The app never left library mode, so there is nothing to tear down.
    CHECK(!harness.presentationInstalled);
    CHECK(harness.count("presentation") == 0);
    CHECK(harness.count("failed") == 1);
    CHECK(harness.count("cleanup") == 0);
    CHECK(harness.count("boxedmain") == 0);

    // The abandoned probe reporting later must change nothing.
    harness.deliver(LaunchPreflightOutcome::Ready);
    CHECK(harness.count("boxedmain") == 0);
    CHECK(harness.count("presentation") == 0);
    CHECK(harness.count("failed") == 1);
    CHECK(harness.count("double-cleanup") == 0);
    CHECK(harness.state == "failed");
}

BOXEDVN_TEST(launch_preflight_success_starts_the_session_once) {
    LaunchHarness harness;
    harness.requestLaunch();
    harness.deliver(LaunchPreflightOutcome::Ready);
    CHECK(harness.state == "running");
    // Guest presentation is entered only here, and strictly before the
    // session starts.
    CHECK(harness.count("presentation") == 1);
    CHECK(harness.count("boxedmain") == 1);
    CHECK(harness.count("cleanup") == 1);
    CHECK(!harness.presentationInstalled);
    const auto presentationAt = std::find(harness.events.begin(),
                                          harness.events.end(), "presentation");
    const auto sessionAt = std::find(harness.events.begin(),
                                     harness.events.end(), "boxedmain");
    CHECK(presentationAt < sessionAt);

    // A duplicate completion cannot start a second session or release the
    // presentation again.
    harness.deliver(LaunchPreflightOutcome::Ready);
    CHECK(harness.count("boxedmain") == 1);
    CHECK(harness.count("cleanup") == 1);
    CHECK(harness.count("double-cleanup") == 0);
}

BOXEDVN_TEST(launch_preflight_rejects_a_superseded_completion) {
    // A newer launch was accepted while the probes were still running. The
    // stale completion must not start an obsolete session, and must not
    // release a presentation the newer launch now owns.
    LaunchHarness harness;
    harness.requestLaunch();
    const std::uint64_t stale = harness.preflight.generation();

    ++harness.currentGeneration; // a newer launch token
    CHECK(harness.currentGeneration != stale);

    harness.deliver(LaunchPreflightOutcome::Ready);
    CHECK(harness.count("ignored") == 1);
    CHECK(harness.count("boxedmain") == 0);
    CHECK(harness.count("presentation") == 0);
    CHECK(harness.count("cleanup") == 0);
    CHECK(!harness.presentationInstalled);
    CHECK(!harness.preflight.sessionStarted());
}

BOXEDVN_TEST(launch_preflight_ignores_a_pending_completion) {
    LaunchPreflight preflight;
    preflight.begin(7);
    CHECK(preflight.complete(LaunchPreflightOutcome::Pending, 7) ==
          LaunchPreflightAction::Ignore);
    CHECK(!preflight.completed());
    // A real outcome afterwards is still honoured.
    CHECK(preflight.complete(LaunchPreflightOutcome::Ready, 7) ==
          LaunchPreflightAction::StartSession);
    CHECK(preflight.completed());
}

BOXEDVN_TEST(launch_preflight_cleanup_is_claimed_by_one_caller) {
    LaunchPreflight preflight;
    preflight.begin(1);
    CHECK(preflight.claimCleanup());
    CHECK(!preflight.claimCleanup());
    CHECK(preflight.cleanedUp());

    // A fresh attempt starts with an unclaimed cleanup.
    preflight.begin(2);
    CHECK(!preflight.cleanedUp());
    CHECK(preflight.claimCleanup());
}
