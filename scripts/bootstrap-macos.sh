#!/usr/bin/env bash
# BoxedVN - check that a macOS machine can build the project.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage: scripts/bootstrap-macos.sh [--install]
#
# By default this only reports what is missing, so it is safe to run on CI and
# on a machine you would rather not have packages installed on.  With
# --install it uses Homebrew to install the missing build tools.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

INSTALL=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install) INSTALL=1; shift ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

require_macos
print_tool_versions

problems=0
report_problem() {
    printf '%serror%s %s\n' "${_bvn_red}" "${_bvn_reset}" "$1" >&2
    problems=$((problems + 1))
}

# --- Xcode ------------------------------------------------------------------
if ! command -v xcodebuild >/dev/null 2>&1; then
    report_problem "xcodebuild is not on PATH. Install Xcode from the App Store."
else
    developer_dir="$(xcode-select -p 2>/dev/null || true)"
    if [[ "${developer_dir}" == *CommandLineTools* ]]; then
        report_problem "xcode-select points at the Command Line Tools
('${developer_dir}'), which do not include the iPhoneOS SDK. Point it at a
full Xcode:
  sudo xcode-select -s /Applications/Xcode.app"
    fi
fi

if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    report_problem "The iPhoneOS SDK is not available. Install the iOS platform:
  xcodebuild -downloadPlatform iOS"
else
    ok "iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version)"
fi

# --- Build tools ------------------------------------------------------------
missing_formulae=()

check_tool() {
    local command_name="$1"
    local formula="$2"
    local minimum="${3:-}"
    if command -v "${command_name}" >/dev/null 2>&1; then
        ok "${command_name} present"
        return 0
    fi
    missing_formulae+=("${formula}")
    report_problem "'${command_name}' is missing${minimum:+ (need ${minimum} or newer)}."
    return 1
}

check_tool cmake cmake "3.24"
check_tool ninja ninja
check_tool git git

# CMake version gate: presets version 6 needs 3.24.
if command -v cmake >/dev/null 2>&1; then
    cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
    cmake_major="${cmake_version%%.*}"
    cmake_minor="$(echo "${cmake_version}" | cut -d. -f2)"
    if (( cmake_major < 3 || (cmake_major == 3 && cmake_minor < 24) )); then
        report_problem "cmake ${cmake_version} is too old; BoxedVN needs 3.24 or newer
for CMakePresets version 6."
    fi
fi

# --- Optional install -------------------------------------------------------
if [[ ${#missing_formulae[@]} -gt 0 ]]; then
    if [[ ${INSTALL} -eq 1 ]]; then
        require_command brew "Install Homebrew from https://brew.sh, or install
${missing_formulae[*]} another way."
        log "Installing: ${missing_formulae[*]}"
        brew install "${missing_formulae[@]}"
        problems=0
    else
        printf '\nTo install the missing tools:\n'
        printf '  brew install %s\n' "${missing_formulae[*]}"
        printf 'or re-run this script with --install.\n'
    fi
fi

# --- Disk space -------------------------------------------------------------
# A full build plus SDL2 plus a root filesystem archive needs headroom.
available_gb="$(df -g "${BOXEDVN_ROOT}" | awk 'NR==2 {print $4}')"
if [[ -n "${available_gb}" ]] && (( available_gb < 10 )); then
    warn "Only ${available_gb} GB free on the volume holding the repository.
A full build plus dependencies needs roughly 10 GB, and a bundled root
filesystem needs more."
else
    ok "${available_gb} GB free"
fi

printf '\n'
if [[ ${problems} -gt 0 ]]; then
    die "${problems} problem(s) found; the build will not succeed until they are fixed."
fi

ok "This machine can build BoxedVN."
printf '\nNext:\n'
printf '  scripts/fetch-dependencies.sh --platform ios\n'
printf '  scripts/fetch-rootfs.sh          # optional, see docs/BUILD_IOS.md\n'
printf '  scripts/build-ios.sh\n'
