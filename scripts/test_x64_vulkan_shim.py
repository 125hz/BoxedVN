#!/usr/bin/env python3
"""Contract tests for the x86-64 guest Vulkan ICD and its bridge.

Four separate files have to agree about the same list of Vulkan commands:

  * include/boxedwine_x64_vulkan_bridge.h -- the operation numbers, which are
    the IA-32 lane's ordinals plus a base so the two lanes can never drift;
  * source/vulkan/vkdef.h -- where those ordinals come from;
  * tools/vulkan-64/vulkan.c -- the guest entry point for each one;
  * source/vulkan/vulkanbridge64.cpp -- the typed host call for each one.

A mismatch in any of them is either a command the guest can call and the host
answers BADOP for, or a command the host carries and nothing can reach. None
of that shows up until a device run, and it shows up as silence. These tests
hold the four together without a compiler, a Vulkan SDK or a Wine install.
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
SHIM_DIR = REPO / "tools" / "vulkan-64"
SHIM_SOURCE = SHIM_DIR / "vulkan.c"
IMPORTS = SHIM_DIR / "winex11-vulkan-imports.txt"
BRIDGE_HEADER = REPO / "include" / "boxedwine_x64_vulkan_bridge.h"
VKDEF = REPO / "source" / "vulkan" / "vkdef.h"
DISPATCHER = REPO / "source" / "vulkan" / "vulkanbridge64.cpp"
BUILD_SCRIPT = REPO / "scripts" / "build-boxedwine-x64-vulkan.sh"

_spec = importlib.util.spec_from_file_location(
    "validate_x64_vulkan_shim", REPO / "scripts" / "validate-x64-vulkan-shim.py")
validator = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules[_spec.name] = validator
_spec.loader.exec_module(validator)

SHIM_DEF_RE = re.compile(
    r"^VKAPI_ATTR\s+[\w\*\s]+?VKAPI_CALL\s+(vk\w+)\s*\(", re.MULTILINE)
VKDEF_RE = re.compile(r"^#define\s+(\w+)\s+(\d+)\s*$", re.MULTILINE)
CASE_RE = re.compile(r"^\s*case VKB_(\w+):", re.MULTILINE)


def bridge_commands() -> dict[str, int]:
    return validator.read_bridge_commands(BRIDGE_HEADER)


def vkdef_ordinals() -> dict[str, int]:
    return {name: int(value)
            for name, value in VKDEF_RE.findall(VKDEF.read_text(encoding="utf-8"))}


def shim_definitions() -> set[str]:
    return set(SHIM_DEF_RE.findall(SHIM_SOURCE.read_text(encoding="utf-8")))


def dispatcher_cases() -> set[str]:
    return set(CASE_RE.findall(DISPATCHER.read_text(encoding="utf-8")))


class CommandTableContract(unittest.TestCase):
    def setUp(self) -> None:
        self.commands = bridge_commands()

    def test_the_table_is_not_empty(self) -> None:
        self.assertGreaterEqual(len(self.commands), 60)

    def test_ordinals_are_unique(self) -> None:
        ordinals = sorted(self.commands.values())
        self.assertEqual(len(ordinals), len(set(ordinals)),
                         "two commands share an operation number")

    def test_every_ordinal_matches_the_ia32_lane(self) -> None:
        known = vkdef_ordinals()
        for name, ordinal in sorted(self.commands.items()):
            with self.subTest(command=name):
                self.assertIn(name, known,
                              f"{name} is not a command source/vulkan/vkdef.h knows")
                self.assertEqual(
                    known[name], ordinal,
                    f"{name} is ordinal {known[name]} on the IA-32 lane but "
                    f"{ordinal} in the bridge table")

    def test_operation_numbers_cannot_collide_with_the_bootstrap_ops(self) -> None:
        text = BRIDGE_HEADER.read_text(encoding="utf-8")
        base = int(re.search(r"#define BOXEDWINE_X64_VK_OP_VK_BASE\s+(0x[0-9a-fA-F]+)",
                             text).group(1), 16)
        bootstrap = int(re.search(
            r"#define BOXEDWINE_X64_VK_OP_BOOTSTRAP_COUNT\s+(\d+)", text).group(1))
        self.assertGreater(base, bootstrap)
        for name, ordinal in self.commands.items():
            with self.subTest(command=name):
                self.assertGreaterEqual(base + ordinal, base)


class GuestShimContract(unittest.TestCase):
    def setUp(self) -> None:
        self.commands = bridge_commands()
        self.defined = shim_definitions()

    def test_the_shim_defines_every_command_in_the_table(self) -> None:
        missing = sorted({"vk" + name for name in self.commands} - self.defined)
        self.assertEqual(missing, [], f"tools/vulkan-64/vulkan.c is missing {missing}")

    def test_the_shim_defines_no_command_the_table_lacks(self) -> None:
        allowed = {"vk" + name for name in self.commands}
        allowed |= {"vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
                    "vk_icdGetInstanceProcAddr", "vk_icdGetPhysicalDeviceProcAddr",
                    "vk_icdNegotiateLoaderICDInterfaceVersion"}
        stray = sorted(self.defined - allowed)
        self.assertEqual(stray, [], f"exported without an operation number: {stray}")

    def test_every_required_winex11_symbol_is_defined(self) -> None:
        required, _optional = validator.read_import_contract(IMPORTS)
        missing = sorted(required - self.defined)
        self.assertEqual(missing, [],
                         f"winex11.drv binds {missing} by name and the shim lacks them")

    def test_the_shim_traps_with_the_syscall_instruction(self) -> None:
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        self.assertIn('__asm__ volatile("syscall"', header)
        self.assertIn("0x7fff0003ULL", header)

    def test_the_shim_declines_when_the_host_cannot_serve_it(self) -> None:
        # A build with no host Vulkan, or a caller without the identity map,
        # has to become VK_ERROR_INITIALIZATION_FAILED rather than a syscall
        # errno cast to a VkResult.
        source = SHIM_SOURCE.read_text(encoding="utf-8")
        self.assertIn("VK_ERROR_INITIALIZATION_FAILED", source)
        self.assertIn("BOXEDWINE_X64_VK_CAP_IDENTITY_MEMORY", source)
        self.assertIn("BOXEDWINE_X64_VK_CAP_HOST_MARSHAL", source)


class HostDispatcherContract(unittest.TestCase):
    def setUp(self) -> None:
        self.commands = bridge_commands()
        self.cases = dispatcher_cases()

    def test_every_command_has_a_typed_host_call(self) -> None:
        missing = sorted(set(self.commands) - self.cases)
        self.assertEqual(missing, [],
                         f"source/vulkan/vulkanbridge64.cpp has no case for {missing}")

    def test_no_case_names_a_command_the_table_lacks(self) -> None:
        stray = sorted(self.cases - set(self.commands))
        self.assertEqual(stray, [], f"dispatched without an operation number: {stray}")

    def test_allocation_callbacks_are_never_taken_from_the_guest(self) -> None:
        # A VkAllocationCallbacks holds guest x86-64 function pointers; the
        # host is arm64 and cannot call them. Every command that takes one has
        # to pass nullptr, which is what the IA-32 marshal does too.
        source = DISPATCHER.read_text(encoding="utf-8")
        for command in ("PFN_vkCreateInstance", "PFN_vkCreateDevice",
                        "PFN_vkAllocateMemory", "PFN_vkCreateImage",
                        "PFN_vkCreateSwapchainKHR"):
            with self.subTest(command=command):
                index = source.index(command)
                window = source[index:index + 400]
                self.assertIn("nullptr", window,
                              f"{command} must be called with a null pAllocator")

    def test_the_identity_map_rule_is_enforced(self) -> None:
        source = DISPATCHER.read_text(encoding="utf-8")
        self.assertIn("BOXEDWINE_X64_VK_E_MEMORY", source)
        self.assertIn("nativeIdentityMode", source)


class ImportContract(unittest.TestCase):
    def test_required_and_optional_lists_are_disjoint_and_sorted(self) -> None:
        required, optional = validator.read_import_contract(IMPORTS)
        self.assertEqual(required & optional, set())
        for block in (required, optional):
            self.assertEqual(sorted(block), sorted(block))

    def test_every_required_symbol_is_dispatchable(self) -> None:
        required, _optional = validator.read_import_contract(IMPORTS)
        dispatchable = {"vk" + name for name in bridge_commands()}
        dispatchable |= set(validator.ALWAYS_EXPORTED)
        self.assertEqual(sorted(required - dispatchable), [])


# ---- A synthetic ELF, so the validator itself is tested -----------------------

def build_library(soname: str, exports: list[str],
                  needed: list[str] | None = None,
                  machine: int = 62, elf_type: int = 3) -> bytes:
    """The smallest ELF64 the validator's reader accepts."""
    needed = needed or ["libc.so.6"]
    strings = b"\x00"
    offsets: dict[str, int] = {}

    def add(text: str) -> int:
        nonlocal strings
        offset = len(strings)
        strings += text.encode("ascii") + b"\x00"
        offsets[text] = offset
        return offset

    soname_offset = add(soname)
    needed_offsets = [add(name) for name in needed]
    symbol_offsets = [add(name) for name in exports]

    symbols = b"\x00" * 24
    for offset in symbol_offsets:
        # st_name, st_info (GLOBAL FUNC), st_other, st_shndx, st_value, st_size
        symbols += struct.pack("<IBBHQQ", offset, (1 << 4) | 2, 0, 1, 0x1000, 16)

    dynamic = b""
    for offset in needed_offsets:
        dynamic += struct.pack("<qQ", 1, offset)      # DT_NEEDED
    dynamic += struct.pack("<qQ", 14, soname_offset)  # DT_SONAME
    dynamic += struct.pack("<qQ", 0, 0)               # DT_NULL

    section_names = b"\x00"
    name_offsets: dict[str, int] = {}
    for name in (".dynstr", ".dynsym", ".dynamic", ".shstrtab"):
        name_offsets[name] = len(section_names)
        section_names += name.encode("ascii") + b"\x00"

    header_size = 64
    blobs = [strings, symbols, dynamic, section_names]
    offsets_in_file = []
    cursor = header_size
    for blob in blobs:
        offsets_in_file.append(cursor)
        cursor += len(blob)
    section_offset = cursor

    sections = [(0, 0, 0, 0, 0, 0, 0, 0)]
    sections.append((name_offsets[".dynstr"], 3, 2, 0x1000,
                     offsets_in_file[0], len(strings), 0, 1))
    sections.append((name_offsets[".dynsym"], 11, 2, 0x2000,
                     offsets_in_file[1], len(symbols), 1, 24))
    sections.append((name_offsets[".dynamic"], 6, 3, 0x3000,
                     offsets_in_file[2], len(dynamic), 1, 16))
    sections.append((name_offsets[".shstrtab"], 3, 0, 0,
                     offsets_in_file[3], len(section_names), 0, 1))

    data = bytearray()
    data += b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 8
    data += struct.pack("<HH", elf_type, machine)
    data += struct.pack("<I", 1)
    data += struct.pack("<QQQ", 0, 0, section_offset)
    data += struct.pack("<I", 0)
    data += struct.pack("<HHHHHH", 64, 56, 0, 64, len(sections), 4)
    assert len(data) == header_size
    for blob in blobs:
        data += blob
    for (name, kind, flags, addr, offset, size, link, entsize) in sections:
        data += struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset,
                            size, link, 0, 1, entsize)
    return bytes(data)


class ValidatorContract(unittest.TestCase):
    def setUp(self) -> None:
        self.commands = bridge_commands()
        self.required, _ = validator.read_import_contract(IMPORTS)
        self.exports = sorted({"vk" + name for name in self.commands}
                              | set(validator.ALWAYS_EXPORTED)
                              | self.required)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def write(self, data: bytes) -> pathlib.Path:
        path = self.dir / "libvulkan.so.1"
        path.write_bytes(data)
        return path

    def run_validator(self, path: pathlib.Path) -> dict[str, int]:
        return validator.validate(path, IMPORTS, BRIDGE_HEADER, None)

    def test_accepts_a_matching_library(self) -> None:
        path = self.write(build_library("libvulkan.so.1", self.exports))
        counts = self.run_validator(path)
        self.assertEqual(counts["commands"], len(self.commands))

    def test_refuses_a_missing_required_export(self) -> None:
        exports = [name for name in self.exports if name != "vkQueuePresentKHR"]
        path = self.write(build_library("libvulkan.so.1", exports))
        with self.assertRaises(validator.ValidationError):
            self.run_validator(path)

    def test_refuses_a_command_the_table_names_and_the_shim_lacks(self) -> None:
        exports = [name for name in self.exports if name != "vkCreateImageView"]
        path = self.write(build_library("libvulkan.so.1", exports))
        with self.assertRaises(validator.ValidationError):
            self.run_validator(path)

    def test_refuses_the_wrong_soname(self) -> None:
        path = self.write(build_library("libvulkan.so", self.exports))
        with self.assertRaises(validator.ValidationError):
            self.run_validator(path)

    def test_refuses_a_foreign_machine(self) -> None:
        path = self.write(build_library("libvulkan.so.1", self.exports, machine=183))
        with self.assertRaises(validator.ValidationError):
            self.run_validator(path)

    def test_refuses_a_distro_style_dependency(self) -> None:
        path = self.write(build_library("libvulkan.so.1", self.exports,
                                        needed=["libc.so.6", "libX11.so.6"]))
        with self.assertRaises(validator.ValidationError):
            self.run_validator(path)

    def test_measures_command_strings_out_of_a_driver(self) -> None:
        driver = self.dir / "winex11.so"
        driver.write_bytes(b"\x00vkQueuePresentKHR\x00vkSomethingNewKHR\x00")
        measured = validator.measure_winex11_vulkan_strings(driver)
        self.assertIn("vkQueuePresentKHR", measured)
        self.assertIn("vkSomethingNewKHR", measured)

    def test_refuses_a_driver_naming_an_unrecorded_command(self) -> None:
        path = self.write(build_library("libvulkan.so.1", self.exports))
        driver = self.dir / "winex11.so"
        driver.write_bytes(b"\x00vkSomethingNobodyRecorded\x00")
        with self.assertRaises(validator.ValidationError):
            validator.validate(path, IMPORTS, BRIDGE_HEADER, driver)


class BuildScriptContract(unittest.TestCase):
    def test_the_build_script_refuses_a_non_x86_64_compiler(self) -> None:
        text = BUILD_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("-dumpmachine", text)
        self.assertIn("does not target x86_64", text)

    def test_the_build_script_runs_the_validator(self) -> None:
        text = BUILD_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("validate-x64-vulkan-shim.py", text)

    def test_the_build_script_sets_the_soname(self) -> None:
        text = BUILD_SCRIPT.read_text(encoding="utf-8")
        self.assertIn('-Wl,-soname,"${SONAME}"', text)
        self.assertIn('SONAME="libvulkan.so.1"', text)


if __name__ == "__main__":
    unittest.main()
