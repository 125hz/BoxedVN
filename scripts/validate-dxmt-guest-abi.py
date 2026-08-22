#!/usr/bin/env python3
"""Validate the guest-facing DXMT ELF and PE ABI without executing it."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import struct
import sys
from dataclasses import dataclass


class ValidationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ValidationError(message)


def unpack_from(fmt: str, data: bytes, offset: int, what: str):
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        fail(f"{what} extends past end of file")
    return struct.unpack_from(fmt, data, offset)


def c_string(data: bytes, offset: int, what: str) -> str:
    if offset < 0 or offset >= len(data):
        fail(f"{what} string offset is outside its table")
    end = data.find(b"\0", offset)
    if end < 0:
        fail(f"{what} string is not terminated")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError as error:
        fail(f"{what} string is not ASCII: {error}")


@dataclass(frozen=True)
class PESection:
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int


class PEImage:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            fail(f"{path}: missing DOS MZ signature")
        (pe_offset,) = unpack_from("<I", self.data, 0x3C, f"{path} DOS header")
        if self.data[pe_offset : pe_offset + 4] != b"PE\0\0":
            fail(f"{path}: missing PE signature")
        coff = pe_offset + 4
        self.machine, section_count, _, _, _, optional_size, _ = unpack_from(
            "<HHIIIHH", self.data, coff, f"{path} COFF header"
        )
        optional = coff + 20
        (self.magic,) = unpack_from("<H", self.data, optional, f"{path} optional header")
        if self.magic != 0x20B:
            fail(f"{path}: expected PE32+ optional header, got 0x{self.magic:04x}")
        (self.size_of_headers,) = unpack_from(
            "<I", self.data, optional + 60, f"{path} optional header"
        )
        (directory_count,) = unpack_from(
            "<I", self.data, optional + 108, f"{path} optional header"
        )
        directory_offset = optional + 112
        if directory_count > 16:
            directory_count = 16
        if directory_offset + directory_count * 8 > optional + optional_size:
            fail(f"{path}: data directories exceed optional header")
        self.directories = [
            unpack_from("<II", self.data, directory_offset + index * 8,
                        f"{path} data directory {index}")
            for index in range(directory_count)
        ]
        section_offset = optional + optional_size
        self.sections: list[PESection] = []
        for index in range(section_count):
            off = section_offset + index * 40
            _, virtual_size, virtual_address, raw_size, raw_offset = unpack_from(
                "<8sIIII", self.data, off, f"{path} section {index}"
            )
            self.sections.append(
                PESection(virtual_address, virtual_size, raw_offset, raw_size)
            )

    def rva_to_offset(self, rva: int, size: int = 1) -> int:
        if rva < self.size_of_headers and rva + size <= len(self.data):
            return rva
        for section in self.sections:
            extent = max(section.virtual_size, section.raw_size)
            if section.virtual_address <= rva and rva + size <= section.virtual_address + extent:
                delta = rva - section.virtual_address
                if delta + size > section.raw_size:
                    fail(f"{self.path}: RVA 0x{rva:x} points into an unbacked section tail")
                offset = section.raw_offset + delta
                if offset + size > len(self.data):
                    fail(f"{self.path}: RVA 0x{rva:x} resolves past end of file")
                return offset
        fail(f"{self.path}: RVA 0x{rva:x} is not covered by a section")

    def exports(self) -> dict[str, int]:
        if not self.directories or self.directories[0] == (0, 0):
            return {}
        export_rva, _ = self.directories[0]
        off = self.rva_to_offset(export_rva, 40)
        fields = unpack_from("<IIHHIIIIIII", self.data, off,
                             f"{self.path} export directory")
        ordinal_base = fields[5]
        function_count = fields[6]
        name_count = fields[7]
        names_rva = fields[9]
        ordinals_rva = fields[10]
        if name_count > function_count or name_count > 1_000_000:
            fail(f"{self.path}: unreasonable export counts")
        names_off = self.rva_to_offset(names_rva, name_count * 4)
        ordinals_off = self.rva_to_offset(ordinals_rva, name_count * 2)
        result: dict[str, int] = {}
        for index in range(name_count):
            (name_rva,) = unpack_from("<I", self.data, names_off + index * 4,
                                      f"{self.path} export name")
            (ordinal_index,) = unpack_from("<H", self.data, ordinals_off + index * 2,
                                           f"{self.path} export ordinal")
            if ordinal_index >= function_count:
                fail(f"{self.path}: export ordinal index is out of range")
            name = c_string(self.data, self.rva_to_offset(name_rva),
                            f"{self.path} export")
            if name in result:
                fail(f"{self.path}: duplicate export name {name}")
            result[name] = ordinal_base + ordinal_index
        return result

    def import_symbols(self) -> dict[str, set[str | int]]:
        if len(self.directories) < 2 or self.directories[1] == (0, 0):
            return {}
        import_rva, import_size = self.directories[1]
        off = self.rva_to_offset(import_rva, min(import_size, 20))
        result: dict[str, set[str | int]] = {}
        for index in range(0, max(import_size, 20), 20):
            entry = unpack_from("<IIIII", self.data, off + index,
                                f"{self.path} import descriptor")
            if entry == (0, 0, 0, 0, 0):
                return result
            name_rva = entry[3]
            name = c_string(self.data, self.rva_to_offset(name_rva),
                            f"{self.path} import").lower()
            thunk_rva = entry[0] or entry[4]
            thunk_off = self.rva_to_offset(thunk_rva, 8)
            symbols: set[str | int] = set()
            for thunk_index in range(1_000_000):
                (thunk,) = unpack_from("<Q", self.data, thunk_off + thunk_index * 8,
                                       f"{self.path} import thunk")
                if thunk == 0:
                    break
                if thunk & (1 << 63):
                    symbols.add(thunk & 0xFFFF)
                else:
                    symbol_off = self.rva_to_offset(thunk, 3)
                    symbols.add(c_string(self.data, symbol_off + 2,
                                         f"{self.path} imported symbol"))
            else:
                fail(f"{self.path}: import thunk table has no terminator")
            result[name] = symbols
        fail(f"{self.path}: import descriptor table has no terminator")

    def imports(self) -> set[str]:
        return set(self.import_symbols())


@dataclass(frozen=True)
class ELFSection:
    name_offset: int
    section_type: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    entry_size: int


class ELFImage:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:4] != b"\x7fELF":
            fail(f"{path}: missing ELF signature")
        if self.data[4] != 2 or self.data[5] != 1:
            fail(f"{path}: expected little-endian ELF64")
        self.elf_type, self.machine = unpack_from("<HH", self.data, 16,
                                                  f"{path} ELF header")
        program_offset, = unpack_from("<Q", self.data, 32, f"{path} ELF header")
        section_offset, = unpack_from("<Q", self.data, 40, f"{path} ELF header")
        program_entry_size, program_count = unpack_from(
            "<HH", self.data, 54, f"{path} ELF header"
        )
        if program_count and program_entry_size != 56:
            fail(f"{path}: unsupported ELF program-header size")
        self.program_headers = [
            unpack_from("<IIQQQQQQ", self.data,
                        program_offset + index * program_entry_size,
                        f"{path} program header {index}")
            for index in range(program_count)
        ]
        section_entry_size, section_count, string_index = unpack_from(
            "<HHH", self.data, 58, f"{path} ELF header"
        )
        if section_entry_size != 64 or section_count == 0 or string_index >= section_count:
            fail(f"{path}: unsupported or missing ELF section table")
        self.sections: list[ELFSection] = []
        for index in range(section_count):
            values = unpack_from("<IIQQQQIIQQ", self.data,
                                 section_offset + index * section_entry_size,
                                 f"{path} section {index}")
            self.sections.append(
                ELFSection(values[0], values[1], values[2], values[3], values[4],
                           values[5], values[6], values[9])
            )
        shstr = self.section_bytes(self.sections[string_index])
        self.sections_by_name: dict[str, ELFSection] = {}
        for section in self.sections:
            name = c_string(shstr, section.name_offset, f"{path} section name")
            self.sections_by_name[name] = section

    def section_bytes(self, section: ELFSection) -> bytes:
        if section.offset > len(self.data) or section.size > len(self.data) - section.offset:
            fail(f"{self.path}: section extends past end of file")
        return self.data[section.offset : section.offset + section.size]

    def dynamic_symbols(self) -> dict[str, tuple[int, int, int]]:
        section = self.sections_by_name.get(".dynsym")
        if section is None or section.entry_size != 24 or section.link >= len(self.sections):
            fail(f"{self.path}: missing or malformed .dynsym")
        strings = self.section_bytes(self.sections[section.link])
        symbols = self.section_bytes(section)
        result: dict[str, tuple[int, int, int]] = {}
        for offset in range(0, len(symbols), section.entry_size):
            name_offset, info, _, section_index, _, size = unpack_from(
                "<IBBHQQ", symbols, offset, f"{self.path} dynamic symbol"
            )
            if name_offset == 0:
                continue
            name = c_string(strings, name_offset, f"{self.path} dynamic symbol")
            result[name] = (section_index, size, info)
        return result

    def dynamic_strings(self) -> tuple[list[str], str | None]:
        section = self.sections_by_name.get(".dynamic")
        if section is None or section.entry_size not in (0, 16) or section.link >= len(self.sections):
            fail(f"{self.path}: missing or malformed .dynamic")
        strings = self.section_bytes(self.sections[section.link])
        dynamic = self.section_bytes(section)
        needed: list[str] = []
        soname: str | None = None
        for offset in range(0, len(dynamic), 16):
            tag, value = unpack_from("<qQ", dynamic, offset,
                                     f"{self.path} dynamic entry")
            if tag == 0:
                break
            if tag == 1:
                needed.append(c_string(strings, value, f"{self.path} DT_NEEDED"))
            elif tag == 14:
                soname = c_string(strings, value, f"{self.path} DT_SONAME")
        return needed, soname


PE_REQUIREMENTS: dict[str, dict[str, object]] = {
    "dxgi.dll": {
        "exports": {
            "CreateDXGIFactory": 9,
            "CreateDXGIFactory1": 10,
            "CreateDXGIFactory2": 11,
            "DXGIGetDebugInterface1": 17,
        },
        "imports": {"winemetal.dll"},
        "import_symbols": {},
    },
    "d3d11.dll": {
        "exports": {
            "D3D11CoreCreateDevice": 18,
            "D3D11CreateDevice": 22,
            "D3D11CreateDeviceAndSwapChain": 23,
            "D3D11On12CreateDevice": 24,
        },
        "imports": {"dxgi.dll", "winemetal.dll"},
        "import_symbols": {"dxgi.dll": {"CreateDXGIFactory1"}},
    },
    "d3d10core.dll": {
        "exports": {
            "D3D10CoreCreateDevice": None,
            "D3D10CoreRegisterLayers": None,
        },
        "imports": {"d3d11.dll"},
        "import_symbols": {"d3d11.dll": {"D3D11CoreCreateDevice"}},
    },
    "winemetal.dll": {
        "exports": {
            "CreateMetalViewFromHWND": 6,
            "MTLCommandBuffer_presentDrawable": 29,
            "MTLCommandBuffer_presentDrawableAfterMinimumDuration": 30,
            "ReleaseMetalView": 100,
        },
        "imports": set(),
        "import_symbols": {},
    },
}

PROBE_REQUIREMENTS: dict[str, object] = {
    "exports": {},
    "imports": {"d3d11.dll"},
    "import_symbols": {"d3d11.dll": {"D3D11CreateDevice"}},
}

GRAPHICS_MANIFEST_FORMAT = "boxedvn-x64-graphics-v1"
GRAPHICS_GUEST_FILES = {
    "probe_sha256": "boxedvn-d3d11-cube-x64.exe",
    "d3d11_sha256": "dxmt-x64/d3d11.dll",
    "dxgi_sha256": "dxmt-x64/dxgi.dll",
    "d3d10core_sha256": "dxmt-x64/d3d10core.dll",
    "winemetal_sha256": "dxmt-x64/winemetal.dll",
}
GRAPHICS_MANIFEST_KEYS = {
    "format",
    "probe",
    *GRAPHICS_GUEST_FILES.keys(),
    "native_archive",
    "native_archive_sha256",
}


def parse_key_value_manifest(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            fail(f"{path}:{line_number}: expected key=value")
        key, value = line.split("=", 1)
        if not key or not value:
            fail(f"{path}:{line_number}: manifest key and value must be non-empty")
        if key in values:
            fail(f"{path}:{line_number}: duplicate manifest key {key}")
        values[key] = value
    missing = GRAPHICS_MANIFEST_KEYS - set(values)
    unknown = set(values) - GRAPHICS_MANIFEST_KEYS
    if missing:
        fail(f"{path}: missing manifest keys: {', '.join(sorted(missing))}")
    if unknown:
        fail(f"{path}: unknown manifest keys: {', '.join(sorted(unknown))}")
    return values


def validate_manifest_file(root: pathlib.Path, relative: str,
                           expected_hash: str, label: str) -> pathlib.Path:
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_hash):
        fail(f"{label}: malformed SHA-256 {expected_hash!r}")
    path = root / relative
    if path.is_symlink() or not path.is_file():
        fail(f"{label}: missing regular file {path}")
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual.lower() != expected_hash.lower():
        fail(f"{label}: SHA-256 mismatch for {path.name}: "
             f"expected {expected_hash.lower()}, got {actual}")
    return path


def validate_graphics_bundle(root: pathlib.Path,
                             require_native_archive: bool) -> None:
    if not root.is_dir():
        fail(f"x64 graphics directory is missing: {root}")
    manifest_path = root / "x64-graphics.manifest"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        fail(f"x64 graphics manifest is missing: {manifest_path}")
    values = parse_key_value_manifest(manifest_path)
    if values["format"] != GRAPHICS_MANIFEST_FORMAT:
        fail(f"{manifest_path}: unsupported format {values['format']!r}")
    if values["probe"] != GRAPHICS_GUEST_FILES["probe_sha256"]:
        fail(f"{manifest_path}: unexpected probe name {values['probe']!r}")
    if values["native_archive"] != "libdxmt_combined.a":
        fail(f"{manifest_path}: unexpected native archive name "
             f"{values['native_archive']!r}")

    paths = {
        key: validate_manifest_file(root, relative, values[key], key)
        for key, relative in GRAPHICS_GUEST_FILES.items()
    }
    if require_native_archive:
        archive = validate_manifest_file(
            root, values["native_archive"], values["native_archive_sha256"],
            "native_archive_sha256")
        if archive.read_bytes()[:8] != b"!<arch>\n":
            fail(f"{archive}: native DXMT library is not a static archive")
    elif not re.fullmatch(r"[0-9a-fA-F]{64}",
                          values["native_archive_sha256"]):
        fail(f"{manifest_path}: malformed native archive SHA-256")

    validate_pe_surface(root / "dxmt-x64", paths["probe_sha256"])
    mode = "build artifact" if require_native_archive else "packaged guest assets"
    print(f"x64 graphics manifest ok: {mode}, all checksums and ABIs match")


def validate_pe(path: pathlib.Path, requirements: dict[str, object] | None = None) -> None:
    image = PEImage(path)
    if image.machine != 0x8664:
        fail(f"{path}: expected AMD64 machine 0x8664, got 0x{image.machine:04x}")
    exports = image.exports()
    imported_symbols = image.import_symbols()
    imports = set(imported_symbols)
    if requirements is not None:
        required_exports = requirements["exports"]
        assert isinstance(required_exports, dict)
        for name, ordinal in required_exports.items():
            if name not in exports:
                fail(f"{path}: missing required export {name}")
            if ordinal is not None and exports[name] != ordinal:
                fail(f"{path}: export {name} has ordinal {exports[name]}, expected {ordinal}")
        required_imports = requirements["imports"]
        assert isinstance(required_imports, set)
        missing_imports = required_imports - imports
        if missing_imports:
            fail(f"{path}: missing required imports {', '.join(sorted(missing_imports))}")
        required_symbols = requirements.get("import_symbols", {})
        assert isinstance(required_symbols, dict)
        for module, names in required_symbols.items():
            missing_names = names - imported_symbols.get(module, set())
            if missing_names:
                fail(f"{path}: missing imports from {module}: "
                     f"{', '.join(sorted(missing_names))}")
    print(
        f"PE ABI ok: {path.name} machine=AMD64 exports={len(exports)} "
        f"imports={','.join(sorted(imports)) or '(none)'}"
    )


def validate_pe_surface(pe_dir: pathlib.Path,
                        probe: pathlib.Path | None = None) -> None:
    """Validate the complete staged DXMT PE surface and its probe.

    Keeping this as one entry point makes direct PE validation and manifest
    validation enforce the same machine/export/import contract. The manifest
    remains responsible for hashes and layout; this function is the
    host/CI-facing ABI gate.
    """
    if not pe_dir.is_dir():
        fail(f"DXMT PE directory is missing: {pe_dir}")
    for filename, requirements in PE_REQUIREMENTS.items():
        path = pe_dir / filename
        if path.is_symlink() or not path.is_file():
            fail(f"missing required PE module {path}")
        validate_pe(path, requirements)
    validate_dxmt_resolution(pe_dir)
    if probe is not None:
        validate_pe(probe, PROBE_REQUIREMENTS)


def expected_unix_count(repo_root: pathlib.Path) -> int:
    header = repo_root / "include" / "boxedwine_x64_hostcall.h"
    match = re.search(
        rb"^#define\s+BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL_COUNT\s+(\d+)U\s*$",
        header.read_bytes(),
        re.MULTILINE,
    )
    if match is None:
        fail(f"{header}: could not read unix-call count")
    return int(match.group(1))


def validate_unixlib(path: pathlib.Path, repo_root: pathlib.Path) -> None:
    image = ELFImage(path)
    if image.elf_type != 3:
        fail(f"{path}: expected ELF ET_DYN (3), got {image.elf_type}")
    if image.machine != 62:
        fail(f"{path}: expected ELF EM_X86_64 (62), got {image.machine}")
    if any(header[0] == 3 for header in image.program_headers):
        fail(f"{path}: guest syscall veneer must not contain PT_INTERP")
    if any(header[0] == 1 and (header[1] & 0x3) == 0x3
           for header in image.program_headers):
        fail(f"{path}: guest syscall veneer contains a writable executable segment")
    symbols = image.dynamic_symbols()
    expected_size = expected_unix_count(repo_root) * 8
    table = symbols.get("__wine_unix_call_funcs")
    if table is None:
        fail(f"{path}: missing dynamic export __wine_unix_call_funcs")
    section_index, size, info = table
    if section_index == 0:
        fail(f"{path}: __wine_unix_call_funcs is undefined")
    if size != expected_size:
        fail(f"{path}: unix-call table size {size}, expected {expected_size}")
    if info >> 4 not in (1, 2):
        fail(f"{path}: unix-call table is not a global/weak dynamic symbol")
    undefined = sorted(
        name for name, (index, _, symbol_info) in symbols.items()
        if index == 0 and (symbol_info >> 4) != 0
    )
    if undefined:
        fail(f"{path}: unexpected undefined dynamic symbols: {', '.join(undefined)}")
    needed, soname = image.dynamic_strings()
    if needed:
        fail(f"{path}: unexpected DT_NEEDED dependencies: {', '.join(needed)}")
    if soname != "winemetal.so":
        fail(f"{path}: expected SONAME winemetal.so, got {soname!r}")
    syscall_immediate = b"\x01\x00\xff\x7f"
    syscall = b"\x0f\x05"
    positions = [index for index in range(len(image.data))
                 if image.data.startswith(syscall_immediate, index)]
    if not positions or not any(syscall in image.data[index : index + 64] for index in positions):
        fail(f"{path}: private syscall immediate/instruction sequence is missing")
    print(
        f"ELF ABI ok: {path.name} machine=EM_X86_64 table={expected_size} "
        "SONAME=winemetal.so needed=(none)"
    )


def validate_dxmt_resolution(pe_dir: pathlib.Path) -> None:
    images = {
        filename: PEImage(pe_dir / filename)
        for filename in PE_REQUIREMENTS
    }
    exports = {filename: image.exports() for filename, image in images.items()}
    for filename, image in images.items():
        for module, symbols in image.import_symbols().items():
            if module not in images:
                continue
            named = exports[module]
            ordinals = set(named.values())
            unresolved = sorted(
                str(symbol) for symbol in symbols
                if (isinstance(symbol, str) and symbol not in named) or
                   (isinstance(symbol, int) and symbol not in ordinals)
            )
            if unresolved:
                fail(f"{filename}: unresolved imports from {module}: "
                     f"{', '.join(unresolved)}")
    print("PE ABI ok: all staged cross-module DXMT imports resolve")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixlib", type=pathlib.Path)
    parser.add_argument("--pe-dir", type=pathlib.Path)
    parser.add_argument("--probe", type=pathlib.Path)
    parser.add_argument("--graphics-dir", type=pathlib.Path)
    parser.add_argument("--packaged-graphics-dir", type=pathlib.Path)
    args = parser.parse_args()
    if (args.unixlib is None and args.pe_dir is None and args.probe is None and
            args.graphics_dir is None and args.packaged_graphics_dir is None):
        parser.error("at least one validation target is required")
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    if args.unixlib is not None:
        validate_unixlib(args.unixlib, repo_root)
    if args.pe_dir is not None:
        validate_pe_surface(args.pe_dir)
    if args.probe is not None:
        validate_pe(args.probe, PROBE_REQUIREMENTS)
    if args.graphics_dir is not None:
        validate_graphics_bundle(args.graphics_dir, True)
    if args.packaged_graphics_dir is not None:
        validate_graphics_bundle(args.packaged_graphics_dir, False)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValidationError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
