#!/usr/bin/env bash
# BoxedVN - validate a developer-supplied BoxedWine64 Wine64 runtime.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/validate-wine64-runtime.sh --input DIR|ARCHIVE.zip
#       [--manifest FILE] [--output-dir DIR] [--record-manifest FILE]
#       [--allow-unpinned]
#
# BoxedWine64's audited builder produces two layered archives rather than one
# redistributable package. This checker accepts that directory or an outer ZIP,
# verifies the exact layer hashes from a developer-owned manifest, and checks
# the paths required by the proven -root/-zip launcher. It never downloads the
# Debian/Wine image and never treats native iOS Wine/DXMT binaries as a guest
# runtime.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

INPUT=""
MANIFEST=""
OUTPUT_DIR=""
RECORD_MANIFEST=""
ALLOW_UNPINNED=0

usage() {
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --input)            [[ $# -ge 2 ]] || die "--input needs a value"
                            INPUT="$2"; shift 2 ;;
        --manifest)         [[ $# -ge 2 ]] || die "--manifest needs a value"
                            MANIFEST="$2"; shift 2 ;;
        --output-dir)       [[ $# -ge 2 ]] || die "--output-dir needs a value"
                            OUTPUT_DIR="$2"; shift 2 ;;
        --record-manifest)  [[ $# -ge 2 ]] || die "--record-manifest needs a value"
                            RECORD_MANIFEST="$2"; shift 2 ;;
        --allow-unpinned)   ALLOW_UNPINNED=1; shift ;;
        -h|--help)          usage; exit 0 ;;
        *)                  die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${INPUT}" ]] || die "--input is required. Supply a Wine64 runtime directory or outer ZIP."
require_command od
require_command unzip

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        die "Neither 'shasum' nor 'sha256sum' is on PATH; cannot pin Wine64 layers."
    fi
}

if [[ "${CI:-}" == "true" && ${ALLOW_UNPINNED} -eq 1 ]]; then
    die "--allow-unpinned is refused in CI. Provide a manifest with pinned layer SHA-256 values."
fi

WORK=""
cleanup() {
    if [[ -n "${WORK}" && -d "${WORK}" ]]; then
        rm -rf "${WORK}"
    fi
}
trap cleanup EXIT

RUNTIME_DIR=""
if [[ -d "${INPUT}" ]]; then
    RUNTIME_DIR="$(cd "${INPUT}" && pwd)"
elif [[ -f "${INPUT}" ]]; then
    case "${INPUT}" in
        *.zip)
            WORK="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-wine64.XXXXXX")"
            if ! unzip -tqq "${INPUT}" >/dev/null 2>&1; then
                die "'${INPUT}' is not a readable outer ZIP. Supply the two audited Wine64 layers and manifest in a valid ZIP."
            fi
            # Only extract the three expected root entries. The path check
            # below rejects traversal and absolute names before extraction.
            while IFS= read -r entry; do
                normalized="${entry#./}"
                [[ "${normalized}" == "${entry}" || "${entry}" == ./* ]] \
                    || die "Outer runtime ZIP contains an unsafe path '${entry}'."
                [[ "${normalized}" != /* && "${normalized}" != *../* && "${normalized}" != ../* ]] \
                    || die "Outer runtime ZIP contains an unsafe path '${entry}'."
            done < <(unzip -Z1 "${INPUT}")
            for expected in glibc-rootfs64.zip wine64.zip wine64-runtime.manifest; do
                matched="$(unzip -Z1 "${INPUT}" | awk -v expected="${expected}" \
                    '$0 == expected || $0 == "./" expected { print; exit }')"
                if [[ -n "${matched}" ]]; then
                    unzip -p "${INPUT}" "${matched}" > "${WORK}/${expected}"
                fi
            done
            RUNTIME_DIR="${WORK}"
            if [[ -z "${MANIFEST}" && -f "${WORK}/wine64-runtime.manifest" ]]; then
                MANIFEST="${WORK}/wine64-runtime.manifest"
            fi
            ;;
        *)
            die "Wine64 runtime input '${INPUT}' is neither a directory nor a .zip archive."
            ;;
    esac
else
    die "Wine64 runtime input '${INPUT}' does not exist.
The BoxedWine64 rootfs build is not downloaded automatically because the
audited Docker image and Wine/Debian contents are not approved for bundling."
fi

GLIBC_ARCHIVE="${RUNTIME_DIR}/glibc-rootfs64.zip"
WINE_ARCHIVE="${RUNTIME_DIR}/wine64.zip"
require_file "${GLIBC_ARCHIVE}" \
    "Expected glibc-rootfs64.zip from third_party/boxedwine64-audit/tools/rootfs64/build-wine64-zip.sh."
require_file "${WINE_ARCHIVE}" \
    "Expected wine64.zip from third_party/boxedwine64-audit/tools/rootfs64/build-wine64-zip.sh."

if [[ -z "${MANIFEST}" && -f "${RUNTIME_DIR}/wine64-runtime.manifest" ]]; then
    MANIFEST="${RUNTIME_DIR}/wine64-runtime.manifest"
fi

MANIFEST_FORMAT=""
MANIFEST_SOURCE=""
MANIFEST_SOURCE_IMAGE=""
MANIFEST_WINE_ADDRESS_CONTRACT=""
MANIFEST_BOXEDWINE_BRIDGE_REQUIRED=""
MANIFEST_GLIBC_ARCHIVE=""
MANIFEST_GLIBC_SHA256=""
MANIFEST_WINE_ARCHIVE=""
MANIFEST_WINE_SHA256=""
MANIFEST_DXMT_UNIXLIB_SHA256=""
if [[ -n "${MANIFEST}" ]]; then
    require_file "${MANIFEST}" \
        "Copy scripts/wine64-runtime-manifest.example and fill in both layer SHA-256 values."
    while IFS='=' read -r key value; do
        [[ -z "${key}" || "${key}" == \#* ]] && continue
        [[ "${key}" =~ ^[a-z_][a-z0-9_]*$ && -n "${value}" ]] \
            || die "Malformed Wine64 manifest line (expected key=value): '${key}=${value}'"
        case "${key}" in
            format)         MANIFEST_FORMAT="${value}" ;;
            glibc_archive)  MANIFEST_GLIBC_ARCHIVE="${value}" ;;
            glibc_sha256)   MANIFEST_GLIBC_SHA256="${value}" ;;
            wine_archive)   MANIFEST_WINE_ARCHIVE="${value}" ;;
            wine_sha256)    MANIFEST_WINE_SHA256="${value}" ;;
            dxmt_unixlib_sha256) MANIFEST_DXMT_UNIXLIB_SHA256="${value}" ;;
            source)         MANIFEST_SOURCE="${value}" ;;
            source_image)   MANIFEST_SOURCE_IMAGE="${value}" ;;
            wine_address_contract) MANIFEST_WINE_ADDRESS_CONTRACT="${value}" ;;
            boxedwine_bridge_required) MANIFEST_BOXEDWINE_BRIDGE_REQUIRED="${value}" ;;
            *)              die "Unsupported Wine64 manifest key '${key}'." ;;
        esac
    done < "${MANIFEST}"
else
    [[ ${ALLOW_UNPINNED} -eq 1 ]] || die "No Wine64 runtime manifest was supplied.
Reproducible packaging requires --manifest with pinned glibc_sha256 and
wine_sha256 values. For a one-off local inspection only, pass --allow-unpinned."
    warn "No Wine64 runtime manifest supplied; layer hashes will be reported but not pinned."
fi

[[ -z "${MANIFEST}" || "${MANIFEST_FORMAT:-boxedvn-wine64-v1}" == "boxedvn-wine64-v1" ]] \
    || die "Unsupported Wine64 runtime manifest format '${MANIFEST_FORMAT:-missing}'."
[[ -z "${MANIFEST}" || \
   "${MANIFEST_SOURCE}" == "third_party/boxedwine64-audit/tools/rootfs64/build-wine64-zip.sh" || \
   "${MANIFEST_SOURCE}" == "scripts/build-wine64-runtime-ci.sh" ]] \
    || die "Manifest source must be the audited builder or the pinned Ubuntu CI builder."
[[ -z "${MANIFEST}" || \
   "${MANIFEST_SOURCE_IMAGE}" == "boxedwine64/wine64-debian:bookworm" || \
   "${MANIFEST_SOURCE_IMAGE}" == "ubuntu-24.04-apt" ]] \
    || die "Manifest source_image must identify the audited container or Ubuntu 24.04 package builder."
if [[ "${MANIFEST_SOURCE}" == "scripts/build-wine64-runtime-ci.sh" ]]; then
    [[ "${MANIFEST_WINE_ADDRESS_CONTRACT}" == "stock-low-teb-hint-fixed-kuser-v1" ]] \
        || die "Ubuntu CI Wine64 manifest must record Wine's low-TEB/fixed-KUSER address contract."
    [[ "${MANIFEST_BOXEDWINE_BRIDGE_REQUIRED}" == "K64_NATIVE_IDENTITY_KUSER_ALIAS" ]] \
        || die "Ubuntu CI Wine64 manifest must require the BoxedWine high-window KUSER bridge."
fi
[[ -z "${MANIFEST}" || "${MANIFEST_GLIBC_ARCHIVE:-glibc-rootfs64.zip}" == "glibc-rootfs64.zip" ]] \
    || die "Manifest glibc_archive must be exactly glibc-rootfs64.zip."
[[ -z "${MANIFEST}" || "${MANIFEST_WINE_ARCHIVE:-wine64.zip}" == "wine64.zip" ]] \
    || die "Manifest wine_archive must be exactly wine64.zip."

is_sha256() { [[ "$1" =~ ^[0-9a-fA-F]{64}$ ]]; }
verify_or_report() {
    local path="$1"
    local key="$2"
    local actual
    actual="$(sha256_file "${path}")"
    printf '  %-12s %s\n' "${key}" "${actual}"
    if [[ -n "${MANIFEST}" ]]; then
        local expected=""
        case "${key}" in
            glibc_sha256) expected="${MANIFEST_GLIBC_SHA256}" ;;
            wine_sha256)  expected="${MANIFEST_WINE_SHA256}" ;;
            *)             die "Internal error: unknown checksum key '${key}'." ;;
        esac
        is_sha256 "${expected}" \
            || die "Manifest ${key} is missing or not a 64-hex SHA-256 value."
        [[ "${actual}" == "${expected}" ]] \
            || die "Checksum mismatch for '${path}'.\n  expected SHA-256: ${expected}\n  actual   SHA-256: ${actual}\nRefresh the developer-owned manifest only after reviewing the source artefact."
        ok "${key} verified"
    fi
}

zip_has() {
    local archive="$1"
    local required="$2"
    unzip -Z1 "${archive}" \
        | sed 's#^\./##' \
        | awk -v required="${required}" '$0 == required || $0 == required "/" || index($0, required "/") == 1 { found=1 } END { exit(found ? 0 : 1) }'
}

check_zip_path() {
    local archive="$1"
    local path="$2"
    zip_has "${archive}" "${path}" \
        || die "'$(basename "${archive}")' is missing required guest path '${path}'.\nThe archive is not the layered output of the audited BoxedWine64 rootfs builder."
    ok "$(basename "${archive}"): ${path}"
}

# Wine64 execs /usr/lib/wine/wineserver directly. The distro ships a small
# /bin/sh wrapper under that name which picks between wineserver64 and
# wineserver32; packaged as the guest's wineserver it made a 64-bit process
# exec a script, which resolved through a 32-bit interpreter and left the
# guest with neither an x86-64 image nor a valid 32-bit state. Both guest
# paths must be the same real x86-64 executable, so check the ELF header
# rather than the name.
check_zip_entry_elf64_x86_64() {
    local archive="$1"
    local path="$2"
    local bytes
    read -r -a bytes <<< "$(unzip -p "${archive}" "${path}" 2>/dev/null \
        | od -An -tu1 -N20 | tr '\n' ' ')"
    [[ ${#bytes[@]} -ge 20 ]] \
        || die "'${path}' in $(basename "${archive}") is too small to be an ELF executable."
    [[ "${bytes[0]}" == 127 && "${bytes[1]}" == 69 && "${bytes[2]}" == 76 && "${bytes[3]}" == 70 ]] \
        || die "'${path}' in $(basename "${archive}") is not an ELF file.\nThe distro's /bin/sh wineserver wrapper must not be packaged as a guest executable."
    [[ "${bytes[4]}" == 2 ]] \
        || die "'${path}' in $(basename "${archive}") is not ELFCLASS64."
    [[ "${bytes[18]}" == 62 && "${bytes[19]}" == 0 ]] \
        || die "'${path}' in $(basename "${archive}") is not EM_X86_64."
    ok "$(basename "${archive}"): ${path} is ELF64 EM_X86_64"
}

zip_entry_sha256() {
    local archive="$1"
    local path="$2"
    if command -v sha256sum >/dev/null 2>&1; then
        unzip -p "${archive}" "${path}" | sha256sum | awk '{print $1}'
    else
        unzip -p "${archive}" "${path}" | shasum -a 256 | awk '{print $1}'
    fi
}

if ! unzip -tqq "${GLIBC_ARCHIVE}" >/dev/null 2>&1; then
    die "'${GLIBC_ARCHIVE}' is not a readable ZIP archive."
fi
if ! unzip -tqq "${WINE_ARCHIVE}" >/dev/null 2>&1; then
    die "'${WINE_ARCHIVE}' is not a readable ZIP archive."
fi

log "Validating BoxedWine64 layered runtime"
verify_or_report "${GLIBC_ARCHIVE}" glibc_sha256
verify_or_report "${WINE_ARCHIVE}" wine_sha256

# These are the paths emitted by the audited build-wine64-zip.sh and consumed
# by run_wine64.sh's -root/-zip launch sequence.
check_zip_path "${GLIBC_ARCHIVE}" lib64/ld-linux-x86-64.so.2
check_zip_path "${GLIBC_ARCHIVE}" lib/x86_64-linux-gnu/libc.so.6
check_zip_path "${WINE_ARCHIVE}" usr/lib/wine/wine64
check_zip_path "${WINE_ARCHIVE}" usr/lib/wine/wineserver64
check_zip_path "${WINE_ARCHIVE}" usr/lib/wine/wineserver
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" usr/lib/wine/wine64
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" usr/lib/wine/wineserver64
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" usr/lib/wine/wineserver
wineserver_generic_sha="$(zip_entry_sha256 "${WINE_ARCHIVE}" usr/lib/wine/wineserver)"
wineserver64_sha="$(zip_entry_sha256 "${WINE_ARCHIVE}" usr/lib/wine/wineserver64)"
[[ -n "${wineserver_generic_sha}" && "${wineserver_generic_sha}" == "${wineserver64_sha}" ]] \
    || die "usr/lib/wine/wineserver and usr/lib/wine/wineserver64 differ.\nWine64 execs the generic path, so both must be the same x86-64 executable.\n  wineserver:   ${wineserver_generic_sha}\n  wineserver64: ${wineserver64_sha}"
ok "$(basename "${WINE_ARCHIVE}"): wineserver matches wineserver64 (${wineserver64_sha})"
check_zip_path "${WINE_ARCHIVE}" usr/lib/x86_64-linux-gnu/wine/x86_64-unix
check_zip_path "${WINE_ARCHIVE}" usr/lib/x86_64-linux-gnu/wine/x86_64-windows
if [[ "${MANIFEST_SOURCE}" == "scripts/build-wine64-runtime-ci.sh" ]]; then
    check_zip_path "${WINE_ARCHIVE}" \
        usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so
    [[ "${MANIFEST_DXMT_UNIXLIB_SHA256}" =~ ^[0-9a-fA-F]{64}$ ]] \
        || die "Ubuntu CI Wine64 manifest is missing dxmt_unixlib_sha256."
    if command -v sha256sum >/dev/null 2>&1; then
        dxmt_actual="$(unzip -p "${WINE_ARCHIVE}" \
            usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so \
            | sha256sum | awk '{print $1}')"
    else
        dxmt_actual="$(unzip -p "${WINE_ARCHIVE}" \
            usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so \
            | shasum -a 256 | awk '{print $1}')"
    fi
    [[ "${dxmt_actual}" == "${MANIFEST_DXMT_UNIXLIB_SHA256}" ]] \
        || die "The Wine64 archive's winemetal.so checksum does not match its manifest."
fi

if [[ -n "${RECORD_MANIFEST}" ]]; then
    mkdir -p "$(dirname "${RECORD_MANIFEST}")"
    {
        printf '%s\n' '# BoxedVN Wine64 runtime manifest v1.'
        printf '%s\n' 'format=boxedvn-wine64-v1'
        printf '%s\n' 'source=third_party/boxedwine64-audit/tools/rootfs64/build-wine64-zip.sh'
        printf '%s\n' 'source_image=boxedwine64/wine64-debian:bookworm'
        printf '%s\n' 'glibc_archive=glibc-rootfs64.zip'
        printf 'glibc_sha256=%s\n' "$(sha256_file "${GLIBC_ARCHIVE}")"
        printf '%s\n' 'wine_archive=wine64.zip'
        printf 'wine_sha256=%s\n' "$(sha256_file "${WINE_ARCHIVE}")"
    } > "${RECORD_MANIFEST}"
    ok "recorded manifest: ${RECORD_MANIFEST}"
fi

if [[ -n "${OUTPUT_DIR}" ]]; then
    mkdir -p "${OUTPUT_DIR}"
    cp "${GLIBC_ARCHIVE}" "${OUTPUT_DIR}/glibc-rootfs64.zip"
    cp "${WINE_ARCHIVE}" "${OUTPUT_DIR}/wine64.zip"
    if [[ -n "${MANIFEST}" ]]; then
        cp "${MANIFEST}" "${OUTPUT_DIR}/wine64-runtime.manifest"
    elif [[ -n "${RECORD_MANIFEST}" ]]; then
        cp "${RECORD_MANIFEST}" "${OUTPUT_DIR}/wine64-runtime.manifest"
    fi
    ok "staged Wine64 layers: ${OUTPUT_DIR}"
fi

printf '\n'
ok "Wine64 runtime validation passed"
