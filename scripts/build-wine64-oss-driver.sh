#!/usr/bin/env bash
# BoxedVN - build Wine's x86-64 OSS audio driver (wineoss.drv + wineoss.so).
#
# Ubuntu does not build this driver. Wine's configure gates dlls/wineoss.drv
# on an OSSv4 <sys/soundcard.h>, which Ubuntu does not ship by default, so the
# amd64 libwine package carries winealsa and winepulse instead -- and neither
# of those can work inside BoxedWine: winepulse wants a PulseAudio daemon on a
# unix socket and winealsa wants /dev/snd, and the guest kernel emulates
# neither. wineoss talks to /dev/dsp and /dev/mixer with raw ioctls, which
# BoxedWine has emulated for the IA-32 lane since forever, and now answers on
# the 64-bit lane too. See docs/PLAN_X64_AUDIO.md.
#
# The version has to match the installed amd64 libwine. Wine's PE/unix driver
# boundary (__wine_unix_call) is a private, unversioned interface: a
# wineoss.so from a different Wine version paired with the packaged
# mmdevapi.dll is undefined behaviour, not a degraded experience. Pass
# --wine-version with the UPSTREAM version of the installed package (Ubuntu's
# "9.0~repack-4build3" is upstream 9.0).
#
# Usage:
#   scripts/build-wine64-oss-driver.sh --output-dir DIR \
#       [--wine-version 9.0] [--wine-source DIR] [--jobs N] \
#       [--oss-include-dir DIR]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

OUTPUT_DIR=""
WINE_VERSION=""
WINE_SOURCE=""
JOBS=""
OSS_INCLUDE_DIR=""
usage() { sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   [[ $# -ge 2 ]] || die "--output-dir needs a value"
                        OUTPUT_DIR="$2"; shift 2 ;;
        --wine-version) [[ $# -ge 2 ]] || die "--wine-version needs a value"
                        WINE_VERSION="$2"; shift 2 ;;
        --wine-source)  [[ $# -ge 2 ]] || die "--wine-source needs a value"
                        WINE_SOURCE="$2"; shift 2 ;;
        --jobs)         [[ $# -ge 2 ]] || die "--jobs needs a value"
                        JOBS="$2"; shift 2 ;;
        --oss-include-dir)
                        [[ $# -ge 2 ]] || die "--oss-include-dir needs a value"
                        OSS_INCLUDE_DIR="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
[[ "$(uname -s)" == "Linux" ]] \
    || die "This builder must run on Linux: the unix half of a Wine driver is an ELF shared object for the guest's host architecture."
[[ -n "${WINE_VERSION}" || -n "${WINE_SOURCE}" ]] \
    || die "Pass --wine-version (the upstream version of the installed amd64 libwine) or --wine-source (an unpacked Wine tree of that version)."

require_command bison
require_command curl
require_command file
require_command flex
require_command gcc
require_command make
require_command tar
# Wine 9.0 builds its PE modules with a mingw cross compiler. Without one,
# configure still succeeds but produces a fake PE module built from the unix
# object, which is not what the guest's loader can map.
require_command x86_64-w64-mingw32-gcc \
    "Install it with 'sudo apt-get install -y gcc-mingw-w64-x86-64'; Wine builds its PE modules with a mingw cross compiler and silently degrades without one."

[[ -n "${JOBS}" ]] || JOBS="$(nproc 2>/dev/null || echo 2)"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

WORK="${BOXEDVN_THIRD_PARTY}/wine-oss"
mkdir -p "${WORK}"

# --- the OSS header ---------------------------------------------------------
#
# Wine needs an OSSv4 <sys/soundcard.h>. Two separate things are needed and
# they used to be conflated here:
#
#   1. the OSSv4 declarations themselves. The kernel's <linux/soundcard.h>
#      from linux-libc-dev is NOT them: it is OSSv3, it has the SNDCTL_DSP_*
#      ioctls and none of oss_sysinfo, oss_audioinfo, SNDCTL_SYSINFO,
#      SNDCTL_AUDIOINFO or SNDCTL_ENGINEINFO -- which are exactly the pieces
#      dlls/wineoss.drv/oss.c enumerates devices with. Ubuntu ships the real
#      OSSv4 header in the "oss4-dev" package (universe). That package
#      installs it AS /usr/include/linux/soundcard.h and dpkg-diverts the
#      kernel's copy to soundcard.h.oss3, so there is no file conflict with
#      linux-libc-dev and <linux/soundcard.h> simply becomes OSSv4.
#
#   2. the <sys/soundcard.h> spelling. glibc used to ship a one-line wrapper
#      and no longer does, so it is generated below.
#
# --oss-include-dir DIR overrides both for a host that has a real
# <sys/soundcard.h> of its own -- a *BSD, or an OSS4 installation under
# /usr/lib/oss/include, which is the path Wine's own configure probes.
#
# Either way the result is checked rather than assumed: a header that is not
# OSSv4 fails here, by name, instead of producing a driver that cannot
# enumerate a device.
if [[ -n "${OSS_INCLUDE_DIR}" ]]; then
    [[ -f "${OSS_INCLUDE_DIR}/sys/soundcard.h" ]] \
        || die "'${OSS_INCLUDE_DIR}' has no sys/soundcard.h. --oss-include-dir wants the directory that CONTAINS sys/soundcard.h, not the sys directory itself."
    OSS_INCLUDE="$(cd "${OSS_INCLUDE_DIR}" && pwd)"
else
    OSS_INCLUDE="${WORK}/oss-include"
    mkdir -p "${OSS_INCLUDE}/sys"
    cat > "${OSS_INCLUDE}/sys/soundcard.h" <<'EOF'
/* Generated by scripts/build-wine64-oss-driver.sh.
 *
 * glibc used to ship exactly this wrapper. Wine's configure gates
 * dlls/wineoss.drv on <sys/soundcard.h>; on a machine with the oss4-dev
 * package installed, <linux/soundcard.h> IS the OSSv4 header the driver
 * needs, because that package installs it there and diverts the kernel's
 * OSSv3 copy out of the way.
 */
#ifndef _BOXEDVN_SYS_SOUNDCARD_H
#define _BOXEDVN_SYS_SOUNDCARD_H
#include <linux/soundcard.h>
#endif
EOF
fi

log "Checking the OSSv4 declarations wineoss needs"
OSS_PROBE="${WORK}/oss-probe.c"
cat > "${OSS_PROBE}" <<'EOF'
#include <sys/soundcard.h>

/* The OSSv4 enumeration interface. oss_sysinfo.numaudioengines is the exact
 * member Wine's configure tests for, so a header that passes this probe and
 * then fails configure would be a Wine change, not a header surprise. */
static oss_sysinfo boxedvn_sysinfo;
static oss_audioinfo boxedvn_audioinfo;
static audio_buf_info boxedvn_bufinfo;
static count_info boxedvn_countinfo;

/* The MIDI and synth types dlls/wineoss.drv/ossmidi.c and midipatch.c are
 * written against. They are OSSv3-era, but a subset header that dropped them
 * would break the build long after this check. */
static struct synth_info boxedvn_synthinfo;
static struct midi_info boxedvn_midiinfo;
static struct sbi_instrument boxedvn_sbi;

/* unsigned, not int: on Linux __SIOR is _IOR, whose read-direction bit is
 * bit 31, so half of these do not fit in a signed int. */
unsigned int boxedvn_requests[] = {
    SNDCTL_SYSINFO, SNDCTL_AUDIOINFO, SNDCTL_ENGINEINFO,
    SNDCTL_DSP_SETFMT, SNDCTL_DSP_SPEED, SNDCTL_DSP_CHANNELS,
    SNDCTL_DSP_GETOSPACE, SNDCTL_DSP_GETISPACE, SNDCTL_DSP_GETFMTS,
    SNDCTL_DSP_SETFRAGMENT, SNDCTL_DSP_GETODELAY, SNDCTL_DSP_HALT,
    SNDCTL_SEQ_NRMIDIS, SNDCTL_SEQ_NRSYNTHS, SNDCTL_SEQ_RESET,
    SNDCTL_SYNTH_INFO, SNDCTL_MIDI_INFO,
    SOUND_MIXER_READ_LINE, SOUND_MIXER_WRITE_PCM,
};
int boxedvn_caps = PCM_CAP_OUTPUT | PCM_CAP_INPUT;
int boxedvn_formats = AFMT_S16_LE | AFMT_U8 | AFMT_FLOAT | AFMT_S32_LE | AFMT_S24_LE;
int boxedvn_engines = (int)sizeof(boxedvn_sysinfo.numaudioengines);
int boxedvn_devnode = (int)sizeof(boxedvn_audioinfo.devnode) + OSS_DEVNODE_SIZE;
int boxedvn_frags = (int)sizeof(boxedvn_bufinfo.fragsize)
                  + (int)sizeof(boxedvn_countinfo.bytes)
                  + (int)sizeof(boxedvn_synthinfo.synth_type)
                  + (int)sizeof(boxedvn_midiinfo.name)
                  + (int)sizeof(boxedvn_sbi.channel);
int main(void) { return 0; }
EOF
gcc -I"${OSS_INCLUDE}" -o "${WORK}/oss-probe" "${OSS_PROBE}" \
    || die "The <sys/soundcard.h> reachable through '${OSS_INCLUDE}' is not OSSv4: it is missing declarations wineoss needs (oss_sysinfo.numaudioengines, oss_audioinfo, SNDCTL_SYSINFO, SNDCTL_AUDIOINFO, SNDCTL_ENGINEINFO). The kernel's <linux/soundcard.h> from linux-libc-dev is OSSv3 and never has them. On Ubuntu/Debian run 'sudo apt-get install -y oss4-dev', which installs the OSSv4 header as /usr/include/linux/soundcard.h and diverts the kernel's copy; elsewhere pass --oss-include-dir with a directory that contains a real OSSv4 sys/soundcard.h."
ok "OSSv4 declarations available under ${OSS_INCLUDE}"

# --- the Wine source --------------------------------------------------------
if [[ -z "${WINE_SOURCE}" ]]; then
    WINE_SOURCE="${WORK}/wine-${WINE_VERSION}"
    if [[ ! -d "${WINE_SOURCE}" ]]; then
        tarball="${WORK}/wine-${WINE_VERSION}.tar.xz"
        # Upstream files a stable series under its own directory and the
        # development releases that follow it under <major>.x: wine-9.0.tar.xz
        # and wine-9.0.1.tar.xz are under source/9.0/, while wine-9.1.tar.xz is
        # under source/9.x/. Which one is right cannot be decided from the
        # version string alone, so try both before giving up. Asking only for
        # <major>.x is what this step failed on once the OSSv4 header check
        # started passing: a 404, and no driver.
        major="${WINE_VERSION%%.*}"
        minor="${WINE_VERSION#*.}"
        minor="${minor%%.*}"
        series_candidates=("${major}.${minor}" "${major}.x")
        if [[ ! -s "${tarball}" ]]; then
            log "Downloading Wine ${WINE_VERSION} sources"
            downloaded=0
            for series in "${series_candidates[@]}"; do
                url="https://dl.winehq.org/wine/source/${series}/wine-${WINE_VERSION}.tar.xz"
                if curl --fail --location --show-error --silent --output "${tarball}.partial" "${url}"; then
                    log "fetched ${url}"
                    downloaded=1
                    break
                fi
            done
            if [[ "${downloaded}" -ne 1 ]]; then
                die "Could not download Wine ${WINE_VERSION} from dl.winehq.org (tried ${series_candidates[*]}). The driver has to be built from the same version as the installed amd64 libwine; pass --wine-source if the tarball must come from elsewhere."
            fi
            mv "${tarball}.partial" "${tarball}"
        fi
        log "Unpacking Wine ${WINE_VERSION}"
        tar -xf "${tarball}" -C "${WORK}"
    fi
fi
[[ -d "${WINE_SOURCE}" ]] || die "Wine source directory '${WINE_SOURCE}' does not exist."
WINE_SOURCE="$(cd "${WINE_SOURCE}" && pwd)"
[[ -d "${WINE_SOURCE}/dlls/wineoss.drv" ]] \
    || die "'${WINE_SOURCE}' has no dlls/wineoss.drv. That directory is where Wine's OSS driver lives; this is not a Wine source tree, or it is a version that moved the driver."

# --- configure and build ----------------------------------------------------
#
# Only the one driver is built. Wine's make will build the tools it needs
# (winebuild, widl, ...) and the generated headers on the way, which is a few
# minutes rather than the half hour a full tree takes.
BUILD="${WORK}/build-${WINE_VERSION:-source}"
mkdir -p "${BUILD}"
if [[ ! -s "${BUILD}/Makefile" ]]; then
    log "Configuring Wine (x86_64 only)"
    (
        cd "${BUILD}"
        # CPPFLAGS, not OSS4_CFLAGS: configure AC_SUBSTs OSS4_CFLAGS itself
        # (to -I/usr/lib/oss/include) and would discard anything passed in.
        # CPPFLAGS is recorded in the generated Makefile and makedep emits it
        # on every compile line, unix and PE alike, so the shim stays
        # reachable for the whole build and not just for configure's probe.
        CPPFLAGS="${CPPFLAGS:+${CPPFLAGS} }-I${OSS_INCLUDE}" "${WINE_SOURCE}/configure" \
            --enable-archs=x86_64 \
            --disable-tests \
            --without-x \
            --without-freetype
    ) || die "Wine's configure failed. Its config.log is at ${BUILD}/config.log."
fi

# configure records whether it accepted the header. A build that got this far
# without it would produce nothing and say nothing, so check the recorded
# answer rather than the eventual absence of a file.
#
# The recorded answer is HAVE_OSS_SYSINFO_NUMAUDIOENGINES, not
# HAVE_SYS_SOUNDCARD_H. configure.ac reaches the header through
# AC_CHECK_HEADER([sys/soundcard.h], [ACTION-IF-FOUND]), and supplying an
# action REPLACES autoconf's default action -- which is the one that would
# have defined HAVE_SYS_SOUNDCARD_H. Wine 9.0's include/config.h.in has no
# such name at all, so grepping for it fails on every tree, header or no
# header. The member check inside that action is both the real OSSv4 gate
# (WINE_NOTICE_WITH turns enable_wineoss_drv off when it fails) and the only
# thing config.h records about OSS.
grep -q '^#define HAVE_OSS_SYSINFO_NUMAUDIOENGINES 1' "${BUILD}/include/config.h" \
    || die "Wine's configure did not accept the OSSv4 <sys/soundcard.h>, so dlls/wineoss.drv is disabled and will not be built. See ${BUILD}/config.log for the failing compile."
ok "configure accepted the OSSv4 <sys/soundcard.h>"

log "Building dlls/wineoss.drv"
# Name the two output files, not the directory. "make dlls/wineoss.drv" asks
# for a target that already exists as a directory in the build tree, so make
# reports "Nothing to be done" and the step then fails on a missing library:
# that is what happened on the first run where configure accepted OSSv4.
if ! make -C "${BUILD}" -j"${JOBS}" dlls/wineoss.drv/wineoss.so dlls/wineoss.drv/wineoss.drv; then
    die "Building dlls/wineoss.drv failed."
fi

# --- verify and stage -------------------------------------------------------
unix_half="${BUILD}/dlls/wineoss.drv/wineoss.so"
pe_half="${BUILD}/dlls/wineoss.drv/wineoss.drv"
[[ -s "${unix_half}" ]] \
    || die "The build produced no wineoss.so. Wine's unix half is what talks to /dev/dsp; without it the PE half has nothing to call."
[[ -s "${pe_half}" ]] \
    || die "The build produced no wineoss.drv. Wine's PE half is what mmdevapi loads; without it the unix half is unreachable."
file "${unix_half}" | grep -Eqi 'ELF 64-bit.*x86-64' \
    || die "'${unix_half}' is not an x86-64 ELF shared object."
[[ "$(head -c 2 "${pe_half}")" == "MZ" ]] \
    || die "'${pe_half}' is not a PE image. Wine builds a fake PE module when no mingw cross compiler is available, and the guest loader cannot map one."

cp "${unix_half}" "${OUTPUT_DIR}/wineoss.so"
cp "${pe_half}" "${OUTPUT_DIR}/wineoss.drv"
ok "wineoss.so  -> ${OUTPUT_DIR}/wineoss.so"
ok "wineoss.drv -> ${OUTPUT_DIR}/wineoss.drv"
log "Pass this directory to scripts/build-wine64-runtime-ci.sh with --oss-driver-dir"
