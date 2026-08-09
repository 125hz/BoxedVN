#!/usr/bin/env bash
# BoxedVN - tests for the dependency pinning machinery.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Verifies that every pinned dependency is well formed and that
# download_pinned actually refuses a file whose checksum does not match.  These
# run without network access.
#
# Registered with ctest by ios/tests/CMakeLists.txt.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"
source "${SCRIPT_DIR}/dependencies.lock.sh"

failures=0
checks=0

check() {
    local description="$1"
    shift
    checks=$((checks + 1))
    if "$@"; then
        printf '  ok   %s\n' "${description}"
    else
        printf '  FAIL %s\n' "${description}" >&2
        failures=$((failures + 1))
    fi
}

is_sha256() {
    [[ "$1" =~ ^[0-9a-f]{64}$ ]]
}

is_https_url() {
    [[ "$1" =~ ^https:// ]]
}

# --- Named single dependencies ---------------------------------------------
check "SDL2 version is set"        test -n "${BOXEDVN_SDL2_VERSION}"
check "SDL2 URL is https"          is_https_url "${BOXEDVN_SDL2_URL}"
check "SDL2 SHA-256 is well formed" is_sha256 "${BOXEDVN_SDL2_SHA256}"
check "SDL2 URL contains its pinned version" \
    grep -q "${BOXEDVN_SDL2_VERSION}" <<<"${BOXEDVN_SDL2_URL}"

check "XcodeGen version is set"        test -n "${BOXEDVN_XCODEGEN_VERSION}"
check "XcodeGen URL is https"          is_https_url "${BOXEDVN_XCODEGEN_URL}"
check "XcodeGen SHA-256 is well formed" is_sha256 "${BOXEDVN_XCODEGEN_SHA256}"
check "XcodeGen URL contains its pinned version" \
    grep -q "${BOXEDVN_XCODEGEN_VERSION}" <<<"${BOXEDVN_XCODEGEN_URL}"

check "MoltenVK version is set"        test -n "${BOXEDVN_MOLTENVK_VERSION}"
check "MoltenVK URL is https"          is_https_url "${BOXEDVN_MOLTENVK_URL}"
check "MoltenVK SHA-256 is well formed" is_sha256 "${BOXEDVN_MOLTENVK_SHA256}"
check "MoltenVK URL contains its pinned version" \
    grep -q "${BOXEDVN_MOLTENVK_VERSION}" <<<"${BOXEDVN_MOLTENVK_URL}"

# --- Root filesystems -------------------------------------------------------
check "at least one root filesystem is pinned" \
    test "${#BOXEDVN_ROOTFS_ENTRIES[@]}" -ge 1

seen_default=0
for entry in "${BOXEDVN_ROOTFS_ENTRIES[@]}"; do
    IFS='|' read -r id url sha256 bytes description <<<"${entry}"
    check "rootfs '${id}': id is non-empty"        test -n "${id}"
    check "rootfs '${id}': URL is https"           is_https_url "${url}"
    check "rootfs '${id}': SHA-256 is well formed" is_sha256 "${sha256}"
    check "rootfs '${id}': size is a positive integer" \
        bash -c "[[ '${bytes}' =~ ^[0-9]+$ ]] && (( ${bytes} > 0 ))"
    check "rootfs '${id}': has a description"      test -n "${description}"
    # A "latest" or otherwise mutable URL would defeat the whole point.
    check "rootfs '${id}': URL is not mutable" \
        bash -c "! grep -Eqi '(latest|master|main|nightly)' <<<'${url}'"
    if [[ "${id}" == "${BOXEDVN_ROOTFS_DEFAULT}" ]]; then
        seen_default=1
    fi
done

check "the default rootfs id exists in the catalogue" test "${seen_default}" -eq 1

# --- No duplicate ids -------------------------------------------------------
duplicate_ids="$(printf '%s\n' "${BOXEDVN_ROOTFS_ENTRIES[@]}" \
    | cut -d'|' -f1 | sort | uniq -d)"
check "rootfs ids are unique" test -z "${duplicate_ids}"

# --- download_pinned actually verifies --------------------------------------
# The point of the pin is that a wrong file is rejected, so that is tested
# directly rather than assumed.
work="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-pin-test.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

printf 'not the real artefact' > "${work}/artifact.bin"
correct_sha="$(shasum -a 256 "${work}/artifact.bin" | awk '{print $1}')"
wrong_sha="0000000000000000000000000000000000000000000000000000000000000000"

# download_pinned and verify_sha256 report failure by calling die, which exits.
# Each is therefore run in a subshell so a refusal does not end this script -
# a refusal is the behaviour under test.  The URL is a nonexistent file:// so
# no network access is attempted.
UNREACHABLE="file:///boxedvn/definitely/not/here"

checks=$((checks + 1))
if ( download_pinned "${UNREACHABLE}" "${work}/artifact.bin" "${correct_sha}" ) \
        >/dev/null 2>&1; then
    printf '  ok   a correctly checksummed cached file is accepted\n'
else
    printf '  FAIL a correctly checksummed cached file was rejected\n' >&2
    failures=$((failures + 1))
fi

# A cached file whose checksum does not match must NOT be accepted.  With an
# unreachable URL the re-download fails, which is itself the correct outcome:
# what must never happen is silent acceptance.
checks=$((checks + 1))
if ( download_pinned "${UNREACHABLE}" "${work}/artifact.bin" "${wrong_sha}" ) \
        >/dev/null 2>&1; then
    printf '  FAIL a file with the wrong checksum was accepted\n' >&2
    failures=$((failures + 1))
else
    printf '  ok   a file with the wrong checksum is refused\n'
fi

# verify_sha256 must fail loudly on a mismatch.
checks=$((checks + 1))
printf 'contents' > "${work}/other.bin"
if ( verify_sha256 "${work}/other.bin" "${wrong_sha}" ) >/dev/null 2>&1; then
    printf '  FAIL verify_sha256 accepted a mismatched checksum\n' >&2
    failures=$((failures + 1))
else
    printf '  ok   verify_sha256 rejects a mismatched checksum\n'
fi

printf '\n%d check(s) run, %d failed\n' "${checks}" "${failures}"
[[ ${failures} -eq 0 ]]
