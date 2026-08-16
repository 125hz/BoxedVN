#!/usr/bin/env bash
# BoxedVN - fetch the pinned sources for the fex64 native-ARM64 stack.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/fetch-fex64-dependencies.sh [--component fex|wine|dxmt|all]
#                                       [--third-party-dir DIR]
#                                       [--force]
#
# This fetches sources only.  It does not build anything: none of the three
# has a working iphoneos build yet, and that is stage M0 in
# docs/ARCHITECTURE_FEX64.md.  Fetching is separated from building so the
# pins can be exercised, and their resolved commits recorded, before any
# toolchain work exists.
#
# Runs anywhere git runs, including on a PC.  Building will not.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.fex64.lock.sh"

COMPONENTS="all"
FORCE=0

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --component)
            [[ $# -ge 2 ]] || die "--component needs a value"
            COMPONENTS="$2"; shift 2 ;;
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --force)
            FORCE=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

case "${COMPONENTS}" in
    all) COMPONENTS="fex wine dxmt" ;;
    fex|wine|dxmt) ;;
    *) die "--component must be fex, wine, dxmt or all, got '${COMPONENTS}'." ;;
esac

require_command git
require_command awk

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
REVISION_FILE="${FEX64_DIR}/PINNED_REVISIONS.txt"

# clone_pinned <name> <repository> <tag> <submodules:yes|no>
#
# Clones at exactly the pinned tag and leaves the checkout detached, so a
# later 'git pull' in the third-party tree cannot silently move a dependency.
# An existing checkout is verified against the pin rather than trusted.
clone_pinned() {
    local name="$1"
    local repository="$2"
    local tag="$3"
    local submodules="$4"
    local destination="${FEX64_DIR}/${name}"

    if [[ -d "${destination}/.git" && "${FORCE}" -eq 0 ]]; then
        local described
        described="$(git -C "${destination}" describe --tags --exact-match 2>/dev/null || echo "")"
        if [[ "${described}" == "${tag}" ]]; then
            ok "${name}: already at ${tag}"
            record_revision "${name}" "${destination}" "${tag}"
            return 0
        fi
        warn "${name}: checkout is at '${described:-an untagged commit}', not the pinned ${tag}"
        log "${name}: re-checking out ${tag}"
        git -C "${destination}" fetch --tags --depth 1 origin "${tag}" \
            || die "${name}: could not fetch ${tag} from ${repository}"
        git -C "${destination}" checkout --detach "${tag}" \
            || die "${name}: could not check out ${tag}"
    else
        [[ "${FORCE}" -eq 1 ]] && rm -rf "${destination}"
        mkdir -p "${FEX64_DIR}"
        log "${name}: cloning ${repository} at ${tag}"
        git clone --depth 1 --branch "${tag}" "${repository}" "${destination}" \
            || die "${name}: clone failed. If ${tag} no longer exists upstream, the pin in
scripts/dependencies.fex64.lock.sh needs updating deliberately, not silently."
    fi

    if [[ "${submodules}" == "yes" ]]; then
        log "${name}: submodules"
        git -C "${destination}" submodule update --init --recursive --depth 1 \
            || die "${name}: submodule checkout failed"
    fi

    record_revision "${name}" "${destination}" "${tag}"
    ok "${name}: $(git -C "${destination}" rev-parse HEAD) (${tag})"
}

# Records the exact commit each tag resolved to.  This file is what makes the
# build reproducible; the tags alone are not, because a tag can be moved.
record_revision() {
    local name="$1"
    local destination="$2"
    local tag="$3"
    local commit
    commit="$(git -C "${destination}" rev-parse HEAD)"

    mkdir -p "${FEX64_DIR}"
    touch "${REVISION_FILE}"
    local temporary="${REVISION_FILE}.new"
    awk -v n="${name}" '$1 != n' "${REVISION_FILE}" > "${temporary}"
    printf '%s %s %s\n' "${name}" "${tag}" "${commit}" >> "${temporary}"
    LC_ALL=C sort -o "${temporary}" "${temporary}"
    mv "${temporary}" "${REVISION_FILE}"
}

log "fex64 sources -> ${FEX64_DIR}"

for component in ${COMPONENTS}; do
    case "${component}" in
        fex)
            clone_pinned fex "${BOXEDVN_FEX_REPOSITORY}" "${BOXEDVN_FEX_TAG}" yes ;;
        wine)
            # Wine vendors no submodules and is large; a depth-1 clone of the
            # tag is roughly 200 MB.
            clone_pinned wine "${BOXEDVN_WINE_REPOSITORY}" "${BOXEDVN_WINE_TAG}" no ;;
        dxmt)
            clone_pinned dxmt "${BOXEDVN_DXMT_REPOSITORY}" "${BOXEDVN_DXMT_TAG}" yes ;;
    esac
done

log "Resolved revisions"
sed 's/^/  /' "${REVISION_FILE}"

cat <<'NEXT'

Nothing was built.  No iphoneos toolchain exists for these yet; that is stage
M0 in docs/ARCHITECTURE_FEX64.md, and it is the next piece of work.

Commit third_party/fex64/PINNED_REVISIONS.txt once a build has succeeded
against these exact commits.
NEXT
