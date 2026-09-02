#!/usr/bin/env python3
"""Rewrite DXMT's unix sources so every guest pointer they read is translated.

DXMT's winemetal unix side reads guest memory through the `.ptr` field of
WMTMemoryPointer/WMTConstMemoryPointer and through a few raw uint64_t fields
it casts itself. Under BoxedWine the translated guest is dereferenced through
one address rule (include/boxedwine_dxmt_guest_pointer.h): the identity for
the high lane, an alias for the canonical low range, and a relocation for
Wine's top-down arena, where every unix-call parameter block and every
stack-resident descriptor lives. Reading those pointers untranslated is what
a device run reported as EFAULT on every DXMT call.

The rewrite wraps each *read* of such a pointer in BOXEDWINE_GUEST_PTR(...):

  * `<expr>.ptr` that is not the target of an assignment;
  * the raw casts DXMT applies to `params->buffer_ptr`, `params->arg` and
    `params->handle` where the field is a guest address rather than a host
    object handle (named explicitly, each expected exactly once);
  * the shader translator thunks (thunk_SM50*), whose parameter blocks carry
    guest pointers with no cast at all: the DXBC, out-parameters on the
    caller's stack, the error buffer, and the compilation-argument chain,
    which is deep-copied by BOXEDWINE_SM50_ARGS because airconv walks its
    `next` links and `elements` arrays natively. Handles (sm50_shader_t,
    sm50_bitcode_t, sm50_error_t) are host pointers and are left alone. A
    device run faulted in host code at a guest stack address at the first
    CreateVertexShader for exactly this reason.

Object handles (the `(NSObject *)params->handle` family) are host pointers
and are left alone. The rewritten copy is written beside the original so its
relative includes still resolve, and the original is never modified. Every
expected site must be found exactly once; a pinned-source change that adds
or removes one fails the build rather than silently shipping an untranslated
read.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

MACRO = "BOXEDWINE_GUEST_PTR"

# `a->b.c.ptr` style chains, not preceded by the macro itself and not the
# left-hand side of a plain assignment (`==` is a comparison and is allowed).
PTR_READ = re.compile(
    r"(?<![\w.>])"
    r"(?P<expr>[A-Za-z_]\w*(?:\[[^\]]*\])?(?:(?:->|\.)[A-Za-z_]\w*(?:\[[^\]]*\])?)*)"
    r"\.ptr\b(?!\s*=(?!=))"
)

# Raw guest addresses DXMT casts itself. Each entry is (file, literal) and
# must appear exactly once in that file.
RAW_SITES = {
    "winemetal_unix.c": [
        "(char *)params->buffer_ptr",
        "(char *)params->arg",
        "(struct WMTRenderPassInfo *)params->arg",
        "(void *)params->handle",
    ],
    "cache.c": [],
}

# Shader translator thunks: exact call text -> the same call with its guest
# pointers translated. Each `old` must occur exactly once in the file. The
# multi-line calls are matched on their argument line alone.
THUNK_REWRITES = {
    "winemetal_unix.c": [
        ("SM50Initialize(params->bytecode, params->bytecode_size, "
         "params->shader, params->reflection, params->error)",
         "SM50Initialize(BOXEDWINE_GUEST_PTR(params->bytecode), "
         "params->bytecode_size, BOXEDWINE_GUEST_PTR(params->shader), "
         "BOXEDWINE_GUEST_PTR(params->reflection), "
         "BOXEDWINE_GUEST_PTR(params->error))"),
        ("SM50Compile(params->shader, params->args, params->func_name, "
         "params->bitcode, params->error)",
         "SM50Compile(params->shader, BOXEDWINE_SM50_ARGS(params->args), "
         "BOXEDWINE_GUEST_PTR(params->func_name), "
         "BOXEDWINE_GUEST_PTR(params->bitcode), "
         "BOXEDWINE_GUEST_PTR(params->error))"),
        ("SM50GetCompiledBitcode(params->bitcode, params->data_out)",
         "SM50GetCompiledBitcode(params->bitcode, "
         "BOXEDWINE_GUEST_PTR(params->data_out))"),
        ("SM50GetErrorMessage(params->error, params->buffer, "
         "params->buffer_size)",
         "SM50GetErrorMessage(params->error, "
         "BOXEDWINE_GUEST_PTR(params->buffer), params->buffer_size)"),
        ("      params->vertex, params->hull, params->hull_args, "
         "params->func_name, params->bitcode, params->error",
         "      params->vertex, params->hull, "
         "BOXEDWINE_SM50_ARGS(params->hull_args), "
         "BOXEDWINE_GUEST_PTR(params->func_name), "
         "BOXEDWINE_GUEST_PTR(params->bitcode), "
         "BOXEDWINE_GUEST_PTR(params->error)"),
        ("      params->hull, params->domain, params->domain_args, "
         "params->func_name, params->bitcode, params->error",
         "      params->hull, params->domain, "
         "BOXEDWINE_SM50_ARGS(params->domain_args), "
         "BOXEDWINE_GUEST_PTR(params->func_name), "
         "BOXEDWINE_GUEST_PTR(params->bitcode), "
         "BOXEDWINE_GUEST_PTR(params->error)"),
        ("      params->vertex, params->geometry, params->vertex_args, "
         "params->func_name, params->bitcode, params->error",
         "      params->vertex, params->geometry, "
         "BOXEDWINE_SM50_ARGS(params->vertex_args), "
         "BOXEDWINE_GUEST_PTR(params->func_name), "
         "BOXEDWINE_GUEST_PTR(params->bitcode), "
         "BOXEDWINE_GUEST_PTR(params->error)"),
        ("      params->vertex, params->geometry, params->geometry_args, "
         "params->func_name, params->bitcode, params->error",
         "      params->vertex, params->geometry, "
         "BOXEDWINE_SM50_ARGS(params->geometry_args), "
         "BOXEDWINE_GUEST_PTR(params->func_name), "
         "BOXEDWINE_GUEST_PTR(params->bitcode), "
         "BOXEDWINE_GUEST_PTR(params->error)"),
        ("SM50GetArgumentsInfo(params->shader, params->constant_buffers, "
         "params->arguments)",
         "SM50GetArgumentsInfo(params->shader, "
         "BOXEDWINE_GUEST_PTR(params->constant_buffers), "
         "BOXEDWINE_GUEST_PTR(params->arguments))"),
    ],
    "cache.c": [],
}

# `.ptr` reads expected per file, so a drift in the pinned source is noticed.
EXPECTED_PTR_READS = {
    "winemetal_unix.c": 50,
    "cache.c": 5,
}


# DXMT hands CAMetalLayer property changes to the main thread with
# dispatch_sync. Under BoxedWine the main thread is owned by the emulator's
# SDL loop for the whole session, and a device run stopped at the first
# layer update of the present stage: the call never returned and the guest
# thread never resumed. Layer property changes are legal from any thread
# inside a CoreAnimation transaction, so the helper applies the block
# inline when it is not already on the main thread. Expected exactly once.
MAIN_THREAD_HELPER = (
    "void\n"
    "execute_on_main(dispatch_block_t block) {\n"
    "  if ([NSThread isMainThread]) {\n"
    "    block();\n"
    "  } else {\n"
    "    dispatch_sync(dispatch_get_main_queue(), block);\n"
    "  }\n"
    "}\n"
)
MAIN_THREAD_HELPER_INLINE = (
    "void\n"
    "execute_on_main(dispatch_block_t block) {\n"
    "  /* BoxedWine: the main thread runs the emulator loop for the whole\n"
    "   * session, so waiting on it here never returns. Layer properties may\n"
    "   * be set from any thread inside a transaction. */\n"
    "  if ([NSThread isMainThread]) {\n"
    "    block();\n"
    "  } else {\n"
    "    [CATransaction begin];\n"
    "    [CATransaction setDisableActions:YES];\n"
    "    block();\n"
    "    [CATransaction commit];\n"
    "  }\n"
    "}\n"
)
MAIN_THREAD_HELPER_FILES = {"winemetal_unix.c"}


class RewriteError(RuntimeError):
    pass


def rewrite_main_thread_helper(text: str) -> str:
    occurrences = text.count(MAIN_THREAD_HELPER)
    if occurrences != 1:
        raise RewriteError(
            f"expected exactly one execute_on_main helper, found {occurrences}; "
            "the pinned DXMT source changed, re-audit the main-thread dispatch")
    return text.replace(MAIN_THREAD_HELPER, MAIN_THREAD_HELPER_INLINE, 1)


def wrap(expression: str) -> str:
    return f"{MACRO}({expression})"


def rewrite_ptr_reads(text: str) -> tuple[str, int]:
    count = 0

    def replace(match: re.Match) -> str:
        nonlocal count
        count += 1
        return wrap(match.group("expr") + ".ptr")

    return PTR_READ.sub(replace, text), count


def rewrite_raw_sites(text: str, sites: list[str]) -> str:
    for literal in sites:
        occurrences = text.count(literal)
        if occurrences != 1:
            raise RewriteError(
                f"expected exactly one occurrence of {literal!r}, found {occurrences}")
        cast, _, field = literal.rpartition(")")
        text = text.replace(literal, f"{cast}){wrap(field)}", 1)
    return text


def rewrite_thunks(text: str, rewrites: list[tuple[str, str]]) -> str:
    for old, new in rewrites:
        occurrences = text.count(old)
        if occurrences != 1:
            raise RewriteError(
                f"expected exactly one occurrence of {old!r}, found {occurrences}; "
                "the pinned DXMT source changed, re-audit the SM50 thunks")
        text = text.replace(old, new, 1)
    return text


def rewrite_source(name: str, text: str) -> str:
    if MACRO in text:
        raise RewriteError(f"{name}: already rewritten")
    rewritten, count = rewrite_ptr_reads(text)
    expected = EXPECTED_PTR_READS.get(name)
    if expected is not None and count != expected:
        raise RewriteError(
            f"{name}: rewrote {count} .ptr reads, expected {expected}; "
            "the pinned DXMT source changed, re-audit the dereference sites")
    rewritten = rewrite_raw_sites(rewritten, RAW_SITES.get(name, []))
    rewritten = rewrite_thunks(rewritten, THUNK_REWRITES.get(name, []))
    if name in MAIN_THREAD_HELPER_FILES:
        rewritten = rewrite_main_thread_helper(rewritten)
    return rewritten


def output_name(source: pathlib.Path) -> pathlib.Path:
    return source.with_name(source.stem + ".boxedwine" + source.suffix)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("sources", nargs="+", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        for source in args.sources:
            text = source.read_text(encoding="utf-8")
            rewritten = rewrite_source(source.name, text)
            target = output_name(source)
            target.write_text(rewritten, encoding="utf-8", newline="\n")
            print(f"rewrote {source} -> {target}")
    except RewriteError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
