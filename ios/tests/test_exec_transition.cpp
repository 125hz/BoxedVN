#include "boxedvn_test.h"
#include "boxedvn/exec_transition.h"

#include <string>
#include <vector>

using namespace boxedvn;

namespace {

// A native/FEX x86-64 parent forks a sparse child, and the child execs a
// 64-bit ELF. This models that transition with a step budget instead of a wall
// clock, so a phase that never completes is reported rather than hanging the
// test the way it hung the device.
class ForkExecHarness {
public:
    static constexpr int kStepBudget = 64;

    ExecTransition transition;
    std::vector<std::string> log;
    // Inherited from the parent by forkProcess64.
    int inheritedCloexecDescriptors = 3;
    int inheritedMappedLeases = 5;
    // A phase named here never ends, standing in for the device stall.
    ExecPhase stallAt = ExecPhase::Count;
    bool parentCanReap = false;

    ExecReplacementImage wineserverImage() const {
        ExecReplacementImage image;
        image.memoryToken = 0xC0FFEE;
        image.cpuMemoryToken = 0xC0FFEE;
        image.imageBase = 0x7b80000000;
        image.imageEnd = 0x7b80100000;
        image.entryRip = 0x7b8001f540;
        image.stackPointer = 0x7a2003ff00;
        return image;
    }

    // Returns the number of steps consumed. Never loops without bound: the
    // point of the test is that exec makes progress.
    int run() {
        int steps = 0;
        transition.begin();
        const ExecPhase order[] = {
            ExecPhase::ThreadReset,   ExecPhase::CloseOnExec,
            ExecPhase::SharedMemory,  ExecPhase::RetireMappedFiles,
            ExecPhase::SignalsAndTimers, ExecPhase::SiblingThreads,
            ExecPhase::Loader,
        };
        for (ExecPhase phase : order) {
            if (++steps > kStepBudget) break;
            if (!transition.beginPhase(phase)) return steps;
            log.push_back(std::string(execPhaseName(phase)) + "-begin");
            if (phase == stallAt) {
                // The device shape: the phase begins and nothing follows.
                return steps;
            }
            switch (phase) {
                case ExecPhase::CloseOnExec:
                    for (int i = 0; i < inheritedCloexecDescriptors; ++i) {
                        transition.retireCloseOnExecDescriptor();
                    }
                    break;
                case ExecPhase::RetireMappedFiles:
                    for (int i = 0; i < inheritedMappedLeases; ++i) {
                        transition.retireMappedFileLease();
                    }
                    break;
                case ExecPhase::Loader:
                    transition.installReplacementImage(wineserverImage());
                    break;
                default:
                    break;
            }
            if (++steps > kStepBudget) break;
            if (!transition.endPhase(phase)) return steps;
            log.push_back(std::string(execPhaseName(phase)) + "-end");
        }
        if (transition.finish() == ExecTransitionResult::Completed) {
            // Only a child that finished its exec can later exit and be
            // observed by the parent's wait4.
            parentCanReap = true;
        }
        return steps;
    }

    int count(const std::string& what) const {
        int n = 0;
        for (const std::string& entry : log) {
            if (entry == what) ++n;
        }
        return n;
    }
};

} // namespace

BOXEDVN_TEST(exec_transition_completes_every_phase_without_deadlock) {
    ForkExecHarness harness;
    const int steps = harness.run();
    CHECK(steps <= ForkExecHarness::kStepBudget);
    CHECK(harness.transition.result() == ExecTransitionResult::Completed);

    // Every phase the child has to unwind actually ran to completion.
    CHECK(harness.transition.phaseCompleted(ExecPhase::ThreadReset));
    CHECK(harness.transition.phaseCompleted(ExecPhase::CloseOnExec));
    CHECK(harness.transition.phaseCompleted(ExecPhase::SharedMemory));
    CHECK(harness.transition.phaseCompleted(ExecPhase::RetireMappedFiles));
    CHECK(harness.transition.phaseCompleted(ExecPhase::SignalsAndTimers));
    CHECK(harness.transition.phaseCompleted(ExecPhase::SiblingThreads));
    CHECK(harness.transition.phaseCompleted(ExecPhase::Loader));

    // The loader is reached, which is precisely what the device never did.
    CHECK(harness.count("loader-begin") == 1);
    CHECK(harness.count("loader-end") == 1);
}

BOXEDVN_TEST(exec_transition_installs_a_usable_replacement_image) {
    ForkExecHarness harness;
    harness.run();
    const ExecReplacementImage& image = harness.transition.replacementImage();

    // The replacement address space and CPU reference each other.
    CHECK(image.crossLinked());
    // The entry point comes from the new image, not the inherited one.
    CHECK(image.belongsToImage(image.entryRip));
    CHECK(!image.belongsToImage(0));
    CHECK(image.stackPointer != 0);
    CHECK(image.usable());
}

BOXEDVN_TEST(exec_transition_retires_inherited_state_exactly_once) {
    ForkExecHarness harness;
    harness.inheritedCloexecDescriptors = 3;
    harness.inheritedMappedLeases = 5;
    harness.run();
    CHECK(harness.transition.closeOnExecRetirements() == 3);
    CHECK(harness.transition.mappedFileRetirements() == 5);

    // Running the transition again starts from a clean slate rather than
    // retiring the parent's leases a second time.
    harness.transition.begin();
    CHECK(harness.transition.closeOnExecRetirements() == 0);
    CHECK(harness.transition.mappedFileRetirements() == 0);
}

BOXEDVN_TEST(exec_transition_reports_the_phase_that_stalled) {
    // The device shape: a phase begins and nothing follows. The transition
    // must name it rather than appearing complete, and the test must finish
    // within its budget rather than hanging the way the child did.
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ExecPhase::Count); ++i) {
        const ExecPhase phase = static_cast<ExecPhase>(i);
        ForkExecHarness harness;
        harness.stallAt = phase;
        const int steps = harness.run();
        CHECK(steps <= ForkExecHarness::kStepBudget);
        CHECK(harness.transition.finish() == ExecTransitionResult::Stalled);
        CHECK(harness.transition.stalledPhase() == phase);
        CHECK(!harness.parentCanReap);
        CHECK(harness.count(std::string(execPhaseName(phase)) + "-begin") == 1);
        CHECK(harness.count(std::string(execPhaseName(phase)) + "-end") == 0);
    }
}

BOXEDVN_TEST(exec_transition_rejects_a_reordered_or_repeated_phase) {
    ExecTransition transition;
    transition.begin();
    // The loader cannot run before the inherited process has been unwound.
    CHECK(!transition.beginPhase(ExecPhase::Loader));
    CHECK(transition.finish() == ExecTransitionResult::OutOfOrder);

    transition.begin();
    CHECK(transition.beginPhase(ExecPhase::ThreadReset));
    CHECK(transition.endPhase(ExecPhase::ThreadReset));
    // Retiring the same inherited state twice is a bug, not a retry.
    CHECK(!transition.beginPhase(ExecPhase::ThreadReset));
    CHECK(transition.finish() == ExecTransitionResult::OutOfOrder);
}

BOXEDVN_TEST(exec_transition_without_an_image_is_not_complete) {
    // Every phase ran, but the loader produced nothing usable. That must not
    // report success: the parent would wait on a child that can never run.
    ExecTransition transition;
    transition.begin();
    const ExecPhase order[] = {
        ExecPhase::ThreadReset,   ExecPhase::CloseOnExec,
        ExecPhase::SharedMemory,  ExecPhase::RetireMappedFiles,
        ExecPhase::SignalsAndTimers, ExecPhase::SiblingThreads,
        ExecPhase::Loader,
    };
    for (ExecPhase phase : order) {
        CHECK(transition.beginPhase(phase));
        CHECK(transition.endPhase(phase));
    }
    CHECK(transition.finish() == ExecTransitionResult::ImageNotInstalled);

    // An image whose CPU still points at the inherited address space is not
    // usable either.
    transition.begin();
    for (ExecPhase phase : order) {
        CHECK(transition.beginPhase(phase));
        CHECK(transition.endPhase(phase));
    }
    ExecReplacementImage crossed;
    crossed.memoryToken = 0xC0FFEE;
    crossed.cpuMemoryToken = 0xDEAD;
    crossed.imageBase = 0x7b80000000;
    crossed.imageEnd = 0x7b80100000;
    crossed.entryRip = 0x7b8001f540;
    crossed.stackPointer = 0x7a2003ff00;
    transition.installReplacementImage(crossed);
    CHECK(!crossed.crossLinked());
    CHECK(transition.finish() == ExecTransitionResult::ImageNotInstalled);
}
