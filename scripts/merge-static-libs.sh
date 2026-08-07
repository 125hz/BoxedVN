#!/usr/bin/env bash
# BoxedVN - merge several static libraries into one.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage: scripts/merge-static-libs.sh OUTPUT.a INPUT1.a [INPUT2.a ...]
#
# The XcodeGen application shell links one archive, so adding or splitting a
# CMake target never requires editing the project spec.  libtool is used rather
# than `ar` because it handles duplicate member names across archives, which
# happens routinely here (several vendored libraries contain a util.c).

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 OUTPUT.a INPUT1.a [INPUT2.a ...]" >&2
    exit 2
fi

output="$1"
shift

for input in "$@"; do
    if [[ ! -f "${input}" ]]; then
        echo "error: input archive '${input}' does not exist" >&2
        exit 1
    fi
done

mkdir -p "$(dirname "${output}")"
rm -f "${output}"

# -no_warning_for_no_symbols keeps the log clean: several Boxedwine translation
# units compile to nothing on iOS because of platform #ifdefs.
xcrun libtool -static -no_warning_for_no_symbols -o "${output}" "$@"

if [[ ! -f "${output}" ]]; then
    echo "error: libtool did not produce '${output}'" >&2
    exit 1
fi
