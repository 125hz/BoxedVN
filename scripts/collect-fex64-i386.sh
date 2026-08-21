#!/usr/bin/env bash
# BoxedVN - collect the i386 PE side out of the built Wine tree.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/collect-fex64-i386.sh [--third-party-dir DIR] [--into DIR]
#                                 [--wow64-into DIR]
#
# Why this exists
# ---------------
# The ARM64EC and aarch64 PE sets that reach the app come from the pinned
# integration's prebuilt runtime, not from the Wine tree this project builds.
# There is no prebuilt i386 set, because the integration never ran a 32-bit
# guest -- so the only source for one is the tree built here, and it needs
# gathering out of Wine's per-DLL build layout before it can be staged.
#
# Wine puts each module's PE output under dlls/<name>/<arch>-windows/, so the
# 32-bit side is spread across a few hundred directories rather than sitting
# in one. Flatten it into the shape the app bundle expects, which is the same
# shape the other two architectures already arrive in: one directory of PE
# files, linked into the prefix at start -- into syswow64 for this one, because
# an i386 ntdll.dll and an ARM64EC ntdll.dll have the same name and are not
# interchangeable.
#
# Writes into the runtime directory the app build is pointed at, so
# build-fex64-app.sh picks it up with no extra argument.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

INTO=""
WOW64_INTO=""

usage() {
    sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --into)
            [[ $# -ge 2 ]] || die "--into needs a value"
            INTO="$2"; shift 2 ;;
        --wow64-into)
            [[ $# -ge 2 ]] || die "--wow64-into needs a value"
            WOW64_INTO="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
WINE_BUILD="${FEX64_DIR}/wine/build-arm64ec"
[[ -n "${INTO}" ]] || INTO="${FEX64_DIR}/mythic/app/Mythic/i386-windows"
[[ -n "${WOW64_INTO}" ]] || WOW64_INTO="${FEX64_DIR}/mythic/app/Mythic/arm64ec-windows"

[[ -d "${WINE_BUILD}/dlls" ]] || die \
"No Wine PE build at '${WINE_BUILD}'. Run scripts/build-fex64-wine.sh --tree arm64ec first."

# Not fatal, and deliberately so. The x86-64 stack does not depend on any of
# this, and a tree whose i386 arch failed to build should still produce an
# IPA -- one that runs 64-bit programs and says why it cannot run 32-bit ones.
# build-fex64-wine.sh already warns when the arch produced nothing; repeating
# the warning here and exiting 0 keeps a CI job from going red over a feature
# that is additive.
count="$(find "${WINE_BUILD}/dlls" -path '*i386-windows*' \
    \( -name '*.dll' -o -name '*.exe' \) 2>/dev/null | wc -l | tr -d ' ')"
if [[ "${count}" -eq 0 ]]; then
    warn "The Wine tree at '${WINE_BUILD}' produced no i386 PE files.
32-bit guests will not start in this build; the x86-64 side is unaffected.
Check that configure was given --enable-archs=...,i386 and that llvm-mingw
has an i686 target."
    exit 0
fi

rm -rf "${INTO}"
mkdir -p "${INTO}"

# -exec cp rather than a loop: the set is a few hundred files and every one of
# them is a distinct basename, because Wine names each PE after its module.
find "${WINE_BUILD}/dlls" -path '*i386-windows*' \
    \( -name '*.dll' -o -name '*.exe' \) -exec cp {} "${INTO}/" \;

# ntdll is the one that cannot be missing: it is what a 32-bit process maps
# before anything else, and without it the guest fails inside the loader with
# nothing to read. The rest of the set degrades one import at a time and says
# which; this one does not.
require_file "${INTO}/ntdll.dll" \
    "The i386 set was collected but has no ntdll.dll, which is the module a
32-bit process cannot start without. Check ${WINE_BUILD}/build.log for its
failure specifically rather than treating the whole arch as broken."


# The 64-bit half of WoW64, which is not part of the i386 set.
#
# wow64.dll and wow64win.dll run in the 64-bit process and are what a 32-bit
# guest's calls are thunked *into*; the CPU backend only executes its
# instructions. The pinned integration ships neither, because it never ran a
# 32-bit guest -- of its 107 ARM64EC modules the only two that matter here are
# ntdll and xtajit64. So these come from the tree built here as well, and land
# in the 64-bit set beside them rather than in syswow64.
#
# Preferred from the ARM64EC output because that is the set that becomes
# system32 in the prefix; the aarch64 build is taken only if ARM64EC has none.
collect_wow64_thunk() {
    local name="$1"
    local found=""
    local arch
    # An explicit if rather than `test && break`: this script runs under
    # set -e, where a trailing failed test at the end of a loop body is the
    # last command executed and takes the whole script down with it.
    for arch in arm64ec aarch64; do
        found="$(find "${WINE_BUILD}/dlls" -path "*${arch}-windows*" \
            -name "${name}" -print -quit 2>/dev/null || true)"
        if [[ -n "${found}" ]]; then
            break
        fi
    done
    if [[ -z "${found}" ]]; then
        warn "The Wine tree produced no ${name}. A 32-bit guest cannot start
without it; the x86-64 side is unaffected."
        return 1
    fi
    mkdir -p "${WOW64_INTO}"
    cp "${found}" "${WOW64_INTO}/${name}"
    log "Collected ${name} from ${found#${WINE_BUILD}/}"
    return 0
}

collect_wow64_thunk wow64.dll || true
collect_wow64_thunk wow64win.dll || true

staged="$(find "${INTO}" -type f | wc -l | tr -d ' ')"
ok "i386 PE side: ${staged} files -> ${INTO}"
