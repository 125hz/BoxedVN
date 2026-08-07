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
