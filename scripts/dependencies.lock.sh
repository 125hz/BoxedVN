#!/usr/bin/env bash
# BoxedVN - pinned third-party dependencies.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Every entry pins an exact version, an exact URL and an exact SHA-256.  No
# script in this repository may download anything that is not listed here.
# See THIRD_PARTY_NOTICES.md for the licence of each dependency.

# --- SDL2 -------------------------------------------------------------------
# Boxedwine targets the SDL2 API.  The tree vendors SDL 2.0.12 headers in
# lib/sdl2 and upstream's macOS build links a prebuilt SDL2 2.0.14 framework,
# but neither is usable on iOS 17+: 2.0.12's UIKit backend predates the modern
# scene lifecycle and Metal renderer.  BoxedVN therefore builds the final SDL2
# release series from source for each target.  SDL2 is zlib-licensed, which is
# GPLv2-compatible.
BOXEDVN_SDL2_VERSION="2.32.10"
BOXEDVN_SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz"
BOXEDVN_SDL2_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"

# --- XcodeGen ---------------------------------------------------------------
# Generates the SwiftUI application shell from ios/project.yml so that no
# .pbxproj is ever hand-edited.  MIT licensed.
BOXEDVN_XCODEGEN_VERSION="2.46.0"
BOXEDVN_XCODEGEN_URL="https://github.com/yonaskolb/XcodeGen/releases/download/2.46.0/xcodegen.zip"
BOXEDVN_XCODEGEN_SHA256="4d9e34b62172d645eed6457cac13fc222569974098ef4ee9c3368bedf0196806"

# --- MoltenVK ---------------------------------------------------------------
# Boxedwine forwards guest Vulkan to the host. MoltenVK implements that host
# Vulkan API over Metal on iOS. Use Khronos's official static arm64 package so
# no dynamic framework needs to be embedded or separately signed.
BOXEDVN_MOLTENVK_VERSION="1.4.2"
BOXEDVN_MOLTENVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar"
BOXEDVN_MOLTENVK_SHA256="b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1"

# --- Boxedwine root filesystems ---------------------------------------------
# Boxedwine runs a real 32-bit Wine inside an emulated Linux and needs a root
# filesystem to do it.  These are upstream Boxedwine's own TinyCore + Wine
# builds, taken from the versioned catalogue at
# http://www.boxedwine.org/v2/26R2/filesV2.xml on 2026-08-07 and pinned by
# exact URL and SHA-256 here.
#
# BoxedVN does NOT redistribute these.  scripts/fetch-rootfs.sh downloads one
# on demand, and no public release should bundle one until the contents have
# been licence-reviewed; see docs/BUILD_IOS.md.
#
# Each entry is "<id>|<url>|<sha256>|<bytes>|<description>".
BOXEDVN_ROOTFS_DEFAULT="wine11"

BOXEDVN_ROOTFS_ENTRIES=(
  "wine10|https://boxedwine.org/v2/5/TinyCore15Wine10.0.zip|f0ed13eaf0c11bc95b229e2a747f04167c4e63445dc274d122758bd7e84b5572|156582181|TinyCore 15 with Wine 10.0 (legacy fallback; Song of Saya hangs on this Wine release)"
  "wine11|https://boxedwine.org/v2/7/TinyCore15Wine11.0.zip|41835c49ce0e582a1d7a610243a8e4a95bc59996fb780c09705dc476bb9b6493|162748254|TinyCore 15 with Wine 11.0 (default; includes Wine's Song of Saya critical-section fix)"
)
