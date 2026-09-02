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
# The WoW64 half of the layer: the 32-bit PE tree and Wine's own thunk modules.
MANIFEST_I386_WINDOWS_MODULE_COUNT=""
MANIFEST_I386_WINDOWS_NTDLL_SHA256=""
MANIFEST_WOW64_DLL_SHA256=""
MANIFEST_WOW64WIN_DLL_SHA256=""
MANIFEST_WOW64CPU_DLL_SHA256=""
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
            i386_windows_module_count) MANIFEST_I386_WINDOWS_MODULE_COUNT="${value}" ;;
            i386_windows_ntdll_sha256) MANIFEST_I386_WINDOWS_NTDLL_SHA256="${value}" ;;
            wow64_dll_sha256)    MANIFEST_WOW64_DLL_SHA256="${value}" ;;
            wow64win_dll_sha256) MANIFEST_WOW64WIN_DLL_SHA256="${value}" ;;
            wow64cpu_dll_sha256) MANIFEST_WOW64CPU_DLL_SHA256="${value}" ;;
            x11_shim_libx11_sha256) MANIFEST_X11_SHIM_LIBX11_SHA256="${value}" ;;
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

# A builtin Windows module has to be a PE image the guest can map, not a
# placeholder, not a truncated entry, and not the text of a link target that
# was stored as a plain file. Check the DOS header, then follow e_lfanew to
# the PE signature and the COFF machine field: AMD64 is 0x8664, little-endian.
check_zip_entry_pe32plus_amd64() {
    local archive="$1"
    local path="$2"
    local bytes
    # -v is required: od otherwise folds repeated zero-filled DOS-header rows
    # into a literal "*", which shifts every subsequent Bash-array index and
    # makes a valid e_lfanew appear to be zero.
    read -r -a bytes <<< "$(unzip -p "${archive}" "${path}" 2>/dev/null \
        | od -An -tu1 -v -N4096 | tr '\n' ' ')"
    [[ ${#bytes[@]} -ge 64 ]] \
        || die "'${path}' in $(basename "${archive}") is too small to be a PE image."
    [[ "${bytes[0]}" == 77 && "${bytes[1]}" == 90 ]] \
        || die "'${path}' in $(basename "${archive}") does not start with MZ.\nA builtin module that is not a PE image cannot be loaded, however large it is."
    local lfanew=$(( bytes[60] + bytes[61] * 256 + bytes[62] * 65536 + bytes[63] * 16777216 ))
    (( lfanew > 0 && lfanew + 6 < ${#bytes[@]} )) \
        || die "'${path}' in $(basename "${archive}") has an unusable PE header offset (${lfanew})."
    [[ "${bytes[lfanew]}" == 80 && "${bytes[lfanew+1]}" == 69 \
       && "${bytes[lfanew+2]}" == 0 && "${bytes[lfanew+3]}" == 0 ]] \
        || die "'${path}' in $(basename "${archive}") has no PE signature at offset ${lfanew}."
    local machine=$(( bytes[lfanew+4] + bytes[lfanew+5] * 256 ))
    (( machine == 34404 )) \
        || die "'${path}' in $(basename "${archive}") is not PE32+ AMD64 (machine 0x$(printf '%04x' "${machine}"))."
    ok "$(basename "${archive}"): ${path} is a PE32+ AMD64 image"
}

# The same header walk for the 32-bit tree. Under new WoW64 the i386 builtins
# are PE32 images (COFF machine 0x014c) executed in the CPU's compatibility
# mode; a 64-bit image staged there, or a 32-bit image staged in the 64-bit
# tree, passes every path check and cannot be mapped by the guest at all.
check_zip_entry_pe32_i386() {
    local archive="$1"
    local path="$2"
    local bytes
    # -v for the same reason as above: od folds repeated zero rows into "*".
    read -r -a bytes <<< "$(unzip -p "${archive}" "${path}" 2>/dev/null \
        | od -An -tu1 -v -N4096 | tr '\n' ' ')"
    [[ ${#bytes[@]} -ge 64 ]] \
        || die "'${path}' in $(basename "${archive}") is too small to be a PE image."
    [[ "${bytes[0]}" == 77 && "${bytes[1]}" == 90 ]] \
        || die "'${path}' in $(basename "${archive}") does not start with MZ.\nA 32-bit builtin that is not a PE image cannot be loaded, however large it is."
    local lfanew=$(( bytes[60] + bytes[61] * 256 + bytes[62] * 65536 + bytes[63] * 16777216 ))
    (( lfanew > 0 && lfanew + 6 < ${#bytes[@]} )) \
        || die "'${path}' in $(basename "${archive}") has an unusable PE header offset (${lfanew})."
    [[ "${bytes[lfanew]}" == 80 && "${bytes[lfanew+1]}" == 69 \
       && "${bytes[lfanew+2]}" == 0 && "${bytes[lfanew+3]}" == 0 ]] \
        || die "'${path}' in $(basename "${archive}") has no PE signature at offset ${lfanew}."
    local machine=$(( bytes[lfanew+4] + bytes[lfanew+5] * 256 ))
    (( machine == 332 )) \
        || die "'${path}' in $(basename "${archive}") is not PE32 i386 (machine 0x$(printf '%04x' "${machine}")).\nThe 32-bit PE tree must come from the i386 libwine package, not from the 64-bit one."
    ok "$(basename "${archive}"): ${path} is a PE32 i386 image"
}

# BoxedWine's ZIP filesystem does not read POSIX symlinks out of an archive:
# it recognises a link by a `.link` suffix on the entry name whose contents are
# the target path (EXT_LINK in source/io/fsnode.h). A real symlink stored in
# the archive becomes a small regular file holding the target text, which is a
# silent failure -- so compatibility paths are checked by that name, and their
# targets are checked to exist.
check_zip_guest_link() {
    local archive="$1"
    local link="$2"
    local target="$3"
    local target_entry="${target#/}"
    local actual
    # unzip exits nonzero for a missing member; keep that inside the explicit
    # diagnostic below instead of letting `set -e` terminate without naming
    # the bad compatibility path.
    actual="$(unzip -p "${archive}" "${link}.link" 2>/dev/null || true)"
    [[ -n "${actual}" ]] \
        || die "'$(basename "${archive}")' has no guest link entry '${link}.link'.\nA POSIX symlink in the archive is not a guest symlink; BoxedWine reads the '.link' name."
    [[ "${actual}" == "${target}" ]] \
        || die "guest link '${link}.link' points at '${actual}', expected '${target}'."
    zip_has "${archive}" "${target_entry}" \
        || die "guest link '${link}.link' points at '${target}', which the archive does not contain."
    ok "$(basename "${archive}"): ${link} -> ${target}"
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

# Wine's font support dlopens FreeType by soname. Two device runs reported
# "Wine cannot find the FreeType font library" after failing to open it at BOTH
# multiarch paths in turn, which is what an archive missing it looks like from
# the guest side. On the runner /lib is a symlink to /usr/lib, but a BoxedWine
# ZIP does not interpret POSIX symlinks, so both guest paths have to hold a
# real x86-64 ELF -- one copy at one of them would still fail the other lookup.
#
# The ELF check is not decoration: the repository also builds a static FreeType
# for the iOS host, and substituting that here would satisfy a name check while
# leaving Wine's Unix side with nothing it can load.
#
# The two paths land in DIFFERENT archives, because the builder partitions the
# stage by top-level directory: lib64/lib/etc/tmp/run/home/var go to the glibc
# layer and usr/root to the Wine layer. Both are extracted into the same guest
# root, so the guest sees both paths -- but each has to be checked against the
# archive that actually carries it.
check_zip_path "${GLIBC_ARCHIVE}" lib/x86_64-linux-gnu/libfreetype.so.6
check_zip_entry_elf64_x86_64 "${GLIBC_ARCHIVE}" lib/x86_64-linux-gnu/libfreetype.so.6

# Fontconfig loads without its configuration and then reports "Cannot load
# default config file", which a device run showed leaves Wine with no font
# backend even though libfreetype opened. A loaded library with no
# configuration is the regression this guards.
check_zip_path "${GLIBC_ARCHIVE}" etc/fonts/fonts.conf

# Not merely present: parseable. A device run reported "out of memory" at both
# <include> lines of fonts.conf and then refused the whole file. Fontconfig
# reports a parse failure that way, and the shape that produces it here is a
# guest link's target text sitting where an XML file should be -- BoxedWine
# writes links as a `.link` file whose contents are the target path, and a
# tree that was meant to be materialised but was not looks exactly like that.
check_zip_entry_is_xml() {
    local archive="$1"
    local path="$2"
    local head
    head="$(unzip -p "${archive}" "${path}" 2>/dev/null | head -c 512 | tr -d '\r')"
    [[ -n "${head}" ]] \
        || die "'${path}' in $(basename "${archive}") is empty."
    case "${head}" in
        '<?xml'*|'<fontconfig'*|'<!--'*|'<!DOCTYPE'*) ;;
        *) die "'${path}' in $(basename "${archive}") does not begin as an XML document.\nA guest link's target text is not a configuration file; fontconfig reports that as a parse failure and then loads nothing." ;;
    esac
    ok "$(basename "${archive}"): ${path} is XML"
}
check_zip_entry_is_xml "${GLIBC_ARCHIVE}" etc/fonts/fonts.conf
# fonts.conf includes conf.d; an empty include is the other half of the same
# failure and is silent from the guest side.
fontconfig_confd_count="$({ unzip -Z1 "${GLIBC_ARCHIVE}" 'etc/fonts/conf.d/*.conf' 2>/dev/null || true; } | wc -l | tr -d ' ')"
(( fontconfig_confd_count > 0 )) \
    || die "'$(basename "${GLIBC_ARCHIVE}")' carries no etc/fonts/conf.d/*.conf entries.\nfonts.conf includes that directory; an empty include leaves fontconfig with no configuration."
while IFS= read -r conf_entry; do
    [[ -n "${conf_entry}" ]] || continue
    check_zip_entry_is_xml "${GLIBC_ARCHIVE}" "${conf_entry}"
done < <({ unzip -Z1 "${GLIBC_ARCHIVE}" 'etc/fonts/conf.d/*.conf' 2>/dev/null || true; } | sed 's#^\./##')
# A guest link anywhere under the configuration tree is the failure itself.
if unzip -Z1 "${GLIBC_ARCHIVE}" | sed 's#^\./##' | grep -q '^etc/fonts/.*\.link$'; then
    die "'$(basename "${GLIBC_ARCHIVE}")' stores guest links under etc/fonts. Fontconfig cannot follow them."
fi
ok "$(basename "${GLIBC_ARCHIVE}"): ${fontconfig_confd_count} fontconfig conf.d files"

# The X11 client libraries winex11.so links against directly. Without them the
# user driver cannot load, the process has no desktop window, and every
# CreateWindowEx fails with ERROR_INVALID_WINDOW_HANDLE.
for x11_core in libX11.so.6 libXext.so.6; do
    check_zip_path "${GLIBC_ARCHIVE}" "lib/x86_64-linux-gnu/${x11_core}"
    check_zip_entry_elf64_x86_64 "${GLIBC_ARCHIVE}" "lib/x86_64-linux-gnu/${x11_core}"
done
# Wine puts its server socket directory in the modelled user's XDG runtime
# directory. The rootfs has to ship it; BoxedWine's permission policy is what
# makes it writable, because ZIP entries carry no Unix mode.
check_zip_path "${GLIBC_ARCHIVE}" run/user/1000
# Wine derives its search paths from where its own files are: the directory it
# reads from /proc/self/exe is where it looks for wineserver, and its module
# root is ntdll.so's parent. The loader therefore has to live in the same tree
# as the modules. It used to be relocated to /usr/lib/wine while the module
# trees stayed here, and a device run reached Windows code and then failed with
# "could not load kernel32.dll, status c0000135" with kernel32.dll present in
# this archive the whole time.
WINE_MODULE_ROOT=usr/lib/x86_64-linux-gnu/wine
check_zip_path "${WINE_ARCHIVE}" usr/lib/x86_64-linux-gnu/libfreetype.so.6
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" usr/lib/x86_64-linux-gnu/libfreetype.so.6
for x11_core in libX11.so.6 libXext.so.6; do
    check_zip_path "${WINE_ARCHIVE}" "usr/lib/x86_64-linux-gnu/${x11_core}"
done

# Wine's X11 user driver, both halves. win32u loads the PE side out of the
# Windows module tree and that side dlopens the Unix side. A runtime missing
# either one runs with no user driver at all, which Wine does not report as an
# error: it simply has no desktop window to parent a new window to.
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/x86_64-windows/winex11.drv"
check_zip_entry_pe32plus_amd64 "${WINE_ARCHIVE}" \
    "${WINE_MODULE_ROOT}/x86_64-windows/winex11.drv"
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/x86_64-unix/winex11.so"
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" \
    "${WINE_MODULE_ROOT}/x86_64-unix/winex11.so"

# BoxedWine's own x86-64 X11 client libraries, which the 64-bit launch puts
# first on LD_LIBRARY_PATH. Without them winex11.so binds to the distro
# libX11, connects to /tmp/.X11-unix/X0, is refused, and the process has no
# desktop window. Every library the driver links or dlopens has to be here as
# an x86-64 shared object.
X11_SHIM_DIR=usr/lib/boxedwine64-x11
for x11_shim in libX11.so.6 libXext.so.6 libXrender.so.1 libXrandr.so.2 \
                libXinerama.so.1 libXi.so.6 libXcursor.so.1 libXfixes.so.3 \
                libXcomposite.so.1 libXxf86vm.so.1; do
    check_zip_path "${WINE_ARCHIVE}" "${X11_SHIM_DIR}/${x11_shim}"
    check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" "${X11_SHIM_DIR}/${x11_shim}"
done

# Wine's built-in bitmap fonts, at the data root and reachable through the
# derived path the guest actually asks for. A device run showed every one of
# these opens failing.
#
# Counted rather than named. Which .fon files a distro ships varies by package
# version -- Ubuntu's fonts-wine 9.0 has vgasys.fon but no vgaoem.fon, and a
# hard-coded list is a guess about someone else's packaging that fails the
# build for the wrong reason. What matters is that the directory is populated.
wine_font_count="$({ unzip -Z1 "${WINE_ARCHIVE}" 'usr/share/wine/fonts/*.fon' 2>/dev/null || true; } | wc -l | tr -d ' ')"
(( wine_font_count >= 8 ))     || die "'$(basename "${WINE_ARCHIVE}")' carries ${wine_font_count} Wine bitmap fonts under usr/share/wine/fonts.
Wine draws a window's non-client area with these; install the distro's Wine font package before assembling the runtime."
ok "$(basename "${WINE_ARCHIVE}"): ${wine_font_count} Wine bitmap fonts"
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wine64"
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver64"
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver"
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wine64"
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver64"
check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver"
wineserver_generic_sha="$(zip_entry_sha256 "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver")"
wineserver64_sha="$(zip_entry_sha256 "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver64")"
[[ -n "${wineserver_generic_sha}" && "${wineserver_generic_sha}" == "${wineserver64_sha}" ]] \
    || die "${WINE_MODULE_ROOT}/wineserver and ${WINE_MODULE_ROOT}/wineserver64 differ.\nWine execs the generic name out of the loader's own directory, so both must be the same x86-64 executable.\n  wineserver:   ${wineserver_generic_sha}\n  wineserver64: ${wineserver64_sha}"
ok "$(basename "${WINE_ARCHIVE}"): wineserver matches wineserver64 (${wineserver64_sha})"
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/x86_64-unix"
check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/x86_64-windows"

# The three builtins the loader needs before it can run anything. Named
# exactly, case-sensitively, because that is how the guest looks them up.
for required_builtin in ntdll.dll kernel32.dll kernelbase.dll; do
    check_zip_path "${WINE_ARCHIVE}" \
        "${WINE_MODULE_ROOT}/x86_64-windows/${required_builtin}"
    check_zip_entry_pe32plus_amd64 "${WINE_ARCHIVE}" \
        "${WINE_MODULE_ROOT}/x86_64-windows/${required_builtin}"
done

# The WoW64 lane. Wine runs 32-bit Windows programs inside the 64-bit Unix
# process: the 32-bit PE builtins live in a third architecture directory under
# the same module root, and Wine's own thunk modules -- which are 64-bit
# builtins -- are what build and enter that 32-bit context. A runtime missing
# either half has no 32-bit lane, and Wine reports that only as a failure to
# start the image, which is indistinguishable from a broken program.
PE32_DIR="${WINE_MODULE_ROOT}/i386-windows"
check_zip_path "${WINE_ARCHIVE}" "${PE32_DIR}"
for required_pe32 in ntdll.dll kernel32.dll; do
    check_zip_path "${WINE_ARCHIVE}" "${PE32_DIR}/${required_pe32}"
    check_zip_entry_pe32_i386 "${WINE_ARCHIVE}" "${PE32_DIR}/${required_pe32}"
done
# The 64-bit tree staying 64-bit is what the loop above already proves for
# ntdll, kernel32 and kernelbase; the thunk modules are held to the same class,
# because the packaging step that adds a second tree is exactly where the two
# architectures can be swapped without any name changing.
for wow64_module in wow64.dll wow64win.dll wow64cpu.dll; do
    check_zip_path "${WINE_ARCHIVE}" \
        "${WINE_MODULE_ROOT}/x86_64-windows/${wow64_module}"
    check_zip_entry_pe32plus_amd64 "${WINE_ARCHIVE}" \
        "${WINE_MODULE_ROOT}/x86_64-windows/${wow64_module}"
done
check_zip_guest_link "${WINE_ARCHIVE}" usr/lib/wine/i386-windows \
    "/${PE32_DIR}"

# The manifest pins the WoW64 half the same way it pins the layer archives: a
# runtime that lost its 32-bit tree between build and packaging still has every
# 64-bit path this checker requires.
if [[ -n "${MANIFEST}" ]]; then
    [[ "${MANIFEST_I386_WINDOWS_MODULE_COUNT}" =~ ^[0-9]+$ ]] \
        || die "Wine64 manifest is missing i386_windows_module_count."
    pe32_entry_count="$(unzip -Z1 "${WINE_ARCHIVE}" \
        | sed 's#^\./##' \
        | awk -v prefix="${PE32_DIR}/" \
            'index($0, prefix) == 1 && $0 != prefix && substr($0, length($0)) != "/" { count++ } END { print count + 0 }')"
    (( pe32_entry_count > 0 )) \
        || die "'$(basename "${WINE_ARCHIVE}")' carries no files under ${PE32_DIR}."
    (( pe32_entry_count == MANIFEST_I386_WINDOWS_MODULE_COUNT )) \
        || die "The Wine64 archive holds ${pe32_entry_count} 32-bit PE modules; its manifest records ${MANIFEST_I386_WINDOWS_MODULE_COUNT}."
    ok "$(basename "${WINE_ARCHIVE}"): ${pe32_entry_count} 32-bit PE modules"
    for pinned in \
        "i386_windows_ntdll_sha256:${MANIFEST_I386_WINDOWS_NTDLL_SHA256}:${PE32_DIR}/ntdll.dll" \
        "wow64_dll_sha256:${MANIFEST_WOW64_DLL_SHA256}:${WINE_MODULE_ROOT}/x86_64-windows/wow64.dll" \
        "wow64win_dll_sha256:${MANIFEST_WOW64WIN_DLL_SHA256}:${WINE_MODULE_ROOT}/x86_64-windows/wow64win.dll" \
        "wow64cpu_dll_sha256:${MANIFEST_WOW64CPU_DLL_SHA256}:${WINE_MODULE_ROOT}/x86_64-windows/wow64cpu.dll"; do
        pinned_key="${pinned%%:*}"
        pinned_rest="${pinned#*:}"
        pinned_expected="${pinned_rest%%:*}"
        pinned_path="${pinned_rest#*:}"
        is_sha256 "${pinned_expected}" \
            || die "Wine64 manifest ${pinned_key} is missing or not a 64-hex SHA-256 value."
        pinned_actual="$(zip_entry_sha256 "${WINE_ARCHIVE}" "${pinned_path}")"
        [[ "${pinned_actual}" == "${pinned_expected}" ]] \
            || die "Checksum mismatch for '${pinned_path}'.\n  expected SHA-256: ${pinned_expected}\n  actual   SHA-256: ${pinned_actual}"
        ok "${pinned_key} verified"
    done
fi

# ntdll needs these locale tables during its earliest process initialization.
# With the multiarch module root Wine derives
# /usr/lib/x86_64-linux-gnu/wine/../../share/wine = /usr/lib/share/wine, so both
# the real data and the guest link from that exact location must be present. A
# device run without this link
# reached ntdll.so+0x12338 and dereferenced a null locale-table pointer.
for required_nls in c_20127.nls locale.nls l_intl.nls; do
    check_zip_path "${WINE_ARCHIVE}" "usr/share/wine/nls/${required_nls}"
done
check_zip_guest_link "${WINE_ARCHIVE}" \
    usr/lib/share/wine /usr/share/wine

# The relocated layout stays reachable, as guest links rather than as copies.
check_zip_guest_link "${WINE_ARCHIVE}" usr/lib/wine/wine64 \
    "/${WINE_MODULE_ROOT}/wine64"
check_zip_guest_link "${WINE_ARCHIVE}" usr/lib/wine/wineserver \
    "/${WINE_MODULE_ROOT}/wineserver"
check_zip_guest_link "${WINE_ARCHIVE}" usr/lib/wine/wineserver64 \
    "/${WINE_MODULE_ROOT}/wineserver64"
check_zip_guest_link "${WINE_ARCHIVE}" usr/lib/wine/x86_64-windows \
    "/${WINE_MODULE_ROOT}/x86_64-windows"
check_zip_guest_link "${WINE_ARCHIVE}" usr/lib/wine/x86_64-unix \
    "/${WINE_MODULE_ROOT}/x86_64-unix"
check_zip_guest_link "${WINE_ARCHIVE}" usr/bin/wine64 \
    "/${WINE_MODULE_ROOT}/wine64"
check_zip_guest_link "${WINE_ARCHIVE}" usr/bin/wineserver \
    "/${WINE_MODULE_ROOT}/wineserver"
if [[ "${MANIFEST_SOURCE}" == "scripts/build-wine64-runtime-ci.sh" ]]; then
    check_zip_path "${WINE_ARCHIVE}" \
        "${WINE_MODULE_ROOT}/x86_64-unix/winemetal.so"
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
        # A recorded manifest has to be one this checker would accept on the
        # next run, so it carries the WoW64 pins the block above requires.
        printf 'i386_windows_module_count=%s\n' "$(unzip -Z1 "${WINE_ARCHIVE}" \
            | sed 's#^\./##' \
            | awk -v prefix="${PE32_DIR}/" \
                'index($0, prefix) == 1 && $0 != prefix && substr($0, length($0)) != "/" { count++ } END { print count + 0 }')"
        printf 'i386_windows_ntdll_sha256=%s\n' \
            "$(zip_entry_sha256 "${WINE_ARCHIVE}" "${PE32_DIR}/ntdll.dll")"
        for recorded_wow64 in wow64 wow64win wow64cpu; do
            printf '%s_dll_sha256=%s\n' "${recorded_wow64}" \
                "$(zip_entry_sha256 "${WINE_ARCHIVE}" \
                    "${WINE_MODULE_ROOT}/x86_64-windows/${recorded_wow64}.dll")"
        done
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
