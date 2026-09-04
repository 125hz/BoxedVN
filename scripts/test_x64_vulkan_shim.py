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
