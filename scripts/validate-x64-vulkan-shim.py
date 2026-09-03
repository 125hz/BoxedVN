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


def read_import_contract(
        path: pathlib.Path) -> tuple[set[str], set[str], set[str]]:
    """Return (required, optional, not_imported) symbol names.

    [required] and [optional] are the driver's LOAD_FUNCPTR and
    LOAD_OPTIONAL_FUNCPTR lists. [not-imported] is the allow-list: names that
    appear as strings in the driver but are never dlsym'd out of the Vulkan
    library, each with the reason in a comment above the block.
    """
    blocks = {"[required]": set(), "[optional]": set(), "[not-imported]": set()}
    target = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line in blocks:
            target = blocks[line]
            continue
        if target is None:
            raise ValidationError(
                f"{path}: symbol {line!r} before a [required]/[optional]/"
                "[not-imported] heading")
        target.add(line)
    required = blocks["[required]"]
    if not required:
        raise ValidationError(f"{path}: no required symbols recorded")
    return required, blocks["[optional]"], blocks["[not-imported]"]


ALIAS_RE = re.compile(r"^\s*X\((\w+),\s*(\w+)\)\s*\\?\s*$")


def read_bridge_aliases(path: pathlib.Path) -> dict[str, str]:
    """Parse BOXEDWINE_X64_VK_ALIASES out of the ABI header: the alternate
    (KHR) spellings the guest ICD answers with a core command's entry point."""
    text = path.read_text(encoding="utf-8")
    start = text.find("#define BOXEDWINE_X64_VK_ALIASES(X)")
    if start < 0:
        raise ValidationError(f"{path}: no BOXEDWINE_X64_VK_ALIASES list")
    aliases: dict[str, str] = {}
    for raw in text[start:].splitlines()[1:]:
        match = ALIAS_RE.match(raw)
        if match:
            aliases[match.group(1)] = match.group(2)
            continue
        stripped = raw.strip()
        if not stripped or stripped.startswith("/*") or stripped.startswith("*"):
            continue
        if not stripped.endswith("\\"):
            break
    return aliases


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
    """Vulkan command names spelled out as whole strings inside a winex11.so.

    A dlsym argument is an ordinary string literal, so this is the only way to
    see one in a compiled driver -- but not every such literal is a dlsym
    argument, and the match has to be narrowed twice or it reports nonsense:

      * anchored at a string boundary, otherwise the driver's own exported
        `X11DRV_vkCreateWin32SurfaceKHR` matches as a bare
        `vkCreateWin32SurfaceKHR`, and its internal `wine_vk_init` matches as
        `vk_init`;
      * `vk` followed by an upper-case letter, which is how every Vulkan
        command is spelled and how none of Wine's own `wine_vk_*` helpers are.

    What survives is a name the driver mentions. Whether it *dlsyms* it cannot
    be decided from the binary, which is what the [not-imported] block of the
    contract file is for.
    """
    data = path.read_bytes()
    found: set[str] = set()
    # (?<![A-Za-z0-9_]) keeps the match from starting in the middle of a
    # longer identifier; the trailing NUL keeps it from ending in the middle.
    for match in re.finditer(rb"(?<![A-Za-z0-9_])vk[A-Z][A-Za-z0-9]{2,}\x00", data):
        found.add(match.group(0)[:-1].decode("ascii"))
    return found


def validate(shim: pathlib.Path, imports: pathlib.Path,
             bridge_header: pathlib.Path,
             winex11: pathlib.Path | None) -> dict[str, int]:
    required, optional, not_imported = read_import_contract(imports)
    commands = read_bridge_commands(bridge_header)
    aliases = read_bridge_aliases(bridge_header)
    unknown_alias = sorted(
        name for name, core in aliases.items() if core not in commands)
    if unknown_alias:
        raise ValidationError(
            f"{bridge_header}: alias(es) {unknown_alias} name a command the "
            "operation table does not carry")

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
    dispatchable |= {"vk" + name for name in aliases}
    undispatchable = sorted(required - dispatchable)
    if undispatchable:
        raise ValidationError(
            f"{bridge_header}: winex11.drv requires {undispatchable}, which the bridge "
            "operation table does not carry")

    named = 0
    if winex11 is not None:
        wanted = measure_winex11_vulkan_strings(winex11)

        # The check that matters, and the one the string scan can actually
        # decide: a name this contract records as dlsym'd has to still be
        # spelled out in the driver. If Wine renamed or dropped one, the shim
        # is exporting a symbol nobody binds and, worse, is probably missing
        # whatever replaced it -- and the failure on a device would be the
        # driver closing the handle with no diagnostic at all.
        vanished = sorted(name for name in required if name not in wanted)
        if vanished:
            raise ValidationError(
                f"{winex11}: does not name {len(vanished)} symbol(s) that "
                f"{imports.name} records as dlsym'd: " + ", ".join(vanished)
                + f". Re-read the LOAD_FUNCPTR list in Wine's "
                  "dlls/winex11.drv/vulkan.c and update the [required] block.")

        # Anything else the driver mentions. This cannot be decided from the
        # binary -- a dlsym argument and any other literal are both just
        # strings -- so an unrecorded name is a request for a human to read
        # vulkan.c, not a build failure. The [not-imported] block is where the
        # answer gets written down once someone has.
        unrecorded = sorted(wanted - required - optional - not_imported - dispatchable)
        if unrecorded:
            print(
                f"note: {winex11} names {len(unrecorded)} Vulkan command(s) not in "
                f"{imports.name}: " + ", ".join(unrecorded[:16])
                + (" ..." if len(unrecorded) > 16 else "")
                + "\n      Check the LOAD_FUNCPTR list in Wine's "
                  "dlls/winex11.drv/vulkan.c: if the driver dlsyms one of these, "
                  "add it to [required] (and to the bridge's operation table); "
                  "otherwise record it under [not-imported] with the reason.",
                file=sys.stderr)
        named = len(wanted)

    return {
        "required": len(required),
        "optional": len(optional),
        "not_imported": len(not_imported),
        "aliases": len(aliases),
        "commands": len(commands),
        "exported": len(exported),
        "driver_named": named,
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
        "required={required} optional={optional} not_imported={not_imported} "
        "aliases={aliases} commands={commands} exported={exported} "
        "driver_named={driver_named}".format(
            shim=args.shim, soname=SONAME, **counts)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
