#!/usr/bin/env bash
# BoxedVN - build the instrumented x86-64 Direct3D 11 acceptance probe.
#
# Deliberately separate from scripts/build-guest-graphics-probe.sh, which
# builds the IA-32 Direct3D 9 probe with the distro's mingw. This one targets
# x86-64 with the pinned llvm-mingw toolchain, because that is the toolchain
# the DXMT PE DLLs beside it are built with.
#
# The probe it produces reports its progress through the Windows stderr handle
# as BOXEDVN_X64_CUBE_STAGE markers. The previous staged executable was an
# opaque prebuilt binary: two device runs showed it exiting with status 1400
# from one of three branches, each of which displays a message box before
# reading GetLastError, so the status could not name the branch. Building the
# probe from source in this repository is what makes the failing stage
# recoverable from a device log.
#
# What it renders is DXMT's own Direct3D 11 cube test (MIT, Copyright (c) 2023
# Feifan He), ported into that instrumentation -- the same demo the sibling
# iOS Wine/FEX/DXMT project runs on device, so a difference between the two
# projects is a difference in the stack rather than in the program. The source
# URLs and the pinned commit are in the probe's own header and in
# THIRD_PARTY_NOTICES.md.
#
# Usage:
#   scripts/build-x64-graphics-probe.sh --toolchain DIR --output PATH

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

TOOLCHAIN=""
OUTPUT=""
SOURCE="${BOXEDVN_SCRIPT_DIR}/guest-probes/x64-d3d11-cube.c"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --toolchain) [[ $# -ge 2 ]] || die "--toolchain needs a value"
                     TOOLCHAIN="$2"; shift 2 ;;
        --output)    [[ $# -ge 2 ]] || die "--output needs a value"
                     OUTPUT="$2"; shift 2 ;;
        --source)    [[ $# -ge 2 ]] || die "--source needs a value"
                     SOURCE="$2"; shift 2 ;;
        -h|--help)   sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
                     exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${TOOLCHAIN}" ]] || die "--toolchain is required."
[[ -n "${OUTPUT}" ]] || die "--output is required."
[[ -d "${TOOLCHAIN}" ]] || die "Toolchain directory '${TOOLCHAIN}' is missing."
require_file "${SOURCE}" "The x86-64 probe source is part of this repository."

CC="${TOOLCHAIN}/bin/x86_64-w64-mingw32-clang"
[[ -x "${CC}" ]] || die "The pinned llvm-mingw toolchain has no ${CC}."

mkdir -p "$(dirname "${OUTPUT}")"

# Console subsystem on purpose: a -mwindows image has no stderr handle, and
# the stage markers are the whole point of this program.
#
# -municode is NOT used: the entry point is main(), and the markers are ASCII.
"${CC}" \
    -std=c11 -O1 -g0 -Wall -Wextra -Werror \
    "${SOURCE}" \
    -o "${OUTPUT}" \
    -ld3d11 -ldxgi -ldxguid -lgdi32 -luser32 -lole32 \
    || die "Failed to build the x86-64 Direct3D 11 probe."

require_command file
file "${OUTPUT}" | grep -Eqi 'PE32\+.*x86-64' \
    || die "'${OUTPUT}' is not an x86-64 PE image."

# The markers have to survive into the shipped binary. A build that optimised
# them away, or a source that stopped emitting them, would leave a device log
# exactly as mute as the opaque binary this replaces.
for marker in \
    'BOXEDVN_X64_CUBE_STAGE ' \
    'register-class' \
    'create-window' \
    'd3d11-create' \
    'shaders' \
    'geometry' \
    'present'; do
    grep -qa -- "${marker}" "${OUTPUT}" \
        || die "The built probe does not contain the '${marker}' marker string."
done

ok "Built instrumented x86-64 Direct3D 11 probe: ${OUTPUT}"
