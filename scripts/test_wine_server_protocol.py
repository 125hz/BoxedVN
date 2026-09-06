"""Compile the shared runtime's protocol assumptions against the staged Wine SDK."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--headers", type=Path, required=True)
args = parser.parse_args()
headers = args.headers.resolve()
repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as temporary:
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
