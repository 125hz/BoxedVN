/*
 * BoxedWine - host-side copy of DXMT's shader compilation argument chain.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * DXMT's PE side describes a shader compilation to airconv with a linked
 * chain of SM50_SHADER_*_DATA structures (airconv_public.h): each node has a
 * `next` link, a `type`, and for the input-layout and stream-output nodes a
 * pointer to an `elements` array. The PE side builds the chain in guest
 * memory and airconv, which runs natively on this port, walks it directly
 * (dxbc_converter.hpp, args_get_data). Translating the head pointer at the
 * thunk is not enough: the links and arrays inside are guest pointers too.
 *
 * This copies the chain into thread-local host storage with every nested
 * pointer translated through the same rule as every other guest pointer
 * (include/boxedwine_dxmt_guest_pointer.h). airconv reads the chain only
 * during the compile call it was passed to, so the copy is valid until the
 * calling thread's next compile, and nothing needs freeing.
 *
 * Compiled beside DXMT's rewritten unix sources by
 * scripts/build-dxmt-ios-native.sh; the rewrite makes each SM50Compile*
 * thunk pass BOXEDWINE_SM50_ARGS(params->...) instead of the raw chain.
 */

#include <stddef.h>
#include <stdint.h>

#include "airconv_public.h"
#include "boxedwine_dxmt_guest_pointer.h"

/* More nodes than DXMT ever links (at most one per argument type). */
#define BOXEDWINE_SM50_MAX_ARGUMENTS 16

union boxedwine_sm50_node {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA header;
    struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA stream_output;
    struct SM50_SHADER_COMMON_DATA common;
    struct SM50_SHADER_PSO_PIXEL_SHADER_DATA pixel_shader;
    struct SM50_SHADER_IA_INPUT_LAYOUT_DATA input_layout;
    struct SM50_SHADER_GS_PASS_THROUGH_DATA gs_pass_through;
    struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry_shader;
    struct SM50_SHADER_PSO_TESSELLATOR_DATA tessellator;
};

static _Thread_local union boxedwine_sm50_node
    boxedwine_sm50_nodes[BOXEDWINE_SM50_MAX_ARGUMENTS];

const void* boxedwine_dxmt_sm50_arguments(const void* guest_chain)
{
    const struct SM50_SHADER_COMPILATION_ARGUMENT_DATA* guest =
        BOXEDWINE_GUEST_PTR(
            (const struct SM50_SHADER_COMPILATION_ARGUMENT_DATA*)guest_chain);
    union boxedwine_sm50_node* previous = NULL;
    const void* head = NULL;
    unsigned count = 0;

    while (guest != NULL && count < BOXEDWINE_SM50_MAX_ARGUMENTS) {
        union boxedwine_sm50_node* node = &boxedwine_sm50_nodes[count++];

        switch (guest->type) {
        case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT:
            node->stream_output =
                *(const struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA*)guest;
            node->stream_output.elements =
                BOXEDWINE_GUEST_PTR(node->stream_output.elements);
            break;
        case SM50_SHADER_COMMON:
            node->common = *(const struct SM50_SHADER_COMMON_DATA*)guest;
            break;
        case SM50_SHADER_PSO_PIXEL_SHADER:
            node->pixel_shader =
                *(const struct SM50_SHADER_PSO_PIXEL_SHADER_DATA*)guest;
            break;
        case SM50_SHADER_IA_INPUT_LAYOUT:
            node->input_layout =
                *(const struct SM50_SHADER_IA_INPUT_LAYOUT_DATA*)guest;
            node->input_layout.elements =
                BOXEDWINE_GUEST_PTR(node->input_layout.elements);
            break;
        case SM50_SHADER_GS_PASS_THROUGH:
            node->gs_pass_through =
                *(const struct SM50_SHADER_GS_PASS_THROUGH_DATA*)guest;
            break;
        case SM50_SHADER_PSO_GEOMETRY_SHADER:
            node->geometry_shader =
                *(const struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA*)guest;
            break;
        case SM50_SHADER_PSO_TESSELLATOR:
            node->tessellator =
                *(const struct SM50_SHADER_PSO_TESSELLATOR_DATA*)guest;
            break;
        default:
            /* A node type this copy does not know: keep its header so the
             * walk continues and airconv skips it as it would have. */
            node->header = *guest;
            break;
        }

        /* Re-link through the host copies. */
        node->header.next = NULL;
        if (previous != NULL) {
            previous->header.next = node;
        } else {
            head = node;
        }
        previous = node;
        guest = BOXEDWINE_GUEST_PTR(
            (const struct SM50_SHADER_COMPILATION_ARGUMENT_DATA*)guest->next);
    }
    return head;
}
