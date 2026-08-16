#!/usr/bin/env bash
# BoxedVN - pinned third-party sources for the fex64 branch.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# These are the three upstreams of the native-ARM64 stack described in
# docs/ARCHITECTURE_FEX64.md.  They are pinned by tag rather than by SHA-256,
# because all three are consumed as git trees with submodules, and a git
# archive of a tag is not byte-stable.  scripts/fetch-fex64-dependencies.sh
# resolves each tag to a commit and records it in
# third_party/fex64/PINNED_REVISIONS.txt; commit that file back once the first
# real build on macOS has succeeded, so the exact tree is reproducible.
#
# Licences.  All three combine with BoxedVN's GPLv2:
#   FEX    MIT
#   Wine   LGPL-2.1-or-later
#   DXMT   LGPL-2.1-or-later
# THIRD_PARTY_NOTICES.md must be updated before any build using these is
# distributed.

# --- FEX --------------------------------------------------------------------
# CPU emulation only: x86-64 -> ARM64.  Used here the way ARM64 desktop Wine
# uses it, as the emulation backend beneath a native Wine, not as a Linux
# usermode emulator.  Its Linux frontend (FEXLoader, RootFS, thunks) has no
# role on iOS and is not built.
#
# FEX-2608 removed the deprecated FEXInterpreter binary; this pin is the first
# release that will not silently offer it.
BOXEDVN_FEX_REPOSITORY="https://github.com/FEX-Emu/FEX.git"
BOXEDVN_FEX_TAG="FEX-2608"

# --- Wine -------------------------------------------------------------------
# Built native ARM64 for Darwin.  Provisional pin: DXMT's own installation
# guide asks for CrossOver Wine 24+, or Wine 8+ with additional APIs exposed
# from winemac.drv.  Neither applies unchanged here, because iOS has no AppKit
# and this branch has to supply its own display driver anyway (problem 5 in
# docs/ARCHITECTURE_FEX64.md).  Upstream is therefore the more honest base to
# carry patches against, and M0 is what decides whether this pin survives.
BOXEDVN_WINE_REPOSITORY="https://gitlab.winehq.org/wine/wine.git"
BOXEDVN_WINE_TAG="wine-11.0"

# --- DXMT -------------------------------------------------------------------
# D3D11/D3D10 -> Metal.  Note that its documented install layout is entirely
# x86_64-unix / x86_64-windows: there is no 32-bit deployment, which is one of
# the reasons this branch is x86-64 only.
BOXEDVN_DXMT_REPOSITORY="https://github.com/3Shain/dxmt.git"
BOXEDVN_DXMT_TAG="v0.80"
