#!/usr/bin/env python3
"""Validate the x86-64 guest Vulkan ICD BoxedWine ships.

Wine's winex11.drv dlopens SONAME_LIBVULKAN and resolves sixteen symbols out
of it with dlsym; a library missing one of them makes the driver close the
handle and give up, which in a device log is indistinguishable from the
library not being there at all. The shim built from tools/vulkan-64 replaces
the distro copy, so it has to be:

  * an ELF64 shared object for EM_X86_64,
  * carrying SONAME libvulkan.so.1,
  * depending on nothing but the guest libc,
  * exporting every required symbol in tools/vulkan-64/winex11-vulkan-imports.txt,
  * exporting one entry point per command in the bridge's operation table,
    so the shim and include/boxedwine_x64_vulkan_bridge.h cannot drift apart.

--winex11 re-measures a given winex11.so: the driver names the symbols it
dlsyms as string literals, so a Wine version that added a required one shows
up as a "vk" string the recorded list does not cover, and the build fails
here rather than on a device.
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import re
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent

# The ELF reader lives in the DXMT validator; reuse it rather than fork it.
_spec = importlib.util.spec_from_file_location(
    "validate_dxmt_guest_abi", SCRIPT_DIR / "validate-dxmt-guest-abi.py")
_dxmt = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules[_spec.name] = _dxmt
_spec.loader.exec_module(_dxmt)
ELFImage = _dxmt.ELFImage
ValidationError = _dxmt.ValidationError

EM_X86_64 = 62
ET_DYN = 3
SHN_UNDEF = 0
STB_LOCAL = 0

SONAME = "libvulkan.so.1"

# libpthread is folded into libc on the glibc the runtime ships, but an older
# builder may still name it.
ALLOWED_NEEDED = {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"}

# Command names in the bridge header that the shim serves itself rather than
# forwarding, and the two lookup entry points, which are not in the command
# table because they are not dispatched.
ALWAYS_EXPORTED = ("vkGetInstanceProcAddr", "vkGetDeviceProcAddr")

COMMAND_RE = re.compile(r"^\s*X\((\w+),\s*(\d+)\)\s*\\?\s*$")


def read_import_contract(path: pathlib.Path) -> tuple[set[str], set[str]]:
    """Return (required, optional) symbol names."""
    required: set[str] = set()
    optional: set[str] = set()
    target = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line == "[required]":
            target = required
            continue
        if line == "[optional]":
            target = optional
            continue
        if target is None:
            raise ValidationError(f"{path}: symbol {line!r} before a [required]/[optional] heading")
        target.add(line)
    if not required:
        raise ValidationError(f"{path}: no required symbols recorded")
    return required, optional


def read_bridge_commands(path: pathlib.Path) -> dict[str, int]:
    """Parse BOXEDWINE_X64_VK_COMMANDS out of the ABI header."""
    text = path.read_text(encoding="utf-8")
    start = text.find("#define BOXEDWINE_X64_VK_COMMANDS(X)")
    if start < 0:
        raise ValidationError(f"{path}: no BOXEDWINE_X64_VK_COMMANDS list")
    commands: dict[str, int] = {}
    for raw in text[start:].splitlines()[1:]:
        match = COMMAND_RE.match(raw)
        if match:
            name, ordinal = match.group(1), int(match.group(2))
            if name in commands:
                raise ValidationError(f"{path}: command {name} listed twice")
            commands[name] = ordinal
            continue
        stripped = raw.strip()
        if not stripped or stripped.startswith("/*") or stripped.startswith("*"):
            continue
        if not stripped.endswith("\\"):
            break
    if not commands:
        raise ValidationError(f"{path}: BOXEDWINE_X64_VK_COMMANDS is empty")
    return commands


def exported_symbols(image: ELFImage) -> set[str]:
    exported: set[str] = set()
    for name, (section_index, _size, info) in image.dynamic_symbols().items():
        binding = info >> 4
        if section_index == SHN_UNDEF or binding == STB_LOCAL:
            continue
        exported.add(name)
    return exported


def measure_winex11_vulkan_strings(path: pathlib.Path) -> set[str]:
    """Every "vk*" string literal in a winex11.so, which is how its dlsym
    names appear once the compiler is done with them."""
    data = path.read_bytes()
    found: set[str] = set()
    for match in re.finditer(rb"vk[A-Za-z0-9_]{3,}\x00", data):
        found.add(match.group(0)[:-1].decode("ascii"))
    return found


def validate(shim: pathlib.Path, imports: pathlib.Path,
             bridge_header: pathlib.Path,
             winex11: pathlib.Path | None) -> dict[str, int]:
    required, optional = read_import_contract(imports)
    commands = read_bridge_commands(bridge_header)

    image = ELFImage(shim)
    if image.machine != EM_X86_64:
        raise ValidationError(f"{shim}: e_machine is {image.machine}, expected EM_X86_64")
    if image.elf_type != ET_DYN:
        raise ValidationError(f"{shim}: not a shared object (e_type {image.elf_type})")
    needed, actual_soname = image.dynamic_strings()
    if actual_soname != SONAME:
        raise ValidationError(f"{shim}: SONAME is {actual_soname!r}, expected {SONAME!r}")
    stray = sorted(set(needed) - ALLOWED_NEEDED)
    if stray:
        raise ValidationError(
            f"{shim}: depends on {stray}; the guest ICD may only need the guest libc")

    exported = exported_symbols(image)

    missing = sorted(required - exported)
    if missing:
        raise ValidationError(
            f"{shim}: does not export {len(missing)} symbol(s) winex11.drv binds by name: "
            + ", ".join(missing))

    expected_commands = {"vk" + name for name in commands}
    missing_commands = sorted(expected_commands - exported)
    if missing_commands:
        raise ValidationError(
            f"{shim}: the bridge table names {len(missing_commands)} command(s) the shim "
            "does not export: " + ", ".join(missing_commands[:12])
            + (" ..." if len(missing_commands) > 12 else ""))

    for name in ALWAYS_EXPORTED:
        if name not in exported:
            raise ValidationError(f"{shim}: does not export {name}")

    # A required symbol that is not in the bridge table would be exported but
    # would answer BOXEDWINE_X64_VK_E_BADOP on its first call, which is worse
    # than not exporting it.
    dispatchable = expected_commands | set(ALWAYS_EXPORTED)
    undispatchable = sorted(required - dispatchable)
    if undispatchable:
        raise ValidationError(
            f"{bridge_header}: winex11.drv requires {undispatchable}, which the bridge "
            "operation table does not carry")

    if winex11 is not None:
        wanted = measure_winex11_vulkan_strings(winex11)
        # Only names the driver could plausibly dlsym: the recorded contract
        # plus anything else spelled like a Vulkan command.
        unrecorded = sorted(wanted - required - optional - dispatchable)
        if unrecorded:
            raise ValidationError(
                f"{winex11}: names {len(unrecorded)} Vulkan command(s) neither recorded in "
                f"{imports.name} nor carried by the bridge: " + ", ".join(unrecorded[:16])
                + (" ..." if len(unrecorded) > 16 else ""))

    return {
        "required": len(required),
        "optional": len(optional),
        "commands": len(commands),
        "exported": len(exported),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--shim", type=pathlib.Path, required=True,
                        help="the built libvulkan.so.1")
    parser.add_argument("--imports", type=pathlib.Path,
                        default=REPO_ROOT / "tools" / "vulkan-64" / "winex11-vulkan-imports.txt",
                        help="recorded winex11.drv import contract")
    parser.add_argument("--bridge-header", type=pathlib.Path,
                        default=REPO_ROOT / "include" / "boxedwine_x64_vulkan_bridge.h",
                        help="the bridge ABI header holding the command table")
    parser.add_argument("--winex11", type=pathlib.Path,
                        help="a winex11.so to re-measure against the recorded contract")
    args = parser.parse_args(argv)
    try:
        counts = validate(args.shim, args.imports, args.bridge_header, args.winex11)
    except ValidationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        "x86-64 guest Vulkan ICD verified: {shim} soname={soname} "
        "required={required} optional={optional} commands={commands} "
        "exported={exported}".format(shim=args.shim, soname=SONAME, **counts)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
