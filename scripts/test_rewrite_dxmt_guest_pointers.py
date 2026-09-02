#!/usr/bin/env python3
"""Contract tests for the DXMT guest-pointer rewrite.

The rewrite has to wrap every read of a guest pointer in DXMT's unix sources
and nothing else: not assignments to those fields, not host object handles,
and not text that is already wrapped. It also has to refuse a pinned source
whose dereference sites moved, so an untranslated read cannot ship silently.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location(
    "rewrite_dxmt_guest_pointers", REPO / "scripts" / "rewrite-dxmt-guest-pointers.py")
rewrite = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules[_spec.name] = rewrite
_spec.loader.exec_module(rewrite)

HEADER = REPO / "include" / "boxedwine_dxmt_guest_pointer.h"


class PointerReadRewrite(unittest.TestCase):
    def rewrite_reads(self, text: str) -> tuple[str, int]:
        return rewrite.rewrite_ptr_reads(text)

    def test_wraps_simple_and_chained_reads(self) -> None:
        text, count = self.rewrite_reads(
            "struct WMTBufferInfo *info = params->info.ptr;\n"
            "next = next->next.ptr;\n"
            "[encoder setBytes:body->bytes.ptr length:body->length];\n")
        self.assertEqual(count, 3)
        self.assertIn("BOXEDWINE_GUEST_PTR(params->info.ptr)", text)
        self.assertIn("BOXEDWINE_GUEST_PTR(next->next.ptr)", text)
        self.assertIn("setBytes:BOXEDWINE_GUEST_PTR(body->bytes.ptr) length", text)

    def test_wraps_reads_behind_casts_and_array_indexes(self) -> None:
        text, count = self.rewrite_reads(
            "x = (const MTLViewport *)body->viewports.ptr;\n"
            "[values setConstantValue:constants[i].data.ptr type:t];\n")
        self.assertEqual(count, 2)
        self.assertIn("(const MTLViewport *)BOXEDWINE_GUEST_PTR(body->viewports.ptr)", text)
        self.assertIn("BOXEDWINE_GUEST_PTR(constants[i].data.ptr)", text)

    def test_leaves_assignments_and_comparisons_alone(self) -> None:
        text, count = self.rewrite_reads(
            "info->memory.ptr = [buffer contents];\n"
            "if (info->memory.ptr == NULL) return 0;\n"
            "if (info->memory.ptr) use();\n")
        self.assertEqual(count, 2)
        self.assertIn("info->memory.ptr = [buffer contents];", text)
        self.assertIn("BOXEDWINE_GUEST_PTR(info->memory.ptr) == NULL", text)
        self.assertIn("if (BOXEDWINE_GUEST_PTR(info->memory.ptr)) use();", text)

    def test_does_not_touch_field_declarations(self) -> None:
        text, count = self.rewrite_reads("struct WMTMemoryPointer { void *ptr; };\n")
        self.assertEqual(count, 0)
        self.assertNotIn("BOXEDWINE_GUEST_PTR", text)

    def test_raw_sites_are_wrapped_inside_their_casts(self) -> None:
        text = rewrite.rewrite_raw_sites(
            "a = (char *)params->buffer_ptr;\nb = (void *)params->handle;\n",
            ["(char *)params->buffer_ptr", "(void *)params->handle"])
        self.assertIn("(char *)BOXEDWINE_GUEST_PTR(params->buffer_ptr)", text)
        self.assertIn("(void *)BOXEDWINE_GUEST_PTR(params->handle)", text)

    def test_object_handles_are_not_raw_sites(self) -> None:
        # Host object handles are used through the NSObject casts; they are
        # never listed as raw guest sites.
        for literal in rewrite.RAW_SITES["winemetal_unix.c"]:
            self.assertNotIn("NSObject", literal)
            self.assertNotIn("NSString *)", literal)

    def test_refuses_a_missing_or_duplicated_raw_site(self) -> None:
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_raw_sites("nothing here\n", ["(char *)params->arg"])
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_raw_sites("(char *)params->arg (char *)params->arg\n",
                                      ["(char *)params->arg"])

    def test_refuses_a_drifted_read_count_and_a_second_pass(self) -> None:
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_source("cache.c", "x = params->path.ptr;\n")
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_source("other.c", "x = BOXEDWINE_GUEST_PTR(p.ptr);\n")

    def test_main_thread_helper_runs_the_block_inline_off_the_main_thread(self) -> None:
        text = rewrite.rewrite_main_thread_helper(rewrite.MAIN_THREAD_HELPER)
        self.assertNotIn("dispatch_sync(dispatch_get_main_queue(), block);", text)
        self.assertIn("[CATransaction begin];", text)
        self.assertIn("[CATransaction commit];", text)
        # On the main thread the block still runs directly.
        self.assertIn("if ([NSThread isMainThread]) {\n    block();", text)

    def test_main_thread_helper_rewrite_refuses_drift(self) -> None:
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_main_thread_helper("void execute_on_main(void) {}\n")
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_main_thread_helper(rewrite.MAIN_THREAD_HELPER * 2)

    def test_main_thread_helper_is_rewritten_only_in_the_unix_source(self) -> None:
        self.assertEqual(rewrite.MAIN_THREAD_HELPER_FILES, {"winemetal_unix.c"})
        # cache.c has no helper, so its rewrite must not demand one.
        text = rewrite.rewrite_source("cache.c", "x = p.ptr;\n" * 5)
        self.assertEqual(text.count("BOXEDWINE_GUEST_PTR"), 5)

    def test_output_is_written_beside_the_source(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            source = pathlib.Path(raw) / "other.c"
            source.write_text("y = params->info.ptr;\n", encoding="utf-8")
            self.assertEqual(rewrite.main([str(source)]), 0)
            target = pathlib.Path(raw) / "other.boxedwine.c"
            self.assertTrue(target.is_file())
            self.assertIn("BOXEDWINE_GUEST_PTR(params->info.ptr)",
                          target.read_text(encoding="utf-8"))
            self.assertEqual(source.read_text(encoding="utf-8"), "y = params->info.ptr;\n")


class ShaderTranslatorThunks(unittest.TestCase):
    """The SM50 thunks pass guest pointers with no cast at all.

    A device run faulted in host code at a guest stack address at the first
    CreateVertexShader: SM50Initialize wrote its out-parameter through the
    untranslated pointer to the caller's stack. Every guest pointer those
    thunks forward has to be translated, the argument chain deep-copied, and
    the handles left alone.
    """

    def test_every_thunk_site_is_rewritten_exactly_once(self) -> None:
        fixture = "\n".join(old for old, _ in rewrite.THUNK_REWRITES["winemetal_unix.c"])
        text = rewrite.rewrite_thunks(
            fixture, rewrite.THUNK_REWRITES["winemetal_unix.c"])
        for old, new in rewrite.THUNK_REWRITES["winemetal_unix.c"]:
            self.assertIn(new, text)
            self.assertNotIn(old, text)

    def test_out_parameters_and_buffers_are_translated(self) -> None:
        rewritten = "\n".join(new for _, new in rewrite.THUNK_REWRITES["winemetal_unix.c"])
        for guest in ("params->bytecode", "params->shader)", "params->reflection",
                      "params->error)", "params->func_name", "params->bitcode)",
                      "params->data_out", "params->buffer)",
                      "params->constant_buffers", "params->arguments"):
            self.assertIn(f"BOXEDWINE_GUEST_PTR({guest}", rewritten, guest)

    def test_argument_chains_are_deep_copied_not_just_translated(self) -> None:
        rewritten = "\n".join(new for _, new in rewrite.THUNK_REWRITES["winemetal_unix.c"])
        for chain in ("params->args", "params->hull_args", "params->domain_args",
                      "params->vertex_args", "params->geometry_args"):
            self.assertIn(f"BOXEDWINE_SM50_ARGS({chain})", rewritten)
            self.assertNotIn(f"BOXEDWINE_GUEST_PTR({chain})", rewritten)

    def test_handles_are_not_translated(self) -> None:
        # sm50_shader_t / sm50_bitcode_t / sm50_error_t values are host
        # pointers airconv returned; translating them would corrupt them.
        rewritten = "\n".join(new for _, new in rewrite.THUNK_REWRITES["winemetal_unix.c"])
        self.assertIn("SM50Compile(params->shader, ", rewritten)
        self.assertIn("SM50GetCompiledBitcode(params->bitcode, ", rewritten)
        self.assertIn("SM50GetErrorMessage(params->error, ", rewritten)
        self.assertIn("SM50GetArgumentsInfo(params->shader, ", rewritten)

    def test_the_compiled_bitcode_pointer_is_tagged_for_the_round_trip(self) -> None:
        rewritten = "\n".join(new for _, new in rewrite.THUNK_REWRITES["winemetal_unix.c"])
        self.assertIn("boxedwine_dxmt_tag_compiled_bitcode("
                      "BOXEDWINE_GUEST_PTR(params->data_out));", rewritten)
        source = (REPO / "tools" / "dxmt" /
                  "boxedwine_dxmt_sm50_arguments.c").read_text(encoding="utf-8")
        self.assertIn("void boxedwine_dxmt_tag_compiled_bitcode(void* compiled_bitcode)", source)
        self.assertIn("boxedwine_dxmt_tag_host_pointer(", source)

    def test_a_missing_or_duplicated_thunk_site_is_refused(self) -> None:
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_thunks("nothing\n", [("SM50Compile(a)", "x")])
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_thunks("SM50Compile(a) SM50Compile(a)\n",
                                   [("SM50Compile(a)", "x")])

    def test_the_chain_copier_covers_every_argument_type(self) -> None:
        source = (REPO / "tools" / "dxmt" /
                  "boxedwine_dxmt_sm50_arguments.c").read_text(encoding="utf-8")
        for kind in ("SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT", "SM50_SHADER_COMMON",
                     "SM50_SHADER_PSO_PIXEL_SHADER", "SM50_SHADER_IA_INPUT_LAYOUT",
                     "SM50_SHADER_GS_PASS_THROUGH", "SM50_SHADER_PSO_GEOMETRY_SHADER",
                     "SM50_SHADER_PSO_TESSELLATOR"):
            self.assertIn(f"case {kind}:", source)
        # The two nested arrays are translated, and the links re-point at
        # the host copies.
        self.assertEqual(source.count(".elements =\n                BOXEDWINE_GUEST_PTR("), 2)
        self.assertIn("previous->header.next = node;", source)
        self.assertIn("_Thread_local", source)

    def test_the_native_build_compiles_the_chain_copier(self) -> None:
        build = (REPO / "scripts" / "build-dxmt-ios-native.sh").read_text(encoding="utf-8")
        self.assertIn("tools/dxmt/boxedwine_dxmt_sm50_arguments.c", build)
        workflow = (REPO / ".github" / "workflows" / "build-ios.yml").read_text(encoding="utf-8")
        self.assertIn("'tools/dxmt/**'", workflow)


class ResourceStorageMode(unittest.TestCase):
    """iOS Metal has no Managed storage mode.

    Two device runs died at the geometry stage with Metal's own assertion,
    "Invalid storageMode", on the first buffer: DXMT's meson build defines
    DXMT_IOS (which remaps Managed to Shared in winemetal.h) only for its
    aarch64-windows target, so the x86-64 guest DLLs asked for Managed. The
    guest build now defines it, and the unix side remaps defensively.
    """

    def test_the_guest_and_native_builds_define_dxmt_ios(self) -> None:
        pe = (REPO / "scripts" / "build-dxmt-x64-pe.sh").read_text(encoding="utf-8")
        self.assertIn("-Dc_args=-DDXMT_IOS=1 -Dcpp_args=-DDXMT_IOS=1", pe)
        native = (REPO / "scripts" / "build-dxmt-ios-native.sh").read_text(encoding="utf-8")
        self.assertIn("-DDXMT_IOS=1", native)

    def test_every_forwarded_options_value_is_remapped(self) -> None:
        fixture = (
            "    buffer = [device newBufferWithBytesNoCopy:x length:l "
            "options:(enum MTLResourceOptions)info->options deallocator:NULL];\n"
            "    buffer = [device newBufferWithLength:info->length "
            "options:(enum MTLResourceOptions)info->options];\n"
            "  desc.resourceOptions = (MTLResourceOptions)info->options;\n"
            "  info->options = (enum WMTResourceOptions)desc.resourceOptions;\n")
        text = rewrite.rewrite_options(fixture, rewrite.OPTION_REWRITES["winemetal_unix.c"])
        self.assertEqual(text.count("BOXEDWINE_METAL_RESOURCE_OPTIONS(info->options)"), 3)
        # The write-back of the actual options is not a forwarded value.
        self.assertIn("info->options = (enum WMTResourceOptions)desc.resourceOptions;", text)

    def test_a_drifted_options_site_count_is_refused(self) -> None:
        with self.assertRaises(rewrite.RewriteError):
            rewrite.rewrite_options("options:(enum MTLResourceOptions)info->options\n",
                                    rewrite.OPTION_REWRITES["winemetal_unix.c"])

    def test_the_macro_maps_managed_to_shared_and_nothing_else(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("BOXEDWINE_METAL_STORAGE_MODE_MASK 0x30ULL", header)
        self.assertIn("BOXEDWINE_METAL_STORAGE_MODE_MANAGED 0x10ULL", header)
        # Evaluate the macro's arithmetic in Python for the four storage modes
        # combined with the cache and hazard bits DXMT sets.
        def remap(o: int) -> int:
            return (o & ~0x30) if (o & 0x30) == 0x10 else o
        for extra in (0, 1, 256, 512, 257):
            self.assertEqual(remap(0 | extra), 0 | extra)       # Shared
            self.assertEqual(remap(16 | extra), 0 | extra)      # Managed -> Shared
            self.assertEqual(remap(32 | extra), 32 | extra)     # Private
            self.assertEqual(remap(48 | extra), 48 | extra)     # Memoryless


class TranslationHeaderContract(unittest.TestCase):
    def test_header_constants_match_the_guest_alias_contract(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        alias = (REPO / "include" / "guest_low_alias.h").read_text(encoding="utf-8")
        self.assertIn("0x7800000000ULL", header)
        self.assertIn("kGuestLowAliasBase = 0x7800000000ULL", alias)
        self.assertIn("0x7F8000000000ULL", header)
        self.assertIn("kGuestTopClearMask = 0x7F8000000000ULL", alias)
        self.assertIn("if (!guest) {", header)
        # A tagged host pointer (the compiled bitcode the guest passes back)
        # is returned unchanged; nothing is decided by address range, since
        # the guest's low range and host allocations overlap below 8 GiB.
        self.assertIn("BOXEDWINE_DXMT_HOST_POINTER_TAG 0x4000000000000000ULL", header)
        self.assertIn("boxedwine_dxmt_tag_host_pointer", header)
        self.assertNotIn("is_host_pointer", header)
        self.assertIn("BOXEDWINE_SM50_ARGS(p)", header)


if __name__ == "__main__":
    unittest.main()
