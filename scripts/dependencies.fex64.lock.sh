#!/usr/bin/env bash
# BoxedVN - pinned third-party sources for the fex64 branch.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# These pins are the iOS forks used by the Mythic project
# (github.com/willfaust/mythic), at the exact commits its submodules record.
# Upstream FEX, Wine and DXMT do not build for iOS; each of these carries the
# port. Using them is the difference between joining a working stack and
# repeating five months of it.
#
# Pinned by commit, not by branch: every one of these branches is under active
# development and moves weekly.
#
# LICENCES.  The three forks inherit their upstreams, and all combine with
# BoxedVN's GPLv2:
#   FEX    MIT
#   Wine   LGPL-2.1-or-later
#   DXMT   LGPL-2.1-or-later
# The Mythic repository *itself* carries no LICENSE file, so its application
# layer (the SwiftUI shell, JITAllocator.c, FEXBridge.mm, the WineServer and
# WineProcess bridges) is all-rights-reserved and must not be copied into this
# tree. BoxedVN supplies its own; see docs/ARCHITECTURE_FEX64.md.
# THIRD_PARTY_NOTICES.md must be updated before anything built from these is
# distributed.

# --- FEX --------------------------------------------------------------------
# x86-64 -> ARM64 translation, entered through ARM64EC's xtajit64 interface
# rather than as a Linux usermode emulator. The fork's iOS work is on
# ios-port-2607; this commit is what Mythic's submodule records.
BOXEDVN_FEX_REPOSITORY="https://github.com/willfaust/FEX.git"
BOXEDVN_FEX_BRANCH="ios-port-2607"
BOXEDVN_FEX_COMMIT="04cbb90c715519136da771af3cc8f1dac9b821a6"

# --- Wine -------------------------------------------------------------------
# Wine 11.4 with the iOS port: ARM64EC PE side, unix side cross-compiled for
# iphoneos, and wineserver running as a thread inside the app rather than as a
# process. That last one is the change that makes any of this possible on a
# system with no fork and no exec.
BOXEDVN_WINE_REPOSITORY="https://github.com/willfaust/wine.git"
BOXEDVN_WINE_BRANCH="ios-build"
BOXEDVN_WINE_COMMIT="78497aaf2735ae2dbd6b285cdf2b562f333fc8a9"

# --- DXMT -------------------------------------------------------------------
# D3D11/D3D10 -> Metal. The fork replaces Cocoa with UIKit, stubs the macOS
# display APIs that do not exist on iOS, and renames its unix-call table so it
# does not collide with ntdll's when everything is linked into one binary.
BOXEDVN_DXMT_REPOSITORY="https://github.com/willfaust/dxmt.git"
BOXEDVN_DXMT_BRANCH="ios-port"
BOXEDVN_DXMT_COMMIT="d6bd546dc685189f4434f87fd15ea7b21009e64f"

# --- llvm-mingw -------------------------------------------------------------
# Builds DXMT's PE side (d3d11.dll, dxgi.dll, winemetal.dll, d3d10core.dll)
# for aarch64-windows. Prebuilt macOS universal toolchain; no source build.
BOXEDVN_LLVM_MINGW_VERSION="20260421"
BOXEDVN_LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260421/llvm-mingw-20260421-ucrt-macos-universal.tar.xz"

# --- LLVM -------------------------------------------------------------------
# DXMT's airconv translates DXBC to Metal AIR through LLVM, so LLVM itself has
# to be cross-built for iphoneos. This is the longest single item in the build
# and the first thing that must be cached in CI.
BOXEDVN_LLVM_VERSION="15.0.7"
BOXEDVN_LLVM_REPOSITORY="https://github.com/llvm/llvm-project.git"
BOXEDVN_LLVM_TAG="llvmorg-15.0.7"

# --- FreeType ---------------------------------------------------------------
# Static, for win32u's unix side. Wine normally dlopens it; on iOS it is
# linked in and reached through a generated symbol table.
BOXEDVN_FREETYPE_VERSION="2.13.3"
BOXEDVN_FREETYPE_REPOSITORY="https://github.com/freetype/freetype.git"
BOXEDVN_FREETYPE_TAG="VER-2-13-3"

# --- GnuTLS stack -----------------------------------------------------------
# GMP -> nettle/hogweed -> GnuTLS, static, for Wine's bcrypt, secur32 and
# crypt32 unix sides. Only needed once anything wants TLS.
BOXEDVN_GMP_VERSION="6.3.0"
BOXEDVN_NETTLE_VERSION="3.10.1"
BOXEDVN_GNUTLS_VERSION="3.8.9"
