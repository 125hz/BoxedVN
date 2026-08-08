#!/usr/bin/env bash
# BoxedVN - increment the build number shown inside the app.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/bump-build.sh                 # CURRENT_PROJECT_VERSION += 1
#   scripts/bump-build.sh --set 12        # set it explicitly
#   scripts/bump-build.sh --marketing 0.2.0
#
# Run this before packaging an IPA that goes to a device.  The build number is
# what tells the user which IPA they are actually running - see AppVersion in
# ios/app/Sources/Runtime.swift, which surfaces it on the first screen.
#
# The number lives in the checked-in XcodeGen specs rather than being generated
# at build time (from a timestamp, say) on purpose: a build produced from a
# given commit must be byte-for-byte reproducible on the GitHub Actions runner,
# and a value that changes on its own would break that.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

SPECS=(
    "${BOXEDVN_ROOT}/ios/project.yml"
    "${BOXEDVN_ROOT}/ios/project-simulator.yml"
)

NEW_BUILD=""
NEW_MARKETING=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --set)       [[ $# -ge 2 ]] || die "--set needs a value"
                     NEW_BUILD="$2"; shift 2 ;;
        --marketing) [[ $# -ge 2 ]] || die "--marketing needs a value"
                     NEW_MARKETING="$2"; shift 2 ;;
        -h|--help)   sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
                     exit 0 ;;
        *)           die "Unknown argument '$1'. Run with --help." ;;
    esac
done

for spec in "${SPECS[@]}"; do
    require_file "${spec}" "Run this from a complete clone of the repository."
done

read_setting() {
    # The specs are small and hand-written; a targeted grep avoids a YAML
    # dependency in a bootstrap script.
    sed -n "s/^[[:space:]]*$2: *\"\{0,1\}\([^\"]*\)\"\{0,1\}[[:space:]]*$/\1/p" \
        "$1" | head -1
}

CURRENT_BUILD="$(read_setting "${SPECS[0]}" CURRENT_PROJECT_VERSION)"
[[ -n "${CURRENT_BUILD}" ]] \
    || die "CURRENT_PROJECT_VERSION not found in ${SPECS[0]}."

if [[ -z "${NEW_BUILD}" ]]; then
    [[ "${CURRENT_BUILD}" =~ ^[0-9]+$ ]] \
        || die "CURRENT_PROJECT_VERSION is '${CURRENT_BUILD}', which is not a
number, so it cannot be incremented automatically.  Pass --set explicitly."
    NEW_BUILD=$(( CURRENT_BUILD + 1 ))
fi

[[ "${NEW_BUILD}" =~ ^[0-9]+$ ]] \
    || die "The build number must be a whole number, got '${NEW_BUILD}'."

for spec in "${SPECS[@]}"; do
    # Both specs must agree: a simulator build reporting a different number
    # than the device build defeats the entire point of showing it.
    perl -pi -e "s/^(\s*CURRENT_PROJECT_VERSION: ).*\$/\${1}\"${NEW_BUILD}\"/" \
        "${spec}"
    if [[ -n "${NEW_MARKETING}" ]]; then
        perl -pi -e "s/^(\s*MARKETING_VERSION: ).*\$/\${1}\"${NEW_MARKETING}\"/" \
            "${spec}"
    fi
done

WROTE_BUILD="$(read_setting "${SPECS[0]}" CURRENT_PROJECT_VERSION)"
[[ "${WROTE_BUILD}" == "${NEW_BUILD}" ]] \
    || die "The rewrite did not take: ${SPECS[0]} still reads '${WROTE_BUILD}'."

MARKETING="$(read_setting "${SPECS[0]}" MARKETING_VERSION)"
ok "Version is now ${MARKETING} (${NEW_BUILD}), was ${MARKETING} (${CURRENT_BUILD})"
log "Commit the spec change, then run scripts/build-ios.sh so the app reports"
log "the matching git revision alongside it."
