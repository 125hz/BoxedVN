#!/usr/bin/env bash
# BoxedVN - build the x86-64 guest Vulkan ICD for BoxedWine.
#
# Wine's winex11.drv dlopens SONAME_LIBVULKAN ("libvulkan.so.1") and binds
# sixteen symbols out of it by name; winevulkan then reaches everything else
# through vkGetInstanceProcAddr. The only file a 64-bit guest currently finds
# under that name is the IA-32 lane's shim in the Boxedwine root filesystem,
# an i386 ELF whose `int 0x9A` trap CPU64 does not decode -- the 64-bit loader
# opens it, rejects it for its ELF class, and walks past it, which is why
# wined3d has no Vulkan adapter and Direct3DCreate9 fails. This builds the
# x86-64 replacement, which reaches the host's MoltenVK through the private
# syscall in include/boxedwine_x64_vulkan_bridge.h.
#
# Runs on an x86-64 Linux builder with gcc. No Vulkan headers are needed: the
# shim passes structures through by pointer and never looks inside one, so
# only the widths of the Vulkan scalars matter and those are fixed by the ABI.
#
# Usage:
#   scripts/build-boxedwine-x64-vulkan.sh --output-dir DIR [--winex11 PATH]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

OUTPUT_DIR=""
WINEX11=""
CC="${CC:-gcc}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"
                      OUTPUT_DIR="$2"; shift 2 ;;
        --winex11)    [[ $# -ge 2 ]] || die "--winex11 needs a value"
                      WINEX11="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
require_command "${CC}" "Install gcc (an x86-64 Linux compiler) before building the guest Vulkan ICD."
require_command python3

SOURCE_DIR="${BOXEDVN_ROOT}/tools/vulkan-64"
INCLUDE_DIR="${BOXEDVN_ROOT}/include"
for required in vulkan.c winex11-vulkan-imports.txt; do
    require_file "${SOURCE_DIR}/${required}"
done
require_file "${INCLUDE_DIR}/boxedwine_x64_vulkan_bridge.h"

# The compiler has to produce x86-64 ELF for the guest. Fail here, by name,
# rather than let a build produce nothing and a device run report a missing
# library: a builder of another architecture needs a cross compiler named
# through CC.
if ! "${CC}" -dumpmachine 2>/dev/null | grep -q '^x86_64'; then
    die "'${CC}' does not target x86_64 ($("${CC}" -dumpmachine 2>/dev/null)). Set CC to an x86-64 Linux compiler before building the guest Vulkan ICD."
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"

SONAME="libvulkan.so.1"
COMMON_FLAGS=(-std=gnu11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror
              -Wno-unused-parameter -Wno-cast-function-type
              -I"${INCLUDE_DIR}")
LINK_FLAGS=(-shared -Wl,--no-undefined -Wl,-z,defs -Wl,-soname,"${SONAME}")

log "Building ${SONAME} (x86-64 guest Vulkan ICD)"
"${CC}" "${COMMON_FLAGS[@]}" "${LINK_FLAGS[@]}" "${SOURCE_DIR}/vulkan.c" \
    -o "${OUTPUT_DIR}/${SONAME}" \
    || die "Failed to build ${SONAME}."

validator_args=(--shim "${OUTPUT_DIR}/${SONAME}"
                --imports "${SOURCE_DIR}/winex11-vulkan-imports.txt"
                --bridge-header "${INCLUDE_DIR}/boxedwine_x64_vulkan_bridge.h")
if [[ -n "${WINEX11}" ]]; then
    require_file "${WINEX11}"
    validator_args+=(--winex11 "${WINEX11}")
fi
python3 "${BOXEDVN_ROOT}/scripts/validate-x64-vulkan-shim.py" "${validator_args[@]}" \
    || die "The built guest Vulkan ICD does not satisfy the winex11.drv import contract."

ok "x86-64 guest Vulkan ICD: ${OUTPUT_DIR}/${SONAME}"
ls -l "${OUTPUT_DIR}/${SONAME}"
