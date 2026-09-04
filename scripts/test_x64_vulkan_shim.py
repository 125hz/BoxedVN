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
DISPATCHER_HEADER = REPO / "source" / "vulkan" / "vulkanbridge64.h"
THREAD_HEADER = REPO / "include" / "kthread.h"
SYSCALL64 = REPO / "source" / "kernel" / "syscall64.cpp"
VULKAN_CORE = REPO / "source" / "vulkan" / "vk" / "vulkan_core.h"
IA32_LANE = REPO / "source" / "vulkan" / "vulkancommon.cpp"
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

def bridge_error_codes() -> dict[str, int]:
    """Parse BOXEDWINE_X64_VK_E_* out of the ABI header, resolving the base.

    Both spellings are accepted -- base-relative, which is how they are
    written, and a plain literal, which is how they used to be. Parsing the
    old form too is the point: a code edited back into a literal has to be
    judged against the 32-bit floor and reported as too high, not quietly
    dropped from the table.
    """
    text = BRIDGE_HEADER.read_text(encoding="utf-8")
    base = int(re.search(
        r"#define BOXEDWINE_X64_VK_E_BASE\s+\((-0x[0-9a-fA-F]+)LL\)",
        text).group(1), 16)
    codes = {"BASE": base}
    for name, offset in re.findall(
            r"#define BOXEDWINE_X64_VK_E_(\w+)\s+"
            r"\(BOXEDWINE_X64_VK_E_BASE - (\d+)\)", text):
        codes[name] = base - int(offset)
    for name, literal in re.findall(
            r"#define BOXEDWINE_X64_VK_E_(\w+)\s+\((-(?:0x)?[0-9a-fA-F]+)L*\)",
            text):
        if name == "BASE":
            continue
        codes[name] = int(literal, 16 if literal.startswith("-0x") else 10)
    return codes


def vulkan_result_values() -> set[int]:
    """Every VkResult vulkan_core.h defines, successes and errors alike."""
    text = VULKAN_CORE.read_text(encoding="utf-8")
    block = text[text.index("typedef enum VkResult {"):]
    block = block[:block.index("} VkResult;")]
    return {int(v) for v in re.findall(r"=\s*(-?\d+)", block)}


def klog_calls(source: str) -> list[str]:
    """Every klog_fmt(...) statement in `source`, parens balanced."""
    calls = []
    for start in (m.end() - 1 for m in re.finditer(r"klog_fmt\s*\(", source)):
        depth = 0
        for i in range(start, len(source)):
            if source[i] == "(":
                depth += 1
            elif source[i] == ")":
                depth -= 1
                if depth == 0:
                    calls.append(source[start:i + 1])
                    break
    return calls



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

    def test_no_entry_point_packs_more_words_than_the_bridge_accepts(self) -> None:
        # The host refuses a count above BOXEDWINE_X64_VK_MAX_ARGS outright, so
        # an entry point that packs one word too many is a command that always
        # answers BOXEDWINE_X64_VK_E_ARGS -- and nothing but a device run would
        # say so. vkCmdWaitEvents is the widest at eleven.
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        limit = int(re.search(r"#define BOXEDWINE_X64_VK_MAX_ARGS\s+(\d+)U",
                              header).group(1))
        source = SHIM_SOURCE.read_text(encoding="utf-8")
        for match in re.finditer(r"\bBW_[RVB]\s*\(", source):
            start = match.end() - 1
            depth = 0
            fields = 1
            for i in range(start, len(source)):
                if source[i] in "([":
                    depth += 1
                elif source[i] in ")]":
                    depth -= 1
                    if depth == 0:
                        break
                elif source[i] == "," and depth == 1:
                    fields += 1
            # The first field is the operation name, not an argument word.
            words = fields - 1
            with self.subTest(call=source[match.start():start + 40]):
                self.assertLessEqual(words, limit)

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


class SurfaceExtensionContract(unittest.TestCase):
    """The platform surface extension is translated in both directions.

    Wine's winevulkan asks winex11.drv which host surface extension to use and
    substitutes that name for the application's VK_KHR_win32_surface before the
    call reaches this bridge. winex11.drv is an X11 driver, so the name that
    arrives is VK_KHR_xlib_surface -- which MoltenVK does not have. A device run
    showed the consequence precisely: Wine's own adapter probe succeeded (it
    enables no surface extension), and DXVK's vkCreateInstance came back
    VK_ERROR_EXTENSION_NOT_PRESENT the moment it asked for a surface. The IA-32
    lane has always translated; these tests hold the 64-bit lane to the same
    contract, and to the same spellings.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.ia32 = IA32_LANE.read_text(encoding="utf-8")

    def enumerate_case(self) -> str:
        start = self.source.index("case VKB_EnumerateInstanceExtensionProperties:")
        return self.source[start:self.source.index("case VKB_CreateInstance:", start)]

    def create_instance_case(self) -> str:
        start = self.source.index("case VKB_CreateInstance:")
        return self.source[start:self.source.index("case VKB_DestroyInstance:", start)]

    def test_both_directions_exist(self) -> None:
        self.assertIn("renameHostSurfaceExtension", self.enumerate_case())
        self.assertIn("substituteHostSurfaceExtension", self.create_instance_case())

    def test_the_guest_facing_name_matches_the_ia32_lane(self) -> None:
        # Both lanes report the same name to Wine, because both are serving
        # the same winex11.drv.
        self.assertIn('kGuestSurfaceExtension = "VK_KHR_xlib_surface"', self.source)
        self.assertIn('"VK_KHR_xlib_surface"', self.ia32)

    def ia32_create_instance(self) -> str:
        start = self.ia32.index("void vk_CreateInstance(")
        return self.ia32[start:self.ia32.index("void vk_DestroyInstance2(", start)]

    def test_the_ios_host_name_matches_the_ia32_lane(self) -> None:
        # SDL's UIKit Vulkan backend is a CAMetalLayer, so both lanes hand
        # MoltenVK VK_EXT_metal_surface and not VK_MVK_macos_surface.
        block = self.source[self.source.index("#if defined(BOXEDWINE_IOS)"):]
        block = block[:block.index("#elif")]
        self.assertIn('"VK_EXT_metal_surface"', block)
        ia32 = self.ia32_create_instance()
        ios = ia32[ia32.index("#ifdef BOXEDWINE_IOS"):]
        ios = ios[:ios.index("#elif")]
        self.assertIn('"VK_EXT_metal_surface"', ios)

    def test_every_name_the_ia32_lane_recognises_is_recognised_here(self) -> None:
        # vulkancommon.cpp's enumeration accepts these three as "the host's
        # platform surface"; a name it would have translated and this lane
        # would not is a device that works on one lane and not the other.
        aliases = self.source[self.source.index("kHostSurfaceAliases[] = {"):]
        aliases = aliases[:aliases.index("};")]
        for name in ("VK_EXT_metal_surface", "VK_MVK_macos_surface",
                     "VK_KHR_win32_surface"):
            with self.subTest(name=name):
                self.assertIn(name, aliases)

    def test_moltenvks_second_surface_spelling_is_recognised(self) -> None:
        # MoltenVK reports VK_EXT_metal_surface AND VK_MVK_ios_surface. Both
        # have to be recognised, and exactly one entry may ever be renamed, or
        # the reported list carries VK_KHR_xlib_surface twice.
        aliases = self.source[self.source.index("kHostSurfaceAliases[] = {"):]
        aliases = aliases[:aliases.index("};")]
        self.assertIn("VK_MVK_ios_surface", aliases)
        body = self.source[self.source.index("U32 renameHostSurfaceExtension("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("return 1;", body)
        self.assertNotIn("++renamed", body)

    def test_the_rename_cannot_change_the_property_count(self) -> None:
        # The two-call idiom: the first call passes a null array and only the
        # count comes back, so a rename that added a name would make the two
        # calls disagree and overrun the caller's array.
        body = self.source[self.source.index("U32 renameHostSurfaceExtension("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("if (!properties || !count)", body)
        case = self.enumerate_case()
        after = case[case.index("PFN_vkEnumerateInstanceExtensionProperties)raw"):]
        self.assertNotIn("*countSlot =", after,
                         "the count the driver reported must reach the guest "
                         "unchanged")

    def test_the_rename_is_bounded_by_what_the_driver_wrote(self) -> None:
        # The shadow array is sized from the caller's capacity; the driver may
        # write fewer and return VK_INCOMPLETE.
        case = self.enumerate_case()
        self.assertIn("const U32 capacity", case)
        self.assertIn("VK_INCOMPLETE", case)
        self.assertIn("written", case)

    def test_the_substitution_happens_before_the_driver_call(self) -> None:
        case = self.create_instance_case()
        self.assertLess(case.index("substituteHostSurfaceExtension"),
                        case.index("PFN_vkCreateInstance)raw"))

    def test_there_is_no_device_level_translation(self) -> None:
        # Every platform surface extension is instance-level. A rewrite in a
        # device-level call would corrupt a device extension list, and the
        # IA-32 lane does not do one either: both of its rewrites live in
        # vk_CreateInstance and vk_EnumerateInstanceExtensionProperties.
        for marker in ("case VKB_CreateDevice:",
                       "case VKB_EnumerateDeviceExtensionProperties:"):
            with self.subTest(case=marker):
                start = self.source.index(marker)
                body = self.source[start:start + 900]
                self.assertNotIn("SurfaceExtension", body)
        instance_only = (self.ia32_create_instance()
                         + self.ia32[self.ia32.index(
                             "void vk_EnumerateInstanceExtensionProperties("):][:2500])
        self.assertEqual(self.ia32.count('"VK_KHR_xlib_surface"'),
                         instance_only.count('"VK_KHR_xlib_surface"'))

    def test_a_driver_with_no_platform_surface_is_named_not_ignored(self) -> None:
        # DXVK only asks for extensions the enumeration claimed, so a missing
        # platform surface surfaces as an unexplained failure much later.
        self.assertIn("status=no-platform-surface", self.source)


class InstanceLifetimeContract(unittest.TestCase):
    """vkDestroyInstance must not answer BOXEDWINE_X64_VK_E_NOPROC.

    MoltenVK is the driver directly, with no Khronos loader, so
    vkGetInstanceProcAddr(VK_NULL_HANDLE, name) answers only for the four
    global commands. An earlier revision resolved every command through a
    single "current instance" that vkDestroyInstance cleared, so a second
    vkDestroyInstance -- which Wine's adapter probe issues -- had nothing left
    to resolve through and came back -8 for a command that returns void.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")

    def test_the_bridge_tracks_live_instances(self) -> None:
        for symbol in ("gLiveInstances", "instanceIsLive", "noteInstanceCreated",
                       "forgetInstance"):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, self.source)

    def test_resolution_can_use_the_instance_a_command_carries(self) -> None:
        # vkDestroyInstance has to resolve while the instance it is about to
        # destroy is still the only live one.
        self.assertIn("PFN_vkVoidFunction hostProc(int index, VkInstance resolveWith)",
                      self.source)
        self.assertIn("resolutionInstance", self.source)
        body = self.source[self.source.index("VkInstance resolutionInstance("):]
        body = body[:body.index("\n}\n")]
        for command in ("VKB_DestroyInstance", "VKB_EnumeratePhysicalDevices",
                        "VKB_DestroySurfaceKHR"):
            with self.subTest(command=command):
                self.assertIn(command, body)

    def test_a_destroy_of_a_dead_instance_is_settled_before_resolution(self) -> None:
        # Both because resolution would dereference the stale handle, and
        # because the NOPROC guard sits between the two.
        body = self.source[self.source.index("S64 dispatchCommand("):]
        body = body[:body.index("switch (index) {")]
        self.assertIn("instanceIsLive", body)
        self.assertLess(body.index("instanceIsLive"),
                        body.index("BOXEDWINE_X64_VK_E_NOPROC"),
                        "the liveness check must come before the NOPROC guard, "
                        "or a second vkDestroyInstance is still answered -8")
        self.assertIn("status=not-live", body)

    def test_a_destroy_of_a_dead_instance_reports_success(self) -> None:
        # vkDestroyInstance returns void; there is no error to report and none
        # may be invented, because the guest reads the word back as a VkResult
        # for the commands that do have one.
        body = self.source[self.source.index("S64 dispatchCommand("):]
        body = body[:body.index("switch (index) {")]
        tail = body[body.index("status=not-live"):]
        self.assertIn("return 0;", tail)

    def test_destroying_an_instance_repoints_rather_than_clears(self) -> None:
        # A second live instance must keep resolving after the first goes.
        body = self.source[self.source.index("bool forgetInstance("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("gInstance = gLiveInstances[i]", body)
        # The dispatch switch's own case, not resolutionInstance's.
        switch = self.source[self.source.index("S64 dispatchCommand("):]
        switch = switch[switch.index("switch (index) {"):]
        case = switch[switch.index("case VKB_DestroyInstance:"):]
        case = case[:case.index("case VKB_EnumeratePhysicalDevices:")]
        self.assertIn("forgetInstance", case)
        self.assertNotIn("gInstance = VK_NULL_HANDLE", case,
                         "clearing the current instance inline is what stranded "
                         "every instance-level command")


class BridgeErrorCodeContract(unittest.TestCase):
    """A bridge refusal must never be readable as a VkResult.

    The codes were -1 through -8, on a comment's claim that the Vulkan errors
    were "far from these". The core VkResult errors are -1 to -13, so every
    bridge code had a VkResult sitting on top of it. A device log is the only
    diagnostic channel this lane has, and two investigations began by reading
    `status=-7` as VK_ERROR_EXTENSION_NOT_PRESENT and `status=-8` as
    VK_ERROR_FEATURE_NOT_PRESENT when both were bridge refusals. These tests
    make that class of confusion unrepresentable rather than merely unlikely.
    """

    def setUp(self) -> None:
        self.codes = bridge_error_codes()
        self.source = DISPATCHER.read_text(encoding="utf-8")

    def test_every_code_is_below_the_32_bit_floor(self) -> None:
        # VkResult is a 32-bit signed enum and this hostcall returns a 64-bit
        # word, so "does not fit in an int32" is an exact test that no future
        # Vulkan header can invalidate -- unlike "is not currently used".
        int32_min = -(2 ** 31)
        for name, value in sorted(self.codes.items()):
            with self.subTest(code=name):
                self.assertLess(value, int32_min,
                                f"BOXEDWINE_X64_VK_E_{name} ({value}) is "
                                "inside the range a VkResult can hold")

    def test_no_code_collides_with_a_vkresult(self) -> None:
        results = vulkan_result_values()
        self.assertIn(-7, results)   # the collision that started this
        self.assertIn(-8, results)
        for name, value in sorted(self.codes.items()):
            with self.subTest(code=name):
                self.assertNotIn(value, results)

    def test_the_codes_are_distinct(self) -> None:
        values = sorted(self.codes.values())
        self.assertEqual(len(values), len(set(values)))

    def test_the_table_is_complete(self) -> None:
        for name in ("BADOP", "BUFFER", "FAULT", "ARGS", "UNIMPL", "NOHOST",
                     "MEMORY", "NOPROC"):
            with self.subTest(code=name):
                self.assertIn(name, self.codes)

    def test_the_historical_ordinal_is_recoverable(self) -> None:
        # An old log printed -7; the base minus the new code still gives 7, so
        # a pre-existing log stays readable against the current header.
        base = self.codes["BASE"]
        for name, value in sorted(self.codes.items()):
            if name == "BASE":
                continue
            with self.subTest(code=name):
                self.assertIn(base - value, range(1, 9))

    def test_a_predicate_exists_for_telling_the_two_apart(self) -> None:
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        self.assertIn("#define BOXEDWINE_X64_VK_IS_ERROR(result)", header)
        self.assertIn("BOXEDWINE_X64_VK_E_BASE", header)

    def test_the_guest_shim_never_hands_a_refusal_to_the_application(self) -> None:
        # Every entry point narrows the 64-bit word to a 32-bit VkResult. A
        # refusal reaching that cast arrives at the application as whatever
        # falls out of the truncation.
        shim = SHIM_SOURCE.read_text(encoding="utf-8")
        call = shim[shim.index("static int64_t bw_call("):]
        call = call[:call.index("\n}\n")]
        self.assertIn("BOXEDWINE_X64_VK_IS_ERROR", call)
        self.assertIn("VK_ERROR_INITIALIZATION_FAILED", call)

    def test_every_logged_code_uses_a_64_bit_conversion(self) -> None:
        # The codes are long long now. Passing one to %d reads the wrong
        # number of bytes off the varargs list, which would corrupt the very
        # log line the renumbering exists to make readable.
        for call in klog_calls(self.source):
            if "BOXEDWINE_X64_VK_E_" not in call:
                continue
            with self.subTest(call=call.split(",")[0][:60]):
                self.assertIn("(long long)BOXEDWINE_X64_VK_E_", call,
                              "a bridge code must be widened explicitly")
                self.assertNotIn("status=%d", call,
                                 "a bridge code needs %lld, not %d")

    def test_the_abi_version_was_bumped_for_the_new_range(self) -> None:
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        version = int(re.search(
            r"#define BOXEDWINE_X64_VK_ABI_VERSION\s+(\d+)U", header).group(1))
        self.assertGreaterEqual(version, 4)


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


class RecordingHalfContract(unittest.TestCase):
    """The command-recording half: everything DXVK needs between a device and a
    presented frame.

    Device logs had DXVK reaching instance creation and then a device, at which
    point it records command buffers -- and the bridge carried not one vkCmd*,
    no command pool, no pipeline and no descriptor. Every command below is on
    that path. The tests are shaped like the rest of this file: they hold the
    four files together, and they pin the handful of marshalling decisions that
    are not mechanical.
    """

    def setUp(self) -> None:
        self.commands = bridge_commands()
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.shim = SHIM_SOURCE.read_text(encoding="utf-8")
        self.pairs = chain_structs()

    def case_body(self, name: str) -> str:
        """The dispatcher's case for one command, up to the next case label."""
        start = self.source.index(f"case VKB_{name}:")
        rest = self.source[start + 1:]
        end = re.search(r"^\s*case VKB_\w+:", rest, re.MULTILINE)
        return rest[:end.start()] if end else rest

    def test_the_command_buffer_lifecycle_is_present(self) -> None:
        for name in ("CreateCommandPool", "DestroyCommandPool",
                     "ResetCommandPool", "AllocateCommandBuffers",
                     "FreeCommandBuffers", "BeginCommandBuffer",
                     "EndCommandBuffer", "ResetCommandBuffer", "QueueSubmit",
                     "QueueSubmit2", "QueueWaitIdle", "DeviceWaitIdle"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_the_synchronisation_objects_are_present(self) -> None:
        for name in ("CreateFence", "DestroyFence", "ResetFences",
                     "GetFenceStatus", "WaitForFences", "CreateSemaphore",
                     "DestroySemaphore", "GetSemaphoreCounterValue",
                     "WaitSemaphores", "SignalSemaphore", "CreateEvent",
                     "DestroyEvent", "GetEventStatus", "SetEvent",
                     "ResetEvent"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_the_resource_and_descriptor_objects_are_present(self) -> None:
        for name in ("CreateBufferView", "DestroyBufferView", "CreateSampler",
                     "DestroySampler", "CreateShaderModule",
                     "DestroyShaderModule", "CreatePipelineCache",
                     "DestroyPipelineCache", "GetPipelineCacheData",
                     "MergePipelineCaches", "CreatePipelineLayout",
                     "DestroyPipelineLayout", "CreateDescriptorSetLayout",
                     "DestroyDescriptorSetLayout",
                     "GetDescriptorSetLayoutSupport", "CreateDescriptorPool",
                     "DestroyDescriptorPool", "ResetDescriptorPool",
                     "AllocateDescriptorSets", "FreeDescriptorSets",
                     "UpdateDescriptorSets", "CreateDescriptorUpdateTemplate",
                     "DestroyDescriptorUpdateTemplate",
                     "UpdateDescriptorSetWithTemplate", "CreateRenderPass",
                     "CreateRenderPass2", "DestroyRenderPass",
                     "CreateFramebuffer", "DestroyFramebuffer",
                     "CreateQueryPool", "DestroyQueryPool",
                     "GetQueryPoolResults", "ResetQueryPool"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_both_pipeline_constructors_are_present(self) -> None:
        for name in ("CreateGraphicsPipelines", "CreateComputePipelines",
                     "DestroyPipeline"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_the_frame_recording_commands_are_present(self) -> None:
        # A frame: begin a scope, bind, set the dynamic state, draw, barrier,
        # end. Every one of these appears in a DXVK frame.
        for name in ("CmdBeginRenderPass", "CmdEndRenderPass",
                     "CmdBeginRenderPass2", "CmdEndRenderPass2",
                     "CmdBeginRendering", "CmdEndRendering", "CmdBindPipeline",
                     "CmdBindDescriptorSets", "CmdBindIndexBuffer",
                     "CmdBindVertexBuffers", "CmdBindVertexBuffers2",
                     "CmdSetViewport", "CmdSetScissor", "CmdSetBlendConstants",
                     "CmdSetStencilReference", "CmdSetViewportWithCount",
                     "CmdSetScissorWithCount", "CmdSetCullMode",
                     "CmdSetFrontFace", "CmdSetPrimitiveTopology",
                     "CmdSetDepthTestEnable", "CmdSetStencilOp", "CmdDraw",
                     "CmdDrawIndexed", "CmdDrawIndirect",
                     "CmdDrawIndexedIndirect", "CmdDispatch",
                     "CmdDispatchIndirect", "CmdCopyBuffer", "CmdCopyImage",
                     "CmdBlitImage", "CmdCopyBufferToImage",
                     "CmdCopyImageToBuffer", "CmdCopyBuffer2", "CmdCopyImage2",
                     "CmdBlitImage2", "CmdResolveImage", "CmdResolveImage2",
                     "CmdClearAttachments", "CmdClearColorImage",
                     "CmdClearDepthStencilImage", "CmdPipelineBarrier",
                     "CmdPipelineBarrier2", "CmdPushConstants",
                     "CmdBeginQuery", "CmdEndQuery", "CmdResetQueryPool",
                     "CmdWriteTimestamp", "CmdCopyQueryPoolResults",
                     "CmdExecuteCommands"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_the_deliberate_extension_omissions_stay_omitted(self) -> None:
        # Extension commands only; the core ones are held by
        # CoreCompletenessContract below. Each is named in the header with the
        # reason, and a half-marshalled version would be worse than none.
        for name in ("CmdSetVertexInputEXT", "CmdSetColorBlendEnableEXT",
                     "CmdSetColorWriteMaskEXT", "CmdTraceRaysKHR",
                     "CmdBuildAccelerationStructuresKHR", "CmdDecodeVideoKHR"):
            with self.subTest(command=name):
                self.assertNotIn(name, self.commands)
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        for reason in ("vkCmdSetVertexInputEXT",
                       "VK_EXT_extended_dynamic_state3",
                       "acceleration-structure"):
            with self.subTest(reason=reason):
                self.assertIn(reason, header,
                              "an omission has to be recorded with its reason")

    def test_the_recording_structures_are_all_in_the_chain_table(self) -> None:
        # A structure absent from the table is dropped from its chain, which
        # for a create-info the command NAMES means the command is handed
        # nothing at all.
        listed = {stype for stype, _struct in self.pairs}
        for stype in (
                "VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO",
                "VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO",
                "VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO",
                "VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO",
                "VK_STRUCTURE_TYPE_SUBMIT_INFO_2",
                "VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO",
                "VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO",
                "VK_STRUCTURE_TYPE_EVENT_CREATE_INFO",
                "VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO",
                "VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO",
                "VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO",
                "VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO",
                "VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO",
                "VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO",
                "VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO",
                "VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET",
                "VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET",
                "VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO",
                "VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2",
                "VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2",
                "VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2",
                "VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO",
                "VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO",
                "VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO",
                "VK_STRUCTURE_TYPE_RENDERING_INFO",
                "VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO",
                "VK_STRUCTURE_TYPE_MEMORY_BARRIER",
                "VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER",
                "VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER",
                "VK_STRUCTURE_TYPE_DEPENDENCY_INFO",
                "VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2",
                "VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2",
                "VK_STRUCTURE_TYPE_BUFFER_COPY_2"):
            with self.subTest(stype=stype):
                self.assertIn(stype, listed)

    def test_dxvk_2x_dynamic_rendering_and_library_nodes_are_carried(self) -> None:
        # DXVK 2.x builds pipelines with dynamic rendering and, where the
        # driver has it, graphics pipeline libraries. Dropping either node
        # would hand MoltenVK a pipeline that describes a different frame.
        listed = {stype for stype, _struct in self.pairs}
        for stype in ("VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO",
                      "VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT",
                      "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR",
                      "VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO"):
            with self.subTest(stype=stype):
                self.assertIn(stype, listed)

    def test_a_write_descriptor_set_selects_one_array_and_nulls_the_rest(self) -> None:
        # VkWriteDescriptorSet's three payload pointers are read one at a time,
        # chosen by descriptorType; the other two are ignored and hold whatever
        # the caller left there. Forwarding an ignored one would hand MoltenVK
        # a guest address.
        body = self.source[
            self.source.index("case (U32)VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET:"):]
        body = body[:body.index(
            "case (U32)VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK:")]
        for member in ("pImageInfo", "pBufferInfo", "pTexelBufferView"):
            with self.subTest(member=member):
                self.assertIn(f"info->{member} =", body)
        for descriptor_type in ("VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER",
                                "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER",
                                "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC",
                                "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER",
                                "VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK"):
            with self.subTest(descriptor_type=descriptor_type):
                self.assertIn(descriptor_type, body)
        # And a type it cannot size is a refusal, not a guess.
        self.assertIn("unsized-descriptor-type", body)

    def test_an_immutable_sampler_array_is_read_only_where_it_is_defined(self) -> None:
        # pImmutableSamplers is ignored -- and therefore uninitialised -- for
        # every descriptor type but the two sampler ones.
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:"):]
        body = body[:body.index("break;\n    }") + 12]
        self.assertIn("VK_DESCRIPTOR_TYPE_SAMPLER", body)
        self.assertIn("VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", body)
        self.assertIn("pImmutableSamplers = nullptr", body)

    def test_the_update_template_span_is_recorded_and_then_required(self) -> None:
        # vkUpdateDescriptorSetWithTemplate's pData has no length in the call:
        # it is described by the entries the template was created with. So the
        # span is measured at creation and an update through a template the
        # bridge has no span for is refused by name.
        for symbol in ("templateDataSpan", "noteTemplateCreated",
                       "templateDataBytes", "forgetTemplate",
                       "descriptorElementSize"):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, self.source)
        create = self.case_body("CreateDescriptorUpdateTemplate")
        self.assertIn("noteTemplateCreated", create)
        update = self.case_body("UpdateDescriptorSetWithTemplate")
        self.assertIn("templateDataBytes", update)
        self.assertIn("status=no-span", update)
        self.assertLess(update.index("templateDataBytes"),
                        update.index("PFN_vkUpdateDescriptorSetWithTemplate"),
                        "the span has to be known before the copy")
        destroy = self.case_body("DestroyDescriptorUpdateTemplate")
        self.assertIn("forgetTemplate", destroy)

    def test_an_extensible_array_never_goes_through_the_flat_copy(self) -> None:
        # A barrier, a submit info and a pipeline create info are all
        # extensible: copying an array of them with the flat inArray/inArrayAt
        # would leave every element's pNext pointing at guest memory.
        # structArrayTyped is the one that mirrors each element's own chain.
        for command in ("CmdPipelineBarrier", "CmdWaitEvents",
                        "CmdWaitEvents2", "UpdateDescriptorSets",
                        "QueueSubmit2", "CreateGraphicsPipelines",
                        "CreateComputePipelines"):
            with self.subTest(command=command):
                self.assertIn("structArrayTyped", self.case_body(command))
        # And the general form of the rule, over the whole dispatcher: nothing
        # the chain table calls extensible may be the element type of a flat
        # copy. This catches the next one as well as these.
        extensible = {struct for _stype, struct in self.pairs}
        for call in re.findall(
                r"\(const (Vk\w+)\*\)\s*m\.inArray(?:At)?\(", self.source):
            with self.subTest(element=call):
                self.assertNotIn(
                    call, extensible,
                    f"{call} is an extensible structure; an array of them has "
                    "to go through structArrayTyped so each element's pNext "
                    "is mirrored")

    def test_a_specialization_constant_block_is_sized_by_its_own_field(self) -> None:
        # VkSpecializationInfo::pData is opaque bytes whose only length is
        # dataSize, and VkSpecializationInfo has no sType, so it cannot go
        # through the chain walker.
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO:"):]
        body = body[:body.index("case (U32)VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT")]
        self.assertIn("VkSpecializationInfo", body)
        self.assertIn("mapEntryCount", body)
        self.assertIn("dataSize", body)
        self.assertIn("info->pName = string(", body)

    def test_a_sample_mask_is_sized_from_the_sample_count(self) -> None:
        # ceil(rasterizationSamples / 32) words. A fixed size would either read
        # past the caller's array or short the driver.
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO:")]
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO:"):]
        body = body[:body.index("case (U32)VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND")]
        self.assertIn("rasterizationSamples", body)
        self.assertIn("+ 31u) / 32u", body)

    def test_an_optional_array_is_distinguished_from_a_required_one(self) -> None:
        # pViewports is null whenever the viewport is dynamic, which is how
        # DXVK builds every pipeline; treating that as a bad pointer would
        # refuse every pipeline creation.
        self.assertIn("inArrayOptional", self.source)
        for marker in ("VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO",
                       "VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO"):
            with self.subTest(structure=marker):
                body = self.source[self.source.index(f"case (U32){marker}:"):]
                body = body[:body.index("break;")]
                self.assertIn("inArrayOptional", body)

    def test_the_compute_stage_is_marshalled_as_an_embedded_structure(self) -> None:
        # VkComputePipelineCreateInfo::stage is a whole
        # VkPipelineShaderStageCreateInfo by value; its pNext and its pName and
        # pSpecializationInfo still have to be marshalled.
        self.assertIn("void embedded(void* host, U32 sType)", self.source)
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:"):]
        body = body[:body.index("break;")]
        self.assertIn("embedded(&info->stage", body)

    def test_a_float_parameter_crosses_as_its_bit_pattern(self) -> None:
        # Casting a float to uint64_t rounds it: a line width of 1.5 would
        # arrive as 1. Both halves reinterpret the object representation
        # instead, and both have to, or the two disagree silently.
        self.assertIn("static uint64_t bw_f32(float value)", self.shim)
        self.assertIn("memcpy(&bits, &value, sizeof bits)", self.shim)
        self.assertIn("float f32(U64 word)", self.source)
        self.assertIn("::memcpy(&value, &bits, sizeof(value))", self.source)
        for command in ("CmdSetLineWidth", "CmdSetDepthBias",
                        "CmdSetDepthBounds"):
            with self.subTest(command=command):
                self.assertIn("F32A(", self.case_body(command))
        # vkCmdSetBlendConstants takes an ARRAY of four floats, which is a
        # pointer and therefore a marshalled block rather than four words.
        blend = self.case_body("CmdSetBlendConstants")
        self.assertNotIn("F32A(", blend)
        self.assertIn("inArrayAt(A(1), 4, sizeof(float))", blend)

    def test_every_new_create_command_forces_a_null_allocator(self) -> None:
        for command in ("PFN_vkCreateCommandPool", "PFN_vkCreateShaderModule",
                        "PFN_vkCreateGraphicsPipelines",
                        "PFN_vkCreateComputePipelines",
                        "PFN_vkCreateDescriptorPool", "PFN_vkCreateRenderPass2",
                        "PFN_vkCreateFramebuffer", "PFN_vkCreateSampler",
                        "PFN_vkCreateDescriptorUpdateTemplate"):
            with self.subTest(command=command):
                index = self.source.index(command)
                self.assertIn("nullptr", self.source[index:index + 400])

    def test_an_output_array_sized_by_its_create_info_is_sized_after_the_copy(self) -> None:
        # vkAllocateCommandBuffers and vkAllocateDescriptorSets take the length
        # of their output array from a field of the create info, which is only
        # readable once the info has been copied into the shadow.
        for command in ("AllocateCommandBuffers", "AllocateDescriptorSets"):
            with self.subTest(command=command):
                body = self.case_body(command)
                self.assertLess(body.index("m.chain(A(1), false)"),
                                body.index("m.outRequired("))
                self.assertIn("A(2),", body)

    def test_the_abi_was_bumped_for_the_recording_half(self) -> None:
        # A container holding a shim built for 4 has no entry point for any of
        # these, and answering NULL for vkCmdDraw reads as a driver that cannot
        # record rather than as a stale container.
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        version = int(re.search(
            r"#define BOXEDWINE_X64_VK_ABI_VERSION\s+(\d+)U", header).group(1))
        self.assertGreaterEqual(version, 5)

    def test_every_recording_alias_resolves_to_a_command(self) -> None:
        # DXVK asks for the KHR/EXT spelling whenever it enabled the extension
        # rather than the core version, which on a 1.0 or 1.1 instance is most
        # of them.
        aliases = validator.read_bridge_aliases(BRIDGE_HEADER)
        for alias, core in (
                ("CmdBeginRenderingKHR", "CmdBeginRendering"),
                ("CmdEndRenderingKHR", "CmdEndRendering"),
                ("CmdPipelineBarrier2KHR", "CmdPipelineBarrier2"),
                ("QueueSubmit2KHR", "QueueSubmit2"),
                ("CmdCopyBuffer2KHR", "CmdCopyBuffer2"),
                ("CmdBlitImage2KHR", "CmdBlitImage2"),
                ("CmdSetCullModeEXT", "CmdSetCullMode"),
                ("CmdSetViewportWithCountEXT", "CmdSetViewportWithCount"),
                ("CmdBindVertexBuffers2EXT", "CmdBindVertexBuffers2"),
                ("CreateRenderPass2KHR", "CreateRenderPass2"),
                ("CmdBeginRenderPass2KHR", "CmdBeginRenderPass2"),
                ("UpdateDescriptorSetWithTemplateKHR",
                 "UpdateDescriptorSetWithTemplate"),
                ("ResetQueryPoolEXT", "ResetQueryPool")):
            with self.subTest(alias=alias):
                self.assertEqual(aliases.get(alias), core)


class SwapchainMaintenanceContract(unittest.TestCase):
    """The surface and swapchain maintenance1 families, and the write-back
    hazard the first of them exposed.

    A device run reached a swapchain and then never presented. Every
    VK_EXT_surface_maintenance1 and VK_EXT_swapchain_maintenance1 node DXVK
    chained was dropped, and the presenter's own output showed the consequence:
    it reported `Present mode: VK_PRESENT_MODE_FIFO_KHR (dynamic: no)`, which
    is what DXVK concludes when VkSurfacePresentModeCompatibilityEXT comes back
    carrying the zero DXVK itself put there.

    VkSurfacePresentModeCompatibilityEXT is also the first structure in the
    table that the driver WRITES INTO and that holds a pointer, which is a
    hazard the marshal had never had to face: the write-back copies a node's
    whole body, so the shadow's own host address would have been stored into
    guest memory.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.pairs = chain_structs()
        self.listed = {stype for stype, _struct in self.pairs}
        self.commands = bridge_commands()

    def test_the_surface_maintenance1_nodes_are_carried(self) -> None:
        for stype in ("VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT",
                      "VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT",
                      "VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT"):
            with self.subTest(stype=stype):
                self.assertIn(stype, self.listed)

    def test_the_swapchain_maintenance1_nodes_are_carried(self) -> None:
        for stype in ("VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT",
                      "VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT",
                      "VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT",
                      "VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT",
                      "VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT"):
            with self.subTest(stype=stype):
                self.assertIn(stype, self.listed)

    def test_the_present_fence_is_marshalled(self) -> None:
        # The one whose absence is a hang rather than a degradation: DXVK
        # attaches a fence per swapchain to every present and waits on it
        # before reusing that image. A dropped node is a wait on a fence
        # nothing will ever signal.
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT:"):]
        body = body[:body.index("break;")]
        self.assertIn("info->pFences = inArray(info->pFences, info->swapchainCount)",
                      body)

    def test_the_nodes_the_device_run_dropped_are_all_covered_now(self) -> None:
        # The complete set of sTypes that run reported as unknown-stype. If any
        # is still absent the same run would drop it again.
        for stype in (
                "VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT",                 # 1000274000
                "VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT",   # 1000274002
                "VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT",  # 1000275002
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES",  # 1000259000
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES",   # 1000470000
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES", # 1000470001
                "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT"):  # 1000455001
            with self.subTest(stype=stype):
                self.assertIn(stype, self.listed)

    def test_release_swapchain_images_is_dispatchable(self) -> None:
        self.assertIn("ReleaseSwapchainImagesEXT", self.commands)
        self.assertIn("case VKB_ReleaseSwapchainImagesEXT:", self.source)

    def test_a_written_back_pointer_member_is_restored_to_the_guests_own(self) -> None:
        # The hazard. flush() copies a written-back node's whole body, so a
        # pointer member left holding the shadow's address would put a HOST
        # address into guest memory -- the fault this file exists to prevent,
        # travelling outward instead of inward.
        self.assertIn("void restorePointer(void* field, U64 guest)", self.source)
        self.assertIn("struct Restore", self.source)
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT:"):]
        body = body[:body.index("break;")]
        self.assertIn("outArray(", body)
        self.assertIn("restorePointer(&info->pPresentModes", body)
        self.assertLess(body.index("outArray("), body.index("restorePointer("),
                        "the guest pointer has to be captured before the shadow "
                        "replaces it")

    def output_structs_holding_a_pointer(self) -> list[tuple[str, str]]:
        """Chain-table structures Vulkan marks as OUTPUT that hold a pointer.

        The header itself carries the marker: an extensible structure the
        driver writes into declares `void* pNext`, and one the caller only
        reads declares `const void* pNext`. So "is this written back?" is a
        fact from vulkan_core.h rather than a judgement, and every such
        structure with a pointer member of its own is subject to the restore
        rule.
        """
        text = VULKAN_CORE.read_text(encoding="utf-8")
        found = []
        for stype, struct in self.pairs:
            match = re.search(r"typedef struct " + struct + r" \{(.*?)\} "
                              + struct + r";", text, re.S)
            if not match:
                continue
            body = match.group(1)
            if not re.search(r"^\s*void\*\s+pNext;", body, re.M):
                continue  # const void* pNext: an input structure
            members = [line for line in body.splitlines()
                       if "*" in line and "pNext" not in line]
            if members:
                found.append((stype, struct))
        return found

    def test_every_output_structure_with_a_pointer_restores_it(self) -> None:
        # The general form of the rule, so the NEXT one is caught here rather
        # than on a device. Today exactly one structure qualifies; the test is
        # written to find them rather than to list them.
        candidates = self.output_structs_holding_a_pointer()
        self.assertTrue(candidates,
                        "the header scan found no output structures at all, "
                        "which means it stopped working rather than that none "
                        "exist")
        for stype, struct in candidates:
            with self.subTest(struct=struct):
                marker = f"case (U32){stype}:"
                self.assertIn(marker, self.source,
                              f"{struct} is written back and holds a pointer, "
                              "so it needs a fixup")
                body = self.source[self.source.index(marker):]
                body = body[:body.index("break;")]
                self.assertIn("restorePointer(", body,
                              f"{struct} would carry a host address back into "
                              "guest memory")

    def test_the_restore_runs_before_anything_is_copied_out(self) -> None:
        flush = self.source[self.source.index("    void flush() {"):]
        flush = flush[:flush.index("\n    }\n")]
        self.assertLess(flush.index("restores"), flush.index("records"),
                        "a pointer member must be put back before the "
                        "write-back copies the body over it")

    def test_the_per_command_log_budget_replaced_the_global_one(self) -> None:
        # The global budget was spent by whatever the program did most: a
        # device run reached exactly 64 dispatched calls, 24 of them eight
        # repetitions of three pipeline-creation commands, and the entire
        # recording and presentation half of the run was invisible. The
        # conclusion drawn from that log was an artifact of the budget.
        self.assertIn("kPerCommandBudget", self.source)
        self.assertIn("kNamedCallCeiling", self.source)
        self.assertIn("gCommandCalls[VKB_COUNT]", self.source)
        self.assertNotIn("kNamedCallBudget", self.source)
        body = self.source[self.source.index("gCommandCalls[index].fetch_add"):]
        body = body[:body.index("}")]
        self.assertIn("result < 0", body)
        self.assertIn("seen < kPerCommandBudget", body)
        self.assertIn("seen=%u", body)

    def test_both_map_paths_carry_the_address_witness(self) -> None:
        # Reasoning from the ABSENCE of a witness only works when every path
        # that could have produced it has one. vkMapMemory had the witness and
        # vkMapMemory2 did not, so "no memory was ever mapped" was a weaker
        # claim than it looked.
        self.assertEqual(self.source.count("BOXEDWINE_X64_VULKAN_MAP"), 2)
        for command in ("MapMemory", "MapMemory2"):
            with self.subTest(command=command):
                start = self.source.index(f"case VKB_{command}:")
                rest = self.source[start + 1:]
                end = re.search(r"^\s*case VKB_\w+:", rest, re.MULTILINE)
                body = rest[:end.start()] if end else rest
                self.assertIn("BOXEDWINE_X64_VULKAN_MAP", body)
                self.assertIn("hostToGuestAddress", body)


class SignallingPathContract(unittest.TestCase):
    """Every command that accepts a synchronisation object the driver must
    later signal, and the argument plumbing that carries it.

    A device run reached vkQueueSubmit2 and vkAcquireNextImageKHR, both
    status=0, and then every guest thread parked forever. The shape that would
    explain that from inside this bridge is a handle accepted and never
    signalled: a semaphore dropped on the way to the driver, a fence forwarded
    in the wrong argument slot, a null that became a stale handle, a timeline
    value truncated to 32 bits, or a timeout narrowed so a wait never ends.
    These tests make each of those unrepresentable rather than merely absent.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.shim = SHIM_SOURCE.read_text(encoding="utf-8")
        self.pairs = chain_structs()

    def case_body(self, name: str) -> str:
        start = self.source.index(f"case VKB_{name}:")
        rest = self.source[start + 1:]
        end = re.search(r"^\s*case VKB_\w+:", rest, re.MULTILINE)
        return rest[:end.start()] if end else rest

    def packed_words(self) -> dict[str, int]:
        """How many argument words the guest ICD packs for each command."""
        packed: dict[str, int] = {}
        for match in re.finditer(r"\bBW_([RVBA])\s*\(", self.shim):
            start = match.end() - 1
            depth = 0
            fields = 1
            end = start
            for i in range(start, len(self.shim)):
                char = self.shim[i]
                if char in "([":
                    depth += 1
                elif char in ")]":
                    depth -= 1
                    if depth == 0:
                        end = i
                        break
                elif char == "," and depth == 1:
                    fields += 1
            call = self.shim[match.start():end + 1]
            name = re.match(r"BW_[RVBA]\s*\(\s*(\w+)", call).group(1)
            words = fields - 1
            if match.group(1) == "A":
                words += 1  # BW_A appends the 64-bit result slot
            packed[name] = words
        return packed

    def highest_index_read(self, name: str) -> int:
        body = self.case_body(name)
        indices = [int(n) for n in
                   re.findall(r"\b(?:A|H|U32A|F32A)\((\d+)\)", body)]
        return max(indices) + 1 if indices else 0

    def test_no_dispatcher_case_reads_past_what_the_guest_packs(self) -> None:
        # The general form of the plumbing bug, over the whole table rather
        # than the handful of commands anyone thought to check. A case that
        # reads A(n) beyond the packed count gets a zero -- which for a
        # semaphore or fence argument is VK_NULL_HANDLE, i.e. an object the
        # driver is never asked to signal, and a wait that never ends.
        packed = self.packed_words()
        commands = bridge_commands()
        offenders = []
        for name in sorted(packed):
            if name not in commands:
                continue  # the macro definition itself, not a call site
            read = self.highest_index_read(name)
            if read > packed[name]:
                offenders.append(f"{name}: packs {packed[name]}, reads A({read - 1})")
        self.assertEqual(offenders, [], "; ".join(offenders))

    def test_the_argument_arity_check_actually_saw_the_table(self) -> None:
        packed = self.packed_words()
        commands = bridge_commands()
        covered = [n for n in commands if n in packed]
        self.assertGreaterEqual(len(covered), len(commands) - 2,
                                "the shim scan missed commands, so the arity "
                                "test above proves nothing")

    def test_a_timeout_crosses_as_a_full_64_bit_value(self) -> None:
        # A narrowed timeout is a plausible source of both failure modes: a
        # wait that never ends and a wait that returns early. UINT64_MAX -- the
        # value DXVK passes -- truncates to 32 bits as 0xffffffff, which is
        # about four seconds rather than forever.
        for command, slot in (("WaitForFences", 4), ("WaitSemaphores", 2),
                              ("AcquireNextImageKHR", 2)):
            with self.subTest(command=command):
                body = self.case_body(command)
                self.assertIn(f"(uint64_t)A({slot})", body,
                              "the timeout must be taken as the whole "
                              "argument word")
                self.assertNotIn(f"U32A({slot})", body,
                                 "a 32-bit read of the timeout slot narrows it")

    def test_the_acquire_forwards_both_signal_objects_in_order(self) -> None:
        # vkAcquireNextImageKHR signals a semaphore, a fence, or both, and
        # either may be VK_NULL_HANDLE. A null must stay null: a driver treats
        # "no semaphore" and "a semaphore" completely differently, and a
        # swapped pair would signal the wrong kind of object.
        body = self.case_body("AcquireNextImageKHR")
        self.assertIn("(VkSemaphore)A(3), (VkFence)A(4)", body)
        self.assertIn("(VkSwapchainKHR)A(1)", body)
        self.assertIn("outRequired(A(5)", body)
        # A cast of the raw argument word is what keeps a zero a null; nothing
        # may substitute a remembered or defaulted handle.
        self.assertNotIn("VK_NULL_HANDLE", body)

    def test_every_queue_submission_forwards_its_fence(self) -> None:
        # The fence is the only thing that tells the caller a submission
        # finished. Dropping it is a wait that never ends.
        for command in ("QueueSubmit", "QueueSubmit2"):
            with self.subTest(command=command):
                body = self.case_body(command)
                self.assertIn("(VkFence)A(3)", body)
                self.assertIn("U32A(1)", body)
                self.assertIn("structArrayTyped", body)

    def test_the_semaphore_arrays_are_marshalled_with_their_values(self) -> None:
        # A timeline wait is a semaphore array and a VALUE array of the same
        # length. Marshalling the handles and not the values is a wait on
        # whatever the shadow happened to contain.
        wait = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO:"):]
        wait = wait[:wait.index("break;")]
        self.assertIn("pSemaphores, info->semaphoreCount", wait)
        self.assertIn("pValues, info->semaphoreCount", wait)
        timeline = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:"):]
        timeline = timeline[:timeline.index("break;")]
        self.assertIn("pWaitSemaphoreValues", timeline)
        self.assertIn("pSignalSemaphoreValues", timeline)

    def test_submit_info_2_marshals_all_three_arrays_by_their_own_type(self) -> None:
        # Wait and signal are VkSemaphoreSubmitInfo (48 bytes, carrying a
        # 64-bit value and a 64-bit stage mask); command buffers are a
        # different, smaller structure. Using one element size for both would
        # walk the wrong stride and hand the driver nonsense handles.
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_SUBMIT_INFO_2:"):]
        body = body[:body.index("break;")]
        self.assertEqual(body.count("structArrayTyped<VkSemaphoreSubmitInfo>"), 2)
        self.assertEqual(
            body.count("structArrayTyped<VkCommandBufferSubmitInfo>"), 1)
        for member in ("pWaitSemaphoreInfos", "pCommandBufferInfos",
                       "pSignalSemaphoreInfos"):
            with self.subTest(member=member):
                self.assertIn(f"info->{member} =", body)

    def test_timeline_semaphore_creation_survives_the_chain(self) -> None:
        # The type and the initial value live in a VkSemaphoreTypeCreateInfo on
        # the pNext chain. If that node were dropped, the driver would make a
        # BINARY semaphore while the program believed it had a timeline one --
        # every later wait on a value would then be waiting for something that
        # can never happen. The structure holds no pointer, so being in the
        # table is sufficient: the byte copy carries initialValue.
        listed = {stype for stype, _struct in self.pairs}
        for stype in ("VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO",
                      "VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO",
                      "VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO",
                      "VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO",
                      "VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO"):
            with self.subTest(stype=stype):
                self.assertIn(stype, listed)
        text = VULKAN_CORE.read_text(encoding="utf-8")
        body = re.search(r"typedef struct VkSemaphoreTypeCreateInfo \{(.*?)\} "
                         r"VkSemaphoreTypeCreateInfo;", text, re.S).group(1)
        self.assertIn("initialValue", body)
        pointers = [line for line in body.splitlines()
                    if "*" in line and "pNext" not in line]
        self.assertEqual(pointers, [],
                         "VkSemaphoreTypeCreateInfo grew a pointer member, so "
                         "the byte copy is no longer sufficient for it")

    def test_the_counter_and_signal_paths_carry_the_full_value(self) -> None:
        counter = self.case_body("GetSemaphoreCounterValue")
        self.assertIn("outRequired(A(2), sizeof(uint64_t))", counter)
        self.assertIn("(VkSemaphore)A(1)", counter)
        signal = self.case_body("SignalSemaphore")
        self.assertIn("m.chain(A(1), false)", signal)

    def test_the_fence_paths_are_plumbed(self) -> None:
        status = self.case_body("GetFenceStatus")
        self.assertIn("(VkFence)A(1)", status)
        for command in ("WaitForFences", "ResetFences"):
            with self.subTest(command=command):
                body = self.case_body(command)
                self.assertIn("inArrayAt(\n            A(2), U32A(1), sizeof(VkFence))",
                              body.replace("\r", ""))

    def test_present_marshals_every_array_it_is_given(self) -> None:
        body = self.source[self.source.index(
            "case (U32)VK_STRUCTURE_TYPE_PRESENT_INFO_KHR:"):]
        body = body[:body.index("break;")]
        for member in ("pWaitSemaphores", "pSwapchains", "pImageIndices",
                       "pResults"):
            with self.subTest(member=member):
                self.assertIn(f"info->{member} =", body)

    def test_the_acquire_and_present_witnesses_are_a_readable_pair(self) -> None:
        # An acquire that succeeds and is never followed by a present is the
        # shape of the hang. Both witnesses are unbudgeted so the pair survives
        # any amount of other traffic, and both name the image index so a
        # reader can tell whether the image the guest was handed is the image
        # it presented.
        self.assertIn("BOXEDWINE_X64_VULKAN_ACQUIRE", self.source)
        acquire = self.case_body("AcquireNextImageKHR")
        for field in ("swapchain=", "timeout=", "semaphore=", "fence=",
                      "index=", "status="):
            with self.subTest(field=field):
                self.assertIn(field, acquire)
        present = self.case_body("QueuePresentKHR")
        for field in ("index=", "waits=", "status="):
            with self.subTest(field=field):
                self.assertIn(field, present)


class PresentationBackendContract(unittest.TestCase):
    """The notifications the native presentation backend is written to receive.

    KVulkan declares five: registerVulkanSwapchain, destroyVulkanSwapchain,
    acquireVulkanSwapchain, submitVulkanWorkload and presentVulkanSwapchain.
    Until this revision no lane called any of them, and three separate
    diagnostics were built on top of that silence:

      - the iOS first-frame watchdog fires on !firstPresentObserved, and only
        presentVulkanSwapchain ever sets it. "Produced no frame for 12
        seconds" was therefore printed by every run that reached a surface,
        whether or not the guest presented, so the line carried no
        information;
      - registerVulkanSwapchain is what fills swapchainSurfaces, and both
        acquireVulkanSwapchain and presentVulkanSwapchain return at their
        first lookup into an empty map;
      - "iOS guest performance: N Vulkan frames/sec" counts through
        bvnReportPresentRate(), which only presentVulkanSwapchain calls.

    A witness that cannot fire is worse than no witness, because a reader
    treats its absence as evidence. These tests keep the wiring attached.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.header = (REPO / "include" / "kvulkan.h").read_text(
            encoding="utf-8")

    def case_body(self, name: str) -> str:
        start = self.source.index(f"case VKB_{name}:")
        rest = self.source[start + 1:]
        end = re.search(r"^\s*case VKB_\w+:", rest, re.MULTILINE)
        return rest[:end.start()] if end else rest

    def swapchain_notifications(self) -> set[str]:
        """The notification set, read from KVulkan rather than listed here.

        Taking it from the header means a sixth notification added to the
        interface is caught by this test instead of quietly joining the five
        that already went uncalled.
        """
        found = set()
        for match in re.finditer(r"virtual\s+void\s+(\w*Vulkan\w*)\s*\(",
                                 self.header):
            name = match.group(1)
            if name in ("createVulkanSurface", "destroyVulkanSurface"):
                continue  # surface lifetime, served by createXlibSurface
            found.add(name)
        return found

    def test_every_kvulkan_notification_is_actually_called(self) -> None:
        notifications = self.swapchain_notifications()
        self.assertTrue(notifications,
                        "the header scan found no notifications at all, so it "
                        "stopped working rather than proving anything")
        for name in sorted(notifications):
            with self.subTest(notification=name):
                self.assertIn(f"->{name}(", self.source,
                              f"{name} is declared for this backend and never "
                              "called, so whatever it feeds reports silence "
                              "as a fact")

    def test_the_swapchain_is_registered_against_its_own_surface(self) -> None:
        # The pairing is what every later lookup is keyed on. Registering
        # against anything but the create-info's surface would attribute
        # frames to the wrong view.
        body = self.case_body("CreateSwapchainKHR")
        self.assertIn("registerVulkanSwapchain((void*)*out,", body)
        self.assertIn("(void*)info->surface", body)
        self.assertIn("result == VK_SUCCESS", body)
        self.assertLess(body.index("result == VK_SUCCESS"),
                        body.index("registerVulkanSwapchain"),
                        "a failed create leaves *out untouched, so it must "
                        "not be registered")

    def test_the_swapchain_is_unregistered_before_it_is_destroyed(self) -> None:
        body = self.case_body("DestroySwapchainKHR")
        self.assertIn("destroyVulkanSwapchain((void*)A(1))", body)
        self.assertLess(body.index("destroyVulkanSwapchain"),
                        body.index("PFN_vkDestroySwapchainKHR"),
                        "the backend has to drop the handle while it is still "
                        "a handle")

    def test_both_acquire_forms_notify(self) -> None:
        # Reasoning from the absence of an acquire only works when every way
        # of acquiring reports. vkAcquireNextImage2KHR names its swapchain
        # inside the info structure rather than in an argument slot.
        one = self.case_body("AcquireNextImageKHR")
        self.assertIn("acquireVulkanSwapchain(\n                (void*)A(1),",
                      one.replace("\r", ""))
        two = self.case_body("AcquireNextImage2KHR")
        self.assertIn("(void*)info->swapchain", two)
        self.assertIn("acquireVulkanSwapchain", two)

    def test_both_submit_forms_notify_under_their_own_name(self) -> None:
        for command, api in (("QueueSubmit", "vkQueueSubmit"),
                             ("QueueSubmit2", "vkQueueSubmit2")):
            with self.subTest(command=command):
                body = self.case_body(command)
                self.assertIn("submitVulkanWorkload", body)
                self.assertIn(f'"{api}"', body)
                self.assertIn("U32A(1)", body)

    def test_the_present_notifies_once_per_swapchain(self) -> None:
        # A present may carry several swapchains, and the backend keys its
        # per-surface frame state by swapchain, so one notification for the
        # batch would leave every swapchain but one looking unpresented.
        body = self.case_body("QueuePresentKHR")
        self.assertIn("for (uint32_t i = 0; i < info->swapchainCount; ++i)",
                      body)
        self.assertIn("presentVulkanSwapchain(\n                        "
                      "(void*)info->pSwapchains[i]", body.replace("\r", ""))
        self.assertIn("info->pResults", body,
                      "per-swapchain results say which swapchain failed")

    def test_the_present_and_acquire_are_timed_on_this_lane_too(self) -> None:
        # The IA-32 lane has timed these since build 72. This lane did not, so
        # "iOS guest present path: N ms inside vkQueuePresentKHR" reported
        # zero for every 64-bit run however long the compositor held the
        # drawable -- another witness reporting silence as a fact.
        self.assertIn("bvnHostPresent::recordPresent", self.source)
        self.assertIn("bvnHostPresent::recordAcquire", self.source)
        self.assertIn("index == VKB_QueuePresentKHR", self.source)
        self.assertIn("index == VKB_AcquireNextImageKHR", self.source)


class SurfaceExtentContract(unittest.TestCase):
    """Whether the extent the host reports for a surface is the size of the
    guest window that surface was made from.

    A device run built two swapchains for one window inside 1.2 seconds: the
    first at 740x555, the second at 804x585. The X11 window was 804x585 for
    the whole run; the host's presentation layer re-parented its view during
    the first frame and the capability query DXVK made inside that window saw
    the transient size. Nothing in the log said so -- only that two
    "Buffer size" lines disagreed, which several unrelated causes produce.

    The fix for the churn is not in this file. Saying that the churn happened,
    and when, is.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")

    def case_body(self, name: str) -> str:
        start = self.source.index(f"case VKB_{name}:")
        rest = self.source[start + 1:]
        end = re.search(r"^\s*case VKB_\w+:", rest, re.MULTILINE)
        return rest[:end.start()] if end else rest

    def test_the_surface_records_the_window_it_was_made_from(self) -> None:
        self.assertIn("rememberSurfaceWindow(", self.source)
        self.assertIn("struct SurfaceWindow", self.source)
        # createXlibSurface is the only place a window id is in hand.
        create = self.source[self.source.index("S64 createXlibSurface("):]
        create = create[:create.index("\n}\n")]
        self.assertIn("rememberSurfaceWindow(handle, (U32)windowId", create)
        self.assertIn("window_size=%ux%u", create)

    def test_both_capability_queries_carry_the_witness(self) -> None:
        # The same rule the map witness follows: reasoning from the absence of
        # a mismatch only works when every path that could report one does.
        for command in ("GetPhysicalDeviceSurfaceCapabilitiesKHR",
                        "GetPhysicalDeviceSurfaceCapabilities2KHR"):
            with self.subTest(command=command):
                body = self.case_body(command)
                self.assertIn("noteSurfaceExtent(", body)
        two = self.case_body("GetPhysicalDeviceSurfaceCapabilities2KHR")
        self.assertIn("(U64)info->surface", two,
                      "the 2KHR form names its surface inside the info "
                      "structure, not in an argument slot")
        self.assertIn("&capabilities->surfaceCapabilities", two)

    def test_the_witness_names_both_sizes_and_the_verdict(self) -> None:
        body = self.source[self.source.index("void noteSurfaceExtent("):]
        body = body[:body.index("\n}\n")]
        for field in ("window_size=", "current=", "min=", "max=", "agrees=",
                      "mismatches=", "status="):
            with self.subTest(field=field):
                self.assertIn(field, body)

    def test_the_undefined_extent_is_not_a_mismatch(self) -> None:
        # 0xffffffff in both fields means "the extent is whatever the
        # swapchain asks for". Reporting that as a disagreement would make
        # every platform that uses it look broken.
        body = self.source[self.source.index("void noteSurfaceExtent("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("0xffffffffu", body)
        self.assertIn("undefined", body)
        self.assertIn("!undefined", body)

    def test_a_surface_this_bridge_did_not_make_is_not_judged(self) -> None:
        # Wine builds surfaces through other paths. Those have no recorded
        # window, and calling that a mismatch would be inventing evidence.
        body = self.source[self.source.index("void noteSurfaceExtent("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("known &&", body)
        self.assertIn("known ? (mismatch ? 0 : 1) : -1", body)

    def test_the_swapchain_witness_pairs_the_request_with_the_window(self) -> None:
        body = self.case_body("CreateSwapchainKHR")
        self.assertIn("BOXEDWINE_X64_VULKAN_SWAPCHAIN", body)
        for field in ("window_size=", "requested=", "old=", "status="):
            with self.subTest(field=field):
                self.assertIn(field, body)
        self.assertIn("info->imageExtent.width", body)


def core_commands() -> dict[str, str]:
    """Every command core Vulkan defines, mapped to the version that added it.

    vulkan_core.h is organised as a sequence of feature blocks, each opened by
    `#define VK_<FEATURE> 1`; the core ones are VK_VERSION_1_0 through
    VK_VERSION_1_4 and everything else is an extension. Reading the prototypes
    per block is what makes "is this command core?" a fact from the header
    rather than a judgement about the name.
    """
    versions = {"VK_VERSION_1_0", "VK_VERSION_1_1", "VK_VERSION_1_2",
                "VK_VERSION_1_3", "VK_VERSION_1_4"}
    found: dict[str, str] = {}
    feature = None
    for line in VULKAN_CORE.read_text(encoding="utf-8").splitlines():
        opened = re.match(r"#define (VK_[A-Za-z0-9_]+) 1\s*$", line)
        if opened:
            feature = opened.group(1)
            continue
        proto = re.match(r"VKAPI_ATTR .*VKAPI_CALL (vk[A-Za-z0-9]+)\(", line)
        if proto and feature in versions and proto.group(1) not in found:
            found[proto.group(1)] = feature
    return found


def core_refusals() -> set[str]:
    """BOXEDWINE_X64_VK_CORE_REFUSALS: the core commands the header records as
    deliberately absent, with the reason written above the list."""
    text = BRIDGE_HEADER.read_text(encoding="utf-8")
    start = text.index("#define BOXEDWINE_X64_VK_CORE_REFUSALS(X)")
    names: set[str] = set()
    for raw in text[start:].splitlines()[1:]:
        match = re.match(r"\s*X\((\w+)\)\s*\\?\s*$", raw)
        if match:
            names.add(match.group(1))
            continue
        if not raw.strip().endswith("\\"):
            break
    return names


class CoreCompletenessContract(unittest.TestCase):
    """No core Vulkan command may be a silent hole.

    Wine's winevulkan builds a dispatch table with one slot per command it
    knows and fills each from vkGetDeviceProcAddr by name. A name the bridge
    cannot resolve leaves that slot null. For an EXTENSION command that is
    harmless -- a caller only reaches those slots after enabling the extension
    -- but for a CORE command it is not, because a caller may reach a core slot
    unconditionally and DXVK does not check. A device run ended precisely
    there: instance creation, device creation and vkGetDeviceQueue all
    succeeded, then the 32-bit process died on an indirect call through a table
    that was mapped and readable with a zero at the entry it wanted, and the
    only core commands the bridge had no ordinal for were the four the log's
    proc-address misses named.

    So the rule is: every core command is either dispatched or explicitly
    refused with a reason. These tests compute the core set from the header
    rather than listing it, so a Vulkan header update that adds a core command
    fails here instead of on a device.
    """

    def setUp(self) -> None:
        self.commands = bridge_commands()
        self.aliases = validator.read_bridge_aliases(BRIDGE_HEADER)
        self.core = core_commands()
        self.refused = core_refusals()

    def test_the_core_set_was_actually_found(self) -> None:
        # A parse that silently found nothing would make every test below pass.
        self.assertGreater(len(self.core), 200)
        for name in ("vkCreateInstance", "vkCmdDraw", "vkQueueSubmit2",
                     "vkGetPhysicalDeviceToolProperties"):
            with self.subTest(command=name):
                self.assertIn(name, self.core)

    def test_every_core_command_is_dispatched_or_refused_by_name(self) -> None:
        served = set(self.commands) | set(self.aliases) | self.refused
        # The two lookup entry points are core, and the shim answers them
        # itself rather than dispatching them; they are never null.
        served |= {"GetInstanceProcAddr", "GetDeviceProcAddr"}
        holes = sorted(f"{name} ({self.core[name]})"
                       for name in self.core if name[2:] not in served)
        self.assertEqual(
            holes, [],
            "core command(s) with no ordinal and no recorded refusal; each "
            "leaves a null in winevulkan's dispatch table: " + ", ".join(holes))

    def test_the_four_commands_the_device_run_named_are_carried(self) -> None:
        # Filtering that run's proc-address misses down to names with no vendor
        # suffix left exactly these four.
        for name in ("EnumeratePhysicalDeviceGroups",
                     "GetPhysicalDeviceSparseImageFormatProperties",
                     "GetPhysicalDeviceSparseImageFormatProperties2",
                     "GetPhysicalDeviceToolProperties"):
            with self.subTest(command=name):
                self.assertIn(name, self.commands)

    def test_every_refusal_names_a_real_core_command(self) -> None:
        # A refusal for a command that is not core, or no longer exists, is a
        # stale entry that would mask a real hole.
        self.assertTrue(self.refused)
        for name in sorted(self.refused):
            with self.subTest(command=name):
                self.assertIn("vk" + name, self.core)
                self.assertNotIn(name, self.commands,
                                 "a command cannot be both refused and carried")

    def test_the_refusals_are_recorded_with_their_reason(self) -> None:
        header = BRIDGE_HEADER.read_text(encoding="utf-8")
        self.assertIn("pHostPointer", header)
        self.assertIn("texel block size", header)

    def test_a_core_miss_is_logged_differently_from_an_extension_miss(self) -> None:
        # The witness. An extension miss is expected and a core miss is how a
        # table gets a hole; a log that spells them the same way cannot be
        # filtered down to the dangerous half, which is what made the first
        # investigation of this fault slow.
        source = DISPATCHER.read_text(encoding="utf-8")
        self.assertIn("hasVendorSuffix", source)
        self.assertIn("procAddrMissKind", source)
        self.assertIn('"core-miss"', source)
        self.assertIn('"extension-miss"', source)
        # Both lookup failure paths have to carry it: one for a name the table
        # does not have, one for a name the DRIVER does not have.
        body = source[source.index("S64 procAddr("):]
        body = body[:body.index("\n#endif // BOXEDWINE_VULKAN")]
        self.assertEqual(body.count("kind=%s"), 2)
        self.assertIn("missing=%s", body)
        self.assertIn("unsupported=%s", body)

    def test_the_vendor_suffix_table_covers_the_tags_in_use(self) -> None:
        # The classifier is only as good as this list: a tag missing from it
        # makes an extension command read as core and floods the dangerous
        # channel. Every suffix that actually appears on a command in
        # vulkan_core.h has to be there.
        source = DISPATCHER.read_text(encoding="utf-8")
        table = source[source.index("kVendorSuffixes[] = {"):]
        table = table[:table.index("};")]
        listed = set(re.findall(r'"(\w+)"', table))
        text = VULKAN_CORE.read_text(encoding="utf-8")
        every = set(re.findall(r"VKAPI_CALL (vk[A-Za-z0-9]+)\(", text))
        core = set(core_commands())
        for name in sorted(every - core):
            suffix = re.search(r"([A-Z]{2,}|[A-Z][a-z]+)$", name)
            if not suffix:
                continue
            tag = suffix.group(1)
            if not tag.isupper():
                continue
            with self.subTest(command=name, tag=tag):
                self.assertIn(tag, listed,
                              f"{name} would be classified as a core miss")


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


class CallingThreadContract(unittest.TestCase):
    """Which guest thread made a bridge call, and whether it survived to
    finish the frame it started.

    A device capture of the 32-bit Direct3D 9 probe ended with a successful
    vkAcquireNextImageKHR -- full 64-bit timeout, a real semaphore, image index
    0 -- no present of any kind, and every surviving thread of the process
    parked in a futex wait on Wine's per-thread alert table with no wake
    attempted for thirty seconds. At that same instant a fifth thread left the
    process through exit(2). The probe's frame loop peeks for messages and
    renders unconditionally, so it cannot have been waiting for a message; and
    the bridge's own witnesses named only the process, so "the thread that
    acquired the image is the thread that left" could be neither stated nor
    refused. It is the difference between a renderer worker retiring on
    schedule and the thread driving the swapchain disappearing mid-frame, and
    those are opposite diagnoses.

    These tests keep the thread on the record at both ends: on every bridge
    call, and on the exit of any thread that leaves a live process behind.
    """

    def setUp(self) -> None:
        self.source = DISPATCHER.read_text(encoding="utf-8")
        self.header = DISPATCHER_HEADER.read_text(encoding="utf-8")
        self.thread_header = THREAD_HEADER.read_text(encoding="utf-8")
        self.syscalls = SYSCALL64.read_text(encoding="utf-8")

    def case_body(self, name: str) -> str:
        start = self.source.index(f"case VKB_{name}:")
        rest = self.source[start + 1:]
        end = re.search(r"^\s*case VKB_\w+:", rest, re.MULTILINE)
        return rest[:end.start()] if end else rest

    def exit_witness(self) -> str:
        start = self.syscalls.index("static void noteThreadExit64(")
        rest = self.syscalls[start:]
        end = re.search(r"^static \w", rest[1:], re.MULTILINE)
        return rest[:end.start() + 1] if end else rest

    def test_the_swapchain_witnesses_name_the_calling_thread(self) -> None:
        # The acquire and the present are the pair the hang is read from. A
        # pair that names only the process cannot say whether one thread did
        # both, which is the question a missing present asks first.
        for command in ("AcquireNextImageKHR", "QueuePresentKHR"):
            with self.subTest(command=command):
                self.assertIn("tid=%04X", self.case_body(command))

    def test_the_per_command_witness_names_the_calling_thread(self) -> None:
        # Same reason, over every dispatched command: the order in which
        # threads reach the bridge is what separates a renderer that hands its
        # frame to a worker from one whose worker has gone.
        head = self.source[:self.source.index('"args=%llu status=%lld seen=%u"')]
        witness = head[head.rindex("klog_fmt("):]
        self.assertIn("BOXEDWINE_X64_VULKAN_BRIDGE", witness)
        self.assertIn("tid=%04X", witness)
        self.assertIn("name, pid, tid,", self.source)

    def test_the_dispatcher_is_handed_the_calling_thread(self) -> None:
        self.assertIn(
            "S64 dispatchCommand(int index, Marshal& m, const U64* args, "
            "U64 count, U32 tid)", self.source)
        self.assertIn("dispatchCommand(index, marshal, args, count, tid)",
                      self.source)

    def test_every_dispatch_records_the_command_on_the_thread(self) -> None:
        # A line printed from inside the bridge cannot say anything about a
        # thread that has already left the process. The record has to live on
        # the thread so a witness fired somewhere else can read it.
        self.assertIn("diagnosticVulkanBridgeCall.store(", self.source)
        self.assertIn("diagnosticVulkanBridgeCalls.fetch_add(", self.source)
        self.assertIn("std::atomic<U32> diagnosticVulkanBridgeCall{0};",
                      self.thread_header)
        self.assertIn("std::atomic<U64> diagnosticVulkanBridgeCalls{0};",
                      self.thread_header)

    def test_the_recorded_command_is_never_cleared(self) -> None:
        # The 32-bit lane's diagnosticVulkanCall is cleared on the way out,
        # because it answers "what is this thread inside right now". This one
        # answers "what was this thread doing at all", and a thread that has
        # returned from the bridge -- or exited -- must still answer it.
        for path in (DISPATCHER, SYSCALL64):
            with self.subTest(path=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertNotIn("diagnosticVulkanBridgeCall.store(0", text)

    def test_the_command_name_is_reachable_from_outside_the_bridge(self) -> None:
        self.assertIn("const char* vulkanBridge64CommandName(U32 callPlusOne);",
                      self.header)
        self.assertIn("const char* vulkanBridge64CommandName(U32 callPlusOne) {",
                      self.source)
        self.assertIn("vulkanBridge64CommandName(", self.syscalls)

    def test_a_lone_thread_exit_is_witnessed(self) -> None:
        self.assertIn("BOXEDWINE_X64_THREAD_EXIT", self.syscalls)
        witness = self.exit_witness()
        for field in ("pid=%u", "tid=%04X", "rip=0x%llx", "rsp=0x%llx",
                      "siblings=%u", "vulkan_calls=%llu", "last_vulkan=%s"):
            with self.subTest(field=field):
                self.assertIn(field, witness)

    def test_the_thread_exit_witness_fires_only_for_a_single_thread_exit(self) -> None:
        # exit_group already has its own marker and its own diagnostics. This
        # one exists for the exit that leaves a process running, because that
        # is the one whose survivors can starve.
        self.assertIn("if (!group) {\n            noteThreadExit64(cpu);",
                      self.syscalls)

    def test_the_thread_exit_witness_snapshots_the_parked_threads(self) -> None:
        # Taken at the instant of the exit, not by a watchdog seconds later:
        # what has to be established is whether the survivors were ALREADY
        # asleep when the thread left, which no later sample can distinguish
        # from threads that parked afterwards.
        witness = self.exit_witness()
        self.assertIn("KThread::logFutexSnapshot();", witness)
        self.assertIn("if (live <= 1) {", witness)
        self.assertLess(witness.index("if (live <= 1) {"),
                        witness.index("KThread::logFutexSnapshot();"))

    def test_the_thread_exit_witness_names_the_departing_syscalls(self) -> None:
        # A thread that returned from its start routine and a thread that was
        # taken out of one are indistinguishable from the exit alone. The
        # syscalls it made getting there are not, so the process-wide ring is
        # replayed for this thread only.
        witness = self.exit_witness()
        self.assertIn("X64_THREAD_EXIT_TAIL", witness)
        self.assertIn("r.threadId != thread->id", witness)
        self.assertIn("matching > 12 ? matching - 12 : 0", witness)
        # And the ring is not claimed: a later exit_group still prints it whole.
        self.assertNotIn("claimDump", witness)

    def test_the_thread_exit_witness_is_budgeted(self) -> None:
        witness = self.exit_witness()
        self.assertIn("reported.fetch_add(1, std::memory_order_relaxed) >= 16",
                      witness)

    def test_the_thread_exit_witness_cannot_fault(self) -> None:
        # It reads the guest stack for the return address that reached the
        # exit. A diagnostic must never be the thing that faults, so the read
        # happens only where the page table says the page is mapped.
        witness = self.exit_witness()
        self.assertIn("isPageMapped(rsp >> K64_PAGE_SHIFT)", witness)
        self.assertLess(witness.index("isPageMapped"),
                        witness.index("readq(rsp)"))


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
