/*
 * BoxedVN - reject structurally invalid graphics pipelines before MoltenVK.
 * GPLv2; see license.txt.
 */

#ifndef __VK_PIPELINE_GUARD_H__
#define __VK_PIPELINE_GUARD_H__

#include <cstdint>

// A graphics pipeline needs at least one shader stage. The Vulkan spec makes
// stageCount >= 1 a requirement for every pipeline that is not built as a
// library, and MoltenVK does not defend itself against the degenerate case:
// with no stages it builds a MTLRenderPipelineDescriptor whose vertex function
// is nil and then dereferences it. On macOS that is an outright SIGSEGV
// (reproduced locally against the bundled MoltenVK).
//
// On iOS the same fault is worse than a crash. Boxedwine installs a SIGSEGV
// handler to service *guest* page faults, so a host fault raised inside a
// Vulkan call is delivered to a handler that cannot map it to guest memory.
// The emulation thread then wedges: it stays RUNNABLE, executes no further
// guest dispatches, and no compile timeout can rescue it, because it never
// reached the shader compiler.
//
// Refusing the call is therefore strictly better than forwarding it. A
// rejected pipeline is a visible, recoverable error the guest can handle;
// a forwarded one takes the process down or freezes it forever.
//
// VK_PIPELINE_CREATE_LIBRARY_BIT_KHR is the one case where a stage-less
// graphics pipeline is legal, so it is excluded from the rule rather than
// blanket-rejected.
constexpr std::uint32_t kVkPipelineCreateLibraryBitKHR = 0x00000800;

struct VkGraphicsPipelineGuardDecision {
    bool submittable = true;
    // Set only when the pipeline is refused; names the specific violation so
    // the device log points at a cause instead of a symptom.
    const char* reason = nullptr;
};

inline VkGraphicsPipelineGuardDecision vkInspectGraphicsPipeline(
    std::uint32_t stageCount, bool stagesPointerPresent,
    std::uint32_t flags, std::uint32_t stageMask = 1,
    bool linkedLibraries = false) {
    VkGraphicsPipelineGuardDecision decision;

    if (stageCount && !stagesPointerPresent) {
        decision.submittable = false;
        decision.reason = "pStages is NULL while stageCount is non-zero";
        return decision;
    }
    if ((flags & kVkPipelineCreateLibraryBitKHR) != 0 || linkedLibraries) {
        // Partial libraries, and complete pipelines linking stages from
        // libraries, may legitimately supply no stages of their own.
        return decision;
    }
    if (stageCount == 0) {
        decision.submittable = false;
        decision.reason = "stageCount is 0; a graphics pipeline that is not a "
                          "library must provide at least a vertex stage";
        return decision;
    }
    // A complete pipeline needs a vertex or mesh shader. Fragment-only
    // pipelines hit the same MoltenVK null pVertexSS dereference as zero
    // stages. Both EXT and NV mesh stage bits are 0x80.
    if (!(stageMask & (0x1u | 0x80u))) {
        decision.submittable = false;
        decision.reason = "complete graphics pipeline has no vertex or mesh stage";
        return decision;
    }
    return decision;
}

#endif
