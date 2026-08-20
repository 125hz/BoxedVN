#!/usr/bin/env bash
# BoxedVN - validate and stage a developer-supplied BoxedWine64 runtime.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# This is intentionally a preparation step, not a downloader. Wine/Debian
# redistribution has not been approved; the caller must provide the output of
# the audited BoxedWine64 rootfs builder and a checksum manifest.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INPUT=""
OUTPUT_DIR=""
MANIFEST=""
ALLOW_UNPINNED=0

usage() {
    cat <<'EOF'
Usage: scripts/prepare-wine64-runtime.sh --input DIR|ARCHIVE.zip \
    --output-dir DIR [--manifest FILE] [--allow-unpinned]

Validates the two layered archives emitted by the audited BoxedWine64 rootfs
builder and stages them with wine64-runtime.manifest. No network download is
performed. --allow-unpinned is for local inspection only and is refused in CI.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --input)          [[ $# -ge 2 ]] || { echo "--input needs a value" >&2; exit 2; }
                          INPUT="$2"; shift 2 ;;
        --output-dir)     [[ $# -ge 2 ]] || { echo "--output-dir needs a value" >&2; exit 2; }
                          OUTPUT_DIR="$2"; shift 2 ;;
        --manifest)       [[ $# -ge 2 ]] || { echo "--manifest needs a value" >&2; exit 2; }
                          MANIFEST="$2"; shift 2 ;;
        --allow-unpinned) ALLOW_UNPINNED=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *)                 echo "Unknown argument '$1'. Run with --help." >&2; exit 2 ;;
    esac
done

[[ -n "${INPUT}" ]] || { usage >&2; exit 2; }
[[ -n "${OUTPUT_DIR}" ]] || { echo "--output-dir is required." >&2; exit 2; }

args=(--input "${INPUT}" --output-dir "${OUTPUT_DIR}")
[[ -n "${MANIFEST}" ]] && args+=(--manifest "${MANIFEST}")
[[ ${ALLOW_UNPINNED} -eq 1 ]] && args+=(--allow-unpinned)
exec "${SCRIPT_DIR}/validate-wine64-runtime.sh" "${args[@]}"
