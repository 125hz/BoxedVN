#!/usr/bin/env python3
"""Validate the host words FEX emitted for one targeted guest instruction.

The translator's targeted IR capture prints the machine words it emitted for a
single guest instruction at compile time. This reads that transcript and fails
when the capture did not happen at all, when a known-invalid encoding appears,
or when a required instruction form is missing. It is the CI counterpart to the
device markers: the same evidence, checked automatically.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# "E [ircap]   +0x0004  3c9f0177"
WORD = re.compile(r"ircap[]][ ]+[+]0x[0-9a-f]+[ ]+([0-9a-f]{8})")

# ARM64 add/sub of a shifted register, either width, flag-setting or not.
ADDSUB_SHIFTED_REGISTER_MASK = 0x7F200000
ADDSUB_SHIFTED_REGISTER_FORMS = {
    0x0B000000,  # add
    0x2B000000,  # adds
    0x4B000000,  # sub
    0x6B000000,  # subs
}


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("transcript", type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument(
        "--forbid-word",
        action="append",
        default=[],
        help="a host word that must not appear, as eight hex digits",
    )
    parser.add_argument(
        "--require-addsub-register",
        action="store_true",
        help="require at least one add/sub of a shifted register",
    )
    arguments = parser.parse_args()

    text = arguments.transcript.read_text(encoding="utf-8", errors="replace")

    if "[ircap]" not in text:
        fail(
            f"{arguments.label}: targeted IR capture produced no output; the "
            "downstream arming patch is not compiled into this translator"
        )
    if "ml623 HOST bytes" not in text:
        fail(
            f"{arguments.label}: IR capture never reached the host-word stage "
            "for the pinned guest target"
        )

    words = WORD.findall(text)
    if not words:
        fail(f"{arguments.label}: IR capture printed no emitted host words")

    for forbidden in arguments.forbid_word:
        if forbidden in words:
            fail(
                f"{arguments.label}: the translator emitted the known-invalid "
                f"word 0x{forbidden} for the pinned guest instruction"
            )

    # An immediate shifted past its field leaves the opcode bits all set, which
    # is unallocated on AArch64. Reject the whole shape, not just one value.
    for word in words:
        if (int(word, 16) >> 21) == 0x7FF:
            fail(
                f"{arguments.label}: emitted word 0x{word} has an unallocated "
                "opcode; an oversized immediate reached the encoding field"
            )

    if arguments.require_addsub_register:
        matched = [
            word
            for word in words
            if (int(word, 16) & ADDSUB_SHIFTED_REGISTER_MASK)
            in ADDSUB_SHIFTED_REGISTER_FORMS
        ]
        if not matched:
            fail(
                f"{arguments.label}: no add/sub of a shifted register among "
                f"{' '.join(words)}; an unencodable constant must be "
                "materialized into a register, not folded into an immediate"
            )
        print(
            f"[fex-vixl] {arguments.label}: register-form add/sub "
            f"{' '.join(matched)}"
        )

    print(f"[fex-vixl] {arguments.label}: host words {' '.join(words)}")


if __name__ == "__main__":
    main()
