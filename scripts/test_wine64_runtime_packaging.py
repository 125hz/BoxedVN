#!/usr/bin/env python3
"""Packaging contract for the guest's wineserver executables.

Wine64 execs `/usr/lib/wine/wineserver` directly. The distro ships a 382-byte
/bin/sh wrapper under that name which picks between wineserver64 and
wineserver32. Packaged as the guest's wineserver, a 64-bit process exec'd a
shell script: BoxedWine resolved it through /bin/sh, took the ELF32 loader
path while the process was still marked 64-bit, skipped both the 32-bit stack
transition and the ELF64 stack builder, and reported success without
installing any usable image. The child then produced nothing and its parent
waited on it forever.

Both guest paths must hold the same real x86-64 executable. These tests drive
the packaging validator against synthetic archives, including one built to the
exact broken shape, so the contract is enforced without a Wine install.
"""

from __future__ import annotations

import hashlib
import os
import posixpath
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILDER = REPO / "scripts" / "build-wine64-runtime-ci.sh"
VALIDATOR = REPO / "scripts" / "validate-wine64-runtime.sh"

# The wrapper the distro installs at /usr/lib/wine/wineserver.
SHELL_WRAPPER = b"""#!/bin/sh -e
if [ -x "$(dirname "$0")/wineserver64" ]; then
    exec "$(dirname "$0")/wineserver64" "$@"
fi
exec "$(dirname "$0")/wineserver32" "$@"
"""


# Wine searches for its font backend at both multiarch paths in turn. On the
# runner /lib is a symlink to /usr/lib, but the guest archive has no symlinks,
# so a single staged copy satisfies only one of the two lookups.
#
# The two paths land in different archives: the builder partitions the stage by
# top-level directory, so lib/ goes to the glibc layer and usr/ to the Wine
# layer. Both are extracted into the same guest root, which is why one copy in
# each is the correct shape rather than a duplicate.
FREETYPE_GLIBC_PATH = "lib/x86_64-linux-gnu/libfreetype.so.6"
FREETYPE_WINE_PATH = "usr/lib/x86_64-linux-gnu/libfreetype.so.6"

# The X11 client libraries winex11.so links against directly. Without them the
# user driver cannot load and the process has no desktop window at all.
X11_CORE_LIBS = ("libX11.so.6", "libXext.so.6")
# BoxedWine's own x86-64 X11 client libraries, placed first on the 64-bit
# launch's LD_LIBRARY_PATH so winex11.so binds to the bridge rather than to a
# distro libX11 that connects to an X socket nobody serves. The validator
# requires every library the driver links or dlopens.
X11_SHIM_DIR = "usr/lib/boxedwine64-x11"
X11_SHIM_LIBS = (
    "libX11.so.6", "libXext.so.6", "libXrender.so.1", "libXrandr.so.2",
    "libXinerama.so.1", "libXi.so.6", "libXcursor.so.1", "libXfixes.so.3",
    "libXcomposite.so.1", "libXxf86vm.so.1",
)
# Fontconfig loads without its configuration and then reports that it cannot
# load a default config file, which leaves Wine with no usable font backend.
FONTCONFIG_PATH = "etc/fonts/fonts.conf"
# fonts.conf includes conf.d. That directory is symlinks on the runner and
# has to reach the archive materialised as real XML: a guest link's target
# text sitting there is what fontconfig reports as a parse failure, which is
# the shape a device run produced at both <include> lines.
FONTCONFIG_CONFD = tuple(
    f"etc/fonts/conf.d/{index:02d}-boxedvn.conf" for index in (10, 20, 30))
FONTCONFIG_XML = b'<?xml version="1.0"?>\n<fontconfig></fontconfig>\n'
# Wine's own bitmap fonts, which the non-client area of a window is drawn with.
# The validator counts rather than names them: which .fon files a distro ships
# varies by package version, and a hard-coded list fails the build for the
# wrong reason. The fixture writes enough to clear the floor.
WINE_FONT_FLOOR = 8
WINE_FONTS = tuple(f"wine{index:02d}.fon" for index in range(WINE_FONT_FLOOR + 2))


def elf(machine: int = 62, elf_class: int = 2, body: bytes = b"") -> bytes:
    """A minimal ELF header, enough for a header-only class/machine check."""
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = elf_class
    header[5] = 1  # little endian
    header[6] = 1  # EI_VERSION
    struct.pack_into("<H", header, 16, 2)  # e_type = ET_EXEC
    struct.pack_into("<H", header, 18, machine)
    return bytes(header) + body


ROOT = "usr/lib/x86_64-linux-gnu/wine"
PE_DIR = ROOT + "/x86_64-windows"
# New WoW64 keeps the 32-bit PE builtins in a third architecture directory
# under the same module root. There is deliberately no i386-unix tree: the
# Unix side stays 64-bit, which is what removes the need for 32-bit Linux
# libraries and is the difference from the distro's old WoW64.
PE32_DIR = ROOT + "/i386-windows"
# COFF machine values. The 32-bit builtins are PE32 images run in the CPU's
# compatibility mode; a 64-bit image staged there passes every path check and
# cannot be mapped by the guest at all.
PE32_MACHINE = 0x014C
PE32PLUS_MACHINE = 0x8664
# The 32-bit builtins a Windows program's import chain reaches before its own
# entry point runs, taken from a device run of a 32-bit Direct3D 9 probe. That
# run resolved every one of these out of the projected i386-windows tree
# except zlib1.dll, which 32-bit wined3d imports: the 64-bit tree carried it,
# the staged 32-bit tree did not, and the process ended STATUS_DLL_NOT_FOUND
# (0xc0000135) with no message and no window. The archive is checked against
# this list precisely because the failure it prevents is silent.
PE32_MODULES = ("ntdll.dll", "kernel32.dll", "kernelbase.dll", "advapi32.dll",
                "sechost.dll", "msvcrt.dll", "ucrtbase.dll", "gdi32.dll",
                "user32.dll", "win32u.dll", "opengl32.dll", "wined3d.dll",
                "d3d9.dll", "zlib1.dll")
# Wine's own WoW64 thunking layer, which lives in the 64-bit tree even though
# it exists to serve 32-bit code: ntdll loads wow64.dll to build the 32-bit
# process, wow64win.dll thunks user/GDI syscalls into the 64-bit win32u, and
# wow64cpu.dll performs the mode transfer.
WOW64_MODULES = ("wow64.dll", "wow64win.dll", "wow64cpu.dll")
DATA_ROOT = "usr/share/wine"
DERIVED_DATA_ROOT = "usr/lib/share/wine"


def pe(machine: int = 0x8664, body: bytes = b"") -> bytes:
    """A minimal PE32+ AMD64 image: DOS header, e_lfanew, PE signature, COFF.

    The validator has to distinguish a real builtin from a truncated entry and
    from the text of a link target stored as a plain file, so a two-byte "MZ"
    is deliberately not enough to pass.
    """
    lfanew = 0x40
    dos = bytearray(b"MZ" + b"\0" * (lfanew - 2))
    dos[0x3c:0x40] = lfanew.to_bytes(4, "little")
    coff = b"PE\0\0" + machine.to_bytes(2, "little") + b"\0" * 18
    return bytes(dos) + coff + body


def guest_link(target: str) -> bytes:
    """BoxedWine reads a guest symlink from a `.link` entry's contents."""
    return target.encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class WineserverPackagingContract(unittest.TestCase):
    """The builder must never package the shell wrapper as a guest binary."""

    def setUp(self) -> None:
        self.builder = BUILDER.read_text(encoding="utf-8")

    def test_builder_packages_the_real_executable_at_both_paths(self) -> None:
        self.assertIn(
            'copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver64"',
            self.builder)
        self.assertIn(
            'copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver"',
            self.builder)

    def test_builder_puts_the_loader_in_the_module_tree(self) -> None:
        # Wine reads /proc/self/exe to decide where wineserver is, and takes
        # its module root from ntdll.so's parent. A loader packaged outside the
        # module tree makes those two derivations name different places, which
        # is how a run reached Windows code and then failed to load kernel32.
        self.assertIn('WINE_MODULE_ROOT="/usr/lib/x86_64-linux-gnu/wine"',
                      self.builder)
        self.assertIn('copy_as "${WINE64}" "${WINE_MODULE_ROOT}/wine64"',
                      self.builder)
        self.assertNotIn('copy_as "${WINE64}" /usr/lib/wine/wine64',
                         self.builder)

    def test_builder_writes_compatibility_paths_as_guest_links(self) -> None:
        # BoxedWine's ZIP filesystem does not read POSIX symlinks out of an
        # archive: it recognises a link by a `.link` suffix whose contents are
        # the target. `ln -s` here produced a small regular file holding the
        # target text, which is a silent failure.
        self.assertIn('> "${STAGE}${link}.link"', self.builder)
        self.assertNotIn("ln -s ", self.builder)
        for link in ("/usr/lib/wine/wine64", "/usr/bin/wine64",
                     "/usr/lib/wine/x86_64-windows"):
            self.assertIn("guest_link \"${WINE_MODULE_ROOT}/", self.builder)
            self.assertIn(link, self.builder)

    def test_builder_links_wines_derived_data_root_to_packaged_nls(self) -> None:
        self.assertIn("cp -aL /usr/share/wine", self.builder)
        self.assertIn(
            "guest_link /usr/share/wine /usr/lib/share/wine",
            self.builder,
        )

    def test_derived_data_root_matches_wines_real_request(self) -> None:
        requested = posixpath.normpath("/" + ROOT + "/../../share/wine")
        self.assertEqual(requested, "/" + DERIVED_DATA_ROOT)

    def test_builder_refuses_an_archive_without_the_builtins(self) -> None:
        # kernel32.dll present in the archive but unreachable is the failure
        # this layout exists to prevent; kernel32.dll absent would be worse.
        for builtin in ("ntdll.dll", "kernel32.dll", "kernelbase.dll"):
            self.assertIn(builtin, self.builder)
        self.assertIn("is not a PE image", self.builder)

    def test_builder_no_longer_packages_the_distro_wrapper(self) -> None:
        # The old shape copied ${WINE_ROOT}/wineserver -- the shell wrapper --
        # straight into the generic guest path.
        self.assertNotIn('copy_as "${WINE_ROOT}/wineserver"', self.builder)

    def test_builder_selects_wineserver_by_elf_header(self) -> None:
        # A generic candidate is acceptable only when its own header proves it
        # is x86-64, never because it happens to be an executable file.
        self.assertIn("is_elf64_x86_64()", self.builder)
        self.assertIn("find_first_elf64_x86_64", self.builder)
        selection = self.builder[self.builder.index("WINE_SERVER=\"$("):]
        self.assertTrue(
            selection.startswith('WINE_SERVER="$(find_first_elf64_x86_64'),
            "wineserver selection must go through the ELF-header check",
        )


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class WineserverArchiveValidation(unittest.TestCase):
    """Drive the real validator against synthetic layered archives."""

    def build_archives(self, directory: Path, wineserver_generic: bytes,
                       freetype: bytes = None,
                       glibc_freetype: bool = True,
                       wine_freetype: bool = True,
                       x11_driver: bool = True,
                       pe32_modules=PE32_MODULES,
                       pe32_machine: int = PE32_MACHINE,
                       wow64_modules=WOW64_MODULES):
        server = elf(body=b"wineserver64 payload")
        if freetype is None:
            freetype = elf(body=b"freetype payload")
        self._freetype = freetype
        self._wine_freetype = wine_freetype
        self._x11_driver = x11_driver
        self._pe32_modules = pe32_modules
        self._pe32_machine = pe32_machine
        self._wow64_modules = wow64_modules
        glibc = directory / "glibc-rootfs64.zip"
        wine = directory / "wine64.zip"
        # The 32-bit PE tree ships in its own archive: inside wine64.zip it
        # is what the app mounts, and mounting it broke the 64-bit lane on
        # device (the cube probe stalled in RegisterClass in four of four
        # runs) and grew the IPA to 400 MB.
        self._pe32_archive = directory / "wine64-pe32.zip"
        self.write_pe32_archive(self._pe32_archive)
        with zipfile.ZipFile(glibc, "w") as archive:
            archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
            archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
            # Wine dlopens FreeType by soname and searches both multiarch
            # paths. A BoxedWine ZIP does not interpret POSIX symlinks, so
            # both have to be real files; this is the glibc layer's copy.
            if glibc_freetype:
                archive.writestr(FREETYPE_GLIBC_PATH, freetype)
            archive.writestr(FONTCONFIG_PATH, FONTCONFIG_XML)
            for fontconfig_conf in FONTCONFIG_CONFD:
                archive.writestr(fontconfig_conf, FONTCONFIG_XML)
            for x11_lib in X11_CORE_LIBS:
                archive.writestr("lib/x86_64-linux-gnu/" + x11_lib, elf())
            # Wine's server needs the modelled user's XDG runtime directory.
            archive.writestr("run/user/1000/", b"")
        with zipfile.ZipFile(wine, "w") as archive:
            self.write_wine_layout(archive, server, wineserver_generic)
        return glibc, wine, server

    def write_wine_layout(self, archive, server: bytes,
                          wineserver_generic: bytes) -> None:
        """The canonical layout: loader and modules in one tree.

        Wine takes the directory it looks for wineserver in from
        /proc/self/exe and its module root from ntdll.so's parent, so the
        loader has to live in the same tree as the modules it loads.
        """
        # The Wine layer carries the /usr copy of the font backend, the X11
        # client libraries, the user driver and Wine's bitmap fonts.
        if getattr(self, "_wine_freetype", True):
            archive.writestr(FREETYPE_WINE_PATH,
                             getattr(self, "_freetype", elf(body=b"freetype")))
        for x11_lib in X11_CORE_LIBS:
            archive.writestr("usr/lib/x86_64-linux-gnu/" + x11_lib, elf())
        for x11_shim in X11_SHIM_LIBS:
            archive.writestr(X11_SHIM_DIR + "/" + x11_shim, elf(body=b"shim"))
        if getattr(self, "_x11_driver", True):
            archive.writestr(PE_DIR + "/winex11.drv", pe(body=b"winex11"))
            archive.writestr(ROOT + "/x86_64-unix/winex11.so", elf())
        for wine_font in WINE_FONTS:
            archive.writestr(DATA_ROOT + "/fonts/" + wine_font, b"fon:" + wine_font.encode())
        archive.writestr(ROOT + "/wine64", elf(body=b"wine64"))
        archive.writestr(ROOT + "/wineserver64", server)
        archive.writestr(ROOT + "/wineserver", wineserver_generic)
        archive.writestr(ROOT + "/x86_64-unix/winemetal.so", elf())
        for builtin in ("ntdll.dll", "kernel32.dll", "kernelbase.dll"):
            archive.writestr(PE_DIR + "/" + builtin, pe(body=builtin.encode()))
        # Wine's WoW64 thunk modules are 64-bit builtins, beside the rest.
        for wow64_module in getattr(self, "_wow64_modules", WOW64_MODULES):
            archive.writestr(PE_DIR + "/" + wow64_module,
                             pe(body=wow64_module.encode()))
        for nls in ("c_20127.nls", "locale.nls", "l_intl.nls"):
            archive.writestr(DATA_ROOT + "/nls/" + nls, b"nls:" + nls.encode())
        for link, target in self.compatibility_links():
            archive.writestr(link + ".link", guest_link(target))

    def write_pe32_archive(self, pe32: Path) -> None:
        """The 32-bit builtins, in their own tree, as PE32 images, with the
        compatibility link that names the tree; all in the third archive."""
        pe32_machine = getattr(self, "_pe32_machine", PE32_MACHINE)
        with zipfile.ZipFile(pe32, "w") as archive:
            for pe32_module in getattr(self, "_pe32_modules", PE32_MODULES):
                archive.writestr(PE32_DIR + "/" + pe32_module,
                                 pe(machine=pe32_machine,
                                    body=b"i386:" + pe32_module.encode()))
            archive.writestr("usr/lib/wine/i386-windows.link",
                             guest_link("/" + PE32_DIR))

    @staticmethod
    def compatibility_links():
        return (
            ("usr/lib/wine/wine64", "/" + ROOT + "/wine64"),
            ("usr/lib/wine/wineserver", "/" + ROOT + "/wineserver"),
            ("usr/lib/wine/wineserver64", "/" + ROOT + "/wineserver64"),
            ("usr/lib/wine/x86_64-windows", "/" + PE_DIR),
            ("usr/lib/wine/x86_64-unix", "/" + ROOT + "/x86_64-unix"),
            ("usr/bin/wine64", "/" + ROOT + "/wine64"),
            ("usr/bin/wineserver", "/" + ROOT + "/wineserver"),
            (DERIVED_DATA_ROOT, "/" + DATA_ROOT),
        )

    @staticmethod
    def wow64_manifest_pins(wine: Path, pe32: Path) -> dict:
        """The WoW64 pins the validator requires beside the layer hashes.

        A runtime that lost its 32-bit tree between build and packaging still
        satisfies every 64-bit path the checker knew about before, so the tree
        is pinned by its archive hash, by count and by the hash of the module
        a WoW64 process loads first; the thunk modules are pinned in the
        64-bit archive.
        """
        def archive_pins(path: Path, entries: dict) -> dict:
            if not path.is_file():
                # A missing archive still gets syntactically valid pins, so
                # the archive check is what reports it rather than the parser.
                return {key: "0" * 64 for key in entries}
            with zipfile.ZipFile(path) as archive:
                names = {item.filename for item in archive.infolist()}
                return {key: (sha256(archive.read(name)) if name in names
                              else "0" * 64)
                        for key, name in entries.items()}

        pins = {"pe32_archive": "wine64-pe32.zip",
                "pe32_sha256": (sha256(pe32.read_bytes()) if pe32.is_file()
                                else "0" * 64)}
        if pe32.is_file():
            with zipfile.ZipFile(pe32) as archive:
                pins["i386_windows_module_count"] = str(sum(
                    1 for item in archive.infolist()
                    if item.filename.startswith(PE32_DIR + "/")
                    and not item.filename.endswith("/")))
        else:
            pins["i386_windows_module_count"] = "0"
        pins.update(archive_pins(
            pe32, {"i386_windows_ntdll_sha256": PE32_DIR + "/ntdll.dll"}))
        pins.update(archive_pins(wine, {
            wow64_module[:-len(".dll")] + "_dll_sha256": PE_DIR + "/" + wow64_module
            for wow64_module in WOW64_MODULES}))
        return pins

    def run_validator(self, directory: Path, glibc: Path, wine: Path,
                      manifest_overrides: dict = None,
                      drop_manifest_keys=()) -> tuple:
        entries = {
            "source":
                "third_party/boxedwine64-audit/tools/rootfs64/build-wine64-zip.sh",
            "source_image": "boxedwine64/wine64-debian:bookworm",
            "glibc_sha256": sha256(glibc.read_bytes()),
            "wine_sha256": sha256(wine.read_bytes()),
        }
        pe32 = getattr(self, "_pe32_archive", None) or (directory / "wine64-pe32.zip")
        if not pe32.is_file() and not getattr(self, "_pe32_absent", False):
            # Cases that build wine64.zip by hand are about the 64-bit layer;
            # give them the third archive so the validator reaches their check.
            self.write_pe32_archive(pe32)
        entries.update(self.wow64_manifest_pins(wine, pe32))
        entries.update(manifest_overrides or {})
        for dropped in drop_manifest_keys:
            entries.pop(dropped, None)
        manifest = directory / "manifest.txt"
        manifest.write_text(
            "".join(f"{key}={value}\n" for key, value in entries.items()),
            encoding="utf-8", newline="\n",
        )
        completed = subprocess.run(
            ["bash", str(VALIDATOR), "--input", str(directory),
             "--manifest", str(manifest)],
            capture_output=True, text=True, timeout=120,
        )
        return completed.returncode, completed.stdout + completed.stderr

    def test_matching_elf64_wineservers_are_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            self.assertIn("wineserver matches wineserver64", output)

    def test_the_old_shell_wrapper_shape_is_rejected(self) -> None:
        # Exactly the artifact that shipped: a 382-byte /bin/sh wrapper at
        # usr/lib/wine/wineserver beside a valid ELF64 wineserver64.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            glibc, wine, _ = self.build_archives(directory, SHELL_WRAPPER)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0,
                                "a shell wrapper at the generic wineserver "
                                "path must fail validation")
            self.assertIn("is not an ELF file", output)

    def test_a_32_bit_wineserver_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            glibc, wine, _ = self.build_archives(
                directory, elf(machine=3, elf_class=1, body=b"i386"))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertTrue("ELFCLASS64" in output or "EM_X86_64" in output,
                            output)

    def test_a_divergent_wineserver_is_rejected(self) -> None:
        # Both are valid x86-64 ELF files, but they are not the same binary.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            glibc, wine, _ = self.build_archives(
                directory, elf(body=b"a different wineserver"))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("differ", output)

    def test_a_missing_generic_wineserver_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc = directory / "glibc-rootfs64.zip"
            wine = directory / "wine64.zip"
            with zipfile.ZipFile(glibc, "w") as archive:
                archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
                archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
                # What the validator now requires in this layer.
                archive.writestr(FREETYPE_GLIBC_PATH, elf(body=b"freetype"))
                archive.writestr(FONTCONFIG_PATH, FONTCONFIG_XML)
                for fontconfig_conf in FONTCONFIG_CONFD:
                    archive.writestr(fontconfig_conf, FONTCONFIG_XML)
                for x11_lib in X11_CORE_LIBS:
                    archive.writestr("lib/x86_64-linux-gnu/" + x11_lib, elf())
                archive.writestr("run/user/1000/", b"")
            with zipfile.ZipFile(wine, "w") as archive:
                self.write_wine_layout(archive, server, server)
            self.rewrite_wine_archive(wine, drop=ROOT + "/wineserver")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("wineserver", output)


    def test_a_missing_builtin_is_rejected(self) -> None:
        for missing in ("kernel32.dll", "kernelbase.dll", "ntdll.dll"):
            with self.subTest(missing=missing):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(directory, server)
                    self.rewrite_wine_archive(wine, drop=PE_DIR + "/" + missing)
                    code, output = self.run_validator(directory, glibc, wine)
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(missing, output)

    def test_missing_nls_data_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, drop=DATA_ROOT + "/nls/locale.nls")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("locale.nls", output)

    def test_missing_derived_data_root_link_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(wine, drop=DERIVED_DATA_ROOT + ".link")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn(DERIVED_DATA_ROOT, output)

    def test_a_builtin_that_is_not_a_pe_image_is_rejected(self) -> None:
        # Two bytes of "MZ" used to be enough. A truncated entry, or the text
        # of a link target stored as a plain file, must not pass for a module.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, replace={PE_DIR + "/kernel32.dll": b"MZ"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("kernel32.dll", output)

    def test_a_32_bit_builtin_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, replace={PE_DIR + "/kernel32.dll": pe(machine=0x014c)})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("AMD64", output)

    def test_a_posix_symlink_is_not_a_guest_link(self) -> None:
        # The shape that shipped: `ln -s` stored as a real symlink, which
        # BoxedWine reads as a regular file holding the target text.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, drop="usr/bin/wine64.link",
                add={"usr/bin/wine64": b"/" + ROOT.encode() + b"/wine64"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("usr/bin/wine64", output)

    def test_a_guest_link_to_nothing_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine,
                replace={"usr/bin/wine64.link": b"/usr/lib/wine/not-packaged"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("usr/bin/wine64", output)

    def rewrite_wine_archive(self, wine: Path, drop: str = "",
                             replace=None, add=None) -> None:
        """Rebuild the Wine archive with one entry dropped, changed or added."""
        replace = replace or {}
        add = add or {}
        with zipfile.ZipFile(wine) as archive:
            entries = [(item.filename, archive.read(item.filename))
                       for item in archive.infolist()]
        with zipfile.ZipFile(wine, "w") as archive:
            for name, data in entries:
                if name == drop:
                    continue
                archive.writestr(name, replace.get(name, data))
            for name, data in add.items():
                archive.writestr(name, data)

    def test_a_missing_guest_runtime_directory_is_rejected(self) -> None:
        # Wine chooses /run/user/1000 for its server socket directory. If the
        # rootfs does not ship it, wineserver cannot start.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc = directory / "glibc-rootfs64.zip"
            wine = directory / "wine64.zip"
            with zipfile.ZipFile(glibc, "w") as archive:
                archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
                archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
                # What the validator now requires in this layer.
                archive.writestr(FREETYPE_GLIBC_PATH, elf(body=b"freetype"))
                archive.writestr(FONTCONFIG_PATH, FONTCONFIG_XML)
                for fontconfig_conf in FONTCONFIG_CONFD:
                    archive.writestr(fontconfig_conf, FONTCONFIG_XML)
                for x11_lib in X11_CORE_LIBS:
                    archive.writestr("lib/x86_64-linux-gnu/" + x11_lib, elf())
            with zipfile.ZipFile(wine, "w") as archive:
                self.write_wine_layout(archive, server, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("run/user/1000", output)


class GuestRuntimeDirectoryStaging(unittest.TestCase):
    """The builder has to stage the runtime directory into the rootfs layer."""

    def test_builder_stages_the_guest_runtime_directory(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn('"${STAGE}/run/user/1000"', builder)
        # It has to land in the rootfs archive, not the Wine archive.
        rootfs_line = [line for line in builder.splitlines()
                       if "glibc-rootfs64.zip" in line and "zip -qry9" in line]
        self.assertTrue(rootfs_line, "rootfs archive command not found")
        index = builder.index(rootfs_line[0])
        self.assertIn(" run ", builder[index:index + 200])


if __name__ == "__main__":
    unittest.main()


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class FreeTypePackaging(unittest.TestCase):
    """Wine's font backend has to be in the archive, and has to be loadable.

    Two device runs reported "Wine cannot find the FreeType font library" after
    failing to open libfreetype.so.6 at both multiarch paths in turn. The
    archive simply did not contain it: nothing named it as a dynamically loaded
    dependency, so nothing missed it.
    """

    def test_builder_seeds_freetype_as_a_required_dynamic_library(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        # A reusable mechanism, not another bare copy: the seed feeds the ldd
        # closure so FreeType's own dependencies come from the runner.
        self.assertIn("seed_dynamic_library()", builder)
        self.assertIn("seed_dynamic_library libfreetype.so.6 required", builder)
        # Required means the build fails without it.
        self.assertIn("requirement}\" == \"required\"", builder)

    def test_builder_stages_dynamic_libraries_at_both_multiarch_paths(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn('copy_as "${found}" "/lib/x86_64-linux-gnu/${soname}"',
                      builder)
        self.assertIn('copy_as "${found}" "/usr/lib/x86_64-linux-gnu/${soname}"',
                      builder)

    def test_builder_refuses_a_non_x86_64_font_library(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        # The repository also builds a static FreeType for the iOS host.
        # Substituting it would satisfy a name check and leave Wine's Unix
        # side with nothing it can load.
        self.assertIn("is not an x86-64 ELF shared object", builder)

    def test_builder_proves_every_seeded_library_landed(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn("was not staged at", builder)


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class FreeTypeArchiveValidation(WineserverArchiveValidation):
    """Drive the real validator against archives with and without FreeType.

    Subclassed only to reuse the archive helpers. The parent's own tests are
    detached below rather than inherited: re-running a two-and-a-half-minute
    suite a second time to borrow two helper methods is not a trade worth
    making, and a duplicate result is not a second piece of evidence.
    """

    def test_a_complete_font_backend_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            for path in (FREETYPE_GLIBC_PATH, FREETYPE_WINE_PATH):
                self.assertIn(path + " is ELF64 EM_X86_64", output)

    def test_a_missing_font_library_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, glibc_freetype=False, wine_freetype=False)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("libfreetype.so.6", output)

    def test_a_font_library_in_only_one_layer_is_rejected(self) -> None:
        # The shape a single staged copy produces. The guest archive has no
        # symlink to make the other path resolve, so each layer needs its own.
        for glibc_has, wine_has, expected in (
                (True, False, FREETYPE_WINE_PATH),
                (False, True, FREETYPE_GLIBC_PATH)):
            with self.subTest(glibc=glibc_has, wine=wine_has):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(
                        directory, server,
                        glibc_freetype=glibc_has, wine_freetype=wine_has)
                    code, output = self.run_validator(directory, glibc, wine)
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(expected, output)

    def test_a_non_x86_64_font_library_is_rejected(self) -> None:
        # An ARM64 ELF passes every name check and cannot be loaded by the
        # guest at all.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, freetype=elf(machine=183))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("EM_X86_64", output)

    def test_a_static_archive_masquerading_as_the_library_is_rejected(self) -> None:
        # What copying the iOS FreeType build in would look like from here.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, freetype=b"!<arch>\n" + b"\0" * 64)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("is not an ELF file", output)


# Detach the inherited cases; see the class docstring. Assigning None leaves
# the attribute present but not callable, which is what unittest's collector
# filters on, so the helpers survive and the duplicate tests do not.
for _inherited in list(vars(WineserverArchiveValidation)):
    if _inherited.startswith("test_"):
        setattr(FreeTypeArchiveValidation, _inherited, None)


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class WindowDriverPackaging(WineserverArchiveValidation):
    """The runtime has to contain everything a window needs to exist.

    A device run reached CreateWindowExW and got ERROR_INVALID_WINDOW_HANDLE
    with no X11 activity anywhere in the log. A process whose user driver never
    loaded has no desktop window to parent to, and Wine does not report that as
    an error of its own -- so the packaging is what has to be proven.
    """

    def test_a_complete_window_runtime_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            self.assertIn("winex11.drv", output)
            self.assertIn("winex11.so", output)

    def test_a_missing_x11_user_driver_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, x11_driver=False)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("winex11", output)

    def test_a_conf_d_holding_link_text_is_rejected(self) -> None:
        # The exact failure shape a device run produced: fontconfig reported
        # "out of memory" at both <include> lines of fonts.conf and then
        # refused the file. A guest link's target text where XML belongs
        # parses as neither, and nothing before this noticed.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.replace_in_archive(glibc, FONTCONFIG_CONFD[0],
                                    b"/usr/share/fontconfig/conf.avail/10-x.conf")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("XML document", output)

    def test_an_empty_conf_d_is_rejected(self) -> None:
        # The other half of the same failure: fonts.conf includes a directory
        # that has nothing in it, which is silent from the guest side.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            for conf in FONTCONFIG_CONFD:
                self.strip_from_archive(glibc, conf)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("conf.d", output)

    def test_a_guest_link_under_the_font_config_tree_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            with zipfile.ZipFile(glibc, "a") as archive:
                archive.writestr("etc/fonts/conf.d/99-link.conf.link",
                                 b"/usr/share/fontconfig/conf.avail/99.conf")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("guest links", output)

    @staticmethod
    def replace_in_archive(path: Path, entry: str, payload: bytes) -> None:
        with zipfile.ZipFile(path) as source:
            kept = [(item, payload if item.filename == entry
                     else source.read(item.filename))
                    for item in source.infolist()]
        with zipfile.ZipFile(path, "w") as target:
            for item, data in kept:
                target.writestr(item, data)

    def test_a_missing_fontconfig_configuration_is_rejected(self) -> None:
        # Fontconfig loading with no default config is the exact shape a
        # device run produced: the library opens, and then every font lookup
        # fails.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.strip_from_archive(glibc, FONTCONFIG_PATH)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("fonts.conf", output)

    def test_missing_x11_client_libraries_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.strip_from_archive(glibc, "lib/x86_64-linux-gnu/libX11.so.6")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("libX11.so.6", output)

    def test_an_underpopulated_wine_font_directory_is_rejected(self) -> None:
        # One font is not a font set. Wine draws every window's non-client
        # area with these, and a device run failed to open all of them.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            for wine_font in WINE_FONTS[1:]:
                self.strip_from_archive(wine, DATA_ROOT + "/fonts/" + wine_font)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("bitmap fonts", output)

    def test_the_font_set_is_counted_not_named(self) -> None:
        # A distro that ships a different set of .fon files must still pass:
        # hard-coding names is a guess about someone else's packaging.
        validator = VALIDATOR.read_text(encoding="utf-8")
        # Checked against the executable lines only. The comment above the
        # check names the very font Ubuntu does not ship, which is the point
        # it is making.
        code = chr(10).join(line for line in validator.splitlines()
                            if not line.lstrip().startswith("#"))
        self.assertNotIn(".fon", code.replace("*.fon", ""))
        self.assertIn("wine_font_count", code)

    @staticmethod
    def strip_from_archive(path: Path, drop: str) -> None:
        """Rewrite an archive without one entry, keeping the manifest valid."""
        with zipfile.ZipFile(path) as source:
            kept = [(item, source.read(item.filename))
                    for item in source.infolist()
                    if item.filename != drop]
        with zipfile.ZipFile(path, "w") as target:
            for item, payload in kept:
                target.writestr(item, payload)


for _inherited in list(vars(WineserverArchiveValidation)):
    if _inherited.startswith("test_"):
        setattr(WindowDriverPackaging, _inherited, None)


class Wow64LayerBuilderContract(unittest.TestCase):
    """The builder has to package both architectures, and prove which is which.

    Ubuntu's wine64 package ships only the 64-bit trees. The 32-bit PE builtins
    come from the same version's i386 package, whose i386-unix tree is the OLD
    WoW64 and needs the 32-bit Linux libraries this lane exists to avoid -- so
    the option names a PE directory, not a package.
    """

    def setUp(self) -> None:
        self.builder = BUILDER.read_text(encoding="utf-8")

    def test_builder_takes_the_i386_pe_tree_as_an_option(self) -> None:
        self.assertIn("--i386-pe-dir", self.builder)
        self.assertIn('I386_PE_DIR="$2"', self.builder)

    def test_the_i386_pe_tree_is_required_in_ci(self) -> None:
        # A runtime without it has no WoW64 lane at all, and nothing
        # downstream reports that: a 32-bit image simply fails to start.
        self.assertIn("--i386-pe-dir is required in CI", self.builder)
        self.assertIn('"${CI:-}" != "true"', self.builder)

    def test_builder_stages_the_tree_beside_the_two_64_bit_trees(self) -> None:
        # Wine appends the architecture directory to each module root it
        # searches, so a tree anywhere else is invisible to the derivation
        # that already had to be fixed for x86_64-windows.
        self.assertIn(
            'I386_PE_GUEST_DIR="${WINE_MODULE_ROOT}/i386-windows"',
            self.builder)
        self.assertIn('mkdir -p "${PE32_STAGE}${I386_PE_GUEST_DIR}"', self.builder)
        self.assertIn(
            'pe32_guest_link "${I386_PE_GUEST_DIR}" /usr/lib/wine/i386-windows',
            self.builder)

    def test_builder_archives_the_32_bit_tree_apart(self) -> None:
        # Inside wine64.zip the tree is what the app mounts; mounting it broke
        # the 64-bit lane on device and grew the IPA to 400 MB, so it ships in
        # wine64-pe32.zip, pinned in the manifest, and the app leaves it out.
        self.assertIn('zip -qry9 "${OUTPUT_DIR}/wine64-pe32.zip" usr', self.builder)
        self.assertIn("pe32_archive=wine64-pe32.zip", self.builder)
        self.assertIn("pe32_sha256=%s", self.builder)
        self.assertIn('"${PE32_STAGE}${I386_PE_GUEST_DIR}"', self.builder)
        build_ios = (REPO / "scripts" / "build-ios.sh").read_text(encoding="utf-8")
        self.assertNotIn("wine64-pe32.zip", build_ios)
        validator = VALIDATOR.read_text(encoding="utf-8")
        self.assertIn("carries an i386-windows tree", validator)

    def test_builder_never_packages_the_old_wow64_unix_tree(self) -> None:
        # i386-unix is old WoW64: ELF32 objects needing a 32-bit Linux loader
        # and the libraries this lane exists to avoid. Checked against the
        # executable lines only -- the comments name it to say why it is
        # excluded, which is the point they are making.
        code = chr(10).join(line for line in self.builder.splitlines()
                            if not line.lstrip().startswith("#"))
        self.assertNotIn("i386-unix", code)

    def test_builder_requires_wines_own_wow64_modules(self) -> None:
        for wow64_module in WOW64_MODULES:
            self.assertIn(wow64_module, self.builder)
        self.assertIn("for wow64_module in wow64.dll wow64win.dll wow64cpu.dll",
                      self.builder)

    def test_builder_checks_the_pe_class_of_each_tree(self) -> None:
        # A name is not evidence of an architecture: a mis-pointed
        # --i386-pe-dir stages 64-bit images under i386-windows, which passes
        # every path check and cannot be mapped by the guest.
        self.assertIn("pe_coff_machine()", self.builder)
        self.assertIn("require_pe_machine", self.builder)
        # 0x014c for the 32-bit tree, 0x8664 for the 64-bit one.
        self.assertIn(
            'require_pe_machine "${PE32_STAGE}${I386_PE_GUEST_DIR}/ntdll.dll" 332',
            self.builder)
        self.assertIn(
            'require_pe_machine "${STAGE}${WINE_MODULE_ROOT}/x86_64-windows/ntdll.dll" 34404',
            self.builder)
        self.assertEqual(332, PE32_MACHINE)
        self.assertEqual(34404, PE32PLUS_MACHINE)

    def test_builder_records_the_wow64_half_in_the_manifest(self) -> None:
        for key in ("i386_windows_module_count", "i386_windows_ntdll_sha256"):
            self.assertIn(key, self.builder)
        # The three wow64 hashes are emitted from one loop over the names.
        self.assertIn("for wow64_module in wow64 wow64win wow64cpu",
                      self.builder)
        self.assertIn("_dll_sha256=%s", self.builder)


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class Wow64ArchiveValidation(WineserverArchiveValidation):
    """Drive the real validator against archives with and without WoW64.

    Subclassed for the archive helpers only; the parent's own cases are
    detached below, as they are for the other derived suites.
    """

    def test_a_complete_wow64_layer_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            self.assertIn(PE32_DIR + "/ntdll.dll is a PE32 i386 image", output)
            for wow64_module in WOW64_MODULES:
                self.assertIn(wow64_module, output)
            self.assertIn("i386-windows -> /" + PE32_DIR, output)

    def test_a_32_bit_tree_missing_one_import_chain_module_is_rejected(self) -> None:
        # The failure this reproduces: a tree carrying ntdll, kernel32 and
        # every other builtin the loader binds to, and not zlib1.dll, which
        # 32-bit wined3d imports. The old check passed such a tree, and the
        # 32-bit process then exited STATUS_DLL_NOT_FOUND before its entry
        # point with nothing on screen to say why.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server,
                pe32_modules=tuple(name for name in PE32_MODULES
                                   if name != "zlib1.dll"))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("zlib1.dll", output)

    def test_a_runtime_without_the_32_bit_tree_is_rejected(self) -> None:
        # A 64-bit-only runtime satisfies every path the checker required
        # before this lane existed, which is exactly why it is checked.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, pe32_modules=())
            self.rewrite_wine_archive(self._pe32_archive,
                                      drop="usr/lib/wine/i386-windows.link")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("i386-windows", output)

    def test_a_32_bit_tree_inside_the_64_bit_archive_is_rejected(self) -> None:
        # The archive the app mounts must stay 64-bit only: shipping the tree
        # inside it is what regressed the lane on device.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, add={PE32_DIR + "/ntdll.dll": pe(machine=PE32_MACHINE)})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("wine64-pe32.zip", output)

    def test_a_missing_32_bit_archive_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self._pe32_archive.unlink()
            self._pe32_absent = True
            try:
                code, output = self.run_validator(directory, glibc, wine)
            finally:
                self._pe32_absent = False
            self.assertNotEqual(code, 0, output)
            self.assertIn("wine64-pe32.zip", output)

    def test_a_manifest_pinning_a_different_32_bit_archive_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(
                directory, glibc, wine,
                manifest_overrides={"pe32_sha256": sha256(b"other")})
            self.assertNotEqual(code, 0, output)
            self.assertIn("pe32_sha256", output)

    def test_a_missing_32_bit_builtin_is_rejected(self) -> None:
        for missing in ("ntdll.dll", "kernel32.dll"):
            with self.subTest(missing=missing):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(directory, server)
                    self.rewrite_wine_archive(
                        self._pe32_archive, drop=PE32_DIR + "/" + missing)
                    code, output = self.run_validator(directory, glibc, wine)
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(missing, output)

    def test_a_64_bit_image_in_the_32_bit_tree_is_rejected(self) -> None:
        # What a mis-pointed --i386-pe-dir produces: every name is right and
        # the guest cannot map a single one of the images.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, pe32_machine=PE32PLUS_MACHINE)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("not PE32 i386", output)

    def test_a_32_bit_image_in_the_64_bit_tree_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine,
                replace={PE_DIR + "/ntdll.dll": pe(machine=PE32_MACHINE)})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("AMD64", output)

    def test_a_truncated_32_bit_builtin_is_rejected(self) -> None:
        # Two bytes of "MZ" is not a module here either.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                self._pe32_archive, replace={PE32_DIR + "/kernel32.dll": b"MZ"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("kernel32.dll", output)

    def test_a_missing_wow64_thunk_module_is_rejected(self) -> None:
        # Without Wine's own WoW64 layer the 64-bit tree is complete and the
        # runtime still cannot start a 32-bit process.
        for missing in WOW64_MODULES:
            with self.subTest(missing=missing):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(
                        directory, server,
                        wow64_modules=tuple(name for name in WOW64_MODULES
                                            if name != missing))
                    code, output = self.run_validator(directory, glibc, wine)
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(missing, output)

    def test_a_wow64_module_that_is_not_a_pe_image_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, replace={PE_DIR + "/wow64cpu.dll": b"MZ not a module"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("wow64cpu.dll", output)

    def test_a_manifest_without_the_wow64_pins_is_rejected(self) -> None:
        for missing in ("i386_windows_module_count",
                        "i386_windows_ntdll_sha256", "pe32_sha256",
                        "wow64_dll_sha256", "wow64win_dll_sha256",
                        "wow64cpu_dll_sha256"):
            with self.subTest(missing=missing):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(directory, server)
                    code, output = self.run_validator(
                        directory, glibc, wine, drop_manifest_keys=(missing,))
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(missing, output)

    def test_a_manifest_pinning_a_different_wow64_module_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(
                directory, glibc, wine,
                manifest_overrides={"wow64win_dll_sha256": sha256(b"other")})
            self.assertNotEqual(code, 0, output)
            self.assertIn("wow64win.dll", output)

    def test_a_manifest_counting_the_wrong_number_of_modules_is_rejected(self) -> None:
        # The shape a partially staged tree produces: the two named builtins
        # are present and the rest of the tree is not.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(
                directory, glibc, wine,
                manifest_overrides={
                    "i386_windows_module_count": str(len(PE32_MODULES) + 7)})
            self.assertNotEqual(code, 0, output)
            self.assertIn("32-bit PE modules", output)

    def test_the_module_count_is_the_one_the_archive_carries(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            self.assertIn(f"{len(PE32_MODULES)} 32-bit PE modules", output)


for _inherited in list(vars(WineserverArchiveValidation)):
    if _inherited.startswith("test_"):
        setattr(Wow64ArchiveValidation, _inherited, None)


class Wow64LayoutHeaderContract(unittest.TestCase):
    """The C header the launch and the device preflight share with the scripts.

    The packaging scripts hard-code these guest paths; if the two ever drift,
    the archive is built somewhere the launch does not look.
    """

    def test_header_names_the_32_bit_tree_under_the_module_root(self) -> None:
        header = (REPO / "include" / "guest_wine64_layout.h").read_text(
            encoding="utf-8")
        self.assertIn(
            '#define K_X64_WINE_PE32_DIR K_X64_WINE_MODULE_ROOT "/i386-windows"',
            header)
        self.assertIn("#define K_X64_WOW64_MODULE_COUNT 3", header)
        for wow64_module in WOW64_MODULES:
            self.assertIn('"' + wow64_module + '"', header)

    def test_the_header_and_the_scripts_agree_on_the_guest_path(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        validator = VALIDATOR.read_text(encoding="utf-8")
        self.assertIn('"${WINE_MODULE_ROOT}/i386-windows"', builder)
        self.assertIn('PE32_DIR="${WINE_MODULE_ROOT}/i386-windows"', validator)
        self.assertEqual(PE32_DIR, "usr/lib/x86_64-linux-gnu/wine/i386-windows")

    def test_every_layer_names_the_same_32_bit_import_chain(self) -> None:
        # Three places decide whether a 32-bit program can start: the builder
        # stages the tree, the validator gates the archive, and the launch
        # reports what the projection actually produced. A name present in one
        # and missing from another is how zlib1.dll shipped absent in the
        # first place.
        header = (REPO / "include" / "guest_wine64_layout.h").read_text(
            encoding="utf-8")
        builder = BUILDER.read_text(encoding="utf-8")
        validator = VALIDATOR.read_text(encoding="utf-8")
        self.assertIn(
            "#define K_X64_WOW64_LANE_PE32_MODULE_COUNT "
            + str(len(PE32_MODULES)),
            header)
        self.assertIn("WOW64_LANE_PE32_MODULES=(", builder)
        for lane_module in PE32_MODULES:
            self.assertIn('"' + lane_module + '"', header, lane_module)
            self.assertIn(lane_module, builder, lane_module)
            self.assertIn(lane_module, validator, lane_module)

    def test_the_launch_reports_the_32_bit_modules_the_projection_lacks(self) -> None:
        # A device run that ends before the program's entry point leaves only
        # the search trace to read, and that trace is hundreds of stat lines
        # that never name what was not found. The projection reports the gap
        # itself, bounded by the size of the list.
        startup = (REPO / "source" / "sdl" / "startupArgs.cpp").read_text(
            encoding="utf-8")
        self.assertIn("x64Wow64LanePe32ModuleNames", startup)
        self.assertIn("BOXEDWINE_X64_PE32_GAP", startup)
