#!/usr/bin/env python3
"""Contract tests for the x86-64 guest X11 shim.

The shim replaces the distro libX11/libXext for Wine's 64-bit driver. What
makes it load is not a guess: tools/x11-64/winex11-imports.txt records the
symbols winex11.so imports, the sources must define every one of them, and
the validator must refuse a library that misses one. These tests hold that
chain together without a compiler or a Wine install.
"""

from __future__ import annotations

import importlib.util
import pathlib
import re
import struct
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
SHIM_DIR = REPO / "tools" / "x11-64"
IMPORTS = SHIM_DIR / "winex11-imports.txt"
BRIDGE_HEADER = REPO / "include" / "boxedwine_x64_x11_bridge.h"
LAYOUT_HEADER = REPO / "source" / "x11" / "x11layout64.h"
LAYOUT_CHECK = SHIM_DIR / "layout_check.c"

_spec = importlib.util.spec_from_file_location(
    "validate_x64_x11_shim", REPO / "scripts" / "validate-x64-x11-shim.py")
validator = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules[_spec.name] = validator
_spec.loader.exec_module(validator)

EXPORT_PATTERN = re.compile(
    r"^BW_EXPORT\s+[A-Za-z_][\w\s\*\(\)]*?\b([A-Za-z_]\w*)\s*\(", re.MULTILINE)
STUB_PATTERN = re.compile(r"^BW_(?:STUB|ABSENT)\((\w+)", re.MULTILINE)


def exported_names(source: pathlib.Path) -> set[str]:
    text = source.read_text(encoding="utf-8")
    names = set()
    for match in EXPORT_PATTERN.finditer(text):
        names.add(match.group(1))
    # XSynchronize returns a function pointer; its declarator hides the name.
    if "XSynchronize(" in text:
        names.add("XSynchronize")
    return names


def stub_library_names(library_index: int) -> set[str]:
    """Names defined for one -DBW_STUB_LIBRARY value in extension_stub.c."""
    text = (SHIM_DIR / "extension_stub.c").read_text(encoding="utf-8")
    sections = re.split(r"^#(?:el)?if BW_STUB_LIBRARY == (\d+).*$", text, flags=re.MULTILINE)
    # sections: [prefix, index, body, index, body, ...]
    for index, body in zip(sections[1::2], sections[2::2]):
        if int(index) == library_index:
            body = body.split("#else")[0]
            return set(STUB_PATTERN.findall(body))
    return set()


class ImportListContract(unittest.TestCase):
    def test_import_list_is_sorted_and_unique(self) -> None:
        names = validator.read_import_list(IMPORTS)
        self.assertEqual(names, sorted(names))
        self.assertEqual(len(names), len(set(names)))
        self.assertGreater(len(names), 150)

    def test_every_import_is_defined_by_the_shim_sources(self) -> None:
        x11 = exported_names(SHIM_DIR / "X11.c")
        xext = exported_names(SHIM_DIR / "X11ext.c")
        missing = []
        for name in validator.read_import_list(IMPORTS):
            provider = validator.library_for_import(name)
            defined = xext if provider == "libXext.so.6" else x11
            if name not in defined:
                missing.append(f"{name} ({provider})")
        self.assertEqual(missing, [], "winex11.so imports without a shim definition")

    def test_display_accessors_wine_reads_through_macros_are_exported(self) -> None:
        x11 = exported_names(SHIM_DIR / "X11.c")
        for name in ("XDefaultScreen", "XScreenCount", "XDefaultRootWindow",
                     "XRootWindow", "XDefaultVisual", "XDefaultDepth",
                     "XConnectionNumber", "XDisplayName", "XCloseDisplay",
                     "XOpenDisplay", "XInitThreads"):
            self.assertIn(name, x11)

    def test_extension_stubs_define_every_dlsym_name(self) -> None:
        indices = {
            "libXrender.so.1": 1, "libXrandr.so.2": 2, "libXinerama.so.1": 3,
            "libXi.so.6": 4, "libXcursor.so.1": 5, "libXfixes.so.3": 6,
            "libXcomposite.so.1": 7, "libXxf86vm.so.1": 8,
        }
        for soname, names in validator.EXTENSION_EXPORTS.items():
            defined = stub_library_names(indices[soname])
            self.assertEqual(sorted(names - defined), [], soname)

    def test_required_exports_split_shape_and_shm_into_libxext(self) -> None:
        required = validator.required_exports(["XOpenDisplay", "XShapeCombineMask",
                                               "XShmPutImage"])
        self.assertIn("XOpenDisplay", required["libX11.so.6"])
        self.assertIn("XShapeCombineMask", required["libXext.so.6"])
        self.assertIn("XShmPutImage", required["libXext.so.6"])
        self.assertNotIn("XShmPutImage", required["libX11.so.6"])


class BridgeAbiContract(unittest.TestCase):
    def setUp(self) -> None:
        self.header = BRIDGE_HEADER.read_text(encoding="utf-8")

    def test_private_syscall_number_is_distinct_from_the_dxmt_call(self) -> None:
        dxmt = (REPO / "include" / "boxedwine_x64_hostcall.h").read_text(encoding="utf-8")
        dxmt_number = re.search(r"DXMT_UNIX_CALL (0x[0-9a-f]+)ULL", dxmt).group(1)
        x11_number = re.search(r"HOSTCALL_X11_BRIDGE (0x[0-9a-f]+)ULL", self.header).group(1)
        self.assertNotEqual(int(dxmt_number, 16), int(x11_number, 16))
        self.assertGreater(int(x11_number, 16), 1024)

    def test_operation_names_are_unique(self) -> None:
        names = re.findall(r"^\s+X\((\w+), \"([a-z0-9-]+)\"\)", self.header, re.MULTILINE)
        symbols = [name for name, _ in names]
        texts = [text for _, text in names]
        self.assertEqual(len(symbols), len(set(symbols)))
        self.assertEqual(len(texts), len(set(texts)))
        for expected in ("INIT_THREADS", "OPEN_DISPLAY", "CREATE_WINDOW",
                         "MAP_WINDOW", "REPORT_UNIMPLEMENTED"):
            self.assertIn(expected, symbols)

    def test_guest_helper_uses_the_syscall_instruction(self) -> None:
        self.assertIn('__asm__ volatile("syscall"', self.header)
        self.assertIn('"D"(op), "S"(argsAddress), "d"(count)', self.header)
        self.assertNotIn("int $0x9b", self.header)

    def test_layout_check_and_host_layout_agree(self) -> None:
        """Every offset the guest asserts, the host asserts with the same value."""
        guest = LAYOUT_CHECK.read_text(encoding="utf-8")
        host = LAYOUT_HEADER.read_text(encoding="utf-8")
        host_values: dict[str, int] = {}
        namespace = None
        for line in host.splitlines():
            ns = re.match(r"\s*namespace (\w+) \{", line)
            if ns:
                namespace = ns.group(1)
                continue
            value = re.match(r"\s*constexpr uint32_t (\w+) = (\d+);", line)
            if value and namespace:
                host_values[f"{namespace}.{value.group(1)}"] = int(value.group(2))
        checked = 0
        for type_name, field, expected in re.findall(
                r"BW_OFFSET\((\w+), (\w+), (\d+)\);", guest):
            key = {"BW_PRIV_DISPLAY": "Display"}.get(type_name, type_name)
            field = {"class": "c_class"}.get(field, field)
            host_key = f"{key}.{field}"
            if host_key in host_values:
                self.assertEqual(host_values[host_key], int(expected), host_key)
                checked += 1
        for type_name, expected in re.findall(r"BW_SIZE\((\w+), (\d+)\);", guest):
            key = {"BW_PRIV_DISPLAY": "Display"}.get(type_name, type_name)
            host_key = f"{key}.size"
            if host_key in host_values:
                self.assertEqual(host_values[host_key], int(expected), host_key)
                checked += 1
        self.assertGreater(checked, 100)


def synthetic_elf(machine: int = 62, elf_type: int = 3, soname: str = "libX11.so.6",
                  needed: tuple[str, ...] = ("libc.so.6",),
                  exports: tuple[str, ...] = ()) -> bytes:
    """An ELF64 with .dynsym/.dynstr/.dynamic/.shstrtab sections, nothing more."""
    strtab = bytearray(b"\0")
    offsets: dict[str, int] = {}

    def add(name: str) -> int:
        if name not in offsets:
            offsets[name] = len(strtab)
            strtab.extend(name.encode() + b"\0")
        return offsets[name]

    for name in needed:
        add(name)
    add(soname)
    symbols = bytearray(b"\0" * 24)
    for name in exports:
        info = (1 << 4) | 2  # STB_GLOBAL, STT_FUNC
        symbols += struct.pack("<IBBHQQ", add(name), info, 0, 1, 0x1000, 16)
    dynamic = bytearray()
    for name in needed:
        dynamic += struct.pack("<qQ", 1, offsets[name])
    dynamic += struct.pack("<qQ", 14, offsets[soname])
    dynamic += struct.pack("<qQ", 0, 0)
    shstr = b"\0.dynsym\0.dynstr\0.dynamic\0.shstrtab\0"
    header_size = 64
    body = bytes(symbols) + bytes(strtab) + bytes(dynamic) + shstr
    sym_off = header_size
    str_off = sym_off + len(symbols)
    dyn_off = str_off + len(strtab)
    shstr_off = dyn_off + len(dynamic)
    section_off = header_size + len(body)

    def section(name_off, stype, off, size, link, entsize):
        return struct.pack("<IIQQQQIIQQ", name_off, stype, 0, 0, off, size, link, 0, 8, entsize)

    sections = b"".join([
        section(0, 0, 0, 0, 0, 0),
        section(1, 11, sym_off, len(symbols), 2, 24),   # .dynsym -> .dynstr
        section(9, 3, str_off, len(strtab), 0, 0),      # .dynstr
        section(17, 6, dyn_off, len(dynamic), 2, 16),   # .dynamic -> .dynstr
        section(26, 3, shstr_off, len(shstr), 0, 0),    # .shstrtab
    ])
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2
    header[5] = 1
    header[6] = 1
    struct.pack_into("<HH", header, 16, elf_type, machine)
    struct.pack_into("<Q", header, 40, section_off)
    struct.pack_into("<HHHHHH", header, 52, 64, 0, 0, 64, 5, 4)
    return bytes(header) + body + sections


class ValidatorContract(unittest.TestCase):
    def check(self, **kwargs) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = pathlib.Path(raw) / "libX11.so.6"
            path.write_bytes(synthetic_elf(**kwargs))
            validator.validate_library(path, "libX11.so.6", {"XOpenDisplay", "XCloseDisplay"})

    def test_accepts_a_matching_library(self) -> None:
        self.check(exports=("XOpenDisplay", "XCloseDisplay", "XExtra"))

    def test_refuses_a_missing_export(self) -> None:
        with self.assertRaises(validator.ValidationError) as context:
            self.check(exports=("XOpenDisplay",))
        self.assertIn("XCloseDisplay", str(context.exception))

    def test_refuses_the_wrong_soname(self) -> None:
        with self.assertRaises(validator.ValidationError):
            self.check(soname="libX11.so.6.4.0", exports=("XOpenDisplay", "XCloseDisplay"))

    def test_refuses_a_foreign_machine(self) -> None:
        with self.assertRaises(validator.ValidationError):
            self.check(machine=183, exports=("XOpenDisplay", "XCloseDisplay"))

    def test_refuses_an_executable(self) -> None:
        with self.assertRaises(validator.ValidationError):
            self.check(elf_type=2, exports=("XOpenDisplay", "XCloseDisplay"))

    def test_refuses_a_distro_style_dependency(self) -> None:
        with self.assertRaises(validator.ValidationError):
            self.check(needed=("libc.so.6", "libxcb.so.1"),
                       exports=("XOpenDisplay", "XCloseDisplay"))

    def test_measures_undefined_x_symbols_of_a_driver(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = pathlib.Path(raw) / "winex11.so"
            data = bytearray(synthetic_elf(soname="winex11.so", exports=("XOpenDisplay", "malloc")))
            # Turn the two exports into undefined references (st_shndx = 0).
            sym_off = 64 + 24
            for index in range(2):
                struct.pack_into("<H", data, sym_off + index * 24 + 6, 0)
            path.write_bytes(bytes(data))
            self.assertEqual(validator.measure_winex11_imports(path), ["XOpenDisplay"])


if __name__ == "__main__":
    unittest.main()
