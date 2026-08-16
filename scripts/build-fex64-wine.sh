#!/usr/bin/env bash
# BoxedVN - configure and build Wine for the fex64 stack.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-wine.sh [--tree macos|arm64ec|all]
#                               [--third-party-dir DIR] [--jobs N] [--force]
#
# Stage B1 of docs/ARCHITECTURE_FEX64.md, and the gate for everything after it.
# Two trees, for different reasons:
#
#   macos    A normal macOS arm64 Wine. Not shipped, and not run. What it is
#            for is its *output*: config.h and the widl-generated headers that
#            the iOS unix side is compiled against. Wine's build system cannot
#            target iOS, so the iOS compile borrows this tree's idea of the
#            platform and overrides what differs.
#
#   arm64ec  The Windows side, built with llvm-mingw. This is what FEX
#            executes x86-64 against, and where xtajit64 plugs in.
#
# Neither recipe is published anywhere - not in Wine, not in the reference
# port's repository - so this is derived, and CI is what corrects it. Expect
# this script to change more than any other on the branch.
#
# macOS with a full Xcode only.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.fex64.lock.sh"

TREES="all"
FORCE=0
JOBS=""

usage() {
    sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tree)
            [[ $# -ge 2 ]] || die "--tree needs a value"
            TREES="$2"; shift 2 ;;
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs needs a value"
            JOBS="$2"; shift 2 ;;
        --force)
            FORCE=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

case "${TREES}" in
    all) TREES="macos arm64ec" ;;
    macos|arm64ec) ;;
    *) die "--tree must be macos, arm64ec or all, got '${TREES}'." ;;
esac

require_macos
require_command git
# Wine's build needs these and macOS ships neither at a usable version.
require_command bison "Install it with 'brew install bison' and put it first on PATH."
require_command flex "Install it with 'brew install flex' and put it first on PATH."

[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
SOURCE="${FEX64_DIR}/wine"
TOOLCHAINS="${FEX64_DIR}/toolchains"
MINGW="${TOOLCHAINS}/llvm-mingw-${BOXEDVN_LLVM_MINGW_VERSION}-ucrt-macos-universal"

if [[ ! -d "${SOURCE}/dlls" ]]; then
    log "Wine sources are missing; fetching"
    "${BOXEDVN_SCRIPT_DIR}/fetch-fex64-dependencies.sh" --component wine \
        || die "could not fetch Wine"
fi

print_tool_versions
log "Wine at $(git -C "${SOURCE}" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# On PATH for the whole script, not just for configure. Configure records the
# compiler it found and make invokes it by name later, so putting it on the
# path for the configure call alone produces a tree that configures cleanly and
# then cannot compile a single DLL.
if [[ -d "${MINGW}/bin" ]]; then
    export PATH="${MINGW}/bin:${PATH}"
fi

require_mingw() {
    [[ -x "${MINGW}/bin/aarch64-w64-mingw32-clang" ]] || die \
"llvm-mingw is missing at '${MINGW}'.
Run scripts/build-fex64-toolchains.sh --stage llvm-mingw first. Wine builds
its DLLs as PE binaries whatever the host, so no configuration here avoids
needing it."
}

# Wine refuses to configure in its source directory, and both trees have to
# coexist anyway.
build_tree() {
    local name="$1"
    shift
    local build="${SOURCE}/build-${name}"

    if [[ "${FORCE}" -eq 1 ]]; then
        rm -rf "${build}"
    elif [[ -f "${build}/include/config.h" ]]; then
        ok "wine/${name}: already configured"
        return 0
    fi

    mkdir -p "${build}"
    log "wine/${name}: configuring"
    if ! ( cd "${build}" && "${SOURCE}/configure" "$@" > configure.log 2>&1 ); then
        warn "wine/${name}: configure failed; last 40 lines"
        tail -40 "${build}/configure.log" >&2
        die "wine/${name}: configure failed. Full log: ${build}/configure.log"
    fi
    ok "wine/${name}: configured"
}

# Everything optional is off. This tree is a header generator, not a Wine
# anyone runs, and every dependency enabled here is one more thing to
# cross-build later for no benefit.
COMMON_CONFIGURE=(
    --disable-tests
    --without-alsa --without-capi --without-coreaudio --without-cups
    --without-dbus --without-fontconfig --without-freetype --without-gettext
    --without-gphoto --without-gnutls --without-gssapi --without-inotify
    --without-krb5 --without-netapi
    --without-opencl --without-opengl --without-oss
    --without-pcap --without-pcsclite --without-pthread --without-pulse
    --without-sane --without-sdl --without-udev --without-unwind
    --without-usb --without-v4l2 --without-vulkan --without-wayland
    --without-x --without-xcomposite --without-xcursor --without-xfixes
    --without-xinerama --without-xinput --without-xinput2 --without-xrandr
    --without-xrender --without-xshape --without-xshm --without-xxf86vm
)

for tree in ${TREES}; do
    case "${tree}" in
        macos)
            # Host build. --enable-win64 because the guest is x86-64 and the
            # generated headers have to describe a 64-bit world.
            #
            # This still needs llvm-mingw. Wine 11 builds its DLLs as PE
            # binaries on every host, so a cross-compiler is not optional even
            # for a tree whose only purpose is generating headers - configure
            # refuses outright without one.
            require_mingw
            PATH="${MINGW}/bin:${PATH}" \
                build_tree macos --enable-win64 --with-mingw \
                    "${COMMON_CONFIGURE[@]}"
            ;;
        arm64ec)
            require_mingw

            # ARM64EC is what makes the whole stack work: the Windows side is
            # ARM64 code that keeps the x86-64 ABI, so a call can cross between
            # emulated and native without a thunk layer in between. aarch64 is
            # built alongside it because Wine's own builtins need a pure ARM64
            # form as well.
            PATH="${MINGW}/bin:${PATH}" \
                build_tree arm64ec \
                    --enable-archs=arm64ec,aarch64 \
                    --with-mingw \
                    "${COMMON_CONFIGURE[@]}"
            ;;
    esac
done

# What each tree is built *for* decides how much of it to build.
#
# The macOS tree is a header generator, and building it all the way through
# fails on purpose-built iOS code: the fork's win32u references glue that lives
# in the application rather than in Wine, so win32u.so cannot link on a macOS
# host and never needs to. Building the include directory alone produces
# config.h and the widl-generated headers, which is the entire reason this tree
# exists.
#
# The ARM64EC tree is the Windows side that actually runs, so it is built in
# full.
make_target() {
    case "$1" in
        macos)   echo "include" ;;
        arm64ec) echo "" ;;
    esac
}

for tree in ${TREES}; do
    build="${SOURCE}/build-${tree}"
    target="$(make_target "${tree}")"
    log "wine/${tree}: building ${target:-the PE side} with ${JOBS} jobs"

    # -k for the ARM64EC tree, and a failure there is not fatal. That tree is
    # built for its PE DLLs; it also tries to build unix-side .so libraries for
    # the *host*, and those reference the same iOS glue that lives in the
    # application rather than in Wine, so win32u.so cannot link here and never
    # needs to. Stopping at the first such failure would throw away every PE
    # DLL that builds after it.
    #
    # What replaces the exit status as the test is the output itself, below.
    if [[ "${tree}" == "arm64ec" ]]; then
        make -C "${build}" -k -j "${JOBS}" > "${build}/build.log" 2>&1 || true
    elif ! make -C "${build}" -j "${JOBS}" ${target} > "${build}/build.log" 2>&1; then
        warn "wine/${tree}: build failed; last 40 lines"
        tail -40 "${build}/build.log" >&2
        die "wine/${tree}: build failed. Full log: ${build}/build.log"
    fi

    require_file "${build}/include/config.h"         "The tree built but produced no config.h, which is what the iOS unix side includes."

    if [[ "${tree}" == "arm64ec" ]]; then
        # The PE side is the deliverable, so count it and insist on the three
        # DLLs without which nothing starts at all.
        count="$(find "${build}/dlls" -path '*arm64ec-windows*' -name '*.dll' 2>/dev/null | wc -l | tr -d ' ')"
        log "wine/arm64ec: ${count} ARM64EC PE DLLs built"
        for required in ntdll kernel32 win32u; do
            find "${build}/dlls/${required}" -name "${required}.dll" 2>/dev/null                 | grep -q . || die "wine/arm64ec: ${required}.dll was not produced.
${count} other PE DLLs were, so this is that library specifically rather than a
broken tree. Its failure is in ${build}/build.log."
        done
        ok "wine/arm64ec: ${count} PE DLLs, including the three that must exist"
    else
        ok "wine/${tree}: built, config.h present"
    fi
done

log "Wine trees"
for tree in ${TREES}; do
    printf '  %-8s %s\n' "${tree}" "${SOURCE}/build-${tree}"
done
