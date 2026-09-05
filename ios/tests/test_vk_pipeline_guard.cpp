/* BoxedVN - graphics pipeline structural guard regression tests. GPLv2. */

#include "boxedvn_test.h"

#include "vkpipelineguard.h"

BOXEDVN_TEST(vk_pipeline_guard_allows_a_normal_pipeline) {
    const VkGraphicsPipelineGuardDecision decision =
        vkInspectGraphicsPipeline(2, true, 0);
    CHECK(decision.submittable);
    CHECK(decision.reason == nullptr);
}

BOXEDVN_TEST(vk_pipeline_guard_refuses_a_stageless_pipeline) {
    // A stageless draw. MoltenVK segfaults on this rather than returning an
    // error, and on iOS that wedges the emulation thread instead of crashing.
    const VkGraphicsPipelineGuardDecision decision =
        vkInspectGraphicsPipeline(0, false, 0);
    CHECK(!decision.submittable);
    CHECK(decision.reason != nullptr);
}

BOXEDVN_TEST(vk_pipeline_guard_refuses_a_null_stage_array) {
    const VkGraphicsPipelineGuardDecision decision =
        vkInspectGraphicsPipeline(2, false, 0);
    CHECK(!decision.submittable);
    CHECK(decision.reason != nullptr);
}

BOXEDVN_TEST(vk_pipeline_guard_allows_a_stageless_pipeline_library) {
    // A pipeline library is the one legal way to supply no stages, so the
    // guard must not turn a valid extension use into a hard failure.
    const VkGraphicsPipelineGuardDecision decision =
        vkInspectGraphicsPipeline(0, false, kVkPipelineCreateLibraryBitKHR);
    CHECK(decision.submittable);
}

BOXEDVN_TEST(vk_pipeline_guard_refuses_fragment_only_complete_pipeline) {
    CHECK(!vkInspectGraphicsPipeline(1, true, 0, 0x10).submittable);
    CHECK(vkInspectGraphicsPipeline(2, true, 0, 0x11).submittable);
    CHECK(vkInspectGraphicsPipeline(2, true, 0, 0x90).submittable);
}

BOXEDVN_TEST(vk_pipeline_guard_preserves_valid_partial_libraries) {
    CHECK(vkInspectGraphicsPipeline(1, true, kVkPipelineCreateLibraryBitKHR, 0x10).submittable);
    CHECK(!vkInspectGraphicsPipeline(1, false, kVkPipelineCreateLibraryBitKHR, 0x10).submittable);
    CHECK(vkInspectGraphicsPipeline(0, false, 0, 0, true).submittable);
}
