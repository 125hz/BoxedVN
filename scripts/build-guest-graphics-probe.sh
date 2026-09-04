#!/usr/bin/env bash
# Build the dependency-free IA-32 Direct3D acceptance probe.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT="${1:-${REPO_ROOT}/build/guest-probes/boxedvn-d3d9-cube.exe}"
CXX="${BOXEDVN_GUEST_CXX:-i686-w64-mingw32-g++}"
OBJDUMP="${BOXEDVN_GUEST_OBJDUMP:-i686-w64-mingw32-objdump}"

if ! command -v "${CXX}" >/dev/null 2>&1; then
    printf 'error: %s is required to build the IA-32 graphics probe.\n' "${CXX}" >&2
    printf 'On Debian/Ubuntu, install g++-mingw-w64-i686.\n' >&2
    exit 1
fi
if ! command -v "${OBJDUMP}" >/dev/null 2>&1; then
    printf 'error: %s is required to validate the graphics probe.\n' "${OBJDUMP}" >&2
    exit 1
fi

mkdir -p "$(dirname "${OUTPUT}")"
# --large-address-aware is not a nicety here; it is the probe's whole address
# space above two gigabytes.
#
# A 32-bit image running under Wine's WoW64 is allocated under a ceiling the
# image itself declares. ntdll's virtual_set_large_address_space() sets
# user_space_wow_limit to limit_2g - 1 for an ordinary PE and limit_4g - 1 for
# one carrying IMAGE_FILE_LARGE_ADDRESS_AWARE; wow64 turns that into
# default_zero_bits and every NtAllocateVirtualMemory the 32-bit half makes
# arrives at ntdll with it as limit_high. Without the flag the probe can never
# be handed an address above 0x80000000, whatever the emulator hosts.
#
# Below that ceiling Wine's two placement paths cannot be combined: map_view
# places a view either inside a range ntdll reserved (map_reserved_area) or
# outside every one of them (map_free_area, which probes MAP_FIXED_NOREPLACE
# and gets EEXIST inside a reservation, because a reservation is a real
# PROT_NONE mapping). So an ordinary image's largest possible single
# allocation is the longer of the free run in [0x10000, 0x68000000) and the one
# in [0x68000000, 0x7f000000) -- 1.62 GiB and 368 MiB. A device log shows this
# probe's renderer asking for 0x48d30000, 1.14 GiB, and being refused.
#
# With the flag the ceiling is four gigabytes, and [0x80000000, 0x100000000) is
# two contiguous gigabytes that no reservation covers and that BoxedVN's low
# lane already hosts. Nothing on the emulator side had to change to reach it.
#
# The flag is a statement about the program: it says no pointer is treated as
# signed and no high bit is used as a tag. That is true of this probe and is
# checked for at the byte below rather than assumed from the link line, because
# a driver that silently dropped the flag would put the ceiling back without
# changing anything visible.
"${CXX}" \
    -std=c++17 -Os -Wall -Wextra -Werror \
    -static-libgcc -static-libstdc++ -mwindows \
    -Wl,--large-address-aware \
    "${REPO_ROOT}/tools/guest-probes/d3d9_cube.cpp" \
    -o "${OUTPUT}" -ld3d9

if ! "${OBJDUMP}" -f "${OUTPUT}" | grep -q 'file format pei-i386'; then
    printf 'error: %s is not an IA-32 PE executable.\n' "${OUTPUT}" >&2
    exit 1
fi

# Read the COFF Characteristics field straight out of the file. objdump's
# rendering of it differs between binutils versions; the byte does not.
# e_lfanew lives at 0x3c, the PE signature at e_lfanew, and the 20-byte COFF
# header follows it, with Characteristics as its last two bytes.
read_le_at() {
    local offset="$1" count="$2" file="$3" hex reversed index
    hex="$(od -An -v -tx1 -j "${offset}" -N "${count}" "${file}" | tr -d ' \n')"
    if [ "${#hex}" -ne $(( count * 2 )) ]; then
        printf 'error: %s is too short to hold its PE headers.\n' "${file}" >&2
        exit 1
    fi
    reversed=""
    index=$(( ${#hex} - 2 ))
    while [ "${index}" -ge 0 ]; do
        reversed="${reversed}${hex:${index}:2}"
        index=$(( index - 2 ))
    done
    printf '%d' "$(( 0x${reversed} ))"
}

PE_HEADER_OFFSET="$(read_le_at 60 4 "${OUTPUT}")"
PE_SIGNATURE="$(od -An -v -tx1 -j "${PE_HEADER_OFFSET}" -N 4 "${OUTPUT}" \
    | tr -d ' \n')"
if [ "${PE_SIGNATURE}" != "50450000" ]; then
    printf 'error: %s has no PE signature at 0x%x.\n' \
        "${OUTPUT}" "${PE_HEADER_OFFSET}" >&2
    exit 1
fi
CHARACTERISTICS="$(read_le_at $(( PE_HEADER_OFFSET + 22 )) 2 "${OUTPUT}")"
if [ $(( CHARACTERISTICS & 0x20 )) -eq 0 ]; then
    printf 'error: %s is not marked IMAGE_FILE_LARGE_ADDRESS_AWARE ' \
        "${OUTPUT}" >&2
    printf '(Characteristics=0x%04x).\n' "${CHARACTERISTICS}" >&2
    printf 'Wine caps an unmarked 32-bit image at 2 GiB of address space, of\n' >&2
    printf 'which the largest single allocation it can ever obtain is the\n' >&2
    printf '1.62 GiB reserved area, and the renderer asks for more than that.\n' >&2
    printf 'Link with -Wl,--large-address-aware.\n' >&2
    exit 1
fi

# The probe is staged into the container's diagnostics folder as a single file
# and run by the guest Wine with nothing beside it, so every name in its import
# table has to be something Wine itself supplies. The mingw runtime DLLs are
# not: a build that lost -static-libgcc/-static-libstdc++, or a toolchain whose
# g++ driver reinstates -shared-libgcc, produces an executable that resolves
# every Windows import and then dies in the loader on libgcc_s_dw2-1.dll, which
# on screen is a program that never opened a window. The three names below are
# the whole i686 mingw runtime the driver can reach for; matching is on the
# stem so a differently versioned or SEH-flavoured copy is caught too.
IMPORTS="$("${OBJDUMP}" -p "${OUTPUT}" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p')"
FORBIDDEN_IMPORTS=""
while IFS= read -r imported; do
    [ -n "${imported}" ] || continue
    case "$(printf '%s' "${imported}" | tr '[:upper:]' '[:lower:]')" in
        libgcc*|libstdc++*|libwinpthread*)
            FORBIDDEN_IMPORTS="${FORBIDDEN_IMPORTS}${imported}"$'\n'
            ;;
    esac
done <<< "${IMPORTS}"
if [ -n "${FORBIDDEN_IMPORTS}" ]; then
    printf 'error: %s imports the mingw runtime, which Wine does not ship:\n' \
        "${OUTPUT}" >&2
    printf '  %s\n' ${FORBIDDEN_IMPORTS} >&2
    printf 'Link the probe with -static-libgcc -static-libstdc++ (and keep the\n' >&2
    printf 'sources free of C++ exceptions and threads) so it depends only on\n' >&2
    printf 'the DLLs Wine provides.\n' >&2
    exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${OUTPUT}" > "${OUTPUT}.sha256"
else
    shasum -a 256 "${OUTPUT}" > "${OUTPUT}.sha256"
fi

printf 'Built IA-32 Direct3D probe: %s\n' "${OUTPUT}"
cat "${OUTPUT}.sha256"
