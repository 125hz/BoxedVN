"""Compile the shared runtime's protocol assumptions against the pinned Wine source headers."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import hashlib
import re
import tarfile
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--headers", type=Path)
parser.add_argument("--fetch-pinned-headers", action="store_true")
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
if not args.headers and not args.fetch_pinned_headers:
    parser.error("use --headers or --fetch-pinned-headers")
with tempfile.TemporaryDirectory() as temporary:
    if args.fetch_pinned_headers:
        # Internal server headers are deliberately omitted from Wine's SDK.
        # Read the same immutable pin used to compile the cached runtime.
        builder = (repo / "scripts/build-wine11-runtime.sh").read_text()
        version = re.search(r"^version=([0-9.]+)$", builder, re.M).group(1)
        expected = re.search(r"^source_sha=([0-9a-f]{64})$", builder, re.M).group(1)
        archive = Path(temporary) / "wine.tar.xz"
        url = f"https://dl.winehq.org/wine/source/{version}/wine-{version}.tar.xz"
        urllib.request.urlretrieve(url, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            raise RuntimeError("Wine source checksum mismatch")
        with tarfile.open(archive) as source:
            members = [m for m in source.getmembers()
                       if m.name.startswith(f"wine-{version}/include/")]
            source.extractall(temporary, members=members, filter="data")
        headers = Path(temporary) / f"wine-{version}/include"
    else:
        headers = args.headers.resolve()
    check = Path(temporary) / "protocol.c"
    constants = "\n".join(line for line in (repo / "include/wine_server_reply.h").read_text().splitlines()
                          if line.startswith("#define K_WINE_"))
    check.write_text(constants + r"""
#include <stddef.h>
#include <wine/server_protocol.h>
#define VERIFY(condition) _Static_assert(condition, #condition)
VERIFY(REQ_init_process_done == K_WINE_REQ_INIT_PROCESS_DONE);
VERIFY(REQ_terminate_thread == K_WINE_REQ_TERMINATE_THREAD);
VERIFY(sizeof(union generic_request) == K_WINE_SERVER_MESSAGE_BYTES);
VERIFY(sizeof(union generic_reply) == K_WINE_SERVER_MESSAGE_BYTES);
VERIFY(offsetof(struct init_process_done_reply, suspend) == 8);
VERIFY(sizeof(struct init_process_done_reply) == 16);
VERIFY(offsetof(struct terminate_thread_request, handle) == 12);
VERIFY(offsetof(struct terminate_thread_request, exit_code) == 16);
""")
    subprocess.run(["gcc", "-std=c11", "-D__WINESRC__", "-fsyntax-only",
                    "-I" + str(headers), "-I" + str(headers / "wine/windows"),
                    "-I" + str(repo / "include"), str(check)], check=True)
print("Wine server message sizes, opcodes and field offsets match the shared runtime")
