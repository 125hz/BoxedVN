#!/usr/bin/env bash
# BoxedVN - fetch the pinned sources for the fex64 native-ARM64 stack.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/fetch-fex64-dependencies.sh [--component mythic|fex|wine|dxmt|all]
#                                       [--third-party-dir DIR]
#                                       [--force]
#
# Fetches sources only; building is scripts/build-fex64-toolchains.sh and
# needs macOS. This runs anywhere git runs, including on a PC, so the pins can
# be checked without a Mac.
#
# Each component is fetched at an exact commit rather than a branch head. All
# three branches move weekly, and a stack this deep cannot be debugged against
# a moving floor.

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
    all) COMPONENTS="mythic fex wine dxmt" ;;
    mythic|fex|wine|dxmt) ;;
    *) die "--component must be mythic, fex, wine, dxmt or all, got '${COMPONENTS}'." ;;
esac

require_command git

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"

# Submodule paths this stack never builds. FEX carries its conformance suites
# as submodules of prebuilt binaries - gvisor, posixtest, the gcc target tests
# - and recursing into them costs gigabytes and, on a CI runner, the disk. None
# of it is reachable from an iOS build.
#
# Matched as an extended regular expression against the submodule path.
SUBMODULE_EXCLUDE_fex='^External/(fex-.*-bins|fex-.*-tests.*)$'

# Submodule commits the recorded one cannot be fetched for, as "<path>=<commit>".
#
# The FEX fork records External/rpmalloc at a4d4185f, which GitHub refuses with
# "upload-pack: not our ref": the commit is not reachable from any ref in
# FEX-Emu/rpmalloc, so it survives only in clones that already had it. Upstream
# FEX pins 1d85c246 for the same release - the tip of that repository's `fex`
# branch - so this is upstream's own choice rather than a guess, and it is the
# only value that makes the fork clonable by anyone else.
SUBMODULE_OVERRIDE_fex='External/rpmalloc=1d85c246cd827ead6865f4f880d4fef53f2b1864'

# checkout_submodule_override <name> <checkout> <path> <commit>
#
# Places one submodule at a commit other than the one recorded in the tree.
# 'git submodule update' cannot do this - it exists to enforce the recorded
# commit - so the working tree is populated directly. The gitlink stays dirty
# against the superproject, which is correct: it records that this checkout
# deliberately differs.
checkout_submodule_override() {
    local name="$1"
    local destination="$2"
    local path="$3"
    local commit="$4"
    local target="${destination}/${path}"
    local url

    local key candidate
    while read -r key candidate; do
        if [[ "${candidate}" == "${path}" ]]; then
            url="$(git -C "${destination}" config --file .gitmodules \
                     --get "${key%.path}.url")"
            break
        fi
    done < <(git -C "${destination}" config --file .gitmodules \
                 --get-regexp '^submodule\..*\.path$')
    [[ -n "${url:-}" ]] || die "${name}: no URL recorded for submodule ${path}"

    if [[ -d "${target}/.git" ]] &&
       [[ "$(git -C "${target}" rev-parse HEAD 2>/dev/null)" == "${commit}" ]]; then
        ok "${name}: ${path} already at the override ${commit:0:12}"
        return 0
    fi

    log "${name}: ${path} overridden to ${commit:0:12}"
    mkdir -p "${target}"
    if [[ ! -d "${target}/.git" ]]; then
        git -C "${target}" init --quiet
        git -C "${target}" remote add origin "${url}"
    fi
    git -C "${target}" fetch --depth 1 --quiet origin "${commit}" \
        || die "${name}: could not fetch ${commit} for ${path} from ${url}"
    git -C "${target}" checkout --quiet --detach "${commit}" \
        || die "${name}: could not check out ${commit} in ${path}"
    ok "${name}: ${path} at ${commit}"
}

# update_submodules <name> <checkout>
#
# Updates every submodule except the excluded ones. Shallow first, because it
# is far faster when it works; a shallow update only succeeds when each
# recorded commit is still a branch tip upstream, which nothing guarantees, so
# it falls back to a full checkout rather than failing.
update_submodules() {
    local name="$1"
    local destination="$2"
    local exclude_variable="SUBMODULE_EXCLUDE_${name}"
    local exclude="${!exclude_variable:-}"
    local override_variable="SUBMODULE_OVERRIDE_${name}"
    local overrides="${!override_variable:-}"

    [[ -f "${destination}/.gitmodules" ]] || return 0

    local override_paths=()
    local override
    for override in ${overrides}; do
        override_paths+=("${override%%=*}")
    done

    local wanted=()
    local path
    while read -r _ path; do
        if [[ -n "${exclude}" && "${path}" =~ ${exclude} ]]; then
            ok "${name}: skipping submodule ${path}"
            continue
        fi
        local overridden=0
        local candidate
        for candidate in ${override_paths[@]+"${override_paths[@]}"}; do
            [[ "${candidate}" == "${path}" ]] && overridden=1
        done
        [[ "${overridden}" -eq 1 ]] && continue
        wanted+=("${path}")
    done < <(git -C "${destination}" config --file .gitmodules \
                 --get-regexp '^submodule\..*\.path$' || true)

    for override in ${overrides}; do
        checkout_submodule_override "${name}" "${destination}" \
            "${override%%=*}" "${override#*=}"
    done

    if [[ "${#wanted[@]}" -eq 0 ]]; then
        ok "${name}: no submodules to update"
        return 0
    fi

    log "${name}: ${#wanted[@]} submodules"
    if git -C "${destination}" submodule update --init --recursive --depth 1 \
            -- "${wanted[@]}"; then
        return 0
    fi

    warn "${name}: shallow submodule update failed; retrying unshallowed"
    git -C "${destination}" submodule update --init --recursive \
        -- "${wanted[@]}" || die "${name}: submodule checkout failed"
}

# fetch_pinned <name> <repository> <branch> <commit> <submodules:yes|no>
#
# Fetches the one commit and leaves the checkout detached on it, so nothing in
# the third-party tree can move a dependency by accident. The branch is used
# only as a fallback for servers that refuse a fetch by SHA, and as
# documentation of where the commit came from.
fetch_pinned() {
    local name="$1"
    local repository="$2"
    local branch="$3"
    local commit="$4"
    local submodules="$5"
    local destination="${FEX64_DIR}/${name}"

    if [[ "${FORCE}" -eq 1 ]]; then
        rm -rf "${destination}"
    elif [[ -d "${destination}/.git" ]]; then
        local head
        head="$(git -C "${destination}" rev-parse HEAD 2>/dev/null || echo "")"
        if [[ "${head}" == "${commit}" ]]; then
            ok "${name}: already at ${commit:0:12}"
            return 0
        fi
        warn "${name}: at ${head:0:12}, pinned to ${commit:0:12}; re-fetching"
    fi

    mkdir -p "${destination}"
    if [[ ! -d "${destination}/.git" ]]; then
        git -C "${destination}" init --quiet
        git -C "${destination}" remote add origin "${repository}"
    fi

    log "${name}: fetching ${commit:0:12} from ${repository}"
    if ! git -C "${destination}" fetch --depth 1 --quiet origin "${commit}" 2>/dev/null; then
        warn "${name}: server refused a fetch by commit; falling back to ${branch}"
        git -C "${destination}" fetch --depth 50 --quiet origin "${branch}" \
            || die "${name}: could not fetch ${branch} from ${repository}"
    fi

    git -C "${destination}" checkout --quiet --detach "${commit}" || die \
"${name}: ${commit} is not reachable from ${branch}.

The pin in scripts/dependencies.fex64.lock.sh predates a force-push or the
branch was renamed. Update the pin deliberately, and record why."

    if [[ "${submodules}" == "yes" ]]; then
        update_submodules "${name}" "${destination}"
    fi

    ok "${name}: $(git -C "${destination}" rev-parse HEAD)"
}

log "fex64 sources -> ${FEX64_DIR}"

for component in ${COMPONENTS}; do
    case "${component}" in
        mythic)
            fetch_pinned mythic "${BOXEDVN_MYTHIC_REPOSITORY}" \
                "${BOXEDVN_MYTHIC_BRANCH}" "${BOXEDVN_MYTHIC_COMMIT}" no ;;
        fex)
            fetch_pinned fex "${BOXEDVN_FEX_REPOSITORY}" \
                "${BOXEDVN_FEX_BRANCH}" "${BOXEDVN_FEX_COMMIT}" yes ;;
        wine)
            fetch_pinned wine "${BOXEDVN_WINE_REPOSITORY}" \
                "${BOXEDVN_WINE_BRANCH}" "${BOXEDVN_WINE_COMMIT}" no ;;
        dxmt)
            fetch_pinned dxmt "${BOXEDVN_DXMT_REPOSITORY}" \
                "${BOXEDVN_DXMT_BRANCH}" "${BOXEDVN_DXMT_COMMIT}" yes ;;
    esac
done

cat <<'NEXT'

Sources only; nothing was built. scripts/build-fex64-toolchains.sh does that
and needs macOS with a full Xcode.
NEXT
