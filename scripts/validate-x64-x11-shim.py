#!/usr/bin/env python3
"""Validate the x86-64 guest X11 client libraries BoxedWine ships.

Wine's winex11.so links libX11.so.6 and libXext.so.6 by DT_NEEDED and looks
the extension libraries up by SONAME. The shim libraries built from
tools/x11-64 replace the distro copies, so each one has to be:

  * an ELF64 shared object for EM_X86_64,
  * carrying exactly the SONAME the driver asks for,
  * depending on nothing but the guest libc,
  * exporting every symbol the driver imports from it.

The import list is measured, not guessed: tools/x11-64/winex11-imports.txt
records the undefined X* symbols of the packaged winex11.so, and --winex11
re-measures a given driver binary and refuses a build whose recorded list
has drifted from it.
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import struct
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent

# The ELF reader lives in the DXMT validator; reuse it rather than fork it.
_spec = importlib.util.spec_from_file_location(
    "validate_dxmt_guest_abi", SCRIPT_DIR / "validate-dxmt-guest-abi.py")
_dxmt = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
# dataclasses resolve the defining module through sys.modules.
sys.modules[_spec.name] = _dxmt
_spec.loader.exec_module(_dxmt)
ELFImage = _dxmt.ELFImage
ValidationError = _dxmt.ValidationError

EM_X86_64 = 62
ET_DYN = 3
SHN_UNDEF = 0
STB_LOCAL = 0
STT_FUNC = 2

# SONAME -> the guest libc dependencies it may carry. libpthread is folded
# into libc on the glibc the runtime ships, but an older builder may still
# name it.
ALLOWED_NEEDED = {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"}

# Every library the runtime places first on the guest's library path.
SHIM_LIBRARIES = (
    "libX11.so.6",
    "libXext.so.6",
    "libXrender.so.1",
    "libXrandr.so.2",
    "libXinerama.so.1",
    "libXi.so.6",
    "libXcursor.so.1",
    "libXfixes.so.3",
    "libXcomposite.so.1",
    "libXxf86vm.so.1",
)

# Symbol names that belong to libXext rather than libX11.
XEXT_PREFIXES = ("XShape", "XShm")

# What winex11.so looks up with dlsym in each extension library, measured
# from its string table. A stub that misses one makes Wine's LOAD_FUNCPTR
# fail the whole library, which is the same outcome as not shipping it.
EXTENSION_EXPORTS = {
    "libXrender.so.1": {
        "XRenderQueryExtension", "XRenderAddGlyphs", "XRenderChangePicture",
        "XRenderComposite", "XRenderCompositeText16", "XRenderCreateGlyphSet",
        "XRenderCreateLinearGradient", "XRenderCreatePicture",
        "XRenderFillRectangle", "XRenderFindFormat", "XRenderFindVisualFormat",
        "XRenderFreeGlyphSet", "XRenderFreePicture",
        "XRenderSetPictureClipRectangles", "XRenderSetPictureTransform",
    },
    "libXrandr.so.2": {
        "XRRQueryExtension", "XRRQueryVersion", "XRRConfigCurrentConfiguration",
        "XRRConfigCurrentRate", "XRRFreeCrtcInfo", "XRRFreeOutputInfo",
        "XRRFreeProviderInfo", "XRRFreeProviderResources",
        "XRRFreeScreenConfigInfo", "XRRFreeScreenResources", "XRRGetCrtcInfo",
        "XRRGetOutputInfo", "XRRGetOutputPrimary", "XRRGetOutputProperty",
        "XRRGetProviderInfo", "XRRGetProviderResources", "XRRGetScreenInfo",
        "XRRGetScreenResources", "XRRGetScreenResourcesCurrent",
        "XRRGetScreenSizeRange", "XRRRates", "XRRSelectInput",
        "XRRSetCrtcConfig", "XRRSetScreenConfig", "XRRSetScreenConfigAndRate",
        "XRRSetScreenSize", "XRRSizes",
    },
    "libXinerama.so.1": {"XineramaQueryExtension", "XineramaQueryScreens"},
    "libXi.so.6": {
        "XIQueryVersion", "XIFreeDeviceInfo", "XIGetClientPointer",
        "XIQueryDevice", "XISelectEvents",
    },
    "libXcursor.so.1": {
        "XcursorImageCreate", "XcursorImageDestroy", "XcursorImageLoadCursor",
        "XcursorImagesCreate", "XcursorImagesDestroy", "XcursorImagesLoadCursor",
        "XcursorLibraryLoadCursor",
    },
    "libXfixes.so.3": {
        "XFixesQueryExtension", "XFixesQueryVersion", "XFixesSelectSelectionInput",
    },
    "libXcomposite.so.1": {
        "XCompositeQueryExtension", "XCompositeQueryVersion", "XCompositeVersion",
        "XCompositeCreateRegionFromBorderClip", "XCompositeNameWindowPixmap",
        "XCompositeRedirectSubwindows", "XCompositeRedirectWindow",
        "XCompositeUnredirectSubwindows", "XCompositeUnredirectWindow",
    },
    "libXxf86vm.so.1": {
        "XF86VidModeQueryExtension", "XF86VidModeQueryVersion",
        "XF86VidModeGetAllModeLines", "XF86VidModeGetGamma",
        "XF86VidModeGetGammaRamp", "XF86VidModeGetGammaRampSize",
        "XF86VidModeGetModeLine", "XF86VidModeLockModeSwitch",
        "XF86VidModeSetGamma", "XF86VidModeSetGammaRamp",
        "XF86VidModeSetViewPort", "XF86VidModeSwitchToMode",
    },
}


def read_import_list(path: pathlib.Path) -> list[str]:
    names: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        names.append(line)
    return names


def library_for_import(name: str) -> str:
    return "libXext.so.6" if name.startswith(XEXT_PREFIXES) else "libX11.so.6"


def required_exports(imports: list[str]) -> dict[str, set[str]]:
    required: dict[str, set[str]] = {library: set() for library in SHIM_LIBRARIES}
    for name in imports:
        required[library_for_import(name)].add(name)
    for library, names in EXTENSION_EXPORTS.items():
        required[library] |= names
    return required


def measure_winex11_imports(path: pathlib.Path) -> list[str]:
    """The undefined X* symbols of a winex11.so: what the loader must resolve."""
    image = ELFImage(path)
    names: list[str] = []
    for name, (section_index, _size, info) in image.dynamic_symbols().items():
        if section_index != SHN_UNDEF:
            continue
        if not (name.startswith("X") or name.startswith("_X")):
            continue
        names.append(name)
    return sorted(names)


def exported_symbols(image: ELFImage) -> set[str]:
    exported: set[str] = set()
    for name, (section_index, _size, info) in image.dynamic_symbols().items():
        binding = info >> 4
        if section_index == SHN_UNDEF or binding == STB_LOCAL:
            continue
        exported.add(name)
    return exported


def validate_library(path: pathlib.Path, soname: str, required: set[str]) -> None:
    image = ELFImage(path)
    if image.machine != EM_X86_64:
        raise ValidationError(f"{path}: e_machine is {image.machine}, expected EM_X86_64")
    if image.elf_type != ET_DYN:
        raise ValidationError(f"{path}: not a shared object (e_type {image.elf_type})")
    needed, actual_soname = image.dynamic_strings()
    if actual_soname != soname:
        raise ValidationError(f"{path}: SONAME is {actual_soname!r}, expected {soname!r}")
    stray = sorted(set(needed) - ALLOWED_NEEDED)
    if stray:
        raise ValidationError(
            f"{path}: depends on {stray}; the shim may only need the guest libc")
    exported = exported_symbols(image)
    missing = sorted(required - exported)
    if missing:
        raise ValidationError(
            f"{path}: does not export {len(missing)} symbol(s) winex11.so needs: "
            + ", ".join(missing[:12]) + (" ..." if len(missing) > 12 else ""))


def validate_shim_dir(shim_dir: pathlib.Path, imports: list[str]) -> dict[str, int]:
    required = required_exports(imports)
    counts: dict[str, int] = {}
    for soname in SHIM_LIBRARIES:
        path = shim_dir / soname
        if not path.is_file():
            raise ValidationError(f"{shim_dir}: missing {soname}")
        validate_library(path, soname, required[soname])
        counts[soname] = len(required[soname])
    return counts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--shim-dir", type=pathlib.Path, required=True,
                        help="directory holding the built shim libraries")
    parser.add_argument("--imports", type=pathlib.Path,
                        default=REPO_ROOT / "tools" / "x11-64" / "winex11-imports.txt",
                        help="recorded winex11.so import list")
    parser.add_argument("--winex11", type=pathlib.Path,
                        help="a winex11.so to re-measure against the recorded list")
    args = parser.parse_args(argv)
    try:
        imports = read_import_list(args.imports)
        if args.winex11 is not None:
            measured = measure_winex11_imports(args.winex11)
            recorded = sorted(imports)
            if measured != recorded:
                added = sorted(set(measured) - set(recorded))
                dropped = sorted(set(recorded) - set(measured))
                raise ValidationError(
                    f"{args.winex11}: import list drifted from {args.imports}: "
                    f"new={added} gone={dropped}. Update the recorded list and the shim.")
            print(f"winex11 imports: {len(measured)} symbols match the recorded list")
        counts = validate_shim_dir(args.shim_dir, imports)
    except ValidationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for soname, count in counts.items():
        print(f"ok: {soname} exports the {count} symbol(s) winex11.so needs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
