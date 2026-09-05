#!/usr/bin/env bash
# BoxedVN - assemble the x86-64 Linux/Wine64 layers on an Ubuntu runner.
#
# This is the CI counterpart of the audited container recipe. It deliberately
# consumes the runner's distro Wine package instead of downloading an opaque
# image: the package manager, runner image, and resulting archive hashes are
# recorded in the generated manifest. The output is suitable for the
# BoxedWine64 -root/-zip launch sequence.
#
# Usage:
#   scripts/build-wine64-runtime-ci.sh --output-dir DIR \
#       [--dxmt-unixlib PATH] [--x11-shim-dir DIR] \
#       [--vulkan-shim PATH] [--dxvk-i386-dir DIR] \
#       [--i386-pe-dir DIR] [--oss-driver-dir DIR]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

OUTPUT_DIR=""
DXMT_UNIXLIB=""
# The x86-64 X11 client libraries built by scripts/build-boxedwine-x64-x11.sh.
# Packaged under a directory of their own that a 64-bit launch places first
# on LD_LIBRARY_PATH; see K_X64_GUEST_X11_LIB_DIR in include/guest_wine64_layout.h.
X11_SHIM_DIR=""
X11_SHIM_GUEST_DIR="/usr/lib/boxedwine64-x11"
X11_SHIM_LIBRARIES=(
    libX11.so.6
    libXext.so.6
    libXrender.so.1
    libXrandr.so.2
    libXinerama.so.1
    libXi.so.6
    libXcursor.so.1
    libXfixes.so.3
    libXcomposite.so.1
    libXxf86vm.so.1
)
# The x86-64 guest Vulkan ICD built by scripts/build-boxedwine-x64-vulkan.sh.
# It rides in the same directory as the X11 shims because that directory is
# already first on the 64-bit lane's LD_LIBRARY_PATH, and a device run of a
# 32-bit Direct3D 9 program proves it is the first path Wine's loader tries
# for this soname. Without it the loader falls through to /lib/libvulkan.so.1,
# which is the IA-32 shim, is rejected for its ELF class, and leaves wined3d
# with no adapter at all.
VULKAN_SHIM=""
VULKAN_SHIM_SONAME="libvulkan.so.1"
# OpenGL has no equivalent and is not going to get one. This build defines no
# GL backend (docs/KNOWN_LIMITATIONS_IOS.md section 3), the host is Metal-only
# and the 64-bit X11 client libraries are a bridge to BoxedWine's built-in X
# server, which serves no GLX -- so there is nothing here to stage and the
# soname is named only so the check below can refuse a wrong one. The shim
# directory heads the guest's LD_LIBRARY_PATH, so a file of this name there is
# found ahead of the IA-32 lane's own shim at /lib/libGL.so.1 and would fail
# identically while looking like ours. See K_X64_GUEST_OPENGL_LIB_PATH in
# include/guest_wine64_layout.h.
OPENGL_SHIM_SONAME="libGL.so.1"
# The prebuilt 32-bit DXVK the app already ships (ios/app/Dxvk). It is staged
# under a directory of its own inside the 32-bit PE layer rather than over
# i386-windows/d3d9.dll, so a launch that does not ask for it gets Wine's own
# d3d9 and the lane's current behaviour cannot regress. The projection into
# syswow64 is the launcher's job and is gated on BOXEDVN_WOW64_D3D9=dxvk; see
# source/sdl/startupArgs.cpp.
DXVK_I386_DIR=""
DXVK_I386_MODULES=(d3d9.dll)
DXVK_I386_OPTIONAL_MODULES=(dxgi.dll d3d11.dll d3d10core.dll)
# The guest module root everything real is packaged under. Kept the same
# as K_X64_WINE_MODULE_ROOT in include/guest_wine64_layout.h, which is what
# the launch and the device preflight use.
WINE_MODULE_ROOT="/usr/lib/x86_64-linux-gnu/wine"
# Defined after the root it hangs off: the i386 DXVK staging directory.
DXVK_I386_GUEST_DIR="${WINE_MODULE_ROOT}/dxvk-i386"
# Wine's side-by-side assemblies, staged in a directory of their own.
#
# Wine ships no winsxs tree and wine.inf never mentions one: the tree in a real
# prefix is a by-product of wineboot's fake-DLL install, which calls
# register_fake_dll for every builtin it copies into the prefix and writes
# windows\winsxs\manifests\<arch>_<name>_<key>_<version>_<lang>_deadbeef.manifest
# for each module carrying an RT_MANIFEST resource NAMED WINE_MANIFEST
# (dlls/setupapi/fakedll.c). This lane never gets that pass -- the prefix's
# system32 is an in-memory projection of the packaged tree rather than a
# directory wineboot filled in, and a device run recorded system32 still empty
# after a wineboot that exited 0 -- so the winsxs half is missing with it.
#
# Nothing reports that. ntdll's lookup_winsxs is the FIRST thing
# lookup_assembly tries; an empty winsxs sends the loader on to the
# private-assembly probes beside the program, and when those miss too
# parse_depend_manifests fails the entire activation context with
# STATUS_SXS_CANT_GEN_ACTCTX. A program whose manifest requires
# Microsoft.Windows.Common-Controls 6.0 then runs with no activation context at
# all and reaches the version 5 common controls instead, which is a different
# set of structure sizes and behaviours from the ones it was built against.
#
# The staging is the same derivation from the same source of truth: the
# packaged modules' own WINE_MANIFEST resources. A hand-written tree would
# drift from the Wine build the layer carries; this one cannot.
WINSXS_GUEST_DIR="/usr/lib/boxedwine64-winsxs"
WINSXS_STAGER="${BOXEDVN_SCRIPT_DIR}/stage-wine64-sxs-assemblies.py"
# The assembly the gate is on. Every other assembly the tree registers rides
# along; this is the one whose absence has been seen to matter.
WINSXS_REQUIRED_ASSEMBLY="Microsoft.Windows.Common-Controls"
# The 32-bit PE builtin tree for new WoW64, extracted from the SAME libwine
# version's i386 package by the caller. Ubuntu's wine64 package ships only the
# 64-bit trees; the i386 package's i386-unix tree is the OLD WoW64 (it needs
# 32-bit Linux libraries) and is deliberately not consumed here -- only the
# i386-windows PE images are, which run in the CPU's compatibility mode inside
# the 64-bit Unix process.
I386_PE_DIR=""
I386_PE_GUEST_DIR="${WINE_MODULE_ROOT}/i386-windows"
# The 32-bit builtins a Windows program's import chain reaches before its own
# entry point runs, taken from a device run of a 32-bit Direct3D 9 probe: the
# loader switched to 32-bit mode at ntdll's WoW64 entry and resolved every one
# of these except zlib1.dll, which 32-bit wined3d imports. That one was in the
# amd64 package's x86_64-windows tree and not in the staged i386-windows tree,
# so the loader searched syswow64, system, windows and the program's own
# directory, found nothing, and ended the process with STATUS_DLL_NOT_FOUND
# (0xc0000135) -- which on screen is a program that never appears, not an
# error. Checked here so a tree that cannot run a 32-bit program fails the
# build instead of shipping.
#
# libgcc_s_dw2-1.dll joined the list from a later run of the same probe, which
# missed it in all four search directories right after mapping opengl32.dll and
# again after uxtheme.dll: Ubuntu builds Wine's i386 PE modules with mingw-w64
# and some of them import the shared i686 libgcc. That miss does not end the
# process the way zlib1's did -- the importing builtin fails to load and its
# caller carries on without Direct3D -- so nothing downstream reports it at
# all, which is the argument for catching it here. The workflow supplies the
# file from the i686 mingw gcc runtime when the i386 package lacks it, exactly
# as it does for zlib1.dll.
WOW64_LANE_PE32_MODULES=(
    ntdll.dll kernel32.dll kernelbase.dll advapi32.dll sechost.dll
    msvcrt.dll ucrtbase.dll gdi32.dll user32.dll win32u.dll
    opengl32.dll wined3d.dll d3d9.dll zlib1.dll libgcc_s_dw2-1.dll
)
# The two 32-bit builtins that carry a program from a Vulkan renderer to
# Wine's WoW64 unix-call dispatch: DXVK's d3d9 links vulkan-1.dll, which is a
# forwarder onto winevulkan.dll, which is what reaches x86_64-unix/winevulkan.so
# and from there the guest libvulkan.so.1 this runtime now stages. Whether the
# i386 package carries them has never been checked, and a missing one is not
# fatal to the lane -- Wine's own d3d9 does not need either -- so this is a
# warning that answers the question on the next CI run rather than a failure
# that could stop a build for a feature the launch has to opt into.
WOW64_LANE_PE32_VULKAN_MODULES=(vulkan-1.dll winevulkan.dll)
# Wine's OSS audio driver, built by scripts/build-wine64-oss-driver.sh from the
# same Wine version the amd64 package provides. See docs/PLAN_X64_AUDIO.md.
#
# Ubuntu does not build this driver: Wine's configure needs <sys/soundcard.h>,
# which Ubuntu does not ship, so the package carries winealsa and winepulse
# instead. Neither of those can work here -- winepulse wants a PulseAudio
# daemon on a unix socket, winealsa wants /dev/snd -- while wineoss talks to
# /dev/dsp and /dev/mixer with raw ioctls, and BoxedWine already emulates
# exactly those two devices for the IA-32 lane.
#
# The two halves are one unit. wineoss.drv (PE) reaches wineoss.so (ELF)
# through __wine_unix_call, which is a private, unversioned interface: a .so
# from a different Wine version paired with this mmdevapi.dll is undefined
# behaviour, not a degraded experience. So both halves are checked, and a
# half-present pair fails the build.
OSS_DRIVER_DIR=""
OSS_DRIVER_UNIX_NAME="wineoss.so"
OSS_DRIVER_PE_NAME="wineoss.drv"
# The audio modules the inventory reports on. The three unix drivers answer
# "what could the guest possibly talk to", and the PE modules answer "can the
# guest even ask" -- mmdevapi is what winmm, dsound and xaudio2 all reach
# audio through, and a tree without it has no audio path at all regardless of
# which driver is present.
AUDIO_UNIX_DRIVERS=(winealsa winepulse wineoss)
AUDIO_PE_MODULES=(mmdevapi.dll dsound.dll xaudio2_9.dll winmm.dll)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"
                       OUTPUT_DIR="$2"; shift 2 ;;
        --dxmt-unixlib) [[ $# -ge 2 ]] || die "--dxmt-unixlib needs a value"
                         DXMT_UNIXLIB="$2"; shift 2 ;;
        --x11-shim-dir) [[ $# -ge 2 ]] || die "--x11-shim-dir needs a value"
                         X11_SHIM_DIR="$2"; shift 2 ;;
        --vulkan-shim) [[ $# -ge 2 ]] || die "--vulkan-shim needs a value"
                        VULKAN_SHIM="$2"; shift 2 ;;
        --dxvk-i386-dir) [[ $# -ge 2 ]] || die "--dxvk-i386-dir needs a value"
                          DXVK_I386_DIR="$2"; shift 2 ;;
        --i386-pe-dir) [[ $# -ge 2 ]] || die "--i386-pe-dir needs a value"
                        I386_PE_DIR="$2"; shift 2 ;;
        --oss-driver-dir) [[ $# -ge 2 ]] || die "--oss-driver-dir needs a value"
                           OSS_DRIVER_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
# Required in CI: a runtime without the 32-bit PE tree has no WoW64 lane, and
# nothing downstream reports that as an error -- a 32-bit image simply fails to
# start. A local inspection build may still be assembled without it.
if [[ -z "${I386_PE_DIR}" ]]; then
    [[ "${CI:-}" != "true" ]] \
        || die "--i386-pe-dir is required in CI. Extract libwine:i386 of the same version as the installed amd64 libwine and pass its i386-windows tree."
    warn "No --i386-pe-dir: the runtime will carry no 32-bit PE builtins and cannot run a WoW64 process."
else
    [[ -d "${I386_PE_DIR}" ]] \
        || die "--i386-pe-dir '${I386_PE_DIR}' is not a directory."
fi
if [[ -n "${DXMT_UNIXLIB}" ]]; then
    require_file "${DXMT_UNIXLIB}" \
        "Build the x86-64 guest DXMT unixlib before assembling the runtime."
fi
[[ "$(uname -s)" == "Linux" ]] || die "This builder must run on Linux."
require_command find
require_command ldd
require_command readlink
require_command file
require_command od
require_command unzip
require_command zip
if [[ -n "${DXMT_UNIXLIB}" ]]; then
    file "${DXMT_UNIXLIB}" | grep -Eqi 'ELF 64-bit.*x86-64' \
        || die "'${DXMT_UNIXLIB}' is not an x86-64 ELF shared object."
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-wine64-stage.XXXXXX")"
# The 32-bit PE tree is staged apart and archived apart (wine64-pe32.zip).
# Shipping it inside wine64.zip regressed the 64-bit lane on device: the cube
# probe stalled in RegisterClass or the app died at explorer's second display
# in four of four runs, and the IPA grew to 400 MB. Until the WoW64 lane can
# run it, the tree is packaged and validated but not mounted by the app.
PE32_STAGE="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-wine64-pe32-stage.XXXXXX")"
cleanup() { rm -rf "${STAGE}" "${PE32_STAGE}"; }
trap cleanup EXIT

find_first() {
    local candidate
    for candidate in "$@"; do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

# Read the ELF header rather than trusting the file mode. The distro ships a
# 382-byte /bin/sh wrapper named `wineserver` that picks between wineserver64
# and wineserver32; packaging that as the guest's wineserver made a 64-bit
# process exec a script, which resolved through a 32-bit interpreter and left
# the guest with neither an x86-64 image nor a valid 32-bit state.
is_elf64_x86_64() {
    local path="$1"
    [[ -f "${path}" ]] || return 1
    local bytes
    read -r -a bytes <<< "$(od -An -tu1 -N20 "${path}" 2>/dev/null | tr '\n' ' ')"
    [[ ${#bytes[@]} -ge 20 ]] || return 1
    # \x7fELF
    [[ "${bytes[0]}" == 127 && "${bytes[1]}" == 69 ]] || return 1
    [[ "${bytes[2]}" == 76 && "${bytes[3]}" == 70 ]] || return 1
    # EI_CLASS == ELFCLASS64
    [[ "${bytes[4]}" == 2 ]] || return 1
    # e_machine == EM_X86_64 (62), little endian
    [[ "${bytes[18]}" == 62 && "${bytes[19]}" == 0 ]] || return 1
    return 0
}

# The PE counterpart of the ELF check above, and needed for the same reason: a
# name is not evidence of an architecture. Ubuntu's i386 libwine package also
# carries an i386-unix tree (the old WoW64), and a mis-pointed --i386-pe-dir
# would stage 64-bit images under i386-windows or 32-bit ones under
# x86_64-windows. Either shape passes every name check and leaves the guest
# unable to load the image at all.
#
# Prints the COFF machine as a decimal number, or nothing when the file is not
# a PE image. Always succeeds, so a caller under `set -e` can test the value.
pe_coff_machine() {
    local path="$1"
    [[ -f "${path}" ]] || return 0
    local bytes
    # -v is required: od otherwise folds the repeated zero-filled DOS-header
    # rows into a literal "*", which shifts every later array index and makes a
    # valid e_lfanew read as zero.
    read -r -a bytes <<< "$(od -An -tu1 -v -N4096 "${path}" 2>/dev/null | tr '\n' ' ')"
    [[ ${#bytes[@]} -ge 64 ]] || return 0
    # "MZ"
    [[ "${bytes[0]}" == 77 && "${bytes[1]}" == 90 ]] || return 0
    local lfanew=$(( bytes[60] + bytes[61] * 256 + bytes[62] * 65536 + bytes[63] * 16777216 ))
    (( lfanew > 0 && lfanew + 6 < ${#bytes[@]} )) || return 0
    # "PE\0\0"
    [[ "${bytes[lfanew]}" == 80 && "${bytes[lfanew+1]}" == 69 \
       && "${bytes[lfanew+2]}" == 0 && "${bytes[lfanew+3]}" == 0 ]] || return 0
    printf '%d\n' $(( bytes[lfanew+4] + bytes[lfanew+5] * 256 ))
    return 0
}

# 0x014c = IMAGE_FILE_MACHINE_I386, 0x8664 = IMAGE_FILE_MACHINE_AMD64.
require_pe_machine() {
    local path="$1" expected="$2" description="$3" actual
    actual="$(pe_coff_machine "${path}")"
    [[ -n "${actual}" ]] \
        || die "'${path}' is not a PE image, so it cannot be a ${description}."
    (( actual == expected )) \
        || die "'${path}' has COFF machine 0x$(printf '%04x' "${actual}"), expected 0x$(printf '%04x' "${expected}") for a ${description}."
}

find_first_elf64_x86_64() {
    local candidate
    for candidate in "$@"; do
        if is_elf64_x86_64 "${candidate}"; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

WINE64="$(find_first \
    /usr/lib/wine/wine64 \
    /usr/lib/x86_64-linux-gnu/wine/wine64 \
    "$(command -v wine64 2>/dev/null || true)" \
    "$(command -v wine 2>/dev/null || true)")" \
    || die "The Ubuntu Wine64 package is not installed or exposes no wine64 ELF."

WINE_ROOT="$(dirname "${WINE64}")"
WINE_UNIX=""
WINE_WINDOWS=""
for candidate in \
    "${WINE_ROOT}/x86_64-unix" \
    /usr/lib/x86_64-linux-gnu/wine/x86_64-unix \
    /usr/lib/wine/x86_64-unix; do
    if [[ -d "${candidate}" ]]; then WINE_UNIX="${candidate}"; break; fi
done
for candidate in \
    "${WINE_ROOT}/x86_64-windows" \
    /usr/lib/x86_64-linux-gnu/wine/x86_64-windows \
    /usr/lib/wine/x86_64-windows; do
    if [[ -d "${candidate}" ]]; then WINE_WINDOWS="${candidate}"; break; fi
done
[[ -n "${WINE_UNIX}" && -n "${WINE_WINDOWS}" ]] \
    || die "Wine64 module trees were not found beside '${WINE64}'."

# A generic `wineserver` candidate is accepted only when its own ELF header
# proves it is an x86-64 executable, never merely because it is executable.
WINE_SERVER="$(find_first_elf64_x86_64 \
    /usr/lib/wine/wineserver64 \
    /usr/lib/x86_64-linux-gnu/wine/wineserver64 \
    /usr/lib/wine/wineserver \
    /usr/lib/x86_64-linux-gnu/wine/wineserver)" \
    || die "The Ubuntu Wine package exposes no x86-64 ELF wineserver binary."
is_elf64_x86_64 "${WINE64}" \
    || die "'${WINE64}' is not an x86-64 ELF executable."

log "Wine64 loader: ${WINE64}"
log "Wine64 unix modules: ${WINE_UNIX}"

copy_abs() {
    local source="$1"
    [[ -e "${source}" ]] || return 0
    local real
    real="$(readlink -f "${source}")"
    [[ -f "${real}" ]] || return 0
    mkdir -p "${STAGE}$(dirname "${source}")"
    cp -aL "${real}" "${STAGE}${source}"
}

copy_as() {
    local source="$1" destination="$2" real
    [[ -e "${source}" ]] || die "Required Wine64 path '${source}' is missing."
    real="$(readlink -f "${source}")"
    [[ -f "${real}" ]] || die "Wine64 path '${source}' is not a regular file."
    mkdir -p "${STAGE}$(dirname "${destination}")"
    cp -aL "${real}" "${STAGE}${destination}"
}

# Resolve the complete native dependency closure of Wine's ELF entry points.
# This keeps the layer independent of the host filesystem after it is mounted
# into BoxedWine and catches both /lib and /usr/lib layouts used by Ubuntu.
SEEDS=("${WINE64}" "${WINE_SERVER}" /lib64/ld-linux-x86-64.so.2)
while IFS= read -r module; do SEEDS+=("${module}"); done < <(
    find "${WINE_UNIX}" -maxdepth 1 -type f -name '*.so' -print
)

# ---- Dynamically loaded libraries -----------------------------------------
#
# A library Wine reaches with dlopen never appears in the ELF DT_NEEDED closure
# below, so it has to be named. Doing that with a bare `cp` per library is what
# left FreeType out: nothing recorded which libraries the guest asks for by
# name, and nothing failed when one of them was missing.
#
# seed_dynamic_library does the three things every such library needs:
#
#   1. finds it in either multiarch directory, following the distro's symlink
#      to the real versioned file;
#   2. adds it to the ldd seed set, so its OWN dependency closure -- for
#      FreeType on Ubuntu 24.04 that means libpng, Brotli, bz2 and zlib -- is
#      resolved from the runner rather than guessed here;
#   3. stages it under BOTH /lib and /usr/lib multiarch paths.
#
# The third is not redundancy. On the runner /lib is a symlink to /usr/lib, so
# only one real path exists -- but a BoxedWine ZIP does not interpret POSIX
# symlinks, so inside the guest the two paths are unrelated. The device log
# shows Wine trying both in turn and failing on both, which is exactly what one
# staged copy at one of them would still produce.
#
# `required` means the build fails without it. A library the guest merely
# prefers is seeded as `optional` and its absence is silent, as before.
MULTIARCH_DIRS=(
    /lib/x86_64-linux-gnu
    /usr/lib/x86_64-linux-gnu
    /lib64
    /usr/lib64
)
DYNAMIC_SEEDS=()
seed_dynamic_library() {
    local soname="$1" requirement="${2:-optional}" dir found=""
    for dir in "${MULTIARCH_DIRS[@]}"; do
        if [[ -f "${dir}/${soname}" ]]; then found="${dir}/${soname}"; break; fi
    done
    if [[ -z "${found}" ]]; then
        if [[ "${requirement}" == "required" ]]; then
            die "The runner has no x86-64 '${soname}'. Install the Ubuntu amd64 runtime package that provides it before assembling the Wine64 layers."
        fi
        return 0
    fi
    if ! is_elf64_x86_64 "$(readlink -f "${found}")"; then
        die "'${found}' is not an x86-64 ELF shared object. Wine's Unix side cannot load a host-architecture or static build of it."
    fi
    SEEDS+=("${found}")
    DYNAMIC_SEEDS+=("${soname}")
    # Both multiarch paths, dereferenced: the archive has no symlinks to follow.
    copy_as "${found}" "/lib/x86_64-linux-gnu/${soname}"
    copy_as "${found}" "/usr/lib/x86_64-linux-gnu/${soname}"
}

# The X11 client libraries. libX11 and libXext are what winex11.so links
# against directly, so a runtime without them has no user driver at all and
# every CreateWindowEx fails with ERROR_INVALID_WINDOW_HANDLE -- there is no
# desktop window to parent to. Required for that reason.
for x11core in libX11.so.6 libXext.so.6; do
    seed_dynamic_library "${x11core}" required
done
# winex11.drv resolves these client libraries with dlopen. The Ubuntu job
# installs the corresponding runtime packages.
for x11lib in \
    libXrender.so.1 \
    libXcursor.so.1 \
    libXfixes.so.3 \
    libXcomposite.so.1 \
    libXi.so.6 \
    libXinerama.so.1 \
    libXxf86vm.so.1 \
    libXrandr.so.2; do
    seed_dynamic_library "${x11lib}"
done

# Wine's font support dlopens FreeType by soname. Without it win32u reports
# "Wine cannot find the FreeType font library" and the guest has no font
# backend at all -- which is the only explicit runtime error two device runs
# produced. REQUIRED: shipping the archive without it is what those runs did.
#
# This must be the x86-64 Linux shared library for Wine's Unix side. The
# repository also builds a static FreeType for the iOS host, which is a
# different artifact for a different architecture and cannot stand in for it.
seed_dynamic_library libfreetype.so.6 required
# FreeType's own optional backends. They are dlopened rather than linked on
# some builds, so seeding them keeps the closure complete either way; the ldd
# pass above resolves whatever this particular build really links against.
for fontlib in \
    libfontconfig.so.1 \
    libharfbuzz.so.0; do
    seed_dynamic_library "${fontlib}"
done

# Wine's X11 and user/environment helpers load NSS modules dynamically. They
# are not present in DT_NEEDED, but getpwuid/getaddrinfo can reach them before
# the first window is created. Include the small glibc NSS set explicitly.
for nsslib in \
    libnss_files.so.2 \
    libnss_dns.so.2 \
    libnss_compat.so.2 \
    libresolv.so.2; do
    seed_dynamic_library "${nsslib}"
done
LIBS=""
for seed in "${SEEDS[@]}"; do
    [[ -f "${seed}" ]] || continue
    while IFS= read -r library; do
        [[ -n "${library}" ]] && LIBS+="${library}"$'\n'
    done < <(ldd "${seed}" 2>/dev/null | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^[[:space:]]*\// && $1 ~ /^\// { print $1 }
    ')
done
while IFS= read -r library; do
    [[ -n "${library}" ]] && copy_abs "${library}"
done < <(printf '%s' "${LIBS}" | sort -u)

# Every dynamically loaded library that was seeded has to be in the stage at
# both guest paths, and has to still be an x86-64 ELF there. A silent miss is
# what shipped an archive with no font backend.
for soname in "${DYNAMIC_SEEDS[@]}"; do
    for guest_dir in /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu; do
        staged="${STAGE}${guest_dir}/${soname}"
        [[ -s "${staged}" ]] \
            || die "Dynamically loaded library '${soname}' was not staged at ${guest_dir}."
        is_elf64_x86_64 "${staged}" \
            || die "Staged '${guest_dir}/${soname}' is not an x86-64 ELF shared object."
    done
done
log "Dynamically loaded libraries staged: ${DYNAMIC_SEEDS[*]}"

ld_loader="$(printf '%s\n' "${LIBS}" | awk '/ld-linux-x86-64\.so\.2$/ { print; exit }')"
if [[ -n "${ld_loader}" ]]; then copy_abs "${ld_loader}"; fi
copy_abs /lib/x86_64-linux-gnu/libc.so.6
copy_abs /lib/x86_64-linux-gnu/libgcc_s.so.1
copy_abs /usr/lib/x86_64-linux-gnu/libgcc_s.so.1

# Wine derives its search paths from where its own files are, so the loader
# and the module trees have to agree. The loader used to be relocated to
# /usr/lib/wine while the module trees stayed under Ubuntu's canonical root:
# the directory Wine reads from /proc/self/exe named one place and ntdll.so's
# parent named another, and a device run reached Windows code and then failed
# with "could not load kernel32.dll" with kernel32.dll present in this archive
# the whole time.
#
# Everything real therefore goes under the canonical module root. /usr/lib/wine
# stays reachable through guest links below.
mkdir -p "${STAGE}${WINE_MODULE_ROOT}"
copy_as "${WINE64}" "${WINE_MODULE_ROOT}/wine64"
# Wine execs `wineserver` out of the loader's own directory. Both names have to
# be the same real x86-64 executable; the distro's /bin/sh wrapper is
# deliberately not packaged at either of them.
copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver64"
copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver"
cp -aL "${WINE_UNIX}" "${STAGE}${WINE_MODULE_ROOT}/"
cp -aL "${WINE_WINDOWS}" "${STAGE}${WINE_MODULE_ROOT}/"
if [[ -n "${DXMT_UNIXLIB}" ]]; then
    cp "${DXMT_UNIXLIB}" \
       "${STAGE}${WINE_MODULE_ROOT}/x86_64-unix/winemetal.so"
fi

# The 32-bit PE builtins, under the same module root as the two 64-bit trees.
# Wine appends the architecture directory to each module root it searches, so
# a tree anywhere else is invisible to the same derivation that already had to
# be fixed for x86_64-windows.
#
# Only the PE tree is copied. The i386 package's i386-unix tree is old WoW64 --
# ELF32 objects that need a 32-bit Linux loader and libraries the guest does
# not have -- and staging it would give Wine a second, unusable way to start a
# 32-bit process.
I386_PE_MODULE_COUNT=0
if [[ -n "${I386_PE_DIR}" ]]; then
    mkdir -p "${PE32_STAGE}${I386_PE_GUEST_DIR}"
    cp -aL "${I386_PE_DIR}/." "${PE32_STAGE}${I386_PE_GUEST_DIR}/"
    # Every module the lane binds to, reported together. One name at a time
    # would hide the shape of the gap: a tree missing only zlib1.dll came from
    # the right package and lost one module, while a tree missing ten of these
    # came from the wrong place entirely, and the fix is different.
    missing_pe32=()
    for required_pe32 in "${WOW64_LANE_PE32_MODULES[@]}"; do
        pe32_path="${PE32_STAGE}${I386_PE_GUEST_DIR}/${required_pe32}"
        [[ -s "${pe32_path}" ]] || missing_pe32+=("${required_pe32}")
    done
    if (( ${#missing_pe32[@]} > 0 )); then
        # Whether the same name exists in the 64-bit tree says where to look:
        # a module Wine built for x86-64 and not for i386 is a gap in the i386
        # package, not in this staging step. The two names Wine does not build
        # at all -- zlib1.dll and libgcc_s_dw2-1.dll, which its mingw-built PE
        # modules import -- are in neither tree and come from their own i686
        # mingw packages instead, so they are named apart from the rest.
        for absent_pe32 in "${missing_pe32[@]}"; do
            case "${absent_pe32}" in
                zlib1.dll|libgcc_s_dw2-1.dll)
                    warn "i386-windows/${absent_pe32} is missing: it is a mingw runtime DLL Wine's i386 PE modules import and neither Wine tree builds. Install the i686 mingw package that carries it (libz-mingw-w64 for zlib1.dll, gcc-mingw-w64-i686 for libgcc_s_dw2-1.dll) and copy it into the tree before staging."
                    ;;
                *)
                    if [[ -s "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${absent_pe32}" ]]; then
                        warn "i386-windows/${absent_pe32} is missing and x86_64-windows/${absent_pe32} is present: the i386 package did not carry it."
                    else
                        warn "i386-windows/${absent_pe32} is missing, and neither does the 64-bit tree carry it."
                    fi
                    ;;
            esac
        done
        die "The staged i386-windows tree is missing ${#missing_pe32[@]} of the ${#WOW64_LANE_PE32_MODULES[@]} modules a 32-bit Windows program's import chain reaches before its own Direct3D works (${missing_pe32[*]}). A 32-bit process that cannot resolve one of the builtins exits STATUS_DLL_NOT_FOUND (0xc0000135) with no message and no window; one that cannot resolve a mingw runtime DLL loses the builtin that imports it and runs on without Direct3D. Extract libwine:i386 of the same version as the installed amd64 libwine and stage its whole i386-windows tree, adding the mingw runtime DLLs named above."
    fi
    require_pe_machine "${PE32_STAGE}${I386_PE_GUEST_DIR}/ntdll.dll" 332 \
        "32-bit PE builtin"
    I386_PE_MODULE_COUNT="$(find "${PE32_STAGE}${I386_PE_GUEST_DIR}" -type f | wc -l | tr -d ' ')"
    (( I386_PE_MODULE_COUNT > 0 )) \
        || die "The staged i386-windows tree is empty."
    log "32-bit PE builtins packaged: ${I386_PE_GUEST_DIR} (${I386_PE_MODULE_COUNT})"
    # The Vulkan half of the 32-bit chain, reported and not enforced.
    for vulkan_pe32 in "${WOW64_LANE_PE32_VULKAN_MODULES[@]}"; do
        if [[ -s "${PE32_STAGE}${I386_PE_GUEST_DIR}/${vulkan_pe32}" ]]; then
            log "i386-windows/${vulkan_pe32}: present (a 32-bit Vulkan renderer can bind)"
        else
            warn "i386-windows/${vulkan_pe32} is missing: a 32-bit DXVK d3d9 cannot reach winevulkan and BOXEDVN_WOW64_D3D9=dxvk will fail to load."
        fi
    done
fi

# ---- Side-by-side assemblies ----------------------------------------------
#
# See WINSXS_GUEST_DIR above for why this exists at all. Each architecture is
# staged into the archive that carries its module tree: a manifest in wine64.zip
# whose assembly directory links into the i386 tree would dangle whenever the
# PE32 archive is not mounted, which is the same argument the loader links make.
# Both land at the same guest path, so the guest sees one merged tree.
require_file "${WINSXS_STAGER}" \
    "The side-by-side assembly stager is part of this repository; a checkout missing it cannot produce a prefix that activates common controls."
WINSXS_MANIFEST_SUBDIR="manifests"
stage_sxs_assemblies() {
    local pe_dir="$1" arch="$2" module_guest_dir="$3" stage_root="$4" label="$5"
    local target="${stage_root}${WINSXS_GUEST_DIR}"
    mkdir -p "${target}"
    # The stager fails when the tree registers no required assembly. That is a
    # build failure on purpose: the alternative is a runtime that looks
    # complete and silently gives every program the version 5 common controls.
    python3 "${WINSXS_STAGER}" \
        --pe-dir "${pe_dir}" \
        --arch "${arch}" \
        --module-guest-dir "${module_guest_dir}" \
        --stage-dir "${target}" \
        --require "${WINSXS_REQUIRED_ASSEMBLY}" >&2 \
        || die "Could not stage the ${label} side-by-side assemblies from '${pe_dir}'. Without them a prefix has no winsxs tree, ntdll's lookup_winsxs finds nothing, and every activation context that names an assembly fails with STATUS_SXS_CANT_GEN_ACTCTX -- which Wine reports as nothing at all."
    local count
    count="$(find "${target}/${WINSXS_MANIFEST_SUBDIR}" -type f -name '*.manifest' 2>/dev/null | wc -l | tr -d ' ')"
    (( count > 0 )) \
        || die "The staged ${label} side-by-side tree contains no manifests."
    printf '%s\n' "${count}"
}
# The stem Wine gives the assembly this gate is on. Built here rather than
# searched for by substring: the file name is <arch>_<name>_<key>_<version>_...
# lowercased, so an architecture-qualified prefix is what distinguishes the
# 64-bit registration from the 32-bit one.
WINSXS_COMMON_CONTROLS_PREFIX_64="amd64_$(printf '%s' "${WINSXS_REQUIRED_ASSEMBLY}" | tr 'A-Z' 'a-z')_"
WINSXS_COMMON_CONTROLS_PREFIX_32="x86_$(printf '%s' "${WINSXS_REQUIRED_ASSEMBLY}" | tr 'A-Z' 'a-z')_"
WINSXS_MANIFEST_COUNT_64="$(stage_sxs_assemblies \
    "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows" amd64 \
    "${WINE_MODULE_ROOT}/x86_64-windows" "${STAGE}" "64-bit")"
log "Side-by-side assemblies packaged: ${WINSXS_GUEST_DIR} (${WINSXS_MANIFEST_COUNT_64} amd64 manifests)"
WINSXS_MANIFEST_COUNT_32=0
if [[ -n "${I386_PE_DIR}" ]]; then
    WINSXS_MANIFEST_COUNT_32="$(stage_sxs_assemblies \
        "${PE32_STAGE}${I386_PE_GUEST_DIR}" x86 \
        "${I386_PE_GUEST_DIR}" "${PE32_STAGE}" "32-bit")"
    log "Side-by-side assemblies packaged: ${WINSXS_GUEST_DIR} (${WINSXS_MANIFEST_COUNT_32} x86 manifests)"
fi
# Named, not counted: the count alone would pass a tree that registered every
# assembly except the one a program's manifest actually requires.
find "${STAGE}${WINSXS_GUEST_DIR}/${WINSXS_MANIFEST_SUBDIR}" -maxdepth 1 -type f \
    -name "${WINSXS_COMMON_CONTROLS_PREFIX_64}*.manifest" -print -quit | grep -q . \
    || die "The staged 64-bit side-by-side tree has no ${WINSXS_REQUIRED_ASSEMBLY} manifest. A program whose application manifest requires version 6 of that assembly gets no activation context and loads the version 5 common controls instead -- a different set of structure sizes and control behaviours, and nothing in the guest reports the substitution."
if [[ -n "${I386_PE_DIR}" ]]; then
    find "${PE32_STAGE}${WINSXS_GUEST_DIR}/${WINSXS_MANIFEST_SUBDIR}" -maxdepth 1 \
        -type f -name "${WINSXS_COMMON_CONTROLS_PREFIX_32}*.manifest" -print -quit \
        | grep -q . \
        || die "The staged 32-bit side-by-side tree has no ${WINSXS_REQUIRED_ASSEMBLY} manifest. The WoW64 lane's ntdll builds its own activation contexts and searches the same winsxs directory for an x86 assembly; without one, a 32-bit program requiring version 6 gets version 5."
fi

# The prebuilt 32-bit DXVK, staged beside the i386 builtins rather than over
# them. Nothing projects it into a prefix unless the launch asks for it, so a
# runtime carrying it behaves exactly as one that does not until
# BOXEDVN_WOW64_D3D9=dxvk is set.
DXVK_I386_MODULE_COUNT=0
if [[ -n "${DXVK_I386_DIR}" ]]; then
    [[ -d "${DXVK_I386_DIR}" ]] || die "--dxvk-i386-dir '${DXVK_I386_DIR}' is not a directory."
    [[ -n "${I386_PE_DIR}" ]] \
        || die "--dxvk-i386-dir needs --i386-pe-dir: the DXVK override rides in the 32-bit PE layer."
    mkdir -p "${PE32_STAGE}${DXVK_I386_GUEST_DIR}"
    for dxvk_module in "${DXVK_I386_MODULES[@]}"; do
        [[ -s "${DXVK_I386_DIR}/${dxvk_module}" ]] \
            || die "The DXVK directory has no ${dxvk_module}: ${DXVK_I386_DIR}"
        cp "${DXVK_I386_DIR}/${dxvk_module}" "${PE32_STAGE}${DXVK_I386_GUEST_DIR}/${dxvk_module}"
        # 0x14c is IMAGE_FILE_MACHINE_I386. A 64-bit DXVK here would be
        # projected into syswow64 and silently refused by the 32-bit loader.
        require_pe_machine "${PE32_STAGE}${DXVK_I386_GUEST_DIR}/${dxvk_module}" 332 \
            "32-bit DXVK override"
        DXVK_I386_MODULE_COUNT=$((DXVK_I386_MODULE_COUNT + 1))
    done
    for dxvk_module in "${DXVK_I386_OPTIONAL_MODULES[@]}"; do
        if [[ -s "${DXVK_I386_DIR}/${dxvk_module}" ]]; then
            cp "${DXVK_I386_DIR}/${dxvk_module}" "${PE32_STAGE}${DXVK_I386_GUEST_DIR}/${dxvk_module}"
            require_pe_machine "${PE32_STAGE}${DXVK_I386_GUEST_DIR}/${dxvk_module}" 332 \
                "32-bit DXVK override"
            DXVK_I386_MODULE_COUNT=$((DXVK_I386_MODULE_COUNT + 1))
        fi
    done
    log "32-bit DXVK override packaged: ${DXVK_I386_GUEST_DIR} (${DXVK_I386_MODULE_COUNT})"
else
    warn "No --dxvk-i386-dir: BOXEDVN_WOW64_D3D9=dxvk will find nothing to project and the 32-bit lane keeps Wine's own d3d9."
fi

# BoxedWine's own x86-64 X11 client libraries. winex11.so links the distro
# libX11 by DT_NEEDED, and that library connects to /tmp/.X11-unix/X0, which
# BoxedWine does not serve: a device run showed the connect refused and every
# CreateWindowEx failing for want of a desktop window. These replacements keep
# the SONAMEs and exports the driver needs but reach BoxedWine's built-in X
# server through a private syscall. They go in a directory of their own that
# the 64-bit launch puts first on LD_LIBRARY_PATH; the distro libraries stay
# at their multiarch paths for anything that still wants them.
if [[ -n "${X11_SHIM_DIR}" ]]; then
    [[ -d "${X11_SHIM_DIR}" ]] || die "--x11-shim-dir '${X11_SHIM_DIR}' is not a directory."
    mkdir -p "${STAGE}${X11_SHIM_GUEST_DIR}"
    for shim in "${X11_SHIM_LIBRARIES[@]}"; do
        [[ -s "${X11_SHIM_DIR}/${shim}" ]] \
            || die "The x86-64 X11 shim directory has no ${shim}. Run scripts/build-boxedwine-x64-x11.sh first."
        is_elf64_x86_64 "${X11_SHIM_DIR}/${shim}" \
            || die "'${X11_SHIM_DIR}/${shim}' is not an x86-64 ELF shared object."
        cp "${X11_SHIM_DIR}/${shim}" "${STAGE}${X11_SHIM_GUEST_DIR}/${shim}"
    done
    log "BoxedWine x86-64 X11 client libraries packaged: ${X11_SHIM_GUEST_DIR} (${#X11_SHIM_LIBRARIES[@]})"
else
    warn "No --x11-shim-dir: the 64-bit user driver will bind to the distro libX11 and fail to connect."
fi

# BoxedWine's own x86-64 Vulkan ICD. Wine's winex11.drv dlopens the bare
# soname, so the guest's ld-linux decides; this directory is first on
# LD_LIBRARY_PATH for a 64-bit launch and is the first path a device run was
# observed trying. Without this file the search falls through to the IA-32
# shim in the root filesystem, whose ELF class the 64-bit loader rejects.
if [[ -n "${VULKAN_SHIM}" ]]; then
    require_file "${VULKAN_SHIM}" \
        "Build the x86-64 guest Vulkan ICD with scripts/build-boxedwine-x64-vulkan.sh first."
    is_elf64_x86_64 "${VULKAN_SHIM}" \
        || die "'${VULKAN_SHIM}' is not an x86-64 ELF shared object."
    mkdir -p "${STAGE}${X11_SHIM_GUEST_DIR}"
    cp "${VULKAN_SHIM}" "${STAGE}${X11_SHIM_GUEST_DIR}/${VULKAN_SHIM_SONAME}"
    log "BoxedWine x86-64 Vulkan ICD packaged: ${X11_SHIM_GUEST_DIR}/${VULKAN_SHIM_SONAME}"
else
    warn "No --vulkan-shim: the 64-bit lane has no Vulkan client library and wined3d/DXVK get no adapter."
fi

# The lane's OpenGL state, recorded rather than assumed. Nothing stages a GL
# client library and nothing is expected to, so absence is the state this logs
# -- but if some future change ever puts one here it has to be an x86-64
# object, for the reason given beside OPENGL_SHIM_SONAME above. A wrong-class
# file in this directory is not a degraded OpenGL; it is the same failure the
# IA-32 shim already produces, wearing our name.
opengl_client="${STAGE}${X11_SHIM_GUEST_DIR}/${OPENGL_SHIM_SONAME}"
if [[ -e "${opengl_client}" ]]; then
    is_elf64_x86_64 "${opengl_client}" \
        || die "'${opengl_client}' is not an x86-64 ELF shared object. This directory heads the guest's LD_LIBRARY_PATH, so this file is found before the IA-32 lane's own ${OPENGL_SHIM_SONAME} and is rejected for its ELF class exactly as that one is."
    log "x86-64 OpenGL client library packaged: ${X11_SHIM_GUEST_DIR}/${OPENGL_SHIM_SONAME}"
else
    log "No x86-64 OpenGL client library, which is this build's settled state: with no GL backend the 64-bit lane's Direct3D is DXMT or wined3d's Vulkan adapter, and wined3d has to be told so -- an unset renderer means its OpenGL adapter."
fi

# The builtin the loader reaches for first after the server handshake. If it is
# not here, or is not a PE image, the guest gets as far as Windows code and no
# further -- so fail the build rather than ship an archive that cannot load.
for required_builtin in ntdll.dll kernel32.dll kernelbase.dll; do
    builtin_path="${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${required_builtin}"
    [[ -s "${builtin_path}" ]] \
        || die "The Wine module tree has no ${required_builtin}: ${builtin_path}"
    [[ "$(head -c 2 "${builtin_path}")" == "MZ" ]] \
        || die "${required_builtin} is not a PE image."
done

# Wine's own WoW64 thunking layer. These are 64-bit builtins even though they
# exist to serve 32-bit code: ntdll loads wow64.dll to build the 32-bit
# process, wow64win.dll thunks the user/GDI syscalls into the 64-bit win32u,
# and wow64cpu.dll performs the mode transfer. Ubuntu's amd64 libwine already
# ships all three; a tree missing one has no 32-bit lane, and Wine reports that
# only as a failure to start the image.
for wow64_module in wow64.dll wow64win.dll wow64cpu.dll; do
    wow64_path="${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${wow64_module}"
    [[ -s "${wow64_path}" ]] \
        || die "The Wine 64-bit module tree has no ${wow64_module}: ${wow64_path}. Without Wine's WoW64 layer the runtime cannot run a 32-bit process at all."
    require_pe_machine "${wow64_path}" 34404 "64-bit WoW64 thunk module"
done
log "WoW64 thunk modules packaged: wow64.dll wow64win.dll wow64cpu.dll"

# The two trees must not be each other. A build that staged the 64-bit tree at
# i386-windows, or the i386 package's images at x86_64-windows, satisfies every
# name check above and leaves the guest with images it cannot map.
require_pe_machine "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/ntdll.dll" 34404 \
    "64-bit PE builtin"

# The X11 user driver, both halves. Wine's win32u loads the PE side out of the
# Windows module tree and that side dlopens the Unix side; without either one
# the process runs with no user driver, which is not an error Wine reports --
# it simply has no desktop window, and every window creation fails.
for driver_half in \
    "x86_64-windows/winex11.drv" \
    "x86_64-unix/winex11.so"; do
    driver_path="${STAGE}${WINE_MODULE_ROOT}/${driver_half}"
    [[ -s "${driver_path}" ]] \
        || die "The Wine module tree has no ${driver_half}. Install the distro package that provides Wine's X11 user driver before assembling the runtime."
done
is_elf64_x86_64 "${STAGE}${WINE_MODULE_ROOT}/x86_64-unix/winex11.so" \
    || die "The packaged winex11.so is not an x86-64 ELF shared object."
[[ "$(head -c 2 "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/winex11.drv")" == "MZ" ]] \
    || die "The packaged winex11.drv is not a PE image."
log "X11 user driver packaged: winex11.drv + winex11.so"

# ---------------------------------------------------------------------------
# Audio. See docs/PLAN_X64_AUDIO.md sections 5.0 and 5.2.
#
# Step one is a fact-finding step, not an assumption: report what the archive
# actually carries for every audio driver, by name, so "we think Ubuntu ships
# winealsa" becomes a recorded fact in the build log. Nothing checked any of
# this before -- the validator's required lists had no audio entry of any
# kind, and the builder failed a missing font backend but said nothing about
# a missing sound driver.
if [[ -n "${OSS_DRIVER_DIR}" ]]; then
    [[ -d "${OSS_DRIVER_DIR}" ]] \
        || die "--oss-driver-dir '${OSS_DRIVER_DIR}' is not a directory."
    oss_unix_src="${OSS_DRIVER_DIR}/${OSS_DRIVER_UNIX_NAME}"
    oss_pe_src="${OSS_DRIVER_DIR}/${OSS_DRIVER_PE_NAME}"
    # Named individually: a caller who built only one half has to be told
    # which one is missing, not that "the directory is wrong".
    [[ -s "${oss_unix_src}" ]] \
        || die "--oss-driver-dir '${OSS_DRIVER_DIR}' has no ${OSS_DRIVER_UNIX_NAME}. Run scripts/build-wine64-oss-driver.sh first; it builds both halves from the pinned Wine sources."
    [[ -s "${oss_pe_src}" ]] \
        || die "--oss-driver-dir '${OSS_DRIVER_DIR}' has no ${OSS_DRIVER_PE_NAME}. Run scripts/build-wine64-oss-driver.sh first; it builds both halves from the pinned Wine sources."
    is_elf64_x86_64 "${oss_unix_src}" \
        || die "'${oss_unix_src}' is not an x86-64 ELF shared object. The unix half of a Wine driver is an ELF .so built for the host architecture of the guest, which on this lane is x86-64."
    require_pe_machine "${oss_pe_src}" 34404 "64-bit Wine OSS user driver"
    cp "${oss_unix_src}" "${STAGE}${WINE_MODULE_ROOT}/x86_64-unix/${OSS_DRIVER_UNIX_NAME}"
    cp "${oss_pe_src}" "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${OSS_DRIVER_PE_NAME}"
    if [[ -n "${I386_PE_DIR}" ]]; then
        oss_pe32_src="${OSS_DRIVER_DIR}/i386-windows/${OSS_DRIVER_PE_NAME}"
        [[ -s "${oss_pe32_src}" ]] || die "WoW64 runtime requires the i386 OSS PE driver."
        require_pe_machine "${oss_pe32_src}" 332 "32-bit Wine OSS user driver"
        cp "${oss_pe32_src}" "${PE32_STAGE}${I386_PE_GUEST_DIR}/${OSS_DRIVER_PE_NAME}"
    fi
    log "Wine OSS audio driver packaged: ${OSS_DRIVER_PE_NAME} + ${OSS_DRIVER_UNIX_NAME}"
else
    warn "No --oss-driver-dir: the runtime carries no wineoss pair, so a 64-bit guest has no audio backend it can reach. See docs/PLAN_X64_AUDIO.md section 5.2."
fi

audio_drivers_present=0
audio_driver_summary=""
for audio_driver in "${AUDIO_UNIX_DRIVERS[@]}"; do
    audio_unix_path="${STAGE}${WINE_MODULE_ROOT}/x86_64-unix/${audio_driver}.so"
    audio_pe_path="${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${audio_driver}.drv"
    audio_unix_state="absent"
    audio_pe_state="absent"
    [[ -s "${audio_unix_path}" ]] && audio_unix_state="present"
    [[ -s "${audio_pe_path}" ]] && audio_pe_state="present"
    if [[ "${audio_unix_state}" == "present" && "${audio_pe_state}" == "present" ]]; then
        audio_status="ok"
        audio_drivers_present=$((audio_drivers_present + 1))
        audio_driver_summary="${audio_driver_summary}${audio_driver_summary:+,}${audio_driver}"
    elif [[ "${audio_unix_state}" == "absent" && "${audio_pe_state}" == "absent" ]]; then
        audio_status="absent"
    else
        audio_status="half"
    fi
    log "WINE64_AUDIO_DRIVER name=${audio_driver#wine} unix=${audio_unix_state} pe=${audio_pe_state} status=${audio_status}"
    if [[ "${audio_status}" == "half" ]]; then
        # Half a driver is always a packaging bug, because the PE half loads
        # the unix half by name through __wine_unix_call and gives up when it
        # cannot. For our own driver that is a build failure; for the distro's
        # it is a warning, because neither alsa nor pulse can work on this
        # lane anyway and stopping the build for one would cost a runtime
        # that is otherwise fine.
        if [[ "${audio_driver}" == "wineoss" ]]; then
            die "The staged tree has only one half of the OSS driver (unix=${audio_unix_state}, pe=${audio_pe_state}). wineoss.drv reaches wineoss.so through Wine's private __wine_unix_call boundary; half a pair is undefined behaviour, not a degraded driver."
        fi
        warn "The staged tree has only one half of ${audio_driver} (unix=${audio_unix_state}, pe=${audio_pe_state}). That driver cannot load."
    fi
done

audio_pe_summary=""
for audio_module in "${AUDIO_PE_MODULES[@]}"; do
    audio_module_state="absent"
    [[ -s "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${audio_module}" ]] \
        && audio_module_state="present"
    audio_pe_summary="${audio_pe_summary}${audio_pe_summary:+ }${audio_module%.dll}=${audio_module_state}"
done
log "WINE64_AUDIO_INVENTORY drivers=${audio_driver_summary:-none} count=${audio_drivers_present} ${audio_pe_summary}"

# mmdevapi is the one PE module that is not optional: winmm, dsound and
# xaudio2 all reach a device through CoCreateInstance(MMDeviceEnumerator),
# so a tree without it has no audio path at all no matter which driver is
# staged. A warning rather than a failure -- it is a core Wine builtin and
# has never been observed missing, and a runtime with no audio is still a
# runtime that runs.
if [[ ! -s "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/mmdevapi.dll" ]]; then
    warn "The staged Wine tree has no mmdevapi.dll. Nothing in the guest can enumerate an audio endpoint, so no audio driver can be reached."
fi
if [[ "${audio_drivers_present}" -eq 0 ]]; then
    warn "The staged Wine tree carries no complete audio driver pair. A 64-bit guest will find no audio endpoint."
fi

# Fontconfig's default configuration. Without /etc/fonts/fonts.conf the library
# loads and then reports "Cannot load default config file", which leaves Wine
# with no font backend even though libfreetype opened successfully.
#
# /etc/fonts/conf.d is a directory of symlinks into
# /usr/share/fontconfig/conf.avail. A BoxedWine ZIP does not interpret POSIX
# symlinks, so the tree is materialised with cp -aL: what lands in the archive
# is the real configuration files, not dangling links.
if [[ -d /etc/fonts ]]; then
    mkdir -p "${STAGE}/etc"
    cp -aL /etc/fonts "${STAGE}/etc/"
else
    die "The runner has no /etc/fonts. Install fontconfig-config before assembling the runtime."
fi
[[ -s "${STAGE}/etc/fonts/fonts.conf" ]] \
    || die "The staged fontconfig tree has no fonts.conf."
# Every configuration file has to be XML, not the text of a link target.
#
# A device run reported "out of memory" from fontconfig at lines 86 and 91 of
# fonts.conf and then "Cannot load config file". That particular failure was
# not a packaging fault -- it was the 64-bit kernel's getcwd returning the
# buffer pointer instead of the length, which made fontconfig's <glob>
# canonicalisation fail; see docs/HANDOFF_X64_FONTCONFIG_GETCWD.md.
#
# The checks below stay because they cover a different, equally silent way to
# produce the same message: /etc/fonts/conf.d is a directory of symlinks on the
# runner, and if any of them reached the archive as a small file containing its
# target path rather than the file it names, fontconfig would read that text,
# fail to parse it, and say exactly this.
#
# Checked here rather than assumed: a .conf that does not parse fails the
# build, and the count is logged so an empty conf.d is visible too.
require_command python3
staged_conf_total=0
while IFS= read -r conf_file; do
    staged_conf_total=$((staged_conf_total + 1))
    python3 - "${conf_file}" <<'FONTCONFIG_XML_CHECK' \
        || die "Staged fontconfig file '${conf_file}' is not parseable XML. A guest link's target text is not a configuration file."
import sys
import xml.etree.ElementTree as ET
path = sys.argv[1]
try:
    ET.parse(path)
except Exception as error:  # noqa: BLE001 - the message is the diagnostic
    sys.stderr.write("not XML: {}: {}\n".format(path, error))
    raise SystemExit(1)
FONTCONFIG_XML_CHECK
done < <(find "${STAGE}/etc/fonts" -type f -name '*.conf')
(( staged_conf_total > 0 )) \
    || die "The staged fontconfig tree contains no .conf files at all."
staged_confd_count="$(find "${STAGE}/etc/fonts/conf.d" -type f -name '*.conf' 2>/dev/null | wc -l | tr -d ' ')"
(( staged_confd_count > 0 )) \
    || die "The staged /etc/fonts/conf.d is empty. fonts.conf includes that directory, and an empty include is what a materialised symlink tree looks like when it did not materialise."
# No .link entries may exist under the configuration tree: BoxedWine reads that
# suffix as a guest symlink, and fontconfig would never see a file at all.
if find "${STAGE}/etc/fonts" -name '*.link' -print -quit | grep -q .; then
    die "The staged fontconfig tree contains guest link entries. Fontconfig cannot follow them; materialise the files instead."
fi
log "Fontconfig configuration staged: ${staged_conf_total} files, ${staged_confd_count} in conf.d"
if [[ -d /usr/share/fontconfig ]]; then
    mkdir -p "${STAGE}/usr/share"
    cp -aL /usr/share/fontconfig "${STAGE}/usr/share/"
fi
# A minimal scalable font set, so fontconfig's configuration resolves to
# something real rather than an empty set.
if [[ -d /usr/share/fonts/truetype ]]; then
    mkdir -p "${STAGE}/usr/share/fonts"
    cp -aL /usr/share/fonts/truetype "${STAGE}/usr/share/fonts/"
fi
# Fontconfig writes its cache under HOME. The directory has to exist or every
# lookup re-scans and re-fails.
mkdir -p "${STAGE}/home/username/.cache/fontconfig" "${STAGE}/var/cache/fontconfig"

if [[ -d /usr/share/wine ]]; then
    mkdir -p "${STAGE}/usr/share"
    cp -aL /usr/share/wine "${STAGE}/usr/share/"
fi
if [[ -d /usr/share/X11/locale ]]; then
    mkdir -p "${STAGE}/usr/share/X11"
    cp -aL /usr/share/X11/locale "${STAGE}/usr/share/X11/"
fi
# Do not stage /usr/share/locale wholesale. Ubuntu ships translation aliases
# whose optional targets are absent in the minimal runner image; following
# those aliases makes cp fail and the message catalogs are not needed for Wine
# startup. The actual glibc C.UTF-8 locale archive is copied below.
if [[ -d /usr/lib/locale/C.utf8 ]]; then
    mkdir -p "${STAGE}/usr/lib/locale"
    cp -aL /usr/lib/locale/C.utf8 "${STAGE}/usr/lib/locale/"
fi
if [[ -f /etc/localtime ]]; then
    mkdir -p "${STAGE}/etc"
    cp -aL /etc/localtime "${STAGE}/etc/localtime"
fi
mkdir -p "${STAGE}/etc"
# Keep identity and NSS selection deterministic. The runtime is mounted as a
# private guest root, so copying the runner's passwd/group database would make
# its host accounts part of the app resource.
printf '%s\n' \
    'root:x:0:0:root:/root:/bin/sh' \
    'username:x:1000:1000:username:/home/username:/bin/sh' \
    'nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin' \
    > "${STAGE}/etc/passwd"
printf '%s\n' \
    'root:x:0:' \
    'users:x:100:' \
    'username:x:1000:' \
    'nogroup:x:65534:' \
    > "${STAGE}/etc/group"
printf '%s\n' \
    'passwd: files' \
    'group: files' \
    'shadow: files' \
    'hosts: files dns' \
    'networks: files' \
    'protocols: files' \
    'services: files' \
    > "${STAGE}/etc/nsswitch.conf"
mkdir -p "${STAGE}/usr/bin" "${STAGE}/root"
# BoxedWine's ZIP filesystem does not read POSIX symlinks out of an archive.
# It recognises a link by a `.link` suffix on the entry name whose contents are
# the target path -- see EXT_LINK in source/io/fsnode.h. A real symlink stored
# in the ZIP becomes a small regular file holding the target text, which is
# what /usr/bin/wine64 and /usr/bin/wineserver silently were.
#
# No trailing newline: the guest reads the whole entry as the target.
guest_link() {
    local target="$1"
    local link="$2"
    mkdir -p "${STAGE}$(dirname "${link}")"
    printf '%s' "${target}" > "${STAGE}${link}.link"
}

# The same shape, written into the 32-bit archive's own stage. A link inside
# wine64.zip to a tree that ships in another archive would dangle whenever
# that archive is not mounted, which today is always.
pe32_guest_link() {
    local target="$1"
    local link="$2"
    mkdir -p "${PE32_STAGE}$(dirname "${link}")"
    printf '%s' "${target}" > "${PE32_STAGE}${link}.link"
}

# Compatibility for anything that still names the relocated layout. Nothing in
# a launch resolves through these; they exist so that naming them is not a
# silent failure.
guest_link "${WINE_MODULE_ROOT}/wine64" /usr/lib/wine/wine64
guest_link "${WINE_MODULE_ROOT}/wineserver" /usr/lib/wine/wineserver
guest_link "${WINE_MODULE_ROOT}/wineserver64" /usr/lib/wine/wineserver64
guest_link "${WINE_MODULE_ROOT}/x86_64-windows" /usr/lib/wine/x86_64-windows
guest_link "${WINE_MODULE_ROOT}/x86_64-unix" /usr/lib/wine/x86_64-unix
if [[ -n "${I386_PE_DIR}" ]]; then
    pe32_guest_link "${I386_PE_GUEST_DIR}" /usr/lib/wine/i386-windows
fi
guest_link "${WINE_MODULE_ROOT}/wine64" /usr/bin/wine64
guest_link "${WINE_MODULE_ROOT}/wineserver" /usr/bin/wineserver
# Wine derives its data root as ../../share/wine from the multiarch module
# directory. /usr/lib/x86_64-linux-gnu/wine/../../share/wine normalizes to
# /usr/lib/share/wine, while Ubuntu installs NLS tables in /usr/share/wine.
# Make the exact derived path guest-visible without duplicating the data.
guest_link /usr/share/wine /usr/lib/share/wine
# Wine's built-in bitmap fonts. The device log shows Wine opening
# /usr/lib/share/wine/fonts/*.fon -- the derived data path above -- and failing
# on every one, because the distro ships them in a separate package. They are
# what the non-client area of a window is drawn with.
[[ -d "${STAGE}/usr/share/wine/fonts" ]] \
    || die "The staged Wine data root has no fonts directory. Install fonts-wine before assembling the runtime."
staged_fon_count="$(find "${STAGE}/usr/share/wine/fonts" -type f -name '*.fon' | wc -l | tr -d ' ')"
(( staged_fon_count > 0 )) \
    || die "The staged Wine fonts directory contains no .fon files."
log "Wine bitmap fonts packaged: ${staged_fon_count}"

# These directories are created by wineserver and by Wine's default prefix.
mkdir -p "${STAGE}/tmp" "${STAGE}/run/user/1000" \
         "${STAGE}/home/username" "${STAGE}/var/tmp"

rm -f "${OUTPUT_DIR}/glibc-rootfs64.zip" "${OUTPUT_DIR}/wine64.zip" \
      "${OUTPUT_DIR}/wine64-pe32.zip" "${OUTPUT_DIR}/wine64-runtime.manifest"
( cd "${STAGE}" && zip -qry9 "${OUTPUT_DIR}/glibc-rootfs64.zip" \
      lib64 lib etc tmp run home var )
( cd "${STAGE}" && zip -qry9 "${OUTPUT_DIR}/wine64.zip" usr root )
if [[ -n "${I386_PE_DIR}" ]]; then
    ( cd "${PE32_STAGE}" && zip -qry9 "${OUTPUT_DIR}/wine64-pe32.zip" usr )
fi

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}
{
    printf '%s\n' '# BoxedVN Wine64 runtime manifest v1.'
    printf '%s\n' 'format=boxedvn-wine64-v1'
    printf '%s\n' 'source=scripts/build-wine64-runtime-ci.sh'
    printf '%s\n' 'source_image=ubuntu-24.04-apt'
    # Ubuntu's Wine64 is not being presented as an iOS-native Wine build.
    # Upstream Wine deliberately starts its 64-bit TEB allocation below 2 GiB
    # and publishes KUSER_SHARED_DATA at 0x7ffe0000.  The BoxedWine native
    # identity backend therefore has to provide its high-window allocator and
    # KUSER alias/fault bridge; recording this contract makes an accidental
    # stock-runtime acceptance visible to the validator and to device logs.
    printf '%s\n' 'wine_address_contract=stock-low-teb-hint-fixed-kuser-v1'
    printf '%s\n' 'boxedwine_bridge_required=K64_NATIVE_IDENTITY_KUSER_ALIAS'
    printf '%s\n' 'glibc_archive=glibc-rootfs64.zip'
    printf 'glibc_sha256=%s\n' "$(sha256_file "${OUTPUT_DIR}/glibc-rootfs64.zip")"
    printf '%s\n' 'wine_archive=wine64.zip'
    printf 'wine_sha256=%s\n' "$(sha256_file "${OUTPUT_DIR}/wine64.zip")"
    # The 32-bit PE tree ships apart, pinned the same way as the two layers.
    if [[ -f "${OUTPUT_DIR}/wine64-pe32.zip" ]]; then
        printf '%s\n' 'pe32_archive=wine64-pe32.zip'
        printf 'pe32_sha256=%s\n' "$(sha256_file "${OUTPUT_DIR}/wine64-pe32.zip")"
    fi
    # The WoW64 half of the layer, recorded so the validator can prove the
    # archive it is given is the one this build produced rather than a 64-bit
    # runtime with the same paths.
    printf 'i386_windows_module_count=%s\n' "${I386_PE_MODULE_COUNT}"
    if (( I386_PE_MODULE_COUNT > 0 )); then
        printf 'i386_windows_ntdll_sha256=%s\n' \
            "$(sha256_file "${PE32_STAGE}${I386_PE_GUEST_DIR}/ntdll.dll")"
    fi
    for wow64_module in wow64 wow64win wow64cpu; do
        printf '%s_dll_sha256=%s\n' "${wow64_module}" \
            "$(sha256_file "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/${wow64_module}.dll")"
    done
    # The side-by-side half, recorded so a runtime assembled before this
    # staging existed is distinguishable from one that has it. Zero here is
    # exactly the shape that gives every program the version 5 common
    # controls, and nothing downstream would otherwise say so.
    printf 'winsxs_manifests_amd64=%s\n' "${WINSXS_MANIFEST_COUNT_64}"
    printf 'winsxs_manifests_x86=%s\n' "${WINSXS_MANIFEST_COUNT_32}"
    if [[ -n "${DXMT_UNIXLIB}" ]]; then
        printf 'dxmt_unixlib_sha256=%s\n' "$(sha256_file "${DXMT_UNIXLIB}")"
    fi
    if [[ -n "${X11_SHIM_DIR}" ]]; then
        printf 'x11_shim_libx11_sha256=%s\n' "$(sha256_file "${X11_SHIM_DIR}/libX11.so.6")"
    fi
    if [[ -n "${VULKAN_SHIM}" ]]; then
        printf 'vulkan_shim_sha256=%s\n' "$(sha256_file "${VULKAN_SHIM}")"
    fi
    printf 'dxvk_i386_modules=%s\n' "${DXVK_I386_MODULE_COUNT}"
} > "${OUTPUT_DIR}/wine64-runtime.manifest"

ok "Wine64 runtime layers: ${OUTPUT_DIR}"
du -h "${OUTPUT_DIR}"/*.zip
