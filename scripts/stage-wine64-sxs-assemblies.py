#!/usr/bin/env python3
r"""Stage the side-by-side assemblies Wine's own prefix setup would install.

Wine ships no winsxs tree and wine.inf never mentions one. The tree in a real
prefix is a by-product of wineboot's fake-DLL install: for every builtin it
copies into the prefix, `install_fake_dll` calls `register_fake_dll`, which
enumerates that module's RT_MANIFEST resources and, for each one whose resource
NAME begins with "WINE_MANIFEST", writes

    %windir%\winsxs\manifests\<arch>_<name>_<key>_<version>_<lang>_deadbeef.manifest
    %windir%\winsxs\<arch>_<name>_<key>_<version>_<lang>_deadbeef\<file>

(dlls/setupapi/fakedll.c: register_manifest, append_manifest_filename,
create_winsxs_dll_path, create_manifest).

ntdll's `lookup_winsxs` is the FIRST thing `lookup_assembly` tries, so an empty
winsxs is what sends a program down the private-assembly path --
<appdir>\<name>.dll, <appdir>\<name>.manifest and the two <name>\<name> forms --
and when those miss too, `parse_depend_manifests` fails the whole activation
context with STATUS_SXS_CANT_GEN_ACTCTX. A program whose manifest asks for
Microsoft.Windows.Common-Controls 6.0 then runs with no activation context at
all, and reaches the version 5 common controls instead.

On this lane the prefix's system32 is an in-memory projection of the packaged
builtin tree rather than a directory wineboot filled in, so that fake-DLL pass
produces nothing and the winsxs half of it is missing with it. This tool does
the same derivation at packaging time, from the same source of truth -- the
packaged modules' own WINE_MANIFEST resources -- so the staged tree cannot drift
from the Wine build it came from the way a hand-written one would.

The assembly directory holds a guest link to the packaged module rather than a
copy: BoxedWine reads a `.link` entry as a symlink whose contents are the target
path (EXT_LINK in source/io/fsnode.h), and duplicating every assembly DLL would
add megabytes to an archive that is already the app's largest resource.

Usage:
  scripts/stage-wine64-sxs-assemblies.py --pe-dir DIR --arch amd64|x86 \
      --module-guest-dir PATH --stage-dir DIR [--require NAME]...
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import Dict, Iterator, List, NamedTuple, Optional, Tuple

# The resource type Wine enumerates, and the resource-name prefix that marks a
# manifest as an assembly to register rather than an application manifest.
# `register_manifest` skips integer resource names outright, so RT_MANIFEST #1
# -- the ordinary application manifest most of Wine's programs carry -- is
# never mistaken for an assembly.
RT_MANIFEST = 24
WINE_MANIFEST_PREFIX = "WINE_MANIFEST"

# The trailer `append_manifest_filename` appends, and the placeholder Wine uses
# when a manifest names no language. `lookup_manifest_file` recognises the
# trailer to tell a Wine-provided assembly from one an installer put there.
ASSEMBLY_TRAILER = "deadbeef"
ASSEMBLY_NEUTRAL_LANGUAGE = "none"

# The architecture token substituted into an empty processorArchitecture, per
# `current_arch` in fakedll.c: "amd64" for the 64-bit PE tree and "x86" for the
# WoW64 one.
ARCHITECTURES = ("amd64", "x86")

# BoxedWine reads a guest symlink from a `.link` entry whose contents are the
# target path, with no trailing newline.
LINK_SUFFIX = ".link"


class Assembly(NamedTuple):
    """One registered side-by-side assembly, named the way Wine names it."""

    arch: str
    name: str
    key: str
    version: str
    language: str
    # The lowercased <arch>_<name>_<key>_<version>_<lang>_deadbeef stem.
    base: str
    # The manifest XML as it must be written, with the architecture filled in.
    manifest: bytes
    # The module names the assembly's <file> elements list.
    files: Tuple[str, ...]
    # The packaged module the manifest was read out of.
    source: str


class StagingError(Exception):
    """A failure that has to stop the build rather than be logged."""


def assembly_base(arch: str, name: str, key: str, version: str,
                  language: str) -> str:
    """The stem `append_manifest_filename` builds, lowercased as it does."""
    return "_".join(
        (arch, name, key, version, language, ASSEMBLY_TRAILER)).lower()


# ---------------------------------------------------------------------------
# PE resource walking.
#
# Deliberately not a general PE parser: it reads exactly the directory Wine's
# EnumResourceNamesW walks, and treats anything it does not recognise as "this
# module carries no assembly" rather than raising. A module with no resource
# section is the ordinary case -- most builtins have no WINE_MANIFEST at all.
# ---------------------------------------------------------------------------


class _Image:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.sections: List[Tuple[int, int, int, int]] = []
        self.resource_rva = 0
        self.resource_size = 0

    def rva_to_offset(self, rva: int) -> Optional[int]:
        for virtual_address, virtual_size, raw_offset, raw_size in self.sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva < virtual_address + span:
                delta = rva - virtual_address
                if delta >= raw_size:
                    return None
                return raw_offset + delta
        return None


def _load_image(data: bytes) -> Optional[_Image]:
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if lfanew <= 0 or lfanew + 24 > len(data) or data[lfanew:lfanew + 4] != b"PE\0\0":
        return None
    coff = lfanew + 4
    sections = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    if optional + 2 > len(data):
        return None
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic == 0x10B:
        directories = optional + 96
    elif magic == 0x20B:
        directories = optional + 112
    else:
        return None
    if directories + 24 > len(data):
        return None
    image = _Image(data)
    # Directory index 2 is the resource table.
    image.resource_rva, image.resource_size = struct.unpack_from(
        "<II", data, directories + 16)
    if image.resource_rva == 0 or image.resource_size == 0:
        return None
    table = optional + optional_size
    for index in range(sections):
        entry = table + index * 40
        if entry + 40 > len(data):
            return None
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, entry + 8)
        image.sections.append(
            (virtual_address, virtual_size, raw_offset, raw_size))
    return image


def _directory_entries(image: _Image,
                       offset: int) -> Iterator[Tuple[int, bool, int]]:
    """(id or name offset, is_name, entry value) for one resource directory."""
    data = image.data
    if offset + 16 > len(data):
        return
    named, ids = struct.unpack_from("<HH", data, offset + 12)
    for index in range(named + ids):
        entry = offset + 16 + index * 8
        if entry + 8 > len(data):
            return
        name, value = struct.unpack_from("<II", data, entry)
        yield name & 0x7FFFFFFF, bool(name & 0x80000000), value


def _resource_string(image: _Image, root: int, offset: int) -> Optional[str]:
    data = image.data
    at = root + offset
    if at + 2 > len(data):
        return None
    length = struct.unpack_from("<H", data, at)[0]
    if at + 2 + length * 2 > len(data):
        return None
    return data[at + 2:at + 2 + length * 2].decode("utf-16-le", "replace")


def wine_manifest_resources(data: bytes) -> List[bytes]:
    """Every RT_MANIFEST resource whose NAME begins with WINE_MANIFEST.

    The name test is Wine's own (`wcsncmp( res_name, L"WINE_MANIFEST", 13 )`),
    and it is the whole reason an application manifest is not staged as though
    it declared an assembly.
    """
    image = _load_image(data)
    if image is None:
        return []
    root = image.rva_to_offset(image.resource_rva)
    if root is None:
        return []
    found: List[bytes] = []
    for type_id, type_is_name, type_value in _directory_entries(image, root):
        if type_is_name or type_id != RT_MANIFEST:
            continue
        if not type_value & 0x80000000:
            continue
        names = root + (type_value & 0x7FFFFFFF)
        for name_id, name_is_name, name_value in _directory_entries(image, names):
            if not name_is_name:
                continue
            resource_name = _resource_string(image, root, name_id)
            if resource_name is None or not resource_name.startswith(
                    WINE_MANIFEST_PREFIX):
                continue
            if not name_value & 0x80000000:
                continue
            languages = root + (name_value & 0x7FFFFFFF)
            for _, _, leaf in _directory_entries(image, languages):
                if leaf & 0x80000000:
                    continue
                entry = root + leaf
                if entry + 8 > len(image.data):
                    continue
                rva, size = struct.unpack_from("<II", image.data, entry)
                at = image.rva_to_offset(rva)
                if at is None or at + size > len(image.data):
                    continue
                found.append(image.data[at:at + size])
    return found


# ---------------------------------------------------------------------------
# Manifest parsing.
#
# Byte-oriented on purpose. The architecture fixup Wine performs is a splice at
# the byte offset of an empty processorArchitecture value, and an XML round-trip
# through a DOM would rewrite a document Wine copies out verbatim.
# ---------------------------------------------------------------------------

_SPACE = (b" ", b"\t", b"\r", b"\n")


def _elements(manifest: bytes) -> Iterator[Tuple[str, int]]:
    """(element name, offset just past the name) for each element start tag."""
    at = 0
    length = len(manifest)
    while True:
        at = manifest.find(b"<", at)
        if at < 0:
            return
        at += 1
        if at < length and manifest[at:at + 1] in (b"/", b"?", b"!"):
            at += 1
            continue
        start = at
        while at < length and manifest[at:at + 1] not in _SPACE and \
                manifest[at:at + 1] not in (b">", b"/"):
            at += 1
        yield manifest[start:at].decode("ascii", "replace"), at


def _attributes(manifest: bytes, at: int) -> Iterator[Tuple[str, int, int]]:
    """(attribute name, value offset, value length) within one start tag."""
    length = len(manifest)
    while at < length:
        while at < length and manifest[at:at + 1] in _SPACE:
            at += 1
        if at >= length or manifest[at:at + 1] in (b">", b"/"):
            return
        start = at
        while at < length and manifest[at:at + 1] not in (b"=", b">") and \
                manifest[at:at + 1] not in _SPACE:
            at += 1
        if at >= length or manifest[at:at + 1] != b"=":
            return
        name = manifest[start:at].decode("ascii", "replace")
        at += 1
        if at >= length or manifest[at:at + 1] not in (b'"', b"'"):
            return
        quote = manifest[at:at + 1]
        at += 1
        end = manifest.find(quote, at)
        if end < 0:
            return
        yield name, at, end - at
        at = end + 1


def parse_assembly(manifest: bytes, current_arch: str,
                   source: str) -> Optional[Assembly]:
    """The assembly one WINE_MANIFEST resource declares, or None.

    Mirrors `register_manifest`: the identity comes from the assemblyIdentity
    element that carries name, version, processorArchitecture and
    publicKeyToken; a missing language becomes "none"; and an EMPTY
    processorArchitecture is filled in with the tree's own architecture, in the
    written manifest as well as in the file name.
    """
    identity: Optional[Dict[str, Tuple[int, int]]] = None
    files: List[str] = []
    for element, at in _elements(manifest):
        if element == "assemblyIdentity" and identity is None:
            identity = {
                name: (offset, size)
                for name, offset, size in _attributes(manifest, at)
            }
        elif element == "file" and identity is not None:
            for name, offset, size in _attributes(manifest, at):
                if name == "name":
                    files.append(
                        manifest[offset:offset + size].decode("utf-8", "replace"))
                    break
    if identity is None:
        return None
    required = ("name", "version", "processorArchitecture", "publicKeyToken")
    if any(field not in identity for field in required):
        return None

    def text(field: str) -> str:
        offset, size = identity[field]
        return manifest[offset:offset + size].decode("utf-8", "replace")

    arch_offset, arch_size = identity["processorArchitecture"]
    arch = text("processorArchitecture")
    written = manifest
    if arch_size == 0:
        arch = current_arch
        written = (manifest[:arch_offset] + current_arch.encode("ascii") +
                   manifest[arch_offset:])
    language = text("language") if "language" in identity \
        else ASSEMBLY_NEUTRAL_LANGUAGE
    name = text("name")
    version = text("version")
    key = text("publicKeyToken")
    return Assembly(
        arch=arch, name=name, key=key, version=version, language=language,
        base=assembly_base(arch, name, key, version, language),
        manifest=written, files=tuple(files), source=source)


# ---------------------------------------------------------------------------
# Staging.
# ---------------------------------------------------------------------------


def collect_assemblies(pe_dir: Path, current_arch: str) -> List[Assembly]:
    """Every assembly the modules in one PE tree register, sorted by name."""
    assemblies: Dict[str, Assembly] = {}
    for entry in sorted(pe_dir.iterdir()):
        if not entry.is_file():
            continue
        try:
            data = entry.read_bytes()
        except OSError:
            continue
        for manifest in wine_manifest_resources(data):
            assembly = parse_assembly(manifest, current_arch, entry.name)
            if assembly is None:
                continue
            # A duplicate base is a duplicate assembly; taking the first keeps
            # the result independent of directory order.
            assemblies.setdefault(assembly.base, assembly)
    return [assemblies[base] for base in sorted(assemblies)]


def stage_assemblies(assemblies: List[Assembly], stage_dir: Path,
                     module_guest_dir: str) -> List[str]:
    """Write the manifests and the assembly directories. Returns the paths."""
    written: List[str] = []
    manifests = stage_dir / "manifests"
    manifests.mkdir(parents=True, exist_ok=True)
    for assembly in assemblies:
        manifest_path = manifests / (assembly.base + ".manifest")
        manifest_path.write_bytes(assembly.manifest)
        written.append(str(manifest_path))
        if not assembly.files:
            continue
        directory = stage_dir / assembly.base
        directory.mkdir(parents=True, exist_ok=True)
        for name in assembly.files:
            # No trailing newline: the guest reads the whole entry as the
            # target path.
            link = directory / (name + LINK_SUFFIX)
            link.write_bytes(
                (module_guest_dir.rstrip("/") + "/" + name).encode("utf-8"))
            written.append(str(link))
    return written


def check_required(assemblies: List[Assembly], required: List[str]) -> None:
    present = {assembly.name.lower() for assembly in assemblies}
    missing = [name for name in required if name.lower() not in present]
    if missing:
        raise StagingError(
            "The packaged module tree registers no side-by-side assembly named "
            + ", ".join(missing)
            + ". Wine builds that tree from the WINE_MANIFEST resource of each "
              "builtin it installs, so a tree without one either came from a "
              "package that strips resources or is missing the module that "
              "carries it -- comctl32.dll for Microsoft.Windows.Common-Controls. "
              "A prefix without that assembly fails every activation context "
              "that depends on it, and the program silently gets version 5.")


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Stage Wine's side-by-side assemblies for a guest prefix.")
    parser.add_argument("--pe-dir", required=True, type=Path,
                        help="The packaged builtin PE tree to read.")
    parser.add_argument("--arch", required=True, choices=ARCHITECTURES,
                        help="The architecture token for an empty "
                             "processorArchitecture, per fakedll.c.")
    parser.add_argument("--module-guest-dir", required=True,
                        help="Guest path of --pe-dir, for the assembly links.")
    parser.add_argument("--stage-dir", required=True, type=Path,
                        help="Where the winsxs shape is written.")
    parser.add_argument("--require", action="append", default=[],
                        help="An assembly name the tree must register.")
    arguments = parser.parse_args(argv)

    if not arguments.pe_dir.is_dir():
        sys.stderr.write(
            "stage-wine64-sxs-assemblies: '{}' is not a directory.\n".format(
                arguments.pe_dir))
        return 2
    assemblies = collect_assemblies(arguments.pe_dir, arguments.arch)
    try:
        check_required(assemblies, arguments.require)
    except StagingError as error:
        sys.stderr.write("stage-wine64-sxs-assemblies: {}\n".format(error))
        return 1
    stage_assemblies(assemblies, arguments.stage_dir,
                     arguments.module_guest_dir)
    for assembly in assemblies:
        print("WINE64_SXS_ASSEMBLY arch={} name={} version={} key={} lang={} "
              "files={} source={}".format(
                  assembly.arch, assembly.name, assembly.version, assembly.key,
                  assembly.language, ",".join(assembly.files) or "none",
                  assembly.source))
    print("WINE64_SXS_SUMMARY arch={} assemblies={}".format(
        arguments.arch, len(assemblies)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
