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

A fifth file joins them for the marshal: source/vulkan/vk/vulkan_core.h, whose
structures the dispatcher's extensible-structure table names. A typo there is a
build failure rather than silence, but the table also has to keep covering the
structures the D3D9 bootstrap actually carries, and nothing but a test says so.
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
VULKAN_CORE = REPO / "source" / "vulkan" / "vk" / "vulkan_core.h"
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
CHAIN_RE = re.compile(
    r"^\s*X\((VK_STRUCTURE_TYPE_\w+),\s*(\w+)\)\s*\\?\s*$", re.MULTILINE)


def bridge_commands() -> dict[str, int]:
    return validator.read_bridge_commands(BRIDGE_HEADER)


def vkdef_ordinals() -> dict[str, int]:
    return {name: int(value)
            for name, value in VKDEF_RE.findall(VKDEF.read_text(encoding="utf-8"))}


def shim_definitions() -> set[str]:
    return set(SHIM_DEF_RE.findall(SHIM_SOURCE.read_text(encoding="utf-8")))


def dispatcher_cases() -> set[str]:
    return set(CASE_RE.findall(DISPATCHER.read_text(encoding="utf-8")))


def chain_structs() -> list[tuple[str, str]]:
    """The dispatcher's extensible-structure table, as (sType, struct) pairs."""
    text = DISPATCHER.read_text(encoding="utf-8")
    start = text.index("#define BOXEDWINE_X64_VK_CHAIN_STRUCTS(X)")
    end = text.index("struct ChainStructInfo", start)
    return CHAIN_RE.findall(text[start:end])


def vulkan_structure_types() -> set[str]:
    return set(re.findall(r"\b(VK_STRUCTURE_TYPE_\w+)\s*=",
                          VULKAN_CORE.read_text(encoding="utf-8")))


def vulkan_struct_names() -> set[str]:
    """Every structure name vulkan_core.h defines, including the alias
    typedefs a promoted extension leaves behind (VkPhysicalDeviceXFeaturesEXT
    for VkPhysicalDeviceXFeatures, which sizeof still resolves through)."""
    text = VULKAN_CORE.read_text(encoding="utf-8")
    names = set(re.findall(r"\}\s*(Vk\w+);", text))
    names |= set(re.findall(r"^typedef\s+Vk\w+\s+(Vk\w+);", text, re.MULTILINE))
    return names


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
        required, _optional, _not_imported = validator.read_import_contract(IMPORTS)
        missing = sorted(required - self.defined)
        self.assertEqual(missing, [],
                         f"winex11.drv binds {missing} by name and the shim lacks them")

    def test_every_alias_maps_onto_a_command_the_shim_defines(self) -> None:
        source = SHIM_SOURCE.read_text(encoding="utf-8")
        self.assertIn("BOXEDWINE_X64_VK_ALIASES(BW_ALIAS_ENTRY)", source,
                      "the shim's lookup table must carry the alias spellings")
        for alias, core in validator.read_bridge_aliases(BRIDGE_HEADER).items():
            with self.subTest(alias=alias):
                self.assertIn("vk" + core, self.defined)
                self.assertIn(core, self.commands)

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

    def test_alias_spellings_resolve_and_fall_back(self) -> None:
        # A caller asking for the KHR spelling has to be served, and the host
        # has to try that spelling against the driver when the core one is
        # absent -- MoltenVK may expose only one of the two.
        source = DISPATCHER.read_text(encoding="utf-8")
        self.assertIn("BOXEDWINE_X64_VK_ALIASES(VKB_ALIAS_FILL)", source)
        self.assertIn("commandIndexForAlias", source)
        self.assertIn("gCommandAlias[index]", source)


class MarshalContract(unittest.TestCase):
    """The dispatcher copies every structure that crosses the bridge instead of
    handing MoltenVK the guest's own pointers.

    The lane's native map is the identity only for the high lane: canonical low
    addresses are served through an alias and Wine's top-down arena -- where
    every thread stack lives, and where DXVK therefore builds its create-infos
    -- is served from a relocated host block. An earlier revision passed the
    guest pointers straight through and MoltenVK followed a
    VkInstanceCreateInfo at a 0x7ffffe... stack address into unmapped host
    memory. These tests hold the marshal in place.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.pairs = chain_structs()

    def test_no_guest_pointer_is_cast_straight_to_a_vulkan_structure(self) -> None:
        # The shape of the bug: `(const VkInstanceCreateInfo*)CP(1)`, a guest
        # address reinterpreted as a host one. Only Vulkan HANDLES, which are
        # values the driver itself handed out, may come through untranslated,
        # and the dispatcher spells that H(n).
        for macro in ("#define P(n)", "#define CP(n)"):
            with self.subTest(macro=macro):
                self.assertNotIn(macro, self.source)
        stray = re.findall(r"\(const Vk\w+\*\)\s*(?:CP|P)\(", self.source)
        self.assertEqual(stray, [])

    def test_a_pointer_that_cannot_be_translated_is_a_vulkan_error(self) -> None:
        self.assertIn("status=bad-pointer", self.source)
        self.assertIn("VK_ERROR_INITIALIZATION_FAILED", self.source)
        self.assertIn("VK_ERROR_UNKNOWN", self.source)

    def test_the_translation_uses_the_shared_alias_contract(self) -> None:
        # The same arithmetic the ARM64 translator emits, not a second copy of
        # it: include/guest_low_alias.h is the one definition both use.
        self.assertIn('#include "guest_low_alias.h"', self.source)
        self.assertIn("boxedvn::guestToHostAddress", self.source)
        self.assertIn("boxedvn::guestRangeHostable", self.source)
        # vkMapMemory hands a host address back; a guest can only hold the
        # canonical form of it.
        self.assertIn("boxedvn::hostToGuestAddress", self.source)

    def test_every_chain_structure_is_a_real_vulkan_structure(self) -> None:
        types = vulkan_structure_types()
        structs = vulkan_struct_names()
        for stype, struct in self.pairs:
            with self.subTest(stype=stype):
                self.assertIn(stype, types,
                              f"{stype} is not an sType vulkan_core.h defines")
                self.assertIn(struct, structs,
                              f"{struct} is not a structure vulkan_core.h defines")

    def test_no_structure_type_is_listed_twice(self) -> None:
        seen = [stype for stype, _struct in self.pairs]
        duplicates = sorted({name for name in seen if seen.count(name) > 1})
        self.assertEqual(duplicates, [],
                         f"listed more than once: {duplicates}")

    def test_the_d3d9_bootstrap_structures_are_all_covered(self) -> None:
        # A structure absent from the table is dropped from its chain, which
        # for any of these means DXVK is told something it did not ask about.
        listed = {stype for stype, _struct in self.pairs}
        for stype in (
                "VK_STRUCTURE_TYPE_APPLICATION_INFO",
                "VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2",
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2",
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2",
                "VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO",
                "VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT",
                "VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT",
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT",
                "VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO",
                "VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO",
                "VK_STRUCTURE_TYPE_SUBMIT_INFO",
                "VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR",
                "VK_STRUCTURE_TYPE_PRESENT_INFO_KHR",
                "VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR",
                "VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR",
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR"):
            with self.subTest(stype=stype):
                self.assertIn(stype, listed)

    def test_a_node_carrying_a_guest_callback_is_dropped_not_forwarded(self) -> None:
        # pfnUserCallback is x86-64 code; the host is arm64 and cannot call it.
        # DXVK attaches this node to its VkInstanceCreateInfo whenever
        # validation is on, so it is on the ordinary startup path.
        listed = {stype for stype, _struct in self.pairs}
        callbacks = (
            "VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT",
            "VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT",
            "VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT")
        callback_body = self.source[
            self.source.index("bool nodeCarriesGuestCallback("):
            self.source.index("// ---- The marshal ---")]
        for stype in callbacks:
            with self.subTest(stype=stype):
                self.assertNotIn(stype, listed,
                                 f"{stype} must not be copyable; it holds a "
                                 "guest function pointer")
                self.assertIn(stype, callback_body)

    def test_an_unknown_chain_node_is_named_rather_than_followed(self) -> None:
        self.assertIn("status=dropped-chain", self.source)
        self.assertIn("unknown-stype", self.source)
        self.assertIn("guest-callback", self.source)

    def test_the_allocation_callbacks_rule_survived_the_rewrite(self) -> None:
        # Same rule as before the marshal, restated against the new call
        # sites: a VkAllocationCallbacks holds guest function pointers.
        self.assertNotIn("VkAllocationCallbacks", self.source)


class NewCommandContract(unittest.TestCase):
    """The commands the marshal work added, and the ABI bump that names them."""

    def setUp(self) -> None:
        self.commands = bridge_commands()

    def test_the_memory_and_sync_commands_dxvk_needs_are_present(self) -> None:
        for name in ("GetBufferMemoryRequirements2", "GetImageMemoryRequirements2",
                     "BindBufferMemory2", "BindImageMemory2",
                     "GetPhysicalDeviceExternalBufferProperties",
                     "GetPhysicalDeviceExternalSemaphoreProperties",
                     "GetPhysicalDeviceExternalFenceProperties",
                     "GetDeviceQueue2", "GetImageSubresourceLayout",
                     "GetSemaphoreCounterValue", "WaitSemaphores",
                     "SignalSemaphore", "AcquireNextImage2KHR"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_the_whole_swapchain_path_is_dispatchable(self) -> None:
        for name in ("CreateSwapchainKHR", "GetSwapchainImagesKHR",
                     "AcquireNextImageKHR", "QueuePresentKHR", "MapMemory",
                     "UnmapMemory", "FlushMappedMemoryRanges",
                     "InvalidateMappedMemoryRanges"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_every_promoted_command_carries_its_khr_spelling(self) -> None:
        # A caller on a 1.0 instance asks for the KHR name; a NULL answer is
        # how DXVK concludes the driver cannot do it at all.
        aliases = validator.read_bridge_aliases(BRIDGE_HEADER)
        for alias, core in (
                ("GetBufferMemoryRequirements2KHR", "GetBufferMemoryRequirements2"),
                ("GetImageMemoryRequirements2KHR", "GetImageMemoryRequirements2"),
                ("BindBufferMemory2KHR", "BindBufferMemory2"),
                ("BindImageMemory2KHR", "BindImageMemory2"),
                ("GetPhysicalDeviceExternalBufferPropertiesKHR",
                 "GetPhysicalDeviceExternalBufferProperties"),
                ("GetSemaphoreCounterValueKHR", "GetSemaphoreCounterValue"),
                ("WaitSemaphoresKHR", "WaitSemaphores"),
                ("SignalSemaphoreKHR", "SignalSemaphore")):
            with self.subTest(alias=alias):
                self.assertEqual(aliases.get(alias), core)

    def test_the_abi_version_was_bumped_for_the_marshal(self) -> None:
        # A container holding a shim built against version 2 has no entry
        # point for the commands version 3 added, and answering NULL for them
        # reads as a driver limitation rather than a stale container. The
        # shim's own check turns the mismatch into a named refusal.
        text = BRIDGE_HEADER.read_text(encoding="utf-8")
        version = int(re.search(
            r"#define BOXEDWINE_X64_VK_ABI_VERSION\s+(\d+)U", text).group(1))
        self.assertGreaterEqual(version, 3)


class ImportContract(unittest.TestCase):
    def test_the_three_blocks_are_disjoint(self) -> None:
        required, optional, not_imported = validator.read_import_contract(IMPORTS)
        self.assertEqual(required & optional, set())
        self.assertEqual(required & not_imported, set(),
                         "a name cannot be both dlsym'd and not imported")
        self.assertEqual(optional & not_imported, set())

    def test_every_required_symbol_is_dispatchable(self) -> None:
        required, _optional, _not_imported = validator.read_import_contract(IMPORTS)
        dispatchable = {"vk" + name for name in bridge_commands()}
        dispatchable |= set(validator.ALWAYS_EXPORTED)
        self.assertEqual(sorted(required - dispatchable), [])

    def test_the_not_imported_block_carries_the_names_ci_reported(self) -> None:
        # The first CI run of this validator failed on exactly these strings.
        # Each is in winex11.so and none is dlsym'd out of the Vulkan library;
        # the reasons are recorded above the block in the contract file.
        _required, _optional, not_imported = validator.read_import_contract(IMPORTS)
        for name in ("vkCreateWin32SurfaceKHR",
                     "vkGetPhysicalDeviceWin32PresentationSupportKHR",
                     "vkGetPhysicalDeviceProperties2KHR",
                     "vkGetPhysicalDeviceMemoryProperties2KHR",
                     "vkGetRandROutputDisplayEXT"):
            with self.subTest(name=name):
                self.assertIn(name, not_imported)

    def test_the_two_khr_names_dxvk_queries_are_still_served(self) -> None:
        # Recorded as not-dlsym'd, but DXVK asks for both through
        # vkGetInstanceProcAddr on a 1.0 instance. Being in [not-imported]
        # must not mean the bridge cannot answer them.
        aliases = {"vk" + name for name in validator.read_bridge_aliases(BRIDGE_HEADER)}
        self.assertIn("vkGetPhysicalDeviceProperties2KHR", aliases)
        self.assertIn("vkGetPhysicalDeviceMemoryProperties2KHR", aliases)


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
        self.required, _, _ = validator.read_import_contract(IMPORTS)
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

    def driver_naming(self, *names: str) -> pathlib.Path:
        """A stand-in winex11.so whose string table holds `names`, plus every
        symbol the contract records as dlsym'd (absent one of those is its own
        failure, which other tests cover)."""
        required, _, _ = validator.read_import_contract(IMPORTS)
        blob = b"\x00".join(
            name.encode() for name in sorted(required) + list(names))
        driver = self.dir / "winex11.so"
        driver.write_bytes(b"\x00" + blob + b"\x00")
        return driver

    def test_measures_command_strings_out_of_a_driver(self) -> None:
        driver = self.dir / "winex11.so"
        driver.write_bytes(b"\x00vkQueuePresentKHR\x00vkSomethingNewKHR\x00")
        measured = validator.measure_winex11_vulkan_strings(driver)
        self.assertIn("vkQueuePresentKHR", measured)
        self.assertIn("vkSomethingNewKHR", measured)

    def test_does_not_measure_a_name_inside_a_longer_identifier(self) -> None:
        # The false positives that failed the first CI run: winex11's own
        # exported X11DRV_* wrappers and its internal wine_vk_* helpers.
        driver = self.dir / "winex11.so"
        driver.write_bytes(
            b"\x00X11DRV_vkCreateWin32SurfaceKHR\x00wine_vk_init\x00"
            b"wine_vk_instance_convert_create_info\x00")
        measured = validator.measure_winex11_vulkan_strings(driver)
        self.assertEqual(measured, set())

    def test_accepts_a_driver_naming_only_allow_listed_extras(self) -> None:
        # The five names CI reported. All are recorded under [not-imported],
        # so the build must pass.
        path = self.write(build_library("libvulkan.so.1", self.exports))
        driver = self.driver_naming(
            "vkCreateWin32SurfaceKHR",
            "vkGetPhysicalDeviceWin32PresentationSupportKHR",
            "vkGetPhysicalDeviceProperties2KHR",
            "vkGetPhysicalDeviceMemoryProperties2KHR",
            "vkGetRandROutputDisplayEXT")
        counts = validator.validate(path, IMPORTS, BRIDGE_HEADER, driver)
        self.assertGreater(counts["driver_named"], 0)

    def test_an_unrecorded_name_is_a_note_not_a_failure(self) -> None:
        # Undecidable from the binary: a dlsym argument and any other literal
        # are both just strings. It gets reported for a human to check, not
        # turned into a build failure.
        path = self.write(build_library("libvulkan.so.1", self.exports))
        driver = self.driver_naming("vkSomethingNobodyRecorded")
        validator.validate(path, IMPORTS, BRIDGE_HEADER, driver)

    def test_refuses_a_driver_that_no_longer_names_a_required_symbol(self) -> None:
        # The drift that *is* decidable, and the one that matters: Wine
        # renamed or dropped a symbol the contract says it dlsyms.
        path = self.write(build_library("libvulkan.so.1", self.exports))
        required, _, _ = validator.read_import_contract(IMPORTS)
        kept = sorted(required - {"vkQueuePresentKHR"})
        driver = self.dir / "winex11.so"
        driver.write_bytes(b"\x00" + b"\x00".join(n.encode() for n in kept) + b"\x00")
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
