#!/usr/bin/env bash
# BoxedVN - build the x86-64 guest X11 client libraries for BoxedWine.
#
# Wine's winex11.so links libX11.so.6 and libXext.so.6 and dlopens the
# extension libraries by name. The distro copies speak the X wire protocol
# and try to reach /tmp/.X11-unix/X0, which BoxedWine does not provide. These
# replacements keep the same SONAMEs and export the same symbols, but reach
# BoxedWine's built-in X server through the private syscall in
# include/boxedwine_x64_x11_bridge.h.
#
# Runs on an x86-64 Linux builder with gcc and the X11 development headers;
# the layout assertions in tools/x11-64/layout_check.c are compiled against
# exactly those headers, so a header revision that moved a field fails here
# rather than on a device.
#
# Usage:
#   scripts/build-boxedwine-x64-x11.sh --output-dir DIR [--winex11 PATH]

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
            sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
require_command "${CC}" "Install gcc (an x86-64 Linux compiler) before building the guest X11 shim."
require_command python3

SOURCE_DIR="${BOXEDVN_ROOT}/tools/x11-64"
INCLUDE_DIR="${BOXEDVN_ROOT}/include"
for required in X11.c X11ext.c extension_stub.c xrandr.c layout_check.c winex11-imports.txt; do
    require_file "${SOURCE_DIR}/${required}"
done
require_file "${INCLUDE_DIR}/boxedwine_x64_x11_bridge.h"

# The compiler has to produce x86-64 ELF for the guest; a builder of another
# architecture would need a cross compiler named through CC.
if ! "${CC}" -dumpmachine 2>/dev/null | grep -q '^x86_64'; then
    die "'${CC}' does not target x86_64 ($("${CC}" -dumpmachine 2>/dev/null)). Set CC to an x86-64 Linux compiler."
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-x64-x11.XXXXXX")"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

COMMON_FLAGS=(-std=gnu11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror
              -Wno-unused-parameter -I"${INCLUDE_DIR}")
LINK_FLAGS=(-shared -Wl,--no-undefined -Wl,-z,defs)

# The layout assertions come first: nothing below is worth linking if the
# headers disagree with the host's picture of the structures.
log "Checking x86-64 Xlib layouts against the system headers"
"${CC}" "${COMMON_FLAGS[@]}" -c "${SOURCE_DIR}/layout_check.c" -o "${WORK}/layout_check.o" \
    || die "The X11 headers on this builder do not match the layouts the bridge writes."

build_library() {
    local soname="$1" output="$2"
    shift 2
    log "Building ${soname}"
    "${CC}" "${COMMON_FLAGS[@]}" "${LINK_FLAGS[@]}" -Wl,-soname,"${soname}" "$@" \
        -o "${OUTPUT_DIR}/${output}" \
        || die "Failed to build ${soname}."
}

build_library libX11.so.6 libX11.so.6 "${SOURCE_DIR}/X11.c" -lpthread
build_library libXext.so.6 libXext.so.6 "${SOURCE_DIR}/X11ext.c"
build_library libXrender.so.1 libXrender.so.1 -DBW_STUB_LIBRARY=1 "${SOURCE_DIR}/extension_stub.c"
# Not a stub: RandR is what decides whether winex11 reports one display mode
# or a list. See tools/x11-64/xrandr.c.
build_library libXrandr.so.2 libXrandr.so.2 "${SOURCE_DIR}/xrandr.c" -lpthread
build_library libXinerama.so.1 libXinerama.so.1 -DBW_STUB_LIBRARY=3 "${SOURCE_DIR}/extension_stub.c"
build_library libXi.so.6 libXi.so.6 -DBW_STUB_LIBRARY=4 "${SOURCE_DIR}/extension_stub.c"
build_library libXcursor.so.1 libXcursor.so.1 -DBW_STUB_LIBRARY=5 "${SOURCE_DIR}/extension_stub.c"
build_library libXfixes.so.3 libXfixes.so.3 -DBW_STUB_LIBRARY=6 "${SOURCE_DIR}/extension_stub.c"
build_library libXcomposite.so.1 libXcomposite.so.1 -DBW_STUB_LIBRARY=7 "${SOURCE_DIR}/extension_stub.c"
build_library libXxf86vm.so.1 libXxf86vm.so.1 -DBW_STUB_LIBRARY=8 "${SOURCE_DIR}/extension_stub.c"

validator_args=(--shim-dir "${OUTPUT_DIR}" --imports "${SOURCE_DIR}/winex11-imports.txt")
if [[ -n "${WINEX11}" ]]; then
    require_file "${WINEX11}"
    validator_args+=(--winex11 "${WINEX11}")
fi
python3 "${BOXEDVN_ROOT}/scripts/validate-x64-x11-shim.py" "${validator_args[@]}" \
    || die "The built guest X11 libraries do not satisfy the winex11.so import contract."

ok "x86-64 guest X11 libraries: ${OUTPUT_DIR}"
ls -l "${OUTPUT_DIR}"
