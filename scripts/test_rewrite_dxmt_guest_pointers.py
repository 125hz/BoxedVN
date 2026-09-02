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


class TranslationHeaderContract(unittest.TestCase):
    def test_header_constants_match_the_guest_alias_contract(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        alias = (REPO / "include" / "guest_low_alias.h").read_text(encoding="utf-8")
        self.assertIn("0x7800000000ULL", header)
        self.assertIn("kGuestLowAliasBase = 0x7800000000ULL", alias)
        self.assertIn("0x7F8000000000ULL", header)
        self.assertIn("kGuestTopClearMask = 0x7F8000000000ULL", alias)
        self.assertIn("if (!guest) {", header)


if __name__ == "__main__":
    unittest.main()
